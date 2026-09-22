#!/usr/bin/env python3
"""把 web/ 下的静态资源转成可内嵌固件的 C 字节数组。

gzip 预压缩：flash 空间宝贵，且 100Mbps 网口下传输也更快。
浏览器全都支持 Content-Encoding: gzip，无需保留未压缩副本。

开发者手动运行、生成物提交进 git，不接入 CMake 构建流程：
    python firmware/scripts/gen_assets.py firmware/web firmware/modules/console/console_assets.c
"""
import gzip
import hashlib
import pathlib
import sys

CONTENT_TYPES = {
    '.html': 'text/html; charset=utf-8',
    '.js':   'application/javascript; charset=utf-8',
    '.css':  'text/css; charset=utf-8',
    '.svg':  'image/svg+xml',
    '.ico':  'image/x-icon',
    '.png':  'image/png',
}


def c_array(data: bytes) -> str:
    out = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        out.append('    ' + ' '.join(f'0x{b:02x},' for b in chunk))
    return '\n'.join(out)


def main() -> int:
    if len(sys.argv) != 3:
        print(f'用法: {sys.argv[0]} <web 源目录> <输出 .c 路径>', file=sys.stderr)
        return 2

    src = pathlib.Path(sys.argv[1])
    dst = pathlib.Path(sys.argv[2])
    if not src.is_dir():
        print(f'错误: 源目录不存在: {src}', file=sys.stderr)
        return 1

    files = sorted(p for p in src.rglob('*') if p.is_file())
    if not files:
        print(f'错误: {src} 下没有文件', file=sys.stderr)
        return 1

    parts = [
        '/**',
        ' * @file console_assets.c',
        ' * @brief 内嵌前端资源（由 scripts/gen_assets.py 生成，请勿手工编辑）',
        ' *',
        ' * 资源经 gzip 预压缩，响应时直接带 Content-Encoding: gzip 发出。',
        ' */',
        '#include "console_assets.h"',
        '',
    ]

    entries = []
    total_raw = total_gz = 0

    for path in files:
        raw = path.read_bytes()
        # mtime=0 让输出可复现：同样的输入必须产生同样的字节，否则每次
        # 构建都会产生无意义的 git 差异
        gz = gzip.compress(raw, compresslevel=9, mtime=0)
        total_raw += len(raw)
        total_gz += len(gz)

        rel = path.relative_to(src).as_posix()
        sym = 'asset_' + rel.replace('/', '_').replace('.', '_').replace('-', '_')
        ctype = CONTENT_TYPES.get(path.suffix, 'application/octet-stream')
        etag = hashlib.sha256(raw).hexdigest()[:16]

        parts.append(f'/* {rel}: 原始 {len(raw)} 字节，gzip 后 {len(gz)} 字节 */')
        parts.append(f'static const unsigned char {sym}[] = {{')
        parts.append(c_array(gz))
        parts.append('};')
        parts.append('')
        entries.append((f'/{rel}', ctype, sym, len(gz), etag))

    parts.append('static const console_asset_t s_assets[] = {')
    for url, ctype, sym, n, etag in entries:
        parts.append(f'    {{ "{url}", "{ctype}", {sym}, {n}u, 1, "{etag}" }},')
    parts.append('    { 0, 0, 0, 0u, 0, 0 }')
    parts.append('};')
    parts.append('')
    parts.append('const console_asset_t *console_asset_index(void) { return s_assets; }')
    parts.append('')
    parts.append('const console_asset_t *console_asset_find(const char *path)')
    parts.append('{')
    parts.append('    const console_asset_t *a;')
    parts.append('    if (!path) return 0;')
    parts.append('    for (a = s_assets; a->path; a++) {')
    parts.append('        const char *p = a->path, *q = path;')
    parts.append('        while (*p && *p == *q) { p++; q++; }')
    parts.append('        if (*p == 0 && *q == 0) return a;')
    parts.append('    }')
    parts.append('    return 0;')
    parts.append('}')
    parts.append('')

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text('\n'.join(parts), encoding='utf-8')
    print(f'已生成 {dst}：{len(entries)} 个资源，原始 {total_raw} 字节，gzip 后 {total_gz} 字节')
    return 0


if __name__ == '__main__':
    sys.exit(main())
