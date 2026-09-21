/**
 * @file main.c
 * @brief gk7205v200 平台单元测试（x86 上测纯解析逻辑）
 *
 * 真实硬件相关的 ops 无法在 x86 上测，本测试只覆盖 /proc 文本解析与
 * 安全存储的文件操作——这两块恰好是出错后最难在板子上诊断的部分。
 * 退出码 = 失败数。
 */
#include "platform/gk7205v200/gk_procfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SECTION(name) printf("== %s\n", name)

static void test_parse_meminfo(void)
{
    uint32_t total = 0, avail = 0;
    const char *sample =
        "MemTotal:          23164 kB\n"
        "MemFree:            8192 kB\n"
        "MemAvailable:      15000 kB\n"
        "Buffers:             512 kB\n";

    SECTION("parse_meminfo");
    CHECK(gk_parse_meminfo(sample, &total, &avail), "解析应成功");
    CHECK(total == 23164, "MemTotal 期望 23164，实际 %u", total);
    CHECK(avail == 15000, "MemAvailable 期望 15000，实际 %u", avail);

    /* 缺少 MemAvailable（老内核）时应回退到 MemFree，而不是失败 */
    const char *no_avail =
        "MemTotal:          23164 kB\n"
        "MemFree:            8192 kB\n";
    total = avail = 0;
    CHECK(gk_parse_meminfo(no_avail, &total, &avail), "缺 MemAvailable 时应回退成功");
    CHECK(avail == 8192, "回退到 MemFree，期望 8192，实际 %u", avail);

    CHECK(!gk_parse_meminfo("garbage\n", &total, &avail), "无效内容应失败");
    CHECK(!gk_parse_meminfo(NULL, &total, &avail), "NULL 应失败");
}

static void test_parse_uptime(void)
{
    uint64_t up = 0;

    SECTION("parse_uptime");
    CHECK(gk_parse_uptime("12345.67 98765.43\n", &up), "解析应成功");
    CHECK(up == 12345, "期望 12345，实际 %llu", (unsigned long long)up);
    CHECK(gk_parse_uptime("0.00 0.00\n", &up) && up == 0, "零值");
    CHECK(!gk_parse_uptime("abc\n", &up), "无效内容应失败");
}

static void test_parse_cpuinfo(void)
{
    char hw[64];
    const char *sample =
        "processor\t: 0\n"
        "model name\t: ARMv7 Processor rev 5 (v7l)\n"
        "BogoMIPS\t: 100.00\n"
        "Hardware\t: Goke GK7205V200 DEMO Board\n"
        "Revision\t: 0000\n";

    SECTION("parse_cpuinfo_hardware");
    CHECK(gk_parse_cpuinfo_hardware(sample, hw, sizeof(hw)), "解析应成功");
    CHECK(strcmp(hw, "Goke GK7205V200 DEMO Board") == 0, "Hardware 实际为 [%s]", hw);

    /* 无 Hardware 字段时必须失败，由调用方回退到 profile 值 */
    CHECK(!gk_parse_cpuinfo_hardware("processor\t: 0\n", hw, sizeof(hw)), "无 Hardware 应失败");

    /* 缓冲区过小不得溢出 */
    char tiny[8];
    gk_parse_cpuinfo_hardware(sample, tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny), "小缓冲区不得溢出");
}

static void test_read_file(void)
{
    char buf[128];
    const char *tmp = "gk_test_tmp.txt";
    FILE *fp = fopen(tmp, "wb");

    SECTION("read_file");
    if (fp) { fputs("hello\n", fp); fclose(fp); }
    CHECK(gk_read_file(tmp, buf, sizeof(buf)), "读取应成功");
    CHECK(strcmp(buf, "hello\n") == 0, "内容实际为 [%s]", buf);
    CHECK(!gk_read_file("gk_no_such_file_xyz", buf, sizeof(buf)), "不存在的文件应失败");
    remove(tmp);
}

static void test_read_line_value(void)
{
    char val[64];
    const char *tmp = "gk_test_kv.txt";
    FILE *fp = fopen(tmp, "wb");

    SECTION("read_line_value");
    if (fp) { fputs("Hardware\t: Goke GK7205V200\nRevision : 0000\n", fp); fclose(fp); }
    CHECK(gk_read_line_value(tmp, "Hardware", val, sizeof(val)), "应找到 Hardware");
    CHECK(strcmp(val, "Goke GK7205V200") == 0, "值实际为 [%s]", val);
    CHECK(!gk_read_line_value(tmp, "NoSuchKey", val, sizeof(val)), "不存在的键应失败");
    remove(tmp);
}

static void test_read_u64(void)
{
    uint64_t v = 0;
    const char *tmp = "gk_test_num.txt";
    FILE *fp = fopen(tmp, "wb");

    SECTION("read_u64");
    if (fp) { fputs("100\n", fp); fclose(fp); }
    CHECK(gk_read_u64(tmp, &v) && v == 100, "期望 100，实际 %llu", (unsigned long long)v);
    remove(tmp);

    /* 网线未插时 sysfs 的 speed 会是 "-1"，必须当作失败而非当成巨大的无符号数 */
    fp = fopen(tmp, "wb");
    if (fp) { fputs("-1\n", fp); fclose(fp); }
    CHECK(!gk_read_u64(tmp, &v), "负数应失败（网线未插时 speed 为 -1）");
    remove(tmp);
}

int main(void)
{
    test_read_file();
    test_read_line_value();
    test_read_u64();
    test_parse_meminfo();
    test_parse_uptime();
    test_parse_cpuinfo();
    printf("RESULT: platform_gk pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
