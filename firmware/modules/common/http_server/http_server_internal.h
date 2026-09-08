/**
 * @file http_server_internal.h
 * @brief http_server 模块内部共享定义——仅 http_server.c 与 http_ws.c 可见
 *
 * 不安装、不被其他模块 include；http_conn_t 对外仍是 http_server.h 里的不透明
 * 指针，这里的完整布局只在本模块内部两个翻译单元之间共享。
 */
#ifndef IPC_HTTP_SERVER_INTERNAL_H
#define IPC_HTTP_SERVER_INTERNAL_H

#include "http_server.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define WOULD_BLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <sys/socket.h>
#include <errno.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define WOULD_BLOCK() (errno == EWOULDBLOCK || errno == EAGAIN)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** 服务端连接槽位总数上限，http_server.c 与 http_ws.c 共享同一份定义——此前
 *  两个文件各自独立声明同值的 16，评审指出这是一处跨翻译单元的复制隐患
 *  （改一处忘改另一处会静默产生不一致），移到这里从源头消除。已升级 WS 的
 *  连接数不可能超过总连接数，http_ws.c 的心跳登记表直接复用本宏。 */
#define CONN_MAX 16

struct http_ws_state; /* 完整定义与全部字段留在 http_ws.c 内部，本头不关心其布局 */

/** 对端地址字符串缓冲：足够容纳 IPv6 文本形式（INET6_ADDRSTRLEN=46，含 NUL） */
#define CONN_PEER_IP_MAX 46

struct http_conn {
    sock_t fd;
    int    used;       /**< 是否被占用（1=占用） */
    char   peer_ip[CONN_PEER_IP_MAX]; /**< accept 时记录的对端地址，只读；取不到时为空串 */
    char  *rbuf;        /**< 接收缓冲，容量 CONN_BUF_MAX，accept 时分配、关闭时释放 */
    size_t rlen;        /**< rbuf 内已接收但尚未解析完的字节数 */
    char  *sbuf;        /**< 发送队列缓冲，realloc 增长，全部发送完毕后回收 */
    size_t scap;        /**< sbuf 容量 */
    size_t slen;        /**< 已入队字节数 */
    size_t soff;        /**< 已发送字节数（soff <= slen） */
    bool   is_ws;       /**< 是否已完成 WS 升级 */
    struct http_ws_state *ws; /**< is_ws 时非 NULL；分配/释放均由 http_ws.c 负责 */
    /** resolution B（Task 7）："先响应再动作"钩子：conn_flush_send 观察到本连接
     *  排队数据已全部交给内核（slen==soff）后调用一次并清空这两个字段；
     *  若连接在触发前被 conn_close，同样清空但不调用。字段直接放在结构体里
     *  （而非按连接指针做键的旁表）：s_srv.conns[] 槽位会被 accept_new_conn
     *  复用，旁表一旦漏清就会把回调错误地继承给复用同一槽位的下一条连接；
     *  结构体字段则天然享受 accept_new_conn 里已有的 memset(slot, 0, ...)，
     *  不需要新增任何清理路径。完整契约见 http_server.h 的
     *  http_conn_defer_after_flush 声明注释。 */
    void (*after_flush)(void *arg);
    void  *after_flush_arg;
};

/* ---- http_server.c 定义，供 http_ws.c 调用 ---- */

/** 环形缓冲有新待发字节后调用，标记可写兴趣（EPOLLOUT 或下一轮 select 纳入
 *  writefds）。可从任意线程调用：epoll_ctl 允许与 epoll_wait 所在线程并发；
 *  Windows 侧本身是每轮重算 fd_set 的空操作，调用它是安全的空操作。 */
void conn_update_poll_interest(http_conn_t *c);

/** 关闭并回收连接（epoll_ctl DEL + closesocket + 释放 rbuf/sbuf + 清零复用）。
 *  只能从事件循环线程调用，不是线程安全的——http_ws_send 等任意线程入口绝不可
 *  调用它；WS 侧只能在 http_ws_on_readable/http_ws_flush/http_ws_tick（均已在
 *  事件循环线程内执行）里因协议原因（收到 close 帧、send 硬错误、心跳超时）
 *  调用它。 */
void conn_close(http_conn_t *c);

/* ---- http_ws.c 定义，供 http_server.c 调用 ---- */

/** c->rbuf[0..c->rlen) 内有新收到的字节时调用，替代 http_parse_request 路径。
 *  内部按 WS 帧边界解析、消费（memmove 移除已处理字节，手法与现有 HTTP 路径
 *  一致），ping/pong/close 控制帧与文本帧回调在这里触发。 */
void http_ws_on_readable(http_conn_t *c);

/** 可写事件里、c->is_ws 为真时额外调用（与既有 conn_flush_send(c) 并存，不是
 *  二选一：101 升级响应仍走 c->sbuf 旧路径发送，只有升级完成后的 WS 帧才走这里）。
 *  内部把环形缓冲里待发字节 send() 出去，WOULD_BLOCK 则原样返回等下次可写事件，
 *  硬错误则 conn_close(c)。 */
void http_ws_flush(http_conn_t *c);

/** conn_close 现有的 free(c->rbuf)/free(c->sbuf) 之前、c->is_ws 为真时调用：
 *  释放环形缓冲内存与锁，避免泄漏。 */
void http_ws_conn_cleanup(http_conn_t *c);

/** 事件循环每轮无条件调用一次（两个平台各自的 while 循环体内各加一行，紧跟
 *  epoll_wait/select 调用之后、且不能被"本轮超时无事件就 continue"的分支挡住，
 *  否则空闲连接永远等不到心跳检查）。内部自行维护"已升级连接"登记表（不依赖
 *  http_server.c 暴露任何遍历接口），检查各连接上次 ping/pong 时间戳，到点发
 *  ping、连续两次无 pong 则 conn_close 断开。零 WS 连接时应是廉价空操作。 */
void http_ws_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* IPC_HTTP_SERVER_INTERNAL_H */
