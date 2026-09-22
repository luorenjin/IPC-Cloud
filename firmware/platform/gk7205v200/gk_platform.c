/**
 * @file gk_platform.c
 * @brief GK7205V200 平台 —— hal_ops_t 聚合与导出
 *
 * 结构与 platform/mock/mock_platform.c 一致。audio/osd/ivs 置 NULL
 * （hal.h 允许可选模块为 NULL）；video/gpio 为必选，用 ENOTSUP 桩填充。
 *
 * 不使用 core/json 做完整 JSON 解析（Ruling 11）：platform/ 是 HAL 分层
 * 架构的 L0，core/ 是 L2，《IPC固件平台化架构_HAL适配方案.md》明确
 * "依赖方向：L4 → L3 → L2 → L1 → L0，严格单向"——链 ipc_core 会构成
 * L0 反向依赖 L2。这里只需要对 profile 的 identity.platform 做存在性/
 * 一致性校验（见 gk_init 注释），strstr 的最小字符串检查足够，不需要
 * 完整解析，做法与 platform/mock/mock_platform.c 的既有 strstr 检查一致。
 */
#include "hal/hal.h"
#include <string.h>
#include <stdio.h>

extern const hal_video_ops_t   gk_video_ops;
extern const hal_gpio_ops_t    gk_gpio_ops;
extern const hal_net_ops_t     gk_net_ops;
extern const hal_storage_ops_t gk_storage_ops;
extern const hal_sys_ops_t     gk_sys_ops;
extern const hal_crypto_ops_t  gk_crypto_ops;
extern void gk_crypto_ensure_dir(void);

static bool g_inited;

static hal_err_t gk_init(const char *profile_json)
{
    if (g_inited) return HAL_ESTATE;
    if (!profile_json) return HAL_EINVAL;

    /* platform_id 以平台自身为准，不从 profile 读取并注入（Ruling 9）：
       gk_sys.c 的 chip_id/platform_id 一律用编译期常量 "gk7205v200"，
       不受 profile 内容影响。这里只做一致性校验——profile 的
       identity.platform 与本平台不符时打印告警但忽略，不阻断启动
       （spec §8 通用原则），也不修改任何状态。这避免了"忘了传
       -DIPC_PROFILE 落到默认 mock-x86.json"这类配置失误让设备把自己
       上报成别的平台（曾经的实现是用 profile 的值覆盖 platform_id，
       构成一个真实的身份污染缺陷）。 */
    if (strstr(profile_json, "\"platform\"") && !strstr(profile_json, "\"gk7205v200\"")) {
        fprintf(stderr, "[gk7205v200] 警告：profile.identity.platform 与本平台不一致，已忽略\n");
    }

    /* 评审 I-3：幂等预建安全存储目录，建立"固件运行起来之后目录必然存在"
       的恒定前提——真机 rootfs 打包（Task 11）理论上也会做这件事，这里
       是双保险，确保即使打包步骤遗漏，gk_crypto.c 的 c_read 仍能正确把
       "目录整体缺失"（此刻探测不到 → 分区未挂载/被删，HAL_EIO）与
       "目录在但 key 未写过"（HAL_ENODEV，真未配置）区分开。 */
    gk_crypto_ensure_dir();

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
