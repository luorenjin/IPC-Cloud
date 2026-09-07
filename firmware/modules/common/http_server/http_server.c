/**
 * @file http_server.c
 * @brief 单线程事件循环 HTTP 服务：连接管理、最长前缀路由分发、响应发送
 *
 * 纪律：所有 handler 都在事件循环线程内同步执行，绝不可阻塞（不做磁盘 I/O、
 * 不查数据库、不调 frame_bus_pull）。http_respond* 只把数据写入该连接自身的
 * 发送队列并标记"可写"兴趣（Linux: EPOLLOUT；Windows: 下一轮 select 纳入
 * writefds），从不直接调用 send/write —— 真正的发送只发生在事件循环的
 * "可写"分支里，从而保证单线程不阻塞。
 *
 * 线程模型：http_server_start() 通过 os_thread_create 另起一个线程运行事件
 * 循环并立即返回；http_server_stop() 置退出标志、os_thread_join 等待线程
 * 结束后再释放所有资源，保证可重复 start/stop 且不泄漏线程/连接/端口。
 *
 * 平台实现：Linux 用 epoll（边缘触发），Windows（x86 测试环境）用 select。
 * 两侧共用路由表、错误码映射、响应拼装与连接读写辅助函数；平台差异只体现在
 * 非阻塞设置、事件循环本体与 epoll 专属的兴趣位管理上。
 */
#include "http_server.h"
#include "http_server_internal.h"
#include "core/os.h"
#include "core/log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>   /* inet_ntop / sockaddr_storage */
#define CLOSESOCK(f) closesocket(f)
/* accept 第三参：Windows 是 int*，POSIX 是 socklen_t*。用自有别名而非直接
   typedef socklen_t，避免与各 Windows 工具链自带的定义撞名。 */
typedef int ipc_socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>
#define CLOSESOCK(f) close(f)
typedef socklen_t ipc_socklen_t;
#endif

#define MOD "http_server"

#define ROUTE_MAX       8
/* CONN_MAX 定义在 http_server_internal.h（与 http_ws.c 共享，避免跨文件复制） */
#define CONN_BUF_MAX    (HTTP_BODY_MAX + 8192)     /**< 每连接接收缓冲，accept 时惰性 malloc */
#define SEND_QUEUE_MAX  (HTTP_BODY_MAX + 8192)     /**< 发送队列上限，防止慢客户端无限占内存 */
#define POLL_TIMEOUT_MS 100                        /**< 事件循环轮询步长：兼顾停止响应速度与 CPU 占用 */

/* ------------------------------------------------------------------------ */
/* 连接结构：完整布局见 http_server_internal.h（http_server.c 与 http_ws.c 共享）  */
/* ------------------------------------------------------------------------ */

static struct {
    sock_t       listen_fd;
    http_conn_t  conns[CONN_MAX];
    os_thread_t *thread;
#ifndef _WIN32
    int          epfd;
#endif
} s_srv = {
    /* 静态零初始化本身会把 listen_fd/epfd 都置 0，而 0 在 POSIX 上恰好是
     * stdin 的 fd 号——"服务从未启动"这一状态因此和"epfd/listen_fd 是合法
     * 值 0"在类型层面无法区分，conn_update_poll_interest/conn_close 里
     * "epfd < 0 即未运行"的守卫在从未 start 过时（IPC_TESTING 测试桩场景）
     * 会被 0 骗过，仍然对垃圾 fd 调用 epoll_ctl（评审指出：这条守卫此前
     * 带着"服务未运行"的注释，却并不真正提供这个保护）。显式初始化为
     * SOCK_INVALID/-1，让"未运行"从一开始就是这两个守卫能正确识别的值。
     * conns[]/thread 未在此列出的字段仍按 C 规则隐式零初始化，行为不变。 */
    .listen_fd = SOCK_INVALID,
#ifndef _WIN32
    .epfd = -1,
#endif
};

static os_mutex_t *s_stop_mu;
static bool        s_should_stop;

#ifndef _WIN32
static int s_listen_tag; /**< 哨兵地址：epoll 事件里用来区分“监听 fd”与“连接 fd” */
#endif

