/**
 * @file hal_osd.h
 * @brief HAL v1 —— OSD 叠加（文本/位图）
 *
 * 文本渲染由 HAL 完成（SoC 叠加或软件叠加），core 层只传字符串与位置。
 * 时间叠加使用 HAL_OSD_TEXT_TIME 类型，由 HAL 每秒自动刷新，避免跨层高频调用。
 */
#ifndef IPC_HAL_OSD_H
#define IPC_HAL_OSD_H

#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_OSD_TEXT_MAX 64

typedef enum {
    HAL_OSD_TEXT = 0,        /**< 静态文本（通道名等） */
    HAL_OSD_TEXT_TIME = 1,   /**< 时间，HAL 自动刷新；text 为 strftime 格式 */
    HAL_OSD_BITMAP = 2       /**< ARGB1555/ARGB8888 位图 */
} hal_osd_kind_t;

typedef struct {
    hal_osd_kind_t kind;
    hal_rect_t     pos;                    /**< 归一化位置，w/h 对位图有效 */
    char           text[HAL_OSD_TEXT_MAX];
    uint32_t       font_px;                /**< 字号（像素，按主码流分辩率） */
    uint32_t       color_argb;
    uint32_t       bg_argb;                /**< 0 表示透明 */
    const uint8_t *bitmap;                 /**< kind=BITMAP 时有效 */
    uint32_t       bitmap_w, bitmap_h;
} hal_osd_cfg_t;

typedef struct {
    uint32_t max_regions_per_channel;
    bool     bitmap;
    bool     hw_text;                      /**< 硬件文本渲染 */
} hal_osd_caps_t;

typedef struct hal_osd_ops {
    hal_err_t (*get_caps)(hal_osd_caps_t *caps);
    /** 创建区域并返回 region_id（>=0） */
    hal_err_t (*create_region)(int ch, const hal_osd_cfg_t *cfg, int *region_id);
    hal_err_t (*update_text)(int region_id, const char *text);
    hal_err_t (*set_pos)(int region_id, const hal_rect_t *pos);
    hal_err_t (*set_enable)(int region_id, bool enable);
    hal_err_t (*destroy_region)(int region_id);
} hal_osd_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* IPC_HAL_OSD_H */
