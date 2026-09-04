/**
 * @file module.h
 * @brief L3 功能模块的统一生命周期契约
 *
 * 每个 L3 模块（idp / rtsp / onvif / gb28181 / recorder / ota / console / ivs_mgr / snapshot）
 * 导出一个 module_desc_t。core 启动器按依赖顺序 init → start，退出时逆序 stop → deinit。
 * 模块可编译为静态库或可加载 .so（低内存平台按 profile.protocols.*.enabled 决定是否加载）。
 *
 * 模块只能依赖：hal/*.h、core/include/core/*.h、自身目录。禁止包含 platform/ 与其他模块的私有头。
 */
#ifndef IPC_CORE_MODULE_H
#define IPC_CORE_MODULE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hal/hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOD_STATE_UNLOADED = 0,
    MOD_STATE_INITED = 1,
    MOD_STATE_RUNNING = 2,
    MOD_STATE_STOPPED = 3,
    MOD_STATE_FAILED = 4
} module_state_t;

typedef struct {
    uint32_t rss_kb_estimate;     /**< 声明的常驻内存估算，供启动器做预算检查 */
    uint32_t threads;             /**< 声明的线程数 */
} module_footprint_t;

typedef struct module_desc {
    const char *name;             /**< "idp" / "rtsp" / "onvif" / "gb28181" / "recorder" / "ota" / "console" / "ivs" / "snapshot" */
    uint32_t    version;          /**< 模块版本 */
    const char *const *deps;      /**< 依赖模块名，NULL 结尾；启动器据此排序 */
    module_footprint_t footprint;

    /** 判定本模块在当前 profile 下是否应启用（如 protocols.rtsp.enabled） */
    bool      (*enabled)(void);
    /** 一次性初始化：注册配置规则、订阅事件、分配资源；不得启动线程 */
    hal_err_t (*init)(void);
    /** 启动线程/监听端口 */
    hal_err_t (*start)(void);
    /** 停止线程、关闭端口；可重复 start */
    hal_err_t (*stop)(void);
    hal_err_t (*deinit)(void);
    /** 自检/健康：返回 HAL_OK 或具体错误；启动器周期调用并汇总到 IDP status.report */
    hal_err_t (*health)(char *detail, size_t cap);
} module_desc_t;

/* 启动器 API（core/module_loader.c 实现） */
hal_err_t module_register(const module_desc_t *desc);
hal_err_t module_start_all(void);       /**< 按依赖拓扑排序，跳过 enabled()==false */
hal_err_t module_stop_all(void);
module_state_t module_state(const char *name);
size_t module_list(const module_desc_t **out, size_t max);

/**
 * 健康巡检（主循环周期调用）：连续 3 次 health() 失败的运行模块自动 stop+start 重启。
 * 全部正常返回 true；存在失败或重启后仍失败（模块置 FAILED，应触发看门狗）返回 false。
 */
bool module_health_check(char *detail, size_t cap);

/* 内置模块声明（各模块目录实现） */
extern const module_desc_t mod_idp;
extern const module_desc_t mod_rtsp;
extern const module_desc_t mod_onvif;
extern const module_desc_t mod_gb28181;
extern const module_desc_t mod_recorder;
extern const module_desc_t mod_ota;
extern const module_desc_t mod_console;
extern const module_desc_t mod_ivs;
extern const module_desc_t mod_snapshot;

#ifdef __cplusplus
}
#endif

#endif /* IPC_CORE_MODULE_H */