/* ------------------------------------------------------------------------ */
/* 路由表（本段逐字来自任务书 Step 4，未作改动）                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
    char prefix[64];
    http_handler_fn fn;
    void *user;
} route_t;

static route_t s_routes[ROUTE_MAX];
static size_t  s_route_count;

hal_err_t http_route(const char *prefix, http_handler_fn fn, void *user)
{
    if (!prefix || !fn) return HAL_EINVAL;
    if (s_route_count >= ROUTE_MAX) return HAL_ENOMEM;
    if (strlen(prefix) >= sizeof(s_routes[0].prefix)) return HAL_EINVAL;
    strcpy(s_routes[s_route_count].prefix, prefix);
    s_routes[s_route_count].fn = fn;
    s_routes[s_route_count].user = user;
    s_route_count++;
    return HAL_OK;
}

static const route_t *route_lookup(const char *path)
{
    const route_t *best = NULL;
    size_t best_len = 0, i;
    for (i = 0; i < s_route_count; i++) {
        size_t plen = strlen(s_routes[i].prefix);
        if (strncmp(path, s_routes[i].prefix, plen) == 0) {
            if (!best || plen > best_len) { best = &s_routes[i]; best_len = plen; }
        }
    }
    return best;
}

const char *http_route_match(const char *path)
{
    const route_t *r = path ? route_lookup(path) : NULL;
    return r ? r->prefix : NULL;
}

/* ------------------------------------------------------------------------ */
/* 错误码 -> HTTP 状态映射                                                    */
/* 补充裁定 1：Task 3 自建通用兜底映射表，不依赖 Task 5 的业务级错误映射；        */
/* Task 5 的 handler 可以自行 http_respond* 后返回 0 绕开这里的通用兜底。        */
/* ------------------------------------------------------------------------ */

static int err_to_status(hal_err_t e)
{
    switch (e) {
        case HAL_EINVAL:   return 400;
        case HAL_ENOTSUP:  return 501; /* 硬性约束：能力探测基石，绝不可退化为 404 */
        case HAL_EBUSY:    return 409;
        case HAL_ETIMEOUT: return 504;
        case HAL_EIO:      return 500;
        case HAL_ENOMEM:   return 500;
        case HAL_ENODEV:   return 404;
        case HAL_EAGAIN:   return 503;
        case HAL_ESTATE:   return 409;
        case HAL_ECORRUPT: return 400;
        default:           return 500;
    }
}

static const char *err_to_name(hal_err_t e)
{
    switch (e) {
        case HAL_OK:       return "HAL_OK";
        case HAL_EINVAL:   return "HAL_EINVAL";
        case HAL_ENOTSUP:  return "HAL_ENOTSUP";
        case HAL_EBUSY:    return "HAL_EBUSY";
        case HAL_ETIMEOUT: return "HAL_ETIMEOUT";
        case HAL_EIO:      return "HAL_EIO";
        case HAL_ENOMEM:   return "HAL_ENOMEM";
        case HAL_ENODEV:   return "HAL_ENODEV";
        case HAL_EAGAIN:   return "HAL_EAGAIN";
        case HAL_ESTATE:   return "HAL_ESTATE";
        case HAL_ECORRUPT: return "HAL_ECORRUPT";
        default:           return "HAL_UNKNOWN";
    }
}

static const char *status_reason(int status)
{
    switch (status) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

/* ------------------------------------------------------------------------ */
/* 平台相关：非阻塞设置                                                        */
/* ------------------------------------------------------------------------ */

static hal_err_t set_nonblocking(sock_t fd)
{
#ifdef _WIN32
    u_long mode = 1;
    return (ioctlsocket(fd, FIONBIO, &mode) == 0) ? HAL_OK : HAL_EIO;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return HAL_EIO;
    return (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) ? HAL_OK : HAL_EIO;
#endif
}

/* ------------------------------------------------------------------------ */
/* 连接生命周期                                                               */
/* ------------------------------------------------------------------------ */

void conn_close(http_conn_t *c)
{
    if (!c->used) return;
    if (c->is_ws) http_ws_conn_cleanup(c);
#ifndef _WIN32
    if (s_srv.epfd >= 0) epoll_ctl(s_srv.epfd, EPOLL_CTL_DEL, c->fd, NULL);
#endif
    CLOSESOCK(c->fd);
    free(c->rbuf);
    free(c->sbuf);
    memset(c, 0, sizeof(*c));
}

#ifndef _WIN32
static void conn_register_for_poll(http_conn_t *c)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = c;
    epoll_ctl(s_srv.epfd, EPOLL_CTL_ADD, c->fd, &ev);
}

