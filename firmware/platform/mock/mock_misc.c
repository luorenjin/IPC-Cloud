/**
 * @file mock_misc.c
 * @brief mock 平台 —— 音频、OSD、IVS、GPIO、网络、存储 的最小实现
 */
#include "hal/hal.h"
#include "mock_os.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ======================= 音频 ======================= */

static bool g_cap_open, g_play_open;
static hal_audio_cfg_t g_acfg;
static uint64_t g_a_next_pts;
static uint8_t g_abuf[4096];
static bool g_abuf_busy;

static hal_err_t a_get_caps(hal_audio_caps_t *c)
{
    if (!c) return HAL_EINVAL;
    memset(c, 0, sizeof(*c));
    c->capture = true; c->playback = true;
    c->inputs_mask = (1u << HAL_AUDIO_IN_MIC);
    c->codecs_mask = (1u << HAL_CODEC_G711A) | (1u << HAL_CODEC_PCM_S16LE);
    c->aec = false; c->agc = true; c->ns = false;
    return HAL_OK;
}
static hal_err_t a_cap_open(const hal_audio_cfg_t *cfg)
{
    if (!cfg) return HAL_EINVAL;
    if (g_cap_open) return HAL_EBUSY;
    if (cfg->sample_rate != 8000 && cfg->sample_rate != 16000) return HAL_ENOTSUP;
    if (cfg->codec != HAL_CODEC_G711A && cfg->codec != HAL_CODEC_PCM_S16LE) return HAL_ENOTSUP;
    g_acfg = *cfg; if (g_acfg.frame_ms == 0) g_acfg.frame_ms = 20;
    g_cap_open = true; g_a_next_pts = mock_os_monotonic_us(); g_abuf_busy = false;
    return HAL_OK;
}
static hal_err_t a_cap_read(hal_frame_t *f, uint32_t timeout_ms)
{
    uint64_t now, deadline; uint32_t samples, bytes;
    if (!f) return HAL_EINVAL;
    if (!g_cap_open) return HAL_ESTATE;
    if (g_abuf_busy) return HAL_EBUSY;
    now = mock_os_monotonic_us(); deadline = now + (uint64_t)timeout_ms * 1000ULL;
    while (now < g_a_next_pts) {
        if (now >= deadline) return HAL_ETIMEOUT;
        mock_os_sleep_ms(1); now = mock_os_monotonic_us();
    }
    samples = g_acfg.sample_rate * g_acfg.frame_ms / 1000;
    bytes = (g_acfg.codec == HAL_CODEC_PCM_S16LE) ? samples * 2 : samples;
    if (bytes > sizeof(g_abuf)) bytes = sizeof(g_abuf);
    memset(g_abuf, 0xD5, bytes); /* G.711A 静音 */
    memset(f, 0, sizeof(*f));
    f->ch = 0; f->codec = g_acfg.codec; f->pts_us = g_a_next_pts; f->data = g_abuf; f->size = bytes; f->shm_fd = -1; f->priv = &g_abuf_busy;
    g_abuf_busy = true;
    g_a_next_pts += (uint64_t)g_acfg.frame_ms * 1000ULL;
    return HAL_OK;
}
static hal_err_t a_cap_release(hal_frame_t *f) { if (!f || f->priv != &g_abuf_busy) return HAL_EINVAL; g_abuf_busy = false; f->priv = NULL; return HAL_OK; }
static hal_err_t a_cap_close(void) { if (!g_cap_open) return HAL_ESTATE; g_cap_open = false; return HAL_OK; }
static hal_err_t a_play_open(uint32_t sr, uint32_t ch) { if (g_play_open) return HAL_EBUSY; if (sr == 0 || ch == 0 || ch > 2) return HAL_EINVAL; g_play_open = true; return HAL_OK; }
static hal_err_t a_play_write(const int16_t *pcm, uint32_t samples, uint32_t to) { (void)to; if (!g_play_open) return HAL_ESTATE; if (!pcm || samples == 0) return HAL_EINVAL; return HAL_OK; }
static hal_err_t a_play_close(void) { if (!g_play_open) return HAL_ESTATE; g_play_open = false; return HAL_OK; }
static hal_err_t a_gain(uint32_t g) { return g <= 100 ? HAL_OK : HAL_EINVAL; }
static hal_err_t a_vol(uint32_t v) { return v <= 100 ? HAL_OK : HAL_EINVAL; }
static hal_err_t a_aec(bool e) { (void)e; return HAL_ENOTSUP; }
static hal_err_t a_agc(bool e) { (void)e; return HAL_OK; }
static hal_err_t a_ns(bool e) { (void)e; return HAL_ENOTSUP; }

