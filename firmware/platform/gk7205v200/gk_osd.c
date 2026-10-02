/**
 * @file gk_osd.c
 * @brief GK7205V200 OSD 叠加 —— hal_osd_ops_t 实现（MPP RGN）
 *
 * 叠加路径：文本先软件渲染成 ARGB1555 位图，再交给 RGN(OVERLAY_RGN)，最后把
 * 同一份位图挂到 **VENC 的主/子两个编码通道**上（本 SDK 的 RGN 挂不了 VPSS 通道：
 * 传 MOD_ID_VPSS 返回 EN_ERR_NOT_SUPPORT，实测见 osd_attach_chns 的注释）。
 * 挂编码通道的意义：**OSD 是烧进码流的**——预览、抓图（子码流）、录像（主码流）、
 * 平台经 ZLMediaKit 拉流，拿到的帧里**本来就有 OSD**，控制台与平台都不需要再合成
 * 一层文字（控制台抓图/录像只是直接复制解码后的画面像素，见 web/js/ui.js 的
 * snapFrom 与 views/shell.js 的 MediaRecorder 实现）。
 *
 * 为什么自己带字库：SDK 不提供字形，RGN 只吃位图。本机控制台的 OSD 用到数字、
 * 大小写字母、常用标点（时间串与通道名）以及汉字（时间串里的中文星期、通道名），
 * 所以内嵌两份点阵：ASCII 16×16 + 汉字 24×24（都由 gen_osd_font.py 生成），按整数倍放大铺进字符格。未收录的
 * 字符用 '-' 顶替——宁可见到占位符，也不要静默丢字符让名字看着缺一段。
 *
 * 配置与状态分离：控制台可能在视频还没打开时就配 OSD，此时 RGN 建不出来
 * （VPSS 组还不存在）。所以 create/update 一律先记进内存表并返回成功，
 * 视频 open 后由 gk_osd_on_video_open() 统一落实——否则用户"开着预览配 OSD"
 * 与"没开预览配 OSD"会得到两种结果。
 *
 * HAL_OSD_TEXT_TIME 由本文件每秒重画（hal_osd.h 把刷新责任放在 HAL 侧，
 * 避免 core 跨层高频调用）。
 *
 * 2026-09-29（控制台 OSD 页对齐实机）新增三项：
 *   - **字号真生效**：以前固定 2 倍（16×16 字符），font_px 被忽略；现在按
 *     font_px/8 取整数放大倍数（1–8 → 8…64 px），与实机档位一致；
 *   - **右对齐**（HAL_OSD_ALIGN_RIGHT）：实机「国标模式」下时间与通道名称右对齐，
 *     位置由「文字宽度 + 最小边距（字符格）」从右边框反算，每个编码通道各算一次；
 *   - **闪烁**：实机「显示效果：闪烁」，由时基线程每 500ms 翻转一次 bShow。
 * 画布改按需分配：大字号下固定预留会吃掉几 MB（板子 MemFree 只有几 MB）。
 *
 * 2026-09-29 同日第二轮（控制台 OSD「星期」不显示）在同一处补齐：
 *   - **中文星期**：实机勾「星期」后时间串变成 `2026-09-29 星期二 22:50:56`，
 *     我方原先只有 8×8 ASCII 字库、且 console 的时间格式串里没有星期，勾了没反应。
 *     现在 gk_osd 把时间格式串里的 `%a` 自己渲染成中文星期（板端 rootfs 没有
 *     locale 数据，strftime 的 %a 只会是英文），字库补 24×24 的「星期X」九个汉字；
 *   - **文本度量按字符格算**：宽度 = 各字符格宽之和（ASCII 格 = 8×倍数、
 *     汉字格 = 24×倍数），右对齐落点与实机同一口径。
 *
 * 2026-10-01（通道名要支持中文/繁体/英文，为将来多语言留口）：
 *   - **汉字字库换成生成的完整表**：原先只手写了「星期X」九个字，通道名里的汉字
 *     会全变 '-'。现在 gk_font_cjk.h 由 firmware/tools/gen_osd_font.py 生成，
 *     收录 GB2312 全集 + Big5 一级汉字共 9520 字（简体全覆盖、繁体常用字覆盖），
 *     字形取自 Noto Sans SC（OFL-1.1，见生成文件头部声明）；
 *   - **UTF-8 按码点解码**：原实现只认「3 字节且恰好等于表中某字」，现在按 UTF-8
 *     规则解出码点再二分查表，2/4 字节字符也不会被拆错；
 *   - **ASCII 表补小写与标点**：通道名允许英文，除大写外补了 a-z 与
 *     `()[]<>?!+=*#@&%'"|~;$`。
 *   字库数据 669KB（源文件 2.9MB），只影响 gk_osd.c 一个编译单元的编译时间。
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

/* ASCII 点阵：16×16，由 firmware/tools/gen_osd_font.py 生成（**勿手改**）：
 *   python firmware/tools/gen_osd_font.py
 * 为什么不再用手写 8×8：点阵是按**整数倍**放大铺进字符格的，8×8 在 64px 档要放大 8 倍，
 * 一个字形像素变成 8×8 的方块 → 数字看着像马赛克（2026-10-01 用户反馈“颗粒感很强、
 * 不圆润”）。16×16 在 64px 档只放大 4 倍，而且 16 能整除全部字号档位
 * （16/32/48/64 → 1/2/3/4 倍），**格宽与旧表完全一致**（64px 档仍是 64px），所以时间项
 * 预留宽度、右对齐、度量口径都不用动，只是细叀 4 倍。字形与汉字同源（Noto Sans SC，
 * SIL OFL-1.1，声明见生成文件头部）。 */
