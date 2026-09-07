/**
 * @file http_ws.c
 * @brief http_server 模块的 WebSocket 升级、收发帧与心跳保活实现
 *
 * 与 http_server.c 共享 http_server_internal.h 里 struct http_conn 的完整布局，
 * 通过该头声明的少量函数互相调用（conn_update_poll_interest/conn_close 由
 * http_server.c 提供；http_ws_on_readable/http_ws_flush/http_ws_conn_cleanup/
 * http_ws_tick 由本文件提供）。
 *
 * 线程模型：
 *   - 事件循环线程（epoll/select 所在线程）：http_ws_upgrade（在路由 handler
 *     内同步执行）、http_ws_on_readable、http_ws_flush、http_ws_tick、
 *     http_ws_conn_cleanup 均只在这个线程上运行，互相之间天然不并发。
 *   - 任意其他线程：http_ws_send/http_ws_send_text/http_ws_close/
 *     http_ws_queue_used/http_ws_dropped 可从任意线程调用（例如未来的采集/
 *     编码线程把视频帧推给某个正在直播的连接）。
 *   - 两类线程通过每条连接一把的 struct http_ws_state::mu 互斥锁同步。真实
 *     不变量（供后续任务——尤其 Task 10 预览与录像回放 B 线——判断能否在
 *     http_ws_send 的生产者线程里做时延敏感的事）：
 *       · 锁内允许调用非阻塞 send()：http_ws_flush 的临界区里就有一次
 *         send()，这是刻意的选择而非疏漏——连接 fd 在 accept 时已
 *         set_nonblocking，一次非阻塞 send() 系统调用耗时有界，不会因对端
 *         迟迟不收数据而被无限拖长，故没有搬到锁外的必要（task-4 集成说明
 *         明确把这一取舍留给实现者裁量）。
 *       · 锁内绝不调用调用方注册的回调（text_fn）：接收路径总是先在锁内把
 *         回调指针复制到局部变量、解锁之后才调用，避免回调同步重入
 *         http_ws_send/http_ws_close 等函数时对同一把非递归锁死锁。
 *       · 锁内绝不调用 conn_close：conn_close 只能在事件循环线程调用且要求
 *         调用时不持有任何 WS 锁，http_ws_flush/http_ws_tick/收到 close 帧
 *         三处调用点均已先解锁再调用它。
 *
 * 已知限制（详见任务报告"关注点"一节）：
 *   1. 不支持分片消息（continuation frame，opcode 0x0/FIN=0 的延续帧）：
 *      控制指令与状态文本都很短，实践中不需要跨帧重组；非 FIN 帧会被正确
 *      跳过字节数但不做任何语义处理。
 *   2. 跨线程的连接生命周期竞争：若 http_ws_send 正在执行时，事件循环线程
 *      恰好因为其他原因（心跳超时/收到 close 帧/send 硬错误）关闭了同一条
 *      连接，struct http_ws_state 有被并发释放的理论风险。当前 brief 与
 *      集成说明均未要求引入引用计数/世代号机制，本实现暂不处理，留给后续
 *      任务（真正接入采集/编码线程时）解决。
 */
#include "http_server_internal.h"

#include "core/log.h"
#include "core/os.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD "http_ws"

/** 心跳周期：每 15 秒发一次 ping；连续 2 次（即 30 秒内两次）未收到 pong 判定断开。 */
#define WS_PING_INTERVAL_US (15ULL * 1000 * 1000)

/** 与 http_server.c 内部私有的 CONN_MAX 同值——该宏本文件不可见，独立声明一份。
 *  已升级 WS 的连接数不可能超过服务器总连接数上限，取相同值即可覆盖全部场景。 */
#define WS_REGISTRY_MAX 16

