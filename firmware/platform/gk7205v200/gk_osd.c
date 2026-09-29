/**
 * @file gk_osd.c
 * @brief GK7205V200 OSD 叠加 —— hal_osd_ops_t 实现（MPP RGN）
 *
 * 叠加路径：文本先软件渲染成 ARGB1555 位图，再交给 RGN(OVERLAY_RGN)，最后把
 * 同一个 RGN 挂到 VPSS 的主/子两个通道上——预览走子码流、抓图走子码流、
 * 录像走主码流，挂 VPSS 通道能让三者都带上 OSD（挂 VENC 就得逐通道配一遍）。
 *
 * 为什么自己带字库：SDK 不提供字形，RGN 只吃位图。本机控制台的 OSD 只用到
 * 数字、大写字母和 "-:. _/," 这几个符号（时间串与通道名都是 ASCII），所以
 * 内嵌一份 8×8 点阵即可，按固定 2 倍放大画成 16×16。未收录的字符（小写、
 * 汉字）用 '-' 顶替——宁可见到占位符，也不要静默丢字符让时间串看着缺一段。
 *
 * 配置与状态分离：控制台可能在视频还没打开时就配 OSD，此时 RGN 建不出来
 * （VPSS 组还不存在）。所以 create/update 一律先记进内存表并返回成功，
 * 视频 open 后由 gk_osd_on_video_open() 统一落实——否则用户"开着预览配 OSD"
 * 与"没开预览配 OSD"会得到两种结果。
 *
 * HAL_OSD_TEXT_TIME 由本文件每秒重画（hal_osd.h 把刷新责任放在 HAL 侧，
 * 避免 core 跨层高频调用）。
 */
#include "hal/hal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gk_api_region.h"
#include "gk_api_sys.h"
#include "gk_api_vpss.h"

/* 与 gk_video.c 保持一致：VI pipe / ISP pipe = 0，VPSS 组 = 0，
   VPSS 通道 0 = 主码流、1 = 子码流。 */
#define GK_OSD_PIPE 0
#define GK_OSD_GRP  0

#define GK_MAIN_W 1920
#define GK_MAIN_H 1080
#define GK_SUB_W  640
#define GK_SUB_H  360

#define GK_GLYPH_W 8
#define GK_GLYPH_H 8
#define GK_OSD_SCALE 2 /* 固定 2 倍 → 每个字符 16×16 */

#define GK_OSD_CHAR_W (GK_GLYPH_W * GK_OSD_SCALE)
#define GK_OSD_CHAR_H (GK_GLYPH_H * GK_OSD_SCALE)

/** 画布能放下的字符数：19 字符的时间串（"2026-09-28 15:30:00"）绰绰有余 */
#define GK_OSD_MAX_CHARS 32
#define GK_OSD_BUF_W (GK_OSD_CHAR_W * GK_OSD_MAX_CHARS) /* 512 */
#define GK_OSD_BUF_H (GK_OSD_CHAR_H)                    /* 32 */

#define GK_OSD_MAX_REGIONS 8
#define GK_OSD_MAX_PER_CHN 4 /**< 与 hal_osd_caps_t 上报值一致 */

/* ---------------------------------------------------------------- 字库 */

/** 8×8 点阵表覆盖的字符集（顺序必须与 GK_FONT 一一对应） */
static const char GK_CHARSET[] =
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "-:. _/,";