#include "gk_font_ascii.h"

#define GK_GLYPH_W 16                   /**< ASCII 格宽基准（= 最小档点阵边长） */
#define GK_GLYPH_H 16
/* 放大倍数上限：16×16 字形 ×4 = 64px（档位 自适应/16/32/48/64 → 1/2/3/4 倍）。
   汉字点阵的倍数另算（见 GK_CJK_CELL）。 */
#define GK_OSD_SCALE_MAX 8
/** 时间项的预留字符格数：`YYYY-MM-DD 星期X HH:MM:SS` = 23 格，留 1 格余量 */
#define GK_OSD_TIME_CELLS 24
/** 画布宽度上限（像素）：64px 档下最多 32 个 ASCII 字符格宽，更长的文本按字符截断 */
#define GK_OSD_BUF_W_MAX (64 * 32)

/* 区域槽位：OSD 最多 12（国标 8 字符 + 通道名 + 时间）+ 遮挡每矩形占主/子两路
   （每路一个 RGN：位图尺寸必须与 RGN 尺寸一致，主/子分辨率不同）→ 4×2 = 8；
   再留点余量，别让某一边把槽位吃干。 */
#define GK_OSD_MAX_REGIONS 24
/* 国标模式最多 8 条自定义字符 + 通道名 + 时间 = 10，留点余量 */
#define GK_OSD_MAX_PER_CHN 12 /**< 与 hal_osd_caps_t 上报值一致 */

/** 遮挡区域（HAL_OSD_COVER）的单个面积上限：**画面面积的 1/4**。
 *
 *  为什么要卡：遮挡是拿一块与矩形同尺寸的 **OVERLAY 不透明位图** 实现的
 *  （见 osd_rgn_create 里为什么不用 COVER_RGN），位图要常驻内存，
 *  一张 1920×1080 的满屏位图 4MB，板子拿不出来。宁可**拒绝**也不缩水：
 *  遮挡偷偷画小了就是隐私泄露，属于宁可报错不能骗人的那一类。
 *  拒绝时 create_region 返回 HAL_EINVAL，控制台会如实告知用户。 */
#define GK_OSD_COVER_AREA_DIV 4

/* ---------------------------------------------------------------- 字库 */

/* ASCII 表在文件头 include（gk_font_ascii.h）；汉字表在下面 include。 */


/* ---------------------------------------------------------------- 汉字字库 */

/* 汉字点阵由 firmware/tools/gen_osd_font.py 生成（**勿手改**）：
 *   python firmware/tools/gen_osd_font.py
 * 字形取 Noto Sans SC（SIL OFL-1.1，版权声明见生成文件头部）wght=600 / 24px
 * 二值化，收录 GB2312 全集 + Big5 一级汉字共 9520 字——简体全覆盖、繁体常用字
 * 覆盖，简繁混排共用一套度量。
 *
 * 想扩语言：改生成脚本的 --charset 重跑即可（日文假名/韩文/西里尔都是码点表 +
 * 点阵，C 侧一个字不用动）。真要上"多语言包"时再改成从 rootfs 读同一份数据，
 * 按语言分文件按需加载，免得整库常驻内存。
 *
 * 格宽：ASCII 16×16（放大 font_px/16 倍）、汉字 24×24（放大 font_px/24 倍），两者
 * 高度都折算到 ≈ font_px，所以中西文混排看着在一条线上。汉字点阵要用 24×24：
 * 8×8 根本画不出「星/期」的笔画（实测糊成一团），16×16 也只勉强认得出。
 *
 * 另有一张**常用字高分辨率表** gk_font_cjk_hi.h（默认 GB2312 一级 3755 字 ×48×48），
 * 在汉字格宽 ≥48 时优先使用：24×24 在自适应档（格宽 72）要放大 3 倍、笔画比数字
 * 粗一圈，用 48 表后 48 格档 1:1、72 格档只放大 1.5 倍；未收录的汉字自动回落全库。
 * ⚠️ 两张表**格宽口径完全一样**（都是 24 的就近整数倍，见 osd_cjk_cell），换表不会
 * 影响度量/预留/右对齐，前端 camera.js 的 devCell() 也不用改。
 */
#include "gk_font_cjk.h"
#include "gk_font_cjk_hi.h"


/* ---------------------------------------------------------------- 内部状态 */

