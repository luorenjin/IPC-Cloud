/**
 * @file mock_video.c
 * @brief mock 平台 —— 合成视频帧生成器
 *
 * 生成符合 Annex-B 结构的伪 H.264/H.265 帧（起始码 + NAL 头 + 随机负载），
 * 按配置帧率与 GOP 输出关键帧/非关键帧，PTS 来自单调时钟。
 * 目的：让帧总线、RTSP/RTMP/PS 打包、录像索引在 x86 上无硬件运行；
 * 负载不可解码，不用于播放画质验证。
 */
#include "hal/hal.h"
#include "mock_os.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MOCK_CH 3
#define MOCK_MAX_FRAME (256 * 1024)
#define MOCK_BUFS 12           /* 每通道缓冲池：需容纳 Σ(各消费者环深)+在途帧 */

typedef struct {
    uint8_t data[MOCK_MAX_FRAME];
    bool    busy;
} mock_buf_t;

typedef struct {
    bool          configured;
    bool          running;
    hal_enc_cfg_t cfg;
    uint64_t      next_pts_us;
    uint32_t      frame_in_gop;
    uint32_t      seq;
    bool          idr_pending;
    mock_buf_t   *pool;          /* MOCK_BUFS 个槽，open 时分配 */
} mock_ch_t;

static mock_ch_t g_ch[MOCK_CH];
static bool g_open;
static hal_isp_mode_t g_isp = HAL_ISP_LINEAR;
static hal_image_t g_img = { 50, 50, 50, 50, 50, 0, 0, 50, 0 };
static hal_daynight_t g_dn = HAL_DAYNIGHT_AUTO;

static uint32_t bytes_per_frame(const hal_enc_cfg_t *c, bool key)
{
    uint32_t bpf = (c->bitrate_kbps * 1000u / 8u) / (c->fps ? c->fps : 1);
    if (key) {
        bpf *= 4; /* 关键帧放大，模拟真实分布 */
    }
    if (bpf < 64) bpf = 64;
    if (bpf > MOCK_MAX_FRAME - 32) bpf = MOCK_MAX_FRAME - 32;
    return bpf;
}

static uint32_t write_nal(uint8_t *p, hal_codec_t codec, bool key, bool config, uint32_t payload)
{
    uint32_t n = 0;
    const uint8_t sc[4] = { 0, 0, 0, 1 };
    memcpy(p + n, sc, 4); n += 4;
    if (codec == HAL_CODEC_H265) {
        /* HEVC NAL 头 2 字节：type<<1 */
        uint8_t type = config ? 32 /*VPS*/ : (key ? 19 /*IDR_W_RADL*/ : 1 /*TRAIL_R*/);
        p[n++] = (uint8_t)(type << 1);
        p[n++] = 0x01;
    } else {
        uint8_t type = config ? 7 /*SPS*/ : (key ? 5 : 1);
        p[n++] = (uint8_t)(0x60 | type);
    }
    for (uint32_t i = 0; i < payload; i++) {
        /* 避免出现 00 00 0x 起始码序列 */
        p[n++] = (uint8_t)(0x10 + (rand() % 0xE0));
    }
    return n;
}

static hal_err_t v_open(void)
{
    if (g_open) return HAL_ESTATE;
    memset(g_ch, 0, sizeof(g_ch));
    for (int i = 0; i < MOCK_CH; i++) {
        g_ch[i].pool = (mock_buf_t *)calloc(MOCK_BUFS, sizeof(mock_buf_t));
        if (!g_ch[i].pool) {
            while (--i >= 0) free(g_ch[i].pool);
            return HAL_ENOMEM;
        }
    }
    g_open = true;
    return HAL_OK;
}

static hal_err_t v_close(void)
{
    if (!g_open) return HAL_ESTATE;
    for (int i = 0; i < MOCK_CH; i++) {
        free(g_ch[i].pool);
        g_ch[i].pool = NULL;
    }
    g_open = false;
    return HAL_OK;
}

static hal_err_t v_probe_sensor(hal_sensor_info_t *info)
{
    if (!info) return HAL_EINVAL;
    memset(info, 0, sizeof(*info));
    strncpy(info->name, "mocksensor", HAL_NAME_MAX - 1);
    info->active.w = 1920; info->active.h = 1080;
    info->max_fps = 60;
    info->hdr_supported = true;
    info->i2c_id = 0x5A5A;
    return HAL_OK;
}

