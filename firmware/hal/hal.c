/**
 * @file hal.c
 * @brief HAL 聚合层通用实现（平台无关）：初始化、版本校验、错误码字符串
 */
#include "hal.h"
#include <string.h>
#include <stdio.h>

static const hal_ops_t *g_hal = NULL;

hal_err_t hal_init(const char *profile_json)
{
    const hal_ops_t *ops;
    hal_err_t rc;

    if (g_hal) {
        return HAL_ESTATE;
    }
    ops = hal_platform_get();
    if (!ops) {
        return HAL_ENODEV;
    }
    if ((ops->version >> 16) != HAL_API_VERSION_MAJOR) {
        fprintf(stderr, "[hal] version mismatch: platform=0x%08x core=0x%08x\n",
                ops->version, HAL_API_VERSION);
        return HAL_ENOTSUP;
    }
    /* 必选模块检查 */
    if (!ops->video || !ops->gpio || !ops->net || !ops->storage || !ops->sys ||
        !ops->init || !ops->deinit || !ops->platform_id) {
        return HAL_EINVAL;
    }
    rc = ops->init(profile_json ? profile_json : "{}");
    if (rc != HAL_OK) {
        return rc;
    }
    g_hal = ops;
    return HAL_OK;
}

hal_err_t hal_deinit(void)
{
    hal_err_t rc;
    if (!g_hal) {
        return HAL_ESTATE;
    }
    rc = g_hal->deinit();
    g_hal = NULL;
    return rc;
}

const hal_ops_t *hal(void)
{
    return g_hal;
}

bool hal_has(hal_module_t mod)
{
    if (!g_hal) {
        return false;
    }
    switch (mod) {
    case HAL_MOD_VIDEO:   return g_hal->video != NULL;
    case HAL_MOD_AUDIO:   return g_hal->audio != NULL;
    case HAL_MOD_OSD:     return g_hal->osd != NULL;
    case HAL_MOD_IVS:     return g_hal->ivs != NULL;
    case HAL_MOD_GPIO:    return g_hal->gpio != NULL;
    case HAL_MOD_NET:     return g_hal->net != NULL;
    case HAL_MOD_STORAGE: return g_hal->storage != NULL;
    case HAL_MOD_SYS:     return g_hal->sys != NULL;
    case HAL_MOD_CRYPTO:  return g_hal->crypto != NULL;
    default:              return false;
    }
}

const char *hal_strerror(hal_err_t err)
{
    switch (err) {
    case HAL_OK:       return "ok";
    case HAL_EINVAL:   return "invalid argument";
    case HAL_ENOTSUP:  return "not supported";
    case HAL_EBUSY:    return "busy";
    case HAL_ETIMEOUT: return "timeout";
    case HAL_EIO:      return "io error";
    case HAL_ENOMEM:   return "out of memory";
    case HAL_ENODEV:   return "no device";
    case HAL_EAGAIN:   return "try again";
    case HAL_ESTATE:   return "bad state";
    case HAL_ECORRUPT: return "corrupt";
    default:           return "unknown";
    }
}