typedef struct {
    bool          used;
    int           ch;      /**< HAL 视频通道（0=主，1=子） */
    bool          enabled; /**< HAL 层的开关 */
    bool          is_time; /**< kind == HAL_OSD_TEXT_TIME */
    bool          is_cover;/**< kind == HAL_OSD_COVER（遮挡矩形，走 COVER_RGN） */
    bool          attached;/**< RGN 已创建并挂到通道上 */
    RGN_HANDLE    rgn;
    hal_osd_cfg_t cfg;
    char          last_text[HAL_OSD_TEXT_MAX]; /**< 上次画过的内容，避免每秒重画 */
    /* 画布**按需分配**（不再固定 512×32）：64px 字号下一个 32 字符的文本就要
       2048×64 像素 = 256KB，16 个区域各留一块会吃掉好几 MB，板子没有这么多余量。 */
    uint16_t     *canvas;
    int           cw, chh;              /**< 当前画布尺寸（像素） */
    int           rgn_w, rgn_h;         /**< RGN 已创建的尺寸（必须与位图一致） */
    int           text_w, text_h;       /**< 当前文本的实测像素尺寸（按字符格算） */
    int           last_x[2], last_y[2]; /**< 各编码通道上次的挂载点（变了要重挂） */
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

/** 字号 → 整数放大倍数（点阵放大后即字符格的像素尺寸） */
static int gk_scale_of(uint32_t font_px, int cell)
{
    int s = (int)((font_px + (uint32_t)cell / 2u) / (uint32_t)cell);   /* 就近取整 */
    if (s < 1) s = 1;
    return s;
}

/** ASCII 字符格的**目标像素宽度**：16 的就近整数倍（档位 16/32/48/64 → 1/2/3/4 倍）。
 *  与前端 overlay 的 devCell() 是同一口径（改一处必须改另一处）。 */
static int osd_ascii_cell(uint32_t font_px)
{
    int s = gk_scale_of(font_px, GK_GLYPH_W);
    if (s > GK_OSD_SCALE_MAX) s = GK_OSD_SCALE_MAX;
    return GK_GLYPH_W * s;
}

/** 汉字字符格的目标像素宽度：24 的就近整数倍、至少 24。 */
static int osd_cjk_cell(uint32_t font_px)
{
    return GK_CJK_CELL * gk_scale_of(font_px, GK_CJK_CELL);
}

/** 一个字符格：点阵 + 放大倍数（格宽 = 点阵宽 × 倍数） */
typedef struct {
    const uint8_t *bits;   /**< 每行 w/8 字节，MSB 在左；NULL = 无字形（画空格） */
    int            w, h;   /**< 点阵边长（ASCII 按格宽选 16/32/48/64 一档；汉字 24） */
    int            tw, th; /**< 目标格宽高（像素）：画进画布时的实际尺寸 */
} gk_glyph_t;

/** 取一个 UTF-8 字符的码点并把 p 前移。非法/截断字节按单字节步进，
 *  绝不会越界读（撞上 '\0' 也只会前进一格然后由调用方的 while 收尾）。 */
static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp;
    size_t   n;

    if (s[0] < 0x80u) { *p = (const char *)s + 1; return s[0]; }
    if ((s[0] & 0xE0u) == 0xC0u) { n = 2; cp = s[0] & 0x1Fu; }
    else if ((s[0] & 0xF0u) == 0xE0u) { n = 3; cp = s[0] & 0x0Fu; }
    else if ((s[0] & 0xF8u) == 0xF0u) { n = 4; cp = s[0] & 0x07u; }
    else { *p = (const char *)s + 1; return 0xFFFDu; }      /* 非法起始字节 */

    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0u) != 0x80u) { *p = (const char *)s + 1; return 0xFFFDu; }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    *p = (const char *)s + n;
    return cp;
}

/** 取 UTF-8 串当前的字符格并把 p 前移到下一个字符。
 *
 *  收录范围：ASCII 表（数字/大小写字母/常用符号）+ 生成字库（GB2312 全集与 Big5
 *  一级汉字共 9520 字，见 gk_font_cjk.h；其中常用字还有 48×48 高分辨率表，见
 *  gk_font_cjk_hi.h），简繁中英混排都能显示。
 *  未收录的（生僻字、emoji、日韩文…）**整字占一格**、用 '-' 顶替，不拆成三个
 *  占位符（否则一个汉字显示成 "---"），也不静默丢字符。
 */
static gk_glyph_t gk_next_glyph(const char **p, uint32_t font_px)
{
    gk_glyph_t g;
    uint32_t cp = utf8_next(p);
    int cell = osd_ascii_cell(font_px);     /* 目标格宽：ASCII 走 16 的倍数 */
    const uint8_t *bits;

    memset(&g, 0, sizeof(g));
    if (cp >= 0x80u) {
        int cjk = osd_cjk_cell(font_px);        /* 汉字格宽另算（24 的倍数） */

        /* 常用字（GB2312 一级）有 48×48 高分辨率表：格宽 ≥48 时优先用它，
           24 格档没必要（48 字形缩到 24 反而糊）。
           注意：**只换点阵，不改格宽**（下面 tw/th 仍用 cjk）。 */
        if (cjk >= GK_CJK_HI_CELL) {
            bits = gk_cjk_hi_lookup(cp);
            if (bits) {
                g.bits = bits;
                g.w = GK_CJK_HI_CELL;
                g.h = GK_CJK_HI_CELL;
                g.tw = cjk;
                g.th = cjk;
                return g;
            }
        }
        bits = gk_cjk_lookup(cp);
        if (bits) {
            g.bits = bits;
            g.w = GK_CJK_CELL;
            g.h = GK_CJK_CELL;
            g.tw = cjk;
            g.th = cjk;
            return g;
        }
        bits = gk_ascii_lookup('-', cell);  /* 生僻字/emoji：占一格占位符 */
    } else {
        /* ASCII 表盖 0x20–0x7E；控制字符等未收录的返回 NULL，osd_draw 会跳过（留白） */
        bits = gk_ascii_lookup(cp, cell);
    }
    g.bits = bits;
    g.tw = cell;
    g.th = cell;
    if (bits) {
        /* 点阵边长就是选中档的边长（16/32/48/64）；与格宽相等时 osd_draw 就是 1:1 铺格 */
        g.w = gk_ascii_size(cell);
        g.h = g.w;
    }
    return g;
}

/** 文本的实测像素尺寸：宽 = 各字符格宽之和，高 = 最高的那个字符格。
 *  右对齐落点、画布尺寸、RGN 尺寸三处都用它，口径必须一致。 */
