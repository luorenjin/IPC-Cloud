/**
 * @file event_bus.h
 * @brief L2 核心服务 —— 进程内事件总线（发布/订阅，异步投递）
 *
 * 事件是模块间解耦的唯一方式：IVS/GPIO/存储/网络/OTA 等产生事件，
 * IDP/GB28181/ONVIF/录像器等订阅并各自转换为协议上报。
 * 投递在事件总线自有线程中顺序执行，回调内禁止阻塞 >10ms（否则应转发到模块自身队列）。
 */
#ifndef IPC_CORE_EVENT_BUS_H
#define IPC_CORE_EVENT_BUS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hal/hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 事件类型：高 8 位为域，低 8 位为动作 */
#define EVT_DOMAIN(x)   ((x) >> 8)
#define EVT_MAKE(d, a)  (((d) << 8) | (a))

enum evt_domain {
    EVT_DOM_SYS = 1,      /**< 启动完成、时间同步、内存告警、看门狗 */
    EVT_DOM_NET = 2,      /**< 链路 up/down、IP 变更、云连接状态 */
    EVT_DOM_STORAGE = 3,  /**< TF 插拔、满、错误 */
    EVT_DOM_IVS = 4,      /**< 移动侦测/人形/入侵/越界/遮挡 */
    EVT_DOM_GPIO = 5,     /**< 按键、报警输入 */
    EVT_DOM_RECORD = 6,   /**< 分段完成、索引更新、覆盖删除 */
    EVT_DOM_OTA = 7,      /**< 阶段变化 */
    EVT_DOM_BIND = 8,     /**< 绑定/解绑状态 */
    EVT_DOM_STREAM = 9,   /**< 消费者增减、超限 */
    EVT_DOM_CONFIG = 10   /**< 配置项变更（key 列表） */
};

/* 常用事件类型（各域动作号从 1 起） */
#define EVT_SYS_STARTED            EVT_MAKE(EVT_DOM_SYS, 1)      /**< 所有模块启动完成 */
#define EVT_SYS_MODULE_FAILED      EVT_MAKE(EVT_DOM_SYS, 2)      /**< payload.str = 模块名 */
#define EVT_SYS_MEM_PRESSURE       EVT_MAKE(EVT_DOM_SYS, 3)      /**< payload.u32 = 当前 RSS KB */
#define EVT_CONFIG_CHANGED         EVT_MAKE(EVT_DOM_CONFIG, 1)   /**< payload.str = 键名 */
#define EVT_STREAM_CONSUMER_ADDED  EVT_MAKE(EVT_DOM_STREAM, 1)   /**< ch, payload.u32 = 当前消费者数 */
#define EVT_STREAM_CONSUMER_GONE   EVT_MAKE(EVT_DOM_STREAM, 2)
#define EVT_STREAM_LIMIT           EVT_MAKE(EVT_DOM_STREAM, 3)   /**< 订阅被拒：超限 */
#define EVT_STREAM_STARTED         EVT_MAKE(EVT_DOM_STREAM, 4)   /**< 编码通道按需启动 */
#define EVT_STREAM_STOPPED         EVT_MAKE(EVT_DOM_STREAM, 5)   /**< 编码通道按需停止 */

typedef struct {
    uint32_t type;        /**< EVT_MAKE(domain, action) */
    uint64_t ts_us;       /**< hal_sys.monotonic_us */
    int      ch;          /**< 相关通道，-1 表示无 */
    /* 负载：小数据内联，大数据由发布者管理生命周期并通过 ptr 传递（订阅者不得持有） */
    union {
        int32_t  i32;
        uint32_t u32;
        int64_t  i64;
        struct { const void *ptr; size_t len; } blob;
        char     str[64];
    } payload;
} event_t;

typedef void (*event_handler_t)(const event_t *evt, void *user);

typedef struct event_sub event_sub_t;

hal_err_t event_bus_init(uint32_t queue_depth);
hal_err_t event_bus_deinit(void);

/** domain_mask：bit(EVT_DOM_x) 组合；0 表示全部 */
hal_err_t event_bus_subscribe(uint32_t domain_mask, event_handler_t handler, void *user, event_sub_t **sub);
hal_err_t event_bus_unsubscribe(event_sub_t *sub);

/** 异步发布（拷贝 event_t）；队列满返回 HAL_EAGAIN 并计数 */
hal_err_t event_bus_publish(const event_t *evt);

/** 便捷：发布只含类型与通道的事件 */
hal_err_t event_bus_emit(uint32_t type, int ch);

uint64_t event_bus_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* IPC_CORE_EVENT_BUS_H */