/** 每字符 8 字节，每字节一行、MSB 在左 */
static const uint8_t GK_FONT[][GK_GLYPH_H] = {
    /* 0 */ { 0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00 },
    /* 1 */ { 0x18, 0x18, 0x38, 0x18, 0x18, 0x18, 0x7E, 0x00 },
    /* 2 */ { 0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00 },
    /* 3 */ { 0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00 },
    /* 4 */ { 0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00 },
    /* 5 */ { 0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00 },
    /* 6 */ { 0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00 },
    /* 7 */ { 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00 },
    /* 8 */ { 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00 },
    /* 9 */ { 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00 },
    /* A */ { 0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00 },
    /* B */ { 0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00 },
    /* C */ { 0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00 },
    /* D */ { 0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00 },
    /* E */ { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00 },
    /* F */ { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00 },
    /* G */ { 0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3E, 0x00 },
    /* H */ { 0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00 },
    /* I */ { 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 },
    /* J */ { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00 },
    /* K */ { 0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00 },
    /* L */ { 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00 },
    /* M */ { 0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00 },
    /* N */ { 0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00 },
    /* O */ { 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 },
    /* P */ { 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00 },
    /* Q */ { 0x3C, 0x66, 0x66, 0x66, 0x6E, 0x6C, 0x36, 0x00 },
    /* R */ { 0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00 },
    /* S */ { 0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00 },
    /* T */ { 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 },
    /* U */ { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 },
    /* V */ { 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00 },
    /* W */ { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00 },
    /* X */ { 0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00 },
    /* Y */ { 0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00 },
    /* Z */ { 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00 },
    /* - */ { 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00 },
    /* : */ { 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00 },
    /* . */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00 },
    /*   */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* _ */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF },
    /* / */ { 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x00, 0x00 },
    /* , */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30 },
};

/* ---------------------------------------------------------------- 内部状态 */

typedef struct {
    bool          used;
    int           ch;      /**< HAL 视频通道（0=主，1=子） */
    bool          enabled; /**< HAL 层的开关 */
    bool          is_time; /**< kind == HAL_OSD_TEXT_TIME */
    bool          attached;/**< RGN 已创建并挂到通道上 */
    RGN_HANDLE    rgn;
    hal_osd_cfg_t cfg;
    char          last_text[HAL_OSD_TEXT_MAX]; /**< 上次画过的内容，避免每秒重画 */
    uint16_t      canvas[GK_OSD_BUF_W * GK_OSD_BUF_H];
} gk_osd_region_t;

static gk_osd_region_t  s_rgn[GK_OSD_MAX_REGIONS];
static pthread_t        s_tick_th;
static volatile bool    s_tick_run;
static bool             s_tick_started;
static bool             s_video_up;   /**< 视频通路是否已就绪（RGN 依赖 VPSS 组） */

/* ---------------------------------------------------------------- 渲染 */

/** ARGB8888 → ARGB1555。RGN 只有 1bit alpha，表达不了半透明，
 *  所以按阈值二值化：亮到一定程度就算"可见"，否则全透明。 */
