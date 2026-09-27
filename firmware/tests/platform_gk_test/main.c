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

static void test_parse_cpu_stat(void)
{
    uint64_t busy = 0, total = 0;

    SECTION("parse_cpu_stat");
    CHECK(gk_parse_cpu_stat("cpu  100 20 30 800 50 0 0 0 0 0\ncpu0 1 2 3 4\n", &busy, &total),
          "解析应成功");
    CHECK(total == 1000, "total=user+nice+system+idle+iowait+... 期望 1000，实际 %llu",
          (unsigned long long)total);
    CHECK(busy == 150, "busy=total-idle-iowait 期望 150，实际 %llu", (unsigned long long)busy);
    CHECK(!gk_parse_cpu_stat("intr 1 2 3\n", &busy, &total), "缺 cpu 行应失败");
    CHECK(!gk_parse_cpu_stat("cpu  \n", &busy, &total), "空字段应失败");
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

#include "hal/hal_sys.h"
extern const hal_sys_ops_t gk_sys_ops;
void gk_sys_set_profile_platform(const char *id);

static void test_sys_ops(void)
{
    hal_sys_info_t info;
    hal_sys_stats_t st;
    uint64_t t1, t2;

    SECTION("sys_ops");

    /* 单调时钟必须真的单调递增 */
    t1 = gk_sys_ops.monotonic_us();
    t2 = gk_sys_ops.monotonic_us();
    CHECK(t1 > 0, "单调时钟不应为 0");
    CHECK(t2 >= t1, "单调时钟必须非递减");

    /* get_info 在任何平台都不得失败——解析不到就回退，绝不返回错误 */
    gk_sys_set_profile_platform("gk7205v200");
    memset(&info, 0, sizeof(info));
    CHECK(gk_sys_ops.get_info(&info) == HAL_OK, "get_info 不得失败");
    CHECK(info.platform_id[0] != '\0', "platform_id 不得为空");
    CHECK(strcmp(info.platform_id, "gk7205v200") == 0,
          "platform_id 应为注入值，实际 [%s]", info.platform_id);
    CHECK(info.soc_name[0] != '\0', "soc_name 不得为空（解析失败时应回退）");
    CHECK(info.hal_version == HAL_API_VERSION, "hal_version 应为 HAL_API_VERSION");

    /* get_stats 同样不得因为 x86 上没有 /proc/meminfo 而失败 */
    memset(&st, 0, sizeof(st));
    CHECK(gk_sys_ops.get_stats(&st) == HAL_OK, "get_stats 不得失败");

    CHECK(gk_sys_ops.get_info(NULL) == HAL_EINVAL, "NULL 应返回 EINVAL");
    CHECK(gk_sys_ops.get_stats(NULL) == HAL_EINVAL, "NULL 应返回 EINVAL");

    /* OTA 本期不支持，必须是 ENOTSUP 而非崩溃 */
    CHECK(gk_sys_ops.ota_begin(0, 1024) == HAL_ENOTSUP, "ota_begin 应为 ENOTSUP");
    CHECK(gk_sys_ops.ota_confirm() == HAL_ENOTSUP, "ota_confirm 应为 ENOTSUP");
}

#include "hal/hal_net.h"
extern const hal_net_ops_t gk_net_ops;

static void test_net_ops(void)
{
    hal_net_caps_t caps;
    hal_netif_status_t st;
    hal_wifi_ap_t aps[4];
    uint32_t n = 0;

    SECTION("net_ops");

    memset(&caps, 0, sizeof(caps));
    CHECK(gk_net_ops.get_caps(&caps) == HAL_OK, "get_caps 不得失败");
    CHECK(caps.eth == true, "以太网应为 true");
    /* WiFi 模块型号未冻结（决策记录 §2.3），本期一律报不支持 */
    CHECK(caps.wifi == false, "本期 WiFi 应为 false");
    CHECK(caps.wifi_ap == false, "本期 AP 应为 false");

    /* 以太网状态：x86 上没有 eth0，必须返回 HAL_OK 且 ip 为空串，
       而不是失败——console_net.c 依赖非失败路径 */
    memset(&st, 0, sizeof(st));
    CHECK(gk_net_ops.get_status(HAL_NETIF_ETH, &st) == HAL_OK,
          "get_status 在无网卡时也不得失败");
    CHECK(st.type == HAL_NETIF_ETH, "type 应回填");
    CHECK(st.ifname[0] != '\0', "ifname 不得为空");

    /* WiFi 查询必须是 ENOTSUP */
    CHECK(gk_net_ops.get_status(HAL_NETIF_WIFI, &st) == HAL_ENOTSUP,
          "WiFi 状态应为 ENOTSUP");
    CHECK(gk_net_ops.wifi_scan(aps, 4, &n, 1000) == HAL_ENOTSUP, "wifi_scan 应为 ENOTSUP");
    CHECK(gk_net_ops.wifi_connect("ssid", "password", HAL_WIFI_SEC_WPA2) == HAL_ENOTSUP,
          "wifi_connect 应为 ENOTSUP");

    CHECK(gk_net_ops.get_caps(NULL) == HAL_EINVAL, "NULL 应返回 EINVAL");
    CHECK(gk_net_ops.get_status(HAL_NETIF_ETH, NULL) == HAL_EINVAL, "NULL 应返回 EINVAL");

    /* 无事件时必须是 EAGAIN，否则 console 的轮询会误判为故障 */
    hal_net_event_t evt;
    CHECK(gk_net_ops.poll_event(&evt, 0) == HAL_EAGAIN, "无事件应为 EAGAIN");
}

#include "hal/hal_storage.h"
extern const hal_storage_ops_t gk_storage_ops;
bool gk_find_mmc_mount(const char *mounts_content, char *dev, size_t dev_cap,
                       char *path, size_t path_cap, char *fstype, size_t fs_cap);

static void test_storage_ops(void)
{
    hal_storage_stat_t st;
    char dev[64], path[128], fs[32];

    SECTION("storage_ops");

    /* 典型的已插卡 /proc/mounts */
    const char *with_card =
        "rootfs / rootfs rw 0 0\n"
        "proc /proc proc rw,relatime 0 0\n"
        "/dev/mmcblk0p1 /mnt/sd vfat rw,relatime 0 0\n";
    CHECK(gk_find_mmc_mount(with_card, dev, sizeof(dev), path, sizeof(path), fs, sizeof(fs)),
          "应找到 mmc 挂载");
    CHECK(strcmp(path, "/mnt/sd") == 0, "挂载点实际为 [%s]", path);
    CHECK(strcmp(fs, "vfat") == 0, "文件系统实际为 [%s]", fs);

    /* 无卡：必须找不到，且不得误匹配 mmcblk 以外的设备 */
    const char *no_card =
        "rootfs / rootfs rw 0 0\n"
        "/dev/mtdblock3 / jffs2 rw,relatime 0 0\n";
    CHECK(!gk_find_mmc_mount(no_card, dev, sizeof(dev), path, sizeof(path), fs, sizeof(fs)),
          "无卡时不应匹配");

    /* stat 在无卡时必须返回 HAL_OK 且 present=false，不是错误 */
    memset(&st, 0, sizeof(st));
    CHECK(gk_storage_ops.stat(&st) == HAL_OK, "无卡时 stat 仍应返回 OK");
    CHECK(st.present == false, "x86 上无 TF 卡，present 应为 false");

    CHECK(gk_storage_ops.stat(NULL) == HAL_EINVAL, "NULL 应返回 EINVAL");
    CHECK(gk_storage_ops.format(HAL_FS_FAT32) == HAL_ENOTSUP, "format 本期应为 ENOTSUP");

    hal_storage_event_t evt;
    CHECK(gk_storage_ops.poll_event(&evt, 0) == HAL_EAGAIN, "无事件应为 EAGAIN");
}

#include "hal/hal_crypto.h"
extern const hal_crypto_ops_t gk_crypto_ops;

static void test_crypto_ops(void)
{
    uint8_t buf[64], r1[32], r2[32];
    size_t len = 0;
    bool exists = false;

    SECTION("crypto_ops");

    /* 测试期间把存储根目录指向当前目录，避免写 /etc */
#ifdef _WIN32
    _putenv_s("IPC_SEC_DIR", "gk_test_sec");
#else
    setenv("IPC_SEC_DIR", "gk_test_sec", 1);
#endif

    /* 写入后应能原样读回 */
    const uint8_t data[] = { 0x01, 0x02, 0x03, 0x04 };
    CHECK(gk_crypto_ops.secure_write("test_key", data, sizeof(data)) == HAL_OK, "写入应成功");
    CHECK(gk_crypto_ops.secure_exists("test_key", &exists) == HAL_OK && exists,
          "写入后 exists 应为 true");
    memset(buf, 0, sizeof(buf));
    CHECK(gk_crypto_ops.secure_read("test_key", buf, sizeof(buf), &len) == HAL_OK, "读取应成功");
    CHECK(len == sizeof(data) && memcmp(buf, data, sizeof(data)) == 0, "内容应一致");

    /* 覆盖写必须完整替换，不得残留旧内容尾部 */
    const uint8_t shorter[] = { 0xAA };
    CHECK(gk_crypto_ops.secure_write("test_key", shorter, 1) == HAL_OK, "覆盖写应成功");
    CHECK(gk_crypto_ops.secure_read("test_key", buf, sizeof(buf), &len) == HAL_OK, "再次读取");
    CHECK(len == 1 && buf[0] == 0xAA, "覆盖写后长度应为 1，实际 %u", (unsigned)len);

    CHECK(gk_crypto_ops.secure_delete("test_key") == HAL_OK, "删除应成功");
    CHECK(gk_crypto_ops.secure_exists("test_key", &exists) == HAL_OK && !exists,
          "删除后 exists 应为 false");
    CHECK(gk_crypto_ops.secure_read("test_key", buf, sizeof(buf), &len) == HAL_ENODEV,
          "读取不存在的键应为 ENODEV");

    /* 私钥不可读（hal_crypto.h 契约，hal_conformance 也会校验） */
    CHECK(gk_crypto_ops.secure_read(HAL_SEC_KEY_DEVICE_KEY, buf, sizeof(buf), &len) == HAL_ENOTSUP,
          "设备私钥必须不可读");

    /* 路径穿越必须被拒绝，否则可写任意文件 */
    CHECK(gk_crypto_ops.secure_write("../escape", data, sizeof(data)) == HAL_EINVAL,
          "路径穿越应被拒绝");
    CHECK(gk_crypto_ops.secure_write("a/b", data, sizeof(data)) == HAL_EINVAL,
          "含斜杠的键应被拒绝");

    /* 随机数必须不同——鉴权挑战值的不可预测性依赖它 */
    CHECK(gk_crypto_ops.random(r1, sizeof(r1)) == HAL_OK, "random 应成功");
    CHECK(gk_crypto_ops.random(r2, sizeof(r2)) == HAL_OK, "random 应成功");
    CHECK(memcmp(r1, r2, sizeof(r1)) != 0, "两次随机数不应相同");

    CHECK(gk_crypto_ops.secure_write(NULL, data, 1) == HAL_EINVAL, "NULL 键应为 EINVAL");
    CHECK(gk_crypto_ops.random(NULL, 8) == HAL_EINVAL, "NULL 缓冲应为 EINVAL");

    /* 超长键名：字符集合合法（无 / \ ..），但拼出的路径会超过内部 256 字节
       缓冲区。sec_path 必须检测 snprintf 截断并拒绝，否则两个仅尾部不同的
       超长键会被截断成同一路径，造成键混淆/互相覆盖。 */
    {
        char long_key[300];
        memset(long_key, 'a', sizeof(long_key) - 1);
        long_key[sizeof(long_key) - 1] = '\0';
        CHECK(gk_crypto_ops.secure_write(long_key, data, sizeof(data)) == HAL_EINVAL,
              "超长键名应被拒绝（防止 snprintf 截断导致键混淆）");
    }
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
    test_parse_cpu_stat();
    test_parse_cpuinfo();
    test_sys_ops();
    test_net_ops();
    test_storage_ops();
    test_crypto_ops();
    printf("RESULT: platform_gk pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
