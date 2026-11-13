# -*- coding: utf-8 -*-
"""ETS2 读档耗时分析 — 解析 game.log.txt，输出各阶段时间轴与最大停顿。
用法: python logtime.py [game.log.txt 路径]
"""
import re, sys, os, collections

path = sys.argv[1] if len(sys.argv) > 1 else r"G:\DOCUMENTS\Euro Truck Simulator 2\game.log.txt"
if not os.path.exists(path):
    sys.exit("找不到日志: " + path)

rows = []
for ln in open(path, encoding='utf-8', errors='ignore'):
    m = re.match(r'(\d+):(\d+):(\d+\.\d+) : (.*)', ln.rstrip('\n'))
    if m:
        rows.append((int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3)), m.group(4)))

if not rows:
    sys.exit("日志里没有带时间戳的行")

print("=" * 68)
print("ETS2 读档耗时分析")
print("=" * 68)
print("日志     : %s" % path)
print("总跨度   : %.2f 秒（进程内时间）" % rows[-1][0])
print("日志行数 : %d" % len(rows))

errs = [l for t, l in rows if '<ERROR>' in l]
print("ERROR    : %d 条" % len(errs))
if errs:
    c = collections.Counter(re.sub(r'^<ERROR>\s*', '', l)[:80] for l in errs)
    for k, v in c.most_common(5):
        print("           %4d x %s" % (v, k))

# ---- 关键阶段 ----
first = lambda pred: next((t for t, l in rows if pred(l)), None)
marks = [
    ("进程启动", lambda l: True),
    ("配置文件已选 (第一次)", lambda l: l.startswith('New profile selected')),
    ("mod 枚举开始 (Active N mods)", lambda l: l.startswith('[mods] Active ') and ' mods (' in l),
    ("开始挂载 mod", lambda l: 'has been mounted' in l),
    ("加载存档 (Loading save)", lambda l: l.startswith('Loading save')),
    ("货物分配完成", lambda l: 'Loading company cargo' in l),
    ("物理引擎初始化", lambda l: '[physx] @0: Starting server' in l),
    ("进入驾驶室 (可玩)", lambda l: "Desktop screen config switched to 'truck'" in l),
]
print("\n--- 关键节点 ---")
seen = set()
for name, pred in marks:
    t = first(pred)
    if t is not None and name not in seen:
        seen.add(name)
        print("  %-26s %8.2f s" % (name, t))

# ---- 最大停顿 ----
gaps = []
for i in range(1, len(rows)):
    d = rows[i][0] - rows[i - 1][0]
    if d > 0.5:
        gaps.append((d, rows[i - 1], rows[i]))
gaps.sort(reverse=True)

print("\n--- 最大停顿 TOP 6 ---")
for d, a, b in gaps[:6]:
    print("  %7.2f s  @ %7.2f" % (d, a[0]))
    print("      前: %s" % a[1][:88])
    print("      后: %s" % b[1][:88])

# ---- 挂载统计 ----
mounts = [(t, l) for t, l in rows if 'has been mounted' in l]
if mounts:
    print("\n--- mod 挂载 ---")
    print("  次数    : %d" % len(mounts))
    print("  时间窗口: %.2f s -> %.2f s" % (mounts[0][0], mounts[-1][0]))
    print("  挂载耗时: %.2f s" % (mounts[-1][0] - mounts[0][0]))

# ---- 静默期（无任何日志输出的区间） ----
print("\n--- 静默期 (>2s 无输出) ---")
for d, a, b in gaps:
    if d > 2:
        print("  %6.2f s   %7.2f -> %7.2f   [%s ...]" % (d, a[0], b[0], a[1][:46]))
