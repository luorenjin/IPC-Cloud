/**
 * @file hal_net.h
 * @brief HAL v1 —— 网络接口（以太网 / WiFi）
 *
 * WiFi 在 HAL 层通用：Linux 目标统一基于 nl80211 + wpa_supplicant 控制接口实现，
 * 模块差异（内核驱动、固件 blob、上电 GPIO）下沉到 BSP 与 profile，本接口不区分模块。
 * IP 层配置（DHCP/静态 IP/DNS）由 core 网络管理器通过标准系统接口完成，不在 HAL 内。
 */
#ifndef IPC_HAL_NET_H
#define IPC_HAL_NET_H

#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_SSID_MAX 33
#define HAL_PSK_MAX  65
#define HAL_IFNAME_MAX 16

typedef enum {
    HAL_NETIF_ETH = 0,
    HAL_NETIF_WIFI = 1
} hal_netif_t;

typedef enum {
    HAL_WIFI_SEC_OPEN = 0,
    HAL_WIFI_SEC_WPA2 = 1,
    HAL_WIFI_SEC_WPA3 = 2,
    HAL_WIFI_SEC_WPA2_WPA3 = 3
} hal_wifi_sec_t;

typedef struct {
    char           ssid[HAL_SSID_MAX];
    uint8_t        bssid[6];
    int            rssi_dbm;
    uint32_t       freq_mhz;
    hal_wifi_sec_t security;
} hal_wifi_ap_t;

typedef enum {
    HAL_LINK_DOWN = 0,
    HAL_LINK_UP = 1,
    HAL_LINK_CONNECTING = 2,
    HAL_LINK_AUTH_FAILED = 3
} hal_link_state_t;

typedef struct {
    hal_netif_t      type;
    char             ifname[HAL_IFNAME_MAX];
    hal_link_state_t link;
    uint8_t          mac[6];
    /* WiFi 专用 */
    char             ssid[HAL_SSID_MAX];
    int              rssi_dbm;
    uint32_t         freq_mhz;
    /* 以太网专用 */
    uint32_t         speed_mbps;
} hal_netif_status_t;

typedef struct {
    hal_netif_t      type;
    hal_link_state_t link;
    uint64_t         ts_us;
} hal_net_event_t;

typedef struct {
    bool eth;
    bool wifi;
    bool wifi_5g;
    uint32_t wifi_sec_mask;   /**< bit(HAL_WIFI_SEC_x) */
    bool wifi_ap;             /**< 是否支持 AP（热点）模式，供本地配网使用 */
} hal_net_caps_t;

typedef struct hal_net_ops {
    hal_err_t (*get_caps)(hal_net_caps_t *caps);
    hal_err_t (*get_status)(hal_netif_t type, hal_netif_status_t *st);
    hal_err_t (*get_mac)(hal_netif_t type, uint8_t mac[6]);

    /* WiFi（无 WiFi 平台返回 HAL_ENOTSUP） */
    hal_err_t (*wifi_power)(bool on);
    hal_err_t (*wifi_scan)(hal_wifi_ap_t *aps, uint32_t max, uint32_t *count, uint32_t timeout_ms);
    hal_err_t (*wifi_connect)(const char *ssid, const char *psk, hal_wifi_sec_t sec);
    hal_err_t (*wifi_disconnect)(void);

    /**
     * 启动 AP（热点）模式，供未配网时的本地 Web 配网使用。
     * 可选能力：不支持的平台将本指针置 NULL，并在 get_caps 中置 wifi_ap=false。
     * ssid    热点名，不超过 HAL_SSID_MAX-1
     * psk     WPA2 密码，8~63 字符；短于 8 返回 HAL_EINVAL
     * channel 2.4G 信道 1~13；0 表示由实现自选
     * 已启动时重复调用返回 HAL_EBUSY。
     */
    hal_err_t (*wifi_ap_start)(const char *ssid, const char *psk, uint8_t channel);
    /** 停止 AP；未启动时返回 HAL_ESTATE */
    hal_err_t (*wifi_ap_stop)(void);

    /** 链路事件轮询；HAL_EAGAIN 表示无事件 */
    hal_err_t (*poll_event)(hal_net_event_t *evt, uint32_t timeout_ms);
} hal_net_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* IPC_HAL_NET_H */
