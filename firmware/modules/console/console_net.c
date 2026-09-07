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

hal_err_t console_dhcp_lease_acquire(console_dhcp_lease_table_t *t, const uint8_t mac[6],
                                     uint32_t now_s, uint32_t *host_out)
{
    uint32_t i, free_idx = CONSOLE_DHCP_POOL_SIZE;

    if (!t || !mac || !host_out) return HAL_EINVAL;
    dhcp_lease_sweep(t, now_s);

    for (i = 0; i < CONSOLE_DHCP_POOL_SIZE; i++) {
        if (t->entries[i].used && memcmp(t->entries[i].mac, mac, 6) == 0) {
            t->entries[i].expires_s = now_s + CONSOLE_DHCP_LEASE_S;   /* 续租 */
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
    t->entries[free_idx].expires_s = now_s + CONSOLE_DHCP_LEASE_S;
    *host_out = CONSOLE_DHCP_POOL_START + free_idx;
    return HAL_OK;
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
 * 四、内部运行态（AP/STA 状态机、扫描缓存、连接请求）——由 s_netst_mu 保护
 * ========================================================================== */

typedef enum { NET_MODE_ETH = 0, NET_MODE_STA = 1, NET_MODE_AP = 2 } net_mode_t;

#define NET_SCAN_MAX            16u
#define NET_SCAN_TTL_US         (10ull * 1000000ull)   /* 缓存 10 秒内的扫描结果直接复用 */
#define NET_CONNECT_WAIT_MS     20000u                 /* 等待关联+DHCP 拿到地址的最长时间 */
#define NET_AP_REOPEN_DELAY_MS  60000u                 /* brief 原文：失败 60 秒后重开 AP */
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

/** 按当前链路状态决定要不要开/关 AP（启动时、配网失败回落时都会调用）。 */
static void net_apply_ap_decision(void)
{
    hal_netif_status_t st;
    bool eth_up = false, wifi_up = false, wifi_cfgd, want_ap;
    net_mode_t cur;

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

    if (want_ap && cur != NET_MODE_AP) {
        char serial[HAL_NAME_MAX] = "";
        char ssid[HAL_SSID_MAX];
        char psk[HAL_PSK_MAX];
        size_t len = 0;

        /* SSID：序列号取自 hal_sys 芯片信息（profile 无逐台设备的序列号字段） */
        if (hal_has(HAL_MOD_SYS) && hal()->sys->get_info) {
            hal_sys_info_t info;
            if (hal()->sys->get_info(&info) == HAL_OK)
                snprintf(serial, sizeof(serial), "%s", info.chip_id);
        }
        if (console_ap_ssid(serial, ssid, sizeof(ssid)) != HAL_OK) return;

        /* PSK：出厂验证码，与 Web 登录同值——开放热点会让邻近用户直接进入
           配网页，不可接受。取不到就不开热点，绝不退化为开放网络。 */
        psk[0] = '\0';
        if (hal_has(HAL_MOD_CRYPTO) && hal()->crypto->secure_read) {
            uint8_t raw[HAL_PSK_MAX];
            if (hal()->crypto->secure_read(HAL_SEC_KEY_VERIFY_CODE, raw, sizeof(raw) - 1, &len) == HAL_OK
                && len > 0 && len < sizeof(psk)) {
                memcpy(psk, raw, len);
                psk[len] = '\0';
                while (len > 0 && (psk[len - 1] == '\n' || psk[len - 1] == '\r' ||
                                   psk[len - 1] == ' '  || psk[len - 1] == '\t'))
                    psk[--len] = '\0';
            }
            secure_wipe_local(raw, sizeof(raw));
        }
        if (psk[0] == '\0') {
            LOGE(MOD, "无出厂验证码可用，无法开启带密码的配网热点（不使用开放热点兜底）");
            secure_wipe_local(psk, sizeof(psk));
            return;
        }

        if (hal_has(HAL_MOD_NET) && hal()->net->wifi_ap_start &&
            hal()->net->wifi_ap_start(ssid, psk, 0) == HAL_OK) {
            os_mutex_lock(s_netst_mu);
            s_netst.mode = NET_MODE_AP;
            snprintf(s_netst.ap_ssid, sizeof(s_netst.ap_ssid), "%s", ssid);
            os_mutex_unlock(s_netst_mu);
            LOGI(MOD, "已开启配网热点");   /* 绝不打印 SSID 之外的任何凭据信息 */
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
    (void)arg;
    net_apply_ap_decision();

    for (;;) {
        net_connect_req_t job;
        bool do_connect = false, do_scan = false;

        memset(&job, 0, sizeof(job));
        os_mutex_lock(s_netst_mu);
        if (s_netst.stop) { os_mutex_unlock(s_netst_mu); break; }
        if (s_netst.connect_req.pending) {
            job = s_netst.connect_req;
            s_netst.connect_req.pending = false;
            do_connect = true;
        } else if (s_netst.scan.in_progress) {
            do_scan = true;
        }
        if (!do_connect && !do_scan)
            os_cond_wait(s_netst_cv, s_netst_mu, 1000);
        os_mutex_unlock(s_netst_mu);

        if (do_connect) net_do_connect(&job);
        else if (do_scan) net_do_scan();
        else net_dhcp_poll_if_ap(200);
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
#define NET_AP_GATEWAY_U32 0xC0A8A901u   /* 192.168.169.1，与 NET_AP_GATEWAY_IP 保持一致 */

static net_sock_t s_dhcp_sock = NET_SOCK_INVALID;
static console_dhcp_lease_table_t s_dhcp_leases;
#ifdef _WIN32
static bool s_wsa_started;
#endif

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
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
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

/** 收一个 DHCP 请求、决策、回一个应答；timeout_ms 内无数据则直接返回。 */
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
    uint32_t host = 0, want_ip, server_ip = NET_AP_GATEWAY_U32;
    size_t out_len = 0;
    uint8_t reply_type;
    hal_err_t rc;
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

    switch (msg.msg_type) {
    case CONSOLE_DHCP_MSG_DISCOVER:
        if (console_dhcp_lease_acquire(&s_dhcp_leases, msg.chaddr,
                                       (uint32_t)(os_monotonic_us() / 1000000ull), &host) != HAL_OK)
            return;   /* 地址池耗尽：不回应，客户端会重试或超时放弃 */
        reply_type = (uint8_t)CONSOLE_DHCP_MSG_OFFER;
        want_ip = (NET_AP_GATEWAY_U32 & 0xFFFFFF00u) | host;
        break;
    case CONSOLE_DHCP_MSG_REQUEST:
        rc = console_dhcp_lease_acquire(&s_dhcp_leases, msg.chaddr,
                                        (uint32_t)(os_monotonic_us() / 1000000ull), &host);
        want_ip = (NET_AP_GATEWAY_U32 & 0xFFFFFF00u) | host;
        reply_type = (uint8_t)((rc == HAL_OK && (msg.requested_ip == 0 || msg.requested_ip == want_ip))
                     ? CONSOLE_DHCP_MSG_ACK : CONSOLE_DHCP_MSG_NAK);
        break;
    case CONSOLE_DHCP_MSG_RELEASE:
        console_dhcp_lease_release(&s_dhcp_leases, msg.chaddr);
        return;
    default:
        return;   /* DECLINE/INFORM 等本极简实现不处理 */
    }

    if (console_dhcp_build_reply(&msg, reply_type, want_ip, server_ip, CONSOLE_DHCP_LEASE_S,
                                 reply, sizeof(reply), &out_len) != HAL_OK)
        return;

    /* 客户端此刻多半还没有 IP：统一广播到 68 端口，比按 flags 广播位精确区分
       单播/广播更简单可靠，对一个 /24 网段的极简实现代价可忽略。 */
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
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
    char ip_buf[16] = "";
    char last_err[128] = "";
    int rssi = 0;
    bool have_mac = false, have_ssid = false, have_rssi = false, have_ip = false;
    json_t *root;
    char *txt;
    hal_err_t rc;

    net_sync_ensure();
    os_mutex_lock(s_netst_mu);
    mode = s_netst.mode;
    if (mode == NET_MODE_AP) {
        snprintf(ip_buf, sizeof(ip_buf), "%s", NET_AP_GATEWAY_IP);
        have_ip = true;
        if (s_netst.ap_ssid[0]) { snprintf(ssid_buf, sizeof(ssid_buf), "%s", s_netst.ap_ssid); have_ssid = true; }
    } else if (mode == NET_MODE_STA) {
        if (s_netst.sta_ssid[0]) { snprintf(ssid_buf, sizeof(ssid_buf), "%s", s_netst.sta_ssid); have_ssid = true; }
    }
    snprintf(last_err, sizeof(last_err), "%s", s_netst.last_error);
    os_mutex_unlock(s_netst_mu);

    /* mac/rssi 尽力而为：mock 平台 get_status(WIFI) 恒 ENOTSUP，AP/STA 模式下
       这两个字段会省略，而不是硬凑假数据（与 ep_video_params 的既有约定一致）。 */
    if (hal_has(HAL_MOD_NET) && hal()->net->get_status) {
        hal_netif_status_t st;
        hal_netif_t iface = (mode == NET_MODE_ETH) ? HAL_NETIF_ETH : HAL_NETIF_WIFI;
        if (hal()->net->get_status(iface, &st) == HAL_OK) {
            snprintf(mac_hex, sizeof(mac_hex), "%02X:%02X:%02X:%02X:%02X:%02X",
                     st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
            have_mac = true;
            if (mode == NET_MODE_STA && st.rssi_dbm != 0) { rssi = st.rssi_dbm; have_rssi = true; }
        }
    }

    root = json_new_object();
    if (!root) return HAL_ENOMEM;
    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "mode", json_new_string(
        mode == NET_MODE_AP ? "ap" : (mode == NET_MODE_STA ? "sta" : "eth")));
    if (have_mac)    json_object_set(root, "mac", json_new_string(mac_hex));
    if (have_ip)     json_object_set(root, "ip", json_new_string(ip_buf));
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
 * job 拷到堆上、登记 http_conn_defer_after_flush 由 console_net_handler
 * 负责（该函数持有 req->conn，本函数不持有）。
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

/**
 * 延后动作：把已校验好的连接请求交给工作线程。只做"锁内拷贝结构体 +
 * 信号 + 释放堆内存"这一件事，不做任何校验/解析（校验已经在 net_dispatch
 * 里、响应发出之前完成），符合 http_conn_defer_after_flush 的"不可阻塞"
 * 契约；同时也让这个回调本身足够简单，正确性可以直接由代码走查确认
 * （与 console_api.c 的 do_reboot/do_reset 同一档次）。
 */
static void net_connect_handoff(void *arg)
{
    net_connect_req_t *job = (net_connect_req_t *)arg;
    if (!job) return;
    net_sync_ensure();
    os_mutex_lock(s_netst_mu);
    s_netst.connect_req = *job;
    s_netst.connect_req.pending = true;
    os_mutex_unlock(s_netst_mu);
    os_cond_signal(s_netst_cv);
    secure_wipe_local(job, sizeof(*job));   /* job->psk 是明文口令，用完立即清零再释放 */
    free(job);
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
    if (e != HAL_OK) return console_reply_err(req->conn, e);   /* 入队失败：不登记动作 */

    if (dfn) {
        net_connect_req_t *heap_job = (net_connect_req_t *)malloc(sizeof(job));
        if (!heap_job) {
            LOGE(MOD, "配网任务交接分配内存失败：本次连接请求已丢失，请客户端重试");
        } else {
            *heap_job = job;
            if (http_conn_defer_after_flush(req->conn, dfn, heap_job) != HAL_OK) {
                secure_wipe_local(heap_job, sizeof(*heap_job));
                free(heap_job);
#ifdef IPC_TESTING
                s_netst_defer_fail_count++;
#endif
                LOGE(MOD, "延后动作登记失败：响应已发出但配网请求不会自动执行，请重试");
            }
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

#endif /* IPC_TESTING */