static uint16_t argb_to_1555(uint32_t c)
{
    uint32_t a = (c >> 24) & 0xFFu, r = (c >> 16) & 0xFFu;
    uint32_t g = (c >> 8) & 0xFFu, b = c & 0xFFu;
    uint32_t on = (a >= 128u) ? 0x8000u : 0u;
    return (uint16_t)(on | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}

static const uint8_t *glyph_of(char c)
{
    const char *p = strchr(GK_CHARSET, c);
    if (!p) p = strchr(GK_CHARSET, '-');   /* 未收录字符用占位符，别静默丢 */
    if (!p) return NULL;
    return GK_FONT[(size_t)(p - GK_CHARSET)];
}

/** 把文本画到画布左上角（画布背景全透明），超出宽度的字符直接截断 */
static void osd_draw(uint16_t *canvas, int cw, int ch, const char *text, uint32_t argb)
{
    uint16_t fg = argb_to_1555(argb);
    memset(canvas, 0, (size_t)cw * (size_t)ch * sizeof(uint16_t)); /* 0 = 全透明 */

    int x = 0;
    for (const char *p = text; *p; p++) {
        if (x + GK_OSD_CHAR_W > cw) break;
        const uint8_t *g = glyph_of(*p);
        if (g) {
            for (int gy = 0; gy < GK_GLYPH_H; gy++) {
                for (int gx = 0; gx < GK_GLYPH_W; gx++) {
                    if (!(g[gy] & (0x80u >> gx))) continue;
                    /* 整数倍放大：一个字形像素铺成 SCALE×SCALE 的方块 */
                    for (int sy = 0; sy < GK_OSD_SCALE; sy++) {
                        for (int sx = 0; sx < GK_OSD_SCALE; sx++) {
                            int px = x + gx * GK_OSD_SCALE + sx;
                            int py = gy * GK_OSD_SCALE + sy;
                            if (px < cw && py < ch) canvas[py * cw + px] = fg;
                        }
                    }
                }
            }
        }
        x += GK_OSD_CHAR_W;
    }
}

/** 把画布推给 RGN（内部会拷进画布缓冲，所以 canvas 是普通内存即可） */
static void osd_push(gk_osd_region_t *r, const char *text)
{
    BITMAP_S bmp;
    GK_S32 rc;

    osd_draw(r->canvas, GK_OSD_BUF_W, GK_OSD_BUF_H, text,
             r->cfg.color_argb ? r->cfg.color_argb : 0xFFFFFFFFu);

    memset(&bmp, 0, sizeof(bmp));
    bmp.enPixelFormat = PIXEL_FORMAT_ARGB_1555;
    bmp.u32Width = GK_OSD_BUF_W;
    bmp.u32Height = GK_OSD_BUF_H;
    bmp.pData = r->canvas;
    rc = GK_API_RGN_SetBitMap(r->rgn, &bmp);
    if (rc != GK_SUCCESS) {
        fprintf(stderr, "[gk_osd] RGN_SetBitMap(0x%x) rc=0x%08x\n", r->rgn, rc);
        return;
    }
    snprintf(r->last_text, sizeof(r->last_text), "%s", text);
}

/* ---------------------------------------------------------------- RGN 生命周期 */

/** 把同一份位图挂到主/子两路编码器上。
 *
 * @note 本 SDK 的 RGN **不支持挂 VPSS 通道**：实测 `RGN_AttachToChn` 传
 * `MOD_ID_VPSS` 返回 `0xa0038008`（EN_ERR_NOT_SUPPORT）。所以改挂 VENC——
 * 挂 VENC 时坐标是**编码分辨率**，主/子各自换算。挂编码器的好处是预览、
 * 抓图、录像三条出口都经 VENC，都能带上 OSD。
 */
static int osd_attach_chns(gk_osd_region_t *r)
{
    static const struct { int venc_chn; int w, h; } CHNS[] = {
        { 0, GK_MAIN_W, GK_MAIN_H },   /* 主码流编码通道 */
        { 1, GK_SUB_W,  GK_SUB_H  },   /* 子码流编码通道 */
    };

    for (size_t i = 0; i < sizeof(CHNS) / sizeof(CHNS[0]); i++) {
        MPP_CHN_S chn;
        RGN_CHN_ATTR_S ca;
        GK_S32 rc;
        int x, y;

        /* 位置来自归一化坐标；窗口不能越界（越界部分有些版本会直接报错），
           所以贴着右/下边时往回缩到刚好放得下 */
        x = (int)(r->cfg.pos.x * (float)CHNS[i].w);
        y = (int)(r->cfg.pos.y * (float)CHNS[i].h);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > CHNS[i].w - GK_OSD_BUF_W) x = CHNS[i].w - GK_OSD_BUF_W;
        if (y > CHNS[i].h - GK_OSD_BUF_H) y = CHNS[i].h - GK_OSD_BUF_H;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        /* 坐标要 2 像素对齐：传奇数 y 会被 AttachToChn 判为 ILLEGAL_PARAM
           （实测 0xa0038003，而 y=324 的同一个区域就能挂上）。 */
        x &= ~1;
        y &= ~1;

        chn.enModId = MOD_ID_VENC;
        chn.s32DevId = 0;
        chn.s32ChnId = CHNS[i].venc_chn;

        memset(&ca, 0, sizeof(ca));
        ca.bShow = r->enabled ? GK_TRUE : GK_FALSE;
        ca.enType = OVERLAY_RGN;
        ca.unChnAttr.stOverlayChn.stPoint.s32X = x;
        ca.unChnAttr.stOverlayChn.stPoint.s32Y = y;
        /* alpha 量程是 0~128（128 = 完全不透明），不是 0~255：传 255 会被
           AttachToChn 判为 ILLEGAL_PARAM（实测 0xa0038003）。 */
        ca.unChnAttr.stOverlayChn.u32BgAlpha = 0;    /* 背景全透明，只留字 */
        ca.unChnAttr.stOverlayChn.u32FgAlpha = 128;  /* 前景不透明 */
        ca.unChnAttr.stOverlayChn.u32Layer = 0;

        rc = GK_API_RGN_AttachToChn(r->rgn, &chn, &ca);
        if (rc != GK_SUCCESS) {
            fprintf(stderr, "[gk_osd] RGN_AttachToChn(venc.%d) rc=0x%08x\n",
                    CHNS[i].venc_chn, rc);
            return -1;
        }
    }
    return 0;
}

