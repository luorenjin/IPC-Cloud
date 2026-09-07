/**
 * @file console_internal.h
 * @brief console 模块内部共享声明（不对外导出）
 */
#ifndef IPC_CONSOLE_INTERNAL_H
#define IPC_CONSOLE_INTERNAL_H

#include "hal/hal_types.h"
#include "modules/common/http_server/http_server.h"

/** console 内部错误码：鉴权失败（HAL 无对应码，不污染 HAL 命名空间） */
#define HAL_EPERM_        (-100)
/** 未登录（区别于已登录但权限不足） */
#define HAL_EUNAUTH_      (-101)

/** hal_err_t → HTTP 状态码 */
int console_http_status(hal_err_t e);
/** 统一错误响应：{"code":<e>,"msg":"中文说明"} */
hal_err_t console_reply_err(http_conn_t *c, hal_err_t e);
/** 错误码对应的中文说明 */
const char *console_err_msg(hal_err_t e);

/* 各子模块的路由注册入口（在 console_init 中调用） */
hal_err_t console_auth_init(void);
hal_err_t console_api_init(void);

#endif /* IPC_CONSOLE_INTERNAL_H */