static void gk_measure(const char *text, uint32_t font_px, int *out_w, int *out_h)
{
    int w = 0, h = 0;
    const char *p = text;

    while (*p) {
        gk_glyph_t g = gk_next_glyph(&p, font_px);
        w += g.tw;
        if (g.th > h) h = g.th;
    }
    if (h <= 0) h = osd_ascii_cell(font_px);
    *out_w = w;
    *out_h = h;
}

/* 本文件后面才定义，push 里要在布局变化时重挂，先声明 */
static int  osd_rgn_create(gk_osd_region_t *r);
static void osd_rgn_destroy(gk_osd_region_t *r);
static void osd_layout_point(const gk_osd_region_t *r, int ci, int *out_x, int *out_y);
static void osd_set_show(gk_osd_region_t *r, bool show);

/** 遮挡矩形（HAL_OSD_COVER）在某个编码通道上的像素矩形。
 *
 *  配置里存的是**画面比例**（hal_osd_config 的 pos.x/y/w/h），所以主/子两路
 *  各自按自己的分辨率换算——同一条配置在两个通道上落在**同一个画面位置**，
 *  而不是同一个像素坐标（与本文件 OSD 文本的处理一致）。
 *  夹到画面内 + 2 像素对齐：越界与奇数尺寸会被 RGN 判 ILLEGAL_PARAM。 */
static void osd_cover_rect(const gk_osd_region_t *r, int ci,
                           int *ox, int *oy, int *ow, int *oh)
{
    int fw = (ci == 0) ? GK_MAIN_W : GK_SUB_W;
    int fh = (ci == 0) ? GK_MAIN_H : GK_SUB_H;
    int x = (int)(r->cfg.pos.x * (float)fw);
    int y = (int)(r->cfg.pos.y * (float)fh);
    int w = (int)(r->cfg.pos.w * (float)fw + 0.5f);
    int h = (int)(r->cfg.pos.h * (float)fh + 0.5f);

    /* 退化矩形（长或宽为 0）没法覆盖：给最小 2 像素，与控制台侧“长或宽太小”
       的提示一致（实机同一句文案：区域长或宽的值过小） */
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > fw - w) x = fw - w;
    if (y > fh - h) y = fh - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    x &= ~1;
    y &= ~1;
    w &= ~1;
    h &= ~1;
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    *ox = x;
    *oy = y;
    *ow = w;
    *oh = h;
}

/** 把文本画到画布左上角（画布背景全透明），超出宽度的字符直接截断 */
static void osd_draw(uint16_t *canvas, int cw, int chh, const char *text, uint32_t font_px,
                     uint32_t argb)
{
    uint16_t fg = argb_to_1555(argb);
    const char *p = text;
    int x = 0;

    memset(canvas, 0, (size_t)cw * (size_t)chh * sizeof(uint16_t)); /* 0 = 全透明 */

    while (*p) {
        gk_glyph_t g = gk_next_glyph(&p, font_px);
        int adv = g.tw;
        if (x + adv > cw) break;
        if (g.bits && g.tw > 0 && g.th > 0) {
            int row_bytes = g.w / 8;
            /* 最近邻铺格：按档预渲染的点阵边长与格宽**相等**时就是 1:1（无缩放、无像素
               台阶）；只有“自适应”在非 1920 主码流上折算出的非常规字号才会落到缩放分支。 */
            for (int py = 0; py < g.th; py++) {
                const uint8_t *row = g.bits + (size_t)(py * g.h / g.th) * (size_t)row_bytes;
                for (int px = 0; px < g.tw; px++) {
                    int gx = px * g.w / g.tw;
                    if (!(row[gx / 8] & (0x80u >> (gx % 8)))) continue;
                    if (x + px < cw && py < chh) canvas[py * cw + x + px] = fg;
                }
            }
        }
        x += adv;
    }
}

/** 时间项的预留范围：24 个 ASCII 格宽（最长的 `YYYY-MM-DD 星期X HH:MM:SS` = 23 格）
 *  与两种字形的较大高度。按这个尺寸先建 RGN，视频起来后每秒刷新文本就不必重挂
 *  （尺寸一变就重挂，会让 OSD 每秒闪一次）。 */
static void osd_reserve_extent(const gk_osd_region_t *r, int *out_w, int *out_h)
{
    int aw = osd_ascii_cell(r->cfg.font_px);
    int ah = aw;
    int cw = osd_cjk_cell(r->cfg.font_px);

    if (aw > GK_OSD_BUF_W_MAX / GK_OSD_TIME_CELLS) aw = GK_OSD_BUF_W_MAX / GK_OSD_TIME_CELLS;
    *out_w = GK_OSD_TIME_CELLS * aw;
    *out_h = (cw > ah) ? cw : ah;
}

/** 按需要的尺寸取画布（**只增不减**）。
 *
 *  为什么要“只增不减”：RGN 的尺寸在建时就定死，必须与推上去的位图尺寸一致。文本
 *  长度一变就缩容 → 每变一次都要重建 RGN，OSD 会闪一下；时间项按预留范围建区、
 *  每秒刷新时文本都短于预留，于是永远不重建。
 *
 *  ⚠ 尺寸变化后必须**在重建 RGN 之后**再画（见 osd_push）：先画后变尺寸会让位图的
 *  行距与 RGN 的宽度对不上，画面上的 OSD 会糊成一条横streak（实测踩过）。*/
