/**
 * @file hal_sys.h
 * @brief HAL v1 —— 系统：芯片标识、时钟、看门狗、复位、资源、OTA 双分区
 *
 * OTA 流程（core 层驱动）：
 *   ota_begin(slot, size) → ota_write()* → ota_end(sha256) → ota_switch_slot(slot) → reboot
 *   → 新固件启动后业务就绪 → ota_confirm()；未 confirm 则 bootloader 下次启动回滚。
 */
#ifndef IPC_HAL_SYS_H
#define IPC_HAL_SYS_H

#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_BOOT_POWER_ON = 0,
    HAL_BOOT_SOFT_RESET = 1,
    HAL_BOOT_WATCHDOG = 2,
    HAL_BOOT_OTA_ROLLBACK = 3,
    HAL_BOOT_UNKNOWN = 4
} hal_boot_reason_t;

typedef struct {
    char     platform_id[HAL_NAME_MAX];   /**< "gk7205v200" / "rv1106" / "mock" */
    char     chip_id[HAL_NAME_MAX];       /**< 芯片唯一 ID（十六进制） */
    char     soc_name[HAL_NAME_MAX];
    uint32_t cpu_mhz;
    uint32_t cpu_cores;
    uint32_t mem_total_kb;
    uint32_t flash_total_kb;
    uint32_t hal_version;                 /**< 实现所依据的 HAL_API_VERSION */
} hal_sys_info_t;

typedef struct {
    uint32_t mem_total_kb;
    uint32_t mem_free_kb;
    uint32_t mem_avail_kb;
    uint32_t cpu_usage_pct;      /**< 0~100，自上次调用以来的平均值 */
    int      temp_milli_c;       /**< 无温度传感器时为 INT32_MIN */
    uint64_t uptime_s;
} hal_sys_stats_t;

typedef struct {
    int      current_slot;       /**< 0 A / 1 B */
    int      other_slot;
    bool     pending_confirm;    /**< 当前分区为新刷入且未 confirm */
    char     current_version[HAL_NAME_MAX];
    char     other_version[HAL_NAME_MAX];
    uint32_t slot_size_kb;
} hal_ota_state_t;

typedef struct hal_sys_ops {
    hal_err_t (*get_info)(hal_sys_info_t *info);
    hal_err_t (*get_stats)(hal_sys_stats_t *st);
    hal_err_t (*get_boot_reason)(hal_boot_reason_t *reason);

    /** 单调时钟微秒；所有 HAL 帧/事件时间戳的时钟源 */
    uint64_t  (*monotonic_us)(void);
    /** 设置系统墙钟（UTC 秒）；供 NTP/平台校时后同步 RTC */
    hal_err_t (*set_wallclock)(int64_t utc_seconds);

    hal_err_t (*reboot)(void);
    hal_err_t (*factory_reset)(bool keep_network);   /**< 清用户数据，保留证书/验证码 */

    /* 看门狗 */
    hal_err_t (*wdt_enable)(uint32_t timeout_s);
    hal_err_t (*wdt_feed)(void);
    hal_err_t (*wdt_disable)(void);

    /* OTA */
    hal_err_t (*ota_get_state)(hal_ota_state_t *st);
    hal_err_t (*ota_begin)(int slot, uint32_t total_size);
    hal_err_t (*ota_write)(const void *data, uint32_t len);
    hal_err_t (*ota_end)(const uint8_t sha256[32]);     /**< 校验整段镜像 */
    hal_err_t (*ota_switch_slot)(int slot);             /**< 设置下次启动分区并置 pending */
    hal_err_t (*ota_confirm)(void);                     /**< 清 pending */
    hal_err_t (*ota_abort)(void);
} hal_sys_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* IPC_HAL_SYS_H */