void conn_update_poll_interest(http_conn_t *c)
{
    struct epoll_event ev;
    if (s_srv.epfd < 0) return; /* 服务未运行（如 IPC_TESTING 测试桩场景）：epfd
                                    无效，与 conn_close 的守卫保持一致，避免对
                                    垃圾 fd 值调用 epoll_ctl */
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET |
                (((c->slen > c->soff) || (c->is_ws && http_ws_queue_used(c) > 0)) ? EPOLLOUT : 0);
    ev.data.ptr = c;
    epoll_ctl(s_srv.epfd, EPOLL_CTL_MOD, c->fd, &ev);
}
#else
static void conn_register_for_poll(http_conn_t *c)
{
    (void)c; /* select 每轮从 s_srv.conns[] 重建 fd_set，无需显式注册 */
}

void conn_update_poll_interest(http_conn_t *c)
{
    (void)c; /* 同上：可写兴趣由 event_loop 每轮按 slen>soff 现算 */
}
#endif

/** 把 accept 拿到的对端地址格式化进 out（失败则置空串，不视为错误） */
static void format_peer_ip(const struct sockaddr_storage *ss, char *out, size_t cap)
{
    out[0] = '\0';
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)(const void *)ss;
        if (!inet_ntop(AF_INET, (const void *)&v4->sin_addr, out, cap)) out[0] = '\0';
    } else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)(const void *)ss;
        if (!inet_ntop(AF_INET6, (const void *)&v6->sin6_addr, out, cap)) out[0] = '\0';
    }
}

const char *http_conn_peer_ip(const http_conn_t *c)
{
    if (!c || !c->peer_ip[0]) return NULL;
    return c->peer_ip;
}

static void accept_new_conn(void)
{
    for (;;) {
        struct sockaddr_storage peer;
        ipc_socklen_t peer_len = (ipc_socklen_t)sizeof(peer);
        char peer_ip[CONN_PEER_IP_MAX];
        sock_t fd;
        http_conn_t *slot = NULL;
        size_t i;

        memset(&peer, 0, sizeof(peer));
        fd = accept(s_srv.listen_fd, (struct sockaddr *)&peer, &peer_len);

        if (fd == SOCK_INVALID) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            return; /* accept 队列已排空（EWOULDBLOCK）或偶发错误：本轮结束 */
        }
        format_peer_ip(&peer, peer_ip, sizeof(peer_ip));

        for (i = 0; i < CONN_MAX; i++) {
            if (!s_srv.conns[i].used) { slot = &s_srv.conns[i]; break; }
        }
        if (!slot) {
            LOGW(MOD, "连接数已达上限(%d)，拒绝新连接", CONN_MAX);
            CLOSESOCK(fd);
            continue;
        }
        if (set_nonblocking(fd) != HAL_OK) {
            LOGW(MOD, "设置新连接为非阻塞失败，拒绝连接");
            CLOSESOCK(fd);
            continue;
        }

        memset(slot, 0, sizeof(*slot));
        slot->fd = fd;
        slot->used = 1;
        memcpy(slot->peer_ip, peer_ip, sizeof(slot->peer_ip)); /* memset 之后再填，避免被清掉 */
        slot->rbuf = (char *)malloc(CONN_BUF_MAX);
        if (!slot->rbuf) {
            LOGE(MOD, "连接接收缓冲区分配失败");
            CLOSESOCK(fd);
            memset(slot, 0, sizeof(*slot));
            continue;
        }
        conn_register_for_poll(slot);
    }
}

/* ------------------------------------------------------------------------ */
/* 发送队列：只入队/出队，实际 send() 只在事件循环“可写”分支里调用                  */
/* ------------------------------------------------------------------------ */

/**
 * 校验并在需要时扩容发送队列，使其足以再容纳 len 字节（只做容量准备，不写入
 * 数据、不推进 slen）。从 conn_queue_send 中抽出这部分逻辑，供 http_respond_ex
 * 在两段 memcpy 之前按 head+body 合计长度一次性预检查/扩容——只要这一步成功，
 * 后续对 conn_queue_send 的调用就已保证不会再因容量不足而失败，从而让
 * head/body 的入队相对调用方呈现原子语义（要么都成功，要么调用失败时
 * c->slen/c->sbuf 相对调用前保持不变）。
 */
