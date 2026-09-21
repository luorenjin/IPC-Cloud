/**
 * @file gk_procfs.h
 * @brief /proc 与 /sys 文本解析（纯函数，不碰硬件，可在 x86 上测试）
 */
#ifndef GK_PROCFS_H
#define GK_PROCFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 整文件读入 buf（NUL 结尾）。文件不存在或超出 cap 时返回 false。 */
bool gk_read_file(const char *path, char *buf, size_t cap);

/** 从 "key: value" 或 "key=value" 行中取值，去首尾空白。未找到返回 false。 */
bool gk_read_line_value(const char *path, const char *key, char *out, size_t cap);

/** 读取只含一个整数的文件（如 /sys/class/net/eth0/speed）。 */
bool gk_read_u64(const char *path, uint64_t *out);

/** 解析 /proc/meminfo 内容，取 MemTotal 与 MemAvailable（单位 kB）。 */
bool gk_parse_meminfo(const char *content, uint32_t *total_kb, uint32_t *avail_kb);

/** 解析 /proc/uptime 内容，取整数秒。 */
bool gk_parse_uptime(const char *content, uint64_t *uptime_s);

/** 解析 /proc/cpuinfo 内容，取 Hardware 字段值。 */
bool gk_parse_cpuinfo_hardware(const char *content, char *out, size_t cap);

#endif /* GK_PROCFS_H */
