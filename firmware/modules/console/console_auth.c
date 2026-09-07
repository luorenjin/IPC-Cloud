/**
 * @file console_auth.c
 * @brief console 鉴权子模块：SCRAM 式挑战-响应、会话、暴力破解防护与 /api/v1/auth/* 端点
 *
 * 威胁模型：本地控制台跑在**明文 HTTP** 之上（局域网直连，无证书体系）。
 * 因此口令绝不过线：浏览器用 Web Crypto 由口令派生 client_key，再对服务端下发的
 * 一次性 nonce 做 HMAC 得到 proof；设备端只存 stored_key（= PBKDF2(口令, salt)），
 * 用同一算式重算并**恒定时间**比对。
 *   salt        = 16 字节随机
 *   stored_key  = PBKDF2-HMAC-SHA256(口令, salt, iter=4096, 32 字节)
 *   proof       = HMAC-SHA256(stored_key, nonce) 的十六进制串
 *
 * 凭据落在 `hal_crypto` 安全存储（键 HAL_SEC_KEY_LOCAL_USER，HAL 为"本地账号哈希"
 * 预留）；平台无 crypto 模块时退化为文件软存储。**绝不写入 core/config**：
 * `cfg_dump_json` 会整棵导出，而 Task 7 的 `GET /api/v1/config` 正建在其之上——
 * 由于 proof = HMAC(stored_key, nonce)，能读到 stored_key 即等价于拿到口令。
 *
 * 线程模型：所有鉴权状态（凭据缓存、nonce/会话/锁定表）只在 http_server 的事件
 * 循环线程内访问，外加 console_init 期间的一次装载（发生在 http_server_start 之前），
 * 因此不加锁——与 http_server 自身的路由表一致。
 *
 * 非阻塞纪律：`console_auth_check` 每个请求都会调，必须廉价（Cookie 解析 + 4 项
 * 表扫描，不做 PBKDF2、不碰存储）。凭据**读**只发生在 `console_auth_init`
 * （http_server 启动之前）。两处例外与计划一致地被接受：
 *   - 登录要跑一次 HMAC（廉价），客户端侧的 PBKDF2 在浏览器里跑；
 *   - `POST /api/v1/auth/password` 会做一次 PBKDF2(4096) 并写一次存储，
 *     期间阻塞 epoll 线程数十毫秒。改密是极低频操作，与 Task 7 的
 *     `PUT /api/v1/config` 同样落盘属同一类取舍，不为此另起线程。
 *
 * 日志脱敏（规则 R8）：口令、出厂验证码、salt、stored_key、nonce、proof、
 * session token 不出现在任何日志分支里。
 */
#ifdef _WIN32
#define _CRT_RAND_S     /* 必须在 <stdlib.h> 之前定义，才能拿到 rand_s */
#endif

#include "console_internal.h"
#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "core/os.h"
#include "hal/hal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>   /* chmod：兜底软存储要收紧到 0600 */
#endif

#define MOD "console"

#define CONSOLE_DEFAULT_USER   "admin"
#define CONSOLE_IP_UNKNOWN     "unknown"   /**< 拿不到对端地址时的锁定桶 */
/** 来源 IP 字符串缓冲宽度。**必须 >= http_server 的 CONN_PEER_IP_MAX（46）**——
 *  地址正是从 `http_conn_peer_ip()` 取来的。两者分居不同头文件（`http_server_internal.h`
 *  不对外可见），无法用 _Static_assert 绑定，改宽任一处时请同步检查另一处。 */
#define CONSOLE_IP_MAX         46

#define CONSOLE_NONCE_LEN      16          /**< nonce 随机字节数（十六进制 32 字符） */
#define CONSOLE_TOKEN_LEN      32          /**< 会话 token 随机字节数 */
#define CONSOLE_HEX_CAP(n)     ((n) * 2 + 1)

#define CONSOLE_NONCE_TTL_US   (60ull * 1000000ull)          /**< 挑战 60s 过期 */
#define CONSOLE_SESSION_IDLE_US (30ull * 60ull * 1000000ull) /**< 会话空闲 30 分钟过期 */
#define CONSOLE_LOCK_BASE_MS   60000u                        /**< 首次锁定 60s */
#define CONSOLE_LOCK_MAX_MS    (15u * 60u * 1000u)           /**< 退避上限 15 分钟 */

#define CONSOLE_BODY_MAX       1024        /**< 鉴权请求体上限，超出直接拒 */

/** 清零敏感缓冲；用 volatile 指针写，避免被优化掉 */
static void secure_wipe(void *p, size_t n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;
    while (n--) *q++ = 0;
}

/**
 * 带截断检测的格式化：截断即返回 HAL_ENOMEM 并把 out 置空串。
 * 响应体与 Set-Cookie 都经此拼装——半截 JSON / 半截 Cookie 比报错更难排查。
 */
static hal_err_t fmt_safe(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!out || cap == 0) return HAL_EINVAL;
    va_start(ap, fmt);
    n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return HAL_ENOMEM; }
    return HAL_OK;
}

/* ==========================================================================
 * 一、SHA-256（FIPS 180-4）
 * hal_crypto 只提供安全存储 / 设备证书 / sign（对已算好的摘要签名）/ random，
 * **没有任何通用哈希原语**，故此处内置一份；与 http_ws.c 的 SHA-1 同理。
 * ========================================================================== */

typedef struct {
    uint32_t h[8];
    uint64_t total_len;
    uint8_t  buf[64];
    size_t   buf_len;
} sha256_ctx_t;

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t ror32(uint32_t v, int n)
{
    return (v >> n) | (v << (32 - n));
}

