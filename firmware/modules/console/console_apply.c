/**
 * @file console_apply.c
 * @brief 把已保存的 net.* / time.* 配置经 HAL 下发到系统
 *
 * 配置中心只负责校验与落盘；真正改网卡地址、起停 NTP、设进程时区属于平台动作，
 * 网络/NTP 经 hal_sys_ops 的可选能力完成（NULL 表示平台不支持 → HAL_ENOTSUP），
 * 时区经 core/os 的 os_set_timezone（纯 libc，无需平台能力）。
 */
#include "console_internal.h"
#include "core/config.h"
#include "core/log.h"
#include "core/os.h"
#include "hal/hal.h"
#include <stdio.h>
#include <string.h>

#define MOD "console"

bool console_ipv4_parse(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int parts = 0;

    /* 逐字符解析：sscanf("%u") 会接受前导空白与 '+'，而这些值最终写进
       平台的网络配置文件，必须与平台侧"只含数字和点"的判定一致 */
    if (!s || !*s) return false;
    while (parts < 4) {
        unsigned n = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            if (digits == 1 && n == 0) return false;   /* 拒绝前导 0（如 "01"） */
            n = n * 10 + (unsigned)(*s - '0');
            if (++digits > 3 || n > 255) return false;
            s++;
        }
        if (!digits) return false;
        v = (v << 8) | n;
        parts++;
        if (parts < 4) {
            if (*s != '.') return false;
            s++;
        }
    }
    if (*s) return false;
    if (out) *out = v;
    return true;
}

static bool mask_valid(uint32_t m)
{
    /* 连续的 1 后接连续的 0，且不能是 0.0.0.0 */
    return m != 0 && ((~m + 1) & ~m) == 0;
}

hal_err_t console_net_read(console_net_cfg_t *c)
{
    bool dhcp = true;
    int64_t mtu = -1;

    memset(c, 0, sizeof(*c));
    c->mtu = -1;                       /* 未配置 → 保持系统默认 */
    if (cfg_get_bool("net.dhcp", &dhcp) != HAL_OK) dhcp = true;
    c->dhcp = dhcp;
    cfg_get_str("net.ip", c->ip, sizeof(c->ip));
    cfg_get_str("net.mask", c->mask, sizeof(c->mask));
    cfg_get_str("net.gw", c->gw, sizeof(c->gw));
    cfg_get_str("net.dns", c->dns, sizeof(c->dns));
    if (cfg_get_int("net.mtu", &mtu) == HAL_OK) c->mtu = (int)mtu;
    return HAL_OK;
}

const char *console_net_check(const console_net_cfg_t *c)
{
    uint32_t ip, mask, gw, dns;

    if (c->mtu != -1 && (c->mtu < 576 || c->mtu > 1500)) return "MTU 范围应为 576-1500";
    if (c->dhcp) return NULL;
    if (!console_ipv4_parse(c->ip, &ip)) return "IP 地址格式不正确";
    if (!console_ipv4_parse(c->mask, &mask) || !mask_valid(mask)) return "子网掩码不正确";
    if ((ip & ~mask) == 0 || (ip & ~mask) == ~mask) return "IP 地址不能是网段地址或广播地址";
    if (c->gw[0]) {
        if (!console_ipv4_parse(c->gw, &gw)) return "网关格式不正确";
        if ((gw & mask) != (ip & mask)) return "网关与 IP 不在同一网段";
        if (gw == ip) return "网关不能与 IP 相同";
    }
    if (c->dns[0]) {
        /* 首选,备用：逗号分隔，逐段校验（对齐实机静态页的两个 DNS 框）*/
        const char *p = c->dns;
        int n = 0;
        while (*p) {
            char part[16];
            size_t i = 0;
            while (*p && *p != ',') {
                if (i >= sizeof(part) - 1) return "DNS 格式不正确";
                part[i++] = *p++;
            }
            part[i] = 0;
            if (!console_ipv4_parse(part, &dns)) return "DNS 格式不正确";
            if (++n > 3) return "DNS 最多 3 个地址";
            if (!*p) break;
            p++;                       /* 跳过逗号；尾部逗号视为格式错误 */
            if (!*p) return "DNS 格式不正确";
        }
    }
    return NULL;
}

