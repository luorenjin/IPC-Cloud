/**
 * @file log.h
 * @brief L2 核心服务 —— 日志（分级、模块标签、环形缓冲、可上传）
 *
 * 输出：stderr（开发）/ syslog 或 /var/log（目标板）/ 内存环形缓冲（供 IDP cmd.diag 上传）。
 * 敏感信息（密码、验证码、token）禁止进入日志；提供 log_mask() 做脱敏。
 */
#ifndef IPC_CORE_LOG_H
#define IPC_CORE_LOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_ERROR = 0,
    LOG_WARN = 1,
    LOG_INFO = 2,
    LOG_DEBUG = 3,
    LOG_TRACE = 4
} log_level_t;

void log_init(log_level_t level, uint32_t ring_kb);
void log_set_level(log_level_t level);
void log_set_module_level(const char *module, log_level_t level);

void log_write(log_level_t level, const char *module, const char *file, int line, const char *fmt, ...);

/** 导出环形缓冲内容（最近 N 字节），供诊断上传 */
size_t log_ring_dump(char *buf, size_t cap);

/** 脱敏：保留前 2 后 2 字符，其余替换为 *；返回静态线程局部缓冲 */
const char *log_mask(const char *secret);

#define LOGE(mod, ...) log_write(LOG_ERROR, mod, __FILE__, __LINE__, __VA_ARGS__)
#define LOGW(mod, ...) log_write(LOG_WARN,  mod, __FILE__, __LINE__, __VA_ARGS__)
#define LOGI(mod, ...) log_write(LOG_INFO,  mod, __FILE__, __LINE__, __VA_ARGS__)
#define LOGD(mod, ...) log_write(LOG_DEBUG, mod, __FILE__, __LINE__, __VA_ARGS__)
#define LOGT(mod, ...) log_write(LOG_TRACE, mod, __FILE__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* IPC_CORE_LOG_H */
