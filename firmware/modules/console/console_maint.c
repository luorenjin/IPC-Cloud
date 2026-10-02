/**
 * @file console_maint.c
 * @brief console 维护子模块：网络诊断（Ping/Tracert）+ 定时重启 + NTP 重试 + HTTP 端口切换
 *
 * 这些事的共同点：都不能放在 http_server 的事件循环线程里跑。
 *  - ping / traceroute 是阻塞子进程调用；
 *  - 定时重启需要按分钟节拍常驻检查；
 *  - NTP 重试要周期性重拉 ntpd（开机时网络/DNS 常常还没就绪）；
 *  - HTTP 端口切换要 stop+start 监听，而投递点正是 http_server 线程的
 *    after_flush 回调（在那儿 stop 会 join 它自己死锁）。
 * 故 `POST /api/v1/system/diag` 只做参数校验 + 投递任务立刻返回，前端用
 * `GET /api/v1/system/diag` 轮询结果；`mod_console` 的 `footprint.threads`
 * 相应从 1 增加到 2（见 console.c）。
 *
 * 命令注入防线（根 AGENTS：拼进 shell 的值必须过字符白名单）：
 *   - 目标地址只允许 `[A-Za-z0-9._:-]`，且**必须以字母数字开头**（不允许
 *     以 '-' 开头，否则会被当成命令行选项），长度 1–64；
 *   - 次数/包大小/超时是整数，各自卡死范围后用 %d 格式化；
 *   - 因此最终命令行里没有任何 shell 元字符（$ ` ; | & > < 空格均被字符集
 *     挡在门外），再额外用双引号包住地址兜底。
 *
 * 输出处理：子进程输出按**可打印 ASCII**归一（其余字节替换为 '.'）——
 * Windows 控制台是 GBK、设备端 busybox 是 ASCII，直接塞进 JSON 会出现非法
 * UTF-8 让浏览器解析失败；诊断输出本来就只有 ASCII 结果行。
 */
#include "console_internal.h"
#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "core/os.h"
#include "hal/hal.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD "console"

#ifdef _WIN32
#define popen  _popen
#define pclose _pclose
#endif

#define DIAG_OUT_MAX   3000   /**< 诊断输出上限（字节，超出截断） */
#define DIAG_TICK_MS   2000   /**< 工作线程节拍：兼顾诊断投递与重启计划检查 */
#define PLAN_LINE_MAX  32

/* 与 console_auth.c / console_api.c / console_net.c 同名同语义的本地 helper：
   三个既有文件各自持有一份 static 实现、互不引用对方符号，本文件照此办理。*/
static hal_err_t fmt_safe(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!out || cap == 0) return HAL_EINVAL;
    va_start(ap, fmt);
    n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return HAL_ENOMEM; }
    return HAL_OK;
}

/* ==========================================================================
 * 一、诊断：参数解析（纯函数）与任务投递
 * ========================================================================== */

typedef enum {
    DIAG_IDLE = 0,
    DIAG_RUNNING = 1,
    DIAG_DONE = 2
} diag_state_t;

typedef struct {
    bool  traceroute;   /**< false=ping, true=tracert/traceroute */
    char  addr[65];
    int   count;        /**< 1..50  */
    int   size;         /**< 4..1472 字节 */
    int   timeout_s;    /**< 1..2 秒 */
} diag_req_t;

typedef struct {
    os_mutex_t  *mu;
    os_cond_t   *cv;
    os_thread_t *th;
    bool stop;
    bool pending;           /**< 有新任务等工作线程取走 */
    diag_req_t  req;
    diag_state_t state;
    /* HTTP 端口切换任务（PRD LC-NET-02）：投递点是 http_server 的 after_flush
       回调（http_server 线程内），真正 stop+start 必须挪到本线程做，否则
       http_server_stop 会 join 它自己而死锁。old 供新端口绑定失败时回滚。 */
    bool port_pending;
    int  port_new;
    int  port_old;
    char  output[DIAG_OUT_MAX + 1];
    char  msg[96];
    int   rc;
    int64_t elapsed_ms;
} maint_t;

static maint_t s_mt;

/** 目标地址白名单：字母数字与 . _ : - ，且首字符必须是字母数字 */
static bool addr_ok(const char *s)
{
    size_t i;
    if (!s || !s[0] || strlen(s) > 64) return false;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') ||
          (s[0] >= '0' && s[0] <= '9')))
        return false;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static const char *json_str(const json_t *j, const char *key)
{
    return json_string(json_get(j, key), NULL);
}

