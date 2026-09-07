/**
 * @file console_net.c
 * @brief console 网络子模块：网络状态、WiFi 配网（AP 热点 + Captive Portal）
 *
 * 背景：设备未联网（或 WiFi 未配置/连接失败）时开启 AP 热点，手机连上后由
 * Captive Portal 自动弹出配网页；提交新 WiFi 后当前连接必断，因此
 * `POST /api/v1/net/wifi/connect` 立即回 202，真正的「停 AP → wifi_connect →
 * 等关联/DHCP」在本文件自己的工作线程里执行（resolution C）：
 * `core/event_bus.h` 没有任何定时/延迟执行设施，`wifi_scan`/`wifi_connect`
 * 又都是阻塞调用，唯一能把它们挪出 http_server 事件循环线程的手段是
 * `os_thread_create`。`mod_console` 的 `footprint.threads` 因此从 0 改为 1
 * （见 console.c）。
 *
 * 线程模型：
 *   - `s_netst`（模式/扫描缓存/连接请求/last_error 等）由 `s_netst_mu` 保护，
 *     handler（http_server 事件循环线程）与工作线程（本文件的
 *     `net_worker_thread`）都可能并发访问。
 *   - `console_net_init()` 只注册配置规则与路由、不起线程（module.h 对
 *     init 的契约）；线程在 `console_net_start()`（对应 module.h 的 start）
 *     里创建，在 `console_net_stop()` 里置 stop 标志、广播、join，可重复
 *     start/stop。
 *   - `net_sync_ensure()` 的懒初始化不是线程安全的，但本模块的调用序列
 *     保证它只会在单线程的模块初始化阶段第一次被触发（`console_net_init`
 *     必然先于 `console_net_start` 执行，此时工作线程还不存在；HTTP
 *     handler 也要等 `http_server_start` 之后才可能被调用，同样晚于
 *     `console_net_init`），不存在两个线程同时首次调用的竞态。
 *
 * DHCP：AP 网段固定 192.168.169.0/24，本文件内置一个只服务这一个 /24 的
 * 极简 DHCP 服务端。报文解析/构造（RFC 2131/2132）与租约表管理是纯函数
 * （二、三节），在 x86 + mock 上用已知字节向量完全测试；真正的 UDP
 * socket 收发（六节）是平台相关的胶水层，本机（x86 + mock，无真实网卡/
 * WiFi）从未执行过，见任务报告 resolution B 的说明。
 *
 * 日志脱敏（规则 R8）：AP 的 PSK 就是出厂验证码，与 Web 登录同值——任何日志、
 * 任何 /net/status 响应里都不得出现它；客户端提交的 WiFi 口令同理，
 * ep_net_wifi_connect 的日志只打 SSID，从不打 psk 字段。
 */
#include "console_internal.h"
#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "core/os.h"
#include "hal/hal.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD "console"

/** 带截断检测的格式化：截断即返回 HAL_ENOMEM 并把 out 置空串（与
 *  console_auth.c/console_api.c 的同名同语义 helper 各自独立一份，三个
 *  .c 互不引用对方的 static 符号）。 */
static hal_err_t fmt_safe(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!out || cap == 0) return HAL_EINVAL;
    va_start(ap, fmt);
    n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return HAL_ENOMEM; }
    return HAL_OK;
}

/** 清零敏感缓冲；用 volatile 指针写，避免被优化掉（与 console_auth.c 的
 *  secure_wipe 同语义，独立一份） */
static void secure_wipe_local(void *p, size_t n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;
    while (n--) *q++ = 0;
}

/* ==========================================================================
 * 一、纯判定/生成函数（无副作用，brief Step 1 的测试直接覆盖）
 * ========================================================================== */

bool console_should_start_ap(bool eth_up, bool wifi_cfgd, bool wifi_up)
{
    if (eth_up) return false;   /* 有线可达：绝不开 AP */
    if (wifi_up) return false;  /* WiFi 已连上：无需 AP */
    /* 走到这里意味着"未配置"与"已配但未连上（超时后）"两种情况，结论相同
       （都需要开 AP 作救济路径），wifi_cfgd 本身不改变结果——保留该参数
       只为接口对称与调用方语义清晰（调用方仍需要算出 wifi_cfgd 才能确定
       "未连上"是否值得开 AP 救济，而不是刚上电还没来得及连）。 */
    (void)wifi_cfgd;
    return true;
}

hal_err_t console_ap_ssid(const char *serial, char *buf, size_t cap)
{
    size_t len, tail_len;
    const char *tail;

    if (!serial || !buf || cap == 0) return HAL_EINVAL;
    len = strlen(serial);
    tail_len = len >= 6 ? 6 : len;
    tail = serial + (len - tail_len);
    return fmt_safe(buf, cap, "IPC-%s", tail);
}

bool console_is_captive_probe(const char *path)
{
    if (!path) return false;
    return strcmp(path, "/generate_204") == 0 ||
           strcmp(path, "/hotspot-detect.html") == 0;
}

/* ==========================================================================
 * 二、DHCP 报文解析/构造（纯函数：不碰 socket，不持有任何全局状态）
 * ========================================================================== */

#define DHCP_FIXED_LEN  236u
#define DHCP_COOKIE_LEN 4u
#define DHCP_MIN_LEN    (DHCP_FIXED_LEN + DHCP_COOKIE_LEN)   /* 240：定长头 + magic cookie */

static uint32_t dhcp_rd_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t dhcp_rd_u16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
static void dhcp_wr_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void dhcp_wr_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

hal_err_t console_dhcp_parse(const uint8_t *buf, size_t len, console_dhcp_msg_t *out)
{
    size_t off;

    if (!buf || !out) return HAL_EINVAL;
    if (len < DHCP_MIN_LEN) return HAL_EINVAL;   /* 连定长头 + cookie 都放不下：截断 */

    memset(out, 0, sizeof(*out));
    out->op    = buf[0];
    out->htype = buf[1];
    out->hlen  = buf[2];
    out->xid   = dhcp_rd_u32be(buf + 4);
    out->flags = dhcp_rd_u16be(buf + 10);
    out->ciaddr = dhcp_rd_u32be(buf + 12);
    out->yiaddr = dhcp_rd_u32be(buf + 16);
    out->giaddr = dhcp_rd_u32be(buf + 24);
    memcpy(out->chaddr, buf + 28, sizeof(out->chaddr));

    if (buf[236] != 0x63 || buf[237] != 0x82 || buf[238] != 0x53 || buf[239] != 0x63)
        return HAL_ECORRUPT;   /* magic cookie 不符 */

    off = DHCP_MIN_LEN;
    while (off < len) {
        uint8_t code = buf[off++];
        uint8_t olen;

        if (code == 0x00) continue;   /* pad */
        if (code == 0xFF) break;      /* end */
        if (off >= len) return HAL_ECORRUPT;   /* 长度字节本身越界 */
        olen = buf[off++];
        if (off + olen > len) return HAL_ECORRUPT;   /* 选项体声称的长度超出报文剩余字节 */

        switch (code) {
        case 53: if (olen >= 1) out->msg_type = buf[off]; break;
        case 50: if (olen == 4) out->requested_ip = dhcp_rd_u32be(buf + off); break;
        default: break;   /* 55(参数请求列表)、12(主机名) 等当前不需要，跳过 */
        }
        off += olen;
    }
    return HAL_OK;
}