static hal_err_t v_get_caps(hal_video_caps_t *caps)
{
    if (!caps) return HAL_EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->channels = MOCK_CH;
    caps->max_size[0].w = 1920; caps->max_size[0].h = 1080; caps->max_fps[0] = 30;
    caps->max_size[1].w = 704;  caps->max_size[1].h = 576;  caps->max_fps[1] = 30;
    caps->max_size[2].w = 640;  caps->max_size[2].h = 360;  caps->max_fps[2] = 15;
    caps->codecs_mask = (1u << HAL_CODEC_H264) | (1u << HAL_CODEC_H265) | (1u << HAL_CODEC_MJPEG);
    caps->wdr = true; caps->hdr = true; caps->denoise_3d = true;
    caps->max_consumers = 8;
    caps->max_held_frames = MOCK_BUFS;
    caps->lens = HAL_LENS_MOTORIZED; caps->lens_af = true; caps->lens_zoom = true;
    return HAL_OK;
}

static hal_err_t v_set_encoder(int ch, const hal_enc_cfg_t *cfg)
{
    mock_ch_t *c;
    if (ch < 0 || ch >= MOCK_CH || !cfg) return HAL_EINVAL;
    if (cfg->codec != HAL_CODEC_H264 && cfg->codec != HAL_CODEC_H265 && cfg->codec != HAL_CODEC_MJPEG) return HAL_ENOTSUP;
    if (cfg->fps == 0 || cfg->fps > 120 || cfg->width < 64 || cfg->height < 64) return HAL_EINVAL;
    c = &g_ch[ch];
    if (c->running && (c->cfg.width != cfg->width || c->cfg.height != cfg->height || c->cfg.codec != cfg->codec)) {
        return HAL_EBUSY; /* 改分辨率/编码需先 stop */
    }
    c->cfg = *cfg;
    if (c->cfg.gop == 0) c->cfg.gop = c->cfg.fps * 2;
    c->configured = true;
    return HAL_OK;
}

static hal_err_t v_get_encoder(int ch, hal_enc_cfg_t *cfg)
{
    if (ch < 0 || ch >= MOCK_CH || !cfg) return HAL_EINVAL;
    if (!g_ch[ch].configured) return HAL_ESTATE;
    *cfg = g_ch[ch].cfg;
    return HAL_OK;
}

static hal_err_t v_start(int ch)
{
    mock_ch_t *c;
    if (ch < 0 || ch >= MOCK_CH) return HAL_EINVAL;
    c = &g_ch[ch];
    if (!c->configured) return HAL_ESTATE;
    if (c->running) return HAL_EBUSY;
    c->running = true;
    c->next_pts_us = mock_os_monotonic_us();
    c->frame_in_gop = 0;
    c->idr_pending = true;
    return HAL_OK;
}

static hal_err_t v_stop(int ch)
{
    if (ch < 0 || ch >= MOCK_CH) return HAL_EINVAL;
    if (!g_ch[ch].running) return HAL_ESTATE;
    g_ch[ch].running = false;
    return HAL_OK;
}

static hal_err_t v_request_idr(int ch)
{
    if (ch < 0 || ch >= MOCK_CH) return HAL_EINVAL;
    g_ch[ch].idr_pending = true;
    return HAL_OK;
}

static hal_err_t v_get_frame(int ch, hal_frame_t *f, uint32_t timeout_ms)
{
    mock_ch_t *c;
    mock_buf_t *slot = NULL;
    uint64_t now, deadline;
    bool key;
    uint32_t n = 0;

    if (ch < 0 || ch >= MOCK_CH || !f) return HAL_EINVAL;
    c = &g_ch[ch];
    if (!c->running) return HAL_ESTATE;

    now = mock_os_monotonic_us();
    deadline = now + (uint64_t)timeout_ms * 1000ULL;
    for (;;) {
        for (int i = 0; i < MOCK_BUFS; i++) {
            if (!c->pool[i].busy) { slot = &c->pool[i]; break; }
        }
        if (slot) break;
        /* 缓冲池耗尽：等 release 或超时（真实编码器通常内部排队） */
        if (now >= deadline) return HAL_EBUSY;
        mock_os_sleep_ms(1);
        now = mock_os_monotonic_us();
    }

    while (now < c->next_pts_us) {
        if (now >= deadline) return HAL_ETIMEOUT;
        {
            uint64_t wait = c->next_pts_us - now;
            if (deadline - now < wait) wait = deadline - now;
            mock_os_sleep_ms((uint32_t)(wait / 1000ULL) + 1);
        }
        now = mock_os_monotonic_us();
    }

    key = c->idr_pending || (c->frame_in_gop == 0);
    if (key) {
        n += write_nal(slot->data + n, c->cfg.codec, false, true, 24);  /* 参数集 */
    }
    n += write_nal(slot->data + n, c->cfg.codec, key, false, bytes_per_frame(&c->cfg, key));

    memset(f, 0, sizeof(*f));
    f->ch = ch;
    f->codec = c->cfg.codec;
    f->pts_us = c->next_pts_us;
    f->flags = key ? (HAL_FRAME_FLAG_KEY | HAL_FRAME_FLAG_CONFIG) : 0;
    f->data = slot->data;
    f->size = n;
    f->shm_fd = -1;
    f->seq = c->seq++;
    f->priv = slot;

    slot->busy = true;
    c->idr_pending = false;
    c->frame_in_gop = (c->frame_in_gop + 1) % c->cfg.gop;
    c->next_pts_us += 1000000ULL / c->cfg.fps;
    return HAL_OK;
}