static int json_int_val(const json_t *j, const char *key, int def, int lo, int hi)
{
    int64_t n = json_int(json_get(j, key), def);   /* 未提供/类型不符 → 默认值 */
    if (n < lo || n > hi) return def;              /* 越界回落默认（实机由输入框限定范围） */
    return (int)n;
}

/**
 * 解析并校验 POST /api/v1/system/diag 的请求体。纯函数，便于测试。
 * 失败时把具体原因写进 out（以 {"code": 开头，handler 会优先用它做响应体）。
 */
static hal_err_t diag_parse(const json_t *j, diag_req_t *out, char *msg, size_t msg_cap)
{
    const char *type, *addr;

    if (!j || !out) return HAL_EINVAL;
    memset(out, 0, sizeof(*out));

    type = json_str(j, "type");
    if (!type || (strcmp(type, "ping") != 0 && strcmp(type, "tracert") != 0)) {
        fmt_safe(msg, msg_cap, "诊断方式只能是 ping 或 tracert");
        return HAL_EINVAL;
    }
    out->traceroute = (strcmp(type, "tracert") == 0);

    addr = json_str(j, "addr");
    if (!addr_ok(addr)) {
        fmt_safe(msg, msg_cap, "目标地址只能包含字母、数字与 . _ : - ，且需以字母或数字开头");
        return HAL_EINVAL;
    }
    snprintf(out->addr, sizeof(out->addr), "%s", addr);

    out->count     = json_int_val(j, "count", 4, 1, 50);
    out->size      = json_int_val(j, "size", 64, 4, 1472);
    out->timeout_s = json_int_val(j, "timeout", 1, 1, 2);
    if (out->traceroute) out->count = 4;   /* traceroute 没有"包数"概念，忽略该参数 */
    return HAL_OK;
}

/** 把子进程输出归一成可打印 ASCII，防止非法 UTF-8 进 JSON */
static void sanitize_output(char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\n' || c == '\t') continue;
        if (c < 0x20 || c >= 0x7f) *s = '.';
    }
}

/** 构造诊断命令行：所有外部输入都已过白名单/范围校验 */
static void build_cmd(const diag_req_t *r, char *cmd, size_t cap)
{
#ifdef _WIN32
    if (r->traceroute)
        snprintf(cmd, cap, "tracert -d -h 20 -w %d \"%s\"", r->timeout_s * 1000, r->addr);
    else
        snprintf(cmd, cap, "ping -n %d -l %d -w %d \"%s\"",
                 r->count, r->size, r->timeout_s * 1000, r->addr);
#else
    /* busybox：ping -c 次数 -s 字节 -W 单次等待秒 -w 整体截止秒；
       traceroute -n 不解析 -m 最大跳数 -w 单跳等待秒 */
    if (r->traceroute)
        snprintf(cmd, cap, "traceroute -n -m 20 -w %d \"%s\"", r->timeout_s, r->addr);
    else
        snprintf(cmd, cap, "ping -c %d -s %d -W %d -w %d \"%s\"",
                 r->count, r->size, r->timeout_s,
                 r->count * r->timeout_s + 2, r->addr);
#endif
    (void)cap;
}

/** 跑一次诊断命令，输出写进 out（自动截断）。返回子进程退出码，失败为 -1 */
static int run_cmd(const char *cmd, char *out, size_t cap)
{
    FILE *fp;
    size_t n = 0;
    int rc;
    char line[256];

    out[0] = '\0';
    fp = popen(cmd, "r");
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        if (n + l >= cap) break;
        memcpy(out + n, line, l);
        n += l;
    }
    out[n] = '\0';
    rc = pclose(fp);
    sanitize_output(out);
    return rc;
}

/* ==========================================================================
 * 二、定时重启：按设备本地时间（time.timezone）检查到点即重启
 * ========================================================================== */

/* 设备本地时间换算用的偏移（秒，东正西负）在 console_apply.c：与 `ep_time_get`
 * 回给前端的时区、`console_tz_ok` 的校验同一份实现，避免三处符号/缺省各写一份。 */

/** 星期表 "0,1,..6"（0=周日）里是否包含 wd；空串视为每天都执行 */
static bool day_in(const char *days, int wd)
{
    const char *p = days;
    if (!p || !p[0]) return true;
    while (*p) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        if (v == wd) return true;
        if (*end != ',') break;
        p = end + 1;
    }
    return false;
}

