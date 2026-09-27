/**
 * @file main.c
 * @brief console 模块单元测试（x86 + mock 平台）
 *
 * 覆盖：错误码→HTTP 状态映射、鉴权全流程、配置读写、能力探测降级。
 * 退出码 = 失败数。
 */
#include "modules/console/console_internal.h"
#include "modules/console/console_assets.h"
#include "core/config.h"
#include "core/profile.h"
#include "core/json.h"
#include "core/event_bus.h"
#include "core/os.h"
#include "hal/hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SECTION(name) printf("== %s\n", name)

static void test_err_mapping(void)
{
    SECTION("错误码映射");
    CHECK(console_http_status(HAL_OK)       == 200, "OK→200");
    CHECK(console_http_status(HAL_EINVAL)   == 400, "EINVAL→400");
    CHECK(console_http_status(HAL_EPERM_)   == 403, "EPERM→403");
    CHECK(console_http_status(HAL_ECSRF_)   == 403, "ECSRF→403（评审 M-4：CSRF 拒绝专属错误码）");
    CHECK(console_http_status(HAL_ENODEV)   == 404, "ENODEV→404");
    CHECK(console_http_status(HAL_EBUSY)    == 409, "EBUSY→409");
    CHECK(console_http_status(HAL_ESTATE)   == 409, "ESTATE→409（已激活设备拒绝重复激活）");
    /* 能力探测基石：不可退化为 404 */
    CHECK(console_http_status(HAL_ENOTSUP)  == 501, "ENOTSUP→501（能力探测基石）");
    CHECK(console_http_status(HAL_EIO)      == 500, "EIO→500");
    CHECK(console_http_status(HAL_ETIMEOUT) == 500, "ETIMEOUT→500");
}

/* PBKDF2-SHA256 已知答案测试（RFC 6070 风格，用 SHA-256 变体） */
static void test_pbkdf2_kat(void)
{
    uint8_t out[32];
    /* P="password", S="salt", c=1 的 PBKDF2-HMAC-SHA256 前 8 字节 */
    static const uint8_t expect[8] = { 0x12,0x0f,0xb6,0xcf,0xfc,0xf8,0xb3,0x2c };
    /* c=4096 完整 32 字节。c=1 的向量**完全跳过迭代循环体**，而生产用的正是 4096 轮；
       且客户端与服务端调的是同一份实现，端到端测试只能证明自洽、不能证明正确
       （第一次真正的独立校验要等 Task 12 的 Web Crypto）。这条向量提前关掉这个缺口。 */
    static const uint8_t expect4096[32] = {
        0xc5,0xe4,0x78,0xd5,0x92,0x88,0xc8,0x41, 0xaa,0x53,0x0d,0xb6,0x84,0x5c,0x4c,0x8d,
        0x96,0x28,0x93,0xa0,0x01,0xce,0x4e,0x11, 0xa4,0x96,0x38,0x73,0xaa,0x98,0x13,0x4a
    };
    /* RFC 4231 Test Case 1：key = 20 字节 0x0b，data = "Hi There" */
    static const uint8_t hmac_key[20] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b
    };
    static const uint8_t hmac_expect[32] = {
        0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53, 0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
        0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7, 0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7
    };
    uint8_t mac[32];

    SECTION("PBKDF2 已知答案");
    CHECK(console_pbkdf2_sha256("password", 8, (const uint8_t*)"salt", 4, 1, out, 32) == HAL_OK,
          "pbkdf2 返回 OK");
    CHECK(memcmp(out, expect, 8) == 0, "PBKDF2(password,salt,1) 前 8 字节匹配 RFC 向量");

    CHECK(console_pbkdf2_sha256("password", 8, (const uint8_t*)"salt", 4, 4096, out, 32) == HAL_OK,
          "pbkdf2 c=4096 返回 OK");
    CHECK(memcmp(out, expect4096, 32) == 0,
          "PBKDF2(password,salt,4096) 全 32 字节匹配已知答案（覆盖迭代循环体）");

    CHECK(console_hmac_sha256(hmac_key, sizeof(hmac_key),
                              (const uint8_t *)"Hi There", 8, mac) == HAL_OK, "hmac 返回 OK");
    CHECK(memcmp(mac, hmac_expect, 32) == 0, "HMAC-SHA256 匹配 RFC 4231 Test Case 1");
}

/** 浏览器端 firmware/web/js/crypto.js 的 selfTest 使用同一向量：两端 proof 必须逐字相同 */
static void test_proof_cross_vector(void)
{
    char proof[65];
    SECTION("proof 跨端向量（与前端 crypto.js 对齐）");
    CHECK(console_auth_make_proof("Admin@12345", "00112233445566778899aabbccddeeff",
                                  "0123456789abcdef0123456789abcdef", proof, sizeof(proof)) == HAL_OK, "计算 proof");
    CHECK(strcmp(proof, "5cdf3e3b1d6c8001595b3bb4aad6a4107b369bf451596c077e79a3aeafb9abb8") == 0,
          "proof 与前端向量一致，实际：%s", proof);
    CHECK(console_auth_make_proof("密码Test123", "00112233445566778899aabbccddeeff",
                                  "0123456789abcdef0123456789abcdef", proof, sizeof(proof)) == HAL_OK, "中文口令");
    CHECK(strcmp(proof, "68edff6c2057e0079074af17aa2f09b1d44fb3492813580dd432fe8b93d77275") == 0,
          "中文口令按 UTF-8 计算，实际：%s", proof);
}

static void test_auth_flow(void)
{
    char salt_hex[64], nonce[64], proof[128];
    hal_err_t e;

    SECTION("鉴权流程");
    /* 首次：以出厂验证码派生凭据 */
    CHECK(console_auth_seed("ABCD1234", NULL, true) == HAL_OK, "以出厂验证码播种");

    /* challenge 返回稳定的 salt 与一次性 nonce */
    CHECK(console_auth_challenge("admin", salt_hex, sizeof(salt_hex), nonce, sizeof(nonce)) == HAL_OK,
          "取 challenge");
    CHECK(strlen(nonce) >= 16, "nonce 长度足够");

    /* 正确口令应通过 */
    CHECK(console_auth_make_proof("ABCD1234", salt_hex, nonce, proof, sizeof(proof)) == HAL_OK,
          "客户端侧计算 proof");
    CHECK(console_auth_verify("admin", nonce, proof) == HAL_OK, "正确 proof 验证通过");

    /* 同一 nonce 不可重放 */
    e = console_auth_verify("admin", nonce, proof);
    CHECK(e != HAL_OK, "nonce 重放必须被拒绝（返回 %d）", (int)e);

    /* 错误口令 */
    CHECK(console_auth_challenge("admin", salt_hex, sizeof(salt_hex), nonce, sizeof(nonce)) == HAL_OK,
          "重新取 challenge");
    CHECK(console_auth_make_proof("WRONGPWD", salt_hex, nonce, proof, sizeof(proof)) == HAL_OK,
          "以错误口令算 proof");
    CHECK(console_auth_verify("admin", nonce, proof) == HAL_EPERM_, "错误口令被拒");
}

static void test_auth_lockout(void)
{
    char salt_hex[64], nonce[64], proof[128];
    int i;
    hal_err_t last = HAL_OK;

    SECTION("暴力破解锁定");
    console_auth_reset_lockout();   /* 测试桩：清空计数 */

    /* 连续 5 次失败后应锁定 */
    for (i = 0; i < 6; i++) {
        console_auth_challenge("admin", salt_hex, sizeof(salt_hex), nonce, sizeof(nonce));
        console_auth_make_proof("BADPASSWORD", salt_hex, nonce, proof, sizeof(proof));
        last = console_auth_verify_from("admin", nonce, proof, "192.168.1.50");
    }
    CHECK(last == HAL_EBUSY, "连续失败后锁定该 IP（返回 %d）", (int)last);

    /* 另一 IP 不受影响 */
    console_auth_challenge("admin", salt_hex, sizeof(salt_hex), nonce, sizeof(nonce));
    console_auth_make_proof("ABCD1234", salt_hex, nonce, proof, sizeof(proof));
    CHECK(console_auth_verify_from("admin", nonce, proof, "192.168.1.51") == HAL_OK,
          "锁定按 IP 隔离，其他 IP 正常");
}

static void test_auth_user_enum(void)
{
    char s1[64], n1[64], s2[64], n2[64], s3[64], n3[64];

    SECTION("防用户名枚举");
    CHECK(console_auth_challenge("nosuchuser", s1, sizeof(s1), n1, sizeof(n1)) == HAL_OK,
          "不存在的用户也返回 challenge");
    CHECK(console_auth_challenge("nosuchuser", s2, sizeof(s2), n2, sizeof(n2)) == HAL_OK, "再取一次");
    CHECK(strcmp(s1, s2) == 0, "同一不存在用户的 salt 必须稳定（否则可据此枚举）");
    CHECK(console_auth_challenge("otheruser", s3, sizeof(s3), n3, sizeof(n3)) == HAL_OK, "另一用户");
    CHECK(strcmp(s1, s3) != 0, "不同用户名派生不同 salt");
}

static void test_must_change_password(void)
{
    SECTION("首次强制改密");
    console_auth_seed("ABCD1234", NULL, true);
    CHECK(console_auth_must_change() == true, "出厂状态需强制改密");
    CHECK(console_auth_set_password("ABCD1234", "NewPass@123") == HAL_OK, "改密成功");
    CHECK(console_auth_must_change() == false, "改密后解除");
    /* 旧口令失效 */
    {
        char salt_hex[64], nonce[64], proof[128];
        console_auth_challenge("admin", salt_hex, sizeof(salt_hex), nonce, sizeof(nonce));
        console_auth_make_proof("ABCD1234", salt_hex, nonce, proof, sizeof(proof));
        CHECK(console_auth_verify("admin", nonce, proof) == HAL_EPERM_, "旧口令改密后失效");
    }
}

/* ========================================================================== */
/* 以下为端点层与会话门禁用例（brief 的五个用例只覆盖鉴权原语，不覆盖 HTTP 面）    */
/* ========================================================================== */

/** 组装一个不含真实连接的请求（conn 为 NULL，来源 IP 因此按“未知”计入锁定桶） */
static void req_make(http_req_t *req, const char *method, const char *path,
                     const char *body, const char *cookie)
{
    memset(req, 0, sizeof(*req));
    snprintf(req->method, sizeof(req->method), "%s", method);
    snprintf(req->path, sizeof(req->path), "%s", path);
    req->body = body;
    req->body_len = body ? strlen(body) : 0;
    if (cookie) {
        req->headers[0].name = "Cookie";
        req->headers[0].value = cookie;
        req->header_count = 1;
    }
}

/** 追加一个请求头（评审 Ruling 18 新增：/auth/activate 起要求 Content-Type，
 *  部分用例还需要 Origin/Host 来验证同源判定）。req_make 只内置 Cookie 这一个
 *  槽位，本函数在其后继续往 headers[] 追加，不覆盖已有的。调用方必须保证
 *  name/value 指向的内存活得比 req 使用期长（测试里一律传字符串字面量，
 *  满足这一点）。header_count 撞上 HTTP_HEADERS_MAX 时静默丢弃——测试用例
 *  的头部数远小于上限，不会触发。 */
static void req_add_header(http_req_t *req, const char *name, const char *value)
{
    if (req->header_count >= (sizeof(req->headers) / sizeof(req->headers[0]))) return;
    req->headers[req->header_count].name = name;
    req->headers[req->header_count].value = value;
    req->header_count++;
}

/** 从响应体里取一个字符串字段；不存在或超长返回 false */
static bool body_field(const char *body, const char *key, char *out, size_t cap)
{
    json_t *j = json_parse(body, 0, NULL, 0);
    const char *v;
    bool ok = false;
    if (!j) return false;
    v = json_string(json_get(j, key), NULL);
    if (v && strlen(v) < cap) { memcpy(out, v, strlen(v) + 1); ok = true; }
    json_free(j);
    return ok;
}

static bool body_flag(const char *body, const char *key, bool def)
{
    json_t *j = json_parse(body, 0, NULL, 0);
    bool v;
    if (!j) return def;
    v = json_bool(json_get(j, key), def);
    json_free(j);
    return v;
}

static int64_t body_int(const char *body, const char *key, int64_t def)
{
    json_t *j = json_parse(body, 0, NULL, 0);
    int64_t v;
    if (!j) return def;
    v = json_int(json_get(j, key), def);
    json_free(j);
    return v;
}

/* ---- 客户端侧十六进制工具：测试独立实现，不复用被测代码的内部函数 ---- */

static void t_hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) { out[i * 2] = H[in[i] >> 4]; out[i * 2 + 1] = H[in[i] & 0x0F]; }
    out[n * 2] = '\0';
}

