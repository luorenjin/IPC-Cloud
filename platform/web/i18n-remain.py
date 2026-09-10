# -*- coding: utf-8 -*-
"""精确列出「非注释」的中文残留，用于人工判定哪些是合理保留。"""
import io, re, glob

CN = re.compile(r'[一-龥]')

def code_only(src):
    """去掉整行注释、行尾注释、块注释体，只留可执行代码"""
    out, inblk = [], False
    for l in src.split('\n'):
        t = l.strip()
        if inblk:
            if '*/' in t:
                inblk = False
            continue
        if t.startswith('/*') and '*/' not in t:
            inblk = True
            continue
        if t.startswith(('//', '*', '<!--')):
            continue
        if t.startswith('/*') and t.endswith('*/'):
            continue
        out.append(re.sub(r'\s//.*$', '', l))
    return '\n'.join(out)

files = (sorted(glob.glob('pages/**/*.vue', recursive=True))
         + sorted(glob.glob('layouts/*.vue'))
         + sorted(glob.glob('components/**/*.vue', recursive=True))
         + sorted(glob.glob('composables/*.ts'))
         + sorted(glob.glob('utils/*.ts')))

rows = []
for p in files:
    if 'locales' in p:
        continue
    c = code_only(io.open(p, encoding='utf-8').read())
    hits = [(i, l.strip()) for i, l in enumerate(c.split('\n'), 1) if CN.search(l)]
    if hits:
        rows.append((p.replace('\\', '/'), hits))

tot = sum(len(h) for _, h in rows)
buf = [f'非注释中文行合计 {tot} 处，分布在 {len(rows)} 个文件\n']
for p, hits in rows:
    buf.append(f'=== {p} ({len(hits)}) ===')
    for i, l in hits:
        buf.append(f'  {i}: {l[:130]}')
io.open('remain2.txt', 'w', encoding='utf-8').write('\n'.join(buf))
print(f'files={len(rows)} lines={tot}')
