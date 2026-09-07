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
#include <stdlib.h>

/* 端到端 socket 测试专用的平台 socket 收发壳（与 http_server.c 内部实现各自独立，
   分别配对自己的 WSAStartup/WSACleanup，互不假设对方已初始化 Winsock）。 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET t_sock_t;
#define T_SOCK_INVALID INVALID_SOCKET
#define T_CLOSESOCK(f) closesocket(f)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int t_sock_t;
#define T_SOCK_INVALID (-1)
#define T_CLOSESOCK(f) close(f)
#endif

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

static int dummy_handler(http_req_t *req, void *user)
{
    (void)req; (void)user;
    return 0;
}

static void test_route_match(void)
{
    SECTION("route longest-prefix");
    CHECK(http_route("/", dummy_handler, NULL) == HAL_OK, "注册兜底 /");
    CHECK(http_route("/onvif/", dummy_handler, NULL) == HAL_OK, "注册 /onvif/");
    CHECK(http_route("/snapshot", dummy_handler, NULL) == HAL_OK, "注册 /snapshot");
    CHECK(http_route("/api/v1/", dummy_handler, NULL) == HAL_OK, "注册 /api/v1/");

    /* 最长前缀优先，而非注册顺序 */
    CHECK(strcmp(http_route_match("/api/v1/system/info"), "/api/v1/") == 0, "命中 /api/v1/");
    CHECK(strcmp(http_route_match("/onvif/device_service"), "/onvif/") == 0, "命中 /onvif/");
    CHECK(strcmp(http_route_match("/snapshot"), "/snapshot") == 0, "精确命中 /snapshot");
    CHECK(strcmp(http_route_match("/index.html"), "/") == 0, "兜底命中 /");
    CHECK(strcmp(http_route_match("/assets/app.js"), "/") == 0, "静态资源走兜底");
}

/* ------------------------------------------------------------------------ */
/* 端到端 socket 测试：真正起server + 真正的 client socket 收发，                 */
/* 而不仅仅是路由表逻辑——证明 http_server_start/http_respond* 确实把数据          */
/* 送上了真实 TCP 连接。                                                        */
/* ------------------------------------------------------------------------ */

static void t_net_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

static void t_net_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

static t_sock_t t_connect(uint16_t port)
{
    t_sock_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr;
#ifdef _WIN32
    DWORD tmo = 2000;
#else
    struct timeval tmo;
    tmo.tv_sec = 2;
    tmo.tv_usec = 0;
#endif
    if (fd == T_SOCK_INVALID) return T_SOCK_INVALID;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        T_CLOSESOCK(fd);
        return T_SOCK_INVALID;
    }
    return fd;
}

/* 判断已收到的字节里是否已包含完整的一个 HTTP 响应（header + 按 Content-Length
   计算的 body）。用于避免"固定次数 recv"的脆弱假设，同时靠 socket 的接收超时
   兜底，防止服务端有 bug 时测试永久卡死。 */
static int t_response_complete(const char *buf, int total)
{
    const char *hdr_end = strstr(buf, "\r\n\r\n");
    const char *cl;
    long body_len;
    int head_bytes;
    if (!hdr_end) return 0;
    head_bytes = (int)(hdr_end + 4 - buf);
    cl = strstr(buf, "Content-Length:");
    if (!cl || cl > hdr_end) return 1; /* 没有 Content-Length：header 收完即算完整 */
    body_len = strtol(cl + strlen("Content-Length:"), NULL, 10);
    return (total - head_bytes) >= (int)body_len;
}

static int t_recv_response(t_sock_t fd, char *buf, size_t cap)
{
    int total = 0;
    for (;;) {
        int n;
        if ((size_t)total >= cap - 1) break;
        n = (int)recv(fd, buf + total, (int)(cap - 1 - (size_t)total), 0);
        if (n <= 0) break; /* 出错/超时/对端关闭 */
        total += n;
        if (t_response_complete(buf, total)) break;
    }
    buf[total] = '\0';
    return total;
}

static int echo_handler(http_req_t *req, void *user)
{
    (void)user;
    return http_respond_json(req->conn, 200, "{\"ok\":true,\"from\":\"e2e\"}") == HAL_OK ? 0 : HAL_EIO;
}