static bool t_hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *len)
{
    size_t n = strlen(hex), i;
    if (n == 0 || (n & 1) || n / 2 > cap) return false;
    for (i = 0; i < n; i += 2) {
        char c1 = hex[i], c2 = hex[i + 1];
        int hi = (c1 >= '0' && c1 <= '9') ? c1 - '0' : (c1 >= 'a' && c1 <= 'f') ? c1 - 'a' + 10 : -1;
        int lo = (c2 >= '0' && c2 <= '9') ? c2 - '0' : (c2 >= 'a' && c2 <= 'f') ? c2 - 'a' + 10 : -1;
        if (hi < 0 || lo < 0) return false;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *len = n / 2;
    return true;
}

/** 把 Set-Cookie 值（"token=xxx; HttpOnly; ..."）截成可回传的 Cookie 头 */
static void cookie_from_set_cookie(const char *set_cookie, char *out, size_t cap)
{
    const char *semi = strchr(set_cookie, ';');
    size_t n = semi ? (size_t)(semi - set_cookie) : strlen(set_cookie);
    if (n >= cap) n = cap - 1;
    memcpy(out, set_cookie, n);
    out[n] = 0;
}

/** 取一次 challenge，回填 salt/nonce/iter（登录与改密都要先走这一步） */
static hal_err_t fetch_challenge(const char *user, const char *ip, char *salt_hex, size_t sc,
                                 char *nonce, size_t nc, unsigned *iter)
{
    char body[512], set_cookie[256], req_body[256];
    http_req_t req;
    hal_err_t rc;

    snprintf(req_body, sizeof(req_body), "{\"user\":\"%s\"}", user);
    req_make(&req, "POST", "/api/v1/auth/challenge", req_body, NULL);
    rc = console_auth_test_dispatch(&req, ip, body, sizeof(body), set_cookie, sizeof(set_cookie));
    if (rc != HAL_OK) return rc;
    if (!body_field(body, "salt", salt_hex, sc)) return HAL_ECORRUPT;
    if (!body_field(body, "nonce", nonce, nc)) return HAL_ECORRUPT;
    if (iter) *iter = (unsigned)body_int(body, "iter", 0);
    return HAL_OK;
}

/** 走一遍 challenge→login，成功时把可回传的 Cookie 写入 cookie_out */
static hal_err_t do_login(const char *user, const char *pwd, const char *ip,
                          char *cookie_out, size_t cap, bool *must_change_out)
{
    char body[512], set_cookie[256], req_body[512];
    char salt_hex[80], nonce[80], proof[160];
    http_req_t req;
    hal_err_t rc;

    rc = fetch_challenge(user, ip, salt_hex, sizeof(salt_hex), nonce, sizeof(nonce), NULL);
    if (rc != HAL_OK) return rc;

    rc = console_auth_make_proof(pwd, salt_hex, nonce, proof, sizeof(proof));
    if (rc != HAL_OK) return rc;

    snprintf(req_body, sizeof(req_body), "{\"user\":\"%s\",\"nonce\":\"%s\",\"proof\":\"%s\"}",
             user, nonce, proof);
    req_make(&req, "POST", "/api/v1/auth/login", req_body, NULL);
    rc = console_auth_test_dispatch(&req, ip, body, sizeof(body), set_cookie, sizeof(set_cookie));
    if (rc != HAL_OK) return rc;
    if (must_change_out) *must_change_out = body_flag(body, "must_change_password", true);
    if (strstr(set_cookie, "HttpOnly") == NULL || strstr(set_cookie, "SameSite=Strict") == NULL)
        return HAL_ECORRUPT;
    cookie_from_set_cookie(set_cookie, cookie_out, cap);
    return HAL_OK;
}

/**
 * 完全按 Task 12 将在浏览器里做的那套，组装一个掩码改密请求体。
 * 刻意只用公开原语（PBKDF2/HMAC）自行推导，不碰被测代码的内部函数——
 * 这样测的是契约本身，而不是实现与自己对账。
 */
static hal_err_t build_password_body(const char *user, const char *old_pwd, const char *new_pwd,
                                     const char *ip, const uint8_t new_salt[16], bool corrupt_proof,
                                     char *out, size_t cap, char *new_salt_hex_out)
{
    char salt_hex[80], nonce[80], proof[160], key_hex[80], chk_hex[80], msg[80];
    uint8_t old_salt[32], old_key[32], mask[32], new_key[32], buf[32];
    size_t old_salt_len = 0, i;
    unsigned iter = 0;
    hal_err_t rc;

    if ((rc = fetch_challenge(user, ip, salt_hex, sizeof(salt_hex),
                              nonce, sizeof(nonce), &iter)) != HAL_OK) return rc;
    if (iter == 0) return HAL_ECORRUPT;   /* challenge 必须下发 iter，否则前端只能硬编码 */

    /* proof：与登录同构，证明持有旧口令 */
    if ((rc = console_auth_make_proof(old_pwd, salt_hex, nonce, proof, sizeof(proof))) != HAL_OK)
        return rc;
    if (corrupt_proof) proof[0] = (proof[0] == 'a') ? 'b' : 'a';

    if (!t_hex_decode(salt_hex, old_salt, sizeof(old_salt), &old_salt_len)) return HAL_ECORRUPT;
    if ((rc = console_pbkdf2_sha256(old_pwd, strlen(old_pwd), old_salt, old_salt_len,
                                    iter, old_key, sizeof(old_key))) != HAL_OK) return rc;

    /* new_key = PBKDF2(新口令, 客户端自选的 new_salt, iter)，
       用 mask = HMAC(old_key, "pwdchg|" || nonce) 遮蔽后上送 */
    if ((rc = console_pbkdf2_sha256(new_pwd, strlen(new_pwd), new_salt, 16,
                                    iter, new_key, sizeof(new_key))) != HAL_OK) return rc;
    snprintf(msg, sizeof(msg), "pwdchg|%s", nonce);
    if ((rc = console_hmac_sha256(old_key, sizeof(old_key),
                                  (const uint8_t *)msg, strlen(msg), mask)) != HAL_OK) return rc;
    for (i = 0; i < sizeof(new_key); i++) buf[i] = (uint8_t)(new_key[i] ^ mask[i]);
    t_hex_encode(buf, sizeof(buf), key_hex);

    /* chk = PBKDF2(新口令, **旧盐**, iter)：新旧口令相同时它必然等于 stored_key，
       服务端据此拒绝。掩码必须是**第三条**、与 proof 和 pwdchg 都域分隔的
       HMAC(old_key, "pwdchk|" || nonce)——同一 nonce 下两个明文共用一条掩码，
       观察者把两段密文异或就直接拿到两份明文的异或。 */
    if ((rc = console_pbkdf2_sha256(new_pwd, strlen(new_pwd), old_salt, old_salt_len,
                                    iter, new_key, sizeof(new_key))) != HAL_OK) return rc;
    snprintf(msg, sizeof(msg), "pwdchk|%s", nonce);
    if ((rc = console_hmac_sha256(old_key, sizeof(old_key),
                                  (const uint8_t *)msg, strlen(msg), mask)) != HAL_OK) return rc;
    for (i = 0; i < sizeof(new_key); i++) buf[i] = (uint8_t)(new_key[i] ^ mask[i]);
    t_hex_encode(buf, sizeof(buf), chk_hex);

    t_hex_encode(new_salt, 16, new_salt_hex_out);
    if (snprintf(out, cap,
                 "{\"user\":\"%s\",\"nonce\":\"%s\",\"proof\":\"%s\","
                 "\"new_salt\":\"%s\",\"new_key_masked\":\"%s\",\"chk_masked\":\"%s\"}",
                 user, nonce, proof, new_salt_hex_out, key_hex, chk_hex) >= (int)cap)
        return HAL_ENOMEM;
    return HAL_OK;
}

static void test_auth_endpoints(void)
{
    /* 每个用例用不同来源 IP：否则全部落进同一个锁定桶，再多几条负例就会静默开始
       返回 HAL_EBUSY 而不是期望的错误码。 */
    static const char *IP_OPS = "192.168.9.10";
    char body[512], set_cookie[256], cookie[128], req_body[640];
    char salt_hex[80], nonce[80], proof[160];
    http_req_t req;
    bool must_change = false;

    SECTION("鉴权端点");
    console_auth_reset_lockout();
    /* 路由注册由 test_activation_bootstrap 那一次 console_auth_init() 完成：
       http_route 不去重且 ROUTE_MAX 只有 8，整个测试二进制只应调它一次，
       否则会把槽位烧给同一个前缀，坑到后续任务注册自己的路由。 */
    CHECK(console_auth_seed("ABCD1234", NULL, true) == HAL_OK, "重新播种（回到出厂强制改密态）");

    /* 未知子路径 → 404 语义；非 POST → 400 语义 */
    req_make(&req, "POST", "/api/v1/auth/bogus", "{}", NULL);
    CHECK(console_auth_test_dispatch(&req, "10.1.0.1", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie))
          == HAL_ENODEV, "未知子路径返回 ENODEV(404)");
    req_make(&req, "GET", "/api/v1/auth/login", NULL, NULL);
    CHECK(console_auth_test_dispatch(&req, "10.1.0.2", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie))
          == HAL_EINVAL, "非 POST 方法被拒");

    /* 畸形请求体 */
    req_make(&req, "POST", "/api/v1/auth/challenge", "not-json", NULL);
    CHECK(console_auth_test_dispatch(&req, "10.1.0.3", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie))
          == HAL_EINVAL, "畸形 JSON 被拒");
    req_make(&req, "POST", "/api/v1/auth/challenge", "{}", NULL);
    CHECK(console_auth_test_dispatch(&req, "10.1.0.4", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie))
          == HAL_EINVAL, "缺 user 字段被拒");

    /* 完整登录 */
    CHECK(do_login("admin", "ABCD1234", IP_OPS, cookie, sizeof(cookie), &must_change) == HAL_OK,
          "登录成功");
    CHECK(must_change == true, "登录响应带 must_change_password=true");
    CHECK(strstr(cookie, "token=") == cookie, "Cookie 形如 token=...");

    /* 会话门禁：must_change 为真时业务端点 403、豁免路径放行 */
    req_make(&req, "GET", "/api/v1/config", NULL, NULL);
    CHECK(console_auth_check(&req) == HAL_EUNAUTH_, "无 Cookie → 未登录");
    req_make(&req, "GET", "/api/v1/config", NULL, "token=deadbeefdeadbeef");
    CHECK(console_auth_check(&req) == HAL_EUNAUTH_, "伪造 token → 未登录");
    req_make(&req, "GET", "/api/v1/config", NULL, cookie);
    CHECK(console_auth_check(&req) == HAL_EPERM_, "未改密时业务端点 403");
    req_make(&req, "GET", "/api/v1/system/info", NULL, cookie);
    CHECK(console_auth_check(&req) == HAL_OK, "system/info 豁免强制改密");
    req_make(&req, "POST", "/api/v1/auth/password", NULL, cookie);
    CHECK(console_auth_check(&req) == HAL_OK, "auth/* 豁免强制改密");

    /* 改密端点：掩码方式，请求体里没有任何明文口令 */
    {
        static const uint8_t new_salt[16] = {
            0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
            0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x00
        };
        char new_salt_hex[64];

        /* 字段缺失 → 400（连 nonce 都不该被消耗） */
        snprintf(req_body, sizeof(req_body), "{\"user\":\"admin\"}");
        req_make(&req, "POST", "/api/v1/auth/password", req_body, cookie);
        CHECK(console_auth_test_dispatch(&req, IP_OPS, body, sizeof(body),
                                         set_cookie, sizeof(set_cookie))
              == HAL_EINVAL, "改密缺字段被拒");

        /* 长度/编码不合法的 new_salt / new_key_masked / chk_masked：必须**先于**
           proof 校验被拒。nonce "deadbeef" 并不存在——若先验校验就会返回 HAL_EPERM_，
           拿到 HAL_EINVAL 才说明畸形输入既没消耗 nonce、也没计进按 IP 的失败锁定。 */
        CHECK(console_auth_set_key_masked("admin", "deadbeef", "00", "1122", "33", "44",
                                          "10.0.0.9") == HAL_EINVAL,
              "new_salt/new_key_masked 长度不对时先于校验被拒");
        CHECK(console_auth_set_key_masked("admin", "deadbeef", "00",
                                          "112233445566778899aabbccddeeff00",
                                          "zz", "44", "10.0.0.9") == HAL_EINVAL,
              "非十六进制字符被拒");

        CHECK(build_password_body("admin", "ABCD1234", "NewPass@123", IP_OPS, new_salt, false,
                                  req_body, sizeof(req_body), new_salt_hex) == HAL_OK,
              "组装掩码改密请求体");
        CHECK(strstr(req_body, "ABCD1234") == NULL && strstr(req_body, "NewPass@123") == NULL,
              "改密请求体里不得出现任何明文口令");
        CHECK(strstr(req_body, "chk_masked") != NULL, "请求体带 chk_masked 字段");

        /* 新口令 == 旧口令：chk 解出来正好等于 stored_key，必须拒绝。
           否则客户端提交 PBKDF2(出厂码, new_salt) 就能清掉 must_change 却继续用出厂口令，
           "已完成强制改密"的设备仍可被标签上的验证码接管。 */
        CHECK(build_password_body("admin", "ABCD1234", "ABCD1234", IP_OPS, new_salt, false,
                                  req_body, sizeof(req_body), new_salt_hex) == HAL_OK,
              "组装新旧口令相同的请求体");
        req_make(&req, "POST", "/api/v1/auth/password", req_body, cookie);
        CHECK(console_auth_test_dispatch(&req, IP_OPS, body, sizeof(body),
                                         set_cookie, sizeof(set_cookie))
              == HAL_EINVAL, "新口令与旧口令相同被拒");
        CHECK(console_auth_must_change() == true, "被拒后 must_change 不得被清掉");

        /* proof 不对 → 403，且凭据不得被改动（否则是 DoS 面） */
        CHECK(build_password_body("admin", "ABCD1234", "Hacker@9999", IP_OPS, new_salt, true,
                                  req_body, sizeof(req_body), new_salt_hex) == HAL_OK,
              "组装 proof 被篡改的请求体");
        req_make(&req, "POST", "/api/v1/auth/password", req_body, cookie);
        CHECK(console_auth_test_dispatch(&req, IP_OPS, body, sizeof(body),
                                         set_cookie, sizeof(set_cookie))
              == HAL_EPERM_, "proof 不符时改密被拒");
        CHECK(console_auth_must_change() == true, "被拒的改密不得改动凭据状态");

        /* 正常改密 */
        CHECK(build_password_body("admin", "ABCD1234", "NewPass@123", IP_OPS, new_salt, false,
                                  req_body, sizeof(req_body), new_salt_hex) == HAL_OK,
              "重新组装掩码改密请求体");
        req_make(&req, "POST", "/api/v1/auth/password", req_body, cookie);
        CHECK(console_auth_test_dispatch(&req, IP_OPS, body, sizeof(body),
                                         set_cookie, sizeof(set_cookie))
              == HAL_OK, "掩码改密成功");
        CHECK(strstr(set_cookie, "Max-Age=0") != NULL, "改密后下发失效 Cookie");
        CHECK(console_auth_must_change() == false, "改密后解除强制改密");
        req_make(&req, "GET", "/api/v1/config", NULL, cookie);
        CHECK(console_auth_check(&req) == HAL_EUNAUTH_, "改密后旧会话失效");

        /* 服务端解出的凭据必须与客户端算的一致：challenge 应回显客户端选的 new_salt，
           且用新口令 + 该 salt 算出的 proof 能通过校验 */
        {
            char got_salt[80], n2[80];
            unsigned it = 0;
            CHECK(fetch_challenge("admin", IP_OPS, got_salt, sizeof(got_salt),
                                  n2, sizeof(n2), &it) == HAL_OK, "改密后取 challenge");
            CHECK(strcmp(got_salt, new_salt_hex) == 0, "服务端已安装客户端自选的 new_salt");
            CHECK(it != 0, "challenge 必须下发 iter（否则前端只能硬编码迭代次数）");
        }
    }

    /* 改密后用新口令重新登录，业务端点放行 */
    CHECK(do_login("admin", "NewPass@123", IP_OPS, cookie, sizeof(cookie), &must_change) == HAL_OK,
          "以新口令登录");
    CHECK(must_change == false, "登录响应 must_change_password=false");
    req_make(&req, "GET", "/api/v1/config", NULL, cookie);
    CHECK(console_auth_check(&req) == HAL_OK, "改密后业务端点放行");

    /* 重放：同一 nonce 第二次登录必须失败 */
    CHECK(fetch_challenge("admin", IP_OPS, salt_hex, sizeof(salt_hex),
                          nonce, sizeof(nonce), NULL) == HAL_OK, "取 challenge");
    console_auth_make_proof("NewPass@123", salt_hex, nonce, proof, sizeof(proof));
    snprintf(req_body, sizeof(req_body), "{\"user\":\"admin\",\"nonce\":\"%s\",\"proof\":\"%s\"}",
             nonce, proof);
    req_make(&req, "POST", "/api/v1/auth/login", req_body, NULL);
    CHECK(console_auth_test_dispatch(&req, IP_OPS, body, sizeof(body),
                                     set_cookie, sizeof(set_cookie))
          == HAL_OK, "首次使用该 nonce 登录成功");
    req_make(&req, "POST", "/api/v1/auth/login", req_body, NULL);
    CHECK(console_auth_test_dispatch(&req, IP_OPS, body, sizeof(body),
                                     set_cookie, sizeof(set_cookie))
          == HAL_EPERM_, "同一 nonce 重放被拒");

    /* nonce 表按 IP 分区：攻击者刷满 challenge 也不得挤掉运维在途的 nonce */
    {
        char ops_salt[80], ops_nonce[80], ops_proof[160];
        int k;
        CHECK(fetch_challenge("admin", IP_OPS, ops_salt, sizeof(ops_salt),
                              ops_nonce, sizeof(ops_nonce), NULL) == HAL_OK, "运维取 challenge");
        for (k = 0; k < 8; k++) {   /* 攻击者连刷 8 次，远超 nonce 表容量 4 */
            char a_salt[80], a_nonce[80];
            CHECK(fetch_challenge("admin", "203.0.113.7", a_salt, sizeof(a_salt),
                                  a_nonce, sizeof(a_nonce), NULL) == HAL_OK, "攻击者取 challenge");
        }
        console_auth_make_proof("NewPass@123", ops_salt, ops_nonce, ops_proof, sizeof(ops_proof));
        CHECK(console_auth_verify_from("admin", ops_nonce, ops_proof, IP_OPS) == HAL_OK,
              "运维的 nonce 未被未鉴权的 challenge 洪水挤掉");
    }

    /* 注销：需登录，且注销后会话立即失效 */
    req_make(&req, "POST", "/api/v1/auth/logout", NULL, NULL);
    CHECK(console_auth_test_dispatch(&req, IP_OPS, body, sizeof(body),
                                     set_cookie, sizeof(set_cookie))
          == HAL_EUNAUTH_, "未登录不能注销");
    req_make(&req, "POST", "/api/v1/auth/logout", NULL, cookie);
    CHECK(console_auth_test_dispatch(&req, IP_OPS, body, sizeof(body),
                                     set_cookie, sizeof(set_cookie))
          == HAL_OK, "注销成功");
    CHECK(strstr(set_cookie, "Max-Age=0") != NULL, "注销下发失效 Cookie");
    req_make(&req, "GET", "/api/v1/config", NULL, cookie);
    CHECK(console_auth_check(&req) == HAL_EUNAUTH_, "注销后会话失效");
}

/**
 * 首次激活自举（Task 8.5：取代出厂验证码自举）。产线不再烧录唯一验证码，
 * `console_auth_init` 不再做任何自动播种；凭据必须由用户首次访问设备时通过
 * `POST /api/v1/auth/activate` 自行设置，`GET /api/v1/auth/state` 供前端在此
 * 之前判断该显示激活页还是登录页。
 */
