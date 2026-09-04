/**
 * @file os.h
 * @brief L2 —— 最小 OS 抽象（线程、互斥、条件变量、时钟、文件原子替换）
 *
 * 目标：Linux（pthread）与 Windows（x86 开发/测试）同源。句柄为不透明指针，避免头文件污染。
 * 这是 core 层唯一允许出现 _WIN32 分支的地方；业务层禁止直接使用平台线程 API。
 */
#ifndef IPC_CORE_OS_H
#define IPC_CORE_OS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct os_mutex os_mutex_t;
typedef struct os_cond  os_cond_t;
typedef struct os_thread os_thread_t;
typedef void (*os_thread_fn)(void *arg);

os_mutex_t *os_mutex_create(void);
void        os_mutex_destroy(os_mutex_t *m);
void        os_mutex_lock(os_mutex_t *m);
void        os_mutex_unlock(os_mutex_t *m);

os_cond_t  *os_cond_create(void);
void        os_cond_destroy(os_cond_t *c);
/** 返回 true 被唤醒，false 超时；timeout_ms=UINT32_MAX 表示无限等待 */
bool        os_cond_wait(os_cond_t *c, os_mutex_t *m, uint32_t timeout_ms);
void        os_cond_signal(os_cond_t *c);
void        os_cond_broadcast(os_cond_t *c);

/** stack_kb=0 使用默认；name 仅用于调试（可为 NULL） */
os_thread_t *os_thread_create(os_thread_fn fn, void *arg, const char *name, uint32_t stack_kb);
void         os_thread_join(os_thread_t *t);

uint64_t os_monotonic_us(void);
int64_t  os_wallclock_ms(void);       /**< UTC 毫秒 */
void     os_sleep_ms(uint32_t ms);

/** 原子替换：将 tmp_path 重命名为 dst_path（覆盖）。成功 0，失败 -1 */
int os_file_replace(const char *tmp_path, const char *dst_path);
/** 读取整个文件到 malloc 缓冲（NUL 结尾）；失败返回 NULL */
char *os_file_read_all(const char *path, size_t *len);
/** 写整个文件（先写 .tmp 再原子替换）；成功 0 */
int os_file_write_atomic(const char *path, const void *data, size_t len);
int os_mkdir_p(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* IPC_CORE_OS_H */