hal_err_t console_dhcp_build_reply(const console_dhcp_msg_t *req, uint8_t msg_type,
                                   uint32_t your_ip, uint32_t server_ip, uint32_t lease_s,
                                   uint8_t *out, size_t cap, size_t *out_len)
{
    bool with_lease = (msg_type == CONSOLE_DHCP_MSG_OFFER || msg_type == CONSOLE_DHCP_MSG_ACK);
    size_t need = DHCP_MIN_LEN + 3u /* 53 */
                + (with_lease ? 6u : 0u) /* 51 */
                + 6u /* 54 */
                + (with_lease ? 6u : 0u) /* 1 子网掩码 */
                + (with_lease ? 6u : 0u) /* 3 网关 */
                + 1u; /* end */
    size_t off;

    if (!req || !out || !out_len) return HAL_EINVAL;
    if (cap < need) return HAL_ENOMEM;   /* 缓冲不足：不写半截报文 */

    memset(out, 0, DHCP_FIXED_LEN);
    out[0] = 2;   /* BOOTREPLY */
    out[1] = req->htype ? req->htype : 1;
    out[2] = req->hlen ? req->hlen : 6;
    out[3] = 0;
    dhcp_wr_u32be(out + 4, req->xid);
    dhcp_wr_u16be(out + 10, req->flags);         /* 回显广播标志位 */
    /* ciaddr(12)/siaddr(20) 保持 0：初次配网场景客户端尚未持有地址，也没有
       下一跳引导服务器 */
    dhcp_wr_u32be(out + 16, with_lease ? your_ip : 0u);   /* NAK 不分配地址 */
    dhcp_wr_u32be(out + 24, req->giaddr);         /* 回显中继代理地址（本实现不支持中继，通常为 0） */
    memcpy(out + 28, req->chaddr, sizeof(req->chaddr));
    /* sname(44..107)/file(108..235) 保持 0 */
    out[236] = 0x63; out[237] = 0x82; out[238] = 0x53; out[239] = 0x63;

    off = DHCP_MIN_LEN;
    out[off++] = 53; out[off++] = 1; out[off++] = msg_type;
    if (with_lease) {
        out[off++] = 51; out[off++] = 4; dhcp_wr_u32be(out + off, lease_s); off += 4;
    }
    out[off++] = 54; out[off++] = 4; dhcp_wr_u32be(out + off, server_ip); off += 4;
    if (with_lease) {
        out[off++] = 1; out[off++] = 4; dhcp_wr_u32be(out + off, 0xFFFFFF00u); off += 4;   /* 255.255.255.0 */
        out[off++] = 3; out[off++] = 4; dhcp_wr_u32be(out + off, server_ip); off += 4;      /* 网关=服务端自身 */
    }
    out[off++] = 0xFF;

    *out_len = off;
    return HAL_OK;
}

/* ==========================================================================
 * 三、DHCP 租约表（纯函数：调用者持有 console_dhcp_lease_table_t 存储）
 * ========================================================================== */

void console_dhcp_lease_table_init(console_dhcp_lease_table_t *t)
{
    if (t) memset(t, 0, sizeof(*t));
}

/** 回收已过期的租约（调用者保证 t 非 NULL） */
static void dhcp_lease_sweep(console_dhcp_lease_table_t *t, uint32_t now_s)
{
    uint32_t i;
    for (i = 0; i < CONSOLE_DHCP_POOL_SIZE; i++) {
        if (t->entries[i].used && t->entries[i].expires_s <= now_s)
            t->entries[i].used = false;
    }
}

/**
 * 取/续租的共同实现：该 MAC 已有记录（无论此前是短时预留还是正式租约）
 * 则把到期时间续成 now_s+ttl_s，否则从空闲槽位分配一个；池满不驱逐。
 * console_dhcp_lease_acquire 与 console_dhcp_lease_offer 的唯一区别就是
 * 传给这里的 ttl_s（完整租期 vs 短时预留），共用同一份查找/分配逻辑，
 * 避免两份几乎相同的代码分叉维护。
 */
static hal_err_t dhcp_lease_reserve(console_dhcp_lease_table_t *t, const uint8_t mac[6],
                                    uint32_t now_s, uint32_t ttl_s, uint32_t *host_out)
{
    uint32_t i, free_idx = CONSOLE_DHCP_POOL_SIZE;

    if (!t || !mac || !host_out) return HAL_EINVAL;
    dhcp_lease_sweep(t, now_s);

    for (i = 0; i < CONSOLE_DHCP_POOL_SIZE; i++) {
        if (t->entries[i].used && memcmp(t->entries[i].mac, mac, 6) == 0) {
            t->entries[i].expires_s = now_s + ttl_s;
            *host_out = CONSOLE_DHCP_POOL_START + i;
            return HAL_OK;
        }
    }
    for (i = 0; i < CONSOLE_DHCP_POOL_SIZE; i++) {
        if (!t->entries[i].used) { free_idx = i; break; }
    }
    if (free_idx == CONSOLE_DHCP_POOL_SIZE) return HAL_ENOMEM;   /* 池已满：不驱逐活跃租约 */

    t->entries[free_idx].used = true;
    memcpy(t->entries[free_idx].mac, mac, 6);
    t->entries[free_idx].expires_s = now_s + ttl_s;
    *host_out = CONSOLE_DHCP_POOL_START + free_idx;
    return HAL_OK;
}

hal_err_t console_dhcp_lease_acquire(console_dhcp_lease_table_t *t, const uint8_t mac[6],
                                     uint32_t now_s, uint32_t *host_out)
{
    return dhcp_lease_reserve(t, mac, now_s, CONSOLE_DHCP_LEASE_S, host_out);
}

hal_err_t console_dhcp_lease_offer(console_dhcp_lease_table_t *t, const uint8_t mac[6],
                                   uint32_t now_s, uint32_t offer_ttl_s, uint32_t *host_out)
{
    return dhcp_lease_reserve(t, mac, now_s, offer_ttl_s, host_out);
}

hal_err_t console_dhcp_lease_release(console_dhcp_lease_table_t *t, const uint8_t mac[6])
{
    uint32_t i;
    if (!t || !mac) return HAL_EINVAL;
    for (i = 0; i < CONSOLE_DHCP_POOL_SIZE; i++) {
        if (t->entries[i].used && memcmp(t->entries[i].mac, mac, 6) == 0) {
            t->entries[i].used = false;
            return HAL_OK;
        }
    }
    return HAL_ENODEV;
}

/* ==========================================================================
 * 三之二、DHCP 协议决策（纯函数：ACK/NAK 判定、want_ip 拼装、DISCOVER 是否
 * 立即提交租约——这是协议策略，不是收发字节的胶水，抽出来单独做 KAT，
 * 把六节"未验证的 socket 胶水"再缩小一圈，见评审 Minor #6）
 * ========================================================================== */

bool console_dhcp_decide(console_dhcp_lease_table_t *t, const console_dhcp_msg_t *req,
                         uint32_t now_s, uint32_t offer_ttl_s, uint32_t lease_s,
                         uint32_t server_ip, uint8_t *msg_type_out, uint32_t *your_ip_out)
{
    uint32_t host = 0, want_ip;
    hal_err_t rc;

    if (!t || !req || !msg_type_out || !your_ip_out) return false;
    *your_ip_out = 0;

    switch (req->msg_type) {
    case CONSOLE_DHCP_MSG_DISCOVER:
        if (console_dhcp_lease_offer(t, req->chaddr, now_s, offer_ttl_s, &host) != HAL_OK)
            return false;   /* 地址池耗尽：不回应，客户端会重试或超时放弃 */
        *msg_type_out = (uint8_t)CONSOLE_DHCP_MSG_OFFER;
        *your_ip_out = (server_ip & 0xFFFFFF00u) | host;
        return true;
    case CONSOLE_DHCP_MSG_REQUEST:
        /* 走 dhcp_lease_reserve 而不是 console_dhcp_lease_acquire，好让调用方
           传入的 lease_s 真正生效——console_dhcp_lease_acquire 对外的公开
           契约是固定 CONSOLE_DHCP_LEASE_S，这里需要的是"调用方指定的租期"，
           两者在 lease_s==CONSOLE_DHCP_LEASE_S 时行为完全一致。 */
        rc = dhcp_lease_reserve(t, req->chaddr, now_s, lease_s, &host);
        want_ip = (server_ip & 0xFFFFFF00u) | host;
        if (rc == HAL_OK && (req->requested_ip == 0 || req->requested_ip == want_ip)) {
            *msg_type_out = (uint8_t)CONSOLE_DHCP_MSG_ACK;
            *your_ip_out = want_ip;
        } else {
            *msg_type_out = (uint8_t)CONSOLE_DHCP_MSG_NAK;   /* your_ip_out 保持 0 */
        }
        return true;
    case CONSOLE_DHCP_MSG_RELEASE:
        console_dhcp_lease_release(t, req->chaddr);
        return false;   /* RELEASE 不回复 */
    default:
        return false;   /* DECLINE/INFORM 等本极简实现不处理 */
    }
}

/* ==========================================================================
 * 四、内部运行态（AP/STA 状态机、扫描缓存、连接请求）——由 s_netst_mu 保护
 * ========================================================================== */

typedef enum { NET_MODE_ETH = 0, NET_MODE_STA = 1, NET_MODE_AP = 2 } net_mode_t;

