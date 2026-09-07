/**
 * @file main.c
 * @brief console 模块单元测试（x86 + mock 平台）
 *
 * 覆盖：错误码→HTTP 状态映射、鉴权全流程、配置读写、能力探测降级。
 * 退出码 = 失败数。
 */
#include "modules/console/console_internal.h"
#include "core/config.h"
#include "core/profile.h"
#include "core/json.h"
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
    CHECK(console_http_status(HAL_ENODEV)   == 404, "ENODEV→404");
    CHECK(console_http_status(HAL_EBUSY)    == 409, "EBUSY→409");
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

static void test_auth_flow(void)
{
    char salt_hex[64], nonce[64], proof[128];
    hal_err_t e;

    SECTION("鉴权流程");
    /* 首次：以出厂验证码派生凭据 */
    CHECK(console_auth_seed("ABCD1234") == HAL_OK, "以出厂验证码播种");

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
    console_auth_seed("ABCD1234");
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
    CHECK(console_auth_init() == HAL_OK, "console_auth_init 注册 /api/v1/auth/ 路由");
    CHECK(http_route_match("/api/v1/auth/login") != NULL, "登录路径可命中路由");
    CHECK(http_route_match("/api/v1/auth/challenge") != NULL, "挑战路径可命中路由");

    CHECK(console_auth_seed("ABCD1234") == HAL_OK, "重新播种（回到出厂强制改密态）");

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
 * 出厂自举：真机上没人调 console_auth_seed，凭据必须由 console_auth_init 从
 * 安全存储的出厂验证码派生。缺了这一步，控制台在真实设备上根本登不进去。
 */
static void test_factory_bootstrap(void)
{
    char salt_hex[80], nonce[80], proof[160];

    SECTION("出厂验证码自举");
    console_auth_reset_lockout();

    /* 模拟产线：把验证码烧进安全存储，清掉已有凭据 */
    CHECK(hal()->crypto->secure_write(HAL_SEC_KEY_VERIFY_CODE,
                                      (const uint8_t *)"FCT98765", 8) == HAL_OK, "烧录出厂验证码");
    hal()->crypto->secure_delete(HAL_SEC_KEY_LOCAL_USER);
    console_auth_test_reload();

    CHECK(console_auth_init() == HAL_OK, "console_auth_init 成功");
    CHECK(console_auth_must_change() == true, "自举后处于强制改密态");
    CHECK(console_auth_challenge_from("admin", salt_hex, sizeof(salt_hex),
                                      nonce, sizeof(nonce), "172.16.0.5") == HAL_OK, "取 challenge");
    CHECK(console_auth_make_proof("FCT98765", salt_hex, nonce, proof, sizeof(proof)) == HAL_OK,
          "以出厂验证码算 proof");
    CHECK(console_auth_verify_from("admin", nonce, proof, "172.16.0.5") == HAL_OK,
          "出厂设备开箱即可用出厂验证码登录");

    /* 反面：安全存储里既无凭据也无验证码时，不得静默放行，且不得让 init 失败 */
    hal()->crypto->secure_delete(HAL_SEC_KEY_LOCAL_USER);
    hal()->crypto->secure_delete(HAL_SEC_KEY_VERIFY_CODE);
    console_auth_test_reload();
    CHECK(console_auth_init() == HAL_OK, "取不到验证码也不能让 console_init 失败");
    CHECK(console_auth_challenge_from("admin", salt_hex, sizeof(salt_hex),
                                      nonce, sizeof(nonce), "172.16.0.6") == HAL_OK,
          "无凭据时 challenge 仍返回（伪 salt，防枚举）");
    CHECK(console_auth_make_proof("FCT98765", salt_hex, nonce, proof, sizeof(proof)) == HAL_OK,
          "算 proof");
    CHECK(console_auth_verify_from("admin", nonce, proof, "172.16.0.6") == HAL_EPERM_,
          "无凭据时任何登录都必须失败，不得静默放行");
}

static void test_cred_not_in_config(void)
{
    char salt_hex[80], nonce[80];
    char *dump;
    const size_t cap = 64 * 1024;

    SECTION("凭据不得进入配置导出");
    CHECK(cfg_init(NULL, "console_test_cfg.json") == HAL_OK, "配置中心就绪");
    CHECK(console_auth_seed("ABCD1234") == HAL_OK, "播种凭据");
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
    cfg_deinit();
    remove("console_test_cfg.json");
}

static void test_cred_fallback_store(void)
{
    char salt_hex[80], nonce[80], proof[160];

    SECTION("无 hal_crypto 平台的软存储兜底");
    console_auth_reset_lockout();
    CHECK(hal_deinit() == HAL_OK, "卸载 HAL，模拟平台不提供 crypto 模块");
    CHECK(hal_has(HAL_MOD_CRYPTO) == false, "crypto 能力不可用");

    CHECK(console_auth_seed("FALLBK99") == HAL_OK, "凭据落到软存储文件");
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
    test_auth_flow();
    test_auth_lockout();
    test_auth_user_enum();
    test_must_change_password();
    test_auth_endpoints();
    test_factory_bootstrap();
    test_cred_not_in_config();
    test_cred_fallback_store();

    printf("RESULT: console pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
