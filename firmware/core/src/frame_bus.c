/**
 * @file frame_bus.c
 * @brief 帧总线：每通道一条，单生产者（HAL）+ 多消费者（引用计数共享帧），按需起停编码
 *
 * 模型：
 *  - 生产者线程从 HAL 取帧，包装为 shared_frame_t（refs = 收到它的消费者数）。
 *  - 每个消费者一条指针环；环满丢最旧（该帧 refs--，归零即归还 HAL）。
 *  - pull 把帧内容拷给调用者并把 priv 指向 shared_frame_t；release 减引用，
 *    最后一个 release 的线程把原始 HAL 帧归还 hal_video.release_frame。
 *  - 按需：首个订阅 -> hal start + request_idr；无消费者超过 idle_stop_ms -> hal stop。
 */
#include "core/frame_bus.h"
#include "core/os.h"
#include "core/log.h"
#include "core/event_bus.h"
#include "core/profile.h"
#include "hal/hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MOD "framebus"
#define BUS_MAX 3
#define SUB_MAX (BUS_MAX * 8)

typedef struct shared_frame {
    hal_frame_t f;          /**< HAL 原始帧（含 HAL 私有 priv），仅归还时使用 */
    uint32_t    refs;
} shared_frame_t;

typedef struct {
    shared_frame_t *slots[64];
    size_t head, tail, count, cap;
} ring_t;

struct frame_sub {
    frame_bus_t *bus;
    ring_t       ring;
    char         name[32];
    bool         active;
    uint64_t     dropped;
};

struct frame_bus {
    int             ch;
    bool            used;
    frame_bus_cfg_t cfg;
    os_mutex_t     *mu;
    os_cond_t      *cv;             /**< 数据到达 / 状态变化 */
    frame_sub_t    *subs[SUB_MAX];
    size_t          sub_count;
    bool            running;        /**< HAL 编码通道已 start */
    os_thread_t    *producer;
    bool            stop_producer;
    uint64_t        last_sub_us;
    /* 统计 */
    uint64_t        published;
    uint64_t        dropped_total;
    uint64_t        win_start_us, win_bytes;
    uint32_t        win_frames, cur_kbps, cur_fps;
};

static struct frame_bus g_buses[BUS_MAX];

/* ---------------- ring（调用者持锁） ---------------- */

static bool ring_push(ring_t *r, shared_frame_t *item)
{
    if (r->count == r->cap) return false;
    r->slots[r->tail] = item;
    r->tail = (r->tail + 1) % r->cap;
    r->count++;
    return true;
}
static bool ring_pop(ring_t *r, shared_frame_t **out)
{
    if (r->count == 0) return false;
    *out = r->slots[r->head];
    r->head = (r->head + 1) % r->cap;
    r->count--;
    return true;
}

static void deref_collect(shared_frame_t *sf, shared_frame_t **pend, size_t *np, size_t max)
{
    if (sf->refs == 0) return;
    if (--sf->refs == 0) {
        if (*np < max) pend[(*np)++] = sf;      /* 在锁外归还 HAL，避免回调死锁 */
        else { hal()->video->release_frame(&sf->f); free(sf); }
    }
}

#define PEND_MAX 16

/* ---------------- 生产者 ---------------- */