#define NET_SCAN_MAX            16u
#define NET_SCAN_TTL_US         (10ull * 1000000ull)   /* 缓存 10 秒内的扫描结果直接复用 */
#define NET_CONNECT_WAIT_MS     20000u                 /* 等待关联+DHCP 拿到地址的最长时间 */
#define NET_AP_REOPEN_DELAY_MS  60000u                 /* brief 原文：失败 60 秒后重开 AP */
#define NET_AP_RECHECK_MS       5000u                  /* 空闲时周期复查 AP 决策的间隔（评审 Important 1） */
#define NET_WIFI_ASSOC_GRACE_MS 15000u                 /* 已配置 WiFi 但未连上时，给 supplicant 的关联宽限期 */
#define NET_DHCP_OFFER_TTL_S    30u                     /* DISCOVER 短时预留的有效期（评审 Minor #4） */
#define NET_AP_GATEWAY_IP       "192.168.169.1"
#define CONSOLE_NET_PREFIX      "/api/v1/net/"
#define CONSOLE_NET_BODY_MAX    (4u * 1024u)

typedef struct {
    char           ssid[HAL_SSID_MAX];
    int            rssi_dbm;
    uint32_t       freq_mhz;
    hal_wifi_sec_t security;
} net_ap_entry_t;

typedef struct {
    bool           in_progress;   /**< 已投递给工作线程、尚未完成 */
    bool           valid;         /**< 是否曾经有过一次成功的扫描 */
    hal_err_t      last_rc;       /**< 最近一次扫描结果（含失败），用于短路永久性 ENOTSUP */
    uint64_t       ts_us;         /**< 最近一次扫描完成时刻（单调时钟） */
    uint32_t       count;
    net_ap_entry_t aps[NET_SCAN_MAX];
} net_scan_t;

typedef struct {
    bool           pending;
    char           ssid[HAL_SSID_MAX];
    char           psk[HAL_PSK_MAX];
    hal_wifi_sec_t sec;
} net_connect_req_t;

typedef struct {
    bool               stop;
    net_mode_t         mode;
    bool               wifi_cfgd;             /**< config 中 net.wifi.ssid 是否非空 */
    char               ap_ssid[HAL_SSID_MAX]; /**< AP 模式下自己开的 SSID，供 /status 展示 */
    char               sta_ssid[HAL_SSID_MAX];/**< STA 模式下连接的 SSID（mock 平台
                                                    get_status(WIFI) 恒 ENOTSUP，读不回来，
                                                    只能自己记） */
    char               last_error[128];       /**< 最近一次配网失败原因，供 /net/status 展示 */
    net_scan_t         scan;
    net_connect_req_t  connect_req;
} net_state_t;

static net_state_t   s_netst;
static os_mutex_t   *s_netst_mu;
static os_cond_t    *s_netst_cv;
static os_thread_t  *s_netst_thread;
static bool          s_netst_sync_ready;

/** 懒初始化互斥量/条件变量。见文件头注释：只在单线程的模块初始化阶段
 *  第一次被触发，不存在竞态。 */
static void net_sync_ensure(void)
{
    if (s_netst_sync_ready) return;
    s_netst_mu = os_mutex_create();
    s_netst_cv = os_cond_create();
    s_netst_sync_ready = true;
}

/** 无 WiFi 型号判定（resolution D 原文）：caps.wifi_ap==false 或
 *  hal()->net->wifi_scan==NULL 时，所有 WiFi 端点一律 HAL_ENOTSUP（→501）。 */
static bool net_wifi_supported(void)
{
    hal_net_caps_t caps;
    if (!hal_has(HAL_MOD_NET) || !hal()->net->get_caps) return false;
    if (hal()->net->get_caps(&caps) != HAL_OK) return false;
    if (!caps.wifi_ap) return false;
    return hal()->net->wifi_scan != NULL;
}

const char *console_net_mode(void)
{
    net_mode_t m;
    net_sync_ensure();
    os_mutex_lock(s_netst_mu);
    m = s_netst.mode;
    os_mutex_unlock(s_netst_mu);
    return m == NET_MODE_AP ? "ap" : (m == NET_MODE_STA ? "sta" : "eth");
}

/* ==========================================================================
 * 五、工作线程：AP 决策、WiFi 扫描/连接（阻塞调用只在这里执行）
 * ========================================================================== */

static void net_dhcp_socket_ensure_open(void);
static void net_dhcp_socket_close_if_open(void);
static void net_dhcp_poll_once(uint32_t timeout_ms);

/** 可被 stop 提前打断的休眠；分段等待（每段 ≤1s）以便及时响应 stop。
 *  返回 true 表示等满了 total_ms，false 表示提前收到 stop。 */
static bool net_interruptible_wait(uint32_t total_ms)
{
    uint64_t deadline = os_monotonic_us() + (uint64_t)total_ms * 1000ull;
    bool stopped;

    os_mutex_lock(s_netst_mu);
    for (;;) {
        uint64_t now;
        uint32_t remain_ms;
        if (s_netst.stop) break;
        now = os_monotonic_us();
        if (now >= deadline) break;
        remain_ms = (uint32_t)((deadline - now) / 1000ull);
        if (remain_ms > 1000u) remain_ms = 1000u;
        os_cond_wait(s_netst_cv, s_netst_mu, remain_ms);
    }
    stopped = s_netst.stop;
    os_mutex_unlock(s_netst_mu);
    return !stopped;
}

/* 评审 Important 3：从出厂验证码派生一个合规 WPA2 PSK；完整规则/理由见
 * console_internal.h 的声明注释。导出（非 static）是为了能直接单元测试
 * ——它只在工作线程内的 net_apply_ap_decision 里被调用，测试套件从不
 * 启动工作线程，不导出就测不到。 */
hal_err_t console_ap_psk_derive(const char *code, char *out, size_t cap)
{
    char tmp[HAL_PSK_MAX];
    size_t len;

    if (!code || !code[0] || !out || cap == 0) return HAL_EINVAL;
    if (fmt_safe(tmp, sizeof(tmp), "IPC%s", code) != HAL_OK) return HAL_ENOMEM;

    len = strlen(tmp);
    if (len < 8) {
        static const char pad[] = "00000000";   /* 固定填充，不用随机数：结果必须可复现 */
        size_t need = 8 - len;
        if (need > sizeof(pad) - 1) { secure_wipe_local(tmp, sizeof(tmp)); return HAL_EINVAL; }
        if (fmt_safe(out, cap, "%s%.*s", tmp, (int)need, pad) != HAL_OK) {
            secure_wipe_local(tmp, sizeof(tmp));
            return HAL_ENOMEM;
        }
    } else if (fmt_safe(out, cap, "%s", tmp) != HAL_OK) {
        secure_wipe_local(tmp, sizeof(tmp));
        return HAL_ENOMEM;
    }
    secure_wipe_local(tmp, sizeof(tmp));

    len = strlen(out);
    if (len < 8 || len > 63) { secure_wipe_local(out, cap); return HAL_EINVAL; }   /* 防御性兜底 */
    return HAL_OK;
}

/**
 * 按当前链路状态决定要不要开/关 AP。调用点：工作线程启动时、周期复查时
 * （评审 Important 1，见 net_worker_thread 的 NET_AP_RECHECK_MS）、配网
 * 失败回落时。
 *
 * 只会被工作线程调用（启动一次 + 循环内周期调用 + net_do_connect 失败路径
 * 调用，三处都在同一个线程上），`s_wifi_grace_started_us` 因此不需要加锁。
 */
static uint64_t s_wifi_grace_started_us;   /* 0 表示当前不在"已配置但未连上"的宽限期内 */

