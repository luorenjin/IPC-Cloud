#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 OSD 汉字点阵字库（C 源），供 firmware/platform/gk7205v200/gk_osd.c 使用。

为什么用生成脚本而不是手写字库：
  * 字形来自**可再分发的开源字体**，授权链清楚（见下）；
  * 字符集、字号、字重都是参数，将来要扩语言/换字库只要改参数重跑；
  * 生成物是可复现的（命令写在输出文件头）。

字库来源与许可证
  * 字体：**Noto Sans SC**（Google/Adobe，SIL Open Font License 1.1）——
    OFL 允许使用、修改、再分发（含把渲染出的点阵嵌入产品），要求保留版权与
    许可证声明、且不得单独售卖字体本身。本脚本把声明写进生成文件的头部。
  * 字形参数：可变字体的 **wght=600**、字号 24px、二值化阈值 110。
    实测（2026-10-01）：Regular(400) 在 24px 下笔画只有 1px、视频压缩后糊；
    Bold(700) 复杂字（體/國）会黏连；**600 最平衡**（简繁都清晰）。

用法
  python firmware/tools/gen_osd_font.py                    # 默认：GB2312 + Big5 一级
  python firmware/tools/gen_osd_font.py --charset gb2312    # 只做简体
  python firmware/tools/gen_osd_font.py --font path/to/font.ttf --wght 500

字符集（可组合，用逗号分隔）
  ascii    可打印 ASCII 0x20–0x7E（ASCII 走内嵌 8×8 表，这里只为度量用，默认不加）
  gb2312   GB2312 全集：6763 汉字 + 682 全角符号（简体全覆盖）
  gb2312-1 GB2312 **一级**汉字 3755（最常用简体字，按拼音排序）——高分辨率表用这个
  big5-1   Big5 一级汉字 5401（**繁体常用字**）
  big5-2   Big5 二级汉字 7652（繁体次常用，体积大，默认不加）

两张表（同一套度量，只是分辨率不同）
  gk_font_cjk.h     24×24 全库（gb2312 ∪ big5-1，9520 字）——所有汉字的**回落**
  gk_font_cjk_hi.h  48×48 常用字（默认 gb2312-1，3755 字）——目标格宽 ≥48 时优先用
  ⚠️ 两张表的**格宽口径必须都是 24 的倍数**（osd_cjk_cell() = 24×就近整数倍），
  高分辨率表只改“用哪张点阵”，**不改格宽**——否则前端 devCell、时间项预留、
  右对齐落点全要跟着改（改完请跑 osd_font_check.c 对账宽度）。