static void producer_thread(void *arg)
{
    frame_bus_t *b = (frame_bus_t *)arg;
    const hal_video_ops_t *v = hal()->video;

    for (;;) {
        hal_frame_t f;
        shared_frame_t *sf;
        hal_err_t rc;
        size_t delivered = 0;
        bool idle_stop = false;
        shared_frame_t *pend[PEND_MAX];
        size_t npend = 0;

        os_mutex_lock(b->mu);
        if (b->stop_producer) { os_mutex_unlock(b->mu); return; }
        if (b->sub_count == 0) {
            uint64_t idle = os_monotonic_us() - b->last_sub_us;
            if (b->running && b->cfg.start_on_demand &&
                idle > (uint64_t)b->cfg.idle_stop_ms * 1000ULL) {
                /* 在锁内停止通道：与 subscribe 的锁内 start 互斥，避免竞态 */
                v->stop(b->ch);
                b->running = false;
                idle_stop = true;
            }
            os_mutex_unlock(b->mu);
            if (idle_stop) {
                event_bus_emit(EVT_STREAM_STOPPED, b->ch);
                LOGD(MOD, "ch%d idle-stopped", b->ch);
            }
            os_sleep_ms(5);
            continue;
        }
        if (!b->running) { os_mutex_unlock(b->mu); os_sleep_ms(5); continue; }
        os_mutex_unlock(b->mu);

        rc = v->get_frame(b->ch, &f, 100);
        if (rc != HAL_OK) {
            if (rc == HAL_EBUSY) {               /* HAL 缓冲耗尽：所有消费者环都满了，退避避免热转 */
                LOGT(MOD, "ch%d HAL buffers busy", b->ch);
                os_sleep_ms(2);
            }
            continue;
        }

        sf = (shared_frame_t *)calloc(1, sizeof(*sf));
        if (!sf) { v->release_frame(&f); continue; }
        sf->f = f;

        os_mutex_lock(b->mu);
        for (size_t s = 0; s < b->sub_count; s++) {
            frame_sub_t *sub = b->subs[s];
            if (!sub->active) continue;
            sf->refs++;
            if (!ring_push(&sub->ring, sf)) {
                shared_frame_t *victim;
                ring_pop(&sub->ring, &victim);      /* 丢最旧，腾出一格 */
                deref_collect(victim, pend, &npend, PEND_MAX);
                sub->dropped++;
                b->dropped_total++;
                ring_push(&sub->ring, sf);
            }
            delivered++;
        }
        if (delivered) {
            b->published++;
            b->win_bytes += f.size;
            b->win_frames++;
            {
                uint64_t now = os_monotonic_us();
                if (now - b->win_start_us >= 1000000ULL) {
                    b->cur_kbps = (uint32_t)(b->win_bytes * 8ULL / (now - b->win_start_us));
                    b->cur_fps = (uint32_t)((uint64_t)b->win_frames * 1000000ULL / (now - b->win_start_us));
                    b->win_start_us = now; b->win_bytes = 0; b->win_frames = 0;
                }
            }
            os_cond_broadcast(b->cv);
        }
        os_mutex_unlock(b->mu);
        for (size_t i = 0; i < npend; i++) { v->release_frame(&pend[i]->f); free(pend[i]); }
        if (!delivered) { free(sf); v->release_frame(&f); }
    }
}

/* ---------------- 生命周期 ---------------- */

hal_err_t frame_bus_create(int ch, const frame_bus_cfg_t *cfg, frame_bus_t **out)
{
    frame_bus_t *b;
    if (ch < 0 || ch >= BUS_MAX || !cfg || !out) return HAL_EINVAL;
    b = &g_buses[ch];
    if (b->used) return HAL_EBUSY;
    memset(b, 0, sizeof(*b));
    b->ch = ch;
    b->cfg = *cfg;
    if (b->cfg.max_consumers == 0) {
        const profile_t *p = profile_get();
        b->cfg.max_consumers = p ? p->frame_bus_consumers : 4;
    }
    /* 平台帧池必须容纳 Σ(消费者数 × 环深) + 在途帧，否则慢消费者会耗尽 HAL 缓冲。
       此处按 HAL 报告的 max_held_frames 收紧环深，保证总线不会饿死编码器。 */
    {
        hal_video_caps_t caps;
        uint32_t held = 8;
        if (hal_has(HAL_MOD_VIDEO) && hal()->video->get_caps(&caps) == HAL_OK && caps.max_held_frames) {
            held = caps.max_held_frames;
        }
        uint32_t budget = held / b->cfg.max_consumers;
        if (budget < 2) budget = 2;
        if (b->cfg.queue_depth == 0) b->cfg.queue_depth = budget < 8 ? budget : 8;
        if (b->cfg.queue_depth > budget) b->cfg.queue_depth = budget;
        if (b->cfg.queue_depth > 64) b->cfg.queue_depth = 64;
        if (b->cfg.queue_depth < 2) b->cfg.queue_depth = 2;
    }
    b->mu = os_mutex_create();
    b->cv = os_cond_create();
    if (!b->mu || !b->cv) {
        if (b->mu) os_mutex_destroy(b->mu);
        if (b->cv) os_cond_destroy(b->cv);
        memset(b, 0, sizeof(*b));
        return HAL_ENOMEM;
    }
    b->used = true;
    b->win_start_us = os_monotonic_us();
    *out = b;
    return HAL_OK;
}

