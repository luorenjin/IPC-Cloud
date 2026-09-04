/**
 * @file hal.h
 * @brief IpcCloud 固件 HAL v1 —— 聚合入口
 *
 * 平台实现（platform/<soc>/）必须导出：
 *     const hal_ops_t *hal_platform_get(void);
 * core 层在启动时调用 hal_init(profile_json) 完成：
 *     1) 取得平台函数表；2) 校验版本；3) 将 profile 传给平台（引脚映射、传感器白名单等）。
 *
 * 可选模块（指针可为 NULL）：osd、ivs、crypto、audio。业务层调用前必须判空，
 * 或使用 hal_has(HAL_MOD_x) 判断；能力清单（profile）与此保持一致。
 */
#ifndef IPC_HAL_H
#define IPC_HAL_H

#include "hal_types.h"
#include "hal_video.h"
#include "hal_audio.h"
#include "hal_osd.h"
#include "hal_ivs.h"
#include "hal_gpio.h"
#include "hal_net.h"
#include "hal_storage.h"
#include "hal_sys.h"
#include "hal_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_MOD_VIDEO = 0,
    HAL_MOD_AUDIO,
    HAL_MOD_OSD,
    HAL_MOD_IVS,
    HAL_MOD_GPIO,
    HAL_MOD_NET,
    HAL_MOD_STORAGE,
    HAL_MOD_SYS,
    HAL_MOD_CRYPTO,
    HAL_MOD_COUNT
} hal_module_t;

typedef struct hal_ops {
    uint32_t     version;        /**< 必须等于 HAL_API_VERSION 的主版本 */
    const char  *platform_id;    /**< 与 profile.identity.platform 一致 */

    /**
     * 平台初始化/反初始化。profile_json 为产品能力清单原文（UTF-8，NUL 结尾），
     * 平台从中读取 gpio_map / video.sensors / network.wifi 等自身需要的字段。
     */
    hal_err_t  (*init)(const char *profile_json);
    hal_err_t  (*deinit)(void);

    const hal_video_ops_t   *video;    /**< 必选 */
    const hal_audio_ops_t   *audio;    /**< 可选 */
    const hal_osd_ops_t     *osd;      /**< 可选 */
    const hal_ivs_ops_t     *ivs;      /**< 可选 */
    const hal_gpio_ops_t    *gpio;     /**< 必选（可全部未映射） */
    const hal_net_ops_t     *net;      /**< 必选 */
    const hal_storage_ops_t *storage;  /**< 必选（可 present=false） */
    const hal_sys_ops_t     *sys;      /**< 必选 */
    const hal_crypto_ops_t  *crypto;   /**< 可选 */
} hal_ops_t;

/** 平台导出的唯一符号 */
const hal_ops_t *hal_platform_get(void);

/* ---- core 层使用的便捷封装（hal/hal.c 提供，平台无需实现） ---- */

/** 取平台函数表、校验版本、调用 init；失败返回错误码 */
hal_err_t hal_init(const char *profile_json);
hal_err_t hal_deinit(void);

/** 已初始化后返回全局函数表；未初始化返回 NULL */
const hal_ops_t *hal(void);

/** 模块是否可用（指针非空） */
bool hal_has(hal_module_t mod);

/** 错误码转字符串 */
const char *hal_strerror(hal_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* IPC_HAL_H */
