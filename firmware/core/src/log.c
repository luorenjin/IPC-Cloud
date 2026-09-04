/**
 * @file log.c
 * @brief 日志：分级、模块级别覆盖、内存环形缓冲、脱敏
 */
#include "core/log.h"
#include "core/os.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#define MOD_LEVELS_MAX 32
#define MASK_SLOTS 4

static log_level_t g_level = LOG_INFO;
static struct { char name[24]; log_level_t level; } g_mod_levels[MOD_LEVELS_MAX];
static size_t g_mod_count;

static char  *g_ring;
static size_t g_ring_cap, g_ring_head;   /* head: 下一个写入位置 */
static bool   g_ring_wrapped;

static os_mutex_t *g_mu;
static char g_mask_buf[MASK_SLOTS][40];
static unsigned g_mask_idx;

static const char *level_str(log_level_t l)
{
    switch (l) {
    case LOG_ERROR: return "E";
    case LOG_WARN:  return "W";
    case LOG_INFO:  return "I";
    case LOG_DEBUG: return "D";
    default:        return "T";
    }
}

void log_init(log_level_t level, uint32_t ring_kb)
{
    if (!g_mu) g_mu = os_mutex_create();
    g_level = level;
    if (ring_kb) {
        free(g_ring);
        g_ring_cap = (size_t)ring_kb * 1024;
        g_ring = (char *)malloc(g_ring_cap);
        g_ring_head = 0; g_ring_wrapped = false;
    }
}

void log_set_level(log_level_t level) { g_level = level; }

void log_set_module_level(const char *module, log_level_t level)
{
    if (!module) return;
    if (!g_mu) g_mu = os_mutex_create();
    os_mutex_lock(g_mu);
    for (size_t i = 0; i < g_mod_count; i++) {
        if (strcmp(g_mod_levels[i].name, module) == 0) { g_mod_levels[i].level = level; os_mutex_unlock(g_mu); return; }
    }
    if (g_mod_count < MOD_LEVELS_MAX) {
        strncpy(g_mod_levels[g_mod_count].name, module, sizeof(g_mod_levels[0].name) - 1);
        g_mod_levels[g_mod_count].level = level;
        g_mod_count++;
    }
    os_mutex_unlock(g_mu);
}

static log_level_t effective_level(const char *module)
{
    if (module) {
        for (size_t i = 0; i < g_mod_count; i++) {
            if (strcmp(g_mod_levels[i].name, module) == 0) return g_mod_levels[i].level;
        }
    }
    return g_level;
}

static void ring_append(const char *s, size_t n)
{
    if (!g_ring || !n) return;
    if (n >= g_ring_cap) { s += n - g_ring_cap; n = g_ring_cap; }
    if (g_ring_head + n <= g_ring_cap) {
        memcpy(g_ring + g_ring_head, s, n);
        g_ring_head += n;
        if (g_ring_head == g_ring_cap) { g_ring_head = 0; g_ring_wrapped = true; }
    } else {
        size_t first = g_ring_cap - g_ring_head;
        memcpy(g_ring + g_ring_head, s, first);
        memcpy(g_ring, s + first, n - first);
        g_ring_head = n - first;
        g_ring_wrapped = true;
    }
}

void log_write(log_level_t level, const char *module, const char *file, int line, const char *fmt, ...)
{
    char line_buf[512];
    int n;
    va_list ap;
    const char *base;

    if (level > effective_level(module)) return;
    if (!g_mu) g_mu = os_mutex_create();

    base = file ? strrchr(file, '/') : NULL;
    if (!base && file) base = strrchr(file, '\\');
    base = base ? base + 1 : (file ? file : "?");

    n = snprintf(line_buf, sizeof(line_buf), "%llu %s [%s] %s:%d ",
                 (unsigned long long)(os_monotonic_us() / 1000ULL), level_str(level), module ? module : "-", base, line);
    if (n < 0) return;
    if ((size_t)n < sizeof(line_buf) - 2) {
        va_start(ap, fmt);
        int m = vsnprintf(line_buf + n, sizeof(line_buf) - (size_t)n - 1, fmt, ap);
        va_end(ap);
        if (m > 0) n += (m < (int)(sizeof(line_buf) - (size_t)n - 1)) ? m : (int)(sizeof(line_buf) - (size_t)n - 2);
    }
    line_buf[n++] = '\n';
    line_buf[n] = 0;

    os_mutex_lock(g_mu);
    fputs(line_buf, stderr);
    ring_append(line_buf, (size_t)n);
    os_mutex_unlock(g_mu);
}

size_t log_ring_dump(char *buf, size_t cap)
{
    size_t total, n;
    if (!buf || !cap || !g_ring) return 0;
    os_mutex_lock(g_mu);
    total = g_ring_wrapped ? g_ring_cap : g_ring_head;
    n = total < cap - 1 ? total : cap - 1;
    if (!g_ring_wrapped) {
        memcpy(buf, g_ring + (g_ring_head - n), n);
    } else {
        /* 从最旧数据（head）开始的 total 字节中取最后 n 字节 */
        size_t skip = total - n;
        size_t start = (g_ring_head + skip) % g_ring_cap;
        size_t first = g_ring_cap - start;
        if (first >= n) memcpy(buf, g_ring + start, n);
        else { memcpy(buf, g_ring + start, first); memcpy(buf + first, g_ring, n - first); }
    }
    buf[n] = 0;
    os_mutex_unlock(g_mu);
    return n;
}

const char *log_mask(const char *secret)
{
    char *out;
    size_t len;
    if (!g_mu) g_mu = os_mutex_create();
    os_mutex_lock(g_mu);
    out = g_mask_buf[g_mask_idx++ % MASK_SLOTS];
    os_mutex_unlock(g_mu);
    if (!secret) { strcpy(out, "(null)"); return out; }
    len = strlen(secret);
    if (len <= 4) { strcpy(out, "****"); return out; }
    {
        size_t stars = len - 4; if (stars > 32) stars = 32;
        out[0] = secret[0]; out[1] = secret[1];
        memset(out + 2, '*', stars);
        out[2 + stars] = secret[len - 2]; out[3 + stars] = secret[len - 1]; out[4 + stars] = 0;
    }
    return out;
}
