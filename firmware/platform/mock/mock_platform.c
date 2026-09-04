/**
 * @file mock_platform.c
 * @brief mock 平台 —— hal_ops_t 聚合与导出
 *
 * 用途：x86 上运行 core/modules 与一致性测试；作为新平台适配的参考骨架。
 * 真实平台照此文件结构导出 hal_platform_get()，各模块 ops 放各自源文件。
 */
#include "hal/hal.h"
#include <string.h>
#include <stdio.h>

extern const hal_video_ops_t   mock_video_ops;
extern const hal_audio_ops_t   mock_audio_ops;
extern const hal_osd_ops_t     mock_osd_ops;
extern const hal_ivs_ops_t     mock_ivs_ops;
extern const hal_gpio_ops_t    mock_gpio_ops;
extern const hal_net_ops_t     mock_net_ops;
extern const hal_storage_ops_t mock_storage_ops;
extern const hal_sys_ops_t     mock_sys_ops;
extern const hal_crypto_ops_t  mock_crypto_ops;

void mock_sys_boot(void);

static bool g_inited;

static hal_err_t mock_init(const char *profile_json)
{
    if (g_inited) return HAL_ESTATE;
    if (!profile_json) return HAL_EINVAL;
    /* 真实平台在此解析 profile 的 gpio_map / video.sensors / network.wifi；mock 仅做存在性检查 */
    if (strstr(profile_json, "\"platform\"") && !strstr(profile_json, "\"mock\"")) {
        fprintf(stderr, "[mock] warning: profile.identity.platform is not \"mock\"\n");
    }
    mock_sys_boot();
    g_inited = true;
    return HAL_OK;
}

static hal_err_t mock_deinit(void)
{
    if (!g_inited) return HAL_ESTATE;
    g_inited = false;
    return HAL_OK;
}

static const hal_ops_t g_mock_ops = {
    HAL_API_VERSION,
    "mock",
    mock_init,
    mock_deinit,
    &mock_video_ops,
    &mock_audio_ops,
    &mock_osd_ops,
    &mock_ivs_ops,
    &mock_gpio_ops,
    &mock_net_ops,
    &mock_storage_ops,
    &mock_sys_ops,
    &mock_crypto_ops
};

const hal_ops_t *hal_platform_get(void)
{
    return &g_mock_ops;
}
