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
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#include <sys/stat.h>
#endif

/* 平台标识常量，用于 platform_id/soc_name/chip_id 的回退值。曾经由
   gk_platform.c 在 init 时从 profile 的 identity.platform 注入，但那样
   会让配错的 profile（如忘了传 -DIPC_PROFILE 而落到默认 mock-x86.json）
   污染设备身份（Ruling 9）——真机会把自己上报成 "mock"。现在
   gk_platform.c 只对 profile 做一致性校验（不符就告警、不采用），
   platform_id 恒为本平台自身值，不再被外部注入。
   gk_sys_set_profile_platform 仍保留（Task 2 声明的产物接口），供
   tests/platform_gk_test/main.c 在不经过 gk_platform.c 的情况下单独
   测试 gk_sys_ops；生产路径不再调用它。 */
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
    if (info->mem_total_kb == 0) {
        /* /proc/meminfo 解析失败时的占位值（板子实际 32MB），真机正常情况下
           会被上面解析出的真实值覆盖；HAL 契约要求 mem_total_kb 非零
           （hal_conformance HAL-02），不得让 get_info 因此失败。
           不用 core/log（分层约束，platform/ 不得反向依赖 core/，见
           gk_platform.c 的说明），改用 fprintf(stderr, ...)，串口上一样
           看得见，与 platform/mock/mock_platform.c 的既有做法一致——
           "失败不阻断启动"不等于"失败不留痕"。 */
        fprintf(stderr, "[gk7205v200] 警告：/proc/meminfo 解析失败，mem_total_kb 使用占位值 32768\n");
        info->mem_total_kb = 32768;
    }

    /* 芯片唯一 ID：GK7205V200 无标准 efuse 读取接口，用 eth0 MAC 代替。
       仅用于设备区分，不作安全用途。 */
    if (gk_read_file("/sys/class/net/eth0/address", val, sizeof(val))) {
        char *p = val, *q = info->chip_id;
        size_t room = sizeof(info->chip_id) - 1;
        while (*p && room > 0) {
            if (*p != ':' && *p != '\n' && *p != '\r') { *q++ = *p; room--; }
            p++;
        }
        *q = '\0';
    }
    if (info->chip_id[0] == '\0') {
        /* 读不到 MAC（如本机 Windows/MSVC 调试环境）时回退为 platform_id 加
           固定后缀，保证 chip_id 非空——HAL 契约要求（hal_conformance HAL-02） */
        fprintf(stderr, "[gk7205v200] 警告：读取 eth0 MAC 失败，chip_id 使用占位值\n");
        snprintf(info->chip_id, sizeof(info->chip_id), "%s-unknown", s_profile_platform);
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
    if (st->mem_total_kb == 0) {
        /* 同 sys_get_info：解析失败时的占位值，真机正常情况下会被真实值覆盖；
           HAL 契约要求 mem_total_kb 非零（hal_conformance HAL-02）。
           mem_avail_kb/mem_free_kb 同步给一个非零占位（total 的一半），
           而不是留 0——留 0 会让 console 状态页把"数据缺失"误显示成
           "内存 100% 占用"，比显示一个粗略估计值更误导。 */
        fprintf(stderr, "[gk7205v200] 警告：/proc/meminfo 解析失败，mem_total/avail/free_kb 使用占位值\n");
        st->mem_total_kb = 32768;
        st->mem_avail_kb = 32768 / 2;
        st->mem_free_kb = 32768 / 2;
    }
    if (gk_read_file("/proc/uptime", buf, sizeof(buf))) {
        uint64_t up = 0;
        if (gk_parse_uptime(buf, &up)) st->uptime_s = up;
    }
    /* 与上一次调用的 /proc/stat 做差；首次调用没有基线，返回 0 */
    if (gk_read_file("/proc/stat", buf, sizeof(buf))) {
        static uint64_t s_prev_busy, s_prev_total;
        uint64_t busy = 0, total = 0;
        if (gk_parse_cpu_stat(buf, &busy, &total)) {
            if (s_prev_total && total > s_prev_total && busy >= s_prev_busy)
                st->cpu_usage_pct = (uint32_t)((busy - s_prev_busy) * 100 / (total - s_prev_total));
            s_prev_busy = busy;
            s_prev_total = total;
        }
    }
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

#define GK_NET_CONF   "/etc/ipc/net.conf"
#define GK_NET_SCRIPT "/etc/init.d/S81dhcp"

static hal_err_t sys_factory_reset(bool keep_network)
{
    /* 配置与凭据由 core/config、console 各自清除；平台侧只需清掉自己落的
       网络文件，否则 cfg 已回到 DHCP 而 S81dhcp 仍按旧静态地址起网。
       不做分区级擦除（超出里程碑范围）。 */
#ifndef _WIN32
    if (!keep_network) remove(GK_NET_CONF);
#else
    (void)keep_network;
#endif
    return sys_reboot();
}

/* 看门狗：GK7205V200 的 /dev/watchdog 尚未在真机上验证（硬件驱动是否就绪、
   设备节点路径均未确认），本期不提供看门狗能力，恒返回 HAL_ENOTSUP。
   （Ruling 8）此前一度改成软件状态机让 wdt_enable/feed/disable 恒报告
   HAL_OK 以凑 hal_conformance HAL-02 的断言，但 core/include/core/module.h
   明确"模块置 FAILED 应触发看门狗"，Task 7 的主循环会据此调用
   module_health_check——一个报告成功却从不复位设备的假看门狗，会让系统
   误以为具备挂死恢复能力而实际没有，真出现挂死时反而永久失联。这与拒绝为
   gk_sign 提供伪签名是同一条原则："伪造的能力会让上层误以为已具备该能力"。
   hal_conformance 的 HAL-02 由此会新增 4 条已知失败（wdt_enable(0)->EINVAL
   的参数校验、wdt_enable/feed/disable 的正常路径），已由控制者裁决为已知、
   可接受的失败，不在本里程碑门禁范围内（见 Ruling 7）。真正接入
   /dev/watchdog 留给硬件驱动就绪后的后续里程碑。 */
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
    snprintf(st->current_version, sizeof(st->current_version), "%s", IPC_FW_VERSION);
    return HAL_OK;
}
static hal_err_t sys_ota_begin(int slot, uint32_t total) { (void)slot; (void)total; return HAL_ENOTSUP; }
static hal_err_t sys_ota_write(const void *d, uint32_t l) { (void)d; (void)l; return HAL_ENOTSUP; }
static hal_err_t sys_ota_end(const uint8_t sha[32])      { (void)sha; return HAL_ENOTSUP; }
static hal_err_t sys_ota_switch(int slot)                { (void)slot; return HAL_ENOTSUP; }
static hal_err_t sys_ota_confirm(void)                   { return HAL_ENOTSUP; }
static hal_err_t sys_ota_abort(void)                     { return HAL_ENOTSUP; }

