# -*- coding: utf-8 -*-
"""
注意：须在 platform/web 目录下运行（脚本用相对路径 glob）：
    cd platform/web && python scripts/i18n-check.py

i18n 一致性检查（结果写入 UTF-8 文件，终端编码不可靠）：
 1. 中英词条键是否一一对应
 2. 是否有重复键（模块间撞键会被后者静默覆盖）
 3. 页面里 t('xxx') 引用的键是否都存在
 4. 是否有定义了却没人用的键
"""
import io, re, glob, os

KEY = re.compile(r"^\s*'([^']+)'\s*:", re.M)

def load(locale):
    """返回 {key: (module, value)}，并报告模块内/模块间重复键"""
    out, dup = {}, []
    for p in sorted(glob.glob(f'locales/{locale}/*.ts')):
        mod = os.path.basename(p)
        if mod == 'index.ts':
            continue
        s = io.open(p, encoding='utf-8').read()
        for m in KEY.finditer(s):
            k = m.group(1)
            if k in out:
                dup.append(f'{k}  ({out[k][0]} vs {mod})')
            out[k] = (mod, s[m.end():s.find('\n', m.end())].strip())
    return out, dup

zh, zh_dup = load('zh-CN')
en, en_dup = load('en')

# 页面里实际引用的键
used = set()
for p in (glob.glob('pages/**/*.vue', recursive=True) + glob.glob('layouts/*.vue')
          + glob.glob('components/**/*.vue', recursive=True) + glob.glob('composables/*.ts')
          + glob.glob('utils/*.ts')):
    s = io.open(p, encoding='utf-8').read()
    # 去掉注释行：文档注释里的示例（如 useI18n.ts 的用法说明）不算真实引用
    s = chr(10).join('' if l.strip().startswith(('//', '*', '/*', '<!--')) else l
                     for l in s.split(chr(10)))
    # 用 (?<![\w.]) 排除 get( / post( / put( 等以 t 结尾的调用，避免把 API 路径当词条
    used |= set(re.findall(r"(?<![\w.])t\(\s*'([^']+)'", s))

# 动态构造的键（模板字符串）无法静态解析，这些前缀一律豁免
DYNAMIC_PREFIX = ('enum.day.', 'enum.source.', 'enum.alarmKind.', 'enum.alarmLevel.',
                  'enum.deviceStatus.', 'enum.nodeStatus.', 'enum.streamStatus.',
                  'enum.result.', 'enum.cap.', 'enum.audit.', 'task.status.', 'task.type.')

missing_en = sorted(set(zh) - set(en))
missing_zh = sorted(set(en) - set(zh))
undefined = sorted(k for k in used if k not in zh and not k.startswith(DYNAMIC_PREFIX))
unused = sorted(k for k in zh if k not in used and not k.startswith(DYNAMIC_PREFIX))

with io.open('i18n-check.txt', 'w', encoding='utf-8') as f:
    f.write(f'中文词条 {len(zh)}  英文词条 {len(en)}  页面引用 {len(used)}\n\n')
    def sec(title, items, show=40):
        f.write(f'== {title}（{len(items)}）==\n')
        for x in items[:show]:
            f.write(f'  {x}\n')
        if len(items) > show:
            f.write(f'  … 另有 {len(items)-show} 条\n')
        f.write('\n')
    sec('英文缺失', missing_en)
    sec('中文缺失', missing_zh)
    sec('重复键(zh)', zh_dup)
    sec('重复键(en)', en_dup)
    sec('引用了但未定义', undefined)
    sec('定义了但未引用', unused, show=25)

bad = len(missing_en) + len(missing_zh) + len(zh_dup) + len(en_dup) + len(undefined)
print(f'zh={len(zh)} en={len(en)} used={len(used)} '
      f'missing_en={len(missing_en)} missing_zh={len(missing_zh)} '
      f'dup={len(zh_dup)+len(en_dup)} undefined={len(undefined)} unused={len(unused)}')
print('RESULT:', 'OK' if bad == 0 else 'HAS_ISSUES')