static uint16_t *osd_canvas_ensure(gk_osd_region_t *r, int w, int h)
{
    uint16_t *p;

    if (w < 1) w = 1;
    if (w > GK_OSD_BUF_W_MAX) w = GK_OSD_BUF_W_MAX;
    if (h < 1) h = 1;
    if (r->canvas && r->cw >= w && r->chh >= h) return r->canvas;
    if (r->canvas && r->cw > w) w = r->cw;      /* 宽度也别缩：RGN 宽度跟着画布走 */
    if (r->canvas && r->chh > h) h = r->chh;
    p = (uint16_t *)realloc(r->canvas, (size_t)w * (size_t)h * sizeof(uint16_t));
    if (!p) return NULL;
    r->canvas = p;
    r->cw = w;
    r->chh = h;
    return p;
}

/** 画布尺寸与 RGN 尺寸是否已经不一致（不一致就得重建 RGN，且重画） */
static bool osd_size_mismatch(const gk_osd_region_t *r)
{
    return r->attached && (r->cw != r->rgn_w || r->chh != r->rgn_h);
}

/** 把画布推给 RGN（内部会拷进画布缓冲，所以 canvas 是普通内存即可）
 *
 *  ⚠ 顺序不能动：**先定尺寸（必要时重建 RGN）→ 再画 → 最后推位图**。
 *  早先的实现是先画、后按新尺寸重建 RGN，结果位图按旧宽度排的行被当成新宽度读，
 *  画面上的 OSD 糊成一条横条（真机实测踩过，2026-09-29）。*/
static void osd_push(gk_osd_region_t *r, const char *text)
{
    /* 遮挡矩形没有文本也没有画布（矩形本身就是它的“内容”）：直接忽略。 */
    if (r->is_cover) return;
    BITMAP_S bmp;
    GK_S32 rc;
    int tw = 0, th = 0, x, y;
    uint16_t *cv;
    bool moved = false;

    /* 度量按字符格算（ASCII 16×16、汉字 24×24 各自的倍数），右对齐落点与画布尺寸
       都由它推出来。空文本（时间项刚建区、还没有第一秒的文本）保持建区时的预留
       尺寸——缩回去再涨回来会让 RGN 每秒重挂一次。 */
    if (text[0]) gk_measure(text, r->cfg.font_px, &tw, &th);
    if (tw < 1 || th < 1) osd_reserve_extent(r, &tw, &th);
    cv = osd_canvas_ensure(r, tw, th);
    if (!cv) {
        fprintf(stderr, "[gk_osd] 画布分配失败（%d×%d，font_px=%u）\n", tw, th, r->cfg.font_px);
        return;
    }
    r->text_w = tw;
    r->text_h = th;
    snprintf(r->last_text, sizeof(r->last_text), "%s", text);

    /* RGN 尺寸在建时定死，画布涨了就得重建（重建在本函数里也要带着新尺寸重画） */
    if (osd_size_mismatch(r)) moved = true;
    for (int i = 0; i < 2; i++) {
        osd_layout_point(r, i, &x, &y);
        if (x != r->last_x[i] || y != r->last_y[i]) moved = true;
    }
    if (moved && r->attached) {
        osd_rgn_destroy(r);
        if (osd_rgn_create(r) != 0) return;
    }
    cv = r->canvas;   /* 重建时画布可能被重新分配过，指针要重新取 */
    if (!cv) return;
    /* 尺寸定下来之后再画：此时 r->cw/chh 就是即将创建的 RGN 尺寸 */
    osd_draw(cv, r->cw, r->chh, text, r->cfg.font_px,
             r->cfg.color_argb ? r->cfg.color_argb : 0xFFFFFFFFu);
    /* 不闪烁的区域随手保证处于显示态（闪烁相位由时基线程接管） */
    if (r->attached && !r->cfg.flicker) osd_set_show(r, r->enabled);

    memset(&bmp, 0, sizeof(bmp));
    bmp.enPixelFormat = PIXEL_FORMAT_ARGB_1555;
    bmp.u32Width = (GK_U32)r->cw;
    bmp.u32Height = (GK_U32)r->chh;
    bmp.pData = cv;
    rc = GK_API_RGN_SetBitMap(r->rgn, &bmp);
    if (rc != GK_SUCCESS) {
        fprintf(stderr, "[gk_osd] RGN_SetBitMap(0x%x) rc=0x%08x\n", r->rgn, rc);
        return;
    }
}

/* ---------------------------------------------------------------- RGN 生命周期 */

/** 某一编码通道上的挂载点（像素，已按该通道分辨率换算并夹到画面内）。
 *
 * `ci`：0 = 主码流、1 = 子码流编码通道（与 CHNS 的 venc_chn 序号一致）。
 * 右对齐（实机「国标模式」下时间与通道名称的排法）时用「文字宽度 + 最小边距」
 * 从右边框反算 —— 对齐实机 `x = 1E4 − 1E4*fontpx*(len/2 + margin)/W` 的归一化算法。
 */
static void osd_layout_point(const gk_osd_region_t *r, int ci, int *out_x, int *out_y)
{
    int fw = (ci == 0) ? GK_MAIN_W : GK_SUB_W;
    int fh = (ci == 0) ? GK_MAIN_H : GK_SUB_H;
    int ascii_w = osd_ascii_cell(r->cfg.font_px);
    int w = r->text_w, h = r->text_h;
    int x, y;

    /* 还没推过文本（刚建区）：按一个字符格兜底，别让宽度算成 0 */
    if (w <= 0) w = ascii_w;
    if (h <= 0) h = ascii_w;
    if (w > r->cw && r->cw > 0) w = r->cw;
    if (r->cfg.align == HAL_OSD_ALIGN_RIGHT) {
        int margin = (int)r->cfg.margin_chars * ascii_w;   /* 最小边距（ASCII 字符格） */
        x = fw - w - margin;
    } else {
        x = (int)(r->cfg.pos.x * (float)fw);              /* 自由定位：归一化左上角 */
    }
    y = (int)(r->cfg.pos.y * (float)fh);

    /* 窗口不能越界（越界部分有些版本会直接报错），所以贴着右/下边时往回缩到刚好放得下 */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > fw - w) x = fw - w;
    if (y > fh - h) y = fh - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    /* 坐标要 2 像素对齐：传奇数 y 会被 AttachToChn 判为 ILLEGAL_PARAM
       （实测 0xa0038003，而 y=324 的同一个区域就能挂上）。 */
    x &= ~1;
    y &= ~1;
    *out_x = x;
    *out_y = y;
}

