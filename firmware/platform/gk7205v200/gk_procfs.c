/**
 * @file gk_procfs.c
 * @brief /proc 与 /sys 文本解析实现
 */
#include "gk_procfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

bool gk_read_file(const char *path, char *buf, size_t cap)
{
    FILE *fp;
    size_t n;

    if (!path || !buf || cap == 0) return false;
    fp = fopen(path, "rb");
    if (!fp) return false;
    n = fread(buf, 1, cap - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return n > 0;
}

/* 去除首尾空白（原地） */
static void trim(char *s)
{
    char *p = s;
    size_t len;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

/** 在已读入的内容中按 key 找值，分隔符为 ':' 或 '='。 */
static bool find_value(const char *content, const char *key, char *out, size_t cap)
{
    const char *line = content;
    size_t klen = strlen(key);

    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t linelen = eol ? (size_t)(eol - line) : strlen(line);

        /* 行首必须匹配 key，且其后只能是空白或分隔符——避免 "MemFree" 命中 "Mem" */
        if (linelen > klen && strncmp(line, key, klen) == 0) {
            const char *p = line + klen;
            while (p < line + linelen && isspace((unsigned char)*p)) p++;
            if (p < line + linelen && (*p == ':' || *p == '=')) {
                size_t vlen;
                p++;
                vlen = (size_t)(line + linelen - p);
                if (vlen >= cap) vlen = cap - 1;
                memcpy(out, p, vlen);
                out[vlen] = '\0';
                trim(out);
                return out[0] != '\0';
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    return false;
}

bool gk_read_line_value(const char *path, const char *key, char *out, size_t cap)
{
    char buf[4096];
    if (!key || !out || cap == 0) return false;
    if (!gk_read_file(path, buf, sizeof(buf))) return false;
    return find_value(buf, key, out, cap);
}

bool gk_read_u64(const char *path, uint64_t *out)
{
    char buf[64];
    char *end;
    unsigned long long v;

    if (!out) return false;
    if (!gk_read_file(path, buf, sizeof(buf))) return false;
    trim(buf);
    /* sysfs 的 speed 在网线未插时为 "-1"，必须拒绝而非回绕成巨大的无符号数 */
    if (buf[0] == '-') return false;
    v = strtoull(buf, &end, 10);
    if (end == buf) return false;
    *out = (uint64_t)v;
    return true;
}

bool gk_parse_meminfo(const char *content, uint32_t *total_kb, uint32_t *avail_kb)
{
    char val[64];

    if (!content || !total_kb || !avail_kb) return false;
    if (!find_value(content, "MemTotal", val, sizeof(val))) return false;
    *total_kb = (uint32_t)strtoul(val, NULL, 10);

    /* Linux 3.14 起才有 MemAvailable；缺失时回退到 MemFree，不视为失败 */
    if (find_value(content, "MemAvailable", val, sizeof(val))) {
        *avail_kb = (uint32_t)strtoul(val, NULL, 10);
    } else if (find_value(content, "MemFree", val, sizeof(val))) {
        *avail_kb = (uint32_t)strtoul(val, NULL, 10);
    } else {
        *avail_kb = 0;
    }
    return *total_kb > 0;
}

bool gk_parse_uptime(const char *content, uint64_t *uptime_s)
{
    char *end;
    double v;

    if (!content || !uptime_s) return false;
    v = strtod(content, &end);
    if (end == content || v < 0) return false;
    *uptime_s = (uint64_t)v;
    return true;
}

bool gk_parse_cpuinfo_hardware(const char *content, char *out, size_t cap)
{
    if (!content || !out || cap == 0) return false;
    out[0] = '\0';
    return find_value(content, "Hardware", out, cap);
}
