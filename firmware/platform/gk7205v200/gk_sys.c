/**
 * @file gk_sys.c
 * @brief GK7205V200 系统信息、统计、重启、看门狗；OTA 本期为桩
 *
 * 全部数据取自标准 Linux 接口（/proc、/sys、clock_gettime），不依赖
 * GOKE 私有 SDK。任何解析失败都降级为默认值而非返回错误——在串口
 * shell 修好之前，启动失败无从诊断（spec §8）。
 */
#include "hal/hal.h"
#include "gk_procfs.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
/* 真机目标是 Linux ARM，clock_gettime/CLOCK_MONOTONIC 为标准接口；
   这里仅为了让本单元测试能在 x86/MSVC 上编译运行，与 platform/mock/mock_os.h
   的做法一致——不代表 GK7205V200 会用到 Windows 分支 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#include <sys/reboot.h>
#endif

/* profile 中的 identity.platform，cpuinfo 解析失败时作为回退值 */
static char s_profile_platform[HAL_NAME_MAX] = "gk7205v200";

void gk_sys_set_profile_platform(const char *id)
{
    if (id && id[0]) {
        snprintf(s_profile_platform, sizeof(s_profile_platform), "%s", id);
    }
}

static hal_err_t sys_get_info(hal_sys_info_t *info)
{
    char buf[4096];
    char val[HAL_NAME_MAX];

    if (!info) return HAL_EINVAL;
    memset(info, 0, sizeof(*info));

    snprintf(info->platform_id, sizeof(info->platform_id), "%s", s_profile_platform);
    info->hal_version = HAL_API_VERSION;
    info->cpu_cores = 1;

    /* SoC 名取自 /proc/cpuinfo 的 Hardware；取不到则回退为 platform_id，
       绝不因此让 get_info 失败——控制台首页依赖它 */
    if (gk_read_file("/proc/cpuinfo", buf, sizeof(buf))
        && gk_parse_cpuinfo_hardware(buf, val, sizeof(val))) {
        snprintf(info->soc_name, sizeof(info->soc_name), "%s", val);
    } else {
        snprintf(info->soc_name, sizeof(info->soc_name), "%s", s_profile_platform);
    }

    if (gk_read_file("/proc/meminfo", buf, sizeof(buf))) {
        uint32_t total = 0, avail = 0;
        if (gk_parse_meminfo(buf, &total, &avail)) info->mem_total_kb = total;
    }

    /* 芯片唯一 ID：GK7205V200 无标准 efuse 读取接口，用 eth0 MAC 代替。
       仅用于设备区分，不作安全用途。 */
    if (gk_read_line_value("/sys/class/net/eth0/address", "", val, sizeof(val))
        || gk_read_file("/sys/class/net/eth0/address", val, sizeof(val))) {
        char *p = val, *q = info->chip_id;
        size_t room = sizeof(info->chip_id) - 1;
        while (*p && room > 0) {
            if (*p != ':' && *p != '\n' && *p != '\r') { *q++ = *p; room--; }
            p++;
        }
        *q = '\0';
    }

    return HAL_OK;
}

static hal_err_t sys_get_stats(hal_sys_stats_t *st)
{
    char buf[4096];

    if (!st) return HAL_EINVAL;
    memset(st, 0, sizeof(*st));
    st->temp_milli_c = INT32_MIN;   /* 无温度传感器，契约要求填 INT32_MIN */

    if (gk_read_file("/proc/meminfo", buf, sizeof(buf))) {
        uint32_t total = 0, avail = 0;
        if (gk_parse_meminfo(buf, &total, &avail)) {
            st->mem_total_kb = total;
            st->mem_avail_kb = avail;
            st->mem_free_kb = avail;
        }
    }
    if (gk_read_file("/proc/uptime", buf, sizeof(buf))) {
        uint64_t up = 0;
        if (gk_parse_uptime(buf, &up)) st->uptime_s = up;
    }
    /* CPU 占用率需要两次采样做差，控制台每次刷新独立调用，
       单次调用无法计算——本期留 0，待 console 侧需要时再实现采样缓存 */
    st->cpu_usage_pct = 0;
    return HAL_OK;
}

