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

/* ---- 网络与配网 ---- */
/**
 * 是否应启动 AP。
 * eth_up      以太网链路是否 up
 * wifi_cfgd   WiFi 是否已配置过（config 中有 SSID）
 * wifi_up     WiFi 是否已连上
 * 规则：以太网 up 则绝不开 AP；否则 WiFi 未配置、或已配但未连上（超时后）开 AP。
 */
bool console_should_start_ap(bool eth_up, bool wifi_cfgd, bool wifi_up);
/** 生成 AP SSID：IPC-{序列号后6位} */
hal_err_t console_ap_ssid(const char *serial, char *buf, size_t cap);
/**
 * 从出厂验证码派生一个合规的 WPA2 PSK（评审 Important 3）。
 *
 * `hal_crypto.h` 把 `HAL_SEC_KEY_VERIFY_CODE` 文档化为"6 位验证码"，短于
 * `hal_net.h` 自己对 `wifi_ap_start` 要求的 8~63 位下限——不能原样传入。
 * 产品决策：不改验证码规范本身（会牵动机身标签、产线流程与 Task 6 的播种
 * 逻辑），改为从验证码派生一个定长前缀 + 验证码本身拼接的 PSK。刻意不用
 * 任何哈希/摘要：用户需要照着设备标签把 PSK 手动敲进手机，派生结果必须是
 * 确定的、可打印的、可抄写的——同一验证码任何时候算出的 PSK 必须完全相同，
 * 且字符集与验证码一致（可读可打的 ASCII）。标签上最终印什么由产品侧另行
 * 确认（不在本次改动范围），这里只保证派生函数本身的规则清楚、稳定。
 *
 * 规则：`"IPC" + 验证码`；若拼接结果仍不足 8 位（验证码短于文档口径的
 * 异常情况），追加固定的 '0' 字符补满 8 位，不引入随机性。code 为空、
 * out/cap 非法，或派生结果仍不满足 8~63 位（防御性兜底，正常不会触发）
 * 均返回 HAL_EINVAL；失败时 out 会被清零，不留半截结果。
 */
hal_err_t console_ap_psk_derive(const char *code, char *out, size_t cap);

/** console_net_ap_decide 的返回值：工作线程应该对 AP 采取的动作。 */
typedef enum {
    CONSOLE_NET_AP_ACTION_NONE = 0,   /**< 保持现状，什么都不做 */
    CONSOLE_NET_AP_ACTION_START = 1,  /**< 应该开 AP（当前不是 AP 模式） */
    CONSOLE_NET_AP_ACTION_STOP = 2    /**< 应该关 AP（当前是 AP 模式） */
} console_net_ap_action_t;

/**
 * AP 决策纯函数（评审 fix round 2）：把"现在该不该重新判定、宽限期是否
 * 已过、判定结果是什么"三件事里的后两件抽出来，不碰 HAL、不碰全局状态，
 * 方便用构造时间戳做 KAT——原来这部分逻辑整段焊在 net_apply_ap_decision
 * 里，只有工作线程真正运行起来才会被执行到，而测试套件从不启动工作线程。
 *
 * 内部先调 console_should_start_ap(eth_up, wifi_cfgd, wifi_up) 得到"要不要
 * 有 AP"的原始判断，再叠加宽限期覆盖：原始判断为真、且原因具体是"eth down
 * 且 wifi 未连上且 wifi 已配置"时，才会在 *grace_started_us 记录的宽限期
 * （NET_WIFI_ASSOC_GRACE_MS，工作线程侧的时间常量）内把判断压成假——给
 * supplicant 关联的时间，对应 brief"已配但未连上（超时后）"的措辞。
 * 最终把"要不要有 AP"与 cur_is_ap（调用前的实际模式是否为 AP）比较，
 * 得到应该采取的动作。
 *
 * `*grace_started_us`（IN/OUT）：0 表示当前不在宽限期内；本函数在"从其他
 * 状态进入宽限期条件"时把它设为 now_us（只设一次，同一状态里的后续调用
 * 不会重置计时起点），在"宽限期条件不再成立"（已经 up，或从未配置过）时
 * 把它清零。调用方（工作线程）只需要持有这个状态、原样传引用，不需要
 * 理解其含义。
 */