static int osd_rgn_create(gk_osd_region_t *r)
{
    RGN_ATTR_S ra;
    GK_S32 rc;

    memset(&ra, 0, sizeof(ra));
    ra.enType = OVERLAY_RGN;
    ra.unAttr.stOverlay.enPixelFmt = PIXEL_FORMAT_ARGB_1555;
    ra.unAttr.stOverlay.stSize.u32Width = GK_OSD_BUF_W;
    ra.unAttr.stOverlay.stSize.u32Height = GK_OSD_BUF_H;
    ra.unAttr.stOverlay.u32BgColor = 0;  /* 背景色透明 */
    ra.unAttr.stOverlay.u32CanvasNum = 1;

    rc = GK_API_RGN_Create(r->rgn, &ra);
    if (rc != GK_SUCCESS) {
        fprintf(stderr, "[gk_osd] RGN_Create(0x%x) rc=0x%08x\n", r->rgn, rc);
        return -1;
    }
    if (osd_attach_chns(r) != 0) {
        GK_API_RGN_Destroy(r->rgn);
        return -1;
    }
    r->attached = true;
    return 0;
}

static void osd_rgn_destroy(gk_osd_region_t *r)
{
    /* Detach 失败（例如 VENC 通道已经拆了）不阻断：Destroy 会把残留一并清掉 */
    static const int CHNS[] = { 0, 1 };
    for (size_t i = 0; i < sizeof(CHNS) / sizeof(CHNS[0]); i++) {
        MPP_CHN_S chn;
        chn.enModId = MOD_ID_VENC;
        chn.s32DevId = 0;
        chn.s32ChnId = CHNS[i];
        GK_API_RGN_DetachFromChn(r->rgn, &chn);
    }
    GK_API_RGN_Destroy(r->rgn);
    r->attached = false;
}

/* ---------------------------------------------------------------- 时间刷新 */

static void *osd_tick_thread(void *arg)
{
    char buf[HAL_OSD_TEXT_MAX];
    (void)arg;

    while (s_tick_run) {
        time_t now = time(NULL);
        struct tm tmv;

        memset(&tmv, 0, sizeof(tmv));
        localtime_r(&now, &tmv);
        for (int i = 0; i < GK_OSD_MAX_REGIONS; i++) {
            gk_osd_region_t *r = &s_rgn[i];
            const char *fmt;
            if (!r->used || !r->enabled || !r->is_time || !r->attached) continue;
            /* text 是 strftime 格式串（见 hal_osd.h 对 HAL_OSD_TEXT_TIME 的说明） */
            fmt = r->cfg.text[0] ? r->cfg.text : "%Y-%m-%d %H:%M:%S";
            if (strftime(buf, sizeof(buf), fmt, &tmv) == 0) continue;
            if (strcmp(buf, r->last_text) == 0) continue;  /* 同一秒内不重画 */
            osd_push(r, buf);
        }
        /* 分片睡，停止时最多 100ms 就能退出，不用等满一秒 */
        for (int i = 0; i < 10 && s_tick_run; i++) usleep(100000);
    }
    return NULL;
}