static hal_err_t sys_get_boot_reason(hal_boot_reason_t *reason)
{
    if (!reason) return HAL_EINVAL;
    /* GK7205V200 无标准的复位原因寄存器导出，无法区分上电/看门狗/软复位 */
    *reason = HAL_BOOT_UNKNOWN;
    return HAL_OK;
}

static uint64_t sys_monotonic_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000ULL) / (uint64_t)freq.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
#endif
}

static hal_err_t sys_set_wallclock(int64_t utc_seconds)
{
#ifdef _WIN32
    (void)utc_seconds;
    return HAL_ENOTSUP;
#else
    struct timespec ts;
    if (utc_seconds < 0) return HAL_EINVAL;
    ts.tv_sec = (time_t)utc_seconds;
    ts.tv_nsec = 0;
    /* clock_settime 失败通常是权限不足（无 CAP_SYS_TIME）；hal_types.h 未定义
       专门的权限错误码，用 HAL_EIO 归入底层调用失败一类，不发明新错误码 */
    if (clock_settime(CLOCK_REALTIME, &ts) != 0) return HAL_EIO;
    return HAL_OK;
#endif
}

static hal_err_t sys_reboot(void)
{
#ifdef _WIN32
    return HAL_ENOTSUP;
#else
    sync();
    reboot(RB_AUTOBOOT);
    return HAL_EIO;   /* 正常不会走到这里 */
#endif
}

static hal_err_t sys_factory_reset(bool keep_network)
{
    (void)keep_network;
    /* 配置与安全存储的清除由 core/config 与 crypto 各自负责，此处只重启。
       本期不实现分区级擦除——那需要碰 flash，超出里程碑范围。 */
    return sys_reboot();
}

/* 看门狗：/dev/watchdog 存在才支持 */
static hal_err_t sys_wdt_enable(uint32_t timeout_s)  { (void)timeout_s; return HAL_ENOTSUP; }
static hal_err_t sys_wdt_feed(void)                  { return HAL_ENOTSUP; }
static hal_err_t sys_wdt_disable(void)               { return HAL_ENOTSUP; }

/* OTA：本期不碰 flash（spec §1.2 非目标） */
static hal_err_t sys_ota_get_state(hal_ota_state_t *st)
{
    if (!st) return HAL_EINVAL;
    memset(st, 0, sizeof(*st));
    st->current_slot = 0;
    st->other_slot = -1;
    snprintf(st->current_version, sizeof(st->current_version), "dev");
    return HAL_OK;
}
static hal_err_t sys_ota_begin(int slot, uint32_t total) { (void)slot; (void)total; return HAL_ENOTSUP; }
static hal_err_t sys_ota_write(const void *d, uint32_t l) { (void)d; (void)l; return HAL_ENOTSUP; }
static hal_err_t sys_ota_end(const uint8_t sha[32])      { (void)sha; return HAL_ENOTSUP; }
static hal_err_t sys_ota_switch(int slot)                { (void)slot; return HAL_ENOTSUP; }
static hal_err_t sys_ota_confirm(void)                   { return HAL_ENOTSUP; }
static hal_err_t sys_ota_abort(void)                     { return HAL_ENOTSUP; }

const hal_sys_ops_t gk_sys_ops = {
    sys_get_info,
    sys_get_stats,
    sys_get_boot_reason,
    sys_monotonic_us,
    sys_set_wallclock,
    sys_reboot,
    sys_factory_reset,
    sys_wdt_enable,
    sys_wdt_feed,
    sys_wdt_disable,
    sys_ota_get_state,
    sys_ota_begin,
    sys_ota_write,
    sys_ota_end,
    sys_ota_switch,
    sys_ota_confirm,
    sys_ota_abort
};
