/**
 * @file config.h
 * @brief L2 核心服务 —— 配置中心（键值、schema 校验、持久化、变更通知）
 *
 * 键命名与 IDP cfg.* 一致（接入规范 §5.7），点分层级：
 *   video.0.main.codec / image.brightness / record.mode / alarm.motion.enabled /
 *   time.ntp / net.dhcp / wifi.ssid / localUser.name / protocols.rtsp.enabled / gb28181.serverId …
 * 三份来源按优先级合并：profile 默认值 < 持久化用户配置 < 运行期临时覆盖。
 * 所有写入先经 schema 校验（类型、范围、枚举），再原子写文件（写临时 + rename），
 * 成功后发布 EVT_DOM_CONFIG 事件，订阅模块据此热应用。
 */
#ifndef IPC_CORE_CONFIG_H
#define IPC_CORE_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hal/hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_KEY_MAX   96
#define CFG_STR_MAX   256

typedef enum {
    CFG_T_BOOL = 0,
    CFG_T_INT = 1,
    CFG_T_STR = 2,
    CFG_T_JSON = 3        /**< 子树（如 alarm.motion.regions 数组），以 JSON 文本存取 */
} cfg_type_t;

/** 被拒绝的键及原因（供 IDP ack.data.rejected[] 与控制台提示） */
typedef struct {
    char key[CFG_KEY_MAX];
    char reason[64];
} cfg_reject_t;

hal_err_t cfg_init(const char *profile_json, const char *persist_path);
hal_err_t cfg_deinit(void);

/* 读 */
hal_err_t cfg_get_bool(const char *key, bool *v);
hal_err_t cfg_get_int(const char *key, int64_t *v);
hal_err_t cfg_get_str(const char *key, char *buf, size_t cap);
hal_err_t cfg_get_json(const char *key, char *buf, size_t cap);   /**< 子树序列化 */

/* 写（单键立即校验+持久化+通知） */
hal_err_t cfg_set_bool(const char *key, bool v);
hal_err_t cfg_set_int(const char *key, int64_t v);
hal_err_t cfg_set_str(const char *key, const char *v);
hal_err_t cfg_set_json(const char *key, const char *json);

/**
 * 批量写（对应 IDP cfg.set）：全部校验后一次持久化；
 * 校验失败的键写入 rejects（最多 max_rejects），其余照常应用；返回被拒数量。
 */
int cfg_apply_json(const char *values_json, cfg_reject_t *rejects, size_t max_rejects);

/** 导出全部有效配置（合并后）为 JSON */
hal_err_t cfg_dump_json(char *buf, size_t cap);

/** 恢复出厂：删除持久化用户配置，保留 keep_keys（如网络） */
hal_err_t cfg_reset(const char *const *keep_keys, size_t n);

/** 注册键的校验规则（模块初始化时登记，未登记的键拒绝写入） */
typedef struct {
    const char *key_pattern;   /**< 支持 * 通配一层，如 "video.*.main.kbps" */
    cfg_type_t  type;
    int64_t     min, max;      /**< INT 范围 */
    const char *enum_csv;      /**< STR 枚举，逗号分隔；NULL 不限 */
    bool        reboot_required;
} cfg_rule_t;

hal_err_t cfg_register_rules(const cfg_rule_t *rules, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* IPC_CORE_CONFIG_H */
