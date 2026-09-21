/**
 * @file main.c
 * @brief IPC 固件主程序：启动编排与信号处理
 *
 * 全仓库第一个非测试的 main()。只做编排，不含业务逻辑。启动顺序固定：
 *   日志 → 读取 profile 原文 → hal_init → cfg_init（内部据此加载 profile）→
 *   模块注册 → 模块启动（含 HTTP 路由注册）→ HTTP 监听
 *
 * 退出路径按初始化的反序释放资源，任一步启动失败都打印中文错误并以非零码
 * 退出——在板子上串口输出是唯一的诊断手段，静默继续会让人完全不知道发生
 * 了什么。这一点与 HAL 层"失败不阻断启动"的原则刻意相反。
 */
#include "core/log.h"
#include "core/config.h"
#include "core/module.h"
#include "core/os.h"
#include "hal/hal.h"
#include "modules/common/http_server/http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define DEFAULT_PORT      8080
#define DEFAULT_PERSIST   "/etc/ipc/config.json"
#define PROFILE_MAX       (64 * 1024)
#define HEALTH_INTERVAL_MS 5000   /**< 模块健康巡检周期 */
#define QUIT_POLL_MS       100    /**< 退出标志轮询步长，决定信号响应延迟上限 */

/* 信号处理器仅可做异步信号安全的操作：只置标志，真正的收尾在主循环里做。 */
static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "用法: %s --profile <能力清单路径> [--port <端口>] [--persist <配置文件路径>]\n"
        "  --profile   必需，产品能力清单 JSON\n"
        "  --port      HTTP 监听端口，默认 %d\n"
        "  --persist   配置持久化路径，默认 %s\n",
        argv0, DEFAULT_PORT, DEFAULT_PERSIST);
}

/* 读取 profile 文件到堆内存，调用方负责 free */
static char *read_profile(const char *path)
{
    FILE *fp = fopen(path, "rb");
    char *buf;
    size_t n;

    if (!fp) return NULL;
    buf = (char *)malloc(PROFILE_MAX);
    if (!buf) { fclose(fp); return NULL; }
    n = fread(buf, 1, PROFILE_MAX - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    if (n == 0) { free(buf); return NULL; }
    return buf;
}

int main(int argc, char **argv)
{
    const char *profile_path = NULL;
    const char *persist_path = DEFAULT_PERSIST;
    int port = DEFAULT_PORT;
    char *profile_json = NULL;
    hal_err_t rc;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            profile_path = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--persist") == 0 && i + 1 < argc) {
            persist_path = argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!profile_path || port <= 0 || port > 65535) {
        usage(argv[0]);
        return 2;
    }

    /* 尽早初始化日志，保证后续失败可见 */
    log_init(LOG_INFO, 64);
    LOGI("app", "IPC 固件启动中，端口 %d", port);

    profile_json = read_profile(profile_path);
    if (!profile_json) {
        LOGE("app", "读取能力清单失败: %s", profile_path);
        return 1;
    }

    rc = hal_init(profile_json);
    if (rc != HAL_OK) {
        LOGE("app", "HAL 初始化失败: %s", hal_strerror(rc));
        free(profile_json);
        return 1;
    }

    rc = cfg_init(profile_json, persist_path);
    if (rc != HAL_OK) {
        LOGE("app", "配置中心初始化失败: %s", hal_strerror(rc));
        goto fail_hal;
    }

    rc = module_register(&mod_console);
    if (rc != HAL_OK) {
        LOGE("app", "注册 console 模块失败: %s", hal_strerror(rc));
        goto fail_cfg;
    }

    rc = module_start_all();
    if (rc != HAL_OK) {
        LOGE("app", "启动模块失败: %s", hal_strerror(rc));
        goto fail_cfg;
    }

    rc = http_server_start((uint16_t)port);
    if (rc != HAL_OK) {
        LOGE("app", "HTTP 监听失败（端口 %d 可能被占用）: %s", port, hal_strerror(rc));
        goto fail_modules;
    }

    /* 监听建立之后再挂信号处理器：此前的失败路径走正常 return，不依赖 g_quit。 */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);   /* 对端断开时写 socket 不得杀死进程 */
#endif

    LOGI("app", "启动完成，控制台地址 http://<设备IP>:%d/", port);

    /*
     * 主循环：周期巡检模块健康，收到信号后退出。
     *
     * 健康巡检节奏仍是 5 秒一次，但不能一次性 os_sleep_ms(5000)：其 POSIX
     * 实现（core/src/os.c）底层是 nanosleep，且 signal() 装的处理器默认带
     * SA_RESTART——nanosleep 被信号打断后会自动重启剩余时长，单次长睡会让
     * Ctrl+C/SIGTERM 最长等满 5 秒才生效，真机上停服务体验很差，且 SIGTERM
     * 超时还可能被 SIGKILL 强杀导致来不及清理。改成 100ms 步长轮询退出标志，
     * 退出响应延迟上限降到 QUIT_POLL_MS，同时累计满 HEALTH_INTERVAL_MS 才
     * 巡检一次，不增加巡检频率。
     */
    while (!g_quit) {
        char detail[128];
        unsigned waited;

        if (!module_health_check(detail, sizeof(detail))) {
            LOGW("app", "模块健康检查异常: %s", detail);
        }
        for (waited = 0; waited < HEALTH_INTERVAL_MS && !g_quit; waited += QUIT_POLL_MS) {
            os_sleep_ms(QUIT_POLL_MS);
        }
    }

    LOGI("app", "收到退出信号，正在停止");
    http_server_stop();
fail_modules:
    module_stop_all();
fail_cfg:
    cfg_deinit();
fail_hal:
    hal_deinit();
    free(profile_json);
    LOGI("app", "已退出");
    return (rc == HAL_OK) ? 0 : 1;
}