/** WS 帧 payload 上限：直接复用已有的 HTTP_BODY_MAX（http_server.h 公开常量）
 *  作为经过验证的安全上限。本文件看不到 http_server.c 私有的
 *  CONN_BUF_MAX（= HTTP_BODY_MAX + 8192），但只要本上限比它小，合法帧就总能
 *  在 c->rbuf 里完整累积，不会误触发通用的"接收缓冲溢出"关闭路径。 */
#define WS_MAX_FRAME_PAYLOAD HTTP_BODY_MAX

/* ============================================================================
 * WS 连接状态：环形发送缓冲 + 背压状态机 + 心跳/关闭标志
 * ==========================================================================*/

struct http_ws_state {
    os_mutex_t *mu; /**< 保护本结构体全部字段的统一锁。心跳时间戳/失联计数
                         目前只在事件循环线程访问，本可不加锁，但统一用同一把
                         锁保护全部字段可以省去"哪些字段需要加锁"的心智负担。
                         多数临界区是纯内存读写，唯一例外是 http_ws_flush：它
                         的临界区内会调用一次非阻塞 send()（fd 早已
                         set_nonblocking，耗时有界，是刻意的取舍而非疏漏，
                         详见文件头线程模型一节）。所有临界区一致遵守的两条
                         硬约束：绝不调用调用方注册的回调（text_fn），绝不
                         调用 conn_close。 */

    /* 发送环形缓冲：仅记账，真正 send() 只发生在 http_ws_flush（事件循环线程） */
    uint8_t *buf;
    size_t   cap;
    size_t   head;      /**< 下一个待发送字节在 buf 中的位置 */
    size_t   used;      /**< 已入队未发送的字节数 */
    bool     dropping;  /**< 背压状态机：true=正在丢弃非关键帧，等下一个关键帧恢复 */
    uint64_t dropped;   /**< 累计被丢弃的帧数 */

    /* 上行文本回调 */
    http_ws_text_fn text_fn;
    void           *text_user;

    /* 心跳保活 */
    uint64_t last_ping_us;
    int      awaiting_pong; /**< 已发送但未应答的 ping 数，连续达到 2 判定断开 */

    /* 跨线程关闭请求：conn_close 限定只能在事件循环线程调用，而 http_ws_close
     * 可能来自任意线程，故这里只置位，由 http_ws_tick 代为执行真正的关闭。 */
    bool close_requested;
};

/** 已升级 WS 的连接登记表。add/remove/遍历三处都只在事件循环线程发生
 *  （http_ws_upgrade 在路由 handler 内同步执行；http_ws_conn_cleanup 由
 *  conn_close 调用；http_ws_tick 由事件循环直接调用），彼此天然不并发，
 *  因此本数组无需加锁。 */
static http_conn_t *s_ws_registry[WS_REGISTRY_MAX];

static void ws_registry_add(http_conn_t *c)
{
    int i;
    for (i = 0; i < WS_REGISTRY_MAX; i++) {
        if (!s_ws_registry[i]) {
            s_ws_registry[i] = c;
            return;
        }
    }
    LOGW(MOD, "WS 心跳注册表已满（上限 %d），该连接将不参与心跳检测", WS_REGISTRY_MAX);
}

static void ws_registry_remove(http_conn_t *c)
{
    int i;
    for (i = 0; i < WS_REGISTRY_MAX; i++) {
        if (s_ws_registry[i] == c) {
            s_ws_registry[i] = NULL;
            return;
        }
    }
}

/* ============================================================================
 * SHA-1（RFC 3174）——firmware 树内没有可复用的哈希原语，握手计算 Accept 值
 * 需要 SHA-1，此处手写一份标准实现，仅供本文件内部使用。
 * ==========================================================================*/

typedef struct {
    uint32_t h[5];
    uint64_t total_len;
    uint8_t  buf[64];
    size_t   buf_len;
} ws_sha1_ctx_t;

static uint32_t ws_rol32(uint32_t v, int bits)
{
    return (v << bits) | (v >> (32 - bits));
}