"""
import argparse
import os
import sys
import time

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("需要 Pillow：pip install pillow")

CELL = 24          # 点阵尺寸（与 gk_osd.c 的 GK_CJK_CELL 一致）
THRESHOLD = 110    # 二值化阈值
ROWS_BYTES = CELL // 8
# ASCII 单独一张表：**按字号档位各预渲染一套**（16/32/48/64）。
# 为什么不是一套点阵 + 整数倍放大：RGN 的 alpha 只有 1 bit（非透明即全透），
# 做不了抗锯齿，所以“放大 N 倍”意味着一个字形像素变成 N×N 个屏幕像素——数字
# 看着就是方块（2026-10-01 用户两次反馈“颗粒感/不够圆润”：8×8 放大 8 倍最粗糙，
# 16×16 放大 4 倍好一些但仍能看出台阶）。换成“**每个档位一张 1:1 的点阵**”后，
# 笔画就是笔画本身，没有台阶。
# 代价只是数据量：95 字 ×(16²+32²+48²+64²)/8 ≈ 91KB，对 2.2MB 的 ipc_app 可忽略。
# 字形仍然“填满格”（渲染字号 ≈ 格边长 × 1.375），与矢量字体实际字高占比一致。
ASCII_CELL = 16            # 最小档（= 格宽基准，gk_osd.c 的 GK_GLYPH_W）
ASCII_SIZES = (16, 32, 48, 64)
ASCII_PX_RATIO = 22 / 16   # 渲染字号 / 格边长（16 格 → 22px；64 格 → 88px）
ASCII_FIRST = 0x20
ASCII_LAST = 0x7E
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\NotoSansSC-VF.ttf",
    r"C:\Windows\Fonts\NotoSansSC-Regular.otf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
]


# ---------------------------------------------------------------- 字符集

def ascii_chars():
    return [chr(c) for c in range(0x20, 0x7F)]


def gb2312_chars():
    """GB2312 全集（含 682 个符号区字符，全角标点对通道名有用）"""
    out = []
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return out


def gb2312_l1_chars():
    """GB2312 **一级**汉字（区 16–55，即 0xB0A1–0xD7F9）共 3755 字。

    一级字按拼音排序，是“日常一定会用到”的那批；高分辨率表只给这批，
    生僻字回落到 24×24 表（体积因此从 9.6MB 降到 ~1.1MB）。
    注：GB2312 的汉字区是 0xB0–0xF7（一二级共 6763），**一级只到 0xD7**，
    末行 0xD7 只到 0xF9 —— 用解码失败过滤即可精确落到 3755。
    """
    out = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return out


def big5_chars(level=1):
    """Big5 汉字：level 1 = 常用（0xA440–0xC67E），level 2 = 次常用（0xC940–0xF9D5）"""
    ranges = {1: [(0xA4, 0xC6)], 2: [(0xC9, 0xF9)]}[level]
    out = []
    for hi in range(0xA1, 0xFA):
        if not any(lo <= hi <= hi2 for lo, hi2 in ranges):
            continue
        for lo in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
            try:
                ch = bytes([hi, lo]).decode("big5")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                out.append(ch)
    return out


def build_charset(names):
    chars = []
    for name in names:
        name = name.strip().lower()
        if name == "ascii":
            chars += ascii_chars()
        elif name == "gb2312":
            chars += gb2312_chars()
        elif name == "gb2312-1":
            chars += gb2312_l1_chars()
        elif name == "big5-1":
            chars += big5_chars(1)
        elif name == "big5-2":
            chars += big5_chars(2)
        else:
            sys.exit("未知字符集：%s（可选 ascii/gb2312/gb2312-1/big5-1/big5-2）" % name)
    # 去重 + 只保留 BMP（点阵表按 u32 码点索引）+ 去掉 ASCII（由内嵌 8×8 表负责）
    seen, out = set(), []
    for ch in chars:
        cp = ord(ch)
        if cp < 0x80 or cp > 0xFFFF or cp in seen:
            continue
        seen.add(cp)
        out.append(cp)
    return sorted(out)


# ---------------------------------------------------------------- 栅格化

def make_font(path, size, wght):
    font = ImageFont.truetype(path, size)
    if wght:
        try:
            axes = font.get_variation_axes()
            if axes:
                font.set_variation_by_axes([wght])
        except Exception:                                        # noqa: BLE001
            print("  （该字体不支持可变字重，按默认字重渲染）")
    return font


def raster(font, cp, cell=CELL):
    """一个码点 → cell×cell 二值点阵（每行 cell/8 字节，MSB 在左）"""
    img = Image.new("L", (cell, cell), 0)
    d = ImageDraw.Draw(img)
    ch = chr(cp)
    box = d.textbbox((0, 0), ch, font=font)
    w, h = box[2] - box[0], box[3] - box[1]
    d.text(((cell - w) // 2 - box[0], (cell - h) // 2 - box[1]), ch, font=font, fill=255)
    px = img.load()
    rows = []
    ink = 0
    rb = cell // 8
    for r in range(cell):
        row = [0] * rb
        for c in range(cell):
            if px[c, r] >= THRESHOLD:
                row[c // 8] |= 0x80 >> (c % 8)
                ink += 1
        rows.append(tuple(row))
    return rows, ink


# ---------------------------------------------------------------- 输出

HEADER = """/**
 * @file gk_font_cjk.{ext}
 * @brief OSD 汉字点阵字库（**自动生成，勿手改**）
 *
 * 生成命令（{date}）：
 *   {cmd}
 *
 * 数据规模：{count} 个码点 × {cell}×{cell} 点阵（每字 {rows} 行 × {rb} 字节）
 *          数据 {kb:.0f} KB
 *
 * 字形来源与许可证（必须随固件保留）：
 *   字体：{fontname} —— SIL Open Font License 1.1（OFL-1.1）
 *   版权：Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font
 *         Name 'Source'. Source is a trademark of Adobe in the United States
 *         and/or other countries.（Noto Sans SC 基于 Source Han Sans / 思源黑体）
 *   本文件是上述字体在 wght={wght} / {size}px 下二值化得到的**派生点阵数据**，
 *   依 OFL-1.1 使用与再分发：保留本声明、不改用字体的保留名称、不单独售卖。
 *
 * 覆盖范围：{coverage}
 * 未收录的字符由 gk_osd.c 渲染成 '-' 占位（宁可看出缺字，也不静默丢字符）。
 */