const hal_audio_ops_t mock_audio_ops = {
    a_get_caps, a_cap_open, a_cap_read, a_cap_release, a_cap_close,
    a_play_open, a_play_write, a_play_close, a_gain, a_vol, a_aec, a_agc, a_ns
};

/* ======================= OSD ======================= */

#define OSD_MAX 8
typedef struct { bool used; int ch; hal_osd_cfg_t cfg; bool enabled; } osd_region_t;
static osd_region_t g_osd[OSD_MAX];

static hal_err_t o_caps(hal_osd_caps_t *c) { if (!c) return HAL_EINVAL; c->max_regions_per_channel = 4; c->bitmap = true; c->hw_text = false; return HAL_OK; }
static hal_err_t o_create(int ch, const hal_osd_cfg_t *cfg, int *id)
{
    int per_ch = 0;
    if (ch < 0 || ch > 2 || !cfg || !id) return HAL_EINVAL;
    for (int i = 0; i < OSD_MAX; i++) if (g_osd[i].used && g_osd[i].ch == ch) per_ch++;
    if (per_ch >= 4) return HAL_EBUSY;
    for (int i = 0; i < OSD_MAX; i++) {
        if (!g_osd[i].used) { g_osd[i].used = true; g_osd[i].ch = ch; g_osd[i].cfg = *cfg; g_osd[i].enabled = true; *id = i; return HAL_OK; }
    }
    return HAL_ENOMEM;
}
static hal_err_t o_text(int id, const char *t) { if (id < 0 || id >= OSD_MAX || !g_osd[id].used || !t) return HAL_EINVAL; strncpy(g_osd[id].cfg.text, t, HAL_OSD_TEXT_MAX - 1); return HAL_OK; }
static hal_err_t o_pos(int id, const hal_rect_t *p) { if (id < 0 || id >= OSD_MAX || !g_osd[id].used || !p) return HAL_EINVAL; g_osd[id].cfg.pos = *p; return HAL_OK; }
static hal_err_t o_enable(int id, bool e) { if (id < 0 || id >= OSD_MAX || !g_osd[id].used) return HAL_EINVAL; g_osd[id].enabled = e; return HAL_OK; }
static hal_err_t o_destroy(int id) { if (id < 0 || id >= OSD_MAX || !g_osd[id].used) return HAL_EINVAL; g_osd[id].used = false; return HAL_OK; }

const hal_osd_ops_t mock_osd_ops = { o_caps, o_create, o_text, o_pos, o_enable, o_destroy };

/* ======================= IVS ======================= */

static hal_ivs_cfg_t g_ivs[HAL_IVS_KIND_COUNT];
static uint64_t g_ivs_next_evt;

