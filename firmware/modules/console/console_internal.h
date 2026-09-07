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
/**
 * 新口令长度上下限。**只对 `console_auth_set_password()`（测试与本地调用者）生效**；
 * 生产端点 `POST /api/v1/auth/password` 走掩码路径，服务端看不到明文，
 * 无法在此强制——长度/复杂度由前端把关。后续任务不要误以为服务端有这层保护。
 */
#define CONSOLE_PWD_MIN      8
#define CONSOLE_PWD_MAX      63

/** 鉴权端点前缀：`console_auth_check` 的强制改密豁免与路由注册共用同一常量 */
#define CONSOLE_AUTH_PREFIX  "/api/v1/auth/"

/**
 * 无 `hal_crypto` 平台的凭据软存储文件名，置于私有数据目录下
 * （目录取 `cfg_get_str("system.data_dir")`，缺省见 `console_auth.c`；
 * 目录 0700 / 文件 0600，POSIX 下显式 chmod）。
 * 与 `hal_crypto.h` 头注释一致："core 层退化为文件系统权限保护的软存储"。
 * 无论走哪条路径，凭据都**不进** `core/config`，因此不会出现在 `cfg_dump_json`
 * 的输出里（Task 7 的 `GET /api/v1/config` 正是在其之上做前缀过滤）。
 */
#define CONSOLE_CRED_FILE    "local_user.bin"
/** 私有数据目录的配置键（R3：路径不硬编码，从配置取） */
#define CONSOLE_DATA_DIR_KEY "system.data_dir"

/** 兜底软存储的完整路径。返回**内部静态缓冲**，下一次调用即被重写；
 *  需要跨调用持有请自行拷贝。供日志与测试使用。 */
const char *console_auth_cred_path(void);

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
/** 带来源 IP 的取挑战：nonce 表满时优先淘汰**同一 IP** 的最旧条目，
 *  使未鉴权攻击者无法用几个请求挤掉他人在途的 challenge */
hal_err_t console_auth_challenge_from(const char *user, char *salt_hex, size_t salt_cap,
                                      char *nonce, size_t nonce_cap, const char *client_ip);
/** 校验 proof；nonce 用后即废 */
hal_err_t console_auth_verify(const char *user, const char *nonce, const char *proof);
/** 带来源 IP 的校验（用于锁定计数） */
hal_err_t console_auth_verify_from(const char *user, const char *nonce, const char *proof,
                                   const char *client_ip);
/** 客户端侧 proof 计算（浏览器用 Web Crypto，本函数供测试与自检） */
hal_err_t console_auth_make_proof(const char *pwd, const char *salt_hex, const char *nonce,
                                  char *proof, size_t proof_cap);
bool      console_auth_must_change(void);
/** 明文改密：供本地/测试路径使用；HTTP 端点走下面的掩码版，口令不上线 */
hal_err_t console_auth_set_password(const char *old_pwd, const char *new_pwd);
/**
 * 掩码改密（口令不出浏览器）：客户端自选 new_salt、本地算
 * `new_key = PBKDF2(新口令, new_salt, iter)`，以
 * `masked = new_key XOR HMAC(old_key, "pwdchg|" || nonce)` 上送，服务端用自己持有的
 * stored_key 解掩码。`proof` 与登录同构，**必须先验通过**才解掩码——否则任何人推一串
 * 随机字节就能把凭据改成谁都不知道的值（DoS）。
 *
 * `masked_chk_hex` 是"新旧口令不得相同"的判据：客户端另用**旧盐**算
 * `chk = PBKDF2(新口令, old_salt, iter)`，以第三条域分隔掩码
 * `HMAC(old_key, "pwdchk|" || nonce)` 遮蔽后上送；服务端解出后与 stored_key 恒定时间
 * 比对，相等即判"口令未变"并拒绝。三条掩码必须互不相同，否则同一 nonce 下两个明文
 * 共用掩码，观察者直接得到二者异或。
 *
 * new_salt_hex 为 32 位小写十六进制（16 字节），masked_key_hex 与 masked_chk_hex
 * 均为 64 位（32 字节）。
 */
