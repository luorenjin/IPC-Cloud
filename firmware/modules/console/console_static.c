/**
 * @file console_static.c
 * @brief 控制台前端静态资源路由："/" 兜底路由
 *
 * 消费 Task 9 生成的 console_assets.c（console_asset_find/console_asset_index），
 * 把内嵌的前端 SPA 资源（index.html/app.js/style.css 等）通过 HTTP 暴露出来。
 * 路由前缀刻意注册为最低优先级："/"，必须在 console_api_init/console_net_init
 * 之后注册，避免遮蔽 /api/v1/* 系列前缀（route_lookup 是最长前缀匹配，见
 * http_server.c；即便不是，注册顺序上也把它放在最后作为防御性写法）。
 */
#include "console_internal.h"
#include "console_assets.h"
#include <string.h>
#include <stdio.h>

/**
 * 静态资源处理：最低优先级的 "/" 兜底路由。
 * 未命中的路径一律回 index.html，交由前端路由处理（SPA 约定）。
 */
static int console_static_handler(http_req_t *req, void *user)
{
    const console_asset_t *a;
    const char *inm;
    char headers[256];

    (void)user;

    /* 只处理 GET/HEAD；其余方法落到这里说明路径确实不存在 */
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
        return console_reply_err(req->conn, HAL_ENODEV);
    }

    a = console_asset_find(req->path);
    /* SPA 前端路由：未知路径（含 "/" 本身）交给前端判断，而不是回 404 */
    if (!a) a = console_asset_find("/index.html");
    if (!a) return console_reply_err(req->conn, HAL_ENODEV);

    /* 内容未变则回 304，省掉一次全量传输 */
    inm = http_header(req, "If-None-Match");
    if (inm && strstr(inm, a->etag) != NULL) {
        snprintf(headers, sizeof(headers), "ETag: \"%s\"\r\n", a->etag);
        return http_respond_ex(req->conn, 304, NULL, headers, NULL, 0);
    }

    snprintf(headers, sizeof(headers),
             "Content-Encoding: gzip\r\n"
             "ETag: \"%s\"\r\n"
             "Cache-Control: no-cache\r\n",
             a->etag);
    return http_respond_ex(req->conn, 200, a->content_type, headers, a->data, a->len);
}

hal_err_t console_static_init(void)
{
    /* 必须最后注册：这是兜底路由，不能遮蔽 /api/v1/* */
    return http_route("/", console_static_handler, NULL);
}