static void test_activation_bootstrap(void)
{
    char body[512], set_cookie[256];
    char salt_hex[80], nonce[80], proof[160];
    http_req_t req;

    SECTION("首次激活自举（取代出厂验证码）");
    console_auth_reset_lockout();

    /* 回到"从未配置"的真实出厂状态。刻意留一份 verify_code 在安全存储里
       （console_net.c 的 AP PSK 派生仍会用到它，与本次改动无关），用来证明
       即便它存在，console_auth 也不会再拿它做任何自动播种。 */
    CHECK(hal()->crypto->secure_write(HAL_SEC_KEY_VERIFY_CODE,
                                      (const uint8_t *)"FCT98765", 8) == HAL_OK,
          "安全存储里仍可能留有验证码（供 AP PSK 派生），与登录无关");
    hal()->crypto->secure_delete(HAL_SEC_KEY_LOCAL_USER);
    console_auth_test_reload();

    /* 整个测试二进制里**唯一**一次 console_auth_init()：既验证路由注册，
       也验证真实入口不再做任何自动播种。其余用例一律走 console_auth_test_bootstrap()，
       否则 http_route（不去重，ROUTE_MAX=8）会被同一前缀反复占槽。 */
    CHECK(console_auth_init() == HAL_OK, "console_auth_init 成功");
    CHECK(http_route_match("/api/v1/auth/login") != NULL, "登录路径可命中路由");
    CHECK(http_route_match("/api/v1/auth/challenge") != NULL, "挑战路径可命中路由");
    CHECK(console_auth_must_change() == true, "未激活按最严处理：视为需要强制改密");

    /* 未激活：GET /auth/state 免鉴权，返回 activated:false */
    req_make(&req, "GET", "/api/v1/auth/state", NULL, NULL);
    CHECK(console_auth_test_dispatch(&req, "172.16.0.5", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_OK,
          "state 端点无需登录即可访问");
    CHECK(body_flag(body, "activated", true) == false,
          "未激活时 activated=false，实际：%s", body);

    /* 即便安全存储里有验证码，也不能拿它登录——没有自动播种，凭据根本不存在 */
    CHECK(console_auth_challenge_from("admin", salt_hex, sizeof(salt_hex),
                                      nonce, sizeof(nonce), "172.16.0.5") == HAL_OK,
          "无凭据时 challenge 仍返回（伪 salt，防枚举）");
    CHECK(console_auth_make_proof("FCT98765", salt_hex, nonce, proof, sizeof(proof)) == HAL_OK, "算 proof");
    CHECK(console_auth_verify_from("admin", nonce, proof, "172.16.0.5") == HAL_EPERM_,
          "安全存储里的验证码不再能登录，不得静默放行");

    /* 弱口令激活被拒，不改变激活状态。评审 Ruling 18 之后 /auth/activate 要求
       Content-Type: application/json，否则会在到达密码强度校验之前就被 CSRF
       网关拒绝——这里补上该头，确保本用例仍然在校验"弱口令"这条路径，而不是
       巧合地撞上同一个 HAL_EINVAL。 */
    req_make(&req, "POST", "/api/v1/auth/activate", "{\"password\":\"short\"}", NULL);
    req_add_header(&req, "Content-Type", "application/json");
    CHECK(console_auth_test_dispatch(&req, "172.16.0.5", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_EINVAL,
          "弱口令激活被拒");

    /* 首次激活：用户自行设置密码（同样需要 Content-Type 才能通过 Ruling 18 的网关） */
    req_make(&req, "POST", "/api/v1/auth/activate", "{\"password\":\"ActivatePwd1\"}", NULL);
    req_add_header(&req, "Content-Type", "application/json");
    CHECK(console_auth_test_dispatch(&req, "172.16.0.5", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_OK,
          "首次激活成功");
    CHECK(console_auth_must_change() == false, "用户自设密码无需再强制改密");

    /* 激活后 state 变为 true */
    req_make(&req, "GET", "/api/v1/auth/state", NULL, NULL);
    CHECK(console_auth_test_dispatch(&req, "172.16.0.5", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_OK, "再次取 state");
    CHECK(body_flag(body, "activated", false) == true,
          "激活后 activated=true，实际：%s", body);

    /* 用刚设置的密码可以直接登录 */
    CHECK(console_auth_challenge_from("admin", salt_hex, sizeof(salt_hex),
                                      nonce, sizeof(nonce), "172.16.0.5") == HAL_OK, "取 challenge");
    CHECK(console_auth_make_proof("ActivatePwd1", salt_hex, nonce, proof, sizeof(proof)) == HAL_OK,
          "以激活口令算 proof");
    CHECK(console_auth_verify_from("admin", nonce, proof, "172.16.0.5") == HAL_OK,
          "用户自设的激活口令可直接登录，且无需先改密");

    /* 安全红线：已激活设备拒绝重复激活，否则任何人都能在局域网内重置密码。
       带上 Content-Type 让请求越过 Ruling 18 网关，真正测到 HAL_ESTATE 这条
       "已激活"判定，而不是被 CSRF 检查提前短路成 HAL_EINVAL。 */
    req_make(&req, "POST", "/api/v1/auth/activate", "{\"password\":\"AnotherPwd2\"}", NULL);
    req_add_header(&req, "Content-Type", "application/json");
    CHECK(console_auth_test_dispatch(&req, "172.16.0.6", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_ESTATE,
          "已激活设备拒绝重复激活");
    CHECK(console_http_status(HAL_ESTATE) == 409, "HAL_ESTATE 映射为 HTTP 409");

    /* 方法校验：state 只认 GET，activate 只认 POST（CSRF 防护对新增端点依然生效） */
    req_make(&req, "POST", "/api/v1/auth/state", NULL, NULL);
    CHECK(console_auth_test_dispatch(&req, "172.16.0.5", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_EINVAL,
          "state 非 GET 被拒");
    req_make(&req, "GET", "/api/v1/auth/activate", NULL, NULL);
    CHECK(console_auth_test_dispatch(&req, "172.16.0.5", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_EINVAL,
          "activate 非 POST 被拒");
}

/**
 * 评审 I-1（opus 二次评审）：Ruling 18 的 CSRF 网关此前没有任何专项用例——
 * test_activation_bootstrap 里的每个 activate 请求都只是顺带补上
 * Content-Type 头以绕过网关，从未直接断言网关本身四条分支各自的行为。
 * 本函数补齐：缺 Content-Type、Content-Type 不对、Origin 与 Host 跨源、
 * Origin 与 Host 同源。跨源/同源两个用例都必须用弱口令（"short"）触发
 * 密码强度校验的 HAL_EINVAL 而不能用合法口令——合法口令会真的把设备激活
 * 掉，消耗"仅限从未配置时可用"这个一次性窗口，干扰后续用例对初始状态的
 * 假设。放在 test_activation_bootstrap 之后、test_corrupt_cred_not_reset
 * 之前，自行把状态复位到"从未配置"，不依赖也不影响相邻用例。
 */
static void test_activate_csrf_gate(void)
{
    char body[512], set_cookie[256];
    http_req_t req;

    SECTION("Ruling 18：CSRF 网关四条分支");
    console_auth_reset_lockout();
    hal()->crypto->secure_delete(HAL_SEC_KEY_LOCAL_USER);
    console_auth_test_reload();

    /* 1) 缺 Content-Type：第 1 层直接拒绝，尚未走到密码校验 */
    req_make(&req, "POST", "/api/v1/auth/activate", "{\"password\":\"ActivatePwd1\"}", NULL);
    CHECK(console_auth_test_dispatch(&req, "172.16.1.1", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_EINVAL,
          "缺 Content-Type 被第 1 层网关拒绝");

    /* 2) Content-Type 非 application/json：浏览器原生 <form> 只能发出
       x-www-form-urlencoded / multipart/form-data / text/plain 之一，
       这里取其一即可代表整类 */
    req_make(&req, "POST", "/api/v1/auth/activate", "{\"password\":\"ActivatePwd1\"}", NULL);
    req_add_header(&req, "Content-Type", "application/x-www-form-urlencoded");
    CHECK(console_auth_test_dispatch(&req, "172.16.1.1", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_EINVAL,
          "表单 Content-Type 被第 1 层网关拒绝");

    /* 3) Content-Type 正确但 Origin 与 Host 跨源：第 2 层拒绝，返回专属的
       HAL_ECSRF_（评审 M-4），不再复用"请先改密"文案的 HAL_EPERM_ */
    req_make(&req, "POST", "/api/v1/auth/activate", "{\"password\":\"short\"}", NULL);
    req_add_header(&req, "Content-Type", "application/json");
    req_add_header(&req, "Origin", "http://evil.example.com");
    req_add_header(&req, "Host", "192.168.1.1:8080");
    CHECK(console_auth_test_dispatch(&req, "172.16.1.1", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_ECSRF_,
          "跨源 Origin 被第 2 层网关拒绝");
    CHECK(console_http_status(HAL_ECSRF_) == 403, "HAL_ECSRF_ 映射为 HTTP 403");

    /* 4) Origin 与 Host 同源：不应被第 2 层拦截，请求应放行到密码强度
       校验——用弱口令验证收到的是 HAL_EINVAL（长度不足）而非 HAL_ECSRF_，
       证明这条请求确实穿过了 CSRF 网关，而不是恰好在网关处也返回同一个
       错误码 */
    req_make(&req, "POST", "/api/v1/auth/activate", "{\"password\":\"short\"}", NULL);
    req_add_header(&req, "Content-Type", "application/json");
    req_add_header(&req, "Origin", "http://192.168.1.1:8080");
    req_add_header(&req, "Host", "192.168.1.1:8080");
    CHECK(console_auth_test_dispatch(&req, "172.16.1.1", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_EINVAL,
          "同源 Origin 未被网关拦截，请求穿透到密码强度校验");

    /* 全程 4 个用例没有一次真正调用 console_auth_seed（要么被网关挡下，
       要么用的是弱口令），设备应仍处于未激活状态 */
    req_make(&req, "GET", "/api/v1/auth/state", NULL, NULL);
    CHECK(console_auth_test_dispatch(&req, "172.16.1.1", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_OK, "取 state");
    CHECK(body_flag(body, "activated", true) == false,
          "本用例全程未激活，实际：%s", body);
}

/**
 * 评审 I-1：Ruling 19 第二判据（config 与凭据矛盾）此前没有专门的用例——
 * 已有的 test_corrupt_cred_not_reset 覆盖的是"凭据记录本身损坏"（第一判据，
 * 即 ACTIVATION_BLOCKED_CRED），不是"config 显示已激活、但凭据读取却是
 * HAL_ENODEV"这种两者矛盾的场景（ACTIVATION_BLOCKED_MISMATCH）。用
 * cfg_set_str 直接写入 localUser.name 模拟"曾经激活过、config 已落盘"，
 * 同时清空凭据存储使其读取结果为 HAL_ENODEV，制造第二判据要拦的矛盾态。
 */
static void test_activate_second_gate_mismatch(void)
{
    char body[512], set_cookie[256];
    char probe[CONSOLE_USER_MAX];
    http_req_t req;

    SECTION("Ruling 19 第二判据：config 与凭据矛盾（M-3 共享判据 activation_gate）");
    console_auth_reset_lockout();
    CHECK(cfg_init(NULL, "console_test_cfg_ruling19.json") == HAL_OK, "配置中心就绪");

    hal()->crypto->secure_delete(HAL_SEC_KEY_LOCAL_USER);
    console_auth_test_reload();
    CHECK(cfg_set_str("localUser.name", "ghostuser") == HAL_OK,
          "模拟 config 显示曾经激活过（localUser.name 已写入）");

    /* 凭据读取单独看是 HAL_ENODEV（真的从未配置），必须靠第二判据才能
       拦下——若第二判据被误删，这里会退化成真的允许激活 */
    req_make(&req, "POST", "/api/v1/auth/activate", "{\"password\":\"ActivatePwd1\"}", NULL);
    req_add_header(&req, "Content-Type", "application/json");
    CHECK(console_auth_test_dispatch(&req, "172.16.2.1", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_ESTATE,
          "config 与凭据矛盾时 activate 必须拒绝（第二判据）");

    /* ep_state 与 ep_activate 共用同一份 activation_gate()（评审 M-3），
       矛盾态下也必须报 activated:true，不能让前端展示激活页 */
    req_make(&req, "GET", "/api/v1/auth/state", NULL, NULL);
    CHECK(console_auth_test_dispatch(&req, "172.16.2.1", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_OK, "取 state");
    CHECK(body_flag(body, "activated", false) == true,
          "矛盾态下 state 必须报 activated=true，实际：%s", body);

    /* config 侧未被上面被拒绝的请求意外覆盖 */
    CHECK(cfg_get_str("localUser.name", probe, sizeof(probe)) == HAL_OK &&
          strcmp(probe, "ghostuser") == 0,
          "config 侧的 localUser.name 未被覆盖，实际：%s", probe);

    cfg_deinit();
    remove("console_test_cfg_ruling19.json");

    /* 收尾：恢复"从未配置"状态，不影响后续用例 */
    hal()->crypto->secure_delete(HAL_SEC_KEY_LOCAL_USER);
    console_auth_test_reload();
}

/**
 * 凭据损坏 / 版本不匹配**绝不能**被当成"从未配置"而允许被 `/auth/activate`
 * 覆盖——那会把一台已设过口令的设备静默重置成攻击者提交的新密码。最难受的
 * 触发路径是将来 bump CRED_VERSION 却没带迁移逻辑就 OTA 出去，会让整批设备
 * 的"损坏"记录都可以被第一个访问者的 activate 请求接管。
 */
static void test_corrupt_cred_not_reset(void)
{
    /* 一条魔数正确、版本号是"未来版本"的记录：正是 OTA 版本跃迁的样子 */
    uint8_t future[96];
    uint8_t back[96];
    size_t len = 0;
    char body[512], set_cookie[256];
    http_req_t req;

    SECTION("凭据损坏不得被 activate 静默覆盖");
    console_auth_reset_lockout();

    memset(future, 0xA7, sizeof(future));
    future[0] = 'I'; future[1] = 'C'; future[2] = 'L'; future[3] = 'U';
    future[4] = 0; future[5] = 0; future[6] = 0; future[7] = 2;   /* version = 2 */

    CHECK(hal()->crypto->secure_write(HAL_SEC_KEY_LOCAL_USER, future, sizeof(future)) == HAL_OK,
          "写入一条未来版本的凭据记录");
    console_auth_test_reload();
    console_auth_test_bootstrap();
    CHECK(console_auth_must_change() == true, "损坏记录下按最严处理：视为需要强制改密");

    /* 1) state 端点必须把"损坏"判定为已占用，不能引导用户以为设备可以激活 */
    req_make(&req, "GET", "/api/v1/auth/state", NULL, NULL);
    CHECK(console_auth_test_dispatch(&req, "172.16.0.7", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_OK, "取 state");
    CHECK(body_flag(body, "activated", false) == true,
          "损坏记录必须算作已占用，不得让前端展示激活页，实际：%s", body);

    /* 2) activate 必须被拒——不得用任意密码把设备"救活"，等于远程可任意重置。
       带上 Content-Type 以越过 Ruling 18 网关，确保测到的是"损坏记录拒绝"
       这条 HAL_ESTATE 判定本身，而非提前被 CSRF 检查短路。 */
    req_make(&req, "POST", "/api/v1/auth/activate", "{\"password\":\"Attacker@123\"}", NULL);
    req_add_header(&req, "Content-Type", "application/json");
    CHECK(console_auth_test_dispatch(&req, "172.16.0.7", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_ESTATE,
          "损坏记录下 activate 必须拒绝");

    /* 3) 存储里的原记录必须原样保留，不得被覆盖 */
    CHECK(hal()->crypto->secure_read(HAL_SEC_KEY_LOCAL_USER, back, sizeof(back), &len) == HAL_OK,
          "读回存储记录");
    CHECK(len == sizeof(future) && memcmp(back, future, sizeof(future)) == 0,
          "损坏记录未被 activate 覆盖");
}

/**
 * 评审 I-3：ENOENT 本身有歧义——可能是"这个 key 从未写过"（目录在，真未
 * 配置），也可能是"整个安全存储目录都不在"（分区未挂载/被误删，目录里
 * 原有的文件自然也读不到，errno 同样是 ENOENT）。此前两者都折叠成
 * HAL_ENODEV，与"从未配置"等价放行 /auth/activate；但后者本质上是
 * Ruling 19 要 fail-closed 的"凭据消失"场景。mock_state_ensure_dir()/
 * gk_crypto_ensure_dir() 已在平台初始化时幂等预建过目录，建立"目录在
 * 固件运行起来之后恒定存在"的前提，运行期间目录消失只能是"曾经存在、
 * 后来被拿走"——用 rename() 整体挪走/挪回 mock_state 目录模拟这一过程
 * （不逐个删除里面的文件，避免影响其它用例已写入的键）。
 */
static void test_missing_storage_dir_fails_closed(void)
{
    uint8_t buf[96];
    size_t len = 0;
    hal_err_t rc;

    SECTION("评审 I-3：安全存储目录整体缺失须 fail-closed，不同于文件未写过");

    hal()->crypto->secure_delete(HAL_SEC_KEY_LOCAL_USER);
    rc = hal()->crypto->secure_read(HAL_SEC_KEY_LOCAL_USER, buf, sizeof(buf), &len);
    CHECK(rc == HAL_ENODEV, "目录存在、文件未写过：真未配置 → HAL_ENODEV，实际 rc=%d", rc);

    CHECK(rename("mock_state", "mock_state_i3_moved") == 0,
          "挪走整个安全存储目录（模拟分区未挂载/被删）");

    rc = hal()->crypto->secure_read(HAL_SEC_KEY_LOCAL_USER, buf, sizeof(buf), &len);
    CHECK(rc == HAL_EIO,
          "目录整体缺失必须 fail-closed 为 HAL_EIO，不得等同于“从未配置”，实际 rc=%d", rc);

    CHECK(rename("mock_state_i3_moved", "mock_state") == 0,
          "挪回目录（模拟分区恢复挂载），内容应原样保留");

    rc = hal()->crypto->secure_read(HAL_SEC_KEY_LOCAL_USER, buf, sizeof(buf), &len);
    CHECK(rc == HAL_ENODEV,
          "目录恢复后应回到 HAL_ENODEV（真未配置），不应停留在 HAL_EIO，实际 rc=%d", rc);
}

static void test_cred_not_in_config(void)
{
    char salt_hex[80], nonce[80];
    char *dump;
    const size_t cap = 64 * 1024;

    SECTION("凭据不得进入配置导出");
    CHECK(cfg_init(NULL, "console_test_cfg.json") == HAL_OK, "配置中心就绪");
    CHECK(console_auth_seed("ABCD1234", NULL, true) == HAL_OK, "播种凭据");
    CHECK(console_auth_challenge("admin", salt_hex, sizeof(salt_hex), nonce, sizeof(nonce)) == HAL_OK,
          "取到公开的 salt");
    dump = (char *)malloc(cap);
    CHECK(dump != NULL, "分配导出缓冲");
    if (dump) {
        CHECK(cfg_dump_json(dump, cap) == HAL_OK, "导出全部配置");
        CHECK(strstr(dump, "localUser.key") == NULL, "导出不含 localUser.key");
        CHECK(strstr(dump, "localUser.salt") == NULL, "导出不含 localUser.salt");
        CHECK(strstr(dump, "localUser.iter") == NULL, "导出不含 localUser.iter");
        CHECK(strstr(dump, salt_hex) == NULL, "导出不含盐值本身");
        free(dump);
    }

    /* 只写键（localUser.password）：能下发、卡长度，但不落盘也不进导出。
       它是平台下发改密用的键，一旦进了导出/备份，等于把口令原样交给每次备份。 */
    SECTION("只写键不进导出与持久化文件");
    {
        cfg_reject_t rejects[4];
        FILE *f = NULL;
        size_t n = 0;
        memset(rejects, 0, sizeof(rejects));
        n = cfg_apply_json("{\"localUser.password\":\"NewPass@123\"}", rejects, 4);
        CHECK(n == 0, "合法口令写入被接受");
        n = cfg_apply_json("{\"localUser.password\":\"short\"}", rejects, 4);
        CHECK(n == 1, "过短口令按长度规则被拒绝");
        n = cfg_apply_json("{\"localUser.password\":\"NoDigitsHere\"}", rejects, 4);
        CHECK(n == 0, "长度合规即通过（字母+数字的要求在平台侧，固件只管长度）");

        dump = (char *)malloc(cap);
        CHECK(dump != NULL, "分配导出缓冲");
        if (dump) {
            CHECK(cfg_dump_json(dump, cap) == HAL_OK, "导出全部配置");
            CHECK(strstr(dump, "localUser.password") == NULL, "导出不含只写键的键名");
            CHECK(strstr(dump, "NewPass@123") == NULL, "导出不含口令明文");
            free(dump);
        }
        f = fopen("console_test_cfg.json", "rb");
        CHECK(f != NULL, "持久化文件存在");
        if (f) {
            char filebuf[4096];
            size_t got = fread(filebuf, 1, sizeof(filebuf) - 1, f);
            filebuf[got] = 0;
            fclose(f);
            CHECK(strstr(filebuf, "NewPass@123") == NULL, "持久化文件不含口令明文");
            CHECK(strstr(filebuf, "localUser.password") == NULL, "持久化文件不含只写键");
        }
    }
    cfg_deinit();
    remove("console_test_cfg.json");
}

/* ================= Ruling 21（I-5/I-6）：event_bus 接入与成功路径覆盖 ================= */
static volatile int g_bind_activated_hits;
static void on_bind_event(const event_t *e, void *u)
{
    (void)u;
    if (e->type == EVT_BIND_ACTIVATED) g_bind_activated_hits++;
}

/**
 * 评审 Ruling 21：此前生产路径的 event_bus_init 从未被真正调用过
 * （main.c 缺失该调用，I-5），且 ep_activate 里 event_bus_emit 的成功路径
 * 也从未被测试覆盖过（I-6）——只测过"总线未初始化时 emit 不崩溃"，没测过
 * "总线就绪时事件真的送达订阅者"。main.c 那一侧已补上 event_bus_init 调用
 * （见 app/main.c），这里在测试里搭一个同样"cfg_init + event_bus_init 都已
 * 就绪"的窗口，重新走一次激活流程，断言两件事：
 *  1) 激活成功后 cfg_get_str("localUser.name", ...) 能读到刚提交的用户名
 *     （对照 ep_activate 里"先播种再写 config"的顺序，见 Ruling 20）；
 *  2) 订阅 EVT_DOM_BIND 的处理器确实收到了 EVT_BIND_ACTIVATED
 *     （对照 core_test/main.c 的 test_event_bus 用法：init → subscribe →
 *     触发 → os_sleep_ms 等异步投递线程跑完 → 断言 → unsubscribe/deinit）。
 * 用完对称地 event_bus_deinit()/cfg_deinit()，不影响后续用例。
 */
static void test_activation_wires_config_and_event_bus(void)
{
    char body[512], set_cookie[256];
    char probe[CONSOLE_USER_MAX];
    event_sub_t *sub = NULL;
    http_req_t req;

    SECTION("激活成功后 config 落地 + event_bus 真正收到事件（Ruling 21）");
    console_auth_reset_lockout();

    /* 回到"从未配置"状态，与 test_activation_bootstrap 相同手法 */
    hal()->crypto->secure_delete(HAL_SEC_KEY_LOCAL_USER);
    console_auth_test_reload();

    CHECK(cfg_init(NULL, "console_test_cfg_evt.json") == HAL_OK, "配置中心就绪");
    CHECK(event_bus_init(64) == HAL_OK, "事件总线就绪（复刻生产路径 main.c 的调用）");
    g_bind_activated_hits = 0;
    CHECK(event_bus_subscribe(1u << EVT_DOM_BIND, on_bind_event, NULL, &sub) == HAL_OK,
          "订阅 EVT_DOM_BIND");

    req_make(&req, "POST", "/api/v1/auth/activate", "{\"user\":\"evtuser\",\"password\":\"EvtBusPwd1\"}", NULL);
    req_add_header(&req, "Content-Type", "application/json");
    CHECK(console_auth_test_dispatch(&req, "172.16.0.9", body, sizeof(body),
                                     set_cookie, sizeof(set_cookie)) == HAL_OK,
          "激活成功（event_bus/cfg 均已就绪）");

    CHECK(cfg_get_str("localUser.name", probe, sizeof(probe)) == HAL_OK && strcmp(probe, "evtuser") == 0,
          "激活成功后 config 里的 localUser.name 是刚提交的用户名，实际：%s", probe);

    os_sleep_ms(50);   /* 事件总线异步投递线程需要时间跑完 */
    CHECK(g_bind_activated_hits == 1, "订阅者确实收到一次 EVT_BIND_ACTIVATED，实际 hits=%d", g_bind_activated_hits);

    CHECK(event_bus_unsubscribe(sub) == HAL_OK, "取消订阅");
    CHECK(event_bus_deinit() == HAL_OK, "事件总线卸载");
    cfg_deinit();
    remove("console_test_cfg_evt.json");

    /* 收尾：把凭据状态还原为"从未配置"，避免影响后续用例对初始状态的假设 */
    hal()->crypto->secure_delete(HAL_SEC_KEY_LOCAL_USER);
    console_auth_test_reload();
}

static void test_cred_fallback_store(void)
{
    char salt_hex[80], nonce[80], proof[160];

    SECTION("无 hal_crypto 平台的软存储兜底");
    console_auth_reset_lockout();
    CHECK(hal_deinit() == HAL_OK, "卸载 HAL，模拟平台不提供 crypto 模块");
    CHECK(hal_has(HAL_MOD_CRYPTO) == false, "crypto 能力不可用");

    CHECK(console_auth_seed("FALLBK99", NULL, true) == HAL_OK, "凭据落到软存储文件");
    /* 路径必须在私有数据目录下，而不是进程工作目录里的裸文件名 */
    CHECK(strchr(console_auth_cred_path(), '/') != NULL, "软存储位于私有数据目录下");
    console_auth_test_reload();  /* 丢弃内存缓存，强制从存储读回 */
    CHECK(console_auth_challenge("admin", salt_hex, sizeof(salt_hex), nonce, sizeof(nonce)) == HAL_OK,
          "从软存储读回后可取 challenge");
    CHECK(console_auth_make_proof("FALLBK99", salt_hex, nonce, proof, sizeof(proof)) == HAL_OK,
          "算 proof");
    CHECK(console_auth_verify_from("admin", nonce, proof, "172.16.9.1") == HAL_OK,
          "软存储凭据校验通过");

    remove(console_auth_cred_path());
    CHECK(hal_init(profile_raw_json()) == HAL_OK, "恢复 HAL");
}

/* ========================================================================== */
/* Task 7：REST —— 配置读写与系统信息                                            */
/* ========================================================================== */

static void test_config_rules(void)
{
    cfg_reject_t rejects[4];
    int n;

    /* 自成一段独立的 cfg_init/cfg_deinit 窗口，与 test_cred_not_in_config
       各自独立（互不重叠，顺序执行不会造成重复初始化）；cfg_apply_json/
       cfg_register_rules 都要求 g.inited 已为真。持久化文件名与
       test_cred_not_in_config 的不同，避免任何残留文件互相干扰。 */
    CHECK(cfg_init(NULL, "console_test_cfg_rules.json") == HAL_OK, "配置中心就绪");

    SECTION("配置规则与拒绝列表");
    CHECK(console_api_register_rules() == HAL_OK, "登记 video.* 规则");

    /* 合法值应被接受 */
    n = cfg_apply_json("{\"video.0.main.kbps\":2048}", rejects, 4);
    CHECK(n == 0, "合法码率被接受，rejected=%d", n);

    /* 越界值应进入 rejected 而非整体失败 */
    n = cfg_apply_json("{\"video.0.main.kbps\":99999,\"video.0.main.gop\":50}", rejects, 4);
    CHECK(n == 1, "越界码率被拒，其余照常应用，rejected=%d", n);
    CHECK(strcmp(rejects[0].key, "video.0.main.kbps") == 0, "拒绝的键名正确：%s", rejects[0].key);
    {
        int64_t gop = 0;
        CHECK(cfg_get_int("video.0.main.gop", &gop) == HAL_OK && gop == 50,
              "同批次的合法键仍被应用（部分成功语义）");
    }

    /* 子码流只允许 h264 */
    n = cfg_apply_json("{\"video.1.sub.codec\":\"h265\"}", rejects, 4);
    CHECK(n == 1, "子码流拒绝 h265");

    /*
     * 上面几条断言在"规则完全缺失/被遮蔽"时也会得出相同结论（例如
     * kbps=99999 无论是被 core 的 32~16384 还是 console 曾经想登记的
     * 128~4096 拒绝，结果都一样是 rejected=1），对"上下界真的来自 profile"
     * 零证明力。这里换一种能真正证伪的写法：直接取 profile 当前的
     * channels[].max_w，边界值本身应被接受、边界值+1 应被拒绝。换一款
     * max_w 不同的型号，这条断言依然成立；反过来，如果对应的规则完全
     * 没被注册（unknown_key）或上界被写死成与 profile 无关的数，这里会
     * 真的变红——而不是像上面几条那样巧合地保持绿色。
     */
    {
        const profile_channel_t *main_ch = profile_channel_by_name("main");
        CHECK(main_ch != NULL, "profile 含 main 通道");
        if (main_ch) {
            char key[CFG_KEY_MAX], body_ok[96], body_bad[96];
            int64_t max_w = (int64_t)main_ch->max_w;

            snprintf(key, sizeof(key), "video.%d.%s.w", main_ch->ch, main_ch->name);

            snprintf(body_ok, sizeof(body_ok), "{\"%s\":%lld}", key, (long long)max_w);
            n = cfg_apply_json(body_ok, rejects, 4);
            CHECK(n == 0, "上界值（profile.max_w=%lld）应被接受，rejected=%d", (long long)max_w, n);

            snprintf(body_bad, sizeof(body_bad), "{\"%s\":%lld}", key, (long long)(max_w + 1));
            n = cfg_apply_json(body_bad, rejects, 4);
            CHECK(n == 1, "上界值+1（%lld）应被拒绝——证明上界确实跟随 profile 而非巧合通过，rejected=%d",
                  (long long)(max_w + 1), n);
            CHECK(strcmp(rejects[0].key, key) == 0, "拒绝的键名正确：%s", rejects[0].key);
        }
    }

    cfg_deinit();
    remove("console_test_cfg_rules.json");
}

static void test_caps_json(void)
{
    char buf[2048];
    SECTION("能力清单");
    CHECK(console_caps_json(buf, sizeof(buf)) == HAL_OK, "生成能力清单");
    CHECK(strstr(buf, "\"wifi\"") != NULL, "含 wifi 字段");
    CHECK(strstr(buf, "\"tf\"") != NULL, "含 tf 字段");
    CHECK(strstr(buf, "\"h265\"") != NULL, "含 h265 字段");
    /* 能力值应来自 profile，而非硬编码 */
    CHECK(strstr(buf, "\"model\"") != NULL, "含型号");
    /* R0：caps 补齐音频/智能/协议/日夜，供前端裁剪表单 */
    CHECK(strstr(buf, "\"audio_in\"") != NULL, "含 audio_in");
    CHECK(strstr(buf, "\"ivs_motion\"") != NULL, "含 ivs_motion");
    CHECK(strstr(buf, "\"gb\"") != NULL, "含 gb 协议位");
    CHECK(strstr(buf, "\"daynight\"") != NULL, "含 daynight");
    /* mock-x86：network 仅有 eth → wifi=false；storage.tf=true */
    CHECK(strstr(buf, "\"wifi\":false") != NULL, "mock 无 WiFi 模块时 wifi=false");
    CHECK(strstr(buf, "\"tf\":true") != NULL, "mock 有 TF 能力");
}

/** R1：能力管理页数据源 GET /api/v1/system/capabilities + module.*.enabled 规则 */
static void test_capabilities_endpoint(void)
{
    http_req_t req;
    char body[16384];
    char cookie[128];
    bool must_change = false;

    SECTION("REST 能力与模块管理端点");
    CHECK(cfg_init(NULL, "console_test_cfg_caps.json") == HAL_OK, "配置中心就绪");
    CHECK(console_api_register_rules() == HAL_OK, "登记 module.*.enabled 规则");
    console_auth_reset_lockout();
    CHECK(console_auth_seed("ABCD1234", NULL, true) == HAL_OK, "播种凭据");
    CHECK(console_auth_set_password("ABCD1234", "NewPass@123") == HAL_OK, "改密");
    CHECK(do_login("admin", "NewPass@123", "192.168.70.10", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "登录成功");

    req_make(&req, "GET", "/api/v1/system/capabilities", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK, "capabilities 成功");
    CHECK(strstr(body, "\"features\":[") != NULL, "含功能明细数组");
    CHECK(strstr(body, "\"modules\":[") != NULL, "含模块目录数组");
    CHECK(strstr(body, "\"module.rtsp.enabled\"") != NULL, "含 RTSP 配置键");
    CHECK(strstr(body, "\"toggleable\"") != NULL, "含可开关标记");
    CHECK(strstr(body, "\"reboot_required\"") != NULL, "含重启策略");
    CHECK(strstr(body, "\"source\":\"hardware\"") != NULL || strstr(body, "\"source\": \"hardware\"") != NULL,
          "含 hardware 来源标签");
    /* console 固件必备：不可开关 */
    CHECK(strstr(body, "\"name\":\"console\"") != NULL, "目录含 console");
    /* mock 允许 rtsp：toggleable 应为 true */
    CHECK(strstr(body, "\"cfg_key\":\"module.rtsp.enabled\"") != NULL, "RTSP 键写入目录");

    /* 关闭 RTSP 后 config_enabled=false */
    CHECK(cfg_set_bool("module.rtsp.enabled", false) == HAL_OK, "写入 module.rtsp.enabled=false");
    req_make(&req, "GET", "/api/v1/system/capabilities", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK, "再次读取");
    CHECK(strstr(body, "\"config_enabled\":false") != NULL, "RTSP 配置开关反映为 false");

    /* PUT config 应能改回 */
    {
        bool on = false;
        req_make(&req, "PUT", "/api/v1/config", "{\"module.rtsp.enabled\":true}", cookie);
        CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK, "PUT 恢复开关");
        CHECK(cfg_get_bool("module.rtsp.enabled", &on) == HAL_OK && on, "配置可读回为 true");
    }

    cfg_deinit();
    remove("console_test_cfg_caps.json");
}

/**
 * 覆盖 GET/PUT /api/v1/config 的端点层：鉴权门禁、全量/前缀读取、部分成功
 * 语义、畸形输入。经 console_api_test_dispatch 分发，不依赖真实 socket。
 */
static void test_api_config_endpoints(void)
{
    http_req_t req;
    char body[8192];
    char cookie[128];
    bool must_change = false;

    SECTION("REST 配置读写端点");
    CHECK(cfg_init(NULL, "console_test_cfg_api.json") == HAL_OK, "配置中心就绪");
    CHECK(console_api_register_rules() == HAL_OK, "登记规则");
    console_auth_reset_lockout();

    /* 未登录 */
    req_make(&req, "GET", "/api/v1/config", NULL, NULL);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_EUNAUTH_,
          "未登录访问 config 返回未登录");

    CHECK(console_auth_seed("ABCD1234", NULL, true) == HAL_OK, "播种凭据（出厂态）");
    CHECK(do_login("admin", "ABCD1234", "192.168.50.10", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "登录成功");

    /* 出厂态必须先改密，config 端点应 403（未豁免强制改密） */
    req_make(&req, "GET", "/api/v1/config", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_EPERM_,
          "未改密时 403");

    CHECK(console_auth_set_password("ABCD1234", "NewPass@123") == HAL_OK, "改密解除强制改密");
    CHECK(do_login("admin", "NewPass@123", "192.168.50.10", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "以新口令重新登录");

    /* GET 全量：至少含 profile 播种的已知键 */
    req_make(&req, "GET", "/api/v1/config", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK, "GET 全量配置成功");
    CHECK(strstr(body, "\"code\":0") != NULL, "响应含 code:0");
    CHECK(strstr(body, "\"data\":{") != NULL, "响应含 data 对象");
    CHECK(strstr(body, "video.0.main.kbps") != NULL, "全量配置含已知键，实际：%s", body);

    /* GET 前缀过滤：键名应已去掉前缀本身。req_make 只填 path，不解析
       "?"——必须像 http_parse_request 那样把 query 单独放进 req.query，
       否则 path 里带着字面 "?prefix=..." 会导致 api_dispatch 的精确匹配
       全部落空。 */
    req_make(&req, "GET", "/api/v1/config", NULL, cookie);
    snprintf(req.query, sizeof(req.query), "prefix=video.0.main");
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK,
          "GET 前缀过滤成功");
    CHECK(strstr(body, "\"kbps\"") != NULL, "前缀过滤后含去掉前缀的键名，实际：%s", body);
    CHECK(strstr(body, "video.0.main.kbps") == NULL, "前缀过滤后不应再重复带前缀");

    /* 前缀不命中任何键：视为空对象而非错误 */
    req_make(&req, "GET", "/api/v1/config", NULL, cookie);
    snprintf(req.query, sizeof(req.query), "prefix=no.such.prefix");
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK, "不存在的前缀仍是成功响应");
    CHECK(strstr(body, "\"data\":{}") != NULL, "不存在的前缀返回空对象，实际：%s", body);

    /* PUT 部分成功：一个合法、一个越界 */
    req_make(&req, "PUT", "/api/v1/config",
             "{\"video.0.main.kbps\":2048,\"video.0.main.gop\":99999}", cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK, "PUT 部分成功仍是 200");
    CHECK(strstr(body, "\"applied\":1") != NULL, "applied=1（其余 1 个被拒），实际：%s", body);
    CHECK(strstr(body, "\"rejected_total\":1") != NULL,
          "rejected_total=1——rejected[] 数组容量固定 8，被拒数超过时会静默截断，"
          "前端靠这个字段判断有没有截断，实际：%s", body);
    CHECK(strstr(body, "\"video.0.main.gop\"") != NULL, "rejected 数组含被拒键名");
    CHECK(strstr(body, "\"reason\"") != NULL, "rejected 条目含 reason 字段");

    /* rejected[] 数组容量固定为 8（ep_config_put 里 cfg_apply_json(.., rejects, 8)），
       提交 9 个全部不合法的键，验证 rejected_total 反映真实的 9、而数组本身
       按插入顺序截断到前 8 个——第 9 个键（zzz.marker）不应出现在响应体里，
       前端必须靠 rejected_total 而不是数组长度才能知道"还有没截断的"。 */
    req_make(&req, "PUT", "/api/v1/config",
             "{\"video.0.main.w\":999999,\"video.0.main.h\":999999,\"video.0.main.fps\":999999,"
             "\"video.0.main.kbps\":999999,\"video.0.main.gop\":999999,"
             "\"video.0.main.codec\":\"bogus\",\"video.0.main.rc\":\"bogus\","
             "\"video.1.sub.codec\":\"h265\",\"zzz.marker.beyond.eighth\":1}", cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK, "全部 9 个键都不合法仍是 200");
    CHECK(strstr(body, "\"applied\":0") != NULL, "9 个键全部不合法，applied=0，实际：%s", body);
    CHECK(strstr(body, "\"rejected_total\":9") != NULL,
          "rejected_total 反映真实被拒总数 9（超过数组容量 8），实际：%s", body);
    CHECK(strstr(body, "zzz.marker.beyond.eighth") == NULL,
          "第 9 个被拒键的详情应被数组容量截断掉，不出现在 rejected[] 里");

    /* PUT 畸形 JSON / 空 body / 不支持的方法 */
    req_make(&req, "PUT", "/api/v1/config", "not-json", cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_EINVAL, "畸形 JSON 被拒");
    req_make(&req, "PUT", "/api/v1/config", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_EINVAL, "空 body 被拒");
    req_make(&req, "DELETE", "/api/v1/config", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_EINVAL, "不支持的方法被拒");

    cfg_deinit();
    remove("console_test_cfg_api.json");
}

/**
 * 覆盖 system/info、system/status、reboot、reset、video/params、
 * storage/info：鉴权门禁（含 system/info 的强制改密豁免）、响应体关键字段、
 * 以及 reboot/reset "登记延后动作而非内联执行"这一 resolution B 核心约束。
 * 这几个端点都不碰 core/config，不需要 cfg_init。
 */
static void test_api_system_endpoints(void)
{
    http_req_t req;
    char body[8192];
    char cookie[128];
    bool must_change = false;
    bool deferred;

    SECTION("REST 系统信息与动作端点");
    console_auth_reset_lockout();
    CHECK(console_auth_seed("ABCD1234", NULL, true) == HAL_OK, "播种凭据（出厂态）");

    /* system/info 豁免的是"强制改密"，不是"登录"本身 */
    req_make(&req, "GET", "/api/v1/system/info", NULL, NULL);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_EUNAUTH_,
          "system/info 未登录时仍返回未登录");

    CHECK(do_login("admin", "ABCD1234", "192.168.50.11", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "登录成功（出厂态）");
    CHECK(must_change == true, "出厂态需要改密");

    req_make(&req, "GET", "/api/v1/system/info", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK,
          "system/info 在强制改密期间仍可访问（服务端豁免）");
    CHECK(strstr(body, "\"model\":\"MOCK-X86\"") != NULL, "含型号，实际：%s", body);
    CHECK(strstr(body, "\"caps\":{") != NULL, "内嵌能力清单对象");
    CHECK(strstr(body, "\"uptime_s\"") != NULL, "含运行时长");
    /* R0 能力驱动：features/modules 是前端导航裁剪的权威来源 */
    CHECK(strstr(body, "\"features\":{") != NULL, "内嵌功能映射对象");
    CHECK(strstr(body, "\"modules\":[") != NULL, "内嵌模块清单数组");
    CHECK(strstr(body, "\"preview.live\":false") != NULL, "本测试未启动视频编码 → 预览不可用（如实上报）");
    CHECK(strstr(body, "\"event.motion\":true") != NULL, "mock 开启移动侦测");
    CHECK(strstr(body, "\"event.smart\":false") != NULL, "mock 无智能算法");
    CHECK(strstr(body, "\"network.wifi\":false") != NULL, "mock 无 WiFi → 功能关闭");
    CHECK(strstr(body, "\"preview.playback\":false") != NULL, "recorder 未注册 → 回放功能关");
    /* 如实上报：模块未注册的功能一律 false，本计划新增的系统功能为 true */
    CHECK(strstr(body, "\"system.ota\":false") != NULL, "ota 模块未注册 → 升级入口隐藏，实际：%s", body);
    CHECK(strstr(body, "\"cloud.bind\":false") != NULL, "idp 模块未注册 → 云绑定隐藏");
    CHECK(strstr(body, "\"netplatform.gb\":false") != NULL, "gb28181 未注册 → 国标接入隐藏");
    CHECK(strstr(body, "\"storage.record\":false") != NULL, "recorder 未注册 → 录像计划隐藏");
    CHECK(strstr(body, "\"system.time\":true") != NULL, "时间设置可用");
    CHECK(strstr(body, "\"system.log\":true") != NULL, "系统日志可用");
    CHECK(strstr(body, "\"system.device\":true") != NULL, "设备名称可用");
    CHECK(strstr(body, "\"network.config\":true") != NULL, "网络设置可用");
    CHECK(strstr(body, "\"storage.format\":false") != NULL, "格式化未实现");

    /* system/status 未豁免，强制改密期间应 403 */
    req_make(&req, "GET", "/api/v1/system/status", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_EPERM_,
          "system/status 未豁免强制改密，403");

    CHECK(console_auth_set_password("ABCD1234", "NewPass@123") == HAL_OK, "改密");
    CHECK(do_login("admin", "NewPass@123", "192.168.50.11", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "以新口令重新登录");

    req_make(&req, "GET", "/api/v1/system/status", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK, "改密后 system/status 可访问");
    CHECK(strstr(body, "\"cpu_usage_pct\"") != NULL, "含 CPU 占用");
    CHECK(strstr(body, "-2147483648") == NULL, "温度不可用时不得返回 INT32_MIN，实际：%s", body);
    CHECK(strstr(body, "\"modules\":[") != NULL, "含模块列表");

    req_make(&req, "GET", "/api/v1/video/params", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK, "video/params 成功");
    CHECK(strstr(body, "\"channels\":[") != NULL, "含通道数组");
    CHECK(strstr(body, "\"running\":false") != NULL,
          "未 set_encoder/start 的通道 running=false（本测试未打开 hal video）");

    req_make(&req, "GET", "/api/v1/storage/info", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK, "storage/info 成功");
    CHECK(strstr(body, "\"present\":true") != NULL, "mock TF 卡存在");
    CHECK(strstr(body, "\"fs\":\"exfat\"") != NULL, "文件系统类型正确");

    /* resolution B 核心约束：reboot/reset 登记延后动作，而不是内联执行
       hal()->sys->reboot/factory_reset——该原语本身（回调确实等到响应字节
       交给内核之后才触发）已由 http_server_test 的真实 socket e2e 测试
       覆盖，这里只验证 console 侧确实调用了登记。 */
    req_make(&req, "POST", "/api/v1/system/reboot", NULL, cookie);
    deferred = false;
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), &deferred) == HAL_OK, "reboot 端点返回成功");
    CHECK(strstr(body, "\"code\":0") != NULL, "reboot 响应体正确，实际：%s", body);
    CHECK(deferred == true, "reboot 登记了延后动作而不是内联执行");

    req_make(&req, "GET", "/api/v1/system/reboot", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_EINVAL, "reboot 必须是 POST");

    req_make(&req, "GET", "/api/v1/bogus", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_ENODEV, "未知路径 404 语义");

    /* reset 放在最后：它会清除凭据与全部会话 */
    req_make(&req, "POST", "/api/v1/system/reset", "{\"keep_network\":true}", cookie);
    deferred = false;
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), &deferred) == HAL_OK, "reset 端点返回成功");
    CHECK(deferred == true, "reset 登记了延后动作而不是内联执行");
}

/** 恢复出厂必须清除本地管理员凭据、使旧会话失效，并回到"未激活"。 */
static void test_reset_wipes_credentials(void)
{
    http_req_t req;
    char body[4096], cookie[128], setck[128];
    bool must_change = false;

    SECTION("恢复出厂清除凭据");
    CHECK(cfg_init(NULL, "console_test_cfg_reset.json") == HAL_OK, "配置中心就绪");
    CHECK(console_api_register_rules() == HAL_OK, "登记规则");
    console_auth_reset_lockout();
    CHECK(console_auth_seed("Admin@12345", "admin", false) == HAL_OK, "已激活");
    CHECK(cfg_set_str("localUser.name", "admin") == HAL_OK, "激活写入用户名");
    CHECK(do_login("admin", "Admin@12345", "192.168.50.12", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "登录");

    req_make(&req, "POST", "/api/v1/system/reset", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_OK, "reset 成功");

    req_make(&req, "GET", "/api/v1/auth/state", NULL, NULL);
    CHECK(console_auth_test_dispatch(&req, "192.168.50.12", body, sizeof(body), setck, sizeof(setck)) == HAL_OK,
          "state 可读");
    CHECK(strstr(body, "\"activated\":false") != NULL, "出厂后回到未激活，实际：%s", body);

    req_make(&req, "GET", "/api/v1/system/status", NULL, cookie);
    CHECK(console_api_test_dispatch(&req, body, sizeof(body), NULL) == HAL_EUNAUTH_, "旧会话失效");

    cfg_deinit();
    remove("console_test_cfg_reset.json");
}

/**
 * 钉住 console_api_handler 内部"先 http_respond_json 入队、再
 * http_conn_defer_after_flush 登记"这个调用顺序本身。上面用
 * console_api_test_dispatch 做的 reboot/reset 测试都绕过了
 * console_api_handler（只测 api_dispatch 这个不碰 conn 的纯函数），
 * 颠倒 handler 内部那两行调用顺序不会被那些测试发现——评审实测过，
 * 对调之后全套测试仍然全绿。这里改用 console_api_test_full_handler
 * 真正走一遍 handler，配一个不含真实 socket 的测试连接
 * （http_ws_test_conn_new）：顺序正确时 http_conn_defer_after_flush
 * 应该成功（发送队列里已经有刚入队的响应）；顺序一旦被颠倒，登记那一刻
 * 发送队列还是空的，会返回 HAL_ESTATE，被下面的计数捕获。
 */
static void test_reboot_handler_order(void)
{
    http_req_t req;
    http_conn_t *conn;
    char cookie[128];
    bool must_change = false;
    unsigned before;

    SECTION("console_api_handler：必须先入队响应、再登记延后动作");
    console_auth_reset_lockout();
    CHECK(console_auth_seed("ABCD1234", NULL, true) == HAL_OK, "播种凭据（出厂态）");
    CHECK(console_auth_set_password("ABCD1234", "NewPass@123") == HAL_OK, "改密解除强制改密");
    CHECK(do_login("admin", "NewPass@123", "192.168.50.12", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "登录成功");

    conn = http_ws_test_conn_new(4096);
    CHECK(conn != NULL, "创建测试连接（不含真实 socket，足够承载 http_respond_json 的入队）");
    if (conn) {
        req_make(&req, "POST", "/api/v1/system/reboot", NULL, cookie);
        req.conn = conn;   /* 生产路径由 http_server.c 的 dispatch_one 回填，这里手动模拟 */

        before = console_api_test_defer_fail_count();
        CHECK(console_api_test_full_handler(&req) == 0, "reboot 请求处理成功（handler 返回 0，已自行响应）");
        CHECK(console_api_test_defer_fail_count() == before,
              "延后动作登记不应失败——若 console_api_handler 内部把 http_respond_json 与 "
              "http_conn_defer_after_flush 两行调用顺序颠倒，登记时发送队列还是空的，"
              "http_conn_defer_after_flush 会返回 HAL_ESTATE，这里的计数就会增加");

        http_ws_test_conn_free(conn);
    }
}

/* ========================================================================== */
/* Task 8：网络状态与 WiFi 配网                                                   */
/* ========================================================================== */

static void test_net_ap_decision(void)
{
    SECTION("AP 启动条件");
    /* 以太网 up 时绝不开 AP —— 有线场景直接用 IP 访问 */
    CHECK(console_should_start_ap(true,  false, false) == false, "以太网 up：不开 AP");
    CHECK(console_should_start_ap(true,  true,  false) == false, "以太网 up 且 WiFi 已配：不开 AP");
    /* 以太网 down 且 WiFi 未配置：开 AP */
    CHECK(console_should_start_ap(false, false, false) == true,  "无网且 WiFi 未配：开 AP");
    /* 以太网 down、WiFi 已配但连接失败：允许回落 AP（重新配网的救济路径） */
    CHECK(console_should_start_ap(false, true,  false) == true,  "WiFi 已配但未连上：回落 AP");
    /* WiFi 已连上：不开 AP */
    CHECK(console_should_start_ap(false, true,  true)  == false, "WiFi 已连上：不开 AP");
}

static void test_net_ap_ssid(void)
{
    char ssid[64];
    SECTION("AP SSID 生成");
    CHECK(console_ap_ssid("SN2026090700123456", ssid, sizeof(ssid)) == HAL_OK, "生成 SSID");
    CHECK(strcmp(ssid, "IPC-123456") == 0, "取序列号后 6 位：%s", ssid);
    /* 序列号过短时不得越界 */
    CHECK(console_ap_ssid("AB", ssid, sizeof(ssid)) == HAL_OK, "短序列号不崩");
    CHECK(strncmp(ssid, "IPC-", 4) == 0, "短序列号仍有前缀：%s", ssid);
}

/**
 * 评审 Important 3：出厂验证码只有 6 位，短于 wifi_ap_start 自己要求的
 * WPA2 下限 8 位，console_ap_psk_derive 派生一个合规的 PSK。补的这条
 * 单元测试是评审 fix round 完成之后自己发现的遗漏——该函数一开始是
 * static、只在从不启动的工作线程里被调用，等于没有测试能覆盖到它；
 * 现在导出为纯函数，直接测。
 */
static void test_ap_psk_derive(void)
{
    char psk[80];

    SECTION("AP PSK 派生（出厂验证码 → 合规 WPA2 口令）");

    /* 文档口径的 6 位验证码：加前缀 "IPC" 后是 9 位，已经 >= 8，不需要再补 */
    CHECK(console_ap_psk_derive("ABC123", psk, sizeof(psk)) == HAL_OK, "6 位验证码派生成功");
    CHECK(strcmp(psk, "IPCABC123") == 0, "派生结果精确等于 前缀+验证码，实际：%s", psk);
    CHECK(strlen(psk) >= 8 && strlen(psk) <= 63, "派生结果满足 WPA2 长度约束");

    /* 确定性：同一验证码任何时候算出的 PSK 必须完全相同（不能引入随机性，
       用户要照着标签把它敲进手机，设备重启后必须还是同一个值） */
    {
        char psk2[80];
        CHECK(console_ap_psk_derive("ABC123", psk2, sizeof(psk2)) == HAL_OK, "再算一次");
        CHECK(strcmp(psk, psk2) == 0, "同一验证码两次派生结果完全相同（确定性）");
    }

    /* 边界：验证码短到加前缀仍不足 8 位（超出文档口径的异常输入）——
       "IPC" + "12" = "IPC12" = 5 位，需要补 3 个 '0' 凑到 8 位 */
    CHECK(console_ap_psk_derive("12", psk, sizeof(psk)) == HAL_OK, "极短验证码也不能崩");
    CHECK(strcmp(psk, "IPC12000") == 0, "补齐到 8 位，精确等于 IPC12000，实际：%s", psk);
    CHECK(strlen(psk) == 8, "补齐后精确等于 8 位下限");

    /* 非法输入 */
    CHECK(console_ap_psk_derive(NULL, psk, sizeof(psk)) == HAL_EINVAL, "空指针验证码被拒");
    CHECK(console_ap_psk_derive("", psk, sizeof(psk)) == HAL_EINVAL, "空串验证码被拒");
    CHECK(console_ap_psk_derive("ABC123", NULL, sizeof(psk)) == HAL_EINVAL, "空输出缓冲被拒");
    CHECK(console_ap_psk_derive("ABC123", psk, 0) == HAL_EINVAL, "零容量输出缓冲被拒");
}

/**
 * 评审 fix round 2：Important 1 的周期复查/宽限期逻辑原来整段焊在
 * net_apply_ap_decision 里，只有工作线程真正跑起来才会被执行到，而测试
 * 套件从不启动工作线程，等于零覆盖。把"宽限期是否已过、判定结果是什么"
 * 抽成不碰 HAL/全局状态的纯函数 console_net_ap_decide 后，用构造时间戳
 * 直接做 KAT。
 */
static void test_net_ap_decide_grace_period(void)
{
    bool grace_active;
    uint64_t grace_started_us;
    console_net_ap_action_t action;

    SECTION("AP 决策纯函数：宽限期状态机（console_net_ap_decide）");

    /*
     * 评审 fix round 3（Minor 3，宽限期哨兵撞车）：*grace_started_us==0
     * 曾经被当作"未在宽限期"的哨兵，但 os_monotonic_us() 理论上可能恰好
     * 返回 0，与合法时间戳撞车——这是本文件已经栽过一次的坑（http_server.c
     * 的 epfd 静态零初始化）。改成独立的 grace_active 布尔位后，
     * grace_started_us 的旧值在"未激活"状态下没有意义，也不强制清零，
     * 所以下面的断言只认 grace_active，不再断言 grace_started_us 变成 0。
     */

    /* 以太网 up：不管其余条件、也不管宽限期是否正在计时，直接给出"不该有
       AP"的结论，且宽限期状态被清除（伪造一个"正在计时"的状态验证会被
       清除，而不是巧合本来就是未激活）。 */
    grace_active = true;
    grace_started_us = 12345;
    action = console_net_ap_decide(true, true, false, false, 1000000, &grace_active, &grace_started_us);
    CHECK(action == CONSOLE_NET_AP_ACTION_NONE, "eth up 且当前不是 AP：无动作，实际=%d", (int)action);
    CHECK(grace_active == false, "eth up 时宽限期状态被清除（grace_active=false）");

    grace_active = true;
    grace_started_us = 12345;
    action = console_net_ap_decide(true, true, false, true, 1000000, &grace_active, &grace_started_us);
    CHECK(action == CONSOLE_NET_AP_ACTION_STOP, "eth up 且当前是 AP：应该关闭，实际=%d", (int)action);
    CHECK(grace_active == false, "eth up 时宽限期状态被清除（grace_active=false）");

    /* WiFi 已连上：同理不该有 AP */
    grace_active = true;
    grace_started_us = 999;
    action = console_net_ap_decide(false, true, true, true, 1000000, &grace_active, &grace_started_us);
    CHECK(action == CONSOLE_NET_AP_ACTION_STOP, "wifi 已连上且当前是 AP：应该关闭");
    CHECK(grace_active == false, "wifi 已连上时宽限期状态被清除");

    /* 从未配置过 WiFi：没有什么好等的，没有宽限期，立即开 */
    grace_active = false;
    grace_started_us = 0;
    action = console_net_ap_decide(false, false, false, false, 1000000, &grace_active, &grace_started_us);
    CHECK(action == CONSOLE_NET_AP_ACTION_START, "从未配置 WiFi：立即开 AP，没有宽限期");
    CHECK(grace_active == false, "从未配置过时不应该进入宽限期状态");

    /* 已配置但未连上：宽限期生效，首次进入时记录计时起点 */
    grace_active = false;
    grace_started_us = 0;
    action = console_net_ap_decide(false, true, false, false, 1000000, &grace_active, &grace_started_us);
    CHECK(action == CONSOLE_NET_AP_ACTION_NONE, "已配置但未连上：宽限期内不开 AP");
    CHECK(grace_active == true, "首次进入宽限期，状态应被置为激活");
    CHECK(grace_started_us == 1000000, "宽限期计时起点精确等于首次进入该状态的时刻，实际=%llu",
          (unsigned long long)grace_started_us);

    /* 宽限期内再次调用（差 1 微秒到 15 秒）：计时起点不变，仍不开——
       若实现没有正确记住起点、每次调用都重新起算，这条会一直是 NONE 测不出
       区别；真正的证伪点在下面"恰好到期"那一条。 */
    action = console_net_ap_decide(false, true, false, false, 1000000 + 14999999, &grace_active, &grace_started_us);
    CHECK(action == CONSOLE_NET_AP_ACTION_NONE, "宽限期即将结束但还没到：仍不开 AP");
    CHECK(grace_active == true && grace_started_us == 1000000,
          "宽限期计时起点不会被后续调用重置，实际 active=%d started=%llu",
          (int)grace_active, (unsigned long long)grace_started_us);

    /* 宽限期恰好结束（15 秒整）：开 AP——如果边界判断写成 <= 而不是 <
       （或反过来该开的时候没开），这条会直接证伪 */
    action = console_net_ap_decide(false, true, false, false, 1000000 + 15000000, &grace_active, &grace_started_us);
    CHECK(action == CONSOLE_NET_AP_ACTION_START, "宽限期恰好结束（15 秒整）：开 AP");

    /* 宽限期结束后，如果已经是 AP 模式，不应该重复"开"（返回 NONE） */
    action = console_net_ap_decide(false, true, false, true, 1000000 + 20000000, &grace_active, &grace_started_us);
    CHECK(action == CONSOLE_NET_AP_ACTION_NONE, "已经是 AP 模式时不重复开启");

    /* 宽限期重新计时：先进入宽限期 → wifi 连上重置 → 断开后重新进入，
       计时起点必须是新的时刻，不是旧的——证明"离开状态即重置"真的生效，
       而不是巧合还没到期。 */
    {
        bool g2_active = false;
        uint64_t g2_started = 0;

        action = console_net_ap_decide(false, true, false, false, 5000000, &g2_active, &g2_started);
        CHECK(action == CONSOLE_NET_AP_ACTION_NONE, "第一次进入宽限期");
        CHECK(g2_active == true && g2_started == 5000000,
              "计时起点为 5000000，实际 active=%d started=%llu",
              (int)g2_active, (unsigned long long)g2_started);

        action = console_net_ap_decide(false, true, true, false, 6000000, &g2_active, &g2_started);
        CHECK(action == CONSOLE_NET_AP_ACTION_NONE, "wifi 连上，不该开 AP（本来就没在 AP 模式）");
        CHECK(g2_active == false, "wifi 连上后宽限期状态被重置为未激活");

        action = console_net_ap_decide(false, true, false, false, 7000000, &g2_active, &g2_started);
        CHECK(action == CONSOLE_NET_AP_ACTION_NONE, "重新进入宽限期");
        CHECK(g2_active == true && g2_started == 7000000,
              "重新进入宽限期后计时起点是新的时刻(7000000)而不是旧的(5000000)——"
              "证明 wifi 连上那次真的重置了计时，而不是巧合还没到期，实际 active=%d started=%llu",
              (int)g2_active, (unsigned long long)g2_started);
    }

    /* 非法输入：grace_active/grace_started_us 任一为 NULL 时安全返回 NONE，
       不崩溃——两个指针各自单独测，确认函数是"任一为 NULL 就短路"而不是
       只检查了其中一个。 */
    {
        bool dummy_active = false;
        uint64_t dummy_started = 1000000;
        CHECK(console_net_ap_decide(false, true, false, false, 1000000, NULL, &dummy_started) ==
              CONSOLE_NET_AP_ACTION_NONE, "grace_active 为 NULL 时安全返回 NONE");
        CHECK(console_net_ap_decide(false, true, false, false, 1000000, &dummy_active, NULL) ==
              CONSOLE_NET_AP_ACTION_NONE, "grace_started_us 为 NULL 时安全返回 NONE");
    }
}

static void test_net_ap_recheck_due(void)
{
    SECTION("AP 周期复查纯函数：边界值（console_net_ap_recheck_due）");
    CHECK(console_net_ap_recheck_due(1000000, 1000000 + 4999999, 5000) == false,
          "差一点点没到 5 秒：还不到复查时间");
    CHECK(console_net_ap_recheck_due(1000000, 1000000 + 5000000, 5000) == true,
          "恰好 5 秒：到复查时间");
    CHECK(console_net_ap_recheck_due(1000000, 1000000 + 5000001, 5000) == true,
          "超过 5 秒：到复查时间");
}

static void test_captive_portal(void)
{
    SECTION("Captive Portal 探测");
    /* AP 模式下这些路径应重定向，让手机自动弹出配网页 */
    CHECK(console_is_captive_probe("/generate_204") == true, "Android 探测");
    CHECK(console_is_captive_probe("/hotspot-detect.html") == true, "iOS 探测");
    CHECK(console_is_captive_probe("/api/v1/config") == false, "普通路径不算探测");
}

/* ---- DHCP：报文解析/构造/租约表 —— 纯函数，用已知/构造字节向量精确核对 ---- */

/** 构造一个真实形状的 DHCPDISCOVER 报文：236 字节定长头 + 4 字节 magic cookie +
 *  选项 53(DISCOVER)/55(参数请求列表)/255(end)，共 249 字节。 */
static void dhcp_test_build_discover(uint8_t *buf, size_t cap, size_t *len)
{
    (void)cap;
    memset(buf, 0, 249);
    buf[0] = 1; buf[1] = 1; buf[2] = 6; buf[3] = 0;                 /* op/htype/hlen/hops */
    buf[4] = 0x12; buf[5] = 0x34; buf[6] = 0x56; buf[7] = 0x78;     /* xid */
    buf[10] = 0x80; buf[11] = 0x00;                                 /* flags: broadcast */
    buf[28] = 0xAA; buf[29] = 0xBB; buf[30] = 0xCC;                 /* chaddr */
    buf[31] = 0xDD; buf[32] = 0xEE; buf[33] = 0xFF;
    buf[236] = 0x63; buf[237] = 0x82; buf[238] = 0x53; buf[239] = 0x63;   /* magic cookie */
    buf[240] = 53; buf[241] = 1; buf[242] = 1;                      /* 选项53=DISCOVER(1) */
    buf[243] = 55; buf[244] = 3; buf[245] = 1; buf[246] = 3; buf[247] = 6; /* 参数请求列表 */
    buf[248] = 255;                                                 /* end */
    *len = 249;
}

static void test_dhcp_parse_discover(void)
{
    uint8_t pkt[300];
    size_t len;
    console_dhcp_msg_t msg;

    SECTION("DHCP 报文解析：DISCOVER");
    dhcp_test_build_discover(pkt, sizeof(pkt), &len);
    CHECK(console_dhcp_parse(pkt, len, &msg) == HAL_OK, "解析 DISCOVER 成功");
    CHECK(msg.op == 1, "op=BOOTREQUEST(1)，实际=%u", msg.op);
    CHECK(msg.htype == 1 && msg.hlen == 6, "htype/hlen 精确匹配");
    CHECK(msg.xid == 0x12345678u, "xid 精确匹配：0x%08X", (unsigned)msg.xid);
    CHECK(msg.flags == 0x8000u, "广播标志位保留：0x%04X", (unsigned)msg.flags);
    CHECK(memcmp(msg.chaddr, "\xAA\xBB\xCC\xDD\xEE\xFF", 6) == 0, "chaddr 精确匹配");
    CHECK(msg.msg_type == CONSOLE_DHCP_MSG_DISCOVER, "msg_type=DISCOVER，实际=%u", msg.msg_type);
    CHECK(msg.requested_ip == 0, "DISCOVER 未带选项 50，requested_ip=0");
}

static void test_dhcp_parse_request_with_options(void)
{
    uint8_t pkt[300];
    console_dhcp_msg_t msg;
    size_t len = 256;

    SECTION("DHCP 报文解析：REQUEST 带选项 50(requested_ip)/54(server_id)");
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 1; pkt[1] = 1; pkt[2] = 6; pkt[3] = 0;
    pkt[4] = 0xCA; pkt[5] = 0xFE; pkt[6] = 0xBA; pkt[7] = 0xBE;   /* xid */
    pkt[28] = 0x02; pkt[29] = 0x00; pkt[30] = 0x00;               /* chaddr */
    pkt[31] = 0xCA; pkt[32] = 0xFE; pkt[33] = 0x02;
    pkt[236] = 0x63; pkt[237] = 0x82; pkt[238] = 0x53; pkt[239] = 0x63;
    pkt[240] = 53; pkt[241] = 1; pkt[242] = 3;                                        /* REQUEST */
    pkt[243] = 50; pkt[244] = 4; pkt[245] = 192; pkt[246] = 168; pkt[247] = 169; pkt[248] = 101;
    pkt[249] = 54; pkt[250] = 4; pkt[251] = 192; pkt[252] = 168; pkt[253] = 169; pkt[254] = 1;
    pkt[255] = 255;

    CHECK(console_dhcp_parse(pkt, len, &msg) == HAL_OK, "解析 REQUEST 成功");
    CHECK(msg.msg_type == CONSOLE_DHCP_MSG_REQUEST, "msg_type=REQUEST，实际=%u", msg.msg_type);
    CHECK(msg.requested_ip == 0xC0A8A965u,
          "requested_ip 精确匹配 192.168.169.101：0x%08X", (unsigned)msg.requested_ip);
    CHECK(memcmp(msg.chaddr, "\x02\x00\x00\xCA\xFE\x02", 6) == 0, "chaddr 精确匹配");
}

static void test_dhcp_parse_malformed(void)
{
    uint8_t pkt[300];
    console_dhcp_msg_t msg;

    SECTION("DHCP 报文解析：截断/畸形输入必须安全拒绝，绝不越界读");

    /* 太短：连定长头+cookie都不够 */
    memset(pkt, 0, sizeof(pkt));
    CHECK(console_dhcp_parse(pkt, 100, &msg) == HAL_EINVAL, "过短报文被拒");

    /* magic cookie 错误 */
    memset(pkt, 0, sizeof(pkt));
    CHECK(console_dhcp_parse(pkt, 240, &msg) == HAL_ECORRUPT, "魔数错误被拒");

    /* 选项声称的长度超出报文剩余字节 */
    memset(pkt, 0, sizeof(pkt));
    pkt[236] = 0x63; pkt[237] = 0x82; pkt[238] = 0x53; pkt[239] = 0x63;
    pkt[240] = 53; pkt[241] = 200;   /* 声称 200 字节的选项体，报文到此只剩 0 字节 */
    CHECK(console_dhcp_parse(pkt, 242, &msg) == HAL_ECORRUPT, "选项长度越界被拒，不越界读");

    /* 长度字节自身就越界（code 字节后已经没有 length 字节了） */
    memset(pkt, 0, sizeof(pkt));
    pkt[236] = 0x63; pkt[237] = 0x82; pkt[238] = 0x53; pkt[239] = 0x63;
    pkt[240] = 53;
    CHECK(console_dhcp_parse(pkt, 241, &msg) == HAL_ECORRUPT, "选项长度字节本身越界被拒");

    CHECK(console_dhcp_parse(NULL, 300, &msg) == HAL_EINVAL, "空指针被拒");
}

static void test_dhcp_build_offer_exact_bytes(void)
{
    uint8_t pkt[300], reply[300];
    size_t len, out_len = 0;
    console_dhcp_msg_t req;

    SECTION("DHCP 应答构造：OFFER 精确字节");
    dhcp_test_build_discover(pkt, sizeof(pkt), &len);
    CHECK(console_dhcp_parse(pkt, len, &req) == HAL_OK, "先解析出请求");

    CHECK(console_dhcp_build_reply(&req, CONSOLE_DHCP_MSG_OFFER,
                                   0xC0A8A964u /* 192.168.169.100 */,
                                   0xC0A8A901u /* 192.168.169.1 */,
                                   CONSOLE_DHCP_LEASE_S, reply, sizeof(reply), &out_len) == HAL_OK,
          "构造 OFFER 成功");
    CHECK(out_len == 268, "OFFER 报文长度精确为 268 字节，实际 %zu", out_len);
    CHECK(reply[0] == 2 && reply[1] == 1 && reply[2] == 6 && reply[3] == 0, "op/htype/hlen/hops 正确");
    CHECK(memcmp(reply + 4, "\x12\x34\x56\x78", 4) == 0, "xid 回显精确匹配");
    CHECK(reply[10] == 0x80 && reply[11] == 0x00, "flags 回显");
    CHECK(memcmp(reply + 16, "\xC0\xA8\xA9\x64", 4) == 0, "yiaddr 精确等于分配的 192.168.169.100");
    CHECK(memcmp(reply + 28, "\xAA\xBB\xCC\xDD\xEE\xFF", 6) == 0, "chaddr 回显精确匹配");
    CHECK(memcmp(reply + 236, "\x63\x82\x53\x63", 4) == 0, "magic cookie 正确");
    CHECK(reply[240] == 53 && reply[241] == 1 && (unsigned char)reply[242] == 2, "选项53=OFFER(2)");
    CHECK(reply[243] == 51 && reply[244] == 4, "选项51(租期)长度=4");
    CHECK(memcmp(reply + 245, "\x00\x00\x1c\x20", 4) == 0, "租期精确等于 7200 秒(0x1C20)");
    CHECK((unsigned char)reply[249] == 54 && reply[250] == 4, "选项54=server id");
    CHECK(memcmp(reply + 251, "\xC0\xA8\xA9\x01", 4) == 0, "server id 精确等于 192.168.169.1");
    CHECK(reply[255] == 1 && reply[256] == 4, "选项1=子网掩码");
    CHECK(memcmp(reply + 257, "\xFF\xFF\xFF\x00", 4) == 0, "子网掩码精确等于 255.255.255.0");
    CHECK(reply[261] == 3 && reply[262] == 4, "选项3=网关");
    CHECK(memcmp(reply + 263, "\xC0\xA8\xA9\x01", 4) == 0, "网关精确等于服务端自身地址");
    CHECK((unsigned char)reply[267] == 0xFF, "报文以 End(0xFF) 结束");
}

static void test_dhcp_build_nak_exact_bytes(void)
{
    uint8_t pkt[300], reply[300];
    size_t len, out_len = 0;
    console_dhcp_msg_t req;

    SECTION("DHCP 应答构造：NAK 精确字节（不含租期/地址选项）");
    dhcp_test_build_discover(pkt, sizeof(pkt), &len);
    console_dhcp_parse(pkt, len, &req);

    CHECK(console_dhcp_build_reply(&req, CONSOLE_DHCP_MSG_NAK, 0, 0xC0A8A901u, 0,
                                   reply, sizeof(reply), &out_len) == HAL_OK, "构造 NAK 成功");
    CHECK(out_len == 250, "NAK 报文长度精确为 250 字节（无 51/1/3 三个选项），实际 %zu", out_len);
    CHECK(memcmp(reply + 16, "\x00\x00\x00\x00", 4) == 0, "NAK 的 yiaddr 必须是 0.0.0.0，不得分配地址");
    CHECK(reply[240] == 53 && reply[241] == 1 && (unsigned char)reply[242] == 6, "选项53=NAK(6)");
    CHECK((unsigned char)reply[243] == 54 && reply[244] == 4, "选项54=server id 紧随其后（无51）");
    CHECK(memcmp(reply + 245, "\xC0\xA8\xA9\x01", 4) == 0, "server id 精确匹配");
    CHECK((unsigned char)reply[249] == 0xFF, "NAK 以 End 结束，无子网掩码/网关选项");
}

static void test_dhcp_build_buffer_too_small(void)
{
    uint8_t pkt[300], reply[10];
    size_t len, out_len = 999;
    console_dhcp_msg_t req;

    SECTION("DHCP 应答构造：缓冲不足必须拒绝，不写半截报文");
    dhcp_test_build_discover(pkt, sizeof(pkt), &len);
    console_dhcp_parse(pkt, len, &req);
    CHECK(console_dhcp_build_reply(&req, CONSOLE_DHCP_MSG_OFFER, 1, 2, 3, reply, sizeof(reply), &out_len)
          == HAL_ENOMEM, "10 字节缓冲装不下 268 字节的 OFFER，返回 ENOMEM");
}

static void test_dhcp_lease_table(void)
{
    console_dhcp_lease_table_t t, full, t2;
    uint8_t mac1[6] = { 0,0,0,0,0,1 };
    uint8_t mac2[6] = { 0,0,0,0,0,2 };
    uint32_t host = 0, host1 = 0;
    uint32_t i;

    SECTION("DHCP 租约表：分配/续租/耗尽/过期回收/释放");
    console_dhcp_lease_table_init(&t);

    CHECK(console_dhcp_lease_acquire(&t, mac1, 1000, &host) == HAL_OK, "首次分配成功");
    CHECK(host == CONSOLE_DHCP_POOL_START, "首个地址精确等于池起始 .%u", (unsigned)CONSOLE_DHCP_POOL_START);
    host1 = host;

    CHECK(console_dhcp_lease_acquire(&t, mac1, 1500, &host) == HAL_OK, "同一 MAC 续租");
    CHECK(host == host1, "续租必须拿回同一地址，而不是新分配一个");

    CHECK(console_dhcp_lease_acquire(&t, mac2, 1000, &host) == HAL_OK, "第二个 MAC 分配");
    CHECK(host == CONSOLE_DHCP_POOL_START + 1,
          "第二个地址精确等于池起始+1（证明真的递增分配，不是巧合）");

    /* 耗尽整个池：CONSOLE_DHCP_POOL_SIZE 个不同 MAC 依次分配 */
    console_dhcp_lease_table_init(&full);
    {
        uint32_t used = 0;
        uint8_t mac[6] = { 0,0,0,0,1,0 };
        for (i = 0; i < CONSOLE_DHCP_POOL_SIZE; i++) {
            mac[5] = (uint8_t)i;
            if (console_dhcp_lease_acquire(&full, mac, 2000, &host) == HAL_OK) used++;
        }
        CHECK(used == CONSOLE_DHCP_POOL_SIZE, "池内 %u 个地址全部分配成功，实际 %u",
              (unsigned)CONSOLE_DHCP_POOL_SIZE, (unsigned)used);

        mac[5] = (uint8_t)(CONSOLE_DHCP_POOL_SIZE & 0xFF);   /* 第 102 个不同的 MAC，池外新客户端 */
        CHECK(console_dhcp_lease_acquire(&full, mac, 2000, &host) == HAL_ENOMEM,
              "池已耗尽时新 MAC 分配失败，返回 ENOMEM（不驱逐活跃租约）");
        CHECK(console_dhcp_lease_acquire(&full, mac, 9000, &host) == HAL_ENOMEM,
              "全部租约到期(9200)之前重试仍然失败（9000<9200）");
        CHECK(console_dhcp_lease_acquire(&full, mac, 9300, &host) == HAL_OK,
              "全部租约过期后(9300>9200)，新 MAC 分配成功——证明过期回收真的生效");
    }

    /* 释放测试：新表，填满后释放一个特定 MAC，验证新 MAC 精确复用被释放的那个地址 */
    console_dhcp_lease_table_init(&t2);
    {
        uint8_t victim_mac[6] = { 0,0,0,0,1,5 };
        uint8_t newcomer[6] = { 9,9,9,9,9,9 };
        uint32_t victim_host = CONSOLE_DHCP_POOL_START + 5;

        for (i = 0; i < CONSOLE_DHCP_POOL_SIZE; i++) {
            uint8_t m[6] = { 0,0,0,0,1,0 };
            m[5] = (uint8_t)i;
            console_dhcp_lease_acquire(&t2, m, 100, &host);
        }
        CHECK(console_dhcp_lease_release(&t2, victim_mac) == HAL_OK, "释放指定 MAC 的租约");
        CHECK(console_dhcp_lease_acquire(&t2, newcomer, 200, &host) == HAL_OK, "释放后新 MAC 分配成功");
        CHECK(host == victim_host, "新 MAC 精确复用被释放的那个地址：.%u（预期 .%u）",
              (unsigned)host, (unsigned)victim_host);
        CHECK(console_dhcp_lease_release(&t2, victim_mac) == HAL_ENODEV,
              "重复释放同一 MAC 返回 ENODEV（已不在表中）");
    }
}

/**
 * 评审 Minor #4：DISCOVER 不应立即提交完整的 2 小时租约——只发 DISCOVER
 * 不发 REQUEST 的客户端（或伪造 MAC 的攻击者）会长期占满整个地址池。
 * console_dhcp_lease_offer 用短得多的 offer_ttl 做临时预留，这里证明它
 * 真的比 console_dhcp_lease_acquire 的完整租期短得多、且到期后会被回收。
 */
static void test_dhcp_lease_offer_short_ttl(void)
{
    console_dhcp_lease_table_t full;
    uint8_t mac[6] = { 7,7,7,7,7,7 };
    uint32_t host = 0, i;

    SECTION("DHCP 租约表：DISCOVER 短时预留（offer）到期比正式租约快得多");
    console_dhcp_lease_table_init(&full);

    /* 用正式 acquire（完整 2 小时租期）填满除一个槽位外的整个池 */
    for (i = 0; i < CONSOLE_DHCP_POOL_SIZE - 1; i++) {
        uint8_t m[6] = { 0,0,0,0,2,0 };
        uint32_t h = 0;
        m[5] = (uint8_t)i;
        CHECK(console_dhcp_lease_acquire(&full, m, 1000, &h) == HAL_OK, "预先填满第 %u 个槽位", (unsigned)i);
    }
    /* 最后一个槽位用短时预留（offer，ttl=30s）而不是正式 acquire */
    CHECK(console_dhcp_lease_offer(&full, mac, 1000, 30, &host) == HAL_OK, "DISCOVER 短时预留最后一个槽位");

    {
        uint8_t newcomer[6] = { 8,8,8,8,8,8 };
        uint32_t h2 = 0;
        CHECK(console_dhcp_lease_offer(&full, newcomer, 1010, 30, &h2) == HAL_ENOMEM,
              "预留未过期(1010<1000+30)时池已满（100 正式 + 1 预留），新 MAC 预留失败");

        /* 预留在 1000+30=1030 到期；正式租约在 1000+7200=8200 到期，远晚于
           前者。1031 时只有"预留"那一个槽位过期，新 MAC 应该能拿到它——
           如果 offer 的 offer_ttl_s 参数没有生效（比如被误实现成固定 2
           小时），这里会仍然 ENOMEM，直接证伪。 */
        CHECK(console_dhcp_lease_offer(&full, newcomer, 1031, 30, &h2) == HAL_OK,
              "预留过期(1031>1030)后，其余 100 个正式租约仍未到期(8200)，"
              "但被回收的这一个槽位足够新 MAC 使用——证明 offer 的短 ttl 真的生效");
        CHECK(h2 == host, "新 MAC 精确复用了刚过期的那个预留地址");
    }
}

/**
 * 评审 Minor #4：DISCOVER 之后及时收到 REQUEST（调用 console_dhcp_lease_
 * acquire）应该把短时预留转正为完整租期，而不是任由它按 offer 的短 ttl
 * 过期——否则一个正常完成握手的客户端也会在几十秒后"莫名其妙"丢失地址。
 */
static void test_dhcp_lease_offer_then_request_upgrades_to_full_lease(void)
{
    console_dhcp_lease_table_t t;
    uint8_t mac[6] = { 9,9,9,9,9,9 };
    uint32_t host_offer = 0, host_request = 0, host_recheck = 0;

    SECTION("DHCP 租约表：DISCOVER 预留 + 及时 REQUEST 转为正式租约");
    console_dhcp_lease_table_init(&t);

    CHECK(console_dhcp_lease_offer(&t, mac, 1000, 30, &host_offer) == HAL_OK, "DISCOVER 预留");
    CHECK(console_dhcp_lease_acquire(&t, mac, 1010, &host_request) == HAL_OK,
          "1010 时收到 REQUEST，调用 acquire 确认租约");
    CHECK(host_request == host_offer, "REQUEST 确认的地址与 DISCOVER 预留的地址精确相同");

    /* 若 acquire 没有正确续成完整租期（比如仍然沿用 offer 留下的 30 秒
       到期时间），1035（>1000+30，但远早于 1010+7200）时这个地址应该已经
       "过期"、新查询会分配到一个新槽位而不是命中原槽位。用 offer 而不是
       acquire 去做这次复查，专门验证"即使不是走 acquire 路径，记录也还在"
       （即真的续成了长租期，而不是恰好又被同一路径的续租逻辑掩盖）。 */
    CHECK(console_dhcp_lease_offer(&t, mac, 1035, 30, &host_recheck) == HAL_OK,
          "REQUEST 之后很久（超过原 offer 的 30 秒窗口）仍能查询到该 MAC 的记录");
    CHECK(host_recheck == host_offer,
          "记录仍然存在且地址不变——证明 acquire 把 30 秒的临时预留正确续成了完整租期，"
          "而不是任由它按原定的 30 秒过期");
}

/**
 * 评审 Minor #6：ACK/NAK 判定、want_ip 拼装、DISCOVER 是否立即提交租约是
 * 协议策略，不是收发字节的胶水，抽成纯函数 console_dhcp_decide 并做 KAT，
 * 把 socket 胶水层（六节）的未验证面缩到最小。
 */
static void test_dhcp_decide_discover_and_request(void)
{
    console_dhcp_lease_table_t t;
    console_dhcp_msg_t req;
    uint8_t type = 0;
    uint32_t ip = 0;
    bool replied;

    SECTION("DHCP 协议决策：console_dhcp_decide 的 DISCOVER/REQUEST/RELEASE 分支");
    console_dhcp_lease_table_init(&t);

    memset(&req, 0, sizeof(req));
    memcpy(req.chaddr, "\xAA\xBB\xCC\xDD\xEE\xFF", 6);
    req.msg_type = CONSOLE_DHCP_MSG_DISCOVER;
    replied = console_dhcp_decide(&t, &req, 1000, 30, CONSOLE_DHCP_LEASE_S, 0xC0A8A901u, &type, &ip);
    CHECK(replied == true, "DISCOVER 应该有应答");
    CHECK(type == CONSOLE_DHCP_MSG_OFFER, "应答类型是 OFFER，实际=%u", type);
    CHECK(ip == 0xC0A8A964u, "分配地址精确等于 192.168.169.100，实际=0x%08X", (unsigned)ip);

    /* 紧接着的 REQUEST：requested_ip 与刚分配的地址一致 → ACK */
    req.msg_type = CONSOLE_DHCP_MSG_REQUEST;
    req.requested_ip = ip;
    replied = console_dhcp_decide(&t, &req, 1005, 30, CONSOLE_DHCP_LEASE_S, 0xC0A8A901u, &type, &ip);
    CHECK(replied == true, "REQUEST 应该有应答");
    CHECK(type == CONSOLE_DHCP_MSG_ACK, "应答类型是 ACK，实际=%u", type);
    CHECK(ip == 0xC0A8A964u, "ACK 分配的地址与 OFFER 一致");

    /*
     * REQUEST 里带一个跟服务端记录不一致的 requested_ip → NAK。
     * 评审 fix round 3：这条测的是"表里已有这个 MAC 的记录，但 requested_ip
     * 对不上"分支，因此必须先让该 MAC 走一次 DISCOVER 在表里落地一条记录，
     * 再发不一致的 REQUEST——如果跳过 DISCOVER 直接发 REQUEST，命中的其实是
     * 下面"裸 REQUEST、未知 MAC"分支（同样回 NAK，但成因完全不同：一个是
     * "有记录但地址不匹配"，一个是"根本没有记录"），会把两件事测混、且在
     * Issue 4 修复后这条本该测的分支反而没被覆盖到。
     */
    {
        console_dhcp_msg_t req2;
        uint32_t offered_ip = 0;

        memset(&req2, 0, sizeof(req2));
        memcpy(req2.chaddr, "\x01\x02\x03\x04\x05\x06", 6);
        req2.msg_type = CONSOLE_DHCP_MSG_DISCOVER;
        replied = console_dhcp_decide(&t, &req2, 1010, 30, CONSOLE_DHCP_LEASE_S, 0xC0A8A901u, &type, &offered_ip);
        CHECK(replied == true && type == CONSOLE_DHCP_MSG_OFFER,
              "先用 DISCOVER 让该 MAC 在租约表里留下一条记录，为下面的 REQUEST 铺垫");

        req2.msg_type = CONSOLE_DHCP_MSG_REQUEST;
        req2.requested_ip = 0xC0A8A9C8u;   /* 192.168.169.200，与实际分配的 offered_ip 不一致 */
        replied = console_dhcp_decide(&t, &req2, 1011, 30, CONSOLE_DHCP_LEASE_S, 0xC0A8A901u, &type, &ip);
        CHECK(replied == true, "requested_ip 不匹配时仍应回复（NAK）");
        CHECK(type == CONSOLE_DHCP_MSG_NAK, "应答类型是 NAK，实际=%u", type);
        CHECK(ip == 0, "NAK 的 your_ip 必须是 0，不得暗示一个地址");

        console_dhcp_lease_release(&t, req2.chaddr);   /* 清理，避免占用下面地址池耗尽用例的槽位 */
    }

    /*
     * 评审 fix round 3 Issue 4：裸 REQUEST（没经过 DISCOVER，表里也没有这个
     * MAC 的既有记录）不应该凭空获得完整租约，应直接 NAK 且不得在租约表里
     * 留下任何新记录——这正是本轮要关闭的攻击面：伪造一批 MAC 跳过
     * DISCOVER、直接群发 REQUEST 抢占地址池。只看返回的 NAK 不足以证明
     * 没有分配，必须额外用 console_dhcp_lease_release 返回 HAL_ENODEV 来
     * 证明表里确实没有这个 MAC 的记录——如果 Issue 4 的修复被回退，这里会
     * 变成 HAL_OK（意外释放到了一条被凭空创建的租约），直接证伪。
     */
    {
        console_dhcp_msg_t req4;
        memset(&req4, 0, sizeof(req4));
        memcpy(req4.chaddr, "\x11\x22\x33\x44\x55\x66", 6);
        req4.msg_type = CONSOLE_DHCP_MSG_REQUEST;
        req4.requested_ip = 0;
        replied = console_dhcp_decide(&t, &req4, 1012, 30, CONSOLE_DHCP_LEASE_S, 0xC0A8A901u, &type, &ip);
        CHECK(replied == true, "裸 REQUEST（无既有记录）仍应回复（NAK）");
        CHECK(type == CONSOLE_DHCP_MSG_NAK, "裸 REQUEST、未知 MAC：应答类型必须是 NAK，实际=%u", type);
        CHECK(ip == 0, "NAK 的 your_ip 必须是 0");
        CHECK(console_dhcp_lease_release(&t, req4.chaddr) == HAL_ENODEV,
              "裸 REQUEST 不应该在租约表里留下任何记录：释放一个从未被分配过的 MAC 必须是 "
              "HAL_ENODEV；若这里意外拿到 HAL_OK 说明裸 REQUEST 仍然凭空创建了租约（回归到"
              "修复前的地址池耗尽漏洞）");
    }

    /* RELEASE：不回复，且租约确实被释放 */
    req.msg_type = CONSOLE_DHCP_MSG_RELEASE;
    replied = console_dhcp_decide(&t, &req, 1020, 30, CONSOLE_DHCP_LEASE_S, 0xC0A8A901u, &type, &ip);
    CHECK(replied == false, "RELEASE 不应该有应答");
    CHECK(console_dhcp_lease_release(&t, req.chaddr) == HAL_ENODEV,
          "RELEASE 已经在 console_dhcp_decide 内部执行过一次，租约已不在表中，重复释放返回 ENODEV");

    /* 不认识的消息类型（如 INFORM）：不回复 */
    req.msg_type = CONSOLE_DHCP_MSG_INFORM;
    replied = console_dhcp_decide(&t, &req, 1030, 30, CONSOLE_DHCP_LEASE_S, 0xC0A8A901u, &type, &ip);
    CHECK(replied == false, "INFORM 等未实现的消息类型不回复");

    /* 地址池耗尽时 DISCOVER 不回复 */
    {
        console_dhcp_lease_table_t full;
        console_dhcp_msg_t req3;
        uint32_t i;
        console_dhcp_lease_table_init(&full);
        for (i = 0; i < CONSOLE_DHCP_POOL_SIZE; i++) {
            uint8_t m[6] = { 0,0,0,0,9,0 };
            uint32_t h = 0;
            m[5] = (uint8_t)i;
            console_dhcp_lease_acquire(&full, m, 1000, &h);
        }
        memset(&req3, 0, sizeof(req3));
        memcpy(req3.chaddr, "\x99\x99\x99\x99\x99\x99", 6);
        req3.msg_type = CONSOLE_DHCP_MSG_DISCOVER;
        replied = console_dhcp_decide(&full, &req3, 1000, 30, CONSOLE_DHCP_LEASE_S, 0xC0A8A901u, &type, &ip);
        CHECK(replied == false, "地址池耗尽时 DISCOVER 不回复（客户端会重试或超时放弃）");
    }
}

/* ---- REST 端点：/api/v1/net/{status,wifi/scan,wifi/connect} ---- */

static void test_net_init_and_routes(void)
{
    SECTION("网络子模块初始化与路由注册");
    CHECK(cfg_init(NULL, "console_test_cfg_net_init.json") == HAL_OK, "配置中心就绪");
    /*
     * 评审 Minor #7：只注册 /api/v1/net/ 一个前缀时，任何路径都只可能命中
     * 它，"精确核对命中的前缀"这条断言对"最长前缀真的优先"这件事零证明力
     * ——哪怕 http_route_match 退化成"先注册先赢"，因为压根没有第二个前缀
     * 参与竞争，断言照样通过。要真正验证生产环境下 /api/v1/net/status 会
     * 命中比 /api/v1/ 更长、更具体的 /api/v1/net/，必须让两个前缀同时
     * 注册。console_api_init()（Task 7）在整个测试二进制里也是唯一一次
     * 调用，与 console_net_init 各占一个路由槽（+ test_activation_bootstrap
     * 已经注册的 /api/v1/auth/，共 3/8，预算充足）。
     */
    CHECK(console_api_init() == HAL_OK, "console_api_init 成功（注册 /api/v1/，唯一一次）");
    CHECK(console_net_init() == HAL_OK, "console_net_init 成功（唯一一次）");
    {
        const char *matched_net = http_route_match("/api/v1/net/status");
        const char *matched_generic = http_route_match("/api/v1/config");
        /* 短路 && 避免 matched 为 NULL 时 strcmp 直接崩溃掉整个测试进程
           （本计划已因"/ 兜底使 !=NULL 检查零证明力"吃过亏，这里改成精确
           比较前缀字符串本身）。 */
        CHECK(matched_net != NULL && strcmp(matched_net, "/api/v1/net/") == 0,
              "/api/v1/net/status 命中更长的 /api/v1/net/ 前缀而不是 /api/v1/，实际：%s",
              matched_net ? matched_net : "(NULL)");
        CHECK(matched_generic != NULL && strcmp(matched_generic, "/api/v1/") == 0,
              "/api/v1/config 命中 /api/v1/ 前缀——证明两个前缀真的同时注册并生效，"
              "而不是 net 是唯一注册过的前缀，实际：%s",
              matched_generic ? matched_generic : "(NULL)");
    }
    cfg_deinit();
    remove("console_test_cfg_net_init.json");
}

static void test_net_status_endpoint(void)
{
    http_req_t req;
    char body[2048], cookie[128];
    bool must_change = false;
    int http_status = 0;

    SECTION("REST 网络状态端点 GET /api/v1/net/status");
    console_net_test_reset();
    console_auth_reset_lockout();
    CHECK(console_auth_seed("ABCD1234", NULL, true) == HAL_OK, "播种凭据（出厂态）");

    req_make(&req, "GET", "/api/v1/net/status", NULL, NULL);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_EUNAUTH_,
          "未登录访问 net/status 返回未登录");

    CHECK(do_login("admin", "ABCD1234", "192.168.60.10", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "登录成功（出厂态）");

    req_make(&req, "GET", "/api/v1/net/status", NULL, cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_EPERM_,
          "未改密时 net/status 403（未豁免强制改密，与 system/status 同类）");

    CHECK(console_auth_set_password("ABCD1234", "NewPass@123") == HAL_OK, "改密解除强制改密");
    CHECK(do_login("admin", "NewPass@123", "192.168.60.10", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "以新口令重新登录");

    /* 默认状态：mock 以太网恒 up、未开 AP、未连 WiFi → mode=eth。
       ip 自 HAL v1.2 起恒存在（hal_netif_status_t 新增字段，mock 的
       n_status 填了固定值）；ssid/rssi/last_error 仍然是不适用即省略，
       不硬凑假数据。 */
    req_make(&req, "GET", "/api/v1/net/status", NULL, cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_OK, "net/status 成功");
    CHECK(http_status == 200, "net/status 状态码 200，实际 %d", http_status);
    CHECK(strstr(body, "\"code\":0") != NULL, "响应含 code:0");
    CHECK(strstr(body, "\"mode\":\"eth\"") != NULL, "默认模式为 eth，实际：%s", body);
    CHECK(strstr(body, "\"mac\":\"02:00:00:CA:FE:01\"") != NULL,
          "eth 模式下 mac 精确等于 mock 以太网地址，实际：%s", body);
    CHECK(strstr(body, "\"ip\":\"192.168.1.100\"") != NULL,
          "eth 模式下 ip 精确等于 mock hal_netif_status_t.ip 的值（HAL v1.2 新增字段），实际：%s", body);
    CHECK(strstr(body, "\"ssid\"") == NULL, "eth 模式下不应出现 ssid 字段，实际：%s", body);
    CHECK(strstr(body, "\"rssi\"") == NULL, "eth 模式下不应出现 rssi 字段，实际：%s", body);
    CHECK(strstr(body, "\"last_error\"") == NULL, "未曾配网失败过，不应出现 last_error 字段，实际：%s", body);

    /* AP 模式：测试桩注入（工作线程未启动，无法真的走 wifi_ap_start），
       验证 ip/ssid 字段的拼装逻辑本身 */
    console_net_test_seed_ap("IPC-00CAFE");
    req_make(&req, "GET", "/api/v1/net/status", NULL, cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_OK,
          "AP 模式 net/status 成功");
    CHECK(strstr(body, "\"mode\":\"ap\"") != NULL, "模式为 ap，实际：%s", body);
    CHECK(strstr(body, "\"ip\":\"192.168.169.1\"") != NULL, "AP 网关地址精确匹配，实际：%s", body);
    CHECK(strstr(body, "\"ssid\":\"IPC-00CAFE\"") != NULL, "回显注入的 AP SSID，实际：%s", body);

    console_net_test_reset();
}

static void test_net_wifi_scan_endpoint(void)
{
    http_req_t req;
    char body[2048], cookie[128];
    bool must_change = false;
    int http_status = 0;

    SECTION("REST WiFi 扫描端点 GET /api/v1/net/wifi/scan");
    console_net_test_reset();
    console_auth_reset_lockout();
    CHECK(console_auth_seed("ABCD1234", NULL, true) == HAL_OK, "播种凭据（出厂态）");
    CHECK(console_auth_set_password("ABCD1234", "NewPass@123") == HAL_OK, "改密解除强制改密");
    CHECK(do_login("admin", "NewPass@123", "192.168.60.11", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "登录成功");

    req_make(&req, "GET", "/api/v1/net/wifi/scan", NULL, NULL);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_EUNAUTH_,
          "未登录访问 wifi/scan 返回未登录");

    req_make(&req, "POST", "/api/v1/net/wifi/scan", NULL, cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_EINVAL,
          "非 GET 方法被拒");

    /* 缓存命中：先注入一条结果，验证 200 + JSON 拼装精确 */
    console_net_test_seed_scan_result("HomeWiFi-5G", -55, 5180, (int)HAL_WIFI_SEC_WPA2);
    req_make(&req, "GET", "/api/v1/net/wifi/scan", NULL, cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_OK,
          "缓存命中时 wifi/scan 成功");
    CHECK(http_status == 200, "缓存命中时状态码 200，实际 %d", http_status);
    CHECK(strstr(body, "\"ssid\":\"HomeWiFi-5G\"") != NULL, "回显注入的 SSID，实际：%s", body);
    CHECK(strstr(body, "\"rssi\":-55") != NULL, "回显注入的 rssi，实际：%s", body);
    CHECK(strstr(body, "\"freq_mhz\":5180") != NULL, "回显注入的频段，实际：%s", body);
    CHECK(strstr(body, "\"security\":\"wpa2\"") != NULL, "安全类型精确映射为 wpa2，实际：%s", body);
    CHECK(strstr(body, "\"age_s\":0") != NULL, "刚注入的结果 age_s=0，实际：%s", body);

    /* 缓存未命中（reset 后）：handler 不会同步调用阻塞的 wifi_scan（本测试没有
       启动工作线程），只会投递任务并回 202，供前端轮询。 */
    console_net_test_reset();
    req_make(&req, "GET", "/api/v1/net/wifi/scan", NULL, cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_OK,
          "缓存未命中时投递扫描任务，dispatch 本身仍返回 HAL_OK");
    CHECK(http_status == 202, "缓存未命中时状态码 202，实际 %d", http_status);
    CHECK(strstr(body, "\"code\":0") != NULL, "202 响应体仍是 code:0 的正常 JSON，实际：%s", body);

    console_net_test_reset();
}

static void test_net_wifi_connect_endpoint(void)
{
    http_req_t req;
    char body[1024], cookie[128];
    bool must_change = false;
    int http_status = 0;

    SECTION("REST WiFi 配网提交端点 POST /api/v1/net/wifi/connect：参数校验与 202 立即响应");
    console_net_test_reset();
    console_auth_reset_lockout();
    CHECK(console_auth_seed("ABCD1234", NULL, true) == HAL_OK, "播种凭据（出厂态）");
    CHECK(console_auth_set_password("ABCD1234", "NewPass@123") == HAL_OK, "改密解除强制改密");
    CHECK(do_login("admin", "NewPass@123", "192.168.60.13", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "登录成功");

    req_make(&req, "POST", "/api/v1/net/wifi/connect", "{\"ssid\":\"Home\",\"psk\":\"12345678\"}", NULL);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_EUNAUTH_,
          "未登录提交配网返回未登录");

    req_make(&req, "GET", "/api/v1/net/wifi/connect", NULL, cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_EINVAL,
          "非 POST 方法被拒");

    req_make(&req, "POST", "/api/v1/net/wifi/connect", NULL, cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_EINVAL,
          "空 body 被拒");

    req_make(&req, "POST", "/api/v1/net/wifi/connect", "not-json", cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_EINVAL,
          "畸形 JSON 被拒");

    req_make(&req, "POST", "/api/v1/net/wifi/connect", "{\"psk\":\"12345678\"}", cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_EINVAL,
          "缺 ssid 被拒");

    req_make(&req, "POST", "/api/v1/net/wifi/connect", "{\"ssid\":\"Home\",\"psk\":\"123\"}", cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_EINVAL,
          "psk 短于 8 位被拒");

    {
        char long_body[128];
        /* 68 位纯数字口令：超过 WPA2 上限 63 位 */
        snprintf(long_body, sizeof(long_body), "{\"ssid\":\"Home\",\"psk\":\"%s\"}",
                 "1234567890123456789012345678901234567890123456789012345678901234567890");
        req_make(&req, "POST", "/api/v1/net/wifi/connect", long_body, cookie);
        CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_EINVAL,
              "psk 长于 63 位被拒");
    }

    req_make(&req, "POST", "/api/v1/net/wifi/connect",
             "{\"ssid\":\"OpenAP\",\"psk\":\"12345678\",\"sec\":\"open\"}", cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_EINVAL,
          "开放网络不应带密码，被拒");

    req_make(&req, "POST", "/api/v1/net/wifi/connect", "{\"ssid\":\"OpenAP\",\"sec\":\"open\"}", cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_OK,
          "开放网络（无密码）参数合法");
    CHECK(http_status == 202, "配网提交立即返回 202，实际 %d", http_status);

    req_make(&req, "POST", "/api/v1/net/wifi/connect", "{\"ssid\":\"HomeWiFi\",\"psk\":\"12345678\"}", cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_OK,
          "合法 WPA2 参数提交成功");
    CHECK(http_status == 202, "状态码 202，实际 %d", http_status);
    CHECK(strcmp(body, "{\"code\":0,\"msg\":\"正在连接，请将设备连回目标网络后访问新地址\"}") == 0,
          "响应体逐字匹配 brief 原文措辞，实际：%s", body);
    CHECK(strstr(body, "HomeWiFi") == NULL, "响应体不得回显提交的 SSID/密码");

    console_net_test_reset();
}

static void test_net_wifi_no_capability(void)
{
    http_req_t req;
    char body[512], cookie[128];
    bool must_change = false;
    int http_status = 0;

    SECTION("无 WiFi 能力时 scan/connect 均返回 ENOTSUP(501)，status 仍可用");
    console_net_test_reset();
    console_auth_reset_lockout();
    CHECK(console_auth_seed("ABCD1234", NULL, true) == HAL_OK, "播种凭据（出厂态）");
    CHECK(console_auth_set_password("ABCD1234", "NewPass@123") == HAL_OK, "改密");
    CHECK(do_login("admin", "NewPass@123", "192.168.60.12", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "登录成功");

    CHECK(hal_deinit() == HAL_OK, "卸载 HAL，模拟设备此刻完全没有网络能力");

    req_make(&req, "GET", "/api/v1/net/wifi/scan", NULL, cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_ENOTSUP,
          "hal_has(HAL_MOD_NET) 为假时 wifi/scan 返回 ENOTSUP（映射 501），不崩溃");

    req_make(&req, "POST", "/api/v1/net/wifi/connect", "{\"ssid\":\"x\",\"psk\":\"12345678\"}", cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_ENOTSUP,
          "同样条件下 wifi/connect 也返回 ENOTSUP");

    req_make(&req, "GET", "/api/v1/net/status", NULL, cookie);
    CHECK(console_net_test_dispatch(&req, body, sizeof(body), &http_status) == HAL_OK,
          "net/status 不依赖 WiFi 能力，无 HAL 时仍能返回（降级为无 mac 字段），不崩溃");
    CHECK(strstr(body, "\"ip\":\"\"") != NULL,
          "无 HAL 时 ip 字段仍然存在、只是降级为空串（而不是被整个省略），"
          "调用方不需要区分“字段缺失”和“值未知”两种情况，实际：%s", body);

    CHECK(hal_init(profile_raw_json()) == HAL_OK, "恢复 HAL，避免影响后续测试");
    console_net_test_reset();
}

/**
 * 钉住 console_net_handler 内部"先 http_respond_json(202) 入队、再
 * http_conn_defer_after_flush 登记"这个调用顺序本身——原理与
 * test_reboot_handler_order 完全一致：console_net_test_dispatch 绕过了
 * console_net_handler（只测不碰 conn 的 net_dispatch），颠倒 handler 内部
 * 那两行调用顺序不会被上面任何一个端点测试发现。
 */
/**
 * 评审 Minor #8：原版本只断言 s_netst_defer_fail_count 不变——把
 * console_net_handler 里整个 "if (dfn) {...}" 块删掉，这个计数同样不变，
 * 测试照样通过，而 net_connect_handoff 在整个测试套件里一次都没有被真正
 * 执行过。补两层验证：
 *   1) handler 返回后立即 peek 静态槽位，确认提交的 ssid 确实被写进去了
 *      （pending 此刻应仍为 false）——如果整个 if(dfn){} 块被删掉，这里
 *      读到的会是空串，直接证伪；
 *   2) 测试连接没有真实事件循环去 flush 它，defer 回调不会自动触发，用
 *      console_net_test_run_handoff() 手动驱动一次，断言 handoff_count
 *      确实 +1、且 pending 翻转为 true——如果 net_connect_handoff 的函数体
 *      被清空，这两条会分别失败。
 */
static void test_net_connect_handler_order(void)
{
    http_req_t req;
    http_conn_t *conn;
    char cookie[128];
    char staged_ssid[HAL_SSID_MAX];
    bool must_change = false;
    bool pending = true;   /* 故意先置 true：若 peek 因某种原因没写这个变量，下面的断言不会假阳性 */
    unsigned before, before_handoff;

    SECTION("console_net_handler：必须先入队 202 响应、再登记延后动作交给工作线程");
    console_net_test_reset();
    console_auth_reset_lockout();
    CHECK(console_auth_seed("ABCD1234", NULL, true) == HAL_OK, "播种凭据（出厂态）");
    CHECK(console_auth_set_password("ABCD1234", "NewPass@123") == HAL_OK, "改密解除强制改密");
    CHECK(do_login("admin", "NewPass@123", "192.168.60.14", cookie, sizeof(cookie), &must_change) == HAL_OK,
          "登录成功");

    conn = http_ws_test_conn_new(4096);
    CHECK(conn != NULL, "创建测试连接（不含真实 socket，足够承载 202 响应的入队）");
    if (conn) {
        req_make(&req, "POST", "/api/v1/net/wifi/connect", "{\"ssid\":\"HomeWiFi\",\"psk\":\"12345678\"}", cookie);
        req.conn = conn;   /* 生产路径由 http_server.c 的 dispatch_one 回填，这里手动模拟 */

        before = console_net_test_defer_fail_count();
        before_handoff = console_net_test_handoff_count();
        CHECK(console_net_test_full_handler(&req) == 0, "配网请求处理成功（handler 返回 0，已自行响应）");
        CHECK(console_net_test_defer_fail_count() == before,
              "延后动作登记不应失败——若 console_net_handler 内部把 http_respond_json 与 "
              "http_conn_defer_after_flush 两行调用顺序颠倒，登记时发送队列还是空的，"
              "http_conn_defer_after_flush 会返回 HAL_ESTATE，这里的计数就会增加");

        console_net_test_peek_connect(staged_ssid, sizeof(staged_ssid), &pending);
        CHECK(strcmp(staged_ssid, "HomeWiFi") == 0,
              "handler 已把提交的连接请求写进工作线程会消费的槽位，实际：%s", staged_ssid);
        CHECK(pending == false, "此刻 defer 回调还没触发，pending 应仍为 false");

        /* 测试连接没有真实事件循环去 flush 它，defer 回调不会自动触发；
           手动驱动一次，验证它确实会把上面 staged 的数据转正。 */
        console_net_test_run_handoff();
        CHECK(console_net_test_handoff_count() == before_handoff + 1,
              "net_connect_handoff 确实被执行了一次");
        console_net_test_peek_connect(staged_ssid, sizeof(staged_ssid), &pending);
        CHECK(pending == true, "回调执行后 pending 应翻转为 true，交给工作线程消费");

        http_ws_test_conn_free(conn);
    }
    console_net_test_reset();
}

static void test_static_assets(void)
{
    const console_asset_t *a;

    SECTION("static_assets");

    /* 首页必须存在，否则浏览器打开是空白 */
    a = console_asset_find("/index.html");
    CHECK(a != NULL, "/index.html 应存在");
    if (a) {
        CHECK(a->len > 0, "内容不得为空");
        CHECK(a->gzipped == 1, "应为 gzip 压缩");
        CHECK(a->etag && a->etag[0], "ETag 不得为空");
        CHECK(strstr(a->content_type, "text/html") != NULL,
              "content_type 实际为 [%s]", a->content_type);
        /* gzip 魔数 0x1f 0x8b */
        CHECK(a->data[0] == 0x1f && a->data[1] == 0x8b, "应有 gzip 魔数");
    }

    CHECK(console_asset_find("/app.js") != NULL, "/app.js 应存在");
    CHECK(console_asset_find("/style.css") != NULL, "/style.css 应存在");
    CHECK(console_asset_find("/no_such_file") == NULL, "不存在的资源应返回 NULL");
    CHECK(console_asset_find(NULL) == NULL, "NULL 应返回 NULL");
}

int main(void)
{
    if (profile_load("profiles/mock-x86.json") != HAL_OK) {
        printf("  FAIL 无法加载 profiles/mock-x86.json（工作目录应为 firmware/）\n");
        return 1;
    }
    if (hal_init(profile_raw_json()) != HAL_OK) {
        printf("  FAIL hal_init 失败\n");
        return 1;
    }

    test_err_mapping();
    test_pbkdf2_kat();
    test_proof_cross_vector();
    test_auth_flow();
    test_auth_lockout();
    test_auth_user_enum();
    test_must_change_password();
    test_activation_bootstrap();    /* 唯一一次 console_auth_init()，顺带注册路由 */
    test_activate_csrf_gate();      /* I-1：Ruling 18 CSRF 网关专项 */
    test_activate_second_gate_mismatch();  /* I-1：Ruling 19 第二判据专项 */
    test_corrupt_cred_not_reset();
    test_missing_storage_dir_fails_closed();   /* I-3：目录整体缺失 vs 文件未写过 */
    test_auth_endpoints();
    test_cred_not_in_config();
    test_activation_wires_config_and_event_bus();   /* Ruling 21：event_bus_init 接入 + 成功路径覆盖 */
    test_cred_fallback_store();
    test_config_rules();
    test_caps_json();
    test_capabilities_endpoint();
    test_api_config_endpoints();
    test_api_system_endpoints();
    test_reset_wipes_credentials();
    test_reboot_handler_order();

    test_net_ap_decision();
    test_net_ap_ssid();
    test_ap_psk_derive();
    test_net_ap_decide_grace_period();
    test_net_ap_recheck_due();
    test_captive_portal();
    test_dhcp_parse_discover();
    test_dhcp_parse_request_with_options();
    test_dhcp_parse_malformed();
    test_dhcp_build_offer_exact_bytes();
    test_dhcp_build_nak_exact_bytes();
    test_dhcp_build_buffer_too_small();
    test_dhcp_lease_table();
    test_dhcp_lease_offer_short_ttl();
    test_dhcp_lease_offer_then_request_upgrades_to_full_lease();
    test_dhcp_decide_discover_and_request();
    test_net_init_and_routes();       /* 唯一一次 console_net_init()/console_api_init()，顺带注册路由 */
    test_net_status_endpoint();
    test_net_wifi_scan_endpoint();
    test_net_wifi_connect_endpoint();
    test_net_wifi_no_capability();
    test_net_connect_handler_order();
    test_static_assets();

    printf("RESULT: console pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
