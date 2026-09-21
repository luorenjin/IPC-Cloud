/**
 * @file gk_platform.c
 * @brief GK7205V200 平台 —— hal_ops_t 聚合与导出
 *
 * 结构与 platform/mock/mock_platform.c 一致。audio/osd/ivs 置 NULL
 * （hal.h 允许可选模块为 NULL）；video/gpio 为必选，用 ENOTSUP 桩填充。
 */
#include "hal/hal.h"
#include "core/json.h"
#include <string.h>
#include <stdio.h>

extern const hal_video_ops_t   gk_video_ops;
extern const hal_gpio_ops_t    gk_gpio_ops;
extern const hal_net_ops_t     gk_net_ops;
extern const hal_storage_ops_t gk_storage_ops;
extern const hal_sys_ops_t     gk_sys_ops;
extern const hal_crypto_ops_t  gk_crypto_ops;

void gk_sys_set_profile_platform(const char *id);

static bool g_inited;

static hal_err_t gk_init(const char *profile_json)
{
    char err[128];
    json_t *j;

    if (g_inited) return HAL_ESTATE;
    if (!profile_json) return HAL_EINVAL;

    /* 从 profile 取 identity.platform，供 sys 在 cpuinfo 解析失败时回退。
       解析失败不阻断启动——sys 自带默认值（spec §8 通用原则） */
    j = json_parse(profile_json, 0, err, sizeof(err));
    if (j) {
        const char *plat = json_string(json_path(j, "identity.platform"), NULL);
        if (plat) gk_sys_set_profile_platform(plat);
        json_free(j);
    }

    g_inited = true;
    return HAL_OK;
}

static hal_err_t gk_deinit(void)
{
    if (!g_inited) return HAL_ESTATE;
    g_inited = false;
    return HAL_OK;
}

static const hal_ops_t g_gk_ops = {
    HAL_API_VERSION,
    "gk7205v200",
    gk_init,
    gk_deinit,
    &gk_video_ops,
    NULL,               /* audio：本期不实现 */
    NULL,               /* osd：本期不实现 */
    NULL,               /* ivs：本期不实现 */
    &gk_gpio_ops,
    &gk_net_ops,
    &gk_storage_ops,
    &gk_sys_ops,
    &gk_crypto_ops
};

const hal_ops_t *hal_platform_get(void)
{
    return &g_gk_ops;
}