static void osd_tick_start(void)
{
    pthread_attr_t attr;
    if (s_tick_started) return;
    s_tick_run = true;
    /* 栈显式给 1MB：板子 MemFree 只有几 MB，默认 8MB 栈会让 pthread_create
       直接失败（与本平台 ISP 线程踩的是同一个坑，实测过）。 */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    if (pthread_create(&s_tick_th, &attr, osd_tick_thread, NULL) != 0) {
        pthread_attr_destroy(&attr);
        s_tick_run = false;
        fprintf(stderr, "[gk_osd] 时间刷新线程创建失败（OSD 时间不会走）\n");
        return;
    }
    pthread_attr_destroy(&attr);
    s_tick_started = true;
}

static void osd_tick_stop(void)
{
    if (!s_tick_started) return;
    s_tick_run = false;
    pthread_join(s_tick_th, NULL);
    s_tick_started = false;
}

/* ---------------------------------------------------------------- 视频联动 */

/** 视频通路就绪后把所有已登记的区域真正建起来（由 gk_video.c 的 open 调用） */
void gk_osd_on_video_open(void)
{
    int built = 0;
    s_video_up = true;
    for (int i = 0; i < GK_OSD_MAX_REGIONS; i++) {
        gk_osd_region_t *r = &s_rgn[i];
        if (!r->used || r->attached) continue;
        if (osd_rgn_create(r) != 0) continue;
        osd_push(r, r->is_time ? "" : r->cfg.text);
        if (r->is_time) osd_tick_start();
        built++;
    }
    fprintf(stderr, "[gk_osd] 视频就绪，已落实 %d 个区域\n", built);
}

/** 视频通路拆除前清理 RGN（配置留在内存里，下次 open 再落实） */
void gk_osd_on_video_close(void)
{
    osd_tick_stop();
    for (int i = 0; i < GK_OSD_MAX_REGIONS; i++) {
        gk_osd_region_t *r = &s_rgn[i];
        if (!r->used || !r->attached) continue;
        osd_rgn_destroy(r);
        r->last_text[0] = '\0';
    }
    s_video_up = false;
}

/* ---------------------------------------------------------------- HAL 接口 */

static hal_err_t o_caps(hal_osd_caps_t *c)
{
    if (!c) return HAL_EINVAL;
    c->max_regions_per_channel = GK_OSD_MAX_PER_CHN;
    c->bitmap = false;   /* 位图接口未接（控制台只用文本） */
    c->hw_text = false;  /* 字形是软件渲染的，没有硬件字库 */
    return HAL_OK;
}

static int osd_find_free(void)
{
    for (int i = 0; i < GK_OSD_MAX_REGIONS; i++)
        if (!s_rgn[i].used) return i;
    return -1;
}

