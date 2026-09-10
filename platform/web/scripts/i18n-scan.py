# -*- coding: utf-8 -*-
"""
注意：须在 platform/web 目录下运行（脚本用相对路径 glob）：
    cd platform/web && python scripts/i18n-scan.py
扫描仍含中文的源文件，输出到 UTF-8 文件（终端编码不可靠，一律写文件再读）。"""
import io, re, glob, sys, os

CN = re.compile(r'[一-龥]')

def strip_comments(src: str) -> str:
    out = []
    for line in src.split('\n'):
        t = line.strip()
        if t.startswith('//') or t.startswith('*') or t.startswith('/*') or t.startswith('<!--'):
            out.append('')
        else:
            # 去掉行尾 // 注释（粗略：不处理字符串里的 //，够用）
            out.append(re.sub(r'\s//\s[^\'"`]*$', '', line))
    return '\n'.join(out)

def scan(paths):
    rows = []
    for p in paths:
        s = io.open(p, encoding='utf-8').read()
        body = strip_comments(s)
        n = len(CN.findall(body))
        if n:
            rows.append((p.replace('\\', '/'), n, len(re.findall(r"\bt\(", body))))
    rows.sort(key=lambda r: -r[1])
    return rows

targets = (sorted(glob.glob('pages/**/*.vue', recursive=True))
           + sorted(glob.glob('layouts/*.vue'))
           + sorted(glob.glob('components/**/*.vue', recursive=True))
           + sorted(glob.glob('composables/*.ts'))
           + sorted(glob.glob('utils/*.ts')))
targets = [t for t in targets if 'locales' not in t]

rows = scan(targets)
total = sum(r[1] for r in rows)
with io.open('i18n-report.txt', 'w', encoding='utf-8') as f:
    f.write(f'仍含中文的文件 {len(rows)} 个，中文字符合计 {total}\n\n')
    for p, n, t in rows:
        f.write(f'{n:6d}  t()={t:<4d} {p}\n')
print(f'files={len(rows)} chars={total}')