hal_err_t frame_bus_destroy(frame_bus_t *b)
{
    if (!b || !b->used) return HAL_EINVAL;
    os_mutex_lock(b->mu);
    if (b->sub_count > 0) { os_mutex_unlock(b->mu); return HAL_EBUSY; }
    b->stop_producer = true;
    os_cond_broadcast(b->cv);
    os_mutex_unlock(b->mu);
    if (b->producer) { os_thread_join(b->producer); b->producer = NULL; }
    if (b->running) { hal()->video->stop(b->ch); b->running = false; }
    os_cond_destroy(b->cv);
    os_mutex_destroy(b->mu);
    b->used = false;
    return HAL_OK;
}

frame_bus_t *frame_bus_get(int ch)
{
    return (ch >= 0 && ch < BUS_MAX && g_buses[ch].used) ? &g_buses[ch] : NULL;
}

/* ---------------- 订阅 ---------------- */

hal_err_t frame_bus_subscribe(frame_bus_t *b, const char *name, bool want_idr_first, frame_sub_t **out)
{
    frame_sub_t *sub;
    bool start_now;

    if (!b || !b->used || !out) return HAL_EINVAL;
    sub = (frame_sub_t *)calloc(1, sizeof(*sub));
    if (!sub) return HAL_ENOMEM;
    sub->bus = b;
    sub->active = true;
    sub->ring.cap = b->cfg.queue_depth;
    strncpy(sub->name, name ? name : "sub", sizeof(sub->name) - 1);

    os_mutex_lock(b->mu);
    if (b->sub_count >= b->cfg.max_consumers) {
        os_mutex_unlock(b->mu);
        free(sub);
        event_bus_emit(EVT_STREAM_LIMIT, b->ch);
        return HAL_EBUSY;
    }
    b->subs[b->sub_count++] = sub;
    b->last_sub_us = os_monotonic_us();
    start_now = !b->running;
    if (start_now) b->running = true;
    if (!b->producer) {
        b->producer = os_thread_create(producer_thread, b, "frbus", 96);
        if (!b->producer) {
            b->sub_count--;
            b->running = false;
            os_mutex_unlock(b->mu);
            free(sub);
            return HAL_EIO;
        }
    }
    os_mutex_unlock(b->mu);

    if (start_now) {
        hal_err_t rc = hal()->video->start(b->ch);
        if (rc != HAL_OK && rc != HAL_EBUSY) {
            LOGE(MOD, "ch%d start failed: %s", b->ch, hal_strerror(rc));
        }
        if (want_idr_first) hal()->video->request_idr(b->ch);
        event_bus_emit(EVT_STREAM_STARTED, b->ch);
    }
    event_bus_emit(EVT_STREAM_CONSUMER_ADDED, b->ch);
    *out = sub;
    return HAL_OK;
}

