/**
 * @file hal_audio.h
 * @brief HAL v1 —— 音频采集/播放
 *
 * 采集输出为 PCM 或已编码帧（由 cfg.codec 决定；平台不支持硬编码时返回 PCM，
 * 由 core 层软编码）。播放输入为 PCM S16LE。
 */
#ifndef IPC_HAL_AUDIO_H
#define IPC_HAL_AUDIO_H

#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_AUDIO_IN_MIC = 0,
    HAL_AUDIO_IN_LINE = 1
} hal_audio_input_t;

typedef struct {
    uint32_t    sample_rate;   /**< 8000 / 16000 / 48000 */
    uint32_t    channels;      /**< 1 / 2 */
    hal_codec_t codec;         /**< 期望输出编码；HAL_CODEC_PCM_S16LE 表示原始 */
    uint32_t    frame_ms;      /**< 每帧时长，建议 20/40/64 */
    hal_audio_input_t input;
} hal_audio_cfg_t;

typedef struct {
    bool     capture;
    bool     playback;
    uint32_t inputs_mask;       /**< bit(HAL_AUDIO_IN_x) */
    uint32_t codecs_mask;       /**< 硬编码支持 bit(HAL_CODEC_x) */
    bool     aec;
    bool     agc;
    bool     ns;
} hal_audio_caps_t;

typedef struct hal_audio_ops {
    hal_err_t (*get_caps)(hal_audio_caps_t *caps);

    /* 采集：read 阻塞至一帧就绪或超时；帧需 release_frame */
    hal_err_t (*capture_open)(const hal_audio_cfg_t *cfg);
    hal_err_t (*capture_read)(hal_frame_t *frame, uint32_t timeout_ms);
    hal_err_t (*capture_release)(hal_frame_t *frame);
    hal_err_t (*capture_close)(void);

    /* 播放：PCM S16LE */
    hal_err_t (*play_open)(uint32_t sample_rate, uint32_t channels);
    hal_err_t (*play_write)(const int16_t *pcm, uint32_t samples, uint32_t timeout_ms);
    hal_err_t (*play_close)(void);

    /* 参数：增益 0~100 */
    hal_err_t (*set_capture_gain)(uint32_t gain);
    hal_err_t (*set_play_volume)(uint32_t volume);
    hal_err_t (*set_aec)(bool enable);
    hal_err_t (*set_agc)(bool enable);
    hal_err_t (*set_ns)(bool enable);
} hal_audio_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* IPC_HAL_AUDIO_H */
