/**
 * @file hal_video.h
 * @brief HAL v1 —— 视频采集/ISP/编码/抓图/镜头控制
 *
 * 线程模型：get_frame 可被多个消费者线程并发调用（每个消费者持有独立订阅），
 * 其余控制接口由 core 层串行化调用，实现方无需加锁。
 * 帧生命周期：get_frame 返回的帧必须由同一消费者调用 release_frame 归还。
 */
#ifndef IPC_HAL_VIDEO_H
#define IPC_HAL_VIDEO_H

#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_VIDEO_MAX_CHANNELS 3

typedef enum {
    HAL_ISP_LINEAR = 0,
    HAL_ISP_WDR = 1,      /**< 2 帧 WDR（SoC 合成） */
    HAL_ISP_HDR = 2       /**< 传感器行交叠 HDR */
} hal_isp_mode_t;

typedef enum {
    HAL_DAYNIGHT_AUTO = 0,
    HAL_DAYNIGHT_DAY = 1,
    HAL_DAYNIGHT_NIGHT = 2
} hal_daynight_t;

typedef enum {
    HAL_LENS_FIXED = 0,
    HAL_LENS_MOTORIZED = 1
} hal_lens_type_t;

/* ---- 图像页可调项的子枚举（都随 hal_image_t 一次下发） ----
 * 为什么不各开一个 HAL op：这些项在 UI 上同属「画面显示 → 图像」一页、
 * 由同一个 REST 端点一次提交，HAL 层没必要为每个控件加一个函数指针；
 * 已有的 set_daynight / set_isp_mode 保留原样（它们是独立语义，且要读回状态）。 */

typedef enum {
    HAL_EXP_AUTO = 0,
    HAL_EXP_MANUAL = 1
} hal_exposure_mode_t;

typedef enum {
    HAL_FLICKER_OFF = 0,
    HAL_FLICKER_50HZ = 1,
    HAL_FLICKER_60HZ = 2
} hal_antiflicker_t;

typedef enum {
    HAL_AWB_AUTO = 0,
    HAL_AWB_INDOOR = 1,
    HAL_AWB_OUTDOOR = 2
} hal_awb_mode_t;

/** 红外补光灯：自动 = 跟随日夜切换；关闭/常开 = 强制 */
typedef enum {
    HAL_IR_AUTO = 0,
    HAL_IR_OFF = 1,
    HAL_IR_ON = 2
} hal_ir_mode_t;

/** 编码通道配置 */
typedef struct {
    hal_codec_t          codec;      /**< H264 / H265 / MJPEG */
    hal_codec_profile_t  profile;
    uint32_t             width;
    uint32_t             height;
    uint32_t             fps;
    hal_rc_mode_t        rc;
    uint32_t             bitrate_kbps;
    uint32_t             gop;         /**< 关键帧间隔（帧数） */
    uint32_t             max_qp;      /**< 0 表示使用平台默认 */
    uint32_t             min_qp;
    bool                 smart_enc;   /**< 是否启用智能编码（H.264+ / H.265+，SmartP/LTR） */
    uint32_t             bg_interval; /**< SmartP 背景关键帧间隔（0 使用平台默认，如 250） */
} hal_enc_cfg_t;

/** 图像参数，范围 0~100；-1 表示不修改 */
typedef struct {
    int brightness;
    int contrast;
    int saturation;
    int sharpness;
    int hue;
    int flip;      /**< 0/1，-1 不改 */
    int mirror;    /**< 0/1，-1 不改 */
    int denoise_3d; /**< 0~100 强度 */
    int backlight_comp; /**< 0/1 区域补偿（背光补偿） */
    /* ---- 以下不是 0~100 图像参数，而是「图像页」上同属一屏的其它项，
     * 统一挂在这里随 set_image 一次下发（理由见上面 hal_exposure_mode_t 注释）。
     * 全部遵守同一契约：**-1 = 不修改**，越界 = EINVAL。 ---- */
    int exposure_mode;   /**< hal_exposure_mode_t */
    int exposure_level;  /**< -3..3，AE 曝光补偿档位（0 = 基线） */
    int antiflicker;     /**< hal_antiflicker_t */
    int awb_mode;        /**< hal_awb_mode_t */
    int ir_mode;         /**< hal_ir_mode_t */
    int ir_sensitivity;  /**< 0..7，日夜自动切换灵敏度（越大越早切夜视） */
    int ir_delay_s;      /**< 5..60，连续满足阈值多久才真的切（防抖） */
} hal_image_t;

