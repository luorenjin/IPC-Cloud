/**
 * @file http_server.h
 * @brief 轻量 HTTP/1.1 + WebSocket 服务件（单线程 epoll）
 *
 * 被 console / onvif / snapshot 共用，按路由前缀分发，共用同一端口。
 * 本部件不含任何业务语义。
 *
 * 关键约束：handler 在 epoll 线程内执行，**绝不可阻塞**
 * （不做磁盘 I/O、不查数据库、不调 frame_bus_pull）。
 * 需要阻塞的工作交给自有线程，经 http_ws_send 投递。
 */
#ifndef IPC_HTTP_SERVER_H
#define IPC_HTTP_SERVER_H

#include "hal/hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_METHOD_MAX   8
#define HTTP_PATH_MAX     256
#define HTTP_QUERY_MAX    256
#define HTTP_HEADERS_MAX  24
#define HTTP_BODY_MAX     (72 * 1024)   /**< 单请求体上限；OTA 走分块，每块 64KB */

/**
 * name/value 指向解析器内部的暂存缓冲区，仅在**下一次** http_parse_request()
 * 调用之前有效；与 http_req_t.body（生命周期同调用者 buf）不同，不可跨请求缓存。
 */
typedef struct {
    const char *name;
    const char *value;
} http_header_t;

typedef struct http_conn http_conn_t;

typedef struct {
    char   method[HTTP_METHOD_MAX];
    char   path[HTTP_PATH_MAX];       /**< 已 URL 解码，不含 query */
    char   query[HTTP_QUERY_MAX];     /**< ? 之后的原始串，未解析 */
    http_header_t headers[HTTP_HEADERS_MAX]; /**< 生命周期见 http_header_t 上方注释 */
    size_t header_count;
    const char *body;                 /**< 指向调用者缓冲区，非拥有 */
    size_t body_len;
    http_conn_t *conn;                /**< 连接句柄，用于响应与 WS 升级 */
} http_req_t;

/** 取请求头，大小写不敏感；不存在返回 NULL */
const char *http_header(const http_req_t *req, const char *name);
/** 取 query 参数；不存在返回 def */
const char *http_query(const http_req_t *req, const char *key, char *buf, size_t cap, const char *def);

/**
 * 解析一个完整请求。
 * 返回 HAL_OK 并置 *consumed 为本请求消耗的字节数；
 * HAL_EAGAIN 表示数据不完整需继续收；HAL_EINVAL 表示畸形请求（调用者应关闭连接）。
 * req->body 指向 buf 内部，生命周期同 buf。
 */
hal_err_t http_parse_request(const char *buf, size_t len, http_req_t *req, size_t *consumed);

/** handler 返回值：0 表示已响应；负值为 hal_err_t，由框架转成错误响应 */
typedef int (*http_handler_fn)(http_req_t *req, void *user);

/**
 * 注册路由前缀。按**最长前缀**匹配，"/" 可作兜底。
 * 必须在 http_server_start 之前调用。前缀数上限 8。
 */
hal_err_t http_route(const char *prefix, http_handler_fn fn, void *user);

/** 查询某路径命中的前缀（测试用）；无匹配返回 NULL */
const char *http_route_match(const char *path);

/**
 * 取该连接的对端地址（accept 时记录的点分十进制 / IPv6 文本形式）。
 * 只读、不拥有，生存期同连接。c 为 NULL 或地址不可得时返回 NULL。
 * 供 handler 做按来源 IP 的限流/锁定；刻意不解析 X-Forwarded-For——
 * 设备直连局域网，采信该头等于让攻击者自选计数桶。
 */
const char *http_conn_peer_ip(const http_conn_t *c);

hal_err_t http_server_start(uint16_t port);
hal_err_t http_server_stop(void);

/** 发送响应。content_type 为 NULL 时用 application/octet-stream */
hal_err_t http_respond(http_conn_t *c, int status, const char *content_type,
                       const void *body, size_t len);
/** 发送 JSON 响应（content_type 固定 application/json; charset=utf-8） */
hal_err_t http_respond_json(http_conn_t *c, int status, const char *json);
/** 发送带额外响应头的响应；extra_headers 形如 "X-A: 1\r\nX-B: 2\r\n"，可为 NULL。
 *  1xx 状态码走精简模板（状态行 + extra_headers + 空行，不带 Content-Type/
 *  Content-Length/Connection——RFC 7230 §3.3.2 禁止 1xx 带 Content-Length），
 *  此时 body/len 会被静默忽略：1xx 没有 body 语义，也没有 Content-Length/
 *  chunked 编码供对端界定 body 边界。 */
hal_err_t http_respond_ex(http_conn_t *c, int status, const char *content_type,
                          const char *extra_headers, const void *body, size_t len);