static void tick_reboot_plan(void)
{
    static int64_t s_fired_day = -1;    /* 已触发的“距纪元天数”，一天只触发一次 */
    bool on = false;
    char hhmm[PLAN_LINE_MAX], days[PLAN_LINE_MAX], tz[48];
    int ph = -1, pm = -1;
    int64_t utc = 0, local;
    int64_t day;
    int wd, hh, mm;

    if (cfg_get_bool("system.reboot.plan.enable", &on) != HAL_OK || !on) { s_fired_day = -1; return; }
    hhmm[0] = '\0';
    if (cfg_get_str("system.reboot.plan.time", hhmm, sizeof(hhmm)) != HAL_OK) return;
    if (sscanf(hhmm, "%2d:%2d", &ph, &pm) != 2 || ph < 0 || ph > 23 || pm < 0 || pm > 59) return;
    days[0] = '\0';
    (void)cfg_get_str("system.reboot.plan.days", days, sizeof(days));
    if (!hal_has(HAL_MOD_SYS) || !hal()->sys->get_wallclock) return;
    if (hal()->sys->get_wallclock(&utc) != HAL_OK || utc <= 0) return;

    tz[0] = '\0';
    if (cfg_get_str("time.timezone", tz, sizeof(tz)) != HAL_OK || !tz[0])
        snprintf(tz, sizeof(tz), CONSOLE_TZ_DEFAULT);   /* 与 ep_time_get 同一缺省，否则差 8 小时 */
    local = utc + console_tz_offset_seconds(tz);

    day  = local / 86400;
    hh   = (int)((local % 86400) / 3600);
    mm   = (int)((local % 3600) / 60);
    wd   = (int)(((day % 7) + 4 + 7) % 7);   /* 1970-01-01 是周四 */
    if (hh != ph || mm != pm) return;
    if (!day_in(days, wd)) return;
    if (s_fired_day == day) return;

    s_fired_day = day;
    LOGI(MOD, "定时重启到点（%02d:%02d），设备即将重启", ph, pm);
    if (hal()->sys->reboot) hal()->sys->reboot();
}

/* ==========================================================================
 * 三、日夜定时切换：按设备本地时间在白天 / 夜晚之间切
 *
 * 图像页「日夜配置 = 日夜定时切换」（cfg `image.daynight=timed`）落的就是这里：
 * 用户给出白天开始与夜晚开始两个时刻（`image.daynight.day_start` /
 * `night_start`，"HH:MM"），到点把 HAL 的日夜档切过去。时间口径与定时重启
 * **必须同源**（`time.timezone`），否则会出现「重启按东八区、日夜按 UTC」的错位。
 *
 * 两个时刻的窗口语义（等价于实机那条 24h 时间轴，这里退化成两个刻度）：
 *   day <  night：白天 = [day, night)，其余为夜晚；
 *   day >  night：窗口跨零点，白天 = [day, 24:00) ∪ [00:00, night)；
 *   day == night：区间退化为 0 长度，无法表达「只在某一刻切」→ 按全天白天处理
 *                 （明确取一边，而不是沉默地跟着当前时刻随机切）。
 *
 * 设备**没有 RTC**：开机墙钟从纪元（1970-01-01）起算，校时之前按它算只会得到
 * 「午夜」这个假结论，所以未校时（< CONSOLE_CLOCK_UNSET_S）时**不下发**，保持
 * 当前档位并限流记日志；NTP 校时成功后本函数自会接手。
 * ========================================================================== */

#define DN_TIME_LEN        6     /* "HH:MM" + '\0' */
#define DN_LOG_MS     300000u    /* 未校时提示的限流：最多 5 分钟一条 */
/* 出厂默认时段：对齐参照实机时间轴的两个默认指针（06:00 / 18:00） */
#define DN_DAY_DEFAULT   "06:00"
#define DN_NIGHT_DEFAULT "18:00"

/** "HH:MM" → 0..1439 分钟；非法（长度、冒号位置、时分越界）返回 -1。
 *  与 system.reboot.plan.time 同一套格式口径（cfg 规则只卡字符集与长度）。 */