#ifndef GK_FONT_CJK_H
#define GK_FONT_CJK_H

#include <stdint.h>

#ifndef GK_CJK_CELL
#define GK_CJK_CELL        {cell}
#define GK_CJK_BYTES       {rb}
#endif
#define GK_CJK_COUNT       {count}

/** 码点表（升序，供二分查找）；下标与点阵数据一一对应 */
static const uint32_t GK_CJK_CODEPOINTS[GK_CJK_COUNT] = {{
{points}
}};

/** 点阵数据：每字 GK_CJK_CELL 行 × GK_CJK_BYTES 字节，MSB 在左，行优先连续存放。
 *  每字一个 72 字节的字符串字面量（不用一个巨大的拼接字面量：拼接后总长超过
 *  C99 要求支持的 4095 字节，GCC 会报 -Woverlength-strings）。
 *  读取时必须转 uint8_t（char 可能带符号）：`(uint8_t)GK_CJK_DATA[i][off]`。 */
static const char GK_CJK_DATA[GK_CJK_COUNT][GK_CJK_CELL * GK_CJK_BYTES] =
{{
{glyphs}
}};

/** 二分查字形；未收录返回 NULL（调用方用 '-' 顶替） */
static inline const uint8_t *gk_cjk_lookup(uint32_t cp)
{{
    uint32_t lo = 0, hi = GK_CJK_COUNT;

    while (lo < hi) {{
        uint32_t mid = lo + (hi - lo) / 2u;
        if (GK_CJK_CODEPOINTS[mid] == cp) return (const uint8_t *)GK_CJK_DATA[mid];
        if (GK_CJK_CODEPOINTS[mid] < cp) lo = mid + 1u;
        else hi = mid;
    }}
    return 0;
}}

#endif /* GK_FONT_CJK_H */
"""


ASCII_HEADER = """/**
 * @file gk_font_ascii.h
 * @brief OSD ASCII 点阵字库（**自动生成，勿手改**）
 *
 * 生成命令（{date}）：
 *   {cmd}
 *
 * 结构：可打印 ASCII 0x{first:02X}–0x{last:02X} 共 {count} 字，**按字号档位各一套**
 * （{sizes}），绘制时**选最接近目标格宽的那套、1:1 铺格**——这样就没有“整数倍
 * 放大”的像素台阶，数字边缘才圆润。每套的字形用「格边长 × {ratio:.3f}」渲染
 * （字形要填满格：字体实际字高只占字号的七成）。
 * 数据共 {kb:.0f}KB（{count}×(16²+32²+48²+64²)/8），对 ipc_app 可忽略。
 *
 * 为什么不做抗锯齿：RGN 的 alpha 只有 1 bit（非透明即全透），半透明表达不了；
 * 所以能做的就只有“把点阵分辨率提到目标尺寸”，即本文件的多档方案。
 *
 * 字形来源与许可证（必须随固件保留）：
 *   字体：{fontname} —— SIL Open Font License 1.1（OFL-1.1）
 *   版权：Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font
 *         Name 'Source'.（Noto Sans SC 基于 Source Han Sans / 思源黑体）
 *   本文件是该字体在不同字号下二值化得到的**派生点阵数据**，
 *   依 OFL-1.1 使用与再分发：保留本声明、不改用字体的保留名称、不单独售卖。
 */
#ifndef GK_FONT_ASCII_H
#define GK_FONT_ASCII_H

#include <stdint.h>

#define GK_ASCII_FIRST  0x{first:02X}
#define GK_ASCII_LAST   0x{last:02X}
#define GK_ASCII_COUNT  {count}
#define GK_ASCII_NUM    {nsizes}

/** 各档的格边长（像素）；绘制时选最接近目标格宽的那一档，1:1 铺格 */
static const uint16_t GK_ASCII_PX[GK_ASCII_NUM] = {{ {sizes} }};

{tables}

/** 各档表的入口（下标与 GK_ASCII_PX 一一对应） */
static const char *const GK_ASCII_TABLES[GK_ASCII_NUM] = {{
{tables_ref}
}};