console_net_ap_action_t console_net_ap_decide(bool eth_up, bool wifi_cfgd, bool wifi_up,
                                              bool cur_is_ap, uint64_t now_us,
                                              uint64_t *grace_started_us);
/**
 * 周期复查纯函数：距离上次判定（last_check_us）是否已经过了
 * recheck_interval_ms 毫秒，到了才需要重新调用 console_net_ap_decide。
 * 只是把 net_worker_thread 里原本内联的时间戳算术单独拿出来，方便对
 * 边界值（恰好等于/差一点点）做 KAT。
 */
bool console_net_ap_recheck_due(uint64_t last_check_us, uint64_t now_us, uint32_t recheck_interval_ms);

/** 是否为手机系统的 Captive Portal 探测路径 */
bool console_is_captive_probe(const char *path);
/** 当前网络模式："ap" / "sta" / "eth" */
const char *console_net_mode(void);
/** 注册 /api/v1/net/ 路由、登记 net.wifi.ssid 配置规则；不得起线程
 *  （module.h 对 init 的契约），在 console_init 中调用。 */
hal_err_t console_net_init(void);
/**
 * 启动/停止配网工作线程（wifi_scan/wifi_connect 阻塞调用、AP 网段 DHCP 收发、
 * 配网失败 60 秒后重开 AP 的定时器均在该线程上执行）。在 console_start/
 * console_stop 中调用，对应 module.h "start 才起线程" 的契约（resolution C）。
 * 可重复 start（幂等）/ stop（未启动时视为成功）。
 */
hal_err_t console_net_start(void);
hal_err_t console_net_stop(void);

/* ---- DHCP：AP 网段 192.168.169.0/24 的极简服务端 ----
 * 报文解析/构造与租约表是纯函数，在 x86 上完全可测（见 console_test 的 KAT
 * 用例）；UDP socket 收发是平台相关的胶水层，在 console_net.c 内单独成段，
 * 未在任何环境下被执行验证——见任务报告 resolution B。 */
#define CONSOLE_DHCP_MSG_DISCOVER 1u
#define CONSOLE_DHCP_MSG_OFFER    2u
#define CONSOLE_DHCP_MSG_REQUEST  3u
#define CONSOLE_DHCP_MSG_DECLINE  4u
#define CONSOLE_DHCP_MSG_ACK      5u
#define CONSOLE_DHCP_MSG_NAK      6u
#define CONSOLE_DHCP_MSG_RELEASE  7u
#define CONSOLE_DHCP_MSG_INFORM   8u

/** 地址池 .100~.200（网关/DHCP 服务端自身固定为 .1），租期 2 小时 */
#define CONSOLE_DHCP_POOL_START   100u
#define CONSOLE_DHCP_POOL_END     200u
#define CONSOLE_DHCP_POOL_SIZE    (CONSOLE_DHCP_POOL_END - CONSOLE_DHCP_POOL_START + 1u)
#define CONSOLE_DHCP_LEASE_S      (2u * 3600u)

/** 解析/构造所需的最小 BOOTP/DHCP 字段集合（RFC 2131 §2 + RFC 2132 常用选项） */
typedef struct {
    uint8_t  op, htype, hlen;
    uint32_t xid;
    uint16_t flags;
    uint32_t ciaddr, yiaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  msg_type;      /**< 选项 53；0 表示报文中未出现（畸形请求） */
    uint32_t requested_ip;  /**< 选项 50，主机字节序；0 表示未带该选项 */
} console_dhcp_msg_t;

/**
 * 解析一个完整 UDP 载荷（BOOTP 定长头 236 字节 + 4 字节 magic cookie + 选项）。
 * 截断、魔数不符、选项长度越界均返回错误，不做越界读；buf/out 为 NULL 返回
 * HAL_EINVAL，长度不足返回 HAL_EINVAL，魔数或选项越界返回 HAL_ECORRUPT。
 */
