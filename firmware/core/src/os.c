/**
 * @file os.c
 * @brief 最小 OS 抽象实现：Windows（Win32）/ POSIX（pthread）
 */
#include "core/os.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
/* ============================ Windows ============================ */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>

struct os_mutex { CRITICAL_SECTION cs; };
struct os_cond  { CONDITION_VARIABLE cv; };
struct os_thread { HANDLE h; os_thread_fn fn; void *arg; };

os_mutex_t *os_mutex_create(void)
{
    os_mutex_t *m = (os_mutex_t *)calloc(1, sizeof(*m));
    if (m) InitializeCriticalSection(&m->cs);
    return m;
}
void os_mutex_destroy(os_mutex_t *m) { if (m) { DeleteCriticalSection(&m->cs); free(m); } }
void os_mutex_lock(os_mutex_t *m) { EnterCriticalSection(&m->cs); }
void os_mutex_unlock(os_mutex_t *m) { LeaveCriticalSection(&m->cs); }

os_cond_t *os_cond_create(void)
{
    os_cond_t *c = (os_cond_t *)calloc(1, sizeof(*c));
    if (c) InitializeConditionVariable(&c->cv);
    return c;
}
void os_cond_destroy(os_cond_t *c) { free(c); }
bool os_cond_wait(os_cond_t *c, os_mutex_t *m, uint32_t timeout_ms)
{
    DWORD to = (timeout_ms == UINT32_MAX) ? INFINITE : (DWORD)timeout_ms;
    return SleepConditionVariableCS(&c->cv, &m->cs, to) != 0;
}
void os_cond_signal(os_cond_t *c) { WakeConditionVariable(&c->cv); }
void os_cond_broadcast(os_cond_t *c) { WakeAllConditionVariable(&c->cv); }

static DWORD WINAPI thread_tramp(LPVOID p)
{
    os_thread_t *t = (os_thread_t *)p;
    t->fn(t->arg);
    return 0;
}
os_thread_t *os_thread_create(os_thread_fn fn, void *arg, const char *name, uint32_t stack_kb)
{
    os_thread_t *t = (os_thread_t *)calloc(1, sizeof(*t));
    (void)name;
    if (!t) return NULL;
    t->fn = fn; t->arg = arg;
    t->h = CreateThread(NULL, (SIZE_T)stack_kb * 1024, thread_tramp, t, 0, NULL);
    if (!t->h) { free(t); return NULL; }
    return t;
}
void os_thread_join(os_thread_t *t)
{
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

uint64_t os_monotonic_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000ULL) / (uint64_t)freq.QuadPart);
}
int64_t os_wallclock_ms(void)
{
    FILETIME ft; ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}
void os_sleep_ms(uint32_t ms) { Sleep(ms); }

int os_file_replace(const char *tmp, const char *dst)
{
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}
int os_mkdir_p(const char *path)
{
    char buf[512]; size_t n = strlen(path);
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') { buf[i] = 0; _mkdir(buf); buf[i] = '\\'; }
    }
    _mkdir(buf);
    return 0;
}

#else
/* ============================ POSIX ============================ */
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

struct os_mutex { pthread_mutex_t mu; };
struct os_cond  { pthread_cond_t cv; };
struct os_thread { pthread_t th; os_thread_fn fn; void *arg; };

os_mutex_t *os_mutex_create(void)
{
    os_mutex_t *m = (os_mutex_t *)calloc(1, sizeof(*m));
    if (m) pthread_mutex_init(&m->mu, NULL);
    return m;
}
void os_mutex_destroy(os_mutex_t *m) { if (m) { pthread_mutex_destroy(&m->mu); free(m); } }
void os_mutex_lock(os_mutex_t *m) { pthread_mutex_lock(&m->mu); }
void os_mutex_unlock(os_mutex_t *m) { pthread_mutex_unlock(&m->mu); }

os_cond_t *os_cond_create(void)
{
    os_cond_t *c = (os_cond_t *)calloc(1, sizeof(*c));
    if (c) {
        pthread_condattr_t a;
        pthread_condattr_init(&a);
        pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
        pthread_cond_init(&c->cv, &a);
        pthread_condattr_destroy(&a);
    }
    return c;
}
void os_cond_destroy(os_cond_t *c) { if (c) { pthread_cond_destroy(&c->cv); free(c); } }
bool os_cond_wait(os_cond_t *c, os_mutex_t *m, uint32_t timeout_ms)
{
    if (timeout_ms == UINT32_MAX) return pthread_cond_wait(&c->cv, &m->mu) == 0;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        return pthread_cond_timedwait(&c->cv, &m->mu, &ts) == 0;
    }
}
void os_cond_signal(os_cond_t *c) { pthread_cond_signal(&c->cv); }
void os_cond_broadcast(os_cond_t *c) { pthread_cond_broadcast(&c->cv); }

static void *thread_tramp(void *p)
{
    os_thread_t *t = (os_thread_t *)p;
    t->fn(t->arg);
    return NULL;
}
os_thread_t *os_thread_create(os_thread_fn fn, void *arg, const char *name, uint32_t stack_kb)
{
    os_thread_t *t = (os_thread_t *)calloc(1, sizeof(*t));
    pthread_attr_t attr;
    if (!t) return NULL;
    t->fn = fn; t->arg = arg;
    pthread_attr_init(&attr);
    if (stack_kb) pthread_attr_setstacksize(&attr, (size_t)stack_kb * 1024);
    if (pthread_create(&t->th, &attr, thread_tramp, t) != 0) { pthread_attr_destroy(&attr); free(t); return NULL; }
    pthread_attr_destroy(&attr);
#if defined(__linux__)
    if (name) pthread_setname_np(t->th, name);
#else
    (void)name;
#endif
    return t;
}
void os_thread_join(os_thread_t *t) { if (!t) return; pthread_join(t->th, NULL); free(t); }

uint64_t os_monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
int64_t os_wallclock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
void os_sleep_ms(uint32_t ms)
{
    struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
int os_file_replace(const char *tmp, const char *dst) { return rename(tmp, dst) == 0 ? 0 : -1; }
int os_mkdir_p(const char *path)
{
    char buf[512]; size_t n = strlen(path);
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/') { buf[i] = 0; mkdir(buf, 0700); buf[i] = '/'; }
    }
    mkdir(buf, 0700);
    return 0;
}
#endif

/* ============================ 公共 ============================ */

char *os_file_read_all(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    long n; char *buf;
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    n = ftell(fp);
    if (n < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); fclose(fp); return NULL; }
    buf[n] = 0;
    fclose(fp);
    if (len) *len = (size_t)n;
    return buf;
}

int os_file_write_atomic(const char *path, const void *data, size_t len)
{
    char tmp[600]; FILE *fp;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    fp = fopen(tmp, "wb");
    if (!fp) return -1;
    if (len && fwrite(data, 1, len, fp) != len) { fclose(fp); remove(tmp); return -1; }
    if (fflush(fp) != 0) { fclose(fp); remove(tmp); return -1; }
    fclose(fp);
    if (os_file_replace(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}