static hal_err_t conn_queue_reserve(http_conn_t *c, size_t len)
{
    if (c->slen + len > SEND_QUEUE_MAX) {
        LOGE(MOD, "fd=%d 发送队列溢出（待发 %zu + 新增 %zu 超过上限 %d）",
             (int)c->fd, c->slen, len, SEND_QUEUE_MAX);
        return HAL_ENOMEM;
    }
    if (c->scap < c->slen + len) {
        size_t newcap = c->scap ? c->scap * 2 : 4096;
        char *nb;
        while (newcap < c->slen + len) newcap *= 2;
        nb = (char *)realloc(c->sbuf, newcap);
        if (!nb) return HAL_ENOMEM;
        c->sbuf = nb;
        c->scap = newcap;
    }
    return HAL_OK;
}

static hal_err_t conn_queue_send(http_conn_t *c, const void *data, size_t len)
{
    hal_err_t rc;
    if (len == 0) return HAL_OK;
    rc = conn_queue_reserve(c, len);
    if (rc != HAL_OK) return rc;
    memcpy(c->sbuf + c->slen, data, len);
    c->slen += len;
    conn_update_poll_interest(c); /* 数据入队后标记可写兴趣，交由事件循环发送 */
    return HAL_OK;
}

static void conn_flush_send(http_conn_t *c)
{
    while (c->slen > c->soff) {
        int n = (int)send(c->fd, c->sbuf + c->soff, (int)(c->slen - c->soff), 0);
        if (n > 0) {
            c->soff += (size_t)n;
            continue;
        }
        if (n < 0 && WOULD_BLOCK()) return; /* 内核发送缓冲已满，等下一次可写事件 */
        LOGW(MOD, "fd=%d send 失败，关闭连接", (int)c->fd);
        conn_close(c);
        return;
    }
    /* 全部发送完毕：回收发送缓冲避免长期占用内存，并撤销可写兴趣 */
    c->slen = 0;
    c->soff = 0;
    if (c->scap > 16384) {
        free(c->sbuf);
        c->sbuf = NULL;
        c->scap = 0;
    }
    conn_update_poll_interest(c);
}

/* ------------------------------------------------------------------------ */
/* 响应接口：handler 与框架内部错误兜底共用；只入队，从不直接 send/write           */
/* ------------------------------------------------------------------------ */

hal_err_t http_respond_ex(http_conn_t *c, int status, const char *content_type,
                          const char *extra_headers, const void *body, size_t len)
{
    char head[1024];
    int n;

    if (!c) return HAL_EINVAL;
    if (!extra_headers) extra_headers = "";

    if (status / 100 == 1) {
        /* 1xx 信息性响应：RFC 7230 §3.3.2 明确规定"A server MUST NOT send a
         * Content-Length header field in any response with a status code of
         * 1xx"，且 1xx 本身没有实体正文语义，也不该带 Content-Type。是否需要
         * Connection 等头完全交给调用方通过 extra_headers 表达（例如 WS 升级
         * 需要 Connection: Upgrade），这里不再像下面的通用模板那样自行追加
         * Content-Type/Content-Length/Connection 三行，只发状态行 +
         * extra_headers + 空行。body/len 无论调用方传入什么，在 1xx 路径下
         * 一律按空处理——没有 Content-Length 也没有 chunked 编码，对端根本
         * 无法界定 body 边界，发了也没有意义。单次 conn_queue_send 自身已是
         * 全有全无的原子操作（reserve 失败则完全不写入、不推进 slen），不需要
         * 像下面 head+body 两段那样额外做合并 reserve。 */
        n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "%s"
                     "\r\n",
                     status, status_reason(status), extra_headers);
        if (n < 0 || (size_t)n >= sizeof(head)) {
            LOGE(MOD, "1xx 响应头拼接失败或过长（extra_headers 是否过长？）");
            return HAL_EINVAL;
        }
        return conn_queue_send(c, head, (size_t)n);
    }

    if (!content_type) content_type = "application/octet-stream";

    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: keep-alive\r\n"
                 "%s"
                 "\r\n",
                 status, status_reason(status), content_type, len, extra_headers);
    if (n < 0 || (size_t)n >= sizeof(head)) {
        LOGE(MOD, "响应头拼接失败或过长（extra_headers 是否过长？）");
        return HAL_EINVAL;
    }
    /* 按 head+body 合计长度一次性预检查/扩容：这一步成功后，下面两次
       conn_queue_send 保证不会再因容量不足而失败，从而保证 head/body 要么
       一起入队成功，要么本次调用失败时发送队列相对调用前保持不变——不会像
       之前那样把已入队的 head 残留在队列里，被框架后续追加的错误响应拼接
       成对端无法解析的畸形 HTTP 流。 */
    if (conn_queue_reserve(c, (size_t)n + ((len && body) ? len : 0)) != HAL_OK) return HAL_ENOMEM;
    if (conn_queue_send(c, head, (size_t)n) != HAL_OK) return HAL_ENOMEM;
    if (len && body) {
        if (conn_queue_send(c, body, len) != HAL_OK) return HAL_ENOMEM;
    }
    return HAL_OK;
}