static hal_err_t v_release_frame(hal_frame_t *f)
{
    mock_buf_t *slot;
    if (!f || !f->priv) return HAL_EINVAL;
    slot = (mock_buf_t *)f->priv;
    if (!slot->busy) return HAL_ESTATE;
    slot->busy = false;
    f->priv = NULL;
    return HAL_OK;
}

static uint8_t g_jpeg[4096];
static bool g_jpeg_busy;

static hal_err_t v_snapshot(int ch, uint32_t quality, hal_frame_t *f)
{
    if (ch < 0 || ch >= MOCK_CH || !f || quality > 100) return HAL_EINVAL;
    if (g_jpeg_busy) return HAL_EBUSY;
    /* 最小 JPEG 头/尾，负载伪造 */
    g_jpeg[0] = 0xFF; g_jpeg[1] = 0xD8; g_jpeg[2] = 0xFF; g_jpeg[3] = 0xE0;
    for (size_t i = 4; i < sizeof(g_jpeg) - 2; i++) g_jpeg[i] = (uint8_t)(rand() & 0xFF);
    g_jpeg[sizeof(g_jpeg) - 2] = 0xFF; g_jpeg[sizeof(g_jpeg) - 1] = 0xD9;
    memset(f, 0, sizeof(*f));
    f->ch = ch; f->codec = HAL_CODEC_MJPEG; f->pts_us = mock_os_monotonic_us();
    f->flags = HAL_FRAME_FLAG_KEY; f->data = g_jpeg; f->size = sizeof(g_jpeg); f->shm_fd = -1;
    f->priv = &g_jpeg_busy;
    g_jpeg_busy = true;
    return HAL_OK;
}

/* snapshot 帧也经 release_frame 归还：priv 指向 g_jpeg_busy 时特判 */
static hal_err_t v_release_any(hal_frame_t *f)
{
    if (f && f->priv == &g_jpeg_busy) {
        g_jpeg_busy = false;
        f->priv = NULL;
        return HAL_OK;
    }
    return v_release_frame(f);
}

static hal_err_t v_set_isp_mode(hal_isp_mode_t m) { if (m > HAL_ISP_HDR) return HAL_EINVAL; g_isp = m; return HAL_OK; }
static hal_err_t v_get_isp_mode(hal_isp_mode_t *m) { if (!m) return HAL_EINVAL; *m = g_isp; return HAL_OK; }

static hal_err_t v_set_image(const hal_image_t *img)
{
    if (!img) return HAL_EINVAL;
#define APPLY(fld, lo, hi) do { if (img->fld != -1) { if (img->fld < (lo) || img->fld > (hi)) return HAL_EINVAL; g_img.fld = img->fld; } } while (0)
    APPLY(brightness, 0, 100); APPLY(contrast, 0, 100); APPLY(saturation, 0, 100);
    APPLY(sharpness, 0, 100); APPLY(hue, 0, 100); APPLY(flip, 0, 1); APPLY(mirror, 0, 1);
    APPLY(denoise_3d, 0, 100); APPLY(backlight_comp, 0, 1);
#undef APPLY
    return HAL_OK;
}
static hal_err_t v_get_image(hal_image_t *img) { if (!img) return HAL_EINVAL; *img = g_img; return HAL_OK; }
static hal_err_t v_set_daynight(hal_daynight_t m) { if (m > HAL_DAYNIGHT_NIGHT) return HAL_EINVAL; g_dn = m; return HAL_OK; }
static hal_err_t v_get_daynight(hal_daynight_t *m, bool *night) { if (!m || !night) return HAL_EINVAL; *m = g_dn; *night = (g_dn == HAL_DAYNIGHT_NIGHT); return HAL_OK; }

static hal_err_t v_lens_focus(int dir, uint32_t speed) { (void)speed; if (dir < -1 || dir > 1) return HAL_EINVAL; return HAL_OK; }
static hal_err_t v_lens_zoom(int dir, uint32_t speed) { (void)speed; if (dir < -1 || dir > 1) return HAL_EINVAL; return HAL_OK; }
static hal_err_t v_lens_af(void) { return HAL_OK; }

const hal_video_ops_t mock_video_ops = {
    v_open, v_close, v_probe_sensor, v_get_caps,
    v_set_encoder, v_get_encoder, v_start, v_stop, v_request_idr,
    v_get_frame, v_release_any, v_snapshot,
    v_set_isp_mode, v_get_isp_mode, v_set_image, v_get_image, v_set_daynight, v_get_daynight,
    v_lens_focus, v_lens_zoom, v_lens_af
};