/**
 * 登记一个"该连接当前排队的响应数据被事件循环真正 send() 完毕后"触发一次的
 * 回调，用于"先响应、再动作"场景（如重启、恢复出厂、OTA 切分区后重启）：
 * handler 应先用 http_respond_* 把响应入队，再调用本函数登记后续动作——
 * http_respond_* 只是把数据拷进该连接的发送队列，真正的 send() 只发生在
 * 事件循环的可写分支（conn_flush_send）里，调用 http_respond_* 之后数据
 * 不一定已经发出去。
 *
 * 保证的强度："已发出"仅指数据已经交给内核 socket 发送缓冲（send() 返回值
 * 覆盖到 c->slen），**不代表对端已经收到**——重启会把内核里尚未真正发出的
 * 字节一起带走，这是 TCP 与操作系统的边界，任何应用层机制都做不到更强。
 * 即便如此也远好于在 handler 里内联执行动作（此时响应可能连发送队列都
 * 还没排上）。
 *
 * 回调契约（务必遵守，否则会破坏调用它的事件循环本身）：
 *   1) 在事件循环线程内同步调用，与 handler 同一线程；
 *   2) 不可阻塞——与 handler 同一条非阻塞纪律（不做磁盘 I/O、不做网络阻塞调用）；
 *   3) 不可在回调里对本连接 c 调用 conn_close，也不可调用 http_respond 系列
 *      或 http_ws 发送函数——触发回调时 conn_flush_send 正在这条连接上执行，
 *      重入会破坏它自身正在维护的状态。需要关闭连接可以直接返回后让上层
 *      正常处理。
 *
 * 若该连接在回调触发前已经关闭（如对端提前断开），回调不会被调用——已经没有
 * 响应可送达，执行动作也不再有意义，调用方不应假设它一定会执行。
 * 同一连接重复调用以最后一次登记为准（覆盖此前尚未触发的登记，不叠加）。
 * c 为 NULL 返回 HAL_EINVAL。
 */
hal_err_t http_conn_defer_after_flush(http_conn_t *c, void (*fn)(void *arg), void *arg);

/** WS 上行文本消息回调（控制指令用），在 epoll 线程内调用，不可阻塞。
 *  text 指向内部静态复用缓冲，生存期仅到本次回调返回为止——需要跨调用
 *  保留内容必须自行复制，不可保存指针供之后使用。 */
typedef void (*http_ws_text_fn)(http_conn_t *c, const char *text, size_t len, void *user);

/**
 * 把 HTTP 连接升级为 WebSocket。
 * queue_cap 为该连接的发送队列字节上限（预览子码流建议 128*1024，主码流 512*1024）。
 * 失败返回 HAL_EINVAL（非法握手）、HAL_ENOMEM，或该连接已完成过一次升级时的
 * HAL_ESTATE。
 */
hal_err_t http_ws_upgrade(http_req_t *req, size_t queue_cap);

/**
 * 向 WS 连接发送二进制帧（仅入队，实际发送在 epoll 线程）。
 * is_key 标记该数据是否为关键帧：队列满时丢弃非关键帧直到下一个关键帧到来，
 * 避免解码器花屏。队列满且本帧被丢弃时返回 HAL_EAGAIN。
 * 可从任意线程调用（内部加锁）。
 */
hal_err_t http_ws_send(http_conn_t *c, const void *data, size_t len, bool is_key);

/** 发送文本帧（状态推送用） */
hal_err_t http_ws_send_text(http_conn_t *c, const char *text);

/** 注册上行文本消息回调 */
hal_err_t http_ws_on_text(http_conn_t *c, http_ws_text_fn fn, void *user);

/** 主动关闭 WS 连接。调用后连接会在下一次事件循环 tick 内关闭（不保证
 *  立即发生）；若此时握手的 101 响应尚未发送完毕，连接将直接断开而不
 *  发送 close 帧（对端还未收到升级成功确认，不会把这条连接当作已建立
 *  的 WS 连接，发 close 帧没有意义）。 */
hal_err_t http_ws_close(http_conn_t *c);

/** 当前队列已用字节（测试与背压统计用） */
size_t http_ws_queue_used(http_conn_t *c);

/** 该连接累计丢帧数 */
uint64_t http_ws_dropped(http_conn_t *c);

#ifdef IPC_TESTING
/** 测试桩：创建不含真实 socket 的 WS 连接 */
http_conn_t *http_ws_test_conn_new(size_t queue_cap);
void         http_ws_test_conn_free(http_conn_t *c);
/** 测试桩：清空发送队列，模拟数据已发出 */
void         http_ws_test_drain(http_conn_t *c);
/** 测试桩：从发送队列头部读出并移除最多 n 字节到 out，返回实际读出字节数；
 *  与 http_ws_test_drain（整体清零、绕回逻辑不会被跑到）不同，保留 head 的
 *  真实推进量，用于构造真正跨越环形缓冲 cap 边界的写入/读出，断言绕回后
 *  字节内容正确（而不只是长度）。 */
size_t       http_ws_test_read_drain(http_conn_t *c, void *out, size_t n);
/** 测试桩：暴露握手 Accept 值计算，供 RFC 6455 §1.3 已知答案测试直接校验
 *  SHA-1 + Base64 + GUID 拼接，不需要构造真实 socket 握手。 */
hal_err_t    http_ws_test_compute_accept(const char *client_key, char *out, size_t out_cap);
#endif

#ifdef __cplusplus
}
#endif

#endif /* IPC_HTTP_SERVER_H */