/** 选档：返回最接近 cell_px 的档位**下标** */
static inline int gk_ascii_pick(int cell_px)
{{
    int i, best = 0, bestd = 1 << 30;

    for (i = 0; i < GK_ASCII_NUM; i++) {{
        int d = (int)GK_ASCII_PX[i] - cell_px;
        if (d < 0) d = -d;
        if (d < bestd) {{ bestd = d; best = i; }}
    }}
    return best;
}}

/** 选档后的点阵边长（= 该档的格边长，16/32/48/64） */
static inline int gk_ascii_size(int cell_px)
{{
    return (int)GK_ASCII_PX[gk_ascii_pick(cell_px)];
}}

/** 取字形：选最接近 cell_px 的一档；不在可打印 ASCII 范围内返回 NULL。
 *  返回位图的边长 = gk_ascii_size(cell_px)，每行 边长/8 字节、MSB 在左。 */
static inline const uint8_t *gk_ascii_lookup(uint32_t cp, int cell_px)
{{
    int idx, size;

    if (cp < GK_ASCII_FIRST || cp > GK_ASCII_LAST) return 0;
    idx = gk_ascii_pick(cell_px);
    size = (int)GK_ASCII_PX[idx];
    return (const uint8_t *)(GK_ASCII_TABLES[idx]
           + (size_t)(cp - GK_ASCII_FIRST) * (size_t)size * (size_t)(size / 8));
}}

#endif /* GK_FONT_ASCII_H */
"""


CJK_HI_HEADER = """/**
 * @file gk_font_cjk_hi.{ext}
 * @brief OSD **常用汉字高分辨率**点阵字库（**自动生成，勿手改**）
 *
 * 生成命令（{date}）：
 *   {cmd}
 *
 * 为什么要有这张表：gk_font_cjk.h 是 24×24 的全库（9520 字），而自适应字号
 * （64px 档）的汉字格宽是 72px —— 只能把 24×24 放大 3 倍，笔画比数字粗一圈。
 * 本表把**常用字**（{coverage}，{count} 字）单独预渲染成 {cell}×{cell}，目标格宽
 * ≥ {cell} 时优先使用：{cell} 格档 1:1、72 格档只放大 1.5 倍。
 * 未收录的汉字**自动回落** 24×24 全库（覆盖范围不变，只是略粗）。
 *
 * 数据规模：{count} 个码点 × {cell}×{cell} 点阵（每字 {rows} 行 × {rb} 字节）
 *          数据 {kb:.0f} KB
 *
 * ⚠️ 格宽口径：本表**不改变**汉字格宽（仍是 24 的就近整数倍，见 gk_osd.c 的
 * osd_cjk_cell()），只改“用哪张点阵”。改完请跑 osd_font_check.c 对账宽度。
 *
 * 字形来源与许可证（必须随固件保留）：与 gk_font_cjk.h 同源——{fontname}，
 * SIL Open Font License 1.1；完整版权声明见 gk_font_cjk.h 头部。
 */
#ifndef GK_FONT_CJK_HI_H
#define GK_FONT_CJK_HI_H

#include <stdint.h>

#define GK_CJK_HI_CELL     {cell}
#define GK_CJK_HI_BYTES    {rb}
#define GK_CJK_HI_COUNT    {count}

/** 码点表（升序，供二分查找）；下标与点阵数据一一对应 */
static const uint32_t GK_CJK_HI_CODEPOINTS[GK_CJK_HI_COUNT] = {{
{points}
}};

/** 点阵数据：每字 GK_CJK_HI_CELL 行 × GK_CJK_HI_BYTES 字节，MSB 在左，行优先连续。
 *  每字一个字符串字面量（理由同 gk_font_cjk.h：拼接成一个巨大字面量会超过
 *  C99 要求支持的 4095 字节，GCC 报 -Woverlength-strings）。
 *  读取时必须转 uint8_t（char 可能带符号）。 */
static const char GK_CJK_HI_DATA[GK_CJK_HI_COUNT][GK_CJK_HI_CELL * GK_CJK_HI_BYTES] =
{{
{glyphs}
}};

/** 二分查高分辨率字形；未收录返回 NULL（调用方回落到 24×24 全库） */
static inline const uint8_t *gk_cjk_hi_lookup(uint32_t cp)
{{
    uint32_t lo = 0, hi = GK_CJK_HI_COUNT;

    while (lo < hi) {{
        uint32_t mid = lo + (hi - lo) / 2u;
        if (GK_CJK_HI_CODEPOINTS[mid] == cp) return (const uint8_t *)GK_CJK_HI_DATA[mid];
        if (GK_CJK_HI_CODEPOINTS[mid] < cp) lo = mid + 1u;
        else hi = mid;
    }}
    return 0;
}}