hal_err_t console_apply_net(void)
{
    console_net_cfg_t c;
    hal_err_t rc;

    if (!hal_has(HAL_MOD_SYS) || !hal()->sys->apply_net) return HAL_ENOTSUP;
    console_net_read(&c);
    if (console_net_check(&c)) return HAL_EINVAL;
    /* mtu <= 0 = 未配置：传 0 给 HAL，由平台保持系统默认 */
    if (c.dhcp) rc = hal()->sys->apply_net("", "", "", "", c.mtu > 0 ? c.mtu : 0);
    else rc = hal()->sys->apply_net(c.ip, c.mask, c.gw, c.dns, c.mtu > 0 ? c.mtu : 0);
    if (rc == HAL_OK) LOGI(MOD, "网络设置已应用：%s", c.dhcp ? "DHCP" : c.ip);
    else LOGE(MOD, "网络设置应用失败：%s", hal_strerror(rc));
    return rc;
}

hal_err_t console_ntp_read(bool *en, char *server, size_t cap)
{
    bool e = true;
    char buf[CONSOLE_NTP_HOST_MAX + 1] = "";

    /* cfg 未写入 → 默认开启（PRD LC-SYS-01）；服务器未写入/为空 → 缺省主机。
       这两处缺省是**行为**而不只是显示：console_apply_time 据此真的去起 ntpd。 */
    if (cfg_get_bool("time.ntp.enable", &e) != HAL_OK) e = true;
    if (cfg_get_str("time.ntp.server", buf, sizeof(buf)) != HAL_OK || !buf[0])
        snprintf(buf, sizeof(buf), CONSOLE_NTP_DEFAULT_HOST);
    if (en) *en = e;
    if (server && cap) snprintf(server, cap, "%s", buf);
    return HAL_OK;
}

hal_err_t console_apply_time(void)
{
    bool en = false;
    char server[CONSOLE_NTP_HOST_MAX + 1] = "";
    hal_err_t rc;

    if (!hal_has(HAL_MOD_SYS) || !hal()->sys->apply_ntp) return HAL_ENOTSUP;
    console_ntp_read(&en, server, sizeof(server));
    rc = hal()->sys->apply_ntp(en ? server : "");
    if (rc != HAL_OK) LOGW(MOD, "NTP 设置应用失败：%s", hal_strerror(rc));
    return rc;
}

/* ============================ 时区 ============================
 *
 * 板上**没有 zoneinfo 数据库**（实测 `ls /usr/share/zoneinfo` 为空），TZ 只能是
 * POSIX 偏移写法：`CST-8`(=UTC+8) / `UTC-5:30`(=UTC+5:30) / `UTC0`。写区名
 * （`Asia/Shanghai`）libc 解析不了会退回 UTC——所以校验函数直接拒掉这类值，
 * 而不是让它"存得进去但不生效"。
 *
 * 偏移符号是 POSIX 的反直觉约定：**local = UTC − offset**，故 `CST-8` 是东八区。
 * 下面返回的秒数已按"东正西负"取反，与页面显示、定时重启换算同一口径。
 */

/** 解出 POSIX TZ 串里的数值偏移（**未取反**的 offset 值，供内部换算）；解析不出返回 false */
static bool tz_parse_offset(const char *tz, int *out_sec)
{
    const char *p = tz;
    int sign = 1, h = 0, m = 0;

    if (!p || !p[0]) return false;
    /* 跳过区名（CST / UTC / EST 等），落到符号或数字上 */
    while (*p && *p != '+' && *p != '-' && (*p < '0' || *p > '9')) p++;
    if (!*p) return false;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }
    if (sscanf(p, "%d:%d", &h, &m) < 1) return false;
    if (h > 24 || m > 59) return false;
    if (out_sec) *out_sec = sign * (h * 3600 + m * 60);
    return true;
}

int console_tz_offset_seconds(const char *tz)
{
    int posix_off = 0;
    if (!tz_parse_offset(tz, &posix_off)) return 0;   /* 解不出按 UTC，不猜时区 */
    return -posix_off;                                /* POSIX: local = UTC − offset */
}

bool console_tz_ok(const char *tz)
{
    size_t i, n;

    if (!tz) return false;
    n = strlen(tz);
    if (n < 1 || n > CONSOLE_TZ_MAX) return false;
    /* 字符集与 cfg 规则 CFG_CHARSET_TZ 保持一致（不接受引号/空格/逗号，避免
       拼进 shell 或配置文件时出问题；`<...>` 区名写法一律不要） */
    for (i = 0; i < n; i++) {
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_+-:", tz[i]))
            return false;
    }
    return tz_parse_offset(tz, NULL);
}

void console_apply_timezone(void)
{
    char tz[CONSOLE_TZ_MAX + 1] = "";

    if (cfg_get_str("time.timezone", tz, sizeof(tz)) != HAL_OK || !tz[0])
        snprintf(tz, sizeof(tz), "%s", CONSOLE_TZ_DEFAULT);
    os_set_timezone(tz);
    LOGI(MOD, "进程时区已应用 %s（OSD 时间与页面显示同源）", tz);
}
