/**
 * @file hal_storage.h
 * @brief HAL v1 —— 可移动存储（TF 卡）
 *
 * 文件读写由 core 层用标准 POSIX 接口在 mount_path 下完成；HAL 只负责
 * 检测、挂载、健康与格式化。热插拔以事件轮询上报。
 */
#ifndef IPC_HAL_STORAGE_H
#define IPC_HAL_STORAGE_H

#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_FS_UNKNOWN = 0,
    HAL_FS_FAT32 = 1,
    HAL_FS_EXFAT = 2,
    HAL_FS_EXT4 = 3
} hal_fs_t;

typedef enum {
    HAL_STOR_HEALTH_OK = 0,
    HAL_STOR_HEALTH_WARN = 1,     /**< 读写错误增多/速度下降 */
    HAL_STOR_HEALTH_BAD = 2,      /**< 需更换 */
    HAL_STOR_HEALTH_UNKNOWN = 3
} hal_storage_health_t;

typedef struct {
    bool     present;
    bool     mounted;
    char     mount_path[HAL_PATH_MAX];
    hal_fs_t fs;
    uint64_t total_bytes;
    uint64_t free_bytes;
    hal_storage_health_t health;
    uint32_t io_errors;           /**< 自上电累计 */
    char     cid[HAL_NAME_MAX];   /**< 卡识别码，用于换卡检测 */
} hal_storage_stat_t;

typedef enum {
    HAL_STOR_EVT_INSERTED = 0,
    HAL_STOR_EVT_REMOVED = 1,
    HAL_STOR_EVT_ERROR = 2
} hal_storage_event_kind_t;

typedef struct {
    hal_storage_event_kind_t kind;
    uint64_t ts_us;
} hal_storage_event_t;

typedef struct hal_storage_ops {
    hal_err_t (*stat)(hal_storage_stat_t *st);
    hal_err_t (*mount)(void);
    hal_err_t (*umount)(void);
    /** 格式化为指定文件系统（阻塞，可能数十秒） */
    hal_err_t (*format)(hal_fs_t fs);
    /** 触发一次健康检测（读写小块测速），结果反映在 stat.health */
    hal_err_t (*health_check)(void);
    hal_err_t (*poll_event)(hal_storage_event_t *evt, uint32_t timeout_ms);
} hal_storage_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* IPC_HAL_STORAGE_H */