/** 把区域挂到编码通道上。
 *
 * @note 本 SDK 的 RGN **不支持挂 VPSS 通道**：实测 `RGN_AttachToChn` 传
 * `MOD_ID_VPSS` 返回 `0xa0038008`（EN_ERR_NOT_SUPPORT）。所以改挂 VENC——
 * 挂 VENC 时坐标是**编码分辨率**，主/子各自换算。挂编码器的好处是预览、
 * 抓图、录像三条出口都经 VENC，都能带上 OSD。
 *
 * 遮挡（is_cover）**只挂 r->ch 这一路**：遮挡位图尺寸必须与 RGN 尺寸一致，
 * 而主/子两路分辨率不同（1920×1080 vs 640×360）——一份位图挂两路会让子码流
 * 上的遮挡块大小错掉（OSD 文本踩过同类问题，见 LEG-UI-19）。控制台侧因此为
 * 每个矩形各建两个 HAL 区域（主/子各一个）。 */
static int osd_attach_chns(gk_osd_region_t *r)
{
    static const struct { int venc_chn; } CHNS[] = {
        { 0 },   /* 主码流编码通道 */
        { 1 },   /* 子码流编码通道 */
    };
    int n = r->is_cover ? 1 : (int)(sizeof(CHNS) / sizeof(CHNS[0]));

    for (int i = 0; i < n; i++) {
        MPP_CHN_S chn;
        RGN_CHN_ATTR_S ca;
        GK_S32 rc;
        int ci = r->is_cover ? r->ch : i;
        int x = 0, y = 0;

        if (!r->is_cover) osd_layout_point(r, ci, &x, &y);

        chn.enModId = MOD_ID_VENC;
        chn.s32DevId = 0;
        chn.s32ChnId = CHNS[ci].venc_chn;

        memset(&ca, 0, sizeof(ca));
        ca.bShow = r->enabled ? GK_TRUE : GK_FALSE;
        ca.enType = OVERLAY_RGN;
        if (r->is_cover) {
            /* 遮挡块：整块位图都是不透明纯黑，所以只要把左上角对准矩形位置、
               前景 alpha 拉满就行（背景 0 = 透明，不影响周围像素）。 */
            int w = 0, h = 0;
            osd_cover_rect(r, ci, &x, &y, &w, &h);
            ca.unChnAttr.stOverlayChn.stPoint.s32X = x;
            ca.unChnAttr.stOverlayChn.stPoint.s32Y = y;
            ca.unChnAttr.stOverlayChn.u32BgAlpha = 0;
            ca.unChnAttr.stOverlayChn.u32FgAlpha = 128;  /* 0~128，128 = 不透明 */
            ca.unChnAttr.stOverlayChn.u32Layer = 0;
        } else {
            ca.unChnAttr.stOverlayChn.stPoint.s32X = x;
            ca.unChnAttr.stOverlayChn.stPoint.s32Y = y;
            /* alpha 量程是 0~128（128 = 完全不透明），不是 0~255：传 255 会被
               AttachToChn 判为 ILLEGAL_PARAM（实测 0xa0038003）。 */
            ca.unChnAttr.stOverlayChn.u32BgAlpha = 0;    /* 背景全透明，只留字 */
            ca.unChnAttr.stOverlayChn.u32FgAlpha = 128;  /* 前景不透明 */
            ca.unChnAttr.stOverlayChn.u32Layer = 0;
        }

        rc = GK_API_RGN_AttachToChn(r->rgn, &chn, &ca);
        if (rc != GK_SUCCESS) {
            fprintf(stderr, "[gk_osd] RGN_AttachToChn(%s venc.%d) rc=0x%08x\n",
                    r->is_cover ? "cover" : "overlay", CHNS[ci].venc_chn, rc);
            return -1;
        }
        r->last_x[ci] = x;
        r->last_y[ci] = y;
    }
    return 0;
}

/** 切换显示/隐藏（用 SetDisplayAttr，比反复 Detach/Attach 稳：后者会让整块 OSD 闪一下）。
 *  可开关与闪烁共用它：`show = enabled && 相位`。 */
static void osd_set_show(gk_osd_region_t *r, bool show)
{
    static const int CHNS[] = { 0, 1 };
    int n = r->is_cover ? 1 : 2;

    if (!r->attached) return;
    for (int i = 0; i < n; i++) {
        MPP_CHN_S chn;
        RGN_CHN_ATTR_S ca;
        int ci = r->is_cover ? r->ch : i;
        chn.enModId = MOD_ID_VENC;
        chn.s32DevId = 0;
        chn.s32ChnId = CHNS[ci];
        if (GK_API_RGN_GetDisplayAttr(r->rgn, &chn, &ca) == GK_SUCCESS) {
            ca.bShow = show ? GK_TRUE : GK_FALSE;
            GK_API_RGN_SetDisplayAttr(r->rgn, &chn, &ca);
        }
    }
}