/* ---- 网络 / NTP / 墙钟：交给 rootfs 里的开机脚本与 busybox 工具 ---- */

/** 只允许点分十进制字符，防止写进 shell 可解析的配置文件里被注入 */
static bool ipv4_chars_ok(const char *s)
{
    for (; s && *s; s++) if (!((*s >= '0' && *s <= '9') || *s == '.')) return false;
    return true;
}

/** DNS 允许 "首选,备用"：在点分十进制基础上多放一个逗号（同样防注入） */
static bool dns_chars_ok(const char *s)
{
    for (; s && *s; s++) if (!((*s >= '0' && *s <= '9') || *s == '.' || *s == ',')) return false;
    return true;
}

static hal_err_t sys_apply_net(const char *ip, const char *mask, const char *gw, const char *dns, int mtu)
{
#ifdef _WIN32
    (void)ip; (void)mask; (void)gw; (void)dns; (void)mtu;
    return HAL_ENOTSUP;
#else
    const char *tmp = GK_NET_CONF ".tmp";
    bool dhcp = !ip || !ip[0];
    FILE *fp;

    if (!ipv4_chars_ok(ip) || !ipv4_chars_ok(mask) || !ipv4_chars_ok(gw) || !dns_chars_ok(dns))
        return HAL_EINVAL;
    if (mtu != 0 && (mtu < 576 || mtu > 1500))
        return HAL_EINVAL;                     /* 与 core/config.c 的 net.mtu 规则同区间 */
    fp = fopen(tmp, "w");
    if (!fp) return HAL_EIO;
    if (dhcp) fprintf(fp, "MODE=dhcp\n");
    else fprintf(fp, "MODE=static\nIP=%s\nMASK=%s\nGW=%s\nDNS=%s\n", ip, mask ? mask : "",
                 gw ? gw : "", dns ? dns : "");
    /* MTU 两种模式都要生效：S81dhcp 读它设置 ifconfig eth0 mtu；0 = 不写，保持默认 */
    if (mtu > 0) fprintf(fp, "MTU=%d\n", mtu);
    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) { fclose(fp); remove(tmp); return HAL_EIO; }
    fclose(fp);
    if (rename(tmp, GK_NET_CONF) != 0) { remove(tmp); return HAL_EIO; }
    /* 后台执行：脚本会杀掉 udhcpc、重配网卡，不能阻塞 HTTP 事件循环 */
    if (system(GK_NET_SCRIPT " restart >/dev/null 2>&1 &") != 0) return HAL_EIO;
    return HAL_OK;