#endif /* GK_FONT_CJK_HI_H */
"""


def gen_cjk_hi(font_path, out, wght, cmd, cell, charset_name):
    """生成常用汉字高分辨率表（默认 48×48 / GB2312 一级）"""
    cps = build_charset([charset_name])
    font = make_font(font_path, cell, wght)
    rb = cell // 8
    kept, glyphs = [], []
    t0 = time.time()
    for i, cp in enumerate(cps):
        rows, ink = raster(font, cp, cell)
        if ink < 4:                                  # 与主表同一条剔除规则，避免空白字形
            continue
        kept.append(cp)
        glyphs.append("".join("".join("\\x%02X" % b for b in row) for row in rows))
        if (i + 1) % 1000 == 0:
            print("  高分辨率表已渲染 %5d/%d …" % (i + 1, len(cps)))
    points = "\n".join("    " + " ".join("0x%04X," % cp for cp in part)
                       for part in [kept[i:i + 10] for i in range(0, len(kept), 10)])
    body = "\n".join('    /* U+%04X */ "%s",' % (cp, g) for cp, g in zip(kept, glyphs))
    kb = len(kept) * cell * rb / 1024.0
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(CJK_HI_HEADER.format(ext="h", date=time.strftime("%Y-%m-%d"), cmd=cmd,
                                      count=len(kept), cell=cell, rows=cell, rb=rb, kb=kb,
                                      coverage=charset_name, points=points, glyphs=body,
                                      fontname=os.path.basename(font_path)))
    print("已生成 %s（%.1f KB 数据，%d 字 @%d×%d，取 %.1fs）"
          % (out, kb, len(kept), cell, cell, time.time() - t0))


def gen_ascii(font_path, out, wght, cmd):
    """生成 ASCII 多档点阵表（每档 1:1 铺格，消除整数倍放大的台阶）"""
    tables, refs, total = [], [], 0
    for size in ASCII_SIZES:
        px = max(8, int(round(size * ASCII_PX_RATIO)))
        font = make_font(font_path, px, wght)
        rb = size // 8
        rows_txt = []
        for cp in range(ASCII_FIRST, ASCII_LAST + 1):
            rows, _ = raster(font, cp, size)
            cells = "".join("".join("\\x%02X" % b for b in row) for row in rows)
            rows_txt.append('    /* 0x%02X */ "%s",' % (cp, cells))
        tables.append("/* ---- %d×%d（渲染字号 %dpx）---- */\n"
                      "static const char GK_ASCII_T%d[GK_ASCII_COUNT][%d] = {\n%s\n};\n"
                      % (size, size, px, size, size * rb, "\n".join(rows_txt)))
        refs.append("    (const char *)GK_ASCII_T%d," % size)
        total += (ASCII_LAST - ASCII_FIRST + 1) * size * rb
    text = ASCII_HEADER.format(date=time.strftime("%Y-%m-%d"), cmd=cmd,
                              count=ASCII_LAST - ASCII_FIRST + 1,
                              first=ASCII_FIRST, last=ASCII_LAST,
                              sizes=", ".join(str(s) for s in ASCII_SIZES),
                              nsizes=len(ASCII_SIZES), ratio=ASCII_PX_RATIO,
                              tables="\n".join(tables), tables_ref="\n".join(refs),
                              kb=total / 1024.0, fontname=os.path.basename(font_path),
                              wght=wght)
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("已生成 %s（ASCII %d 字 × %s 四档，%.0fKB 数据）"
          % (out, ASCII_LAST - ASCII_FIRST + 1, "/".join(str(s) for s in ASCII_SIZES), total / 1024.0))


def main():
    ap = argparse.ArgumentParser(description="生成 OSD 汉字/ASCII 点阵字库")
    ap.add_argument("--font", default=None, help="字体文件（默认自动找 Noto Sans SC）")
    ap.add_argument("--wght", type=int, default=600, help="可变字体字重（默认 600）")
    ap.add_argument("--size", type=int, default=CELL, help="栅格化字号（默认 = 点阵尺寸 24）")
    ap.add_argument("--charset", default="gb2312,big5-1", help="字符集组合（默认 gb2312,big5-1）")
    ap.add_argument("--out", default=None, help="汉字表输出（默认 platform/gk7205v200/gk_font_cjk.h）")
    ap.add_argument("--ascii-out", default=None,
                    help="ASCII 表输出（默认 platform/gk7205v200/gk_font_ascii.h）")
    ap.add_argument("--no-ascii", action="store_true", help="只生成汉字表")
    ap.add_argument("--cjk-hi-charset", default="gb2312-1",
                    help="高分辨率汉字表的字符集（默认 gb2312-1 = 最常用的 3755 字）")
    ap.add_argument("--cjk-hi-size", type=int, default=48,
                    help="高分辨率汉字表的点阵边长（默认 48；与主表 24 同为格宽基准的倍数）")
    ap.add_argument("--cjk-hi-out", default=None,
                    help="高分辨率汉字表输出（默认 platform/gk7205v200/gk_font_cjk_hi.h）")
    ap.add_argument("--no-cjk-hi", action="store_true", help="不生成高分辨率汉字表")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    fw = os.path.dirname(here)                      # firmware/
    out = args.out or os.path.join(fw, "platform", "gk7205v200", "gk_font_cjk.h")
    font_path = args.font or next((p for p in FONT_CANDIDATES if os.path.exists(p)), None)
    if not font_path or not os.path.exists(font_path):
        sys.exit("找不到字体文件，请用 --font 指定（需 OFL 等可再分发的开源字体）")

    cps = build_charset(args.charset.split(","))
    print("字符集：%s → %d 个码点（已去重、去掉 ASCII）" % (args.charset, len(cps)))

    font = make_font(font_path, args.size, args.wght)
    t0 = time.time()
    glyphs, ink_total, blanks = [], 0, 0
    for i, cp in enumerate(cps):
        rows, ink = raster(font, cp)
        if ink < 4:                                  # 缺字（.notdef/空白）：剔除，别让通道名出现空洞
            blanks += 1
            continue
        glyphs.append((cp, rows))
        ink_total += ink
        if (i + 1) % 2000 == 0:
            print("  已渲染 %5d/%d …" % (i + 1, len(cps)))
    print("渲染完成：%d 字（剔除无字形 %d 个），平均墨点 %.1f，用时 %.1fs"
          % (len(glyphs), blanks, ink_total / max(1, len(glyphs)), time.time() - t0))

    final_cps = [cp for cp, _ in glyphs]          # 与点阵一一对应（可能剔除了缺字）
    points = "\n".join("    " + " ".join("0x%04X," % cp for cp in part)
                       for part in [final_cps[i:i + 10] for i in range(0, len(final_cps), 10)])
    rows_txt = []
    for cp, rows in glyphs:
        # 一个字形 = 一行字符串（每字节 \xXX，后面总跟 \x 或引号，不会与后续 hex 字符歧义）
        cells = "".join("".join("\\x%02X" % b for b in row) for row in rows)
        rows_txt.append('    /* U+%04X */ "%s",' % (cp, cells))
    kb = len(glyphs) * CELL * ROWS_BYTES / 1024.0
    cmd = " ".join(sys.argv)
    text = HEADER.format(ext="h", date=time.strftime("%Y-%m-%d"), cmd=cmd,
                         count=len(glyphs), cell=CELL, rows=CELL, rb=ROWS_BYTES, kb=kb,
                         fontname=os.path.basename(font_path), wght=args.wght, size=args.size,
                         coverage=args.charset, points=points, glyphs="\n".join(rows_txt))
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("已生成 %s（%.1f KB 数据，%d 字）" % (out, kb, len(glyphs)))

    if not args.no_ascii:
        gen_ascii(font_path,
                  args.ascii_out or os.path.join(fw, "platform", "gk7205v200", "gk_font_ascii.h"),
                  args.wght, cmd)

    if not args.no_cjk_hi:
        # 高分辨率表给**常用字**：整个 9520 字全做要 ≈9.6MB（放不下），只做一级字
        # ≈1.1MB，生僻字自动回落 24×24。
        gen_cjk_hi(font_path,
                   args.cjk_hi_out or os.path.join(fw, "platform", "gk7205v200",
                                                   "gk_font_cjk_hi.h"),
                   args.wght, cmd, args.cjk_hi_size, args.cjk_hi_charset)


if __name__ == "__main__":
    main()
