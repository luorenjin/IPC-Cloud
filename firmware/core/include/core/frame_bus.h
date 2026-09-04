/**
 * @file frame_bus.h
 * @brief L2 核心服务 —— 帧总线（零拷贝、多消费者、按需拉取）
 *
 * 设计：
 *  - 每个编码通道一条总线；生产者线程从 HAL get_frame 取帧后发布；
 *    每个消费者持有独立环形引用队列，慢消费者丢帧不影响他人（丢帧计数可查）。
 *  - 引用计数：帧被所有消费者 release 后才归还 HAL（hal_video.release_frame）。
 *  - 按需：无消费者时生产者停止拉帧并调用 hal_video.stop(ch)，节省编码器与内存；
 *    第一个消费者订阅时 start(ch) 并 request_idr。
 *  - 消费者上限来自 profile.limits.frame_bus_consumers；超限 subscribe 返回 HAL_EBUSY。
 */
#ifndef IPC_CORE_FRAME_BUS_H
#define IPC_CORE_FRAME_BUS_H

#include "hal/hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct frame_bus frame_bus_t;
typedef struct frame_sub frame_sub_t;

typedef struct {
    uint32_t max_consumers;      /**< 来自 profile.limits */
    uint32_t queue_depth;        /**< 每消费者队列深度（帧数），建议 8~30 */
    bool     start_on_demand;    /**< 无消费者时停止编码通道 */
    uint32_t idle_stop_ms;       /**< 最后一个消费者退订后延迟停止，避免抖动 */
} frame_bus_cfg_t;

typedef struct {
    uint32_t consumers;
    uint64_t frames_published;
    uint64_t frames_dropped;     /**< 所有消费者累计丢帧 */
    uint32_t bitrate_kbps;       /**< 最近 1s 统计 */
    uint32_t fps;
    bool     running;
} frame_bus_stats_t;

/* 生命周期（core 启动时为 profile 中每个通道创建一条） */
hal_err_t frame_bus_create(int ch, const frame_bus_cfg_t *cfg, frame_bus_t **bus);
hal_err_t frame_bus_destroy(frame_bus_t *bus);
frame_bus_t *frame_bus_get(int ch);

/* 订阅：name 用于诊断（"rtsp:sess1" / "idp:main" / "recorder"） */
hal_err_t frame_bus_subscribe(frame_bus_t *bus, const char *name, bool want_idr_first, frame_sub_t **sub);
hal_err_t frame_bus_unsubscribe(frame_sub_t *sub);

/** 取帧：阻塞至有帧或超时；返回的帧必须 frame_bus_release */
hal_err_t frame_bus_pull(frame_sub_t *sub, hal_frame_t *frame, uint32_t timeout_ms);
hal_err_t frame_bus_release(frame_sub_t *sub, hal_frame_t *frame);

/** 请求关键帧（新播放者加入时） */
hal_err_t frame_bus_request_idr(frame_bus_t *bus);

hal_err_t frame_bus_stats(frame_bus_t *bus, frame_bus_stats_t *st);
hal_err_t frame_bus_sub_dropped(frame_sub_t *sub, uint64_t *dropped);

#ifdef __cplusplus
}
#endif

#endif /* IPC_CORE_FRAME_BUS_H */