#endif
}

static bool host_chars_ok(const char *s)
{
    for (; s && *s; s++) {
        char c = *s;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '.' || c == '-')) return false;
    }
    return true;
}

/* ntpd 每次校时成功后执行 -S 指定的程序；用它落一个标记文件，
   控制台据此如实显示"已同步/同步中"，而不是一启动就宣称生效 */
#define GK_NTP_MARK "/tmp/ntp_synced"
#define GK_NTP_HOOK "/tmp/ntp_hook.sh"

static hal_err_t sys_apply_ntp(const char *server)
{
#ifdef _WIN32
    (void)server;
    return HAL_ENOTSUP;
#else
    char cmd[256];
    FILE *fp;

    if (server && strlen(server) > 127) return HAL_EINVAL;
    if (!host_chars_ok(server)) return HAL_EINVAL;
    if (system("killall ntpd >/dev/null 2>&1") < 0) return HAL_EIO;
    remove(GK_NTP_MARK);
    if (!server || !server[0]) return HAL_OK;
    fp = fopen(GK_NTP_HOOK, "w");
    if (!fp) return HAL_EIO;
    fprintf(fp, "#!/bin/sh\n[ \"$1\" = step ] || [ \"$1\" = stratum ] || [ \"$1\" = periodic ] && touch " GK_NTP_MARK "\n");
    fclose(fp);
    chmod(GK_NTP_HOOK, 0700);
    /* busybox ntpd：-p 指定服务器，-S 校时后回调，默认后台常驻持续校时 */
    snprintf(cmd, sizeof(cmd), "ntpd -p %s -S " GK_NTP_HOOK " >/dev/null 2>&1", server);
    if (system(cmd) != 0) return HAL_EIO;
    return HAL_OK;
#endif
}

static bool sys_ntp_synced(void)
{
#ifdef _WIN32
    return false;
#else
    return access(GK_NTP_MARK, F_OK) == 0;
#endif
}

static hal_err_t sys_get_wallclock(int64_t *utc)
{
    if (!utc) return HAL_EINVAL;
    *utc = (int64_t)time(NULL);
    return HAL_OK;
}

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
    sys_ota_abort,
    sys_apply_net,
    sys_apply_ntp,
    sys_get_wallclock,
    sys_ntp_synced
};