static void net_apply_ap_decision(void)
{
    hal_netif_status_t st;
    bool eth_up = false, wifi_up = false, wifi_cfgd, want_ap;
    net_mode_t cur;
    uint64_t now;

    if (hal_has(HAL_MOD_NET) && hal()->net->get_status &&
        hal()->net->get_status(HAL_NETIF_ETH, &st) == HAL_OK)
        eth_up = (st.link == HAL_LINK_UP);
    if (hal_has(HAL_MOD_NET) && hal()->net->get_status &&
        hal()->net->get_status(HAL_NETIF_WIFI, &st) == HAL_OK)
        wifi_up = (st.link == HAL_LINK_UP);

    os_mutex_lock(s_netst_mu);
    wifi_cfgd = s_netst.wifi_cfgd;
    cur = s_netst.mode;
    os_mutex_unlock(s_netst_mu);

    want_ap = console_should_start_ap(eth_up, wifi_cfgd, wifi_up);

    /* 宽限期（评审 Important 1）：已配置 WiFi 但尚未连上时，给 supplicant
       一段时间完成关联，避免开机瞬间（还没来得及连）就误判成"连不上"而
       开 AP——brief 原文的限定词就是"已配但未连上（超时后）"。只有
       "eth down 且 wifi 未连上且 wifi 已配置"这一具体原因导致 want_ap 为
       真时才启用宽限期；eth up 或 wifi 已连上时 console_should_start_ap
       已经返回 false，走不到这里；wifi 从未配置过时没有什么好等的，
       直接开 AP。
       计时起点只在"从其他状态进入这个状态"时设一次（下面 else 分支里
       条件不成立就清零），并不会在这个状态里被后续调用重置——这意味着
       net_do_connect 失败路径里"等 60 秒后重开 AP"调用本函数时，如果设备
       从等待关联开始就一直处于这个状态（没有中途 up 过），宽限期早就在
       更早的周期复查里过期了，不会在 60 秒之外再叠加一段宽限期延迟。 */
    now = os_monotonic_us();
    if (want_ap && !eth_up && !wifi_up && wifi_cfgd) {
        if (s_wifi_grace_started_us == 0) s_wifi_grace_started_us = now;
        if (now - s_wifi_grace_started_us < (uint64_t)NET_WIFI_ASSOC_GRACE_MS * 1000ull)
            want_ap = false;   /* 宽限期内先不开 AP，等下一次周期复查 */
    } else {
        s_wifi_grace_started_us = 0;   /* 条件不再成立（已经 up，或从未配置过），重置计时 */
    }

    if (want_ap && cur != NET_MODE_AP) {
        char serial[HAL_NAME_MAX] = "";
        char ssid[HAL_SSID_MAX];
        char code_buf[HAL_PSK_MAX];
        char psk[HAL_PSK_MAX];
        size_t len = 0;
        hal_err_t rc;

        /* SSID：序列号取自 hal_sys 芯片信息（profile 无逐台设备的序列号字段） */
        if (hal_has(HAL_MOD_SYS) && hal()->sys->get_info) {
            hal_sys_info_t info;
            if (hal()->sys->get_info(&info) == HAL_OK)
                snprintf(serial, sizeof(serial), "%s", info.chip_id);
        }
        if (console_ap_ssid(serial, ssid, sizeof(ssid)) != HAL_OK) return;

        /* PSK：从出厂验证码派生（console_ap_psk_derive，评审 Important 3）；
           取不到验证码或派生失败就不开热点，绝不退化为开放网络——开放
           热点会让邻近用户直接进入配网页，不可接受。 */
        code_buf[0] = '\0';
        if (hal_has(HAL_MOD_CRYPTO) && hal()->crypto->secure_read) {
            uint8_t raw[HAL_PSK_MAX];
            if (hal()->crypto->secure_read(HAL_SEC_KEY_VERIFY_CODE, raw, sizeof(raw) - 1, &len) == HAL_OK
                && len > 0 && len < sizeof(code_buf)) {
                memcpy(code_buf, raw, len);
                code_buf[len] = '\0';
                while (len > 0 && (code_buf[len - 1] == '\n' || code_buf[len - 1] == '\r' ||
                                   code_buf[len - 1] == ' '  || code_buf[len - 1] == '\t'))
                    code_buf[--len] = '\0';
            }
            secure_wipe_local(raw, sizeof(raw));
        }
        if (code_buf[0] == '\0') {
            LOGE(MOD, "无出厂验证码可用，无法开启带密码的配网热点（不使用开放热点兜底）");
            return;
        }
        rc = console_ap_psk_derive(code_buf, psk, sizeof(psk));
        secure_wipe_local(code_buf, sizeof(code_buf));
        if (rc != HAL_OK) {
            LOGE(MOD, "从出厂验证码派生 WPA2 口令失败（rc=%d），无法开启配网热点", (int)rc);
            return;
        }

        rc = (hal_has(HAL_MOD_NET) && hal()->net->wifi_ap_start)
             ? hal()->net->wifi_ap_start(ssid, psk, 0) : HAL_ENOTSUP;
        if (rc == HAL_OK) {
            os_mutex_lock(s_netst_mu);
            s_netst.mode = NET_MODE_AP;
            snprintf(s_netst.ap_ssid, sizeof(s_netst.ap_ssid), "%s", ssid);
            os_mutex_unlock(s_netst_mu);
            LOGI(MOD, "已开启配网热点");   /* 绝不打印 SSID 之外的任何凭据信息 */
        } else {
            /* 评审 Important 3：失败不能静默——这是设备唯一的救济路径，
               静默死掉会让现场无从排查。日志与 last_error 都只带错误码，
               不带 psk/验证码。 */
            LOGE(MOD, "开启配网热点失败 rc=%d", (int)rc);
            os_mutex_lock(s_netst_mu);
            snprintf(s_netst.last_error, sizeof(s_netst.last_error),
                     "开启配网热点失败（hal_err=%d）", (int)rc);
            os_mutex_unlock(s_netst_mu);
        }
        secure_wipe_local(psk, sizeof(psk));
    } else if (!want_ap && cur == NET_MODE_AP) {
        if (hal_has(HAL_MOD_NET) && hal()->net->wifi_ap_stop) hal()->net->wifi_ap_stop();
        os_mutex_lock(s_netst_mu);
        s_netst.mode = NET_MODE_ETH;
        os_mutex_unlock(s_netst_mu);
    }
}

/** 执行一次配网：停 AP → wifi_connect → 等关联/DHCP；失败 60 秒后重开 AP。
 *  全程在工作线程执行，可安全阻塞。 */
static void net_do_connect(const net_connect_req_t *job)
{
    hal_err_t rc;
    bool up = false;
    uint32_t waited_ms = 0;
    net_mode_t cur;

    os_mutex_lock(s_netst_mu);
    cur = s_netst.mode;
    os_mutex_unlock(s_netst_mu);
    if (cur == NET_MODE_AP) {
        if (hal_has(HAL_MOD_NET) && hal()->net->wifi_ap_stop) hal()->net->wifi_ap_stop();
        os_mutex_lock(s_netst_mu);
        s_netst.mode = NET_MODE_ETH;
        os_mutex_unlock(s_netst_mu);
    }

    rc = (hal_has(HAL_MOD_NET) && hal()->net->wifi_connect)
         ? hal()->net->wifi_connect(job->ssid, job->psk, job->sec)
         : HAL_ENOTSUP;

    if (rc == HAL_OK) {
        while (waited_ms < NET_CONNECT_WAIT_MS) {
            hal_netif_status_t st;
            if (hal_has(HAL_MOD_NET) && hal()->net->get_status &&
                hal()->net->get_status(HAL_NETIF_WIFI, &st) == HAL_OK && st.link == HAL_LINK_UP) {
                up = true;
                break;
            }
            if (!net_interruptible_wait(500)) return;   /* 收到 stop：放弃本次流程，不再回落 AP */
            waited_ms += 500;
        }
    }

    if (up) {
        os_mutex_lock(s_netst_mu);
        s_netst.mode = NET_MODE_STA;
        s_netst.wifi_cfgd = true;   /* 评审 Minor #3：内存态同步置位，不只是落盘的 cfg；
                                       Important 1 加了周期复查后 net_apply_ap_decision
                                       真的会读到这个字段，必须保持自洽 */
        snprintf(s_netst.sta_ssid, sizeof(s_netst.sta_ssid), "%s", job->ssid);
        s_netst.last_error[0] = '\0';
        os_mutex_unlock(s_netst_mu);
        cfg_set_str("net.wifi.ssid", job->ssid);   /* 持久化"已配网"标志，供下次开机判定 wifi_cfgd */
        if (hal_has(HAL_MOD_GPIO) && hal()->gpio->set) hal()->gpio->set(HAL_PIN_STATUS_LED, true);
        LOGI(MOD, "WiFi 配网成功，已切换到 STA 模式");
    } else {
        os_mutex_lock(s_netst_mu);
        snprintf(s_netst.last_error, sizeof(s_netst.last_error),
                 "连接失败或等待超时（hal_err=%d）", (int)rc);
        os_mutex_unlock(s_netst_mu);
        LOGW(MOD, "WiFi 配网失败，%u 秒后重开配网热点", (unsigned)(NET_AP_REOPEN_DELAY_MS / 1000u));
        if (!net_interruptible_wait(NET_AP_REOPEN_DELAY_MS)) return;
        net_apply_ap_decision();
    }
}

