/**
 * @file hal_ivs.h
 * @brief HAL v1 —— 智能分析（移动侦测 / 人形 / 区域入侵 / 越界 / 遮挡）
 *
 * 平台差异：GK7205 用 IVE/MD；RV1106 用 NPU；mock 用帧差。
 * 能力可选：hal_ops_t.ivs 可为 NULL，能力清单须同步声明 ivs 为空。
 * 事件以轮询方式获取（poll_event），便于 core 层统一线程模型。
 */
#ifndef IPC_HAL_IVS_H
#define IPC_HAL_IVS_H

#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_IVS_MAX_REGIONS 8
#define HAL_IVS_MAX_OBJECTS 16

typedef enum {
    HAL_IVS_MOTION = 0,
    HAL_IVS_HUMANOID = 1,
    HAL_IVS_INTRUSION = 2,     /**< 区域入侵 */
    HAL_IVS_LINECROSS = 3,     /**< 越界 */
    HAL_IVS_TAMPER = 4,        /**< 视频遮挡 */
    HAL_IVS_KIND_COUNT
} hal_ivs_kind_t;

typedef struct {
    bool       enable;
    uint32_t   sensitivity;               /**< 0~100 */
    uint32_t   region_count;
    hal_rect_t regions[HAL_IVS_MAX_REGIONS];
    /* 越界专用：线段端点（归一化） */
    float      line_x1, line_y1, line_x2, line_y2;
    int        line_dir;                  /**< 0 双向 / 1 A→B / 2 B→A */
    uint32_t   min_duration_ms;           /**< 触发最小持续时间 */
} hal_ivs_cfg_t;

typedef struct {
    hal_rect_t box;
    uint32_t   label;      /**< 0 unknown / 1 person / 2 vehicle … */
    float      score;
} hal_ivs_object_t;

typedef struct {
    hal_ivs_kind_t kind;
    int            ch;                    /**< 分析所用通道（通常为子码流对应的 VI） */
    uint64_t       ts_us;
    bool           start;                 /**< true 开始 / false 结束 */
    uint32_t       object_count;
    hal_ivs_object_t objects[HAL_IVS_MAX_OBJECTS];
} hal_ivs_event_t;

typedef struct {
    uint32_t kinds_mask;                  /**< bit(HAL_IVS_x) */
    uint32_t max_regions;
    bool     objects;                     /**< 是否输出目标框 */
    char     engine[HAL_NAME_MAX];        /**< "ive" / "npu" / "sw" */
} hal_ivs_caps_t;

typedef struct hal_ivs_ops {
    hal_err_t (*get_caps)(hal_ivs_caps_t *caps);
    hal_err_t (*configure)(hal_ivs_kind_t kind, const hal_ivs_cfg_t *cfg);
    hal_err_t (*get_config)(hal_ivs_kind_t kind, hal_ivs_cfg_t *cfg);
    /** 阻塞至有事件或超时；HAL_EAGAIN 表示无事件 */
    hal_err_t (*poll_event)(hal_ivs_event_t *evt, uint32_t timeout_ms);
} hal_ivs_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* IPC_HAL_IVS_H */