hal_err_t http_respond(http_conn_t *c, int status, const char *content_type,
                       const void *body, size_t len)
{
    return http_respond_ex(c, status, content_type, NULL, body, len);
}

hal_err_t http_respond_json(http_conn_t *c, int status, const char *json)
{
    size_t len = json ? strlen(json) : 0;
    return http_respond_ex(c, status, "application/json; charset=utf-8", NULL, json, len);
}

/* ------------------------------------------------------------------------ */
/* 请求分发                                                                   */
/* ------------------------------------------------------------------------ */

static void dispatch_one(http_conn_t *c, http_req_t *req)
{
    const route_t *r;

    req->conn = c; /* 补充裁定 4：http_parse_request 不填 conn，这里手动回填 */
    r = route_lookup(req->path);
    if (!r) {
        http_respond_json(c, 404, "{\"error\":\"not_found\"}");
        return;
    }

    {
        int rc = r->fn(req, r->user);
        if (rc < 0) {
            char json[64];
            int status = err_to_status((hal_err_t)rc);
            snprintf(json, sizeof(json), "{\"error\":\"%s\"}", err_to_name((hal_err_t)rc));
            http_respond_json(c, status, json);
        }
        /* rc == 0：handler 已自行调用 http_respond*，框架不重复响应 */
    }
}

static void conn_handle_readable(http_conn_t *c)
{
    for (;;) {
        char tmp[4096];
        int n = (int)recv(c->fd, tmp, (int)sizeof(tmp), 0);

        if (n > 0) {
            if (c->rlen + (size_t)n > CONN_BUF_MAX) {
                LOGW(MOD, "fd=%d 接收缓冲溢出，关闭连接", (int)c->fd);
                conn_close(c);
                return;
            }
            memcpy(c->rbuf + c->rlen, tmp, (size_t)n);
            c->rlen += (size_t)n;

            if (c->is_ws) {
                http_ws_on_readable(c);
                if (!c->used) return;
                continue; /* 边缘触发下继续 recv 直至 EWOULDBLOCK，排空内核缓冲 */
            }

            /* 单请求串行处理：每次 dispatch 都同步跑完（含 handler 内部调用的
               http_respond*）之后才 memmove 缓冲、解析下一个请求。这保证了
               http_parse_request 内部 header 暂存区（static hbuf）不会被跨
               请求脏读——无论是同一连接的管线化请求，还是不同连接之间。 */
            for (;;) {
                http_req_t req;
                size_t consumed = 0;
                hal_err_t pr = http_parse_request(c->rbuf, c->rlen, &req, &consumed);

                if (pr == HAL_OK) {
                    dispatch_one(c, &req);
                    if (!c->used) return; /* handler 期间连接已被关闭（如 send 失败） */
                    memmove(c->rbuf, c->rbuf + consumed, c->rlen - consumed);
                    c->rlen -= consumed;
                    continue; /* 同一缓冲内可能还有管线化的下一个请求 */
                }
                if (pr == HAL_EAGAIN) break; /* 数据不完整：等待下一次可读事件 */

                LOGW(MOD, "fd=%d 畸形请求，关闭连接", (int)c->fd);
                conn_close(c);
                return;
            }
            continue; /* 边缘触发下需继续 recv 直至 EWOULDBLOCK，排空内核缓冲 */
        }
        if (n == 0) { conn_close(c); return; }  /* 对端正常关闭 */
        if (WOULD_BLOCK()) return;                /* 本次可读事件处理完毕 */
        LOGW(MOD, "fd=%d recv 出错，关闭连接", (int)c->fd);
        conn_close(c);
        return;
    }
}

