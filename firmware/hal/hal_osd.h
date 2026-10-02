/**
 * @file hal_osd.h
 * @brief HAL v1 —— OSD 叠加（文本/位图）
 *
 * 文本渲染由 HAL 完成（SoC 叠加或软件叠加），core 层只传字符串与位置。
 * 时间叠加使用 HAL_OSD_TEXT_TIME 类型，由 HAL 每秒自动刷新，避免跨层高频调用。
 *
 * v1.4（2026-09-29，控制台 OSD 页对齐 TP-LINK 实机）在结构体**末尾**追加
 * align / margin_chars / flicker 三项：
 *   - align=RIGHT 时 pos.x 被忽略，由 HAL 按「文字实测宽度 + margin_chars 个
 *     字符格」从右边框反算（实机「国标模式」的时间与通道名称就是右对齐）；
 *   - margin_chars 即实机的「最小边距」（0–2 格，仅国标模式可见）；
 *   - flicker 为真时 HAL 让该区域周期性隐现（实机「显示效果：闪烁」）。
 * 老实现忽略这三个字段即可（默认 0/false = 左对齐、无闪烁），行为与 v1.3 一致。
 *
 * v1.5（2026-09-29，控制台 OSD「星期」）**无结构体变更，只澄清一个约定**：
 * HAL_OSD_TEXT_TIME 的 text 是 strftime 格式串，其中 `%a`/`%A` 应由 HAL 渲染成
 * **本地语言的星期**（gk7205v200 上就是「星期二」）。板端 rootfs 一般没有 locale
 * 数据、C locale 下 strftime 的 %a 只会给英文缩写，所以这个渲染得由平台层自己做；
 * 没实现的平台照 strftime 走即可（英文星期），降级但不出错。平台层需保证 OSD
 * 字库画得出这几个字（点阵实现见 gk_osd.c 的 GK_CJK_FONT）。
 *
 * v1.6（2026-10-01，控制台「区域覆盖」页对齐 TP-LINK 实机）新增
 * **HAL_OSD_COVER**（隐私区域的矩形遮挡，实机叫「区域覆盖」）：
 *   - 一个遮挡矩形 = 一个区域，`pos` 的 x/y/w/h **四个字段都有效**（归一化
 *     0.0–1.0 的画面比例；矩形必须 w>0 且 h>0），`text` 忽略、`update_text` 无效；
 *   - `color_argb` 是遮挡色（只用 RGB 部分；平台实现拿它当覆盖色，本 SKU 用
 *     不透明纯黑，与实机把遮挡区涂黑的做法一致）；
 *   - 改矩形（`set_pos`）与开关（`set_enable`）与其他区域同义；
 *   - 能力位 `hal_osd_caps_t.cover`：false 表示该平台不支持遮挡矩形，控制台
 *     据此隐藏「区域覆盖」页签（能力驱动，别在各处写死 SoC 名）。
 *   - 遮挡与文本区域共用每通道区域上限（max_regions_per_channel）。
 * 老实现忽略该 kind（返回 HAL_ENOTSUP / 不认）即可。
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
    HAL_OSD_TEXT_TIME = 1,   /**< 时间，HAL 自动刷新；text 为 strftime 格式
                                  （`%a`/`%A` 按 v1.5 约定渲染成本地语言星期） */
    HAL_OSD_BITMAP = 2,      /**< ARGB1555/ARGB8888 位图 */
    HAL_OSD_COVER = 3        /**< v1.6：矩形遮挡（隐私区域的「区域覆盖」）。
                                  用 pos 的 x/y/w/h（都要有效）表达矩形，
                                  color_argb 为遮挡色，text 忽略 */
} hal_osd_kind_t;

/** 文本水平对齐方式（`HAL_OSD_ALIGN_RIGHT` 时 pos.x 无效，见文件头说明） */
typedef enum {
    HAL_OSD_ALIGN_LEFT = 0,
    HAL_OSD_ALIGN_RIGHT = 1
} hal_osd_align_t;

typedef struct {
    hal_osd_kind_t kind;
    hal_rect_t     pos;                    /**< 归一化位置；w/h 对位图与遮挡
                                                （HAL_OSD_COVER）有效 */
    char           text[HAL_OSD_TEXT_MAX];
    uint32_t       font_px;                /**< 字号（像素，按主码流分辩率） */
    uint32_t       color_argb;
    uint32_t       bg_argb;                /**< 0 表示透明 */
    const uint8_t *bitmap;                 /**< kind=BITMAP 时有效 */
    uint32_t       bitmap_w, bitmap_h;
    /* ---- v1.4 追加（放在末尾，避免影响既有聚合初始化） ---- */
    hal_osd_align_t align;                 /**< 默认 LEFT */
    uint32_t       margin_chars;           /**< 右对齐时隔右边框的字符格数（0–2） */
    bool           flicker;                /**< 文本闪烁（周期性隐现） */
} hal_osd_cfg_t;

typedef struct {
    uint32_t max_regions_per_channel;
    bool     bitmap;
    bool     hw_text;                      /**< 硬件文本渲染 */
    bool     right_align;                  /**< v1.4：支持 HAL_OSD_ALIGN_RIGHT */
    bool     flicker;                      /**< v1.4：支持文本闪烁 */
    bool     cover;                        /**< v1.6：支持 HAL_OSD_COVER 矩形遮挡 */
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
