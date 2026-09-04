/**
 * @file mock_os.h
 * @brief mock 平台内部：跨 Windows/Linux 的最小 OS 封装（仅 mock 使用）
 */
#ifndef IPC_MOCK_OS_H
#define IPC_MOCK_OS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static inline uint64_t mock_os_monotonic_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000ULL) / (uint64_t)freq.QuadPart);
}
static inline void mock_os_sleep_ms(uint32_t ms) { Sleep(ms); }
#else
#  include <time.h>
#  include <unistd.h>
static inline uint64_t mock_os_monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
static inline void mock_os_sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

#endif /* IPC_MOCK_OS_H */