/* ------------------------------------------------------------------------ */
/* 事件循环（平台相关）                                                        */
/* ------------------------------------------------------------------------ */

static bool should_stop(void)
{
    bool v;
    os_mutex_lock(s_stop_mu);
    v = s_should_stop;
    os_mutex_unlock(s_stop_mu);
    return v;
}

#ifdef _WIN32
static void event_loop(void)
{
    while (!should_stop()) {
        fd_set rfds, wfds;
        struct timeval tv;
        int n;
        size_t i;

        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(s_srv.listen_fd, &rfds);
        for (i = 0; i < CONN_MAX; i++) {
            http_conn_t *c = &s_srv.conns[i];
            if (!c->used) continue;
            FD_SET(c->fd, &rfds);
            if ((c->slen > c->soff) || (c->is_ws && http_ws_queue_used(c) > 0)) FD_SET(c->fd, &wfds);
        }

        tv.tv_sec = 0;
        tv.tv_usec = POLL_TIMEOUT_MS * 1000;
        n = select(0, &rfds, &wfds, NULL, &tv); /* Windows 忽略首参 nfds */
        http_ws_tick(); /* 必须在下面的超时 continue 之前，否则空闲连接永远等不到心跳检查 */
        if (n <= 0) continue; /* 超时或偶发错误：回到循环头重新检查停止标志 */

        if (FD_ISSET(s_srv.listen_fd, &rfds)) accept_new_conn();

        for (i = 0; i < CONN_MAX; i++) {
            http_conn_t *c = &s_srv.conns[i];
            if (!c->used) continue;
            if (FD_ISSET(c->fd, &wfds)) {
                conn_flush_send(c);
                if (!c->used) continue;
                /* 必须等 c->sbuf 彻底发空（slen==soff）才能开始发 WS 帧字节：
                 * 101 升级响应正是先入队到 sbuf、is_ws 才置真，若 sbuf 还剩
                 * 半截未发就在这里插入 WS 帧，会把两路字节交错进同一个
                 * socket，产生对端无法解析的畸形流（Important 2，评审指出
                 * 是 integration-notes 本身的设计缺陷，这里按裁定原地修复）。 */
                if (c->is_ws && c->slen == c->soff) { http_ws_flush(c); if (!c->used) continue; }
            }
            if (FD_ISSET(c->fd, &rfds)) conn_handle_readable(c);
        }
    }
}
#else
static void event_loop(void)
{
    struct epoll_event events[CONN_MAX + 1];

    while (!should_stop()) {
        int n = epoll_wait(s_srv.epfd, events, CONN_MAX + 1, POLL_TIMEOUT_MS);
        int i;

        http_ws_tick(); /* 必须在下面的 continue 之前，否则空闲连接永远等不到心跳检查 */
        if (n < 0) continue; /* EINTR 等偶发错误：回到循环头重新检查停止标志 */

        for (i = 0; i < n; i++) {
            http_conn_t *c;

            if (events[i].data.ptr == &s_listen_tag) {
                accept_new_conn();
                continue;
            }
            c = (http_conn_t *)events[i].data.ptr;
            if (!c->used) continue; /* 防御：本轮事件批次内该连接已被关闭 */

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                conn_close(c);
                continue;
            }
            if (events[i].events & EPOLLOUT) {
                conn_flush_send(c);
                if (!c->used) continue;
                /* 必须等 c->sbuf 彻底发空（slen==soff）才能开始发 WS 帧字节：
                 * 101 升级响应正是先入队到 sbuf、is_ws 才置真，若 sbuf 还剩
                 * 半截未发就在这里插入 WS 帧，会把两路字节交错进同一个
                 * socket，产生对端无法解析的畸形流（Important 2，评审指出
                 * 是 integration-notes 本身的设计缺陷，这里按裁定原地修复）。 */
                if (c->is_ws && c->slen == c->soff) { http_ws_flush(c); if (!c->used) continue; }
            }
            if (events[i].events & EPOLLIN) conn_handle_readable(c);
        }
    }
}
#endif