/** 执行一次 WiFi 扫描（阻塞调用），结果写入缓存供 handler 读取。 */
static void net_do_scan(void)
{
    hal_wifi_ap_t aps[NET_SCAN_MAX];
    uint32_t count = 0;
    hal_err_t rc = HAL_ENOTSUP;

    if (hal_has(HAL_MOD_NET) && hal()->net->wifi_scan)
        rc = hal()->net->wifi_scan(aps, NET_SCAN_MAX, &count, 8000);

    os_mutex_lock(s_netst_mu);
    s_netst.scan.in_progress = false;
    s_netst.scan.last_rc = rc;
    if (rc == HAL_OK) {
        uint32_t i, n = count > NET_SCAN_MAX ? NET_SCAN_MAX : count;
        for (i = 0; i < n; i++) {
            snprintf(s_netst.scan.aps[i].ssid, sizeof(s_netst.scan.aps[i].ssid), "%s", aps[i].ssid);
            s_netst.scan.aps[i].rssi_dbm = aps[i].rssi_dbm;
            s_netst.scan.aps[i].freq_mhz = aps[i].freq_mhz;
            s_netst.scan.aps[i].security = aps[i].security;
        }
        s_netst.scan.count = n;
        s_netst.scan.valid = true;
        s_netst.scan.ts_us = os_monotonic_us();
    }
    os_mutex_unlock(s_netst_mu);
}

/** AP 运行期间顺带服务 DHCP；非 AP 模式下确保 socket 已关闭。 */
static void net_dhcp_poll_if_ap(uint32_t timeout_ms)
{
    net_mode_t mode;
    os_mutex_lock(s_netst_mu);
    mode = s_netst.mode;
    os_mutex_unlock(s_netst_mu);
    if (mode == NET_MODE_AP) {
        net_dhcp_socket_ensure_open();
        net_dhcp_poll_once(timeout_ms);
    } else {
        net_dhcp_socket_close_if_open();
    }
}

static void net_worker_thread(void *arg)
{
    uint64_t last_ap_check_us;

    (void)arg;
    net_apply_ap_decision();
    last_ap_check_us = os_monotonic_us();

    for (;;) {
        net_connect_req_t job;
        bool do_connect = false, do_scan = false;
        uint64_t now;

        memset(&job, 0, sizeof(job));
        os_mutex_lock(s_netst_mu);
        if (s_netst.stop) { os_mutex_unlock(s_netst_mu); break; }
        if (s_netst.connect_req.pending) {
            job = s_netst.connect_req;
            /* 评审 Minor #2：消费后不能只清 pending，静态槽位里明文 psk 的
               副本要跟着清零——堆副本、栈副本都已经 wipe，这份不能漏。 */
            secure_wipe_local(&s_netst.connect_req, sizeof(s_netst.connect_req));
            do_connect = true;
        } else if (s_netst.scan.in_progress) {
            do_scan = true;
        }
        if (!do_connect && !do_scan)
            os_cond_wait(s_netst_cv, s_netst_mu, 1000);
        os_mutex_unlock(s_netst_mu);

        if (do_connect) {
            net_do_connect(&job);
        } else if (do_scan) {
            net_do_scan();
        } else {
            net_dhcp_poll_if_ap(200);
        }

        /* 评审 Important 1：AP 决策不能只在启动时算一次——开机时 WiFi 还没
           关联就误开 AP 之后，如果没有周期复查，mode 会永远停在 "ap"，即使
           supplicant 随后真的把 WiFi 连上了；反过来运行中插/拔网线也需要
           能被感知到。每隔 NET_AP_RECHECK_MS 重新跑一次判定；do_connect 走
           完之后也顺带重新计时——net_do_connect 自己已经在失败路径里调过
           一次 net_apply_ap_decision，这里再核一次不会重复开/关，是幂等的。 */
        now = os_monotonic_us();
        if (now - last_ap_check_us >= (uint64_t)NET_AP_RECHECK_MS * 1000ull) {
            net_apply_ap_decision();
            last_ap_check_us = now;
        }
    }
    net_dhcp_socket_close_if_open();
}

hal_err_t console_net_start(void)
{
    net_sync_ensure();
    if (s_netst_thread) return HAL_OK;   /* 幂等：重复 start 不重建线程 */
    os_mutex_lock(s_netst_mu);
    s_netst.stop = false;
    os_mutex_unlock(s_netst_mu);
    s_netst_thread = os_thread_create(net_worker_thread, NULL, "consnet", 64);
    return s_netst_thread ? HAL_OK : HAL_EIO;
}

hal_err_t console_net_stop(void)
{
    if (!s_netst_thread) return HAL_OK;   /* 未启动过：视为成功 */
    os_mutex_lock(s_netst_mu);
    s_netst.stop = true;
    os_mutex_unlock(s_netst_mu);
    os_cond_broadcast(s_netst_cv);
    os_thread_join(s_netst_thread);
    s_netst_thread = NULL;
    return HAL_OK;
}

/* ==========================================================================
 * 六、DHCP socket 收发层 —— 平台相关，未在任何环境下被执行验证
 *
 * 本机是 x86 + mock，没有真实网卡可绑定 UDP 67 端口、也没有真实客户端会
 * 发 DHCPDISCOVER 广播——这一段代码从未被跑过一次。上面二、三节的报文
 * 解析/构造/租约表已经是与 socket 无关的纯函数并有完整的字节级测试覆盖；
 * 这里只是"从网线上收字节、调用那些纯函数、把结果发回网线"的胶水，
 * 刻意收窄到最小，方便日后在真实硬件上单独验证。
 * ========================================================================== */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET net_sock_t;
#define NET_SOCK_INVALID INVALID_SOCKET
#define NET_CLOSESOCK(f) closesocket(f)
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int net_sock_t;
#define NET_SOCK_INVALID (-1)
#define NET_CLOSESOCK(f) close(f)
#endif

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define NET_AP_GATEWAY_U32          0xC0A8A901u   /* 192.168.169.1，与 NET_AP_GATEWAY_IP 保持一致 */
#define NET_AP_SUBNET_BROADCAST_U32 (NET_AP_GATEWAY_U32 | 0x000000FFu)   /* 192.168.169.255 */

static net_sock_t s_dhcp_sock = NET_SOCK_INVALID;
static console_dhcp_lease_table_t s_dhcp_leases;
#ifdef _WIN32
static bool s_wsa_started;
#endif

/**
 * 评审 Important 4：socket 原来 bind 到 INADDR_ANY:67、应答又广播到
 * 255.255.255.255——不区分接口，会接收/应答来自任意网卡（包括有线上联口）
 * 的 DHCP 流量，在客户 LAN 上变成流氓 DHCP 服务器。改为：
 *   1) bind 到 AP 网段自身地址（NET_AP_GATEWAY_U32），而非 INADDR_ANY；
 *   2) 应答目的地址改为 AP 网段的定向广播（NET_AP_SUBNET_BROADCAST_U32=
 *      192.168.169.255），不用全局受限广播 255.255.255.255——后者会经
 *      内核路由表在所有具备广播能力的接口上外泄，前者只会经拥有该网段
 *      路由的接口（AP 自身）送出。
 *
 * **已知的可移植性注意事项（本机无法验证，未来在真实硬件上必须确认）**：
 * 多数 BSD 派生的 socket 实现里，UDP socket 若 bind 到一个具体的单播地址
 * 而非 INADDR_ANY，只会收到目的地址精确匹配该地址的报文——而 DHCPDISCOVER
 * 按 RFC 2131 通常以目的地址 255.255.255.255（受限广播）发出，客户端此时
 * 还不知道网关地址。如果这一行为在目标平台上成立，bind 到具体地址会导致
 * 收不到 DISCOVER，DHCP 服务名存实亡。真正正确、可移植的做法是绑定到
 * INADDR_ANY 但把套接字绑定到具体网络接口（Linux 上是 SO_BINDTODEVICE，
 * 需要 AP 接口名——目前 hal_net.h 的 wifi_ap_start 不回传接口名，取不到），
 * 或用 recvmsg + IP_PKTINFO 按到达接口过滤。这两种做法都比现在复杂得多，
 * 且都无法在 x86 + mock 上验证效果。当前先按评审的要求实现"bind 到具体
 * 地址"这一步；**在真实硬件上联调这一层时，第一件事就是确认 DISCOVER
 * 是否还能被收到**，收不到就需要换成上述按接口过滤的方案。无论 bind 方式
 * 如何，"应答不经全局广播外泄到其他接口"这条改动都是纯收益、不用回退。
 */