static int hhmm_min(const char *s)
{
    int h, m;
    if (!s || strlen(s) != 5 || s[2] != ':') return -1;
    if (sscanf(s, "%2d:%2d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

/** 「现在」（本地时间的分钟数）是否落在夜晚窗口；窗口定义见本节文件头注释 */
static bool timed_is_night(int day_min, int night_min, int now_min)
{
    bool in_day;

    if (day_min == night_min) return false;                 /* 退化：全天白天 */
    in_day = (day_min < night_min)
           ? (now_min >= day_min && now_min < night_min)    /* 同一昼夜内 */
           : (now_min >= day_min || now_min < night_min);   /* 跨零点 */
    return !in_day;
}

void console_maint_daynight_sync(void)
{
    static int s_applied = -1;          /* 已下发的档（-1 = 未下发或已让位给其它档） */
    static uint64_t s_last_log_us;
    char mode[16], ds[DN_TIME_LEN], ns[DN_TIME_LEN], tz[48];
    int day_min, night_min, now_min;
    int64_t utc = 0, local;
    hal_daynight_t want;
    uint64_t now_us;

    if (!hal_has(HAL_MOD_VIDEO) || !hal()->video || !hal()->video->set_daynight) return;
    mode[0] = '\0';
    if (cfg_get_str("image.daynight", mode, sizeof(mode)) != HAL_OK ||
        strcmp(mode, "timed") != 0) {
        s_applied = -1;   /* 离开定时档：清缓存，下次回到 timed 时重新判一次 */
        return;
    }
    if (!hal_has(HAL_MOD_SYS) || !hal()->sys->get_wallclock) return;

    /* 两个时刻取不到（键没写过）就用出厂默认；解析不了（键被外部改成非法串）
       也退回默认——不让一个坏值把整套日夜切换停掉。 */
    ds[0] = ns[0] = '\0';
    if (cfg_get_str("image.daynight.day_start", ds, sizeof(ds)) != HAL_OK)
        snprintf(ds, sizeof(ds), DN_DAY_DEFAULT);
    if (cfg_get_str("image.daynight.night_start", ns, sizeof(ns)) != HAL_OK)
        snprintf(ns, sizeof(ns), DN_NIGHT_DEFAULT);
    day_min = hhmm_min(ds);
    night_min = hhmm_min(ns);
    if (day_min < 0) day_min = hhmm_min(DN_DAY_DEFAULT);
    if (night_min < 0) night_min = hhmm_min(DN_NIGHT_DEFAULT);

    if (hal()->sys->get_wallclock(&utc) != HAL_OK) return;
    if (utc < CONSOLE_CLOCK_UNSET_S) {
        now_us = os_monotonic_us();
        if (now_us - s_last_log_us > DN_LOG_MS) {
            s_last_log_us = now_us;
            LOGI(MOD, "日夜定时切换待校时：墙钟未同步，暂不切换（时段 %s/%s）", ds, ns);
        }
        return;
    }

    tz[0] = '\0';
    if (cfg_get_str("time.timezone", tz, sizeof(tz)) != HAL_OK || !tz[0])
        snprintf(tz, sizeof(tz), CONSOLE_TZ_DEFAULT);   /* 与 ep_time_get 同一缺省 */
    local = utc + console_tz_offset_seconds(tz);
    now_min = (int)((local % 86400) / 60);

    want = timed_is_night(day_min, night_min, now_min) ? HAL_DAYNIGHT_NIGHT : HAL_DAYNIGHT_DAY;
    if ((int)want == s_applied) return;
    if (hal()->video->set_daynight(want) != HAL_OK) {
        LOGW(MOD, "日夜定时切换下发失败（%s），下一拍重试",
             want == HAL_DAYNIGHT_NIGHT ? "夜晚" : "白天");
        return;      /* 不记 s_applied：保持"未落实"，下一拍重试 */
    }
    s_applied = (int)want;
    LOGI(MOD, "日夜定时切换：本地 %02d:%02d 进入%s（时段 %s/%s）",
         now_min / 60, now_min % 60, want == HAL_DAYNIGHT_NIGHT ? "夜晚" : "白天", ds, ns);
    /* 日夜是两套配置（image.* / image.night.*）：切到哪个时段就把那套参数一并
       落到 HAL，否则切了 IRCUT/夜视却还在跑上一时段的画质参数（console_api.c
       的 console_image_apply_period 实现，键未写过时回落白天套）。 */
    console_image_apply_period(want == HAL_DAYNIGHT_NIGHT);
}

/* ==========================================================================
 * 四、NTP 重试：开机时网络/DNS 常常还没就绪，busybox ntpd 解析失败即退出
 * ========================================================================== */

#define NTP_RETRY_MIN_MS   10000u     /* 首次重拉：开机 10 秒后（DHCP/DNS 通常已就绪） */
#define NTP_RETRY_MAX_MS  120000u     /* 退避上限：断网时不至于每 10 秒折腾一次 */
#define NTP_RETRY_LOG_MS  300000u     /* 日志限流：最多 5 分钟一条 */

/**
 * 「该不该继续尝试校时」的纯判据：
 *   开启 ∧ 墙钟还落在 CONSOLE_CLOCK_UNSET_S（2000-01-01）之前
 * 设备没有 RTC，墙钟开机从 0 起算；只要它还没被校到正常年份，就说明
 * NTP 没成功（或根本没起），需要重拉。用户手动校时会把 ntp.enable 关掉，
 * 因此不会与手动设置打架。
 */
static bool ntp_retry_needed(bool enabled, int64_t utc)
{
    /* utc == 0 正是「刚开机、墙钟还在纪元点」的典型值，必须算作未校时；
       负值才是非法（get_wallclock 的失败由调用方按返回码处理，不靠数值猜）。 */
    return enabled && utc >= 0 && utc < CONSOLE_CLOCK_UNSET_S;
}

/**
 * 为什么必须有这一步：`console_apply_time()` 只在 console_start 里跑一次，
 * 那时链路/DNS 往往还没就绪，ntpd 起不来或立即退出，之后**没有任何代码
 * 会再试**——结果就是「联网了时间还停在 1970-01-01」。这里在工作线程里
 * 按退避节奏重拉，直到墙钟离开 1970（或用户关掉 NTP）为止。
 */
static void tick_ntp_retry(void)
{
    static uint64_t next_us;       /* 下次允许重拉的单调时刻 */
    static uint32_t backoff_ms;    /* 当前退避（0 表示尚未开始） */
    static uint64_t last_log_us;
    bool en = false;
    int64_t utc = 0;
    uint64_t now;

    if (!hal_has(HAL_MOD_SYS) || !hal()->sys->get_wallclock || !hal()->sys->apply_ntp) return;
    console_ntp_read(&en, NULL, 0);
    now = os_monotonic_us();
    if (!en || hal()->sys->get_wallclock(&utc) != HAL_OK || !ntp_retry_needed(en, utc)) {
        backoff_ms = 0;   /* 已校时 / 已关闭：退避与计时归零，下次异常从头来 */
        next_us = 0;
        return;
    }
    if (next_us && now < next_us) return;

    if (console_apply_time() != HAL_OK) {
        LOGW(MOD, "NTP 重拉失败（设备时间仍为开机计时），稍后重试");
    } else if (!last_log_us || (now - last_log_us) >= NTP_RETRY_LOG_MS * 1000ull) {
        last_log_us = now;
        LOGW(MOD, "NTP 尚未同步（设备时间仍为开机计时），按缺省/已保存服务器重拉 ntpd");
    }
    backoff_ms = backoff_ms == 0 ? NTP_RETRY_MIN_MS
               : (backoff_ms >= NTP_RETRY_MAX_MS / 2 ? NTP_RETRY_MAX_MS : backoff_ms * 2);
    next_us = now + (uint64_t)backoff_ms * 1000ull;
}

/* ==========================================================================
 * 五、工作线程
 * ========================================================================== */

static void run_diag(const diag_req_t *r)
{
    char cmd[256];
    char out[DIAG_OUT_MAX + 1];
    int rc;
    uint64_t t0 = os_monotonic_us();

    build_cmd(r, cmd, sizeof(cmd));
    rc = run_cmd(cmd, out, sizeof(out));

    os_mutex_lock(s_mt.mu);
    snprintf(s_mt.output, sizeof(s_mt.output), "%s", out);
    s_mt.rc = rc;
    s_mt.elapsed_ms = (int64_t)((os_monotonic_us() - t0) / 1000ull);
    if (rc != 0 && !out[0])
        snprintf(s_mt.msg, sizeof(s_mt.msg), "诊断命令执行失败（本机可能没有该命令）");
    else
        s_mt.msg[0] = '\0';
    s_mt.state = DIAG_DONE;
    os_mutex_unlock(s_mt.mu);
}

/**
 * 切换 HTTP 监听端口：停旧监听 → 起新端口；新端口绑定失败则回滚旧端口，
 * 设备绝不掉进“两个端口都没有”的状态（失败只能靠串口日志排查）。
 * 调用前等一拍：投递发生在响应 flush 之后的回调里，给 TCP 收尾留时间。
 */
static void run_port_rebind(int neu, int old)
{
    os_sleep_ms(300);
    if (http_server_stop() != HAL_OK) {
        LOGW(MOD, "端口切换跳过：HTTP 服务未在运行");
        return;
    }
    if (http_server_start((uint16_t)neu) == HAL_OK) {
        LOGI(MOD, "HTTP 监听端口已切换为 %d", neu);
        return;
    }
    LOGE(MOD, "切换到端口 %d 失败（被占用或权限不足），回滚到 %d", neu, old);
    if (old > 0 && old != neu)
        http_server_start((uint16_t)old);
}

static void maint_thread(void *arg)
{
    (void)arg;
    for (;;) {
        diag_req_t req;
        bool job = false;
        int port_new = -1, port_old = -1;

        os_mutex_lock(s_mt.mu);
        if (!s_mt.stop && !s_mt.pending && !s_mt.port_pending)
            os_cond_wait(s_mt.cv, s_mt.mu, DIAG_TICK_MS);
        if (s_mt.stop) { os_mutex_unlock(s_mt.mu); break; }
        if (s_mt.pending) {
            req = s_mt.req;
            s_mt.pending = false;
            s_mt.state = DIAG_RUNNING;
            job = true;
        }
        if (s_mt.port_pending) {
            port_new = s_mt.port_new;
            port_old = s_mt.port_old;
            s_mt.port_pending = false;
        }
        os_mutex_unlock(s_mt.mu);

        if (job) run_diag(&req);
        if (port_new > 0) run_port_rebind(port_new, port_old);
        tick_reboot_plan();
        tick_ntp_retry();
        /* 日夜定时切换：到点误差 ≤ 工作节拍（DIAG_TICK_MS）。非 timed 档时
           本调用只清内部缓存，代价几乎为零。 */
        console_maint_daynight_sync();
    }
}

/** 投递「切换 HTTP 监听端口」任务；见 console_internal.h 的声明注释。
 *  重复投递（上一次还没被取走）→ HAL_ESTATE：端口是人手改的，不排队。 */
hal_err_t console_maint_post_port(int new_port, int old_port)
{
    if (new_port <= 0 || new_port > 65535) return HAL_EINVAL;
    /* 懒初始化兜底：与 console_maint_diag 同理（单测可能没走模块生命周期） */
    if (!s_mt.mu && console_maint_init() != HAL_OK) return HAL_EIO;
    os_mutex_lock(s_mt.mu);
    if (s_mt.port_pending) {
        os_mutex_unlock(s_mt.mu);
        return HAL_ESTATE;
    }
    s_mt.port_new = new_port;
    s_mt.port_old = old_port;
    s_mt.port_pending = true;
    os_mutex_unlock(s_mt.mu);
    os_cond_signal(s_mt.cv);
    return HAL_OK;
}

/* ==========================================================================
 * 六、端点（由 console_api.c 分发进来）
 * ========================================================================== */

static hal_err_t maint_json(char *out, size_t cap, const char *code_msg, int code)
{
    json_t *root = json_new_object();
    char *txt;
    hal_err_t rc;

    if (!root) return HAL_ENOMEM;
    json_object_set(root, "code", json_new_int(code));
    if (code_msg && code_msg[0]) json_object_set(root, "msg", json_new_string(code_msg));
    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

/** POST /api/v1/system/diag：校验 + 投递任务（不执行） */
hal_err_t console_maint_diag(const http_req_t *req, char *out, size_t cap)
{
    json_t *j;
    diag_req_t r;
    char msg[128];
    hal_err_t rc;

    msg[0] = '\0';
    if (!req || !req->body || !req->body_len) return HAL_EINVAL;
    /* 懒初始化兜底：生产路径里 console_init 必然先于 HTTP 服务，但单测直接
       驱动 api_dispatch 时可能没走模块生命周期——只建互斥/条件变量，不起线程。 */
    if (!s_mt.mu && console_maint_init() != HAL_OK) return HAL_EIO;
    j = json_parse(req->body, req->body_len, NULL, 0);
    if (!j) {
        fmt_safe(out, cap, "{\"code\":-1,\"msg\":\"请求体不是合法 JSON\"}");
        return HAL_EINVAL;
    }
    rc = diag_parse(j, &r, msg, sizeof(msg));
    json_free(j);
    if (rc != HAL_OK) {
        fmt_safe(out, cap, "{\"code\":-1,\"msg\":\"%s\"}", msg);
        return HAL_EINVAL;
    }

    os_mutex_lock(s_mt.mu);
    if (s_mt.pending || s_mt.state == DIAG_RUNNING) {
        os_mutex_unlock(s_mt.mu);
        /* 上一次还没跑完：不排队第二发（诊断是人手点的，没必要串行等待），
           如实告诉前端继续轮询即可。 */
        return maint_json(out, cap, "上一次诊断仍在进行，请稍候查看结果", 0);
    }
    s_mt.req = r;
    s_mt.pending = true;
    s_mt.state = DIAG_RUNNING;
    s_mt.output[0] = '\0';
    s_mt.msg[0] = '\0';
    s_mt.rc = 0;
    s_mt.elapsed_ms = 0;
    os_mutex_unlock(s_mt.mu);
    os_cond_signal(s_mt.cv);
    return maint_json(out, cap, "诊断已开始", 0);
}

/** GET /api/v1/system/diag：查询状态与结果（前端轮询） */
hal_err_t console_maint_diag_state(char *out, size_t cap)
{
    json_t *root;
    const char *st = "idle";
    char *txt;
    hal_err_t rc;

    if (!s_mt.mu && console_maint_init() != HAL_OK) return HAL_EIO;
    root = json_new_object();
    if (!root) return HAL_ENOMEM;
    os_mutex_lock(s_mt.mu);
    if (s_mt.state == DIAG_RUNNING) st = "running";
    else if (s_mt.state == DIAG_DONE) st = "done";
    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "state", json_new_string(st));
    json_object_set(root, "type", json_new_string(s_mt.req.traceroute ? "tracert" : "ping"));
    json_object_set(root, "addr", json_new_string(s_mt.req.addr));
    json_object_set(root, "rc", json_new_int(s_mt.rc));
    json_object_set(root, "elapsed_ms", json_new_int(s_mt.elapsed_ms));
    if (s_mt.msg[0]) json_object_set(root, "msg", json_new_string(s_mt.msg));
    json_object_set(root, "output", json_new_string(s_mt.output));
    os_mutex_unlock(s_mt.mu);

    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

/* ==========================================================================
 * 七、生命周期（module.h：init 不起线程，start 才起）
 * ========================================================================== */

hal_err_t console_maint_init(void)
{
    if (s_mt.mu) return HAL_ESTATE;
    memset(&s_mt, 0, sizeof(s_mt));
    s_mt.mu = os_mutex_create();
    s_mt.cv = os_cond_create();
    if (!s_mt.mu || !s_mt.cv) return HAL_ENOMEM;
    return HAL_OK;
}

hal_err_t console_maint_start(void)
{
    if (!s_mt.mu) return HAL_ESTATE;
    s_mt.stop = false;
    s_mt.th = os_thread_create(maint_thread, NULL, "consmaint", 64);
    if (!s_mt.th) return HAL_ENOMEM;
    return HAL_OK;
}

hal_err_t console_maint_stop(void)
{
    if (!s_mt.mu) return HAL_OK;
    os_mutex_lock(s_mt.mu);
    s_mt.stop = true;
    os_mutex_unlock(s_mt.mu);
    os_cond_broadcast(s_mt.cv);
    if (s_mt.th) { os_thread_join(s_mt.th); s_mt.th = NULL; }
    return HAL_OK;
}

#ifdef IPC_TESTING
/* 测试桩：定时重启的时区换算、星期表判定、NTP 重试判据都是纯函数，单独
   暴露给 console_test，防止「按 UTC 走、页面按东八区显示」与「默认不校时」
   这类整点/整年错位回归。 */
int  console_maint_test_tz_offset(const char *tz) { return console_tz_offset_seconds(tz); }
bool console_maint_test_day_in(const char *days, int wd) { return day_in(days, wd); }
bool console_maint_test_ntp_needed(bool enabled, int64_t utc) { return ntp_retry_needed(enabled, utc); }
int  console_maint_test_hhmm_min(const char *s) { return hhmm_min(s); }
bool console_maint_test_timed_is_night(int day_min, int night_min, int now_min)
{
    return timed_is_night(day_min, night_min, now_min);
}
#endif