static void server_thread_fn(void *arg)
{
    (void)arg;
    event_loop();
}

/* ------------------------------------------------------------------------ */
/* 启动 / 停止                                                                */
/* ------------------------------------------------------------------------ */

hal_err_t http_server_start(uint16_t port)
{
    sock_t fd;
    struct sockaddr_in addr;
    int reuse = 1;

    if (s_srv.thread) {
        LOGW(MOD, "http_server 已在运行，忽略重复启动");
        return HAL_ESTATE;
    }

#ifdef _WIN32
    {
        WSADATA wsa;
        int wr = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (wr != 0) {
            LOGE(MOD, "WSAStartup 失败: %d", wr);
            return HAL_EIO;
        }
    }
#endif

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == SOCK_INVALID) {
        LOGE(MOD, "创建监听 socket 失败");
#ifdef _WIN32
        WSACleanup();
#endif
        return HAL_EIO;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOGE(MOD, "绑定端口 %u 失败", (unsigned)port);
        CLOSESOCK(fd);
#ifdef _WIN32
        WSACleanup();
#endif
        return HAL_EIO;
    }
    if (listen(fd, 16) != 0) {
        LOGE(MOD, "监听失败");
        CLOSESOCK(fd);
#ifdef _WIN32
        WSACleanup();
#endif
        return HAL_EIO;
    }
    if (set_nonblocking(fd) != HAL_OK) {
        LOGE(MOD, "设置监听 socket 为非阻塞失败");
        CLOSESOCK(fd);
#ifdef _WIN32
        WSACleanup();
#endif
        return HAL_EIO;
    }

    if (!s_stop_mu) {
        s_stop_mu = os_mutex_create();
        if (!s_stop_mu) {
            LOGE(MOD, "创建停止标志互斥锁失败");
            CLOSESOCK(fd);
#ifdef _WIN32
            WSACleanup();
#endif
            return HAL_ENOMEM;
        }
    }
    s_should_stop = false;

    memset(s_srv.conns, 0, sizeof(s_srv.conns));
    s_srv.listen_fd = fd;

#ifndef _WIN32
    s_srv.epfd = epoll_create1(0);
    if (s_srv.epfd < 0) {
        LOGE(MOD, "epoll_create1 失败");
        CLOSESOCK(fd);
        s_srv.listen_fd = SOCK_INVALID;
        return HAL_EIO;
    }
    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLET;
        ev.data.ptr = &s_listen_tag;
        epoll_ctl(s_srv.epfd, EPOLL_CTL_ADD, fd, &ev);
    }
#endif

    s_srv.thread = os_thread_create(server_thread_fn, NULL, "http", 64);
    if (!s_srv.thread) {
        LOGE(MOD, "创建 http_server 事件循环线程失败");
        CLOSESOCK(fd);
        s_srv.listen_fd = SOCK_INVALID;
#ifndef _WIN32
        close(s_srv.epfd);
        s_srv.epfd = -1;
#endif
#ifdef _WIN32
        WSACleanup();
#endif
        return HAL_EIO;
    }

    LOGI(MOD, "http_server 已启动，端口 %u", (unsigned)port);
    return HAL_OK;
}

hal_err_t http_server_stop(void)
{
    size_t i;

    if (!s_srv.thread) return HAL_ESTATE;

    os_mutex_lock(s_stop_mu);
    s_should_stop = true;
    os_mutex_unlock(s_stop_mu);

    os_thread_join(s_srv.thread);
    s_srv.thread = NULL;

    for (i = 0; i < CONN_MAX; i++) {
        if (s_srv.conns[i].used) conn_close(&s_srv.conns[i]);
    }

    CLOSESOCK(s_srv.listen_fd);
    s_srv.listen_fd = SOCK_INVALID;

#ifndef _WIN32
    if (s_srv.epfd >= 0) { close(s_srv.epfd); s_srv.epfd = -1; }
#endif
#ifdef _WIN32
    WSACleanup();
#endif

    LOGI(MOD, "http_server 已停止");
    return HAL_OK;
}