static int osd_rgn_create(gk_osd_region_t *r)
{
    RGN_ATTR_S ra;
    GK_S32 rc;
    int w = 0, h = 0;

    /* 遮挡矩形：**不走 COVER_RGN**——本 SDK 的 MPP 不接受把 COVER_RGN 挂到
       任何可用通道（VENC 与 VPSS 都返回 0xa0038008 EN_ERR_NOT_SUPPORT，
       2026-10-01 真机实测），而 OVERLAY 挂 VENC 是既有 OSD 一直在用的通路。
       于是遮挡 = 一块与矩形同尺寸的 **不透明纯黑 OVERLAY 位图**：
       RGN 尺寸建时就定、位图尺寸必须与它一致，所以尺寸变化要重建（同 OSD）。
       代价是位图常驻内存 → 面积上限见 GK_OSD_COVER_AREA_DIV。 */
    if (r->is_cover) {
        int x, y, w, h, fw, fh, i;
        uint16_t *cv;
        BITMAP_S bmp;

        fw = (r->ch == 0) ? GK_MAIN_W : GK_SUB_W;
        fh = (r->ch == 0) ? GK_MAIN_H : GK_SUB_H;
        osd_cover_rect(r, r->ch, &x, &y, &w, &h);
        /* 宁可拒绝也不缩水：画小了就是隐私泄露（控制台会如实报错） */
        if ((long)w * (long)h > (long)(fw / GK_OSD_COVER_AREA_DIV) * (long)fh) {
            fprintf(stderr, "[gk_osd] 遮挡区域过大（%d×%d，通道 %d 上限约 %d×%d）\n",
                    w, h, r->ch, fw / GK_OSD_COVER_AREA_DIV, fh);
            return -1;
        }
        if (!osd_canvas_ensure(r, w, h)) return -1;
        cv = r->canvas;
        for (i = 0; i < r->cw * r->chh; i++) cv[i] = 0x8000u;   /* 不透明纯黑（A=1,R=G=B=0） */

        memset(&ra, 0, sizeof(ra));
        ra.enType = OVERLAY_RGN;
        ra.unAttr.stOverlay.enPixelFmt = PIXEL_FORMAT_ARGB_1555;
        ra.unAttr.stOverlay.stSize.u32Width = (GK_U32)r->cw;
        ra.unAttr.stOverlay.stSize.u32Height = (GK_U32)r->chh;
        ra.unAttr.stOverlay.u32BgColor = 0;
        ra.unAttr.stOverlay.u32CanvasNum = 1;
        rc = GK_API_RGN_Create(r->rgn, &ra);
        if (rc != GK_SUCCESS) {
            fprintf(stderr, "[gk_osd] cover RGN_Create(0x%x, %d×%d) rc=0x%08x\n",
                    r->rgn, r->cw, r->chh, rc);
            return -1;
        }
        memset(&bmp, 0, sizeof(bmp));
        bmp.enPixelFormat = PIXEL_FORMAT_ARGB_1555;
        bmp.u32Width = (GK_U32)r->cw;
        bmp.u32Height = (GK_U32)r->chh;
        bmp.pData = cv;
        /* ⚠ 顺序：**先挂通道、再推位图**（与 OSD 文本同序）。
           子码流是 MJPEG 编码通道，实测“先推位图、再挂”在主码流上看着正常，
           但 MJPEG 通道那条路不会把画布标成已用（/proc/umap/rgn 里 buf0=0），
           子码流画面里就看不到遮挡；改成挂完再推即两路都生效。 */
        if (osd_attach_chns(r) != 0) {
            GK_API_RGN_Destroy(r->rgn);
            return -1;
        }
        rc = GK_API_RGN_SetBitMap(r->rgn, &bmp);
        if (rc != GK_SUCCESS) {
            fprintf(stderr, "[gk_osd] cover RGN_SetBitMap(0x%x) rc=0x%08x\n", r->rgn, rc);
            osd_rgn_destroy(r);
            return -1;
        }
        r->attached = true;
        r->rgn_w = r->cw;
        r->rgn_h = r->chh;
        return 0;
    }

    /* RGN 的尺寸必须在建时就定下，且与 SetBitMap 的位图尺寸一致：
       先按当前文本（时间项按预留范围）把画布备好，再拿画布尺寸去建 RGN。 */
    if (r->is_time) {
        osd_reserve_extent(r, &w, &h);
    } else {
        gk_measure(r->cfg.text, r->cfg.font_px, &w, &h);
    }
    if (w < 1 || h < 1) osd_reserve_extent(r, &w, &h);
    if (!osd_canvas_ensure(r, w, h)) return -1;

    memset(&ra, 0, sizeof(ra));
    ra.enType = OVERLAY_RGN;
    ra.unAttr.stOverlay.enPixelFmt = PIXEL_FORMAT_ARGB_1555;
    ra.unAttr.stOverlay.stSize.u32Width = (GK_U32)r->cw;
    ra.unAttr.stOverlay.stSize.u32Height = (GK_U32)r->chh;
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
    r->rgn_w = r->cw;
    r->rgn_h = r->chh;
    return 0;
}

