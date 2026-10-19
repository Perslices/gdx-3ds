#!/usr/bin/env python3
"""ships_agg.py -- the per-racer cost from one log: [c5cat] windows joined with the n60ships receipts.

    tools/native/ships_agg.py LOG [--md N ...] [--ab 0|1] [--lines A-B] [--by near|vis|none]

Joins, by `frame=` (they share the [c5cat] window when its clock is armed: c5pmu / gputrace /
mainprof), each [c5cat] line with the [ships] census (`[debug] ships_census=1`), the [lsplit]
logic split (`[debug] logic_split=1`) and the [ab] phase (`[debug] nrr_ab`). Then, per game mode
(`md` from the census: 1 GP, 14 time attack, 3/5 VS) and per bin of ships on screen, the window
medians of the tick and render categories and of the logic split.

    --md N      keep only these modes (repeatable)
    --ab P      keep only windows whose [ab] phase is P (time-sliced card A/B: 0 = levers off)
    --lines A-B keep windows by log line (one play segment)
    --by near   bin by near machines on screen (LOD 1-5: meshes) -- the default
    --by vis    bin by all visible machines (meshes + far specks)
    --by none   one block per mode

Race windows only: [c5cat] sky + trk >= 0.5 ms (the hwprof_agg `--race` rule). Stdlib only.
"""
import argparse
import re
import statistics as st
from collections import defaultdict

TICK = ['logic', 'dlb', 'aud', 'ld', 'vi', 'inp', 'tail', 'task']
REND = ['fbeg', 'misc', 'sky', 'trk', 'dress', 'mach', 'mesh', 'shd', 'fx', 'hud', 'fend']
SPLIT = ['menus', 'efx', 'rpre', 'coll', 'ai', 'phys', 'rpost', 'cam', 'bg', 'crs', 'oth']
CENSUS = ['act', 'vis', 'l1', 'l2', 'l3', 'l4', 'l5', 'l6', 'shd', 'boost', 'near', 'hl', 'crash', 'ghost']


def kv(text):
    return {k: float(v) for k, v in re.findall(r'(\w+)=(-?[\d.]+)', text)}


def parse(path, lines):
    cat, ships, split, ab = {}, {}, {}, {}
    for no, line in enumerate(open(path, errors='replace'), 1):
        if lines and not (lines[0] <= no <= lines[1]):
            continue
        if line.startswith('[c5cat] frame=') and 'wall=' in line:
            d = kv(line)
            cat[int(d['frame'])] = d
        elif line.startswith('[ships] frame='):
            head, _, rest = line.partition('|')
            d = kv(head)
            c = kv(rest)
            d.update({'c_' + k: v for k, v in c.items()})
            ships[int(d['frame'])] = d
        elif line.startswith('[lsplit] frame='):
            d = kv(line)
            split[int(d['frame'])] = d
        elif line.startswith('[ab] frame='):
            d = kv(line)
            ab[int(d['frame'])] = int(d['phase'])
    return cat, ships, split, ab


def near(c):
    return sum(c.get('c_l%d' % i, 0.0) for i in range(1, 6))


def bin_of(sh, by):
    if by == 'none' or sh is None:
        return 'all'
    v = near(sh) if by == 'near' else sh.get('c_vis', 0.0)
    if by == 'near':
        edges = [(0, 1.5, '<=1.5'), (1.5, 2.5, '1.5-2.5'), (2.5, 3.5, '2.5-3.5'), (3.5, 99, '>3.5')]
    else:
        edges = [(0, 1.5, '<=1.5'), (1.5, 6, '1.5-6'), (6, 15, '6-15'), (15, 99, '>15')]
    for lo, hi, name in edges:
        if lo <= v < hi:
            return name
    return '?'


def med(rows, key, src):
    vals = [r[src][key] for r in rows if r[src] is not None and key in r[src]]
    return st.median(vals) if vals else float('nan')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--md', type=int, action='append')
    ap.add_argument('--ab', type=int)
    ap.add_argument('--lines')
    ap.add_argument('--by', default='near', choices=['near', 'vis', 'none'])
    args = ap.parse_args()
    lines = tuple(int(x) for x in args.lines.split('-')) if args.lines else None
    cat, ships, split, ab = parse(args.log, lines)
    blocks = defaultdict(list)
    for frame, c in sorted(cat.items()):
        if c.get('sky', 0.0) + c.get('trk', 0.0) < 0.5:
            continue
        sh = ships.get(frame)
        md = int(sh['md']) if sh is not None else -1
        if args.md and md not in args.md:
            continue
        if args.ab is not None and ab.get(frame) != args.ab:
            continue
        row = {'cat': c, 'ships': sh, 'split': split.get(frame)}
        blocks[(md, bin_of(sh, args.by))].append(row)
    if not blocks:
        print('no windows matched')
        return
    for (md, b), rows in sorted(blocks.items()):
        rn = st.median([r['cat']['r'] / r['cat']['n'] for r in rows if r['cat'].get('n')])  # any window length
        print(f"md={md} {args.by}={b} windows={len(rows)} r/n={rn:.2f}")
        print('  census ' + ' '.join(f"{k}={med(rows, 'c_' + k, 'ships'):.2f}" for k in CENSUS))
        print('  tick   ' + ' '.join(f"{k}={med(rows, k, 'cat'):.2f}" for k in TICK))
        print('  render ' + ' '.join(f"{k}={med(rows, k, 'cat'):.2f}" for k in REND))
        ships_ms = sum(med(rows, k, 'cat') for k in ['mach', 'mesh', 'shd', 'fx'])
        print(f"  ships(mach+mesh+shd+fx)={ships_ms:.2f}")
        if any(r['split'] for r in rows):
            print('  lsplit ' + ' '.join(f"{k}={med(rows, k, 'split'):.3f}" for k in SPLIT) +
                  f" reads={med(rows, 'reads', 'split'):.0f}")


if __name__ == '__main__':
    main()
