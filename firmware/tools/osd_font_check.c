/* OSD 字库自检（**离线工具，不参与固件构建**）
 *
 * 直接 include 生成的 gk_font_cjk.h，用 gk_osd.c 同一套 UTF-8 解码 + 二分查表 +
 * 整数倍放大逻辑渲染几行文本，输出 PGM 位图，用来肉眼确认「码点表和点阵是否对齐、
 * 解码有没有跑偏、简繁英混排是否整齐、未收录字符是否只占一格」。
 * 换了字库（改 --charset/--wght/--font 重跑 gen_osd_font.py）之后跑一遍即可回归。
 *
 * 编译运行（VS 开发者命令提示符，或先 call vcvars64.bat；注意必须带 /utf-8，
 * 否则源文件里的中文字面量会按本地代码页解析成乱码）：
 *   cl /nologo /W3 /utf-8 firmware\tools\osd_font_check.c
 *   osd_font_check.exe            # 生成 osd_check.pgm（P5 灰度，255=字），当前目录
 *   python -c "from PIL import Image;im=Image.open('osd_check.pgm');im.resize((im.width*2,im.height*2),Image.NEAREST).save('osd_check.png')"
 *
 * 注意：下面的 GK_CHARSET/GK_FONT（ASCII 8×8 表）是 gk_osd.c 的**副本**——改了那边
 * 的字形记得同步这里，否则自检结果与板子不一致。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../platform/gk7205v200/gk_font_cjk.h"
#include "../platform/gk7205v200/gk_font_cjk_hi.h"
#include "../platform/gk7205v200/gk_font_ascii.h"

#define GK_GLYPH_W 16               /* ASCII 格宽基准（= 最小档点阵边长） */
#define GK_GLYPH_H 16
#define GK_OSD_SCALE_MAX 8

/* 下面 utf8_next / gk_scale_of 与 gk_osd.c 逐行一致；字形查表直接用生成表里的
 * gk_ascii_lookup / gk_cjk_lookup（不再复制手写 8×8 表——那份已由生成表取代）。 */

static int gk_scale_of(unsigned font_px, int cell)
{
    int s = (int)((font_px + (unsigned)cell / 2u) / (unsigned)cell);
    if (s < 1) s = 1;
    return s;
}

static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp;
    size_t   n;

    if (s[0] < 0x80u) { *p = (const char *)s + 1; return s[0]; }
    if ((s[0] & 0xE0u) == 0xC0u) { n = 2; cp = s[0] & 0x1Fu; }
    else if ((s[0] & 0xF0u) == 0xE0u) { n = 3; cp = s[0] & 0x0Fu; }
    else if ((s[0] & 0xF8u) == 0xF0u) { n = 4; cp = s[0] & 0x07u; }
    else { *p = (const char *)s + 1; return 0xFFFDu; }

    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0u) != 0x80u) { *p = (const char *)s + 1; return 0xFFFDu; }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    *p = (const char *)s + n;
    return cp;
}

/* ---------------------------------------------------------------- 渲染 */

/** 与 gk_osd.c 同一口径：ASCII 格宽 = 16 的就近整数倍；汉字 = 24 的就近整数倍 */
static int osd_ascii_cell(unsigned px)
{
    int s = gk_scale_of(px, GK_GLYPH_W);
    return GK_GLYPH_W * (s > GK_OSD_SCALE_MAX ? GK_OSD_SCALE_MAX : s);
}
static int osd_cjk_cell(unsigned px) { return GK_CJK_CELL * gk_scale_of(px, GK_CJK_CELL); }

#define W 2100
#define LINE_H 80
static uint8_t s_img[LINE_H * 24][W];
static int     s_rows;

/* 与 gk_osd.c 同一口径选表：格宽 ≥48 时优先用常用字 48×48 高分辨率表，
 * 未收录则回落 24×24 全库。**两张表的格宽口径一样**（都是 24 的倍数），
 * 所以换表不会改变文本宽度——这也是下面宽度对账仍然有效的原因。 */
static const uint8_t *cjk_bits(unsigned px, uint32_t cp, int *out_cell)
{
    int cell = osd_cjk_cell(px);
    if (cell >= GK_CJK_HI_CELL) {
        const uint8_t *hi = gk_cjk_hi_lookup(cp);
        if (hi) { *out_cell = GK_CJK_HI_CELL; return hi; }
    }
    {
        const uint8_t *b = gk_cjk_lookup(cp);
        if (b) { *out_cell = GK_CJK_CELL; return b; }
    }
    return NULL;
}

static int measure(const char *t, unsigned px)
{
    int w = 0;
    const char *p = t;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        int cell = 0;
        w += (cp >= 0x80u && cjk_bits(px, cp, &cell)) ? osd_cjk_cell(px) : osd_ascii_cell(px);
    }
    return w;
}

