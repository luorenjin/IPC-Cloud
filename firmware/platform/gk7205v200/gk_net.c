/**
 * @file gk_net.c
 * @brief GK7205V200 网络状态；WiFi 本期为桩
 *
 * 以太网信息取自 getifaddrs 与 /sys/class/net/<if>/*。WiFi 模块型号
 * 尚未冻结（决策记录 §2.3），全部返回 HAL_ENOTSUP——console_net.c
 * 已有 hal_has 判空保护，配网页会正确显示“不支持”。
 */
#include "hal/hal.h"
#include "gk_procfs.h"
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

#define ETH_IFNAME "eth0"

static hal_err_t net_get_caps(hal_net_caps_t *caps)
{
    if (!caps) return HAL_EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->eth = true;
    /* WiFi 模块型号未定（决策记录 §2.3），本期不声明任何 WiFi 能力 */
    caps->wifi = false;
    caps->wifi_5g = false;
    caps->wifi_sec_mask = 0;
    caps->wifi_ap = false;
    return HAL_OK;
}

#ifndef _WIN32
/* 填充 eth0 的 IPv4 地址与掩码；未获取到时保持空串（hal_net.h 契约） */
static void fill_ipv4_mask(const char *ifname, char *ip, size_t ipc, char *mask, size_t maskc)
{
    struct ifaddrs *ifa = NULL, *p;

    if (ip) ip[0] = '\0';
    if (mask) mask[0] = '\0';
    if (getifaddrs(&ifa) != 0) return;
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_name) continue;
        if (p->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(p->ifa_name, ifname) != 0) continue;
        if (ip) inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, ip, (socklen_t)ipc);
        if (mask && p->ifa_netmask)
            inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_netmask)->sin_addr, mask, (socklen_t)maskc);
        break;
    }
    freeifaddrs(ifa);
}

/**
 * 默认网关：/proc/net/route 里 Destination 为全 0 的那一行。
 * 网关列是**主机字节序的十六进制**（192.168.1.1 打印成 0101A8C0），
 * 直接塞进 in_addr 再 inet_ntop 即可——本平台是小端，字节序天然对上。
 */
static void fill_gw(const char *ifname, char *out, size_t cap)
{
    FILE *fp;
    char line[256], iface[HAL_IFNAME_MAX];
    unsigned long dest = 1, gw = 0;
    bool head = true;

    out[0] = '\0';
    fp = fopen("/proc/net/route", "r");
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        if (head) { head = false; continue; }   /* 跳过表头 */
        if (sscanf(line, "%15s %lx %lx", iface, &dest, &gw) != 3) continue;
        if (strcmp(iface, ifname) != 0 || dest != 0) continue;
        {
            struct in_addr a;
            a.s_addr = (uint32_t)gw;
            if (!inet_ntop(AF_INET, &a, out, (socklen_t)cap)) out[0] = '\0';
        }
        break;
    }
    fclose(fp);
}

/** 主 DNS：/etc/resolv.conf 的第一条 nameserver */
static void fill_dns(char *out, size_t cap)
{
    FILE *fp;
    char line[256];

    out[0] = '\0';
    fp = fopen("/etc/resolv.conf", "r");
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line, *v;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "nameserver", 10) != 0) continue;
        p += 10;
        while (*p == ' ' || *p == '\t' || *p == '=') p++;
        v = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (p > v) { *p = '\0'; snprintf(out, cap, "%s", v); }
        break;
    }
    fclose(fp);
}
#endif

static void fill_mac(const char *ifname, uint8_t mac[6])
{
    char path[128], val[64];
    unsigned int b[6];

    memset(mac, 0, 6);
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    if (!gk_read_file(path, val, sizeof(val))) return;
    if (sscanf(val, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
    }
}

static hal_err_t net_get_status(hal_netif_t type, hal_netif_status_t *st)
{
    char path[128], val[64];
    uint64_t speed = 0;

    if (!st) return HAL_EINVAL;
    if (type == HAL_NETIF_WIFI) return HAL_ENOTSUP;
    if (type != HAL_NETIF_ETH) return HAL_EINVAL;

    memset(st, 0, sizeof(*st));
    st->type = HAL_NETIF_ETH;
    snprintf(st->ifname, sizeof(st->ifname), "%s", ETH_IFNAME);
    st->link = HAL_LINK_DOWN;

    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ETH_IFNAME);
    if (gk_read_file(path, val, sizeof(val)) && strncmp(val, "up", 2) == 0) {
        st->link = HAL_LINK_UP;
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/speed", ETH_IFNAME);
    if (gk_read_u64(path, &speed)) st->speed_mbps = (uint32_t)speed;

    fill_mac(ETH_IFNAME, st->mac);
#ifndef _WIN32
    fill_ipv4_mask(ETH_IFNAME, st->ip, sizeof(st->ip), st->mask, sizeof(st->mask));
    fill_gw(ETH_IFNAME, st->gw, sizeof(st->gw));
    fill_dns(st->dns, sizeof(st->dns));
    {
        uint64_t mtu = 0;
        snprintf(path, sizeof(path), "/sys/class/net/%s/mtu", ETH_IFNAME);
        if (gk_read_u64(path, &mtu) && mtu > 0 && mtu <= 65535) st->mtu = (uint32_t)mtu;
    }
#endif

    /* 网卡不存在时以上全部取不到值，仍返回 HAL_OK：ip 为空串即表示“未知”，
       这是 hal_net.h 明确约定的语义，调用方据此判断 */
    return HAL_OK;
}

static hal_err_t net_get_mac(hal_netif_t type, uint8_t mac[6])
{
    if (!mac) return HAL_EINVAL;
    if (type == HAL_NETIF_WIFI) return HAL_ENOTSUP;
    if (type != HAL_NETIF_ETH) return HAL_EINVAL;
    fill_mac(ETH_IFNAME, mac);
    return HAL_OK;
}

/* WiFi 全部为桩 */
static hal_err_t net_wifi_power(bool on) { (void)on; return HAL_ENOTSUP; }
static hal_err_t net_wifi_scan(hal_wifi_ap_t *aps, uint32_t max, uint32_t *count, uint32_t timeout_ms)
{
    (void)aps; (void)max; (void)timeout_ms;
    if (count) *count = 0;
    return HAL_ENOTSUP;
}
static hal_err_t net_wifi_connect(const char *ssid, const char *psk, hal_wifi_sec_t sec)
{
    (void)ssid; (void)psk; (void)sec; return HAL_ENOTSUP;
}
static hal_err_t net_wifi_disconnect(void) { return HAL_ENOTSUP; }
/* AP（热点）模式为可选能力：本平台不实现，函数表中对应指针置 NULL，
   并在 net_get_caps 中同步置 wifi_ap=false——两者必须一致，这是
   hal_net.h 明文约定的契约（hal_conformance 的 HAL-07 会校验）。 */

static hal_err_t net_poll_event(hal_net_event_t *evt, uint32_t timeout_ms)
{
    (void)evt; (void)timeout_ms;
    /* 本期不做链路事件监听（需 netlink socket）。返回 EAGAIN 表示无事件，
       console 的轮询会正常继续，不会误判为故障 */
    return HAL_EAGAIN;
}

const hal_net_ops_t gk_net_ops = {
    net_get_caps,
    net_get_status,
    net_get_mac,
    net_wifi_power,
    net_wifi_scan,
    net_wifi_connect,
    net_wifi_disconnect,
    NULL,   /* wifi_ap_start：不支持，caps.wifi_ap 已置 false */
    NULL,   /* wifi_ap_stop */
    net_poll_event
};
