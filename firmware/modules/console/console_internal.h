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

/* ---- 鉴权 ---- */
#define CONSOLE_SALT_LEN     16
#define CONSOLE_KEY_LEN      32
#define CONSOLE_ITER         4096   /**< 低端 SoC 上需压在 200ms 内，故远低于 OWASP 建议值 */
#define CONSOLE_SESSION_MAX  4
#define CONSOLE_LOCK_IPS     8
#define CONSOLE_FAIL_LIMIT   5
#define CONSOLE_NONCE_MAX    4      /**< 在途挑战上限，60s 过期，满则淘汰最旧 */
#define CONSOLE_USER_MAX     32     /**< 本地账号名缓冲（含 NUL） */
#define CONSOLE_PWD_MIN      8      /**< 新口令长度下限 */
#define CONSOLE_PWD_MAX      63     /**< 新口令长度上限 */

/** 鉴权端点前缀：`console_auth_check` 的强制改密豁免与路由注册共用同一常量 */
#define CONSOLE_AUTH_PREFIX  "/api/v1/auth/"

/**
 * 无 `hal_crypto` 平台的凭据软存储文件（相对进程工作目录）。
 * 与 `hal_crypto.h` 头注释一致："core 层退化为文件系统权限保护的软存储"。
 * 无论走哪条路径，凭据都**不进** `core/config`，因此不会出现在 `cfg_dump_json`
 * 的输出里（Task 7 的 `GET /api/v1/config` 正是在其之上做前缀过滤）。
 */
#define CONSOLE_CRED_FILE    "console_local_user.bin"

hal_err_t console_pbkdf2_sha256(const char *pwd, size_t pwd_len,
                                const uint8_t *salt, size_t salt_len,
                                uint32_t iter, uint8_t *out, size_t out_len);
hal_err_t console_hmac_sha256(const uint8_t *key, size_t key_len,
                              const uint8_t *msg, size_t msg_len, uint8_t out[32]);

/** 以出厂验证码播种凭据（首次启动或恢复出厂后调用） */
hal_err_t console_auth_seed(const char *factory_code);
/** 取挑战：salt 十六进制串 + 一次性 nonce（60s 过期） */
hal_err_t console_auth_challenge(const char *user, char *salt_hex, size_t salt_cap,
                                 char *nonce, size_t nonce_cap);
/** 校验 proof；nonce 用后即废 */
hal_err_t console_auth_verify(const char *user, const char *nonce, const char *proof);
/** 带来源 IP 的校验（用于锁定计数） */
hal_err_t console_auth_verify_from(const char *user, const char *nonce, const char *proof,
                                   const char *client_ip);
/** 客户端侧 proof 计算（浏览器用 Web Crypto，本函数供测试与自检） */
hal_err_t console_auth_make_proof(const char *pwd, const char *salt_hex, const char *nonce,
                                  char *proof, size_t proof_cap);
bool      console_auth_must_change(void);
hal_err_t console_auth_set_password(const char *old_pwd, const char *new_pwd);
/** 校验请求中的会话 token；未登录返回 HAL_EUNAUTH_，未改密返回 HAL_EPERM_ */
hal_err_t console_auth_check(const http_req_t *req);

#ifdef IPC_TESTING
void console_auth_reset_lockout(void);
/** 测试桩：丢弃凭据内存缓存，强制下次访问重新从存储读回 */
void console_auth_test_reload(void);
/**
 * 测试桩：不经 http_respond 直接跑一次 `/api/v1/auth/*` 分发。
 * 返回 HAL_OK 时 body 为 200 响应体、set_cookie 为 Set-Cookie 值（无则为空串）；
 * 其他返回值即真实 handler 交给 `console_reply_err` 的错误码。
 */
hal_err_t console_auth_test_dispatch(const http_req_t *req, char *body, size_t body_cap,
                                     char *set_cookie, size_t cookie_cap);
#endif

#endif /* IPC_CONSOLE_INTERNAL_H */
