/**
 * @file main.c
 * @brief http_server 单元测试（x86 + mock 平台）
 *
 * 覆盖：请求解析（正常/畸形/超长/不完整）、header 大小写、query 取值。
 * 退出码 = 失败数。
 */
#include "modules/common/http_server/http_server.h"
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SECTION(name) printf("== %s\n", name)

static void test_parse_basic(void)
{
    http_req_t req;
    size_t consumed = 0;
    const char *raw =
        "GET /api/v1/system/info?verbose=1 HTTP/1.1\r\n"
        "Host: 192.168.1.10\r\n"
        "X-Token: abc123\r\n"
        "\r\n";

    SECTION("parse basic");
    CHECK(http_parse_request(raw, strlen(raw), &req, &consumed) == HAL_OK, "解析成功");
    CHECK(strcmp(req.method, "GET") == 0, "method=%s", req.method);
    CHECK(strcmp(req.path, "/api/v1/system/info") == 0, "path=%s", req.path);
    CHECK(strcmp(req.query, "verbose=1") == 0, "query=%s", req.query);
    CHECK(consumed == strlen(raw), "consumed=%zu", consumed);
    CHECK(req.body_len == 0, "无 body");
    /* header 名大小写不敏感 */
    CHECK(http_header(&req, "host") != NULL && strcmp(http_header(&req, "host"), "192.168.1.10") == 0,
          "小写取 Host");
    CHECK(http_header(&req, "X-TOKEN") != NULL && strcmp(http_header(&req, "X-TOKEN"), "abc123") == 0,
          "大写取 X-Token");
    CHECK(http_header(&req, "Nope") == NULL, "不存在的头返回 NULL");
}

static void test_parse_body(void)
{
    http_req_t req;
    size_t consumed = 0;
    const char *raw =
        "POST /api/v1/config HTTP/1.1\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"a\":\"bcdef\"}";   /* 13 字节 */

    SECTION("parse body");
    CHECK(http_parse_request(raw, strlen(raw), &req, &consumed) == HAL_OK, "带 body 解析成功");
    CHECK(req.body_len == 13, "body_len=%zu", req.body_len);
    CHECK(req.body != NULL && memcmp(req.body, "{\"a\":\"bcdef\"}", 13) == 0, "body 内容");
    CHECK(consumed == strlen(raw), "consumed 含 body");
}

static void test_parse_incomplete(void)
{
    http_req_t req;
    size_t consumed = 0;
    const char *headers_partial = "GET /x HTTP/1.1\r\nHost: a\r\n";       /* 缺空行 */
    const char *body_partial =
        "POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc";              /* body 不足 */

    SECTION("parse incomplete");
    CHECK(http_parse_request(headers_partial, strlen(headers_partial), &req, &consumed) == HAL_EAGAIN,
          "头未结束返回 EAGAIN");
    CHECK(http_parse_request(body_partial, strlen(body_partial), &req, &consumed) == HAL_EAGAIN,
          "body 不足返回 EAGAIN");
}

static void test_parse_malformed(void)
{
    http_req_t req;
    size_t consumed = 0;
    char longpath[HTTP_PATH_MAX + 64];
    char raw[HTTP_PATH_MAX + 128];

    SECTION("parse malformed");
    CHECK(http_parse_request("GARBAGE\r\n\r\n", 11, &req, &consumed) == HAL_EINVAL, "无方法行");
    CHECK(http_parse_request("GET\r\n\r\n", 7, &req, &consumed) == HAL_EINVAL, "缺路径");

    /* 超长路径必须拒绝而非溢出 */
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = '\0';
    longpath[0] = '/';
    snprintf(raw, sizeof(raw), "GET %s HTTP/1.1\r\n\r\n", longpath);
    CHECK(http_parse_request(raw, strlen(raw), &req, &consumed) == HAL_EINVAL, "超长路径拒绝");

    /* Content-Length 超上限必须拒绝 */
    {
        const char *toobig = "POST /x HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n";
        CHECK(http_parse_request(toobig, strlen(toobig), &req, &consumed) == HAL_EINVAL,
              "超大 Content-Length 拒绝");
    }
}

static void test_query(void)
{
    http_req_t req;
    size_t consumed = 0;
    char buf[64];
    const char *raw = "GET /ws/v1/live?stream=main&fps_div=2 HTTP/1.1\r\n\r\n";

    SECTION("query");
    CHECK(http_parse_request(raw, strlen(raw), &req, &consumed) == HAL_OK, "解析");
    CHECK(strcmp(http_query(&req, "stream", buf, sizeof(buf), "sub"), "main") == 0, "取 stream");
    CHECK(strcmp(http_query(&req, "fps_div", buf, sizeof(buf), "1"), "2") == 0, "取 fps_div");
    CHECK(strcmp(http_query(&req, "absent", buf, sizeof(buf), "def"), "def") == 0, "缺省值");
}

int main(void)
{
    test_parse_basic();
    test_parse_body();
    test_parse_incomplete();
    test_parse_malformed();
    test_query();
    printf("RESULT: http_server pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