static void sha256_block(sha256_ctx_t *ctx, const uint8_t blk[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16) |
               ((uint32_t)blk[i * 4 + 2] << 8) | (uint32_t)blk[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];

    for (i = 0; i < 64; i++) {
        uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + SHA256_K[i] + w[i];
        uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->h[0] = 0x6a09e667u; ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u; ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu; ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu; ctx->h[7] = 0x5be0cd19u;
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    ctx->total_len += len;
    while (len > 0) {
        size_t take = 64 - ctx->buf_len;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += take;
        data += take;
        len -= take;
        if (ctx->buf_len == 64) {
            sha256_block(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    uint64_t bit_len = ctx->total_len * 8;
    int i;

    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 56) {
        while (ctx->buf_len < 64) ctx->buf[ctx->buf_len++] = 0;
        sha256_block(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56) ctx->buf[ctx->buf_len++] = 0;
    for (i = 0; i < 8; i++) ctx->buf[56 + i] = (uint8_t)((bit_len >> ((7 - i) * 8)) & 0xFF);
    sha256_block(ctx, ctx->buf);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)((ctx->h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = (uint8_t)((ctx->h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = (uint8_t)((ctx->h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i] & 0xFF);
    }
    secure_wipe(ctx, sizeof(*ctx));
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

/* ==========================================================================
 * 二、HMAC-SHA256 与 PBKDF2（RFC 2104 / RFC 2898）
 * ========================================================================== */

hal_err_t console_hmac_sha256(const uint8_t *key, size_t key_len,
                              const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
    uint8_t k[64], pad[64], inner[32];
    sha256_ctx_t ctx;
    size_t i;

    if ((!key && key_len) || (!msg && msg_len) || !out) return HAL_EINVAL;

    memset(k, 0, sizeof(k));
    if (key_len > sizeof(k)) sha256(key, key_len, k);
    else if (key_len) memcpy(k, key, key_len);

    for (i = 0; i < sizeof(pad); i++) pad[i] = (uint8_t)(k[i] ^ 0x36);
    sha256_init(&ctx);
    sha256_update(&ctx, pad, sizeof(pad));
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);

    for (i = 0; i < sizeof(pad); i++) pad[i] = (uint8_t)(k[i] ^ 0x5c);
    sha256_init(&ctx);
    sha256_update(&ctx, pad, sizeof(pad));
    sha256_update(&ctx, inner, sizeof(inner));
    sha256_final(&ctx, out);

    secure_wipe(k, sizeof(k));
    secure_wipe(pad, sizeof(pad));
    secure_wipe(inner, sizeof(inner));
    return HAL_OK;
}

/** PBKDF2 内部拼接 salt||INT32BE(i) 用的缓冲上限；本模块 salt 恒为 16 字节 */
#define PBKDF2_SALT_MAX 64

hal_err_t console_pbkdf2_sha256(const char *pwd, size_t pwd_len,
                                const uint8_t *salt, size_t salt_len,
                                uint32_t iter, uint8_t *out, size_t out_len)
{
    uint8_t seed[PBKDF2_SALT_MAX + 4];
    uint8_t u[32], t[32];
    size_t done = 0;
    uint32_t blk = 1;

    if (!pwd || !out || out_len == 0 || iter == 0) return HAL_EINVAL;
    if (!salt && salt_len) return HAL_EINVAL;
    if (salt_len > PBKDF2_SALT_MAX) return HAL_EINVAL;

    while (done < out_len) {
        size_t take;
        uint32_t i;

        if (salt_len) memcpy(seed, salt, salt_len);
        seed[salt_len]     = (uint8_t)((blk >> 24) & 0xFF);
        seed[salt_len + 1] = (uint8_t)((blk >> 16) & 0xFF);
        seed[salt_len + 2] = (uint8_t)((blk >> 8) & 0xFF);
        seed[salt_len + 3] = (uint8_t)(blk & 0xFF);

        /* U1 = HMAC(pwd, salt || INT(blk))，其后 Ui = HMAC(pwd, U(i-1))，逐轮异或 */
        if (console_hmac_sha256((const uint8_t *)pwd, pwd_len, seed, salt_len + 4, u) != HAL_OK)
            return HAL_EIO;
        memcpy(t, u, sizeof(t));
        for (i = 1; i < iter; i++) {
            uint8_t next[32];
            size_t j;
            if (console_hmac_sha256((const uint8_t *)pwd, pwd_len, u, sizeof(u), next) != HAL_OK)
                return HAL_EIO;
            memcpy(u, next, sizeof(u));
            for (j = 0; j < sizeof(t); j++) t[j] ^= u[j];
            secure_wipe(next, sizeof(next));
        }

        take = out_len - done < sizeof(t) ? out_len - done : sizeof(t);
        memcpy(out + done, t, take);
        done += take;
        blk++;
    }

    secure_wipe(seed, sizeof(seed));
    secure_wipe(u, sizeof(u));
    secure_wipe(t, sizeof(t));
    return HAL_OK;
}

/* ==========================================================================
 * 三、十六进制编解码与恒定时间比对
 * ========================================================================== */

static hal_err_t hex_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char H[] = "0123456789abcdef";
    size_t i;
    if (!in || !out || cap < n * 2 + 1) return HAL_ENOMEM;
    for (i = 0; i < n; i++) {
        out[i * 2]     = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
    return HAL_OK;
}

static int hex_val(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static hal_err_t hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t n, i;
    if (!hex || !out || !out_len) return HAL_EINVAL;
    n = strlen(hex);
    if (n == 0 || (n & 1) || n / 2 > cap) return HAL_EINVAL;
    for (i = 0; i < n; i += 2) {
        int hi = hex_val(hex[i]), lo = hex_val(hex[i + 1]);
        if (hi < 0 || lo < 0) return HAL_EINVAL;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n / 2;
    return HAL_OK;
}

/** 恒定时间字节比对：累加异或、不早退，避免以耗时泄漏首个差异位置 */
static bool ct_equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    uint8_t diff = 0;
    size_t i;
    for (i = 0; i < n; i++) diff |= (uint8_t)(x[i] ^ y[i]);
    return diff == 0;
}

/** 恒定时间字符串比对。长度本身不是机密（proof/token 长度固定且公开） */
static bool ct_str_equal(const char *a, const char *b)
{
    size_t la, lb;
    if (!a || !b) return false;
    la = strlen(a); lb = strlen(b);
    if (la != lb) return false;
    return ct_equal(a, b, la);
}

/* ==========================================================================
 * 四、随机数：优先 hal_crypto，缺失时退化为时钟/地址混合
 * ========================================================================== */

/**
 * 平台 CSPRNG：POSIX 读 `/dev/urandom`，Windows 用 `rand_s`（底层 RtlGenRandom）。
 * 取不到返回 false，由调用方退到强度不足的时钟混合并告警。
 * `/dev/urandom` 在启动后不阻塞，读几十字节的代价可忽略；且该路径只在平台没有
 * hal_crypto 时才会走到。
 */
static bool platform_csprng(uint8_t *buf, size_t len)
{
#ifdef _WIN32
    size_t i = 0;
    while (i < len) {
        unsigned int v;
        size_t take;
        if (rand_s(&v) != 0) return false;
        take = len - i < sizeof(v) ? len - i : sizeof(v);
        memcpy(buf + i, &v, take);
        i += take;
    }
    return true;
#else
    FILE *fp = fopen("/dev/urandom", "rb");
    size_t n;
    if (!fp) return false;
    n = fread(buf, 1, len, fp);
    fclose(fp);
    return n == len;
#endif
}

static hal_err_t rand_bytes(uint8_t *buf, size_t len)
{
    static uint64_t counter;
    size_t done = 0;

    if (!buf || len == 0) return HAL_EINVAL;

    if (hal_has(HAL_MOD_CRYPTO) && hal()->crypto->random) {
        if (hal()->crypto->random(buf, len) == HAL_OK) return HAL_OK;
        LOGW(MOD, "hal_crypto 随机数失败，改用平台 CSPRNG");
    }
    if (platform_csprng(buf, len)) return HAL_OK;

    /* 最后的兜底：单调时钟 + 墙钟 + 栈地址 + 递增计数过 SHA-256。
       **不具密码学强度**——无 ASLR 的设备上栈地址恒定、计数器可由请求数推断，
       真实熵只剩微秒抖动。而本函数同时供 nonce 与 32 字节 session token 使用，
       走到这里意味着会话可被预测。故每次都按 ERROR 级别告警，不做降噪。 */
    LOGE(MOD, "无 hal_crypto 且平台 CSPRNG 不可用，随机源强度不达标："
              "会话 token 与 nonce 可能可预测，请为该平台提供 crypto 模块或 /dev/urandom");
    while (done < len) {
        struct {
            uint64_t mono, wall, counter;
            uintptr_t addr1, addr2;
        } mix;
        uint8_t digest[32];
        size_t take;

        memset(&mix, 0, sizeof(mix));
        mix.mono = os_monotonic_us();
        mix.wall = (uint64_t)os_wallclock_ms();
        mix.counter = ++counter;
        mix.addr1 = (uintptr_t)(const void *)buf;
        mix.addr2 = (uintptr_t)(const void *)&mix;
        sha256((const uint8_t *)&mix, sizeof(mix), digest);

        take = len - done < sizeof(digest) ? len - done : sizeof(digest);
        memcpy(buf + done, digest, take);
        done += take;
        secure_wipe(digest, sizeof(digest));
        secure_wipe(&mix, sizeof(mix));
    }
    return HAL_OK;
}

/* ==========================================================================
 * 五、凭据存储：hal_crypto 安全存储 / 文件软存储，永不进 core/config
 * ========================================================================== */

typedef struct {
    char     user[CONSOLE_USER_MAX];
    uint8_t  salt[CONSOLE_SALT_LEN];
    uint8_t  key[CONSOLE_KEY_LEN];
    uint32_t iter;
    bool     must_change;
    bool     valid;
} cred_t;

/* 存储镜像布局（固定 96 字节，显式大端序列化，不依赖结构体内存布局）：
     0..3   魔数 'I','C','L','U'
     4..7   记录版本
     8..11  PBKDF2 迭代次数
    12..15  强制改密标志
    16..31  salt
    32..63  stored_key
    64..95  用户名（NUL 补齐） */
#define CRED_BLOB_LEN  96u
#define CRED_VERSION   1u
#define CRED_OFF_VER   4u
#define CRED_OFF_ITER  8u
#define CRED_OFF_FLAG  12u
#define CRED_OFF_SALT  16u
#define CRED_OFF_KEY   32u
#define CRED_OFF_USER  64u
#define CRED_USER_CAP  32u   /**< 用户名在存储镜像里的固定宽度，不随内存常量变动 */
#define CRED_ITER_MAX  100000u /**< 迭代次数上限：拦住损坏记录把 epoll 线程拖死 */

_Static_assert(CONSOLE_USER_MAX >= CRED_USER_CAP,
               "用户名内存缓冲不得小于存储镜像里的固定宽度");
_Static_assert(CRED_ITER_MAX >= CONSOLE_ITER, "迭代上限必须容纳本模块自身的取值");

static cred_t s_cred;
static bool   s_cred_loaded;
/** 未播种时伪 salt 的派生密钥（仅本次运行有效，见 decoy_salt 注释） */
static uint8_t s_decoy_key[CONSOLE_KEY_LEN];
static bool    s_decoy_ready;

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static bool crypto_store_available(void)
{
    return hal_has(HAL_MOD_CRYPTO) && hal()->crypto->secure_read && hal()->crypto->secure_write;
}

/* 私有数据目录的缺省值（R3：优先取配置键 CONSOLE_DATA_DIR_KEY，此处只是兜底）。
   POSIX 上必须是绝对路径——凭据里含 stored_key，按本方案读到它即可算出 proof
   完成鉴权，落在进程工作目录下等于把它交给任何能改变 CWD 的启动方式。
   Windows 分支仅供 x86 开发/测试环境。 */
#ifdef _WIN32
#define CONSOLE_DATA_DIR_DEFAULT "ipc_state"
#else
#define CONSOLE_DATA_DIR_DEFAULT "/var/lib/ipc-console"
#endif

const char *console_auth_cred_path(void)
{
    static char path[HAL_PATH_MAX];
    char dir[HAL_PATH_MAX - 32];

    dir[0] = '\0';
    if (cfg_get_str(CONSOLE_DATA_DIR_KEY, dir, sizeof(dir)) != HAL_OK || dir[0] == '\0')
        snprintf(dir, sizeof(dir), "%s", CONSOLE_DATA_DIR_DEFAULT);
    snprintf(path, sizeof(path), "%s/%s", dir, CONSOLE_CRED_FILE);
    return path;
}

/** 建好私有目录并收紧权限（POSIX 0700）；失败不致命，写文件时会再报错 */
static void cred_dir_prepare(void)
{
    char dir[HAL_PATH_MAX];
    char *slash;

    snprintf(dir, sizeof(dir), "%s", console_auth_cred_path());
    slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    if (dir[0] == '\0') return;
    os_mkdir_p(dir);
#ifndef _WIN32
    chmod(dir, S_IRWXU);   /* 0700 */
#endif
}

static hal_err_t cred_persist(const cred_t *c)
{
    uint8_t blob[CRED_BLOB_LEN];
    hal_err_t rc;

    memset(blob, 0, sizeof(blob));
    blob[0] = 'I'; blob[1] = 'C'; blob[2] = 'L'; blob[3] = 'U';
    put_u32(blob + CRED_OFF_VER,  CRED_VERSION);
    put_u32(blob + CRED_OFF_ITER, c->iter);
    put_u32(blob + CRED_OFF_FLAG, c->must_change ? 1u : 0u);
    memcpy(blob + CRED_OFF_SALT, c->salt, CONSOLE_SALT_LEN);
    memcpy(blob + CRED_OFF_KEY,  c->key,  CONSOLE_KEY_LEN);
    memcpy(blob + CRED_OFF_USER, c->user, CRED_USER_CAP - 1); /* 末字节留 NUL */

    if (crypto_store_available()) {
        rc = hal()->crypto->secure_write(HAL_SEC_KEY_LOCAL_USER, blob, sizeof(blob));
    } else {
        /* 退化软存储：私有目录 0700 + 文件 0600。文件内含 stored_key，
           读到它即可算出 proof 完成鉴权，权限收紧不是可选项。
           关键点同样是它不经 core/config。 */
        const char *path = console_auth_cred_path();
        cred_dir_prepare();
        rc = os_file_write_atomic(path, blob, sizeof(blob)) == 0 ? HAL_OK : HAL_EIO;
#ifndef _WIN32
        if (rc == HAL_OK && chmod(path, S_IRUSR | S_IWUSR) != 0)   /* 0600 */
            LOGW(MOD, "凭据软存储权限收紧失败，请检查数据目录挂载与 umask");
#endif
    }
    secure_wipe(blob, sizeof(blob));
    return rc;
}

static hal_err_t cred_fetch(cred_t *c)
{
    uint8_t blob[CRED_BLOB_LEN];
    size_t len = 0;
    hal_err_t rc;

    memset(blob, 0, sizeof(blob));
    if (crypto_store_available()) {
        rc = hal()->crypto->secure_read(HAL_SEC_KEY_LOCAL_USER, blob, sizeof(blob), &len);
        if (rc != HAL_OK) return rc;
    } else {
        size_t flen = 0;
        char *raw = os_file_read_all(console_auth_cred_path(), &flen);
        if (!raw) return HAL_ENODEV;
        if (flen == sizeof(blob)) { memcpy(blob, raw, sizeof(blob)); len = flen; }
        secure_wipe(raw, flen);
        free(raw);
    }

    if (len != sizeof(blob)) return HAL_ECORRUPT;
    if (blob[0] != 'I' || blob[1] != 'C' || blob[2] != 'L' || blob[3] != 'U') return HAL_ECORRUPT;
    if (get_u32(blob + CRED_OFF_VER) != CRED_VERSION) return HAL_ECORRUPT;

    memset(c, 0, sizeof(*c));
    c->iter = get_u32(blob + CRED_OFF_ITER);
    c->must_change = get_u32(blob + CRED_OFF_FLAG) != 0;
    memcpy(c->salt, blob + CRED_OFF_SALT, CONSOLE_SALT_LEN);
    memcpy(c->key,  blob + CRED_OFF_KEY,  CONSOLE_KEY_LEN);
    memcpy(c->user, blob + CRED_OFF_USER, CRED_USER_CAP - 1);
    c->user[CRED_USER_CAP - 1] = '\0';
    c->valid = c->iter > 0 && c->iter <= CRED_ITER_MAX && c->user[0] != '\0';
    secure_wipe(blob, sizeof(blob));
    if (!c->valid) memset(c, 0, sizeof(*c));
    return c->valid ? HAL_OK : HAL_ECORRUPT;
}

/** 首次访问时装载凭据；`console_auth_init` 在 http_server 启动前先调一次，
 *  使请求路径上不会出现读存储的阻塞动作 */
static void cred_ensure_loaded(void)
{
    if (s_cred_loaded) return;
    s_cred_loaded = true;               /* 无论成败都只尝试一次 */
    if (cred_fetch(&s_cred) != HAL_OK) {
        memset(&s_cred, 0, sizeof(s_cred));
        LOGI(MOD, "尚无本地账号凭据，等待以出厂验证码播种");
    }
}

/** 本地账号名：取 config 的 localUser.name（非机密），缺省 admin */
static void auth_user_name(char *out, size_t cap)
{
    out[0] = '\0';
    if (cfg_get_str("localUser.name", out, cap) != HAL_OK || out[0] == '\0')
        snprintf(out, cap, "%s", CONSOLE_DEFAULT_USER);
}

static uint32_t auth_iter(void)
{
    return s_cred.valid ? s_cred.iter : (uint32_t)CONSOLE_ITER;
}

static const uint8_t *decoy_key(void)
{
    if (!s_decoy_ready) {
        if (rand_bytes(s_decoy_key, sizeof(s_decoy_key)) != HAL_OK)
            memset(s_decoy_key, 0xA5, sizeof(s_decoy_key));
        s_decoy_ready = true;
    }
    return s_decoy_key;
}

/**
 * 不存在的用户名 → 由用户名派生的**伪造但稳定**的 salt，堵住用户名枚举。
 *
 * 派生密钥必须既机密又持久，所以用 stored_key：真实 salt 经 challenge 公开，
 * 若拿它当密钥，任何人都能自行算出伪 salt，从而反推"该用户名不存在"，
 * 枚举防护形同虚设。尚未播种时退回本次运行的随机密钥——此时所有用户名都不存在，
 * 本就无从区分。
 */
static hal_err_t decoy_salt(const char *user, uint8_t out[CONSOLE_SALT_LEN])
{
    uint8_t mac[32];
    char msg[24 + CONSOLE_USER_MAX];
    hal_err_t rc;

    snprintf(msg, sizeof(msg), "ipc-console-decoy|%s", user);
    rc = console_hmac_sha256(s_cred.valid ? s_cred.key : decoy_key(), CONSOLE_KEY_LEN,
                             (const uint8_t *)msg, strlen(msg), mac);
    if (rc != HAL_OK) return rc;
    memcpy(out, mac, CONSOLE_SALT_LEN);
    secure_wipe(mac, sizeof(mac));
    return HAL_OK;
}

/* ==========================================================================
 * 六、挑战 nonce 表（容量 CONSOLE_NONCE_MAX，60s 过期，用后即废）
 * ========================================================================== */

typedef struct {
    char     nonce[CONSOLE_HEX_CAP(CONSOLE_NONCE_LEN)];
    char     user[CONSOLE_USER_MAX];
    char     ip[CONSOLE_IP_MAX];
    uint64_t issued_us;
    bool     used;
} nonce_ent_t;

static nonce_ent_t s_nonces[CONSOLE_NONCE_MAX];

static void nonces_clear(void)
{
    secure_wipe(s_nonces, sizeof(s_nonces));
}

/**
 * 选一个 nonce 槽位。淘汰序刻意按来源 IP 分区：
 *   1) 空闲或已过期的槽位；
 *   2) **同一 IP** 的最旧条目；
 *   3) 实在没有才动别人的最旧条目。
 * `/api/v1/auth/challenge` 不需要会话、也不受登录锁定约束，若一律淘汰全表最旧，
 * 攻击者只要在运维取 challenge 与提交 login 之间插 4 个请求就能持续把运维挤出去。
 * 分区之后，只要攻击者自己已占着一个槽位，就再也挤不掉别人的。
 */
static nonce_ent_t *nonce_slot(uint64_t now, const char *ip)
{
    nonce_ent_t *same_ip = NULL, *oldest = &s_nonces[0];
    size_t i;

    for (i = 0; i < CONSOLE_NONCE_MAX; i++) {
        nonce_ent_t *e = &s_nonces[i];
        if (!e->used || now - e->issued_us > CONSOLE_NONCE_TTL_US) return e;
        if (strcmp(e->ip, ip) == 0 && (!same_ip || e->issued_us < same_ip->issued_us)) same_ip = e;
        if (e->issued_us < oldest->issued_us) oldest = e;
    }
    return same_ip ? same_ip : oldest;
}

static void nonce_add(const char *nonce, const char *user, const char *ip)
{
    uint64_t now = os_monotonic_us();
    nonce_ent_t *slot = nonce_slot(now, ip);

    memset(slot, 0, sizeof(*slot));
    snprintf(slot->nonce, sizeof(slot->nonce), "%s", nonce);
    snprintf(slot->user, sizeof(slot->user), "%s", user);
    snprintf(slot->ip, sizeof(slot->ip), "%s", ip);
    slot->issued_us = now;
    slot->used = true;
}

/** 取用 nonce：命中即从表中删除（无论校验是否通过），杜绝重放 */
static bool nonce_take(const char *nonce, const char *user)
{
    uint64_t now = os_monotonic_us();
    size_t i;

    for (i = 0; i < CONSOLE_NONCE_MAX; i++) {
        nonce_ent_t *e = &s_nonces[i];
        bool ok;
        if (!e->used || strcmp(e->nonce, nonce) != 0) continue;
        ok = (now - e->issued_us <= CONSOLE_NONCE_TTL_US) && strcmp(e->user, user) == 0;
        memset(e, 0, sizeof(*e));
        return ok;
    }
    return false;
}

/* ==========================================================================
 * 七、会话表（容量 CONSOLE_SESSION_MAX，空闲 30 分钟过期，不持久化）
 * ========================================================================== */

typedef struct {
    char     token[CONSOLE_HEX_CAP(CONSOLE_TOKEN_LEN)];
    char     user[CONSOLE_USER_MAX];
    uint64_t last_us;
    bool     used;
} sess_t;

static sess_t s_sessions[CONSOLE_SESSION_MAX];

static void sessions_clear(void)
{
    secure_wipe(s_sessions, sizeof(s_sessions));
}

static hal_err_t session_new(const char *user, char *token, size_t cap)
{
    uint8_t raw[CONSOLE_TOKEN_LEN];
    sess_t *slot = &s_sessions[0];
    uint64_t now = os_monotonic_us();
    hal_err_t rc;
    size_t i;

    if (cap < CONSOLE_HEX_CAP(CONSOLE_TOKEN_LEN)) return HAL_ENOMEM;
    if ((rc = rand_bytes(raw, sizeof(raw))) != HAL_OK) return rc;

    for (i = 0; i < CONSOLE_SESSION_MAX; i++) {
        sess_t *s = &s_sessions[i];
        if (!s->used || now - s->last_us > CONSOLE_SESSION_IDLE_US) { slot = s; break; }
        if (s->last_us < slot->last_us) slot = s;   /* 超出并发上限则踢最旧 */
    }
    memset(slot, 0, sizeof(*slot));
    rc = hex_encode(raw, sizeof(raw), slot->token, sizeof(slot->token));
    secure_wipe(raw, sizeof(raw));
    if (rc != HAL_OK) return rc;
    snprintf(slot->user, sizeof(slot->user), "%s", user);
    slot->last_us = now;
    slot->used = true;
    snprintf(token, cap, "%s", slot->token);
    return HAL_OK;
}

/** 查会话并顺延空闲计时；不早退，避免以耗时暴露命中槽位 */
static sess_t *session_find(const char *token)
{
    uint64_t now = os_monotonic_us();
    sess_t *hit = NULL;
    size_t i;

    for (i = 0; i < CONSOLE_SESSION_MAX; i++) {
        sess_t *s = &s_sessions[i];
        if (!s->used) continue;
        if (now - s->last_us > CONSOLE_SESSION_IDLE_US) { secure_wipe(s, sizeof(*s)); continue; }
        if (ct_str_equal(s->token, token)) hit = s;
    }
    if (hit) hit->last_us = now;
    return hit;
}

static void session_drop(const char *token)
{
    size_t i;
    for (i = 0; i < CONSOLE_SESSION_MAX; i++) {
        sess_t *s = &s_sessions[i];
        if (s->used && ct_str_equal(s->token, token)) secure_wipe(s, sizeof(*s));
    }
}

/* ==========================================================================
 * 八、按来源 IP 的失败锁定（容量 CONSOLE_LOCK_IPS，5 次锁 60s，2 倍退避至 15 分钟）
 * ========================================================================== */

typedef struct {
    char     ip[CONSOLE_IP_MAX];
    uint32_t fails;
    uint32_t lock_ms;           /**< 上一次锁定时长，用于 2 倍退避 */
    uint64_t lock_until_us;
    uint64_t last_us;
    bool     used;
} lock_ent_t;

static lock_ent_t s_locks[CONSOLE_LOCK_IPS];

/** 淘汰序：锁定中的按解锁时刻排，未锁定的按最后活动时刻排 —— 取最小者淘汰，
 *  这样攻击者无法用另外 8 个来源 IP 把自己的锁定项挤掉 */
static uint64_t lock_rank(const lock_ent_t *e)
{
    return e->lock_until_us > e->last_us ? e->lock_until_us : e->last_us;
}

static lock_ent_t *lock_get(const char *ip, bool create)
{
    lock_ent_t *victim = NULL;
    size_t i;

    for (i = 0; i < CONSOLE_LOCK_IPS; i++) {
        if (s_locks[i].used && strcmp(s_locks[i].ip, ip) == 0) return &s_locks[i];
    }
    if (!create) return NULL;

    for (i = 0; i < CONSOLE_LOCK_IPS; i++) {
        if (!s_locks[i].used) { victim = &s_locks[i]; break; }
    }
    if (!victim) {
        victim = &s_locks[0];
        for (i = 1; i < CONSOLE_LOCK_IPS; i++) {
            if (lock_rank(&s_locks[i]) < lock_rank(victim)) victim = &s_locks[i];
        }
    }
    memset(victim, 0, sizeof(*victim));
    snprintf(victim->ip, sizeof(victim->ip), "%s", ip);
    victim->last_us = os_monotonic_us();
    victim->used = true;
    return victim;
}

static bool lock_blocked(const char *ip)
{
    const lock_ent_t *e = lock_get(ip, false);
    return e != NULL && os_monotonic_us() < e->lock_until_us;
}

static void lock_fail(const char *ip)
{
    lock_ent_t *e = lock_get(ip, true);
    if (!e) return;
    e->last_us = os_monotonic_us();
    if (++e->fails < CONSOLE_FAIL_LIMIT) return;

    e->fails = 0;   /* 解锁后重新计数，下一轮锁定时长翻倍 */
    if (e->lock_ms == 0) e->lock_ms = CONSOLE_LOCK_BASE_MS;
    else if (e->lock_ms >= CONSOLE_LOCK_MAX_MS / 2) e->lock_ms = CONSOLE_LOCK_MAX_MS;
    else e->lock_ms *= 2;
    e->lock_until_us = e->last_us + (uint64_t)e->lock_ms * 1000ull;
    LOGW(MOD, "来源 %s 连续 %d 次登录失败，锁定 %u 秒", ip, CONSOLE_FAIL_LIMIT,
         (unsigned)(e->lock_ms / 1000u));
}

static void lock_ok(const char *ip)
{
    lock_ent_t *e = lock_get(ip, false);
    if (e) memset(e, 0, sizeof(*e));   /* 登录成功即清计数与退避 */
}

/* ==========================================================================
 * 九、对外鉴权 API
 * ========================================================================== */

hal_err_t console_auth_seed(const char *factory_code)
{
    cred_t c;
    hal_err_t rc;

    if (!factory_code || !factory_code[0]) return HAL_EINVAL;

    memset(&c, 0, sizeof(c));
    auth_user_name(c.user, sizeof(c.user));
    c.iter = CONSOLE_ITER;
    if ((rc = rand_bytes(c.salt, sizeof(c.salt))) != HAL_OK) goto fail;
    rc = console_pbkdf2_sha256(factory_code, strlen(factory_code), c.salt, sizeof(c.salt),
                               c.iter, c.key, sizeof(c.key));
    if (rc != HAL_OK) goto fail;
    c.must_change = true;   /* 出厂态：登录后必须先改密 */
    c.valid = true;

    if ((rc = cred_persist(&c)) != HAL_OK) {
        LOGE(MOD, "本地账号凭据持久化失败：%s", hal_strerror(rc));
        goto fail;
    }

    s_cred = c;
    s_cred_loaded = true;
    sessions_clear();
    nonces_clear();
    secure_wipe(&c, sizeof(c));
    LOGI(MOD, "本地账号凭据已按出厂验证码初始化（用户 %s，迭代 %u，需强制改密）",
         s_cred.user, (unsigned)s_cred.iter);
    return HAL_OK;

fail:
    secure_wipe(&c, sizeof(c));
    return rc;
}

hal_err_t console_auth_challenge_from(const char *user, char *salt_hex, size_t salt_cap,
                                      char *nonce, size_t nonce_cap, const char *client_ip)
{
    const char *ip = (client_ip && client_ip[0]) ? client_ip : CONSOLE_IP_UNKNOWN;
    uint8_t salt[CONSOLE_SALT_LEN];
    uint8_t raw[CONSOLE_NONCE_LEN];
    char text[CONSOLE_HEX_CAP(CONSOLE_NONCE_LEN)];
    hal_err_t rc;

    if (!user || !user[0] || !salt_hex || !nonce) return HAL_EINVAL;
    if (strlen(user) >= CONSOLE_USER_MAX) return HAL_EINVAL;
    if (salt_cap < CONSOLE_HEX_CAP(CONSOLE_SALT_LEN) || nonce_cap < sizeof(text)) return HAL_ENOMEM;

    cred_ensure_loaded();

    if (s_cred.valid && strcmp(user, s_cred.user) == 0) memcpy(salt, s_cred.salt, sizeof(salt));
    else if ((rc = decoy_salt(user, salt)) != HAL_OK) return rc;

    if ((rc = hex_encode(salt, sizeof(salt), salt_hex, salt_cap)) != HAL_OK) return rc;
    if ((rc = rand_bytes(raw, sizeof(raw))) != HAL_OK) return rc;
    if ((rc = hex_encode(raw, sizeof(raw), text, sizeof(text))) != HAL_OK) return rc;
    secure_wipe(raw, sizeof(raw));

    nonce_add(text, user, ip);
    snprintf(nonce, nonce_cap, "%s", text);
    return HAL_OK;
}

hal_err_t console_auth_challenge(const char *user, char *salt_hex, size_t salt_cap,
                                 char *nonce, size_t nonce_cap)
{
    return console_auth_challenge_from(user, salt_hex, salt_cap, nonce, nonce_cap, NULL);
}

hal_err_t console_auth_verify_from(const char *user, const char *nonce, const char *proof,
                                   const char *client_ip)
{
    const char *ip = (client_ip && client_ip[0]) ? client_ip : CONSOLE_IP_UNKNOWN;
    uint8_t mac[32];
    char expect[CONSOLE_HEX_CAP(CONSOLE_KEY_LEN)];
    bool nonce_ok, user_ok, proof_ok;

    if (!user || !nonce || !proof) return HAL_EINVAL;
    cred_ensure_loaded();

    if (lock_blocked(ip)) return HAL_EBUSY;

    nonce_ok = nonce_take(nonce, user);   /* 命中即作废，重放必失败 */
    user_ok  = s_cred.valid && strcmp(user, s_cred.user) == 0;

    /* 用户不存在时同样跑一次 HMAC 并比对（用 decoy 密钥），
       使"用户名是否存在"不能从响应耗时上区分 */
    if (console_hmac_sha256(user_ok ? s_cred.key : decoy_key(), CONSOLE_KEY_LEN,
                            (const uint8_t *)nonce, strlen(nonce), mac) != HAL_OK)
        return HAL_EIO;
    if (hex_encode(mac, sizeof(mac), expect, sizeof(expect)) != HAL_OK) return HAL_EIO;
    proof_ok = ct_str_equal(expect, proof);
    secure_wipe(mac, sizeof(mac));
    secure_wipe(expect, sizeof(expect));

    if (!nonce_ok || !user_ok || !proof_ok) {
        lock_fail(ip);
        LOGW(MOD, "登录校验失败（来源 %s）", ip);
        return HAL_EPERM_;
    }
    lock_ok(ip);
    return HAL_OK;
}

hal_err_t console_auth_verify(const char *user, const char *nonce, const char *proof)
{
    return console_auth_verify_from(user, nonce, proof, NULL);
}

hal_err_t console_auth_make_proof(const char *pwd, const char *salt_hex, const char *nonce,
                                  char *proof, size_t proof_cap)
{
    uint8_t salt[PBKDF2_SALT_MAX], client_key[CONSOLE_KEY_LEN], mac[32];
    size_t salt_len = 0;
    hal_err_t rc;

    if (!pwd || !salt_hex || !nonce || !proof) return HAL_EINVAL;
    if (proof_cap < CONSOLE_HEX_CAP(CONSOLE_KEY_LEN)) return HAL_ENOMEM;
    if ((rc = hex_decode(salt_hex, salt, sizeof(salt), &salt_len)) != HAL_OK) return rc;

    /* 迭代次数取本模块常量：challenge 也是按这个数下发的（见 auth_iter） */
    rc = console_pbkdf2_sha256(pwd, strlen(pwd), salt, salt_len, CONSOLE_ITER,
                               client_key, sizeof(client_key));
    if (rc == HAL_OK)
        rc = console_hmac_sha256(client_key, sizeof(client_key),
                                 (const uint8_t *)nonce, strlen(nonce), mac);
    if (rc == HAL_OK)
        rc = hex_encode(mac, sizeof(mac), proof, proof_cap);

    secure_wipe(salt, sizeof(salt));
    secure_wipe(client_key, sizeof(client_key));
    secure_wipe(mac, sizeof(mac));
    return rc;
}

bool console_auth_must_change(void)
{
    cred_ensure_loaded();
    return s_cred.valid ? s_cred.must_change : true;   /* 未播种：按最严处理 */
}

hal_err_t console_auth_set_password(const char *old_pwd, const char *new_pwd)
{
    cred_t c;
    uint8_t check[CONSOLE_KEY_LEN];
    size_t new_len;
    hal_err_t rc;
    bool old_ok;

    if (!old_pwd || !new_pwd) return HAL_EINVAL;
    cred_ensure_loaded();
    if (!s_cred.valid) return HAL_ESTATE;

    new_len = strlen(new_pwd);
    if (new_len < CONSOLE_PWD_MIN || new_len > CONSOLE_PWD_MAX) return HAL_EINVAL;
    if (strcmp(new_pwd, old_pwd) == 0) return HAL_EINVAL;

    rc = console_pbkdf2_sha256(old_pwd, strlen(old_pwd), s_cred.salt, CONSOLE_SALT_LEN,
                               s_cred.iter, check, sizeof(check));
    if (rc != HAL_OK) return rc;
    old_ok = ct_equal(check, s_cred.key, CONSOLE_KEY_LEN);
    secure_wipe(check, sizeof(check));
    if (!old_ok) {
        LOGW(MOD, "改密被拒：旧口令不符");
        return HAL_EPERM_;
    }

    c = s_cred;
    c.iter = CONSOLE_ITER;
    if ((rc = rand_bytes(c.salt, sizeof(c.salt))) != HAL_OK) goto fail;   /* 换新盐 */
    rc = console_pbkdf2_sha256(new_pwd, new_len, c.salt, sizeof(c.salt), c.iter,
                               c.key, sizeof(c.key));
    if (rc != HAL_OK) goto fail;
    c.must_change = false;
    if ((rc = cred_persist(&c)) != HAL_OK) goto fail;

    s_cred = c;
    secure_wipe(&c, sizeof(c));
    sessions_clear();   /* 改密后全部会话失效，必须以新口令重新登录 */
    nonces_clear();
    LOGI(MOD, "本地账号口令已修改，全部会话已失效");
    return HAL_OK;

fail:
    secure_wipe(&c, sizeof(c));
    return rc;
}

/**
 * 掩码 = HMAC(stored_key, <用途标签> || "|" || nonce)。
 *
 * 域分隔在这里是必需项而不是洁癖，两层理由：
 *  - 与登录 proof（= HMAC(stored_key, nonce)）分隔：proof 本身明文上送，
 *    若掩码复用同一条 HMAC，等于把掩码随包附送；
 *  - 两条掩码彼此也必须分隔：同一 nonce 下两个不同明文若共用一条掩码，
 *    观察者把两个密文异或就直接得到两份明文的异或。
 */
static hal_err_t mask_derive(const char *tag, const char *nonce, uint8_t out[32])
{
    char msg[16 + CONSOLE_HEX_CAP(CONSOLE_NONCE_LEN)];
    if (fmt_safe(msg, sizeof(msg), "%s|%s", tag, nonce) != HAL_OK) return HAL_EINVAL;
    return console_hmac_sha256(s_cred.key, CONSOLE_KEY_LEN,
                               (const uint8_t *)msg, strlen(msg), out);
}

/** 解掩码：out = masked XOR HMAC(stored_key, tag||"|"||nonce) */
static hal_err_t unmask(const char *tag, const char *nonce,
                        const uint8_t masked[CONSOLE_KEY_LEN], uint8_t out[CONSOLE_KEY_LEN])
{
    uint8_t mask[32];
    size_t i;
    hal_err_t rc = mask_derive(tag, nonce, mask);
    if (rc != HAL_OK) return rc;
    for (i = 0; i < CONSOLE_KEY_LEN; i++) out[i] = (uint8_t)(masked[i] ^ mask[i]);
    secure_wipe(mask, sizeof(mask));
    return HAL_OK;
}

hal_err_t console_auth_set_key_masked(const char *user, const char *nonce, const char *proof,
                                      const char *new_salt_hex, const char *masked_key_hex,
                                      const char *masked_chk_hex, const char *client_ip)
{
    cred_t c;
    uint8_t new_salt[CONSOLE_SALT_LEN];
    uint8_t masked_key[CONSOLE_KEY_LEN], masked_chk[CONSOLE_KEY_LEN], chk[CONSOLE_KEY_LEN];
    size_t n = 0;
    hal_err_t rc;
    bool same_pwd;

    if (!user || !nonce || !proof || !new_salt_hex || !masked_key_hex || !masked_chk_hex)
        return HAL_EINVAL;
    cred_ensure_loaded();
    if (!s_cred.valid) return HAL_ESTATE;

    /* 先解析再校验：格式错误不该消耗 nonce，也不该计进按 IP 的失败锁定 */
    rc = hex_decode(new_salt_hex, new_salt, sizeof(new_salt), &n);
    if (rc != HAL_OK || n != CONSOLE_SALT_LEN) return HAL_EINVAL;
    rc = hex_decode(masked_key_hex, masked_key, sizeof(masked_key), &n);
    if (rc != HAL_OK || n != CONSOLE_KEY_LEN) return HAL_EINVAL;
    rc = hex_decode(masked_chk_hex, masked_chk, sizeof(masked_chk), &n);
    if (rc != HAL_OK || n != CONSOLE_KEY_LEN) return HAL_EINVAL;

    /* 先验旧口令（含 nonce 一次性与按 IP 锁定），通过后才解掩码 */
    if ((rc = console_auth_verify_from(user, nonce, proof, client_ip)) != HAL_OK) {
        LOGW(MOD, "改密被拒：旧口令校验未通过");
        return rc;
    }

    /* 新旧口令不得相同。客户端另用**旧盐**算 chk = PBKDF2(新口令, old_salt, iter)：
       若新口令就是旧口令，chk 必然等于 stored_key。
       注意这条只对如实计算 chk 的客户端成立——服务端看不到明文，无法把客户端
       钉死在"chk 确实由新口令派生"上。它挡住的是"用户图省事把新口令填成出厂码"
       这一真实场景（也正是 must_change 存在的理由）；一个已经知道旧口令、
       刻意伪造 chk 的攻击者绕得过去，但那种攻击者本就能把设备改成任意口令，
       并不因此多拿到什么。零知识改密无法做得更强，详见报告。 */
    if ((rc = unmask("pwdchk", nonce, masked_chk, chk)) != HAL_OK) return rc;
    same_pwd = ct_equal(chk, s_cred.key, CONSOLE_KEY_LEN);
    secure_wipe(chk, sizeof(chk));
    secure_wipe(masked_chk, sizeof(masked_chk));
    if (same_pwd) {
        LOGW(MOD, "改密被拒：新口令与旧口令相同");
        return HAL_EINVAL;
    }

    c = s_cred;
    memcpy(c.salt, new_salt, sizeof(new_salt));
    /* XOR 遮蔽只保机密性、不保完整性（可延展），这不是 AEAD：中间人翻转密文位
       就能翻转 new_key 对应位。实际影响接近零——明文 HTTP 下主动中间人本来就能
       重写整条请求，而没有旧口令仍过不了上面的 verify_from，翻转的结果只是把设备
       改成一个谁都不知道的口令。别在此基础上假设它有完整性保护。 */
    if ((rc = unmask("pwdchg", nonce, masked_key, c.key)) != HAL_OK) goto fail;
    /* iter 保持不变：客户端正是按 challenge 下发的 auth_iter()（即当前 s_cred.iter）
       派生 new_key 的，这里改成别的值会与客户端算出的密钥失配。 */
    c.must_change = false;
    secure_wipe(masked_key, sizeof(masked_key));

    if ((rc = cred_persist(&c)) != HAL_OK) goto fail;
    s_cred = c;
    secure_wipe(&c, sizeof(c));
    sessions_clear();   /* 改密后全部会话失效，必须以新口令重新登录 */
    nonces_clear();
    LOGI(MOD, "本地账号口令已修改（掩码方式，口令未上线），全部会话已失效");
    return HAL_OK;

fail:
    secure_wipe(&c, sizeof(c));
    secure_wipe(masked_key, sizeof(masked_key));
    return rc;
}

/** 从 Cookie 头里取一个 cookie 值；不存在返回 HAL_ENODEV，超长返回 HAL_EINVAL */
static hal_err_t cookie_get(const char *hdr, const char *name, char *out, size_t cap)
{
    size_t nlen;
    const char *p = hdr;

    if (!hdr || !name || !out || cap == 0) return HAL_EINVAL;
    nlen = strlen(name);

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (!*p) break;
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            const char *end = v;
            while (*end && *end != ';') end++;
            if ((size_t)(end - v) >= cap) return HAL_EINVAL;
            memcpy(out, v, (size_t)(end - v));
            out[end - v] = '\0';
            return HAL_OK;
        }
        while (*p && *p != ';') p++;
    }
    return HAL_ENODEV;
}

/** 强制改密期间仍可访问的路径：鉴权端点本身与设备信息（前端渲染菜单要用） */
static bool path_exempt_from_must_change(const char *path)
{
    return strncmp(path, CONSOLE_AUTH_PREFIX, sizeof(CONSOLE_AUTH_PREFIX) - 1) == 0 ||
           strcmp(path, "/api/v1/system/info") == 0;
}

hal_err_t console_auth_check(const http_req_t *req)
{
    char token[CONSOLE_HEX_CAP(CONSOLE_TOKEN_LEN)];
    hal_err_t rc;

    if (!req) return HAL_EUNAUTH_;
    rc = cookie_get(http_header(req, "Cookie"), "token", token, sizeof(token));
    if (rc != HAL_OK) return HAL_EUNAUTH_;
    if (!session_find(token)) {
        secure_wipe(token, sizeof(token));
        return HAL_EUNAUTH_;
    }
    secure_wipe(token, sizeof(token));

    /* 服务端独立拦截，不依赖前端锁定改密页 */
    if (console_auth_must_change() && !path_exempt_from_must_change(req->path))
        return HAL_EPERM_;
    return HAL_OK;
}

/* ==========================================================================
 * 十、HTTP 端点：POST /api/v1/auth/{challenge,login,password,logout}
 *
 * 成功响应一律 200 + JSON；失败交给 console_reply_err 输出
 * {"code":<hal_err>,"msg":"中文说明"} 并映射状态码。
 * 响应体里只放十六进制串与数字，无需 JSON 转义。
 * ========================================================================== */

static const char *req_client_ip(const http_req_t *req)
{
    /* 只认 TCP 对端地址：设备直连局域网，采信 X-Forwarded-For 等于让攻击者
       自选锁定桶，按 IP 退避会被完全绕过。 */
    const char *ip = (req && req->conn) ? http_conn_peer_ip(req->conn) : NULL;
    return (ip && ip[0]) ? ip : CONSOLE_IP_UNKNOWN;
}

/** 取请求体里的字符串字段；缺失、为空或超长一律 HAL_EINVAL */
static hal_err_t body_str(const json_t *j, const char *key, char *out, size_t cap)
{
    const char *v = json_string(json_get(j, key), NULL);
    size_t len;
    if (!v || !v[0]) return HAL_EINVAL;
    len = strlen(v);
    if (len >= cap) return HAL_EINVAL;
    memcpy(out, v, len + 1);
    return HAL_OK;
}

static hal_err_t ep_challenge(const json_t *j, const char *ip, char *out, size_t cap)
{
    char user[CONSOLE_USER_MAX];
    char salt_hex[CONSOLE_HEX_CAP(CONSOLE_SALT_LEN)];
    char nonce[CONSOLE_HEX_CAP(CONSOLE_NONCE_LEN)];
    hal_err_t rc;

    if ((rc = body_str(j, "user", user, sizeof(user))) != HAL_OK) return rc;
    if ((rc = console_auth_challenge_from(user, salt_hex, sizeof(salt_hex),
                                          nonce, sizeof(nonce), ip)) != HAL_OK) return rc;
    return fmt_safe(out, cap,
                    "{\"code\":0,\"salt\":\"%s\",\"iter\":%u,\"nonce\":\"%s\",\"expire_s\":%u}",
                    salt_hex, (unsigned)auth_iter(), nonce,
                    (unsigned)(CONSOLE_NONCE_TTL_US / 1000000ull));
}

static hal_err_t ep_login(const json_t *j, const char *ip,
                          char *out, size_t cap, char *cookie, size_t cookie_cap)
{
    char user[CONSOLE_USER_MAX];
    char nonce[CONSOLE_HEX_CAP(CONSOLE_NONCE_LEN)];
    char proof[CONSOLE_HEX_CAP(CONSOLE_KEY_LEN)];
    char token[CONSOLE_HEX_CAP(CONSOLE_TOKEN_LEN)];
    hal_err_t rc;

    if ((rc = body_str(j, "user",  user,  sizeof(user)))  != HAL_OK) return rc;
    if ((rc = body_str(j, "nonce", nonce, sizeof(nonce))) != HAL_OK) return rc;
    if ((rc = body_str(j, "proof", proof, sizeof(proof))) != HAL_OK) return rc;

    if ((rc = console_auth_verify_from(user, nonce, proof, ip)) != HAL_OK) return rc;
    if ((rc = session_new(user, token, sizeof(token))) != HAL_OK) return rc;

    /* token 只经 HttpOnly Cookie 下发，不进响应体：明文 HTTP 下 token 本就可被
       嗅探（设计已接受该残留风险），至少不要再让页面脚本能读到它。 */
    rc = fmt_safe(cookie, cookie_cap, "token=%s; HttpOnly; SameSite=Strict; Path=/", token);
    secure_wipe(token, sizeof(token));
    if (rc != HAL_OK) return rc;

    rc = fmt_safe(out, cap, "{\"code\":0,\"must_change_password\":%s,\"idle_timeout_s\":%u}",
                  console_auth_must_change() ? "true" : "false",
                  (unsigned)(CONSOLE_SESSION_IDLE_US / 1000000ull));
    if (rc != HAL_OK) return rc;
    LOGI(MOD, "用户 %s 登录成功（来源 %s）", user, ip);
    return HAL_OK;
}

/** 下发一个立即失效的 token Cookie（改密与注销共用） */
static hal_err_t cookie_clear(char *cookie, size_t cap)
{
    return fmt_safe(cookie, cap, "token=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
}

/**
 * 改密：与登录同样"口令不过线"。请求体全部是十六进制串与用户名，没有任何明文口令。
 * 代价是服务端看不到明文，无法在此校验口令强度（长度/复杂度由前端把关，
 * `console_auth_set_password` 的 8..63 限制只覆盖本地/测试路径）。
 */
static hal_err_t ep_password(const json_t *j, const http_req_t *req, const char *ip,
                             char *out, size_t cap, char *cookie, size_t cookie_cap)
{
    char user[CONSOLE_USER_MAX];
    char nonce[CONSOLE_HEX_CAP(CONSOLE_NONCE_LEN)];
    char proof[CONSOLE_HEX_CAP(CONSOLE_KEY_LEN)];
    char new_salt[CONSOLE_HEX_CAP(CONSOLE_SALT_LEN)];
    char masked[CONSOLE_HEX_CAP(CONSOLE_KEY_LEN)];
    char masked_chk[CONSOLE_HEX_CAP(CONSOLE_KEY_LEN)];
    hal_err_t rc;

    /* 需已登录；CONSOLE_AUTH_PREFIX 已从强制改密拦截里豁免，故此处可达 */
    if ((rc = console_auth_check(req)) != HAL_OK) return rc;

    if ((rc = body_str(j, "user",  user,  sizeof(user)))  != HAL_OK) return rc;
    if ((rc = body_str(j, "nonce", nonce, sizeof(nonce))) != HAL_OK) return rc;
    if ((rc = body_str(j, "proof", proof, sizeof(proof))) != HAL_OK) return rc;
    if ((rc = body_str(j, "new_salt", new_salt, sizeof(new_salt))) != HAL_OK) return rc;
    if ((rc = body_str(j, "new_key_masked", masked, sizeof(masked))) != HAL_OK) return rc;
    if ((rc = body_str(j, "chk_masked", masked_chk, sizeof(masked_chk))) != HAL_OK) return rc;

    rc = console_auth_set_key_masked(user, nonce, proof, new_salt, masked, masked_chk, ip);
    if (rc != HAL_OK) return rc;

    if ((rc = cookie_clear(cookie, cookie_cap)) != HAL_OK) return rc; /* 会话已全部作废，同步清掉浏览器 Cookie */
    return fmt_safe(out, cap, "{\"code\":0,\"msg\":\"口令已修改，请以新口令重新登录\"}");
}

static hal_err_t ep_logout(const http_req_t *req, char *out, size_t cap,
                           char *cookie, size_t cookie_cap)
{
    char token[CONSOLE_HEX_CAP(CONSOLE_TOKEN_LEN)];
    hal_err_t rc;

    if ((rc = console_auth_check(req)) != HAL_OK) return rc;
    if (cookie_get(http_header(req, "Cookie"), "token", token, sizeof(token)) == HAL_OK) {
        session_drop(token);
        secure_wipe(token, sizeof(token));
    }
    if ((rc = cookie_clear(cookie, cookie_cap)) != HAL_OK) return rc;
    return fmt_safe(out, cap, "{\"code\":0,\"msg\":\"已注销\"}");
}

/**
 * 分发 /api/v1/auth/ 下的四个端点。
 * 返回 HAL_OK 时 out 为 200 响应体、cookie 为 Set-Cookie 值（无则空串）；
 * 其他返回值由调用方交给 console_reply_err。
 */
static hal_err_t auth_dispatch(const http_req_t *req, const char *ip_override,
                               char *out, size_t cap, char *cookie, size_t cookie_cap)
{
    const size_t plen = sizeof(CONSOLE_AUTH_PREFIX) - 1;
    const char *sub;
    const char *ip;
    json_t *j = NULL;
    hal_err_t rc;

    if (!req || !out || cap == 0 || !cookie || cookie_cap == 0) return HAL_EINVAL;
    /* ip_override 只由测试桩传入（构造不出真实连接）；生产路径恒为 NULL */
    ip = (ip_override && ip_override[0]) ? ip_override : req_client_ip(req);
    out[0] = '\0';
    cookie[0] = '\0';

    if (strncmp(req->path, CONSOLE_AUTH_PREFIX, plen) != 0) return HAL_ENODEV;
    sub = req->path + plen;
    if (strcmp(sub, "challenge") != 0 && strcmp(sub, "login") != 0 &&
        strcmp(sub, "password") != 0 && strcmp(sub, "logout") != 0)
        return HAL_ENODEV;

    /* 四个端点均为 POST：写操作不应可由 <img src> 之类的跨站 GET 触发 */
    if (strcmp(req->method, "POST") != 0) return HAL_EINVAL;
    if (req->body_len > CONSOLE_BODY_MAX) return HAL_EINVAL;
    if (req->body_len) {
        j = json_parse(req->body, req->body_len, NULL, 0);
        if (!j) return HAL_EINVAL;
    }

    if (strcmp(sub, "challenge") == 0)     rc = ep_challenge(j, ip, out, cap);
    else if (strcmp(sub, "login") == 0)    rc = ep_login(j, ip, out, cap, cookie, cookie_cap);
    else if (strcmp(sub, "password") == 0) rc = ep_password(j, req, ip, out, cap, cookie, cookie_cap);
    else                                   rc = ep_logout(req, out, cap, cookie, cookie_cap);

    json_free(j);
    return rc;
}

static int console_auth_handler(http_req_t *req, void *user)
{
    char body[320], cookie[192];
    hal_err_t rc;

    (void)user;
    rc = auth_dispatch(req, NULL, body, sizeof(body), cookie, sizeof(cookie));
    if (rc != HAL_OK) return console_reply_err(req->conn, rc);

    if (cookie[0]) {
        char extra[256];
        if (fmt_safe(extra, sizeof(extra), "Set-Cookie: %s\r\n", cookie) != HAL_OK)
            return console_reply_err(req->conn, HAL_EIO);
        return http_respond_ex(req->conn, 200, "application/json; charset=utf-8",
                               extra, body, strlen(body));
    }
    return http_respond_json(req->conn, 200, body);
}

/**
 * 首次启动 / 恢复出厂后自动播种：从安全存储读产线烧录的出厂验证码
 * （HAL 为此预留了 HAL_SEC_KEY_VERIFY_CODE），派生凭据并置 must_change。
 *
 * 没有这一步，`s_cred.valid` 在真机上永远为假，每次登录都返回 HAL_EPERM_——
 * 控制台根本进不去。两条纪律：
 *  - 取不到验证码时**绝不静默放行**，打明确的 ERROR，登录继续失败；
 *  - 但也**不让 console_init 失败**，否则整个 console 模块起不来，连诊断页都没了。
 */
static void cred_bootstrap_from_factory_code(void)
{
    uint8_t raw[64];
    char code[sizeof(raw) + 1];
    size_t len = 0;
    hal_err_t rc;

    if (s_cred.valid) return;

    if (!crypto_store_available()) {
        LOGE(MOD, "无安全存储可读出厂验证码：本地控制台将无法登录，"
                  "该平台需实现 hal_crypto 或预置凭据");
        return;
    }
    rc = hal()->crypto->secure_read(HAL_SEC_KEY_VERIFY_CODE, raw, sizeof(raw), &len);
    if (rc != HAL_OK || len == 0 || len > sizeof(raw)) {
        LOGE(MOD, "安全存储中没有出厂验证码（键 %s，rc=%d）：本地控制台将无法登录，"
                  "需产线烧录后重启或恢复出厂", HAL_SEC_KEY_VERIFY_CODE, (int)rc);
        secure_wipe(raw, sizeof(raw));
        return;
    }

    memcpy(code, raw, len);
    code[len] = '\0';
    /* 产线写入可能带尾随换行/空白，去掉后再派生 */
    while (len > 0 && (code[len - 1] == '\n' || code[len - 1] == '\r' ||
                       code[len - 1] == ' '  || code[len - 1] == '\t')) code[--len] = '\0';

    if (len == 0) {
        LOGE(MOD, "出厂验证码为空：本地控制台将无法登录");
    } else if ((rc = console_auth_seed(code)) != HAL_OK) {
        LOGE(MOD, "以出厂验证码播种本地账号失败：%s（控制台将无法登录）", hal_strerror(rc));
    }
    secure_wipe(code, sizeof(code));
    secure_wipe(raw, sizeof(raw));
}

hal_err_t console_auth_init(void)
{
    /* 启动期读一次凭据：请求路径上因此不会出现读安全存储/文件的阻塞动作 */
    cred_ensure_loaded();
    cred_bootstrap_from_factory_code();
    return http_route(CONSOLE_AUTH_PREFIX, console_auth_handler, NULL);
}

#ifdef IPC_TESTING

void console_auth_reset_lockout(void)
{
    memset(s_locks, 0, sizeof(s_locks));
}

void console_auth_test_reload(void)
{
    secure_wipe(&s_cred, sizeof(s_cred));
    s_cred_loaded = false;
}

hal_err_t console_auth_test_dispatch(const http_req_t *req, const char *client_ip,
                                     char *body, size_t body_cap,
                                     char *set_cookie, size_t cookie_cap)
{
    return auth_dispatch(req, client_ip, body, body_cap, set_cookie, cookie_cap);
}

#endif /* IPC_TESTING */