hal_err_t console_dhcp_parse(const uint8_t *buf, size_t len, console_dhcp_msg_t *out);
/**
 * 构造应答报文：msg_type 为 OFFER/ACK 时附带选项 51(租期)/1(子网掩码)/3(网关，
 * 取 server_ip 自身)；NAK 只附带 53/54。your_ip/server_ip 为主机字节序 IPv4。
 * cap 不足以容纳完整报文时返回 HAL_ENOMEM，不写半截报文。
 */
hal_err_t console_dhcp_build_reply(const console_dhcp_msg_t *req, uint8_t msg_type,
                                   uint32_t your_ip, uint32_t server_ip, uint32_t lease_s,
                                   uint8_t *out, size_t cap, size_t *out_len);

/** 租约表条目：MAC → 地址池偏移（记录的是完整的最后一个字节，取值范围
 *  [CONSOLE_DHCP_POOL_START, CONSOLE_DHCP_POOL_END]） */
typedef struct {
    bool     used;
    uint8_t  mac[6];
    uint32_t expires_s;
} console_dhcp_lease_t;

typedef struct {
    console_dhcp_lease_t entries[CONSOLE_DHCP_POOL_SIZE];
} console_dhcp_lease_table_t;

void console_dhcp_lease_table_init(console_dhcp_lease_table_t *t);
/**
 * 取/续租正式租约（REQUEST/ACK 确认后调用）：该 MAC 已持有记录（无论是此前
 * 的正式租约还是 console_dhcp_lease_offer 留下的短时预留）则把到期时间续成
 * 完整的 CONSOLE_DHCP_LEASE_S，否则从空闲地址中分配一个（过期记录会先被
 * 回收）。host_out 是完整的最后一个字节。地址池耗尽返回 HAL_ENOMEM——刻意
 * 不驱逐活跃租约（这与鉴权表的 LRU 淘汰语义不同：驱逐会让仍在线的设备
 * 突然失联）。
 */
hal_err_t console_dhcp_lease_acquire(console_dhcp_lease_table_t *t, const uint8_t mac[6],
                                     uint32_t now_s, uint32_t *host_out);
/**
 * DISCOVER 阶段的短时预留：语义与 console_dhcp_lease_acquire 完全一致
 * （同一 MAC 复用同一地址、池满不驱逐），唯一区别是到期时间用调用方给定的
 * offer_ttl_s 而不是固定的完整租期——DISCOVER 之后若没有对应的 REQUEST
 * （即没有人再调 console_dhcp_lease_acquire 把同一 MAC 的到期时间续成完整
 * 租期），这个预留会在 offer_ttl_s 后被下一次 sweep 回收，避免只发
 * DISCOVER 不发 REQUEST 的客户端（或伪造 MAC 的攻击者）长期占满整个地址池。
 */
hal_err_t console_dhcp_lease_offer(console_dhcp_lease_table_t *t, const uint8_t mac[6],
                                   uint32_t now_s, uint32_t offer_ttl_s, uint32_t *host_out);
/** 主动释放（收到 DHCPRELEASE 时调用）；该 MAC 未持有租约返回 HAL_ENODEV */
hal_err_t console_dhcp_lease_release(console_dhcp_lease_table_t *t, const uint8_t mac[6]);

/**
 * DHCP 协议决策（纯函数，不碰 socket）：给定已解析的请求与当前租约表状态，
 * 决定要不要回复、回复的消息类型（OFFER/ACK/NAK）与分配到的 IP（主机字节序，
 * server_ip 所在 /24 网段内）。DISCOVER 走 console_dhcp_lease_offer（短时
 * 预留）；REQUEST 走 console_dhcp_lease_acquire（正式确认，requested_ip
 * 存在且与实际分配不一致时答 NAK）；RELEASE 释放记录且不回复；其余消息
 * 类型（DECLINE/INFORM 等本极简实现不处理）也不回复。
 * 返回 true 表示应该发送一个应答（*msg_type_out/*your_ip_out 均已填好，
 * NAK 时 *your_ip_out 恒为 0）；返回 false 表示什么都不用做（地址池耗尽、
 * RELEASE、或不认识的消息类型），调用方不应发送任何报文。
 */
