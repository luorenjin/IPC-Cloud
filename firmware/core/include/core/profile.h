/**
 * @file profile.h
 * @brief L2 核心服务 —— 产品能力清单（profile）加载与查询
 *
 * 加载 profiles/<model>.json，按 profile.v1.schema.json 校验，提供结构化只读视图。
 * 能力清单是"通用功能一套代码"的数据来源：ONVIF GetCapabilities、IDP hello、
 * GB28181 DeviceInfo、控制台菜单显隐都从这里取值，禁止在业务代码中硬编码型号判断。
 */
#ifndef IPC_CORE_PROFILE_H
#define IPC_CORE_PROFILE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hal/hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_MAX_CHANNELS 3
#define PROFILE_MAX_SENSORS  4
#define PROFILE_STR 64

typedef struct {
    int      ch;
    char     name[8];              /**< main / sub / third */
    uint32_t codecs_mask;          /**< bit(HAL_CODEC_x) */
    uint32_t max_w, max_h, max_fps;
    hal_codec_t def_codec;
    uint32_t def_w, def_h, def_fps, def_kbps, def_gop;
    hal_rc_mode_t def_rc;
} profile_channel_t;

typedef struct {
    /* identity */
    char model[PROFILE_STR], vendor[PROFILE_STR], hw[PROFILE_STR], platform[PROFILE_STR];
    char gb_manufacturer[PROFILE_STR];

    /* video */
    uint32_t sensor_count;
    char     sensors[PROFILE_MAX_SENSORS][PROFILE_STR];
    bool     lens_motorized, lens_af, lens_zoom;
    char     lens_driver[PROFILE_STR];
    uint32_t channel_count;
    profile_channel_t channels[PROFILE_MAX_CHANNELS];
    bool     isp_wdr, isp_hdr, isp_3dnr;
    char     daynight[PROFILE_STR];
    uint32_t snapshot_w, snapshot_h;

    /* audio */
    bool audio_in_mic, audio_in_line, audio_out;
    uint32_t audio_codecs_mask;
    bool audio_aec;

    /* ivs */
    char     ivs_engine[PROFILE_STR];      /**< "none" 表示无 */
    uint32_t ivs_kinds_mask;               /**< bit(HAL_IVS_x) */
    uint32_t ivs_max_regions;

    /* storage */
    bool     tf;
    uint32_t tf_max_gb, segment_s, reserve_pct;

    /* network */
    bool eth, wifi;
    char wifi_module[PROFILE_STR];
    bool wifi_5g, wifi_wpa3;

    /* protocols */
    bool idp_enabled;     char idp_broker[PROFILE_STR * 2]; bool idp_psk; uint32_t idp_keepalive_s;
    bool rtsp_enabled;    uint32_t rtsp_port, rtsp_max_sessions;
    bool onvif_enabled;   uint32_t onvif_port; char onvif_events[PROFILE_STR]; bool onvif_discovery;
    bool gb_enabled;      bool gb_tcp, gb_playback, gb_alarm;

    /* limits */
    uint32_t frame_bus_consumers, playback_sessions, mem_budget_mb, cpu_budget_pct;

    /* security / ota */
    bool hw_secure, force_password_change, telnet, ssh;
    bool ota_ab; uint32_t ota_slot_mb, ota_confirm_timeout_s;
} profile_t;

/** 加载并校验；成功后 profile_get() 可用，profile_raw_json() 返回原文供 HAL init */
hal_err_t profile_load(const char *path);
hal_err_t profile_load_from_string(const char *json);
const profile_t *profile_get(void);
const char *profile_raw_json(void);

/** 能力字符串列表（接入规范 §3.4），供 IDP hello.capabilities / ONVIF / GB 使用 */
size_t profile_capabilities(const char **out, size_t max);

/** 按通道名取通道（"main"/"sub"），NULL 表示不存在 */
const profile_channel_t *profile_channel_by_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* IPC_CORE_PROFILE_H */
