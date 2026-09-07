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
#include <windows.h>
typedef SOCKET t_sock_t;
#define T_SOCK_INVALID INVALID_SOCKET
#define T_CLOSESOCK(f) closesocket(f)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
typedef int t_sock_t;
#define T_SOCK_INVALID (-1)
#define T_CLOSESOCK(f) close(f)
#endif

/** 跨平台毫秒级 sleep，仅供下面的延后动作 e2e 测试轮询等待用 */
static void t_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

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

/* 前向声明：http_conn_defer_after_flush 的 e2e 测试 handler，定义在文件靠后
   与其测试函数放在一起，但必须挂在 echo_handler 已经占用的 "/e2e/echo"
   前缀下面分发（见下）——ROUTE_MAX=8 在本文件里已经被既有测试注册满，
   新场景不能再申请新前缀。 */
static int defer_handler(http_req_t *req, void *user);

static int echo_handler(http_req_t *req, void *user)
{
    /* 顺带回显 http_conn_peer_ip：这是 handler 拿到来源 IP 的唯一途径（console
       的按 IP 登录锁定依赖它），只有走真实 accept 才能验证地址确实被记下来了。 */
    const char *peer;
    char body[128];

    /* 复用本前缀承载 Task 7 resolution B 的 http_conn_defer_after_flush e2e
       测试子路径，而不是新注册一个前缀（"/e2e/echo" 是这个子路径的最长匹配
       前缀，route_lookup 会把它路由到这个 handler）。 */
    if (strcmp(req->path, "/e2e/echo/defer1") == 0) return defer_handler(req, user);

    peer = http_conn_peer_ip(req->conn);
    snprintf(body, sizeof(body), "{\"ok\":true,\"from\":\"e2e\",\"peer\":\"%s\"}", peer ? peer : "");
    return http_respond_json(req->conn, 200, body) == HAL_OK ? 0 : HAL_EIO;
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
            CHECK(strstr(resp, "\"peer\":\"127.0.0.1\"") != NULL,
                  "handler 经 http_conn_peer_ip 取到真实对端地址: %s", resp);
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

/* ------------------------------------------------------------------------ */
/* e2e：http_respond_ex 发送队列溢出时必须原子失败（Task 3 fix round 1 回归测试）  */
/*                                                                            */
/* 缺陷背景：http_respond_ex 对 head/body 分两次独立调用 conn_queue_send 入队；  */
/* 若 head 入队成功但 body 因发送队列容量上限而入队失败，此前 head 会不可逆地    */
/* 残留在队列里——handler 把失败透传给框架后，dispatch_one 会在同一连接上再追    */
/* 加一个完整的错误响应，与残留的半截 head 拼接成对端无法解析的畸形 HTTP 流。    */
/*                                                                            */
/* http_server.c 内部 SEND_QUEUE_MAX = HTTP_BODY_MAX + 8192 = 81920 字节        */
/* （文件内静态常量，测试文件拿不到）。这里让响应体达到 100KB，无论加不加上     */
/* 响应头都稳定超过这个上限，从而确定性地触发"容量上限"这条失败分支——不依赖    */
/* 真实 OOM/realloc 失败，可移植。                                             */
/* ------------------------------------------------------------------------ */

#define T_OVERFLOW_BODY_LEN (100 * 1024)
static char s_overflow_body[T_OVERFLOW_BODY_LEN];

static int overflow_handler(http_req_t *req, void *user)
{
    hal_err_t rc;
    (void)user;
    rc = http_respond_ex(req->conn, 200, "application/octet-stream", NULL,
                          s_overflow_body, sizeof(s_overflow_body));
    /* 既定用法约定：http_respond* 失败时，handler 把失败原样透传给框架
       （见文件头注释与上面的 echo_handler），由 dispatch_one 在同一连接上
       追加错误响应。这正是暴露"半截 head 残留 + 框架错误响应拼接"缺陷的路径。 */
    return (rc == HAL_OK) ? 0 : (int)rc;
}

static void test_e2e_respond_ex_atomic_on_overflow(void)
{
    const uint16_t port = 18082;
    t_sock_t fd;
    char resp[4096];
    const char *raw =
        "GET /e2e/overflow HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n";

    SECTION("e2e http_respond_ex 发送队列溢出必须原子失败（不残留半截 head）");
    memset(s_overflow_body, 'x', sizeof(s_overflow_body));
    t_net_init();

    CHECK(http_route("/e2e/overflow", overflow_handler, NULL) == HAL_OK, "注册 /e2e/overflow");
    CHECK(http_server_start(port) == HAL_OK, "服务端启动");

    fd = t_connect(port);
    CHECK(fd != T_SOCK_INVALID, "客户端连接成功");
    if (fd != T_SOCK_INVALID) {
        int sent = (int)send(fd, raw, (int)strlen(raw), 0);
        CHECK(sent == (int)strlen(raw), "请求发送完整");

        {
            int n = t_recv_response(fd, resp, sizeof(resp));

            CHECK(n > 0, "收到响应");
            /* 核心断言：缺陷复现时，body 入队失败前 head（200，声明
               Content-Length=T_OVERFLOW_BODY_LEN）已经不可逆入队成功，框架
               随后追加的 500 错误响应会紧跟在这个不完整的 head 后面一起被
               发出——客户端首先看到的会是那个残留的 200 状态行。修复后
               head+body 应作为一个整体一起失败，队列里应只有框架追加的这
               一个完整 500 响应。 */
            CHECK(strncmp(resp, "HTTP/1.1 500", strlen("HTTP/1.1 500")) == 0,
                  "必须是框架追加的单个完整 500 响应，而非残留的 200 头，实际收到: %s", resp);

            {
                const char *hdr_end = strstr(resp, "\r\n\r\n");
                const char *cl = strstr(resp, "Content-Length:");
                CHECK(hdr_end != NULL, "响应头必须以空行结束（格式完整）");
                CHECK(cl != NULL && hdr_end != NULL && cl < hdr_end, "响应必须带 Content-Length 头");
                if (hdr_end && cl && cl < hdr_end) {
                    long declared_len = strtol(cl + strlen("Content-Length:"), NULL, 10);
                    int head_bytes = (int)(hdr_end + 4 - resp);
                    /* 字节流必须恰好是一个完整响应：实际收到的 body 字节数与
                       响应头声明的 Content-Length 完全一致，既不短少（被截断，
                       客户端会一直等待剩余字节直至超时）也不多出（被其他
                       响应的字节拼接污染）。 */
                    CHECK((n - head_bytes) == (int)declared_len,
                          "body 字节数应等于声明的 Content-Length（头部 %d 字节之后实收 %d 字节 body，声明 %ld）",
                          head_bytes, n - head_bytes, declared_len);
                }
            }
            CHECK(strstr(resp, "HAL_ENOMEM") != NULL,
                  "错误体应包含 HAL_ENOMEM（http_respond_ex 因发送队列溢出返回的错误码）: %s", resp);
        }

        /* 溢出发生后，同一 keep-alive 连接必须仍能干净地处理下一个请求，
           证明这次失败没有把连接拖入不可恢复的畸形状态。 */
        {
            char resp2[4096];
            const char *raw2 =
                "GET /e2e/echo HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "\r\n";
            int sent2 = (int)send(fd, raw2, (int)strlen(raw2), 0);
            int n2;
            CHECK(sent2 == (int)strlen(raw2), "溢出之后，同连接再次发送请求成功");
            n2 = t_recv_response(fd, resp2, sizeof(resp2));
            CHECK(n2 > 0 && strncmp(resp2, "HTTP/1.1 200", strlen("HTTP/1.1 200")) == 0,
                  "溢出之后同连接仍可正常处理后续请求: %s", resp2);
        }

        T_CLOSESOCK(fd);
    }

    CHECK(http_server_stop() == HAL_OK, "服务端停止");
    t_net_cleanup();
}

/* ------------------------------------------------------------------------ */
/* e2e：http_respond_ex 对 1xx 状态码必须走精简模板（Task 4 评审修正 1 回归测试）  */
/*                                                                            */
/* 缺陷背景：http_respond_ex 的通用模板固定给每个响应追加 Content-Type/         */
/* Content-Length/Connection 三行；直接把这套模板套在 101 上会违反 RFC 7230    */
/* §3.3.2（"a server MUST NOT send a Content-Length header field in any       */
/* response with a status code of 1xx"），且与 http_ws_upgrade 通过           */
/* extra_headers 追加的 Connection: Upgrade 重复。这里不走真实 WS 握手（无需  */
/* 构造 Sec-WebSocket-Key 等头），直接调用 http_respond_ex(..., 101, ...) 验证 */
/* 的是模板本身的字节输出，覆盖面与 http_ws_upgrade 内部实际调用完全一致。      */
/* ------------------------------------------------------------------------ */

static int ws101_handler(http_req_t *req, void *user)
{
    hal_err_t rc;
    (void)user;
    rc = http_respond_ex(req->conn, 101, NULL,
                          "Upgrade: websocket\r\nConnection: Upgrade\r\n", NULL, 0);
    return (rc == HAL_OK) ? 0 : (int)rc;
}

static void test_e2e_101_lean_template(void)
{
    const uint16_t port = 18083;
    t_sock_t fd;
    char resp[4096];
    const char *raw =
        "GET /e2e/ws101 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n";

    SECTION("e2e 1xx 响应必须走精简模板（不带 Content-Length，Connection 不重复）");
    t_net_init();

    CHECK(http_route("/e2e/ws101", ws101_handler, NULL) == HAL_OK, "注册 /e2e/ws101");
    CHECK(http_server_start(port) == HAL_OK, "服务端启动");

    fd = t_connect(port);
    CHECK(fd != T_SOCK_INVALID, "客户端连接成功");
    if (fd != T_SOCK_INVALID) {
        int sent = (int)send(fd, raw, (int)strlen(raw), 0);
        CHECK(sent == (int)strlen(raw), "请求发送完整");

        {
            /* 101 精简模板没有 Content-Length，t_response_complete 对没有
               Content-Length 的响应按"收到头部空行即算完整"处理，正好匹配。 */
            int n = t_recv_response(fd, resp, sizeof(resp));
            const char *p;
            int conn_header_count = 0;

            CHECK(n > 0, "收到响应");
            CHECK(strncmp(resp, "HTTP/1.1 101 Switching Protocols",
                          strlen("HTTP/1.1 101 Switching Protocols")) == 0,
                  "状态行必须是完整的 \"HTTP/1.1 101 Switching Protocols\""
                  "（reason phrase 也要锁住，防止 status_reason 缺 case 101 退化为"
                  " Unknown），实际响应: %s", resp);
            CHECK(strstr(resp, "Content-Length:") == NULL,
                  "1xx 响应绝不能带 Content-Length（RFC 7230 §3.3.2 MUST NOT）: %s", resp);
            CHECK(strstr(resp, "Content-Type:") == NULL,
                  "1xx 精简模板不应带 Content-Type: %s", resp);

            for (p = resp; (p = strstr(p, "Connection:")) != NULL; p += strlen("Connection:")) {
                conn_header_count++;
            }
            CHECK(conn_header_count == 1,
                  "Connection 头必须恰好出现一次，不与通用模板的 Connection: keep-alive 重复，"
                  "实际 %d 次: %s", conn_header_count, resp);
        }

        T_CLOSESOCK(fd);
    }

    CHECK(http_server_stop() == HAL_OK, "服务端停止");
    t_net_cleanup();
}

/* ------------------------------------------------------------------------ */
/* e2e/单元测试：http_conn_defer_after_flush（Task 7 resolution B）              */
/* ——"先响应再动作"，调用顺序由返回值强制                                     */
/*                                                                            */
/* 背景：console 的重启/恢复出厂端点需要"响应确实发出去之后才执行动作"，        */
/* 而 http_respond_* 只是把数据拷进发送队列，真正的 send() 只发生在事件循环    */
/* 的可写分支（conn_flush_send）里。这里不测 console，只测这个通用原语本身。    */
/*                                                                            */
/* 说明：最初尝试过用"收窄客户端 SO_RCVBUF + 70KB 大响应体 + 客户端故意不读"   */
/* 去制造一个"响应确实还卡在队列里"的窗口，实测在本机回环 socket 上，即便客户端 */
/* 完全不读，这套组合仍会在 300ms 内被 conn_flush_send 判定为已发送完毕——     */
/* 回环网络栈的缓冲能力显然超出 SEND_QUEUE_MAX(81920 字节) 这个应用层上限，     */
/* 无法在这个预算内可靠制造真正的部分发送/EWOULDBLOCK，勉强凑数只会做出一条    */
/* 时快时慢的 flaky 测试。因此不去赌网络时序，改为验证三条不依赖时序的事实：   */
/*   1) 单元级、确定性：登记时若发送队列已空（未入队任何数据），必须返回      */
/*      HAL_ESTATE，不注册、不同步执行——这是评审后新加的契约，用一个不含     */
/*      任何真实 socket 的裸连接直接测，不需要 HTTP 往返。                    */
/*   2) 端到端事实（真实 socket，有界轮询）：先入队响应、再登记，一次正常的   */
/*      请求/响应之后，回调最终确实会被触发且只触发一次。                     */
/*   3) 顺序契约本身：defer_handler 里"先 respond 后 defer"这个正确顺序，     */
/*      登记调用必须成功（不应该返回 HAL_ESTATE）——颠倒这两行的调用顺序      */
/*      会让这条断言变红，钉住调用顺序而不只是钉住"最终会不会 fire"。         */
/* "连接在触发前关闭则动作被放弃"这条契约改为代码走查确认，不做 e2e：          */
/* conn_close 新增的分支只清空 after_flush/after_flush_arg 两个字段，函数里   */
/* 没有任何路径会调用存在里面的函数指针（见 http_server.c 的 conn_close）。    */
/* ------------------------------------------------------------------------ */

static void defer_mark(void *arg) { *(volatile bool *)arg = true; }

static void test_defer_after_flush_requires_pending_data(void)
{
    http_conn_t *c;
    bool fired = false;

    SECTION("http_conn_defer_after_flush：登记时若无待发数据，必须返回 HAL_ESTATE（不注册、不同步执行）");
    c = http_ws_test_conn_new(4096);
    CHECK(c != NULL, "创建测试连接（不含真实 socket，也不入队任何响应）");
    if (c) {
        CHECK(http_conn_defer_after_flush(c, defer_mark, (void *)&fired) == HAL_ESTATE,
              "未先入队任何响应就登记，必须返回 HAL_ESTATE");
        CHECK(!fired, "回调不应该被同步触发——调用顺序错误应该报错，而不是静默替调用方执行");
        http_ws_test_conn_free(c);
    }
}

static volatile bool s_defer_fired;
static volatile bool s_defer_register_failed;  /**< 顺序正确（先 respond 后 defer）时登记若仍失败则置真——应恒为假 */

static int defer_handler(http_req_t *req, void *user)
{
    hal_err_t rc;
    (void)user;
    /* 顺序是本测试要钉住的东西：respond 必须在 defer 之前。颠倒这两行，
       下面的 http_conn_defer_after_flush 调用会因为此刻还没有入队任何
       数据（c->slen==c->soff）而返回 HAL_ESTATE，被 test_defer_after_
       flush_fires_once_sent 的断言捕获。 */
    rc = http_respond_json(req->conn, 200, "{\"code\":0,\"msg\":\"ok\"}");
    if (rc != HAL_OK) return (int)rc;
    rc = http_conn_defer_after_flush(req->conn, defer_mark, (void *)&s_defer_fired);
    if (rc != HAL_OK) s_defer_register_failed = true;
    return 0;
}

static void test_defer_after_flush_fires_once_sent(void)
{
    const uint16_t port = 18084;
    t_sock_t fd;
    char resp[4096];
    int i;
    const char *raw =
        "GET /e2e/echo/defer1 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n";

    SECTION("e2e http_conn_defer_after_flush：先响应再登记的正确顺序下，回调最终必被触发一次");
    s_defer_fired = false;
    s_defer_register_failed = false;
    t_net_init();

    /* 不再新注册前缀：/e2e/echo/defer1 经 echo_handler 里的路径分支路由到
       defer_handler（见 echo_handler 前的说明——本文件的 ROUTE_MAX(8) 已被
       既有测试占满）。/e2e/echo 前缀已由 test_e2e_request_response 注册过，
       该测试固定先于本测试运行——用 strcmp 精确核对命中的就是这个前缀本身，
       而不是随便断言"非 NULL"：本文件已经注册了 "/" 兜底路由，任何路径
       （包括打错的）在那种写法下都会非 NULL，断言形同虚设。 */
    CHECK(strcmp(http_route_match("/e2e/echo/defer1"), "/e2e/echo") == 0,
          "/e2e/echo 前缀已注册（由更早的 e2e 测试完成），且命中的正是这个前缀");
    CHECK(http_server_start(port) == HAL_OK, "服务端启动");

    fd = t_connect(port);
    CHECK(fd != T_SOCK_INVALID, "客户端连接成功");
    if (fd != T_SOCK_INVALID) {
        int sent = (int)send(fd, raw, (int)strlen(raw), 0);
        CHECK(sent == (int)strlen(raw), "请求发送完整");

        {
            int n = t_recv_response(fd, resp, sizeof(resp));
            CHECK(n > 0, "收到响应");
            CHECK(strncmp(resp, "HTTP/1.1 200", strlen("HTTP/1.1 200")) == 0, "状态行应为 200");
        }

        CHECK(!s_defer_register_failed,
              "响应已经入队后再登记 defer，必须成功（不应返回 HAL_ESTATE）——"
              "颠倒 defer_handler 里 respond/defer 两行的调用顺序会让这里变红");

        /* 回调在服务端事件循环线程里触发，与测试线程存在微小时序间隙：
           有界轮询而非固定 sleep 或立即断言。 */
        for (i = 0; i < 100 && !s_defer_fired; i++) t_sleep_ms(20);
        CHECK(s_defer_fired, "响应数据全部交给内核之后，回调必须被触发");

        T_CLOSESOCK(fd);
    }

    CHECK(http_server_stop() == HAL_OK, "服务端停止");
    t_net_cleanup();
}

/* ------------------------------------------------------------------------ */
/* 握手 accept 值已知答案测试（Task 4 评审 Important 3-1）                        */
/*                                                                            */
/* 853 行手写密码学（SHA-1+Base64+GUID 拼接）此前没有任何直接测试，唯一兜底是   */
/* "Task 10 有人开浏览器联调时会发现"。RFC 6455 §1.3 自带标准向量：一条断言    */
/* 同时钉死 SHA-1、Base64、GUID 拼接三件事。                                   */
/* ------------------------------------------------------------------------ */

static void test_ws_handshake_known_answer(void)
{
    char accept[64];

    SECTION("ws 握手 accept 值已知答案测试（RFC 6455 §1.3 标准向量）");

    CHECK(http_ws_test_compute_accept("dGhlIHNhbXBsZSBub25jZQ==", accept, sizeof(accept)) == HAL_OK,
          "计算 accept 值应成功");
    CHECK(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0,
          "accept 值必须精确匹配 RFC 6455 §1.3 标准向量，实际: %s", accept);
}

/* ------------------------------------------------------------------------ */
/* 环形缓冲绕回：断言绕回后字节内容正确，而非只验证长度（Task 4 评审 Important 3-2） */
/*                                                                            */
/* http_ws_test_drain 每次把 head 清零，测试里因此永远不会触发真正的绕回写入/  */
/* 读出路径，而绕回逻辑在预览推流场景下会持续运行。这里用 http_ws_test_read_  */
/* drain（只推进 head、不清零）刻意构造一次跨越 cap 边界的写入，逐字节比对    */
/* 读回内容，而不只是核对长度。                                               */
/* ------------------------------------------------------------------------ */

static void test_ws_ring_wraparound(void)
{
    http_conn_t *c;
    uint8_t payload_a[60], payload_b[50];
    uint8_t readback[100];
    uint8_t expect_b[52];
    size_t i, n;

    SECTION("ws 环形缓冲绕回：断言绕回后字节内容正确（而不只是长度）");

    for (i = 0; i < sizeof(payload_a); i++) payload_a[i] = (uint8_t)(0x10 + (i & 0x3F));
    for (i = 0; i < sizeof(payload_b); i++) payload_b[i] = (uint8_t)(0x80 + (i & 0x3F));

    c = http_ws_test_conn_new(100); /* 刻意选小容量，方便手工推算绕回边界 */
    CHECK(c != NULL, "创建测试连接（queue_cap=100）");
    if (!c) return;

    /* 首帧 60 字节 payload（opcode=0x2，len<126 头部占 2 字节，共 62 字节），
       整帧读出丢弃后 head=62、used=0、cap 不变——为下一帧制造"从 62 开始写
       52 字节会跨越 cap=100 边界"的条件。 */
    CHECK(http_ws_send(c, payload_a, sizeof(payload_a), true) == HAL_OK, "首帧（占位用）入队成功");
    CHECK(http_ws_queue_used(c) == 62, "首帧入队后 used 应为 62，实际 %zu", http_ws_queue_used(c));
    n = http_ws_test_read_drain(c, readback, 62);
    CHECK(n == 62, "首帧应整帧读出 62 字节，实际 %zu", n);
    CHECK(http_ws_queue_used(c) == 0, "首帧读出后队列应清空");

    /* 第二帧 50 字节 payload（头部 2 字节，共 52 字节）：写入起点
       tail=(head+used)%cap=(62+0)%100=62，cap-tail=38<52，ws_ring_write 必须
       分两段写（buf[62..99] 38 字节 + buf[0..13] 14 字节）——这是真正跨越
       cap 边界的绕回，不是理论上可能发生而已。 */
    CHECK(http_ws_send(c, payload_b, sizeof(payload_b), true) == HAL_OK,
          "第二帧入队成功（应触发绕回写入）");
    CHECK(http_ws_queue_used(c) == 52, "第二帧入队后 used 应为 52，实际 %zu", http_ws_queue_used(c));

    n = http_ws_test_read_drain(c, readback, 52);
    CHECK(n == 52, "第二帧应整帧读出 52 字节（应触发绕回读取），实际 %zu", n);

    /* 期望字节 = ws_encode_header(0x2, 50) 的 2 字节头部 {0x82, 0x32}
       （0x80|0x2=0x82，50<126 单字节长度域 0x32=50）+ payload_b 原文。 */
    expect_b[0] = 0x82;
    expect_b[1] = 0x32;
    memcpy(expect_b + 2, payload_b, sizeof(payload_b));
    CHECK(memcmp(readback, expect_b, sizeof(expect_b)) == 0,
          "绕回读出的字节内容必须与写入时逐字节一致，而不只是长度一致");

    http_ws_test_conn_free(c);
}

/* ------------------------------------------------------------------------ */
/* WS 背压：队列满后丢弃非关键帧，恢复时必须从关键帧续传                            */
/* ------------------------------------------------------------------------ */

static void test_ws_backpressure(void)
{
    http_conn_t *c;
    uint8_t payload[4096];
    int i, sent_ok = 0, dropped = 0;

    SECTION("ws backpressure");
    memset(payload, 0xAB, sizeof(payload));

    c = http_ws_test_conn_new(16 * 1024);   /* 测试桩：不含真实 socket 的 WS 连接 */
    CHECK(c != NULL, "创建测试连接");
    if (!c) return;

    /* 灌入远超队列容量的数据，且全部为非关键帧 */
    for (i = 0; i < 32; i++) {
        hal_err_t e = http_ws_send(c, payload, sizeof(payload), false);
        if (e == HAL_OK) sent_ok++;
        else if (e == HAL_EAGAIN) dropped++;
    }
    CHECK(dropped > 0, "队列满后应有丢弃，dropped=%d", dropped);
    CHECK(http_ws_queue_used(c) <= 16 * 1024, "队列不得超出容量 used=%zu", http_ws_queue_used(c));

    /* 丢弃状态下：非关键帧继续丢，关键帧应被接纳（先腾空队列） */
    http_ws_test_drain(c);
    CHECK(http_ws_send(c, payload, sizeof(payload), false) == HAL_EAGAIN,
          "丢弃态下非关键帧仍被丢弃");
    CHECK(http_ws_send(c, payload, sizeof(payload), true) == HAL_OK,
          "丢弃态遇关键帧应恢复发送");
    CHECK(http_ws_send(c, payload, sizeof(payload), false) == HAL_OK,
          "恢复后非关键帧正常入队");
    CHECK(http_ws_dropped(c) == (uint64_t)(dropped + 1), "丢帧计数准确");

    http_ws_test_conn_free(c);
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
    test_e2e_respond_ex_atomic_on_overflow();
    test_e2e_101_lean_template();
    test_defer_after_flush_requires_pending_data();
    test_defer_after_flush_fires_once_sent();
    test_ws_handshake_known_answer();
    test_ws_ring_wraparound();
    test_ws_backpressure();
    printf("RESULT: http_server pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