static void net_dhcp_socket_ensure_open(void)
{
    struct sockaddr_in addr;
    int on = 1;

    if (s_dhcp_sock != NET_SOCK_INVALID) return;
#ifdef _WIN32
    if (!s_wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            LOGE(MOD, "DHCP：WSAStartup 失败，AP 模式下客户端将无法自动获取地址");
            return;
        }
        s_wsa_started = true;
    }
#endif
    s_dhcp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_dhcp_sock == NET_SOCK_INVALID) {
        LOGE(MOD, "DHCP：创建 UDP socket 失败");
        return;
    }
    setsockopt(s_dhcp_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
    setsockopt(s_dhcp_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(NET_AP_GATEWAY_U32);   /* 绑定到 AP 网段自身，而非 INADDR_ANY */
    addr.sin_port = htons(DHCP_SERVER_PORT);
    if (bind(s_dhcp_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOGE(MOD, "DHCP：绑定 UDP 67 端口失败（需要管理员/root 权限，或端口已被占用）");
        NET_CLOSESOCK(s_dhcp_sock);
        s_dhcp_sock = NET_SOCK_INVALID;
        return;
    }
    console_dhcp_lease_table_init(&s_dhcp_leases);
    LOGI(MOD, "DHCP 服务已在配网热点网段启动（.100~.200，租期 %u 秒）",
         (unsigned)CONSOLE_DHCP_LEASE_S);
}

static void net_dhcp_socket_close_if_open(void)
{
    if (s_dhcp_sock == NET_SOCK_INVALID) return;
    NET_CLOSESOCK(s_dhcp_sock);
    s_dhcp_sock = NET_SOCK_INVALID;
}

/** 收一个 DHCP 请求、决策、回一个应答；timeout_ms 内无数据则直接返回。
 *  协议策略（要不要回、回什么）全部在纯函数 console_dhcp_decide 里，这里
 *  只做"收字节→调用它→发字节"的胶水（评审 Minor #6，缩小未验证面）。 */
static void net_dhcp_poll_once(uint32_t timeout_ms)
{
    uint8_t buf[600], reply[600];
    struct sockaddr_in from;
    fd_set rfds;
    struct timeval tv;
    int n;
#ifdef _WIN32
    int fromlen = (int)sizeof(from);
#else
    socklen_t fromlen = sizeof(from);
#endif
    console_dhcp_msg_t msg;
    uint32_t your_ip = 0;
    size_t out_len = 0;
    uint8_t reply_type = 0;
    struct sockaddr_in to;

    if (s_dhcp_sock == NET_SOCK_INVALID) return;

    FD_ZERO(&rfds);
    FD_SET(s_dhcp_sock, &rfds);
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
#ifdef _WIN32
    if (select(0, &rfds, NULL, NULL, &tv) <= 0) return;   /* Windows 忽略首参 nfds，与 http_server.c 同做法 */
#else
    if (select(s_dhcp_sock + 1, &rfds, NULL, NULL, &tv) <= 0) return;
#endif

    n = recvfrom(s_dhcp_sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
    if (n <= 0) return;
    if (console_dhcp_parse(buf, (size_t)n, &msg) != HAL_OK) return;
    if (msg.op != 1) return;   /* 只处理 BOOTREQUEST */

    if (!console_dhcp_decide(&s_dhcp_leases, &msg, (uint32_t)(os_monotonic_us() / 1000000ull),
                             NET_DHCP_OFFER_TTL_S, CONSOLE_DHCP_LEASE_S, NET_AP_GATEWAY_U32,
                             &reply_type, &your_ip))
        return;   /* 不需要回复：地址池耗尽 / RELEASE / 未实现的消息类型 */

    if (console_dhcp_build_reply(&msg, reply_type, your_ip, NET_AP_GATEWAY_U32, CONSOLE_DHCP_LEASE_S,
                                 reply, sizeof(reply), &out_len) != HAL_OK)
        return;

    /* 客户端此刻多半还没有 IP：广播到 68 端口而不是精确单播（比按 flags
       广播位区分单播/广播更简单可靠）；用 AP 网段定向广播而不是全局受限
       广播，见 net_dhcp_socket_ensure_open 顶部注释——避免应答外泄到其他
       接口。 */
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(NET_AP_SUBNET_BROADCAST_U32);
    to.sin_port = htons(DHCP_CLIENT_PORT);
    sendto(s_dhcp_sock, (const char *)reply, (int)out_len, 0, (struct sockaddr *)&to, sizeof(to));
}

/* ==========================================================================
 * 七、HTTP 端点：/api/v1/net/{status,wifi/scan,wifi/connect}
 * ========================================================================== */

static hal_err_t ep_net_status(char *out, size_t cap)
{
    net_mode_t mode;
    char mac_hex[24] = "";
    char ssid_buf[HAL_SSID_MAX] = "";
    char ip_buf[HAL_IP_MAX] = "";
    char last_err[128] = "";
    int rssi = 0;
    bool have_mac = false, have_ssid = false, have_rssi = false;
    json_t *root;
    char *txt;
    hal_err_t rc;

    net_sync_ensure();
    os_mutex_lock(s_netst_mu);
    mode = s_netst.mode;
    if (mode == NET_MODE_AP) {
        /* AP 网段是我们自己固定分配的网关地址，不查 HAL——mock 的
           get_status(WIFI) 恒 ENOTSUP，且这个值本就不该由 HAL 决定。 */
        snprintf(ip_buf, sizeof(ip_buf), "%s", NET_AP_GATEWAY_IP);
        if (s_netst.ap_ssid[0]) { snprintf(ssid_buf, sizeof(ssid_buf), "%s", s_netst.ap_ssid); have_ssid = true; }
    } else if (mode == NET_MODE_STA) {
        if (s_netst.sta_ssid[0]) { snprintf(ssid_buf, sizeof(ssid_buf), "%s", s_netst.sta_ssid); have_ssid = true; }
    }
    snprintf(last_err, sizeof(last_err), "%s", s_netst.last_error);
    os_mutex_unlock(s_netst_mu);

    /* mac/ip（eth/sta 模式）、rssi 尽力而为：mock 平台 get_status(WIFI) 恒
       ENOTSUP，sta 模式下这三个字段会省略/留空，而不是硬凑假数据（与
       ep_video_params 的既有约定一致）。ip 字段本身自 HAL v1.2 起恒存在于
       响应体中——hal_netif_status_t.ip 未获取到地址时约定为空串，直接
       透传即可，调用方不需要再猜"缺字段是什么意思"。 */
    if (hal_has(HAL_MOD_NET) && hal()->net->get_status) {
        hal_netif_status_t st;
        hal_netif_t iface = (mode == NET_MODE_ETH) ? HAL_NETIF_ETH : HAL_NETIF_WIFI;
        if (hal()->net->get_status(iface, &st) == HAL_OK) {
            snprintf(mac_hex, sizeof(mac_hex), "%02X:%02X:%02X:%02X:%02X:%02X",
                     st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
            have_mac = true;
            if (mode != NET_MODE_AP) snprintf(ip_buf, sizeof(ip_buf), "%s", st.ip);
            if (mode == NET_MODE_STA && st.rssi_dbm != 0) { rssi = st.rssi_dbm; have_rssi = true; }
        }
    }

    root = json_new_object();
    if (!root) return HAL_ENOMEM;
    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "mode", json_new_string(
        mode == NET_MODE_AP ? "ap" : (mode == NET_MODE_STA ? "sta" : "eth")));
    if (have_mac)    json_object_set(root, "mac", json_new_string(mac_hex));
    /* ip 恒存在（HAL v1.2 起有明确"未知即空串"的约定），不再按 have_ip 省略 */
    json_object_set(root, "ip", json_new_string(ip_buf));
    if (have_ssid)   json_object_set(root, "ssid", json_new_string(ssid_buf));
    if (have_rssi)   json_object_set(root, "rssi", json_new_int(rssi));
    if (last_err[0]) json_object_set(root, "last_error", json_new_string(last_err));

    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

/** 调用者持锁（s_netst_mu）时使用：把当前扫描缓存拼成响应体。 */
static hal_err_t net_build_scan_json_locked(char *out, size_t cap)
{
    json_t *root, *arr;
    char *txt;
    uint32_t i;
    hal_err_t rc;
    uint64_t age_us = os_monotonic_us() - s_netst.scan.ts_us;

    root = json_new_object();
    arr = json_new_array();
    if (!root || !arr) { json_free(root); json_free(arr); return HAL_ENOMEM; }
    json_object_set(root, "code", json_new_int(0));

    for (i = 0; i < s_netst.scan.count; i++) {
        const net_ap_entry_t *e = &s_netst.scan.aps[i];
        json_t *jo = json_new_object();
        const char *sec_name;
        if (!jo) continue;
        switch (e->security) {
        case HAL_WIFI_SEC_OPEN:      sec_name = "open"; break;
        case HAL_WIFI_SEC_WPA3:      sec_name = "wpa3"; break;
        case HAL_WIFI_SEC_WPA2_WPA3: sec_name = "wpa2wpa3"; break;
        default:                     sec_name = "wpa2"; break;
        }
        json_object_set(jo, "ssid", json_new_string(e->ssid));
        json_object_set(jo, "rssi", json_new_int(e->rssi_dbm));
        json_object_set(jo, "freq_mhz", json_new_int((int64_t)e->freq_mhz));
        json_object_set(jo, "security", json_new_string(sec_name));
        json_array_push(arr, jo);
    }
    json_object_set(root, "aps", arr);
    json_object_set(root, "age_s", json_new_int((int64_t)(age_us / 1000000ull)));

    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

/**
 * GET /api/v1/net/wifi/scan：hal_net.wifi_scan 是阻塞调用，不可在 epoll
 * 线程直接调——选 brief 里"更省线程"的方案：有缓存直接返回，否则投一个
 * 扫描任务给工作线程并回 202 让前端轮询。
 * last_rc==HAL_ENOTSUP 时直接如实回报 501，不再无意义地反复重试
 * （区别于其他失败：其余错误码允许 NET_SCAN_TTL_US 之后自然重试）。
 */
static hal_err_t ep_net_wifi_scan(char *out, size_t cap, int *http_status)
{
    hal_err_t rc;
    uint64_t now;
    bool trigger = false;

    if (!net_wifi_supported()) return HAL_ENOTSUP;

    net_sync_ensure();
    os_mutex_lock(s_netst_mu);
    now = os_monotonic_us();
    if (s_netst.scan.in_progress) {
        os_mutex_unlock(s_netst_mu);
        *http_status = 202;
        return fmt_safe(out, cap, "{\"code\":0,\"msg\":\"正在扫描，请稍后重试\"}");
    }
    if (s_netst.scan.valid && (now - s_netst.scan.ts_us) < NET_SCAN_TTL_US) {
        rc = net_build_scan_json_locked(out, cap);
        os_mutex_unlock(s_netst_mu);
        return rc;
    }
    if (s_netst.scan.last_rc == HAL_ENOTSUP) {
        os_mutex_unlock(s_netst_mu);
        return HAL_ENOTSUP;
    }
    s_netst.scan.in_progress = true;
    trigger = true;
    os_mutex_unlock(s_netst_mu);
    if (trigger) os_cond_signal(s_netst_cv);
    *http_status = 202;
    return fmt_safe(out, cap, "{\"code\":0,\"msg\":\"正在扫描，请稍后重试\"}");
}

/** 解析并校验 POST /api/v1/net/wifi/connect 的请求体，产出工作线程要用的
 *  job 参数。纯函数：不碰任何全局状态，方便测试。 */
static hal_err_t net_parse_connect_body(const json_t *j, net_connect_req_t *out)
{
    const char *ssid, *psk, *sec;
    size_t psk_len;

    if (!j || !out) return HAL_EINVAL;
    ssid = json_string(json_get(j, "ssid"), NULL);
    if (!ssid || !ssid[0] || strlen(ssid) >= HAL_SSID_MAX) return HAL_EINVAL;

    psk = json_string(json_get(j, "psk"), "");
    psk_len = strlen(psk);
    if (psk_len >= HAL_PSK_MAX) return HAL_EINVAL;

    sec = json_string(json_get(j, "sec"), NULL);
    memset(out, 0, sizeof(*out));
    snprintf(out->ssid, sizeof(out->ssid), "%s", ssid);
    snprintf(out->psk, sizeof(out->psk), "%s", psk);

    if (sec && strcmp(sec, "open") == 0) out->sec = HAL_WIFI_SEC_OPEN;
    else if (sec && strcmp(sec, "wpa3") == 0) out->sec = HAL_WIFI_SEC_WPA3;
    else if (sec && strcmp(sec, "wpa2wpa3") == 0) out->sec = HAL_WIFI_SEC_WPA2_WPA3;
    else if (sec && strcmp(sec, "wpa2") == 0) out->sec = HAL_WIFI_SEC_WPA2;
    else out->sec = psk_len > 0 ? HAL_WIFI_SEC_WPA2 : HAL_WIFI_SEC_OPEN;   /* 未指定时按有无口令推断 */

    if (out->sec == HAL_WIFI_SEC_OPEN) {
        if (psk_len != 0) return HAL_EINVAL;   /* 开放网络不应带密码 */
    } else if (psk_len < 8 || psk_len > 63) {
        return HAL_EINVAL;   /* WPA2/3 约束，与 hal_net.h 的 wifi_ap_start 一致 */
    }
    return HAL_OK;
}

static hal_err_t ep_net_wifi_connect(const http_req_t *req, net_connect_req_t *job_out,
                                     char *out, size_t cap, int *http_status)
{
    json_t *j;
    hal_err_t rc;

    if (strcmp(req->method, "POST") != 0) return HAL_EINVAL;
    if (!net_wifi_supported()) return HAL_ENOTSUP;
    if (!req->body || req->body_len == 0) return HAL_EINVAL;

    j = json_parse(req->body, req->body_len, NULL, 0);
    if (!j) return HAL_EINVAL;
    rc = net_parse_connect_body(j, job_out);
    json_free(j);
    if (rc != HAL_OK) return rc;

    LOGI(MOD, "收到配网请求（SSID 见前端提交记录，口令不入日志）");
    *http_status = 202;
    return fmt_safe(out, cap, "{\"code\":0,\"msg\":\"正在连接，请将设备连回目标网络后访问新地址\"}");
}

/* ==========================================================================
 * 八、分发与路由注册
 * ========================================================================== */

/**
 * 纯函数：按 req->path/method 分发，只往 out 写响应体、不碰 req->conn，
 * 也不调用 http_respond 系列或 http_conn_defer_after_flush——与
 * console_api.c 的 api_dispatch 同一手法，使 console_net_test_dispatch
 * 能在没有真实/伪造连接的情况下驱动它。connect 端点成功时把要交给工作
 * 线程的 job 写进 *job_out、并把 *dfn 置为 net_connect_handoff；真正把
 * job 写进工作线程会消费的静态槽位（s_netst.connect_req）、登记
 * http_conn_defer_after_flush 由 console_net_handler 负责（该函数持有
 * req->conn，本函数不持有）。**不经堆**——见 net_connect_handoff 的注释。
 */
static void net_connect_handoff(void *arg);

static hal_err_t net_dispatch(const http_req_t *req, char *out, size_t cap,
                              net_connect_req_t *job_out, void (**dfn)(void *), int *http_status)
{
    hal_err_t e;

    *http_status = 200;
    *dfn = NULL;

    e = console_auth_check(req);
    if (e != HAL_OK) return e;

    if (strcmp(req->path, "/api/v1/net/status") == 0)
        return strcmp(req->method, "GET") == 0 ? ep_net_status(out, cap) : HAL_EINVAL;

    if (strcmp(req->path, "/api/v1/net/wifi/scan") == 0)
        return strcmp(req->method, "GET") == 0 ? ep_net_wifi_scan(out, cap, http_status) : HAL_EINVAL;

    if (strcmp(req->path, "/api/v1/net/wifi/connect") == 0) {
        hal_err_t rc = ep_net_wifi_connect(req, job_out, out, cap, http_status);
        if (rc == HAL_OK) *dfn = net_connect_handoff;
        return rc;
    }

    return HAL_ENODEV;
}

#ifdef IPC_TESTING
static unsigned s_netst_handoff_count;
#endif

/**
 * 延后动作：把已经写进 s_netst.connect_req（pending 仍为 false）的连接
 * 请求转正。评审 Important 2：原实现把含明文 psk 的 net_connect_req_t
 * malloc 到堆上传给这里当 arg；而 http_server.c 的 conn_close 在回调触发
 * 前断连时只清指针、不调用回调、也不释放 arg（它假设 arg 不需要释放，见
 * http_server.h 的所有权语义说明）——"提交后连接必断"恰恰是这个端点自己
 * 的设计场景，已登录用户提交后立刻断开还能反复触发，每次泄漏一块含明文
 * 口令的堆内存。
 *
 * 改为不经堆：console_net_handler 在响应发出前就已经把 job 写进
 * s_netst.connect_req（此时 pending 仍是 false，工作线程不会碰它），
 * 这个回调只需要把 pending 翻成 true 并唤醒工作线程——不携带任何数据，
 * arg 传 NULL 即可。即使连接在回调触发前断开，未转正的静态槽位也不是
 * "泄漏"：不是堆内存，没有人需要为它调用 free，下一次配网请求会覆盖它。
 */
static void net_connect_handoff(void *arg)
{
    (void)arg;
#ifdef IPC_TESTING
    s_netst_handoff_count++;
#endif
    os_mutex_lock(s_netst_mu);
    s_netst.connect_req.pending = true;
    os_mutex_unlock(s_netst_mu);
    os_cond_signal(s_netst_cv);
}

#ifdef IPC_TESTING
/** 测试可见的计数器：正常调用顺序（先 http_respond_json 入队、再
 *  http_conn_defer_after_flush 登记）下应恒为 0。与 console_api.c 的
 *  s_defer_register_fail_count 同一手法。 */
static unsigned s_netst_defer_fail_count;
#endif

static int console_net_handler(http_req_t *req, void *user)
{
    char *body;
    net_connect_req_t job;
    void (*dfn)(void *) = NULL;
    int http_status = 200;
    hal_err_t e;

    (void)user;
    body = (char *)malloc(CONSOLE_NET_BODY_MAX);
    if (!body) return console_reply_err(req->conn, HAL_ENOMEM);

    memset(&job, 0, sizeof(job));
    e = net_dispatch(req, body, CONSOLE_NET_BODY_MAX, &job, &dfn, &http_status);
    if (e != HAL_OK) { free(body); return console_reply_err(req->conn, e); }

    /* 调用顺序是硬约束：必须先让响应真正入队，才能登记"响应发出后"的动作
       ——与 console_api.c 的 reboot/reset 同一约束，见其详细注释。 */
    e = http_respond_json(req->conn, http_status, body);
    free(body);
    if (e != HAL_OK) { secure_wipe_local(&job, sizeof(job)); return console_reply_err(req->conn, e); }   /* 入队失败：不登记动作 */

    if (dfn) {
        /* 不用堆：把已校验好的 job 直接写进工作线程会消费的静态槽位
           （pending 保持 false，工作线程不会碰它），defer 回调
           （net_connect_handoff）只需要在响应确认入队之后把 pending 翻成
           true 并唤醒工作线程——回调本身不携带任何数据，arg 传 NULL。
           这一步仍然安排在 http_respond_json 之后，与"先响应再动作"的
           既有约定保持一致，虽然写静态槽位本身并不阻塞、也不会被
           conn_close 需要清理。 */
        os_mutex_lock(s_netst_mu);
        s_netst.connect_req = job;
        os_mutex_unlock(s_netst_mu);
        if (http_conn_defer_after_flush(req->conn, dfn, NULL) != HAL_OK) {
#ifdef IPC_TESTING
            s_netst_defer_fail_count++;
#endif
            LOGE(MOD, "延后动作登记失败：响应已发出但配网请求不会自动执行，请重试");
        }
    }
    secure_wipe_local(&job, sizeof(job));   /* 栈上的明文 psk 副本用完清零 */
    return 0;
}

hal_err_t console_net_init(void)
{
    cfg_rule_t r;
    char cfgd[HAL_SSID_MAX] = "";
    hal_err_t rc;

    net_sync_ensure();

    memset(&r, 0, sizeof(r));
    r.key_pattern = "net.wifi.ssid";
    r.type = CFG_T_STR;
    rc = cfg_register_rules(&r, 1);
    if (rc != HAL_OK)
        LOGW(MOD, "注册 net.wifi.ssid 配置规则失败（rc=%d）：断电重启后可能无法识别已配网状态，"
                  "不影响本次运行时的配网流程", (int)rc);

    if (cfg_get_str("net.wifi.ssid", cfgd, sizeof(cfgd)) == HAL_OK && cfgd[0]) {
        os_mutex_lock(s_netst_mu);
        s_netst.wifi_cfgd = true;
        os_mutex_unlock(s_netst_mu);
    }

    return http_route(CONSOLE_NET_PREFIX, console_net_handler, NULL);
}

#ifdef IPC_TESTING

void console_net_test_reset(void)
{
    net_sync_ensure();
    os_mutex_lock(s_netst_mu);
    memset(&s_netst.scan, 0, sizeof(s_netst.scan));
    memset(&s_netst.connect_req, 0, sizeof(s_netst.connect_req));
    s_netst.last_error[0] = '\0';
    s_netst.mode = NET_MODE_ETH;
    s_netst.ap_ssid[0] = '\0';
    s_netst.sta_ssid[0] = '\0';
    os_mutex_unlock(s_netst_mu);
}

hal_err_t console_net_test_dispatch(const http_req_t *req, char *body, size_t body_cap,
                                    int *http_status_out)
{
    net_connect_req_t job;
    void (*dfn)(void *) = NULL;
    int http_status = 200;
    hal_err_t rc;

    memset(&job, 0, sizeof(job));
    rc = net_dispatch(req, body, body_cap, &job, &dfn, &http_status);
    if (http_status_out) *http_status_out = http_status;
    secure_wipe_local(&job, sizeof(job));
    return rc;
}

int console_net_test_full_handler(http_req_t *req)
{
    return console_net_handler(req, NULL);
}

unsigned console_net_test_defer_fail_count(void)
{
    return s_netst_defer_fail_count;
}

void console_net_test_seed_scan_result(const char *ssid, int rssi_dbm, uint32_t freq_mhz,
                                       int security)
{
    net_sync_ensure();
    os_mutex_lock(s_netst_mu);
    s_netst.scan.count = 1;
    snprintf(s_netst.scan.aps[0].ssid, sizeof(s_netst.scan.aps[0].ssid), "%s", ssid);
    s_netst.scan.aps[0].rssi_dbm = rssi_dbm;
    s_netst.scan.aps[0].freq_mhz = freq_mhz;
    s_netst.scan.aps[0].security = (hal_wifi_sec_t)security;
    s_netst.scan.valid = true;
    s_netst.scan.in_progress = false;
    s_netst.scan.last_rc = HAL_OK;
    s_netst.scan.ts_us = os_monotonic_us();
    os_mutex_unlock(s_netst_mu);
}

void console_net_test_seed_ap(const char *ssid)
{
    net_sync_ensure();
    os_mutex_lock(s_netst_mu);
    s_netst.mode = NET_MODE_AP;
    snprintf(s_netst.ap_ssid, sizeof(s_netst.ap_ssid), "%s", ssid);
    os_mutex_unlock(s_netst_mu);
}

void console_net_test_peek_connect(char *ssid_out, size_t cap, bool *pending_out)
{
    net_sync_ensure();
    os_mutex_lock(s_netst_mu);
    if (ssid_out && cap) snprintf(ssid_out, cap, "%s", s_netst.connect_req.ssid);
    if (pending_out) *pending_out = s_netst.connect_req.pending;
    os_mutex_unlock(s_netst_mu);
}

void console_net_test_run_handoff(void)
{
    net_connect_handoff(NULL);
}

unsigned console_net_test_handoff_count(void)
{
    return s_netst_handoff_count;
}

#endif /* IPC_TESTING */