bool console_dhcp_decide(console_dhcp_lease_table_t *t, const console_dhcp_msg_t *req,
                         uint32_t now_s, uint32_t offer_ttl_s, uint32_t lease_s,
                         uint32_t server_ip, uint8_t *msg_type_out, uint32_t *your_ip_out);

#ifdef IPC_TESTING
/** 测试桩：清空 console_net 内部运行态（扫描缓存/连接请求/last_error/
 *  AP·STA 记忆 SSID、当前模式）为初始值，不影响已注册的路由表。各测试函数
 *  之间用它隔离，避免用例顺序耦合（与 console_auth_reset_lockout 同类用途）。 */
void console_net_test_reset(void);
/**
 * 测试桩：不经 http_respond/真实连接直接跑一次 /api/v1/net/ 下的分发。
 * 返回 HAL_OK 时 body 为响应体、*http_status_out 为该响应本该使用的 HTTP
 * 状态码（200 或 202）；其他返回值即真实 handler 交给 console_reply_err
 * 的错误码，语义与 console_api_test_dispatch 一致。不会真的把配网请求交给
 * 工作线程（工作线程未必在跑），只验证参数校验与响应体/状态码。
 */
hal_err_t console_net_test_dispatch(const http_req_t *req, char *body, size_t body_cap,
                                    int *http_status_out);
/**
 * 测试桩：直接跑真正的 console_net_handler（而非绕过它的纯分发），用于钉住
 * "先入队 202 响应、再登记延后动作把连接请求交给工作线程" 这个调用顺序本身，
 * 与 console_api_test_full_handler 同一手法。req->conn 必须是真实或测试用
 * 连接（如 http_ws_test_conn_new 的返回值）。
 */
int console_net_test_full_handler(http_req_t *req);
/** 见 console_net_test_full_handler：延后动作登记失败的次数，正常调用顺序
 *  下恒为 0。 */
unsigned console_net_test_defer_fail_count(void);
/** 测试桩：直接向扫描缓存注入一条"已完成"的结果，绕开需要真实 WiFi 硬件的
 *  工作线程路径，用于测试 /net/wifi/scan 缓存命中分支的 JSON 拼装是否正确。
 *  security 取值见 hal_wifi_sec_t。 */
void console_net_test_seed_scan_result(const char *ssid, int rssi_dbm, uint32_t freq_mhz,
                                       int security);
/** 测试桩：直接把内部状态置为"AP 模式下已开启热点 ssid"，绕开需要真实 WiFi
 *  硬件的工作线程路径，用于测试 /net/status 在 AP 模式下的字段拼装。 */
void console_net_test_seed_ap(const char *ssid);
/**
 * 测试桩：读一次当前 connect_req 槽位（handler 在响应发出前写入、
 * net_connect_handoff 在响应发出后转正的那个静态槽位）。ssid_out 为该
 * 槽位当前记的 SSID（未提交过则是空串），pending_out 为其 pending 标志
 * （true 表示已被 handoff 转正、等待工作线程消费）。用于证明 handler 确实
 * 把提交的连接请求写进了这个槽位——如果 console_net_handler 内部登记
 * defer 的整段逻辑被删掉，这里读到的 ssid 会是空串。
 */
void console_net_test_peek_connect(char *ssid_out, size_t cap, bool *pending_out);
/**
 * 测试桩：直接调用 net_connect_handoff（不经 http_conn_defer_after_flush）。
 * 测试用连接（http_ws_test_conn_new）没有真实事件循环去 flush 它，defer
 * 回调因此不会自动触发；这个函数让测试能够验证该回调本身确实会把
 * console_net_test_peek_connect 读到的 pending 标志翻转为 true。
 */
void console_net_test_run_handoff(void);
/** 见 console_net_test_run_handoff：net_connect_handoff 被调用的次数，
 *  用于证明测试真的驱动到了这个回调，而不是只测了它的登记动作。 */
unsigned console_net_test_handoff_count(void);
#endif

#endif /* IPC_CONSOLE_INTERNAL_H */
