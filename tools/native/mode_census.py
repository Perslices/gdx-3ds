#!/usr/bin/env python3
"""mode_census.py LOG: [c5cat] windows (receipt_every=64) tagged by game mode from per-tick
[gstate] lines; per mode: windows, median rf (ms per rendered tick), median per-bucket render ms,
ex-harness = rf - tail (tail carries the oracle's [gstate]/[fh] hashing), and the [ninterp]
counts per mode. Run with [debug] receipt_every=64 mainprof=1 gstate_every=1 frameskip=0
(docs/research/native/n60misc-progress.md section 3)."""
import re, sys, statistics as st
from collections import defaultdict
MODES = {0:'TITLE',1:'GP_RACE',2:'PRACTICE',3:'VS_2P',4:'VS_3P',5:'VS_4P',6:'RECORDS',7:'MAIN_MENU',
         8:'MACHINE_SELECT',9:'MACHINE_SETTINGS',10:'COURSE_SELECT',11:'CREDITS',12:'CREDITS2',13:'COURSE_EDIT',
         14:'TIME_ATTACK',15:'GP_NEXT_COURSE',16:'CREATE_MACHINE',17:'GP_END_CS',18:'GP_NEXT_MACH_SET',
         19:'RECORDS_COURSE_SEL',20:'OPTIONS',21:'DEATH_RACE',22:'EAD_DEMO'}
REND = ['fbeg','misc','sky','trk','dress','mach','mesh','shd','fx','hud','mir','swp','fend']
TICK = ['logic','dlb','aud','ld','vi','inp','apt','tail','task']
modeAt = {}
wins = []
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'\[gstate\] frame=\d+ tick=(\d+) mode=(\d+)', line)
    if m:
        modeAt[int(m.group(1))] = int(m.group(2)); continue
    if line.startswith('[c5cat] frame=') and 'wall=' in line:
        kv = {k: float(v) for k, v in re.findall(r'(\w+)=(-?[\d.]+)', line)}
        wins.append(kv)
by = defaultdict(list)
for w in wins:
    end = int(w['frame']); n = int(w['n'])
    ms = {modeAt.get(t) for t in range(end - n + 1, end + 1)}
    ms.discard(None)
    key = MODES.get(ms.pop(), '?') if len(ms) == 1 else 'mixed/transition'
    by[key].append(w)
print('| mode | windows | rf p50 | ex-harness p50 | ex-harness max | logic+dlb | fbeg | misc | mesh | hud | race cats | fend |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|')
for key, ws in sorted(by.items(), key=lambda kv: -len(kv[1])):
    med = lambda c: st.median(w.get(c, 0.0) for w in ws)
    ex = [w['rf'] - w.get('tail', 0.0) for w in ws]
    cats = st.median(sum(w.get(c, 0.0) for c in ['sky','trk','dress','mach','shd','fx','mir','swp']) for w in ws)
    print(f"| {key} | {len(ws)} | {med('rf'):.1f} | {st.median(ex):.1f} | {max(ex):.1f} | {med('logic')+med('dlb'):.1f} | {med('fbeg'):.1f} | "
          f"{med('misc'):.1f} | {med('mesh'):.1f} | {med('hud'):.1f} | {cats:.1f} | {med('fend'):.1f} |")

# [ninterp] receipts (per 64 interpreter tasks), tagged with the mode of the last [gstate] seen.
cur = None; ni = defaultdict(list)
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'\[gstate\] frame=\d+ tick=\d+ mode=(\d+)', line)
    if m:
        cur = int(m.group(1)); continue
    if line.startswith('[ninterp] tasks='):
        kv = {k: float(v) for k, v in re.findall(r'(\w+)=(\d+)', line.split('|')[0] + line.split('|')[1])}
        ni[MODES.get(cur, '?')].append(kv)
print()
print('| mode | receipts | lists/task | cmds/task | rect/task | tri/task | vtx/task | tmem/task | batches/task |')
print('|---|---|---|---|---|---|---|---|---|')
for key, ws in sorted(ni.items(), key=lambda kv: -len(kv[1])):
    pt = lambda c: st.median(w.get(c, 0) / max(w.get('tasks', 1), 1) for w in ws)
    print(f"| {key} | {len(ws)} | {pt('lists'):.1f} | {pt('cmds'):.0f} | {pt('rect'):.0f} | {pt('tri'):.0f} | {pt('vtx'):.0f} | {pt('tmem'):.0f} | {pt('batches'):.1f} |")
