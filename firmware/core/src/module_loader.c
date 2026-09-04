/**
 * @file module_loader.c
 * @brief 模块启动器：注册、enabled 过滤、依赖拓扑排序、init/start/stop、健康巡检与内存预算
 */
#include "core/module.h"
#include "core/profile.h"
#include "core/log.h"
#include "core/os.h"
#include "hal/hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MOD "loader"
#define MODULES_MAX 16

typedef struct {
    const module_desc_t *desc;
    module_state_t       state;
    uint32_t             health_fails;
} slot_t;

static struct {
    slot_t   slots[MODULES_MAX];
    size_t   count;
    bool     started;
    /* 拓扑序缓存 */
    size_t   order[MODULES_MAX];
    size_t   order_count;
} g;

hal_err_t module_register(const module_desc_t *desc)
{
    if (!desc || !desc->name || !desc->init || !desc->start || !desc->stop || !desc->deinit) return HAL_EINVAL;
    if (g.count >= MODULES_MAX) return HAL_ENOMEM;
    for (size_t i = 0; i < g.count; i++) {
        if (strcmp(g.slots[i].desc->name, desc->name) == 0) return HAL_EBUSY;
    }
    g.slots[g.count].desc = desc;
    g.slots[g.count].state = MOD_STATE_UNLOADED;
    g.count++;
    return HAL_OK;
}

/* ---------------- 拓扑排序（Kahn，稳定：按注册序） ---------------- */

static int find_by_name(const char *name)
{
    for (size_t i = 0; i < g.count; i++) {
        if (g.slots[i].desc->enabled && !g.slots[i].desc->enabled()) continue;   /* 未启用模块不参与 */
        if (strcmp(g.slots[i].desc->name, name) == 0) return (int)i;
    }
    return -1;
}

static bool slot_enabled(const slot_t *s)
{
    return !s->desc->enabled || s->desc->enabled();
}

static hal_err_t topo_sort(void)
{
    size_t n = 0;
    bool done[MODULES_MAX];
    memset(done, 0, sizeof(done));
    g.order_count = 0;

    for (size_t i = 0; i < g.count; i++) if (slot_enabled(&g.slots[i])) n++;
    if (n == 0) return HAL_OK;

    while (g.order_count < n) {
        bool progressed = false;
        for (size_t i = 0; i < g.count; i++) {
            bool ready = true;
            if (done[i] || !slot_enabled(&g.slots[i])) continue;
            if (g.slots[i].desc->deps) {
                for (const char *const *d = g.slots[i].desc->deps; *d; d++) {
                    int di = find_by_name(*d);
                    if (di < 0) continue;                       /* 依赖未启用：视为不存在 */
                    if (!done[di]) { ready = false; break; }
                }
            }
            if (ready) {
                done[i] = true;
                g.order[g.order_count++] = i;
                progressed = true;
            }
        }
        if (!progressed) {
            LOGE(MOD, "dependency cycle detected");
            return HAL_ESTATE;
        }
    }
    return HAL_OK;
}

static uint32_t budget_check(void)
{
    const profile_t *p = profile_get();
    uint64_t total_kb = 0;
    if (!p) return 0;
    for (size_t i = 0; i < g.order_count; i++) total_kb += g.slots[g.order[i]].desc->footprint.rss_kb_estimate;
    if (total_kb > (uint64_t)p->mem_budget_mb * 1024ULL) {
        LOGW(MOD, "declared footprint %lluKB exceeds budget %uMB",
             (unsigned long long)total_kb, p->mem_budget_mb);
    }
    return (uint32_t)total_kb;
}

hal_err_t module_start_all(void)
{
    hal_err_t rc = topo_sort();
    if (rc != HAL_OK) return rc;
    if (g.started) return HAL_ESTATE;
    LOGI(MOD, "footprint estimate: %uKB", budget_check());

    for (size_t i = 0; i < g.order_count; i++) {
        slot_t *s = &g.slots[g.order[i]];
        rc = s->desc->init();
        if (rc != HAL_OK) {
            LOGE(MOD, "%s init failed: %s", s->desc->name, hal_strerror(rc));
            s->state = MOD_STATE_FAILED;
            return rc;
        }
        s->state = MOD_STATE_INITED;
    }
    for (size_t i = 0; i < g.order_count; i++) {
        slot_t *s = &g.slots[g.order[i]];
        rc = s->desc->start();
        if (rc != HAL_OK) {
            LOGE(MOD, "%s start failed: %s", s->desc->name, hal_strerror(rc));
            s->state = MOD_STATE_FAILED;
            return rc;
        }
        s->state = MOD_STATE_RUNNING;
        LOGI(MOD, "started %s (v%u, %uKB est, %u threads)", s->desc->name, s->desc->version,
             s->desc->footprint.rss_kb_estimate, s->desc->footprint.threads);
    }
    g.started = true;
    return HAL_OK;
}

hal_err_t module_stop_all(void)
{
    if (!g.started) return HAL_ESTATE;
    for (size_t i = g.order_count; i-- > 0; ) {
        slot_t *s = &g.slots[g.order[i]];
        if (s->state == MOD_STATE_RUNNING) {
            hal_err_t rc = s->desc->stop();
            if (rc != HAL_OK) LOGW(MOD, "%s stop: %s", s->desc->name, hal_strerror(rc));
            s->state = MOD_STATE_STOPPED;
        }
    }
    for (size_t i = g.order_count; i-- > 0; ) {
        slot_t *s = &g.slots[g.order[i]];
        if (s->state == MOD_STATE_STOPPED || s->state == MOD_STATE_INITED) {
            hal_err_t rc = s->desc->deinit();
            if (rc != HAL_OK) LOGW(MOD, "%s deinit: %s", s->desc->name, hal_strerror(rc));
            s->state = MOD_STATE_UNLOADED;
        }
    }
    g.started = false;
    return HAL_OK;
}

module_state_t module_state(const char *name)
{
    for (size_t i = 0; i < g.count; i++) if (strcmp(g.slots[i].desc->name, name) == 0) return g.slots[i].state;
    return MOD_STATE_UNLOADED;
}

size_t module_list(const module_desc_t **out, size_t max)
{
    size_t n = 0;
    if (!out) return g.count;
    for (size_t i = 0; i < g.count && n < max; i++) out[n++] = g.slots[i].desc;
    return n;
}

/**
 * 健康巡检：由主循环周期调用。
 * 连续 3 次失败的运行模块尝试 stop+start 重启；重启 3 次仍失败返回 false（触发看门狗）。
 */
bool module_health_check(char *detail, size_t cap)
{
    bool all_ok = true;
    for (size_t i = 0; i < g.count; i++) {
        slot_t *s = &g.slots[i];
        if (s->state != MOD_STATE_RUNNING || !s->desc->health) continue;
        if (s->desc->health(detail, cap) == HAL_OK) {
            s->health_fails = 0;
            continue;
        }
        s->health_fails++;
        all_ok = false;
        LOGW(MOD, "%s health failed (%u)", s->desc->name, s->health_fails);
        if (s->health_fails >= 3) {
            LOGE(MOD, "restarting %s", s->desc->name);
            if (s->desc->stop() == HAL_OK && s->desc->start() == HAL_OK) {
                s->health_fails = 0;
            } else {
                s->state = MOD_STATE_FAILED;
            }
        }
    }
    return all_ok;
}
