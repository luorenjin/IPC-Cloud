/**
 * @file event_bus.c
 * @brief 进程内事件总线：环形队列 + 单投递线程 + 域掩码订阅
 */
#include "core/event_bus.h"
#include "core/os.h"
#include "core/log.h"
#include <stdlib.h>
#include <string.h>

#define MOD "evt"
#define SUBS_MAX 32

struct event_sub {
    uint32_t        domain_mask;
    event_handler_t handler;
    void           *user;
    bool            active;
};

static struct {
    bool          inited;
    event_t      *queue;
    uint32_t      cap, head, tail, count;
    uint64_t      dropped;
    event_sub_t   subs[SUBS_MAX];
    os_mutex_t   *mu;
    os_cond_t    *cv;
    os_thread_t  *thread;
    bool          stop;
} g;

static void deliver_thread(void *arg)
{
    (void)arg;
    for (;;) {
        event_t evt;
        event_sub_t snapshot[SUBS_MAX];
        os_mutex_lock(g.mu);
        while (g.count == 0 && !g.stop) os_cond_wait(g.cv, g.mu, UINT32_MAX);
        if (g.stop && g.count == 0) { os_mutex_unlock(g.mu); return; }
        evt = g.queue[g.head];
        g.head = (g.head + 1) % g.cap;
        g.count--;
        memcpy(snapshot, g.subs, sizeof(snapshot));   /* 回调在锁外执行，避免回调内订阅/退订死锁 */
        os_mutex_unlock(g.mu);

        for (int i = 0; i < SUBS_MAX; i++) {
            const event_sub_t *s = &snapshot[i];
            if (!s->active || !s->handler) continue;
            if (s->domain_mask == 0 || (s->domain_mask & (1u << EVT_DOMAIN(evt.type)))) {
                s->handler(&evt, s->user);
            }
        }
    }
}

hal_err_t event_bus_init(uint32_t queue_depth)
{
    if (g.inited) return HAL_ESTATE;
    if (queue_depth < 8) queue_depth = 8;
    memset(&g, 0, sizeof(g));
    g.queue = (event_t *)calloc(queue_depth, sizeof(event_t));
    if (!g.queue) return HAL_ENOMEM;
    g.cap = queue_depth;
    g.mu = os_mutex_create();
    g.cv = os_cond_create();
    if (!g.mu || !g.cv) return HAL_ENOMEM;
    g.inited = true;
    g.thread = os_thread_create(deliver_thread, NULL, "evt", 64);
    if (!g.thread) { g.inited = false; return HAL_EIO; }
    return HAL_OK;
}

hal_err_t event_bus_deinit(void)
{
    if (!g.inited) return HAL_ESTATE;
    os_mutex_lock(g.mu);
    g.stop = true;
    os_cond_broadcast(g.cv);
    os_mutex_unlock(g.mu);
    os_thread_join(g.thread);
    os_cond_destroy(g.cv);
    os_mutex_destroy(g.mu);
    free(g.queue);
    memset(&g, 0, sizeof(g));
    return HAL_OK;
}

hal_err_t event_bus_subscribe(uint32_t domain_mask, event_handler_t handler, void *user, event_sub_t **sub)
{
    if (!g.inited) return HAL_ESTATE;
    if (!handler || !sub) return HAL_EINVAL;
    os_mutex_lock(g.mu);
    for (int i = 0; i < SUBS_MAX; i++) {
        if (!g.subs[i].active) {
            g.subs[i].domain_mask = domain_mask;
            g.subs[i].handler = handler;
            g.subs[i].user = user;
            g.subs[i].active = true;
            *sub = &g.subs[i];
            os_mutex_unlock(g.mu);
            return HAL_OK;
        }
    }
    os_mutex_unlock(g.mu);
    return HAL_ENOMEM;
}

hal_err_t event_bus_unsubscribe(event_sub_t *sub)
{
    if (!g.inited) return HAL_ESTATE;
    if (!sub || sub < g.subs || sub >= g.subs + SUBS_MAX) return HAL_EINVAL;
    os_mutex_lock(g.mu);
    memset(sub, 0, sizeof(*sub));
    os_mutex_unlock(g.mu);
    return HAL_OK;
}

hal_err_t event_bus_publish(const event_t *evt)
{
    if (!g.inited) return HAL_ESTATE;
    if (!evt) return HAL_EINVAL;
    os_mutex_lock(g.mu);
    if (g.count == g.cap) {
        g.dropped++;
        os_mutex_unlock(g.mu);
        return HAL_EAGAIN;
    }
    g.queue[g.tail] = *evt;
    if (g.queue[g.tail].ts_us == 0) g.queue[g.tail].ts_us = os_monotonic_us();
    g.tail = (g.tail + 1) % g.cap;
    g.count++;
    os_cond_signal(g.cv);
    os_mutex_unlock(g.mu);
    return HAL_OK;
}

hal_err_t event_bus_emit(uint32_t type, int ch)
{
    event_t e;
    memset(&e, 0, sizeof(e));
    e.type = type; e.ch = ch;
    return event_bus_publish(&e);
}

uint64_t event_bus_dropped(void) { return g.dropped; }
