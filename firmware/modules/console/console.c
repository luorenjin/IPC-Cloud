/**
 * @file console.c
 * @brief 本地 Web 管理端模块：生命周期、路由注册、错误响应
 *
 * 声明 threads=0：复用 http_server 的 epoll 线程，因此所有 handler 必须非阻塞。
 */
#include "console_internal.h"
#include "core/module.h"
#include <stdio.h>

static bool s_routes_registered;

int console_http_status(hal_err_t e)
{
    switch (e) {
    case HAL_OK:        return 200;
    case HAL_EINVAL:    return 400;
    case HAL_EUNAUTH_:  return 401;
    case HAL_EPERM_:    return 403;
    case HAL_ENODEV:    return 404;
    case HAL_EBUSY:     return 409;
    /* 能力不存在：前端据此隐藏菜单。不可退化为 404，否则无法区分"无此功能"与"路径错误" */
    case HAL_ENOTSUP:   return 501;
    default:            return 500;
    }
}

const char *console_err_msg(hal_err_t e)
{
    switch (e) {
    case HAL_OK:        return "成功";
    case HAL_EINVAL:    return "参数非法";
    case HAL_EUNAUTH_:  return "未登录或会话已过期";
    case HAL_EPERM_:    return "无权限：请先修改初始密码";
    case HAL_ENODEV:    return "资源不存在";
    case HAL_EBUSY:     return "资源被占用，请稍后重试";
    case HAL_ENOTSUP:   return "当前设备不支持该功能";
    case HAL_ETIMEOUT:  return "操作超时";
    case HAL_ENOMEM:    return "内存不足";
    case HAL_ECORRUPT:  return "数据校验失败";
    default:            return "内部错误";
    }
}

hal_err_t console_reply_err(http_conn_t *c, hal_err_t e)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"code\":%d,\"msg\":\"%s\"}", (int)e, console_err_msg(e));
    return http_respond_json(c, console_http_status(e), body);
}

static bool console_enabled(void)
{
    /* 本地控制台为固件必备能力（决策记录已冻结），恒开 */
    return true;
}

static hal_err_t console_init(void)
{
    hal_err_t e;
    if ((e = console_auth_init()) != HAL_OK) return e;
    if ((e = console_api_init()) != HAL_OK) return e;
    s_routes_registered = true;
    return HAL_OK;
}

static hal_err_t console_start(void) { return HAL_OK; }   /* 端口由 http_server 统一监听 */
static hal_err_t console_stop(void)  { return HAL_OK; }
static hal_err_t console_deinit(void) { s_routes_registered = false; return HAL_OK; }

static hal_err_t console_health(char *detail, size_t cap)
{
    /* 只反映 console 自身：路由丢失或会话表损坏。
       刻意不纳入端口可达性（属 http_server）、有无活跃预览（零连接是正常态）、
       WiFi 状态（非 console 职责）——否则拔网线就会触发模块重启。 */
    if (!s_routes_registered) {
        snprintf(detail, cap, "路由未注册");
        return HAL_EIO;
    }
    return HAL_OK;
}

const module_desc_t mod_console = {
    .name = "console",
    .version = 1,
    .deps = NULL,
    .footprint = { .rss_kb_estimate = 64, .threads = 0 },   /* 预览/回放队列在后续任务中计入 */
    .enabled = console_enabled,
    .init = console_init,
    .start = console_start,
    .stop = console_stop,
    .deinit = console_deinit,
    .health = console_health,
};