hal_err_t console_auth_set_key_masked(const char *user, const char *nonce, const char *proof,
                                      const char *new_salt_hex, const char *masked_key_hex,
                                      const char *masked_chk_hex, const char *client_ip);
/** 校验请求中的会话 token；未登录返回 HAL_EUNAUTH_，未改密返回 HAL_EPERM_ */
hal_err_t console_auth_check(const http_req_t *req);

#ifdef IPC_TESTING
void console_auth_reset_lockout(void);
/** 测试桩：丢弃凭据内存缓存，强制下次访问重新从存储读回 */
void console_auth_test_reload(void);
/** 测试桩：只跑"装载凭据 + 出厂自举"这两步，不注册路由。
 *  `http_route` 无脑追加、不去重，而 `ROUTE_MAX` 只有 8——测试里反复调
 *  `console_auth_init()` 会把槽位烧给同一个前缀，后续任务在同一测试二进制里
 *  注册自己的路由时会莫名其妙拿到 HAL_ENOMEM。 */
void console_auth_test_bootstrap(void);
/**
 * 测试桩：不经 http_respond 直接跑一次 `/api/v1/auth/*` 分发。
 * 返回 HAL_OK 时 body 为 200 响应体、set_cookie 为 Set-Cookie 值（无则为空串）；
 * 其他返回值即真实 handler 交给 `console_reply_err` 的错误码。
 */
hal_err_t console_auth_test_dispatch(const http_req_t *req, const char *client_ip,
                                     char *body, size_t body_cap,
                                     char *set_cookie, size_t cookie_cap);
#endif

/* ---- REST ---- */
/**
 * 供 console_api_init 调用、也供测试直接调用。当前不注册任何规则：
 * video.* 通道规则（上下界/枚举取自 profile 的 channels[].max 与
 * codecs_mask）与 image./record./net. 等通用键均已由 core/config.c 的
 * cfg_init（其内部 seed_channel + register_common_rules）统一登记，
 * 早于本函数被调用；重复登记只会成为永远不被 rule_for 命中的死代码，
 * 详见 console_api.c 里本函数的实现注释。仅为未来出现 console 独有、
 * core 未覆盖的键预留入口。
 */
hal_err_t console_api_register_rules(void);
/** 生成能力清单 JSON，供前端按能力渲染菜单 */
hal_err_t console_caps_json(char *buf, size_t cap);

#ifdef IPC_TESTING
/**
 * 测试桩：不经 http_respond/真实连接直接跑一次 /api/v1/ 下（除
 * /api/v1/auth/ 外）的分发。返回 HAL_OK 时 body 为 200 响应体；其他返回值
 * 即真实 handler 交给 console_reply_err 的错误码，语义与
 * console_auth_test_dispatch 一致。
 * deferred_out 非 NULL 时，置位表示这次调用登记了一个"先响应再动作"
 * （重启/恢复出厂）而不是内联执行——只判定"是否登记"，不会真的执行该
 * 动作；动作确实会等到响应发出后才触发，由 http_server_test 对
 * http_conn_defer_after_flush 本身的 e2e 测试覆盖，不在本模块重复验证。
 */
hal_err_t console_api_test_dispatch(const http_req_t *req, char *body, size_t body_cap,
                                    bool *deferred_out);
/**
 * 测试桩：直接跑真正的 console_api_handler（而非绕过它的 api_dispatch），
 * 用于钉住"先 http_respond_json 入队、再 http_conn_defer_after_flush 登记"
 * 这个调用顺序本身——颠倒顺序会让 http_conn_defer_after_flush 命中"此刻
 * 无待发数据"分支返回 HAL_ESTATE，被 console_api_test_defer_fail_count()
 * 的计数捕获。req->conn 必须由调用方设成一个真实或测试用连接（如
 * http_ws_test_conn_new 的返回值）——不同于 console_api_test_dispatch，
 * 这个函数会真的调用 http_respond_json/http_conn_defer_after_flush。
 */
int console_api_test_full_handler(http_req_t *req);
/** 见 console_api_test_full_handler 的用法说明：延后动作登记失败的次数，
 *  正常调用顺序下恒为 0。 */
unsigned console_api_test_defer_fail_count(void);
#endif

#endif /* IPC_CONSOLE_INTERNAL_H */