static void ws_sha1_process_block(ws_sha1_ctx_t *ctx, const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = ws_rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = ctx->h[0];
    b = ctx->h[1];
    c = ctx->h[2];
    d = ctx->h[3];
    e = ctx->h[4];

    for (i = 0; i < 80; i++) {
        uint32_t f, k, tmp;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }

        tmp = ws_rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ws_rol32(b, 30);
        b = a;
        a = tmp;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

static void ws_sha1_init(ws_sha1_ctx_t *ctx)
{
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xC3D2E1F0u;
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

static void ws_sha1_update(ws_sha1_ctx_t *ctx, const uint8_t *data, size_t len)
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
            ws_sha1_process_block(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

static void ws_sha1_final(ws_sha1_ctx_t *ctx, uint8_t out[20])
{
    uint64_t bit_len = ctx->total_len * 8;
    int i;

    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 56) {
        while (ctx->buf_len < 64) ctx->buf[ctx->buf_len++] = 0;
        ws_sha1_process_block(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56) ctx->buf[ctx->buf_len++] = 0;

    for (i = 0; i < 8; i++) {
        ctx->buf[56 + i] = (uint8_t)((bit_len >> ((7 - i) * 8)) & 0xFF);
    }
    ws_sha1_process_block(ctx, ctx->buf);

    for (i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)((ctx->h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = (uint8_t)((ctx->h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = (uint8_t)((ctx->h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i] & 0xFF);
    }
}

static void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    ws_sha1_ctx_t ctx;
    ws_sha1_init(&ctx);
    ws_sha1_update(&ctx, data, len);
    ws_sha1_final(&ctx, out);
}

/* ============================================================================
 * Base64（标准字母表，含 '=' 填充）——同样没有现成原语可复用，仅供本文件内部
 * 把 20 字节 SHA-1 摘要编码进 Sec-WebSocket-Accept。
 * ==========================================================================*/

static const char WS_B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t ws_base64_encode(const uint8_t *data, size_t len, char *out, size_t out_cap)
{
    size_t i, oi = 0;
    size_t need = ((len + 2) / 3) * 4;
    if (need + 1 > out_cap) return 0;

    for (i = 0; i + 3 <= len; i += 3) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[oi++] = WS_B64_TABLE[(v >> 18) & 0x3F];
        out[oi++] = WS_B64_TABLE[(v >> 12) & 0x3F];
        out[oi++] = WS_B64_TABLE[(v >> 6) & 0x3F];
        out[oi++] = WS_B64_TABLE[v & 0x3F];
    }
    if (len - i == 1) {
        uint32_t v = (uint32_t)data[i] << 16;
        out[oi++] = WS_B64_TABLE[(v >> 18) & 0x3F];
        out[oi++] = WS_B64_TABLE[(v >> 12) & 0x3F];
        out[oi++] = '=';
        out[oi++] = '=';
    } else if (len - i == 2) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[oi++] = WS_B64_TABLE[(v >> 18) & 0x3F];
        out[oi++] = WS_B64_TABLE[(v >> 12) & 0x3F];
        out[oi++] = WS_B64_TABLE[(v >> 6) & 0x3F];
        out[oi++] = '=';
    }
    out[oi] = '\0';
    return oi;
}

/* ============================================================================
 * 握手：header 校验辅助 + Sec-WebSocket-Accept 计算
 * ==========================================================================*/

static bool ws_ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/** Connection 头是逗号分隔的 token 列表（如 "keep-alive, Upgrade"），必须按
 *  token 判断是否包含目标值，而不能要求整个字段值恰好等于它。 */
static bool ws_header_has_token_ci(const char *header_value, const char *token)
{
    const char *p = header_value;
    if (!p) return false;
    while (*p) {
        char tok[32];
        size_t n = 0;
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        while (*p && *p != ',') {
            if (n + 1 < sizeof(tok)) tok[n++] = (char)*p;
            p++;
        }
        while (n > 0 && (tok[n - 1] == ' ' || tok[n - 1] == '\t')) n--;
        tok[n] = '\0';
        if (tok[0] != '\0' && ws_ci_eq(tok, token)) return true;
    }
    return false;
}

static hal_err_t ws_compute_accept(const char *client_key, char *out, size_t out_cap)
{
    static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char concat[256];
    int n;
    uint8_t digest[20];

    n = snprintf(concat, sizeof(concat), "%s%s", client_key, WS_GUID);
    if (n < 0 || (size_t)n >= sizeof(concat)) return HAL_EINVAL;

    ws_sha1((const uint8_t *)concat, (size_t)n, digest);

    if (ws_base64_encode(digest, sizeof(digest), out, out_cap) == 0) return HAL_EINVAL;
    return HAL_OK;
}

/* ============================================================================
 * 环形缓冲基础读写 + 帧头编码
 * ==========================================================================*/

static void ws_ring_write(struct http_ws_state *ws, const void *data, size_t len)
{
    size_t tail = (ws->head + ws->used) % ws->cap;
    size_t first = ws->cap - tail;
    if (first > len) first = len;
    memcpy(ws->buf + tail, data, first);
    if (len > first) memcpy(ws->buf, (const uint8_t *)data + first, len - first);
    ws->used += len;
}

/** 编码服务端下行帧头（FIN=1，不分片；不设置 MASK 位——服务端下行帧禁止掩码）。
 *  out 至少需要 10 字节空间（1 字节 FIN+opcode + 最多 9 字节长度域）。 */
static size_t ws_encode_header(uint8_t out[10], int opcode, size_t payload_len)
{
    size_t n = 0;
    out[n++] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (payload_len < 126) {
        out[n++] = (uint8_t)payload_len;
    } else if (payload_len <= 0xFFFF) {
        out[n++] = 126;
        out[n++] = (uint8_t)((payload_len >> 8) & 0xFF);
        out[n++] = (uint8_t)(payload_len & 0xFF);
    } else {
        int i;
        out[n++] = 127;
        for (i = 7; i >= 0; i--) out[n++] = (uint8_t)((payload_len >> (i * 8)) & 0xFF);
    }
    return n;
}

/* ============================================================================
 * WS 状态的创建/销毁
 * ==========================================================================*/

static struct http_ws_state *ws_state_create(size_t queue_cap)
{
    struct http_ws_state *ws;

    if (queue_cap == 0) return NULL;
    ws = (struct http_ws_state *)calloc(1, sizeof(*ws));
    if (!ws) return NULL;
    ws->buf = (uint8_t *)malloc(queue_cap);
    if (!ws->buf) {
        free(ws);
        return NULL;
    }
    ws->cap = queue_cap;
    ws->mu = os_mutex_create();
    if (!ws->mu) {
        free(ws->buf);
        free(ws);
        return NULL;
    }
    return ws;
}

static void ws_state_destroy(struct http_ws_state *ws)
{
    if (!ws) return;
    os_mutex_destroy(ws->mu);
    free(ws->buf);
    free(ws);
}

/* ============================================================================
 * 发送：控制帧尽力而为写入 + 业务帧背压状态机
 * ==========================================================================*/

/** ping/pong/close 回复等控制帧：尽力而为直接入队，不经过 dropping 状态机
 *  （那是给视频等业务帧用的背压逻辑）。控制帧 payload 很小（<=125 字节），
 *  正常情况下不会实质性挤占业务数据的队列空间；真放不下就放弃，连接是否
 *  健康自有心跳超时/send 硬错误兜底判断。 */
static void ws_send_control_frame(http_conn_t *c, int opcode, const uint8_t *payload, size_t len)
{
    struct http_ws_state *ws = c->ws;
    uint8_t hdr[10];
    size_t hdr_len = ws_encode_header(hdr, opcode, len);
    bool ok;

    os_mutex_lock(ws->mu);
    ok = (ws->cap - ws->used >= hdr_len + len);
    if (ok) {
        ws_ring_write(ws, hdr, hdr_len);
        if (len) ws_ring_write(ws, payload, len);
    }
    os_mutex_unlock(ws->mu);

    if (ok) conn_update_poll_interest(c);
}

/** 业务帧（文本/二进制）入队：先判断当前空间是否足够放下这一帧——不够就
 *  无条件丢弃并计数（不管是不是关键帧，物理上放不下就是放不下）；空间足够
 *  时才看背压状态：正在丢弃且不是关键帧→继续丢；正在丢弃且是关键帧→
 *  从这一帧起恢复正常；未在丢弃→直接入队。 */
static hal_err_t ws_enqueue_frame(http_conn_t *c, int opcode, const void *payload, size_t len, bool is_key)
{
    struct http_ws_state *ws = c->ws;
    uint8_t hdr[10];
    size_t hdr_len = ws_encode_header(hdr, opcode, len);
    size_t needed = hdr_len + len;
    bool do_enqueue = false;

    os_mutex_lock(ws->mu);

    if (needed > ws->cap - ws->used) {
        ws->dropping = true;
        ws->dropped++;
    } else if (ws->dropping) {
        if (is_key) {
            ws->dropping = false;
            do_enqueue = true;
        } else {
            ws->dropped++;
        }
    } else {
        do_enqueue = true;
    }

    if (do_enqueue) {
        ws_ring_write(ws, hdr, hdr_len);
        if (len) ws_ring_write(ws, (const uint8_t *)payload, len);
    }

    os_mutex_unlock(ws->mu);

    if (!do_enqueue) return HAL_EAGAIN;
    conn_update_poll_interest(c);
    return HAL_OK;
}

/* ============================================================================
 * 接收：从 c->rbuf 里解析出一帧并按 opcode 分派
 * ==========================================================================*/

/** 尝试从 buf[0..len) 消费一个完整帧。返回消费掉的字节数；返回 0 表示数据
 *  不完整（需要等待更多字节到达），或者本次调用内已经因协议违规/超限
 *  conn_close 关闭了连接（调用方需要在返回后检查 c->used）。 */
static size_t ws_try_consume_frame(http_conn_t *c, const uint8_t *buf, size_t len)
{
    uint64_t raw_len;
    size_t hdr_len, payload_len, mask_off, total;
    int fin, opcode, masked;
    uint8_t mask_key[4];
    const uint8_t *payload;

    if (len < 2) return 0;

    fin = (buf[0] & 0x80) != 0;
    opcode = buf[0] & 0x0F;
    masked = (buf[1] & 0x80) != 0;
    raw_len = buf[1] & 0x7Fu;
    hdr_len = 2;

    if (raw_len == 126) {
        if (len < 4) return 0;
        raw_len = ((uint64_t)buf[2] << 8) | buf[3];
        hdr_len = 4;
    } else if (raw_len == 127) {
        size_t i;
        if (len < 10) return 0;
        raw_len = 0;
        for (i = 0; i < 8; i++) raw_len = (raw_len << 8) | buf[2 + i];
        hdr_len = 10;
    }

    /* 用 uint64_t 暂存原始长度字段、与上限比较后才收窄为 size_t——避免 32 位
     * 目标平台上 size_t 只有 32 位时，畸形/恶意帧声明的超大长度在收窄过程中
     * 截断绕过后面的上限检查（这类平台是本项目的实际目标之一，不能假设
     * size_t 总是 64 位）。 */
    if ((opcode == 0x8 || opcode == 0x9 || opcode == 0xA) && raw_len > 125) {
        LOGW(MOD, "fd=%d WS 控制帧声明 payload 超过协议上限 125 字节，关闭连接", (int)c->fd);
        conn_close(c);
        return 0;
    }
    if (raw_len > WS_MAX_FRAME_PAYLOAD) {
        LOGW(MOD, "fd=%d WS 帧声明 payload 超过本实现上限 %d 字节，关闭连接",
             (int)c->fd, WS_MAX_FRAME_PAYLOAD);
        conn_close(c);
        return 0;
    }
    payload_len = (size_t)raw_len;

    mask_off = hdr_len;
    if (masked) hdr_len += 4;

    total = hdr_len + payload_len;
    if (len < total) return 0; /* 数据不完整，等待下次可读事件继续收 */

    if (masked) memcpy(mask_key, buf + mask_off, 4);
    payload = buf + hdr_len;

    if (!fin) {
        /* 不支持分片消息（continuation frame）：正确跳过字节数，不破坏后续
         * 帧的边界对齐即可，不做任何语义处理。 */
        return total;
    }

    switch (opcode) {
    case 0x1: { /* 文本帧：解掩码后回调上行处理器（回调在锁外调用，避免重入死锁） */
        http_ws_text_fn fn;
        void *user;
        os_mutex_lock(c->ws->mu);
        fn = c->ws->text_fn;
        user = c->ws->text_user;
        os_mutex_unlock(c->ws->mu);
        if (fn) {
            static char text_buf[4096];
            size_t n = payload_len < sizeof(text_buf) - 1 ? payload_len : sizeof(text_buf) - 1;
            size_t i;
            for (i = 0; i < n; i++) {
                text_buf[i] = (char)(masked ? (payload[i] ^ mask_key[i & 3]) : payload[i]);
            }
            text_buf[n] = '\0';
            fn(c, text_buf, n, user);
        }
        break;
    }

    case 0x2: /* 二进制帧：本模块未定义客户端上行二进制语义，正确跳过即可 */
        break;

    case 0x8: { /* close：入队一个空 close 帧并立即尝试发出，然后关闭连接。
                 * 走与所有下行数据相同的环形缓冲+http_ws_flush 路径（而不是
                 * 在这里直接 send()），保持"实际 send() 只发生在 http_ws_flush"
                 * 这一条唯一出口，避免与队列里可能尚未发完的其他数据错序。 */
        ws_send_control_frame(c, 0x8, NULL, 0);
        http_ws_flush(c); /* 尽力让 close 回复尽快发出；发不完也无妨，即将关闭连接 */
        if (c->used) conn_close(c); /* 上面若因 send 硬错误已经关闭，这里不再重复关闭 */
        return total;
    }

    case 0x9: { /* ping：解掩码后原样回显为 pong（控制帧 payload<=125，已在上面校验过） */
        uint8_t unmasked[125];
        size_t i;
        for (i = 0; i < payload_len; i++) {
            unmasked[i] = (uint8_t)(masked ? (payload[i] ^ mask_key[i & 3]) : payload[i]);
        }
        ws_send_control_frame(c, 0xA, unmasked, payload_len);
        break;
    }

    case 0xA: /* pong：清零心跳失联计数 */
        os_mutex_lock(c->ws->mu);
        c->ws->awaiting_pong = 0;
        os_mutex_unlock(c->ws->mu);
        break;

    default:
        /* 未知/保留 opcode：忽略 payload，仅正确跳过字节数 */
        break;
    }

    return total;
}

/* ============================================================================
 * 公开 API 实现
 * ==========================================================================*/

hal_err_t http_ws_upgrade(http_req_t *req, size_t queue_cap)
{
    http_conn_t *c;
    const char *upg, *ver, *key, *conn_hdr;
    char accept_key[64];
    char extra[256];
    struct http_ws_state *ws;
    hal_err_t rc;

    if (!req || !req->conn || queue_cap == 0) return HAL_EINVAL;
    c = req->conn;
    if (c->is_ws) return HAL_ESTATE;

    upg = http_header(req, "Upgrade");
    conn_hdr = http_header(req, "Connection");
    ver = http_header(req, "Sec-WebSocket-Version");
    key = http_header(req, "Sec-WebSocket-Key");

    if (!upg || !ws_ci_eq(upg, "websocket")) return HAL_EINVAL;
    if (!conn_hdr || !ws_header_has_token_ci(conn_hdr, "upgrade")) return HAL_EINVAL;
    if (!ver || strcmp(ver, "13") != 0) return HAL_EINVAL;
    if (!key || !*key) return HAL_EINVAL;

    if (ws_compute_accept(key, accept_key, sizeof(accept_key)) != HAL_OK) return HAL_EINVAL;

    ws = ws_state_create(queue_cap);
    if (!ws) return HAL_ENOMEM;

    snprintf(extra, sizeof(extra),
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n",
             accept_key);

    /* 复用现成的 http_respond_ex 回 101（brief Step 4 明确要求）。http_respond_ex
     * 对 1xx 状态码走精简模板：只发状态行 + extra_headers + 空行，不会像
     * 2xx/4xx/5xx 通用路径那样自行追加 Content-Type/Content-Length/
     * Connection 三行（Content-Length 出现在 1xx 响应上是 RFC 7230 §3.3.2
     * 的 MUST NOT）——这里 extra 里的 Connection: Upgrade 就是响应里唯一的
     * Connection 头，不会重复。 */
    rc = http_respond_ex(c, 101, NULL, extra, NULL, 0);
    if (rc != HAL_OK) {
        ws_state_destroy(ws);
        return rc;
    }

    c->is_ws = true;
    c->ws = ws;
    ws->last_ping_us = os_monotonic_us();
    ws_registry_add(c);

    return HAL_OK;
}

hal_err_t http_ws_send(http_conn_t *c, const void *data, size_t len, bool is_key)
{
    if (!c || !c->ws || (!data && len)) return HAL_EINVAL;
    return ws_enqueue_frame(c, 0x2, data, len, is_key);
}

hal_err_t http_ws_send_text(http_conn_t *c, const char *text)
{
    if (!c || !c->ws || !text) return HAL_EINVAL;
    /* 文本消息（状态推送）按"关键帧"对待：只要队列物理空间足够就必定入队，
     * 不受视频帧背压状态机的丢弃状态拖累；真放不下时与视频帧共用同一条
     * "空间不足"判定，同样计入 dropped（见 ws_enqueue_frame）。 */
    return ws_enqueue_frame(c, 0x1, text, strlen(text), true);
}

hal_err_t http_ws_on_text(http_conn_t *c, http_ws_text_fn fn, void *user)
{
    if (!c || !c->ws) return HAL_EINVAL;
    os_mutex_lock(c->ws->mu);
    c->ws->text_fn = fn;
    c->ws->text_user = user;
    os_mutex_unlock(c->ws->mu);
    return HAL_OK;
}

hal_err_t http_ws_close(http_conn_t *c)
{
    if (!c || !c->ws) return HAL_EINVAL;
    /* 只置位，真正的 conn_close 留给 http_ws_tick 在事件循环线程里代为执行
     * ——本函数须可从任意线程调用，但 conn_close 不是线程安全的。 */
    os_mutex_lock(c->ws->mu);
    c->ws->close_requested = true;
    os_mutex_unlock(c->ws->mu);
    return HAL_OK;
}

size_t http_ws_queue_used(http_conn_t *c)
{
    size_t used;
    if (!c || !c->ws) return 0;
    os_mutex_lock(c->ws->mu);
    used = c->ws->used;
    os_mutex_unlock(c->ws->mu);
    return used;
}

uint64_t http_ws_dropped(http_conn_t *c)
{
    uint64_t d;
    if (!c || !c->ws) return 0;
    os_mutex_lock(c->ws->mu);
    d = c->ws->dropped;
    os_mutex_unlock(c->ws->mu);
    return d;
}

/* ============================================================================
 * http_server_internal.h 声明、供 http_server.c 调用的内部函数
 * ==========================================================================*/

void http_ws_on_readable(http_conn_t *c)
{
    for (;;) {
        size_t consumed = ws_try_consume_frame(c, (const uint8_t *)c->rbuf, c->rlen);
        if (consumed == 0) break;
        if (!c->used) return; /* 帧处理过程中连接已被关闭（如收到 close 帧） */
        memmove(c->rbuf, c->rbuf + consumed, c->rlen - consumed);
        c->rlen -= consumed;
    }
}

void http_ws_flush(http_conn_t *c)
{
    struct http_ws_state *ws = c->ws;

    for (;;) {
        size_t contig;
        int n;

        os_mutex_lock(ws->mu);
        if (ws->used == 0) {
            os_mutex_unlock(ws->mu);
            break;
        }
        contig = ws->cap - ws->head;
        if (contig > ws->used) contig = ws->used;
        n = (int)send(c->fd, (const char *)(ws->buf + ws->head), (int)contig, 0);
        if (n > 0) {
            ws->head = (ws->head + (size_t)n) % ws->cap;
            ws->used -= (size_t)n;
            os_mutex_unlock(ws->mu);
            continue;
        }
        os_mutex_unlock(ws->mu);
        if (n < 0 && WOULD_BLOCK()) return; /* 保留现有 poll 兴趣，等下次可写事件 */
        LOGW(MOD, "fd=%d WS send 失败，关闭连接", (int)c->fd);
        conn_close(c);
        return;
    }
    conn_update_poll_interest(c); /* 只在"这一轮把队列彻底发空"时调用一次 */
}

void http_ws_conn_cleanup(http_conn_t *c)
{
    if (!c->ws) return;
    ws_registry_remove(c);
    ws_state_destroy(c->ws);
    c->ws = NULL;
}

void http_ws_tick(void)
{
    int i;
    uint64_t now = os_monotonic_us();

    for (i = 0; i < WS_REGISTRY_MAX; i++) {
        http_conn_t *c = s_ws_registry[i];
        struct http_ws_state *ws;
        bool need_close = false;
        bool need_ping = false;

        if (!c) continue;
        ws = c->ws;

        os_mutex_lock(ws->mu);
        if (ws->close_requested) {
            need_close = true;
        } else if (now - ws->last_ping_us >= WS_PING_INTERVAL_US) {
            if (ws->awaiting_pong >= 2) {
                need_close = true;
            } else {
                need_ping = true;
                ws->awaiting_pong++;
                ws->last_ping_us = now;
            }
        }
        os_mutex_unlock(ws->mu);

        if (need_close) {
            LOGI(MOD, "fd=%d WS 连接关闭（主动请求或心跳超时）", (int)c->fd);
            conn_close(c); /* 内部会触发 http_ws_conn_cleanup -> ws_registry_remove(c) */
            continue;
        }
        if (need_ping) ws_send_control_frame(c, 0x9, NULL, 0);
    }
}

/* ============================================================================
 * IPC_TESTING：独立于真实 socket/epoll/心跳注册表之外的测试桩
 * ==========================================================================*/
#ifdef IPC_TESTING

http_conn_t *http_ws_test_conn_new(size_t queue_cap)
{
    http_conn_t *c = (http_conn_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = SOCK_INVALID;
    c->used = 1;
    c->ws = ws_state_create(queue_cap);
    if (!c->ws) {
        free(c);
        return NULL;
    }
    c->is_ws = true;
    /* 刻意不调用 ws_registry_add：测试桩不接入真实 socket/epoll，只用来独立
     * 验证 http_ws_send 的背压状态机，不应参与心跳 tick 或走 conn_close 路径。 */
    return c;
}

void http_ws_test_conn_free(http_conn_t *c)
{
    if (!c) return;
    ws_state_destroy(c->ws);
    free(c);
}

void http_ws_test_drain(http_conn_t *c)
{
    if (!c || !c->ws) return;
    os_mutex_lock(c->ws->mu);
    c->ws->head = 0;
    c->ws->used = 0; /* 模拟数据已全部发出：只清记账，不改 dropping/dropped */
    os_mutex_unlock(c->ws->mu);
}

#endif /* IPC_TESTING */