/** 传感器信息 */
typedef struct {
    char     name[HAL_NAME_MAX];   /**< 如 "sc230ai" */
    hal_size_t active;             /**< 有效像素 */
    uint32_t max_fps;
    bool     hdr_supported;
    uint32_t i2c_id;               /**< 探测到的传感器 ID */
} hal_sensor_info_t;

/** 视频能力（供能力清单校验与 UI 灰显） */
typedef struct {
    uint32_t     channels;                 /**< 可用编码通道数 */
    hal_size_t   max_size[HAL_VIDEO_MAX_CHANNELS];
    uint32_t     max_fps[HAL_VIDEO_MAX_CHANNELS];
    uint32_t     codecs_mask;              /**< bit(HAL_CODEC_x) */
    bool         wdr;
    bool         hdr;
    bool         denoise_3d;
    uint32_t     max_consumers;            /**< 帧总线消费者上限（平台建议值） */
    uint32_t     max_held_frames;          /**< 单通道允许同时持有（未 release）的帧数；
                                                平台不足时 get_frame 返回 HAL_EBUSY。
                                                帧总线据此收紧每消费者环深。 */
    hal_lens_type_t lens;
    bool         lens_af;
    bool         lens_zoom;
} hal_video_caps_t;

typedef struct hal_video_ops {
    /* 生命周期 */
    hal_err_t (*open)(void);
    hal_err_t (*close)(void);
    hal_err_t (*probe_sensor)(hal_sensor_info_t *info);
    hal_err_t (*get_caps)(hal_video_caps_t *caps);

    /* 编码通道 */
    hal_err_t (*set_encoder)(int ch, const hal_enc_cfg_t *cfg);   /**< 运行中可调帧率/码率/GOP；改分辩率需 stop */
    hal_err_t (*get_encoder)(int ch, hal_enc_cfg_t *cfg);
    hal_err_t (*start)(int ch);
    hal_err_t (*stop)(int ch);
    hal_err_t (*request_idr)(int ch);

    /* 取帧（阻塞至有帧或超时；timeout_ms=0 非阻塞） */
    hal_err_t (*get_frame)(int ch, hal_frame_t *frame, uint32_t timeout_ms);
    hal_err_t (*release_frame)(hal_frame_t *frame);

    /* 抓图：独立于编码通道，返回 JPEG 帧（需 release_frame） */
    hal_err_t (*snapshot)(int ch, uint32_t jpeg_quality, hal_frame_t *jpeg);

    /* ISP / 图像 */
    hal_err_t (*set_isp_mode)(hal_isp_mode_t mode);
    hal_err_t (*get_isp_mode)(hal_isp_mode_t *mode);
    hal_err_t (*set_image)(const hal_image_t *img);
    hal_err_t (*get_image)(hal_image_t *img);
    hal_err_t (*set_daynight)(hal_daynight_t mode);
    hal_err_t (*get_daynight)(hal_daynight_t *mode, bool *is_night_now);

    /* 镜头（可选；定焦返回 HAL_ENOTSUP，指针可为 NULL） */
    hal_err_t (*lens_focus)(int dir /* -1 near, 0 stop, +1 far */, uint32_t speed);
    hal_err_t (*lens_zoom)(int dir /* -1 wide, 0 stop, +1 tele */, uint32_t speed);
    hal_err_t (*lens_af_trigger)(void);
} hal_video_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* IPC_HAL_VIDEO_H */