static void osd_rgn_destroy(gk_osd_region_t *r)
{
    /* Detach 失败（例如 VENC 通道已经拆了）不阻断：Destroy 会把残留一并清掉 */
    static const int CHNS[] = { 0, 1 };
    int n = r->is_cover ? 1 : 2;

    for (int i = 0; i < n; i++) {
        MPP_CHN_S chn;
        int ci = r->is_cover ? r->ch : i;
        chn.enModId = MOD_ID_VENC;
        chn.s32DevId = 0;
        chn.s32ChnId = CHNS[ci];
        GK_API_RGN_DetachFromChn(r->rgn, &chn);
    }
    GK_API_RGN_Destroy(r->rgn);
    r->attached = false;
    r->rgn_w = r->rgn_h = 0;
}

/* ---------------------------------------------------------------- 时间刷新 */

/** 时间串格式化：strftime + 本 HAL 的一个扩展——`%a`/`%A` 渲染成中文星期。
 *
 *  板端 rootfs 没有 locale 数据（setlocale 拿不到 zh_CN），C locale 下 strftime 的
 *  `%a` 只会给英文缩写；而 OSD 字库是点阵、只收了中文星期那三个字，所以 `%a` 这一段
 *  自己算，**其余转换符一律照旧交给 strftime**（早先的写法把整串都当普通字符拷了，
 *  结果画面上直接打出 `%Y-%m-%d`，实测踩过）。
 *
 *  实现：按 `%a/%A` 把格式串切成若干段（跳过 `%%`），每段交给 strftime，中间插入中文星期。
 *  其它实现（mock / 别的 SOC）照 strftime 走即可，`%a` 会退化成英文——降级但不出错。 */
static int osd_format_time(char *out, size_t cap, const char *fmt, const struct tm *tmv)
{
    static const char WK[7][10] = {
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
    };
    char seg[HAL_OSD_TEXT_MAX];
    const char *p = fmt;
    size_t o = 0;

    while (*p) {
        const char *hit = NULL;
        size_t seg_len, n;

        /* 找下一个 `%a`/`%A`（`%%` 不是转换符，跳过） */
        for (const char *q = p; *q; q++) {
            if (q[0] != '%') continue;
            if (q[1] == '%') { q++; continue; }
            if (q[1] == 'a' || q[1] == 'A') { hit = q; break; }
        }
        seg_len = hit ? (size_t)(hit - p) : strlen(p);
        if (seg_len) {
            if (seg_len >= sizeof(seg)) seg_len = sizeof(seg) - 1;
            memcpy(seg, p, seg_len);
            seg[seg_len] = '\0';
            n = strftime(out + o, cap - o, seg, tmv);
            if (n == 0) return 0;          /* 放不下或转换失败：这一秒不刷 */
            o += n;
        }
        if (!hit) break;
        {
            const char *wk = WK[(tmv->tm_wday >= 0 && tmv->tm_wday < 7) ? tmv->tm_wday : 0];
            n = strlen(wk);
            if (o + n >= cap) return 0;
            memcpy(out + o, wk, n);
            o += n;
        }
        p = hit + 2;
    }
    out[o] = '\0';
    return (int)o;
}

static void *osd_tick_thread(void *arg)
{
    char buf[HAL_OSD_TEXT_MAX];
    int  slice = 0;
    bool blink_on = true;
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
            /* text 是 strftime 格式串（见 hal_osd.h 对 HAL_OSD_TEXT_TIME 的说明），
               `%a` 由 osd_format_time 渲染成中文星期 */
            fmt = r->cfg.text[0] ? r->cfg.text : "%Y-%m-%d %H:%M:%S";
            if (osd_format_time(buf, sizeof(buf), fmt, &tmv) <= 0) continue;
            if (strcmp(buf, r->last_text) == 0) continue;  /* 同一秒内不重画 */
            osd_push(r, buf);
        }
        /* 每 500ms 翻转一次闪烁相位（实机「显示效果：闪烁」＝文本周期性隐现） */
        if (++slice >= 5) {
            slice = 0;
            blink_on = !blink_on;
            for (int i = 0; i < GK_OSD_MAX_REGIONS; i++) {
                gk_osd_region_t *r = &s_rgn[i];
                if (!r->used || !r->attached || !r->cfg.flicker) continue;
                if (!r->enabled) continue;
                osd_set_show(r, blink_on);
            }
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
    c->right_align = true;  /* v1.4：支持右对齐（国标模式的时间/通道名） */
    c->flicker = true;      /* v1.4：支持文本闪烁 */
    c->cover = true;        /* v1.6：支持矩形遮挡（控制台「区域覆盖」） */
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
    /* 遮挡矩形（v1.6）：四个字段都要有效——没有长宽的“矩形”没意义，
       这里直接拒（控制台侧另有一道“长或宽太小”的提示，对齐实机文案）。 */
    if (cfg->kind == HAL_OSD_COVER &&
        (cfg->pos.w <= 0.0f || cfg->pos.w > 1.0f || cfg->pos.h <= 0.0f || cfg->pos.h > 1.0f))
        return HAL_EINVAL;

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
    r->is_cover = (cfg->kind == HAL_OSD_COVER);
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
    /* 遮挡矩形用 pos 的四个字段（文本只用 x/y）：长宽越界直接拒，
       免得拖出一个跑到画面外的框。 */
    if (r->is_cover && (pos->w <= 0.0f || pos->w > 1.0f || pos->h <= 0.0f || pos->h > 1.0f))
        return HAL_EINVAL;

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
    osd_set_show(r, enable);
    return HAL_OK;
}

static hal_err_t o_destroy(int id)
{
    gk_osd_region_t *r = osd_of(id);
    if (!r) return HAL_EINVAL;
    if (r->attached) osd_rgn_destroy(r);
    free(r->canvas);   /* 画布是按需分配的，别随槽位复用泄着 */
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