static hal_err_t i_caps(hal_ivs_caps_t *c) { if (!c) return HAL_EINVAL; memset(c, 0, sizeof(*c)); c->kinds_mask = (1u << HAL_IVS_MOTION); c->max_regions = 4; c->objects = false; strncpy(c->engine, "sw", HAL_NAME_MAX - 1); return HAL_OK; }
static hal_err_t i_cfg(hal_ivs_kind_t k, const hal_ivs_cfg_t *c)
{
    if (k >= HAL_IVS_KIND_COUNT || !c) return HAL_EINVAL;
    if (k != HAL_IVS_MOTION) return HAL_ENOTSUP;
    if (c->sensitivity > 100 || c->region_count > 4) return HAL_EINVAL;
    g_ivs[k] = *c;
    g_ivs_next_evt = mock_os_monotonic_us() + 2000000ULL;
    return HAL_OK;
}
static hal_err_t i_get(hal_ivs_kind_t k, hal_ivs_cfg_t *c) { if (k >= HAL_IVS_KIND_COUNT || !c) return HAL_EINVAL; *c = g_ivs[k]; return HAL_OK; }
static hal_err_t i_poll(hal_ivs_event_t *e, uint32_t to)
{
    uint64_t now, deadline;
    if (!e) return HAL_EINVAL;
    if (!g_ivs[HAL_IVS_MOTION].enable) { mock_os_sleep_ms(to > 50 ? 50 : to); return HAL_EAGAIN; }
    now = mock_os_monotonic_us(); deadline = now + (uint64_t)to * 1000ULL;
    while (now < g_ivs_next_evt) {
        if (now >= deadline) return HAL_EAGAIN;
        mock_os_sleep_ms(5); now = mock_os_monotonic_us();
    }
    memset(e, 0, sizeof(*e));
    e->kind = HAL_IVS_MOTION; e->ch = 1; e->ts_us = now; e->start = true;
    g_ivs_next_evt = now + 5000000ULL; /* 每 5s 一次模拟事件 */
    return HAL_OK;
}

const hal_ivs_ops_t mock_ivs_ops = { i_caps, i_cfg, i_get, i_poll };

/* ======================= GPIO ======================= */

static uint32_t g_mapped = (1u << HAL_PIN_IRCUT_A) | (1u << HAL_PIN_IRCUT_B) | (1u << HAL_PIN_IR_LED) | (1u << HAL_PIN_STATUS_LED) | (1u << HAL_PIN_RESET_KEY);
static bool g_level[HAL_PIN_COUNT];
static uint32_t g_duty[HAL_PIN_COUNT];

static hal_err_t g_mask(uint32_t *m) { if (!m) return HAL_EINVAL; *m = g_mapped; return HAL_OK; }
static hal_err_t g_set(hal_gpio_pin_t p, bool l) { if (p >= HAL_PIN_COUNT) return HAL_EINVAL; if (!(g_mapped & (1u << p))) return HAL_ENOTSUP; if (p == HAL_PIN_RESET_KEY) return HAL_EINVAL; g_level[p] = l; return HAL_OK; }
static hal_err_t g_get(hal_gpio_pin_t p, bool *l) { if (p >= HAL_PIN_COUNT || !l) return HAL_EINVAL; if (!(g_mapped & (1u << p))) return HAL_ENOTSUP; *l = g_level[p]; return HAL_OK; }
static hal_err_t g_pwm(hal_gpio_pin_t p, uint32_t d) { if (p >= HAL_PIN_COUNT || d > 100) return HAL_EINVAL; if (!(g_mapped & (1u << p))) return HAL_ENOTSUP; g_duty[p] = d; g_level[p] = d >= 50; return HAL_OK; }
static hal_err_t g_adc(hal_gpio_pin_t p, uint32_t *v) { (void)v; if (p >= HAL_PIN_COUNT) return HAL_EINVAL; return HAL_ENOTSUP; }
static hal_err_t g_wait(hal_key_event_t *e, uint32_t to) { if (!e) return HAL_EINVAL; mock_os_sleep_ms(to > 50 ? 50 : to); return HAL_EAGAIN; }

const hal_gpio_ops_t mock_gpio_ops = { g_mask, g_set, g_get, g_pwm, g_adc, g_wait };

/* ======================= 网络 ======================= */