static void render(const char *t, unsigned px)
{
    uint8_t *row = s_img[s_rows * LINE_H];
    int x = 0;
    const char *p = t;
    int width = measure(t, px);
    int miss = 0;

    /* 先报一遍每个字符的解析结果，便于对照是"字库缺字"还是"渲染错位" */
    printf("  \"%s\" 宽=%dpx :", t, width);
    {
        const char *q = t;
        while (*q) {
            uint32_t cp = utf8_next(&q);
            const uint8_t *b = (cp >= 0x80u) ? gk_cjk_lookup(cp) : NULL;
            int hits_hi = (cp >= 0x80u) && (osd_cjk_cell(px) >= GK_CJK_HI_CELL)
                          && gk_cjk_hi_lookup(cp) != NULL;
            if (cp < 0x80u) printf(" [%c]", (char)cp);
            else if (b) printf(" U+%04X(%s)", cp, hits_hi ? "hi48" : "24");
            else { printf(" U+%04X(缺字→'-')", cp); miss++; }
        }
    }
    printf("\n");

    p = t;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        const uint8_t *bits = NULL;
        int cell = osd_ascii_cell(px);
        int gw = 0, gh = 0;

        if (cp >= 0x80u) {
            int gcell = 0;
            bits = cjk_bits(px, cp, &gcell);
            if (bits) { gw = gh = gcell; cell = osd_cjk_cell(px); }
            else { bits = gk_ascii_lookup('-', cell); gw = gh = gk_ascii_size(cell); }
        } else {
            bits = gk_ascii_lookup(cp, cell);
            gw = gh = gk_ascii_size(cell);
        }
        if (bits && gw > 0) {
            /* 与 gk_osd.c 同款：最近邻铺格（点阵边长 == 格宽时就是 1:1） */
            int row_bytes = gw / 8;
            for (int py = 0; py < cell; py++) {
                const uint8_t *brow = bits + (size_t)(py * gh / cell) * (size_t)row_bytes;
                for (int pxx = 0; pxx < cell; pxx++) {
                    int gx = pxx * gw / cell;
                    if (!(brow[gx / 8] & (0x80u >> (gx % 8)))) continue;
                    if (x + pxx < W && py < LINE_H) row[py * W + x + pxx] = 255;
                }
            }
        }
        x += cell;
    }
    s_rows++;
}

int main(void)
{
    static const char *TEXT[] = {
        "门口摄像机 Camera-1",
        "東門體國臺 繁體測試",
        "ABC xyz ()[]?!+=*#@&% 0123456789",
        "生僻字: 𠮷 与 emoji: 😀 都该占一格",
    };
    /* 四档字号都要过：24/32 走汉字格宽 24（只用 24×24 全库），48/64 走格宽 48/72
       （命中常用字 48×48 高分辨率表；繁体常用字多在一级字里，正好能看到差别）。
       改字库/改选表逻辑后重跑：**各档文本宽度必须与上一版逐字一致**（格宽口径没变），
       再放大点阵图看笔画粗细是否符合预期。 */
    static const unsigned SIZES[] = { 24, 32, 48, 64 };
    FILE *f;
    int i, j;

    printf("全库：%u 字 @%dx%d；高分辨率表：%u 字 @%dx%d（数据 %.0fKB）\n",
           (unsigned)GK_CJK_COUNT, GK_CJK_CELL, GK_CJK_BYTES * 8,
           (unsigned)GK_CJK_HI_COUNT, GK_CJK_HI_CELL, GK_CJK_HI_CELL,
           (double)GK_CJK_HI_COUNT * GK_CJK_HI_CELL * (GK_CJK_HI_CELL / 8) / 1024.0);
    for (j = 0; j < (int)(sizeof(SIZES) / sizeof(SIZES[0])); j++) {
        printf("=== 字号 %upx（ASCII 格宽 %d，汉字格宽 %d）===\n",
               SIZES[j], osd_ascii_cell(SIZES[j]), osd_cjk_cell(SIZES[j]));
        for (i = 0; i < (int)(sizeof(TEXT) / sizeof(TEXT[0])); i++)
            render(TEXT[i], SIZES[j]);
    }

    f = fopen("osd_check.pgm", "wb");
    if (!f) { printf("写 osd_check.pgm 失败\n"); return 1; }
    fprintf(f, "P5\n%d %d\n255\n", W, s_rows * LINE_H);
    fwrite(s_img, 1, (size_t)W * (size_t)s_rows * LINE_H, f);
    fclose(f);
    printf("已写出 osd_check.pgm（%d 行）\n", s_rows);
    return 0;
}