static void test_e2e_request_response(void)
{
    const uint16_t port = 18080;
    t_sock_t fd;
    char resp[4096];
    const char *raw =
        "GET /e2e/echo HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n";

    SECTION("e2e 请求/响应（真实 socket 收发）");
    t_net_init();

    CHECK(http_route("/e2e/echo", echo_handler, NULL) == HAL_OK, "注册 /e2e/echo");
    CHECK(http_server_start(port) == HAL_OK, "服务端启动");

    fd = t_connect(port);
    CHECK(fd != T_SOCK_INVALID, "客户端连接成功");
    if (fd != T_SOCK_INVALID) {
        int sent = (int)send(fd, raw, (int)strlen(raw), 0);
        CHECK(sent == (int)strlen(raw), "请求发送完整");

        {
            int n = t_recv_response(fd, resp, sizeof(resp));
            CHECK(n > 0, "收到响应");
            CHECK(strncmp(resp, "HTTP/1.1 200", strlen("HTTP/1.1 200")) == 0,
                  "状态行应为 200，实际响应: %s", resp);
            CHECK(strstr(resp, "\"ok\":true") != NULL, "body 内容正确: %s", resp);
        }

        /* 同一 keep-alive 连接上再发一次请求，验证连接循环里"解析->分发->
           收缩缓冲->解析下一个"的串行节奏能正确处理同连接的重复请求 */
        {
            char resp2[4096];
            int n2;
            int sent2 = (int)send(fd, raw, (int)strlen(raw), 0);
            CHECK(sent2 == (int)strlen(raw), "第二次请求发送完整（复用同一 keep-alive 连接）");
            n2 = t_recv_response(fd, resp2, sizeof(resp2));
            CHECK(n2 > 0 && strncmp(resp2, "HTTP/1.1 200", strlen("HTTP/1.1 200")) == 0,
                  "同连接第二次请求同样成功: %s", resp2);
        }

        T_CLOSESOCK(fd);
    }

    CHECK(http_server_stop() == HAL_OK, "服务端停止（须真正关闭监听端口，供后续测试复用端口段）");
    t_net_cleanup();
}

static int notsup_handler(http_req_t *req, void *user)
{
    (void)req; (void)user;
    return HAL_ENOTSUP;
}

static void test_e2e_enotsup_501(void)
{
    const uint16_t port = 18081;
    t_sock_t fd;
    char resp[4096];
    const char *raw =
        "GET /e2e/notsup HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n";

    SECTION("e2e HAL_ENOTSUP 必须映射为真实的 HTTP 501（Global Constraints 硬性要求）");
    t_net_init();

    CHECK(http_route("/e2e/notsup", notsup_handler, NULL) == HAL_OK, "注册 /e2e/notsup");
    CHECK(http_server_start(port) == HAL_OK, "服务端启动");

    fd = t_connect(port);
    CHECK(fd != T_SOCK_INVALID, "客户端连接成功");
    if (fd != T_SOCK_INVALID) {
        int sent = (int)send(fd, raw, (int)strlen(raw), 0);
        CHECK(sent == (int)strlen(raw), "请求发送完整");

        {
            int n = t_recv_response(fd, resp, sizeof(resp));
            CHECK(n > 0, "收到响应");
            CHECK(strncmp(resp, "HTTP/1.1 501", strlen("HTTP/1.1 501")) == 0,
                  "状态行必须是 501，绝不能退化为 404，实际响应: %s", resp);
            CHECK(strstr(resp, "HAL_ENOTSUP") != NULL, "错误体应含 HAL_ENOTSUP: %s", resp);
        }

        T_CLOSESOCK(fd);
    }

    CHECK(http_server_stop() == HAL_OK, "服务端停止");
    t_net_cleanup();
}

int main(void)
{
    test_parse_basic();
    test_parse_body();
    test_parse_incomplete();
    test_parse_malformed();
    test_query();
    test_route_match();
    test_e2e_request_response();
    test_e2e_enotsup_501();
    printf("RESULT: http_server pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