static hal_err_t n_caps(hal_net_caps_t *c) { if (!c) return HAL_EINVAL; memset(c, 0, sizeof(*c)); c->eth = true; c->wifi = false; c->wifi_ap = true; return HAL_OK; }
static hal_err_t n_status(hal_netif_t t, hal_netif_status_t *s)
{
    if (!s) return HAL_EINVAL;
    if (t != HAL_NETIF_ETH) return HAL_ENOTSUP;
    memset(s, 0, sizeof(*s));
    s->type = t; strncpy(s->ifname, "eth0", HAL_IFNAME_MAX - 1); s->link = HAL_LINK_UP; s->speed_mbps = 100;
    s->mac[0] = 0x02; s->mac[1] = 0x00; s->mac[2] = 0x00; s->mac[3] = 0xCA; s->mac[4] = 0xFE; s->mac[5] = 0x01;
    return HAL_OK;
}
static hal_err_t n_mac(hal_netif_t t, uint8_t m[6]) { hal_netif_status_t s; hal_err_t rc = n_status(t, &s); if (rc) return rc; memcpy(m, s.mac, 6); return HAL_OK; }
static hal_err_t n_wpower(bool on) { (void)on; return HAL_ENOTSUP; }
static hal_err_t n_wscan(hal_wifi_ap_t *a, uint32_t mx, uint32_t *c, uint32_t to) { (void)a; (void)mx; (void)to; if (c) *c = 0; return HAL_ENOTSUP; }
static hal_err_t n_wconn(const char *s, const char *p, hal_wifi_sec_t sec) { (void)s; (void)p; (void)sec; return HAL_ENOTSUP; }
static hal_err_t n_wdisc(void) { return HAL_ENOTSUP; }

static bool s_ap_running;

static hal_err_t mock_wifi_ap_start(const char *ssid, const char *psk, uint8_t channel)
{
    size_t psk_len;
    if (!ssid || !psk) return HAL_EINVAL;
    if (ssid[0] == '\0') return HAL_EINVAL;
    psk_len = strlen(psk);
    if (psk_len < 8 || psk_len > 63) return HAL_EINVAL;   /* WPA2 约束 */
    if (channel > 13) return HAL_EINVAL;
    if (s_ap_running) return HAL_EBUSY;
    s_ap_running = true;
    return HAL_OK;
}

static hal_err_t mock_wifi_ap_stop(void)
{
    if (!s_ap_running) return HAL_ESTATE;
    s_ap_running = false;
    return HAL_OK;
}

static hal_err_t n_poll(hal_net_event_t *e, uint32_t to) { if (!e) return HAL_EINVAL; mock_os_sleep_ms(to > 50 ? 50 : to); return HAL_EAGAIN; }

const hal_net_ops_t mock_net_ops = { n_caps, n_status, n_mac, n_wpower, n_wscan, n_wconn, n_wdisc, mock_wifi_ap_start, mock_wifi_ap_stop, n_poll };

/* ======================= 存储 ======================= */

static bool g_mounted;

static hal_err_t st_stat(hal_storage_stat_t *s)
{
    if (!s) return HAL_EINVAL;
    memset(s, 0, sizeof(*s));
    s->present = true; s->mounted = g_mounted;
    strncpy(s->mount_path, "mock_state/tf", HAL_PATH_MAX - 1);
    s->fs = HAL_FS_EXFAT;
    s->total_bytes = 64ULL * 1024 * 1024 * 1024;
    s->free_bytes = 8ULL * 1024 * 1024 * 1024;
    s->health = HAL_STOR_HEALTH_OK;
    strncpy(s->cid, "MOCKCARD0001", HAL_NAME_MAX - 1);
    return HAL_OK;
}
static hal_err_t st_mount(void) { if (g_mounted) return HAL_ESTATE; g_mounted = true; return HAL_OK; }
static hal_err_t st_umount(void) { if (!g_mounted) return HAL_ESTATE; g_mounted = false; return HAL_OK; }
static hal_err_t st_format(hal_fs_t fs) { if (fs != HAL_FS_EXFAT && fs != HAL_FS_FAT32) return HAL_ENOTSUP; if (g_mounted) return HAL_EBUSY; return HAL_OK; }
static hal_err_t st_health(void) { return HAL_OK; }
static hal_err_t st_poll(hal_storage_event_t *e, uint32_t to) { if (!e) return HAL_EINVAL; mock_os_sleep_ms(to > 50 ? 50 : to); return HAL_EAGAIN; }

const hal_storage_ops_t mock_storage_ops = { st_stat, st_mount, st_umount, st_format, st_health, st_poll };