static hal_err_t o_create(int ch, const hal_osd_cfg_t *cfg, int *id)
{
    int per_ch = 0, idx;

    if (!cfg || !id) return HAL_EINVAL;
    if (ch < 0 || ch > 1) return HAL_EINVAL;   /* 本平台只有主/子两路 */
    if (cfg->kind == HAL_OSD_BITMAP) return HAL_ENOTSUP;  /* caps.bitmap=false */

    for (int i = 0; i < GK_OSD_MAX_REGIONS; i++)
        if (s_rgn[i].used && s_rgn[i].ch == ch) per_ch++;
    if (per_ch >= GK_OSD_MAX_PER_CHN) return HAL_EBUSY;

    idx = osd_find_free();
    if (idx < 0) return HAL_ENOMEM;

    gk_osd_region_t *r = &s_rgn[idx];
    memset(r, 0, sizeof(*r));
    r->used = true;
    r->ch = ch;
    r->enabled = true;
    r->is_time = (cfg->kind == HAL_OSD_TEXT_TIME);
    r->cfg = *cfg;
    r->cfg.text[HAL_OSD_TEXT_MAX - 1] = '\0';
    r->rgn = (RGN_HANDLE)idx;   /* 句柄直接用槽位号，销毁后可复用 */

    /* 视频还没起来就先只登记：RGN 依赖 VPSS 组，这时创建必然失败 */
    if (s_video_up) {
        if (osd_rgn_create(r) != 0) {
            r->used = false;
            return HAL_EIO;
        }
        osd_push(r, r->is_time ? "" : r->cfg.text);
        if (r->is_time) osd_tick_start();
    }
    *id = idx;
    return HAL_OK;
}

static gk_osd_region_t *osd_of(int id)
{
    if (id < 0 || id >= GK_OSD_MAX_REGIONS) return NULL;
    if (!s_rgn[id].used) return NULL;
    return &s_rgn[id];
}

static hal_err_t o_text(int id, const char *text)
{
    gk_osd_region_t *r = osd_of(id);
    if (!r || !text) return HAL_EINVAL;

    snprintf(r->cfg.text, sizeof(r->cfg.text), "%s", text);
    if (r->attached) osd_push(r, r->is_time ? "" : r->cfg.text);
    return HAL_OK;
}

static hal_err_t o_pos(int id, const hal_rect_t *pos)
{
    gk_osd_region_t *r = osd_of(id);
    if (!r || !pos) return HAL_EINVAL;
    if (pos->x < 0.0f || pos->x > 1.0f || pos->y < 0.0f || pos->y > 1.0f) return HAL_EINVAL;

    r->cfg.pos = *pos;
    /* 位置是挂载属性，改位置要重新 Attach（SetDisplayAttr 也可，但它要求
       通道已挂过；重新 Attach 覆盖同通道既有效又简单） */
    if (r->attached) {
        osd_rgn_destroy(r);
        if (osd_rgn_create(r) != 0) return HAL_EIO;
        osd_push(r, r->is_time ? "" : r->cfg.text);
    }
    return HAL_OK;
}

static hal_err_t o_enable(int id, bool enable)
{
    gk_osd_region_t *r = osd_of(id);
    if (!r) return HAL_EINVAL;

    r->enabled = enable;
    if (r->attached) {
        /* 用 bShow 开关比反复 Detach/Attach 稳：后者会让整块 OSD 闪一下 */
        static const int CHNS[] = { 0, 1 };
        for (size_t i = 0; i < sizeof(CHNS) / sizeof(CHNS[0]); i++) {
            MPP_CHN_S chn;
            RGN_CHN_ATTR_S ca;
            chn.enModId = MOD_ID_VENC;
            chn.s32DevId = 0;
            chn.s32ChnId = CHNS[i];
            if (GK_API_RGN_GetDisplayAttr(r->rgn, &chn, &ca) == GK_SUCCESS) {
                ca.bShow = enable ? GK_TRUE : GK_FALSE;
                GK_API_RGN_SetDisplayAttr(r->rgn, &chn, &ca);
            }
        }
    }
    return HAL_OK;
}

static hal_err_t o_destroy(int id)
{
    gk_osd_region_t *r = osd_of(id);
    if (!r) return HAL_EINVAL;
    if (r->attached) osd_rgn_destroy(r);
    memset(r, 0, sizeof(*r));
    return HAL_OK;
}

const hal_osd_ops_t gk_osd_ops = {
    o_caps,
    o_create,
    o_text,
    o_pos,
    o_enable,
    o_destroy,
};
