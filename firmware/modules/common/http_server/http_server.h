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

#ifdef __cplusplus
}
#endif

#endif /* IPC_HTTP_SERVER_H */