hal_err_t frame_bus_unsubscribe(frame_sub_t *sub)
{
    frame_bus_t *b;
    size_t idx = (size_t)-1;
    shared_frame_t *pend[64];
    size_t npend = 0;

    if (!sub) return HAL_EINVAL;
    b = sub->bus;
    os_mutex_lock(b->mu);
    for (size_t i = 0; i < b->sub_count; i++) if (b->subs[i] == sub) { idx = i; break; }
    if (idx == (size_t)-1) { os_mutex_unlock(b->mu); return HAL_EINVAL; }
    {
        shared_frame_t *sf;
        while (ring_pop(&sub->ring, &sf)) deref_collect(sf, pend, &npend, 64);
    }
    sub->active = false;
    memmove(&b->subs[idx], &b->subs[idx + 1], (b->sub_count - idx - 1) * sizeof(b->subs[0]));
    b->sub_count--;
    b->last_sub_us = os_monotonic_us();
    /* 停止编码通道统一由生产者处理（idle_stop_ms 到期），保证事件与状态一致 */
    os_cond_broadcast(b->cv);
    os_mutex_unlock(b->mu);

    for (size_t i = 0; i < npend; i++) {
        hal()->video->release_frame(&pend[i]->f);
        free(pend[i]);
    }
    free(sub);
    event_bus_emit(EVT_STREAM_CONSUMER_GONE, b->ch);
    return HAL_OK;
}

/* ---------------- 取帧 / 归还 ---------------- */

hal_err_t frame_bus_pull(frame_sub_t *sub, hal_frame_t *frame, uint32_t timeout_ms)
{
    frame_bus_t *b;
    shared_frame_t *sf;
    uint64_t deadline;
    bool got = false;

    if (!sub || !frame) return HAL_EINVAL;
    b = sub->bus;
    deadline = os_monotonic_us() + (uint64_t)timeout_ms * 1000ULL;

    os_mutex_lock(b->mu);
    for (;;) {
        got = ring_pop(&sub->ring, &sf);
        if (got) break;
        if (b->stop_producer || !sub->active) { os_mutex_unlock(b->mu); return HAL_ESTATE; }
        {
            uint64_t now = os_monotonic_us();
            if (now >= deadline) { os_mutex_unlock(b->mu); return HAL_ETIMEOUT; }
            os_cond_wait(b->cv, b->mu, (uint32_t)((deadline - now) / 1000ULL) + 1);
        }
    }
    os_mutex_unlock(b->mu);

    *frame = sf->f;                    /* data/size/pts/flags 等对外可见 */
    frame->priv = sf;                  /* release 用 */
    return HAL_OK;
}

hal_err_t frame_bus_release(frame_sub_t *sub, hal_frame_t *frame)
{
    shared_frame_t *sf;
    frame_bus_t *b;
    bool last = false;

    if (!sub || !frame || !frame->priv) return HAL_EINVAL;
    sf = (shared_frame_t *)frame->priv;
    frame->priv = NULL;
    b = sub->bus;
    os_mutex_lock(b->mu);
    if (sf->refs > 0 && --sf->refs == 0) last = true;
    os_cond_broadcast(b->cv);
    os_mutex_unlock(b->mu);
    if (last) {
        hal()->video->release_frame(&sf->f);      /* 锁外归还 */
        free(sf);
    }
    return HAL_OK;
}

hal_err_t frame_bus_request_idr(frame_bus_t *b)
{
    if (!b || !b->used) return HAL_EINVAL;
    return b->running ? hal()->video->request_idr(b->ch) : HAL_ESTATE;
}

hal_err_t frame_bus_stats(frame_bus_t *b, frame_bus_stats_t *st)
{
    if (!b || !b->used || !st) return HAL_EINVAL;
    os_mutex_lock(b->mu);
    memset(st, 0, sizeof(*st));
    st->consumers = (uint32_t)b->sub_count;
    st->frames_published = b->published;
    st->frames_dropped = b->dropped_total;
    st->bitrate_kbps = b->cur_kbps;
    st->fps = b->cur_fps;
    st->running = b->running;
    os_mutex_unlock(b->mu);
    return HAL_OK;
}

hal_err_t frame_bus_sub_dropped(frame_sub_t *sub, uint64_t *dropped)
{
    if (!sub || !dropped) return HAL_EINVAL;
    *dropped = sub->dropped;
    return HAL_OK;
}
