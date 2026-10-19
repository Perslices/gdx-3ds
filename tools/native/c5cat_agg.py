#!/usr/bin/env python3
"""c5cat_agg.py -- summarise [c5cat] receipts (C5 Stage 0 per-category CPU timers).

    tools/native/c5cat_agg.py LOG [--md N ...] [--race] [--clean]

Each [c5cat] window (64 host iterations) is tagged with the scene of the next [gpu] line in FILE
ORDER (1 = GP race, 14 = time attack, 3/5 = 2P/4P demo). The two frame counters are NOT
comparable: [c5cat] frame= counts host iterations (logic ticks), [gpu] frame= counts rendered
frames, and under frameskip they diverge 2-4x (r60-research.md §1). Options:
    --md N     keep only these scenes (repeatable; default: every scene, one block each)
    --race     keep only windows that actually drew the race scene (sky + trk >= 0.5 ms)
    --median   print window medians instead of means (use for budgets: the sync file-log flush
               spikes inflate means, not medians)
    --clean    also drop windows holding an oracle SHOT/[fh] readback or a load spike
               (tail > 1 ms or fend > 2 ms), for emulator replays

Per block: the window means of every column, the render-work shares (misc..fend, the columns
the retained renderer moves), the partition closure (max |res|) and, with [debug] c5steal=1,
the mean core-0 steal. Column meanings are in port/3ds/gdx3ds_mainprof.h.
"""
import argparse
import re
from collections import defaultdict

TICK = ['logic', 'dlb', 'aud', 'ld', 'vi', 'inp', 'apt', 'pace', 'tail', 'task', 'idle']
REND = ['fbeg', 'misc', 'sky', 'trk', 'dress', 'mach', 'mesh', 'shd', 'fx', 'hud', 'mir', 'swp',
        'fend']
WORK = REND[1:]  # render work, without the frame-begin waits


def parse(path):
    gpu, windows, steal = [], [], []
    pending = []
    for line in open(path, errors='replace'):
        m = re.search(r'\[gpu\] frame=(\d+) .* md=(-?\d+)', line)
        if m:
            md = int(m.group(2))
            gpu.append((int(m.group(1)), md))
            for win in pending:
                win['_md'] = md
            pending = []
            continue
        if not line.startswith('[c5cat] frame='):
            continue
        kv = {k: float(v) for k, v in re.findall(r'(\w+)=(-?[\d.]+)', line)}
        if 'steal' in kv:
            steal.append(kv)
        elif 'wall' in kv:
            windows.append(kv)
            pending.append(kv)
    last = gpu[-1][1] if gpu else None
    for win in pending:
        win['_md'] = last
    return gpu, windows, steal


def keep(win, args):
    if args.race and win.get('sky', 0.0) + win.get('trk', 0.0) < 0.5:
        return False
    if args.clean and (win.get('tail', 0.0) > 1.0 or win.get('fend', 0.0) > 2.0):
        return False
    return True


def report(md, rows, steal, median=False):
    n = len(rows)

    def mean(key):
        vals = sorted(r.get(key, 0.0) for r in rows)
        if median:
            mid = n // 2
            if n % 2:
                return vals[mid]
            return (vals[mid - 1] + vals[mid]) / 2
        return sum(vals) / n

    work = sum(mean(k) for k in WORK)
    stat = 'median' if median else 'mean'
    print(f"md={md} [{stat}] windows={n} wall={mean('wall'):.2f} rf={mean('rf'):.2f} "
          f"r/n={mean('r') / mean('n'):.2f} max|res|={max(abs(r['res']) for r in rows):.3f} "
          f"tk={mean('tk'):.0f} ovh={mean('ovh'):.2f} renderwork={work:.2f}")
    print('  tick   ' + ' '.join(f"{k}={mean(k):.2f}" for k in TICK))
    print('  render ' + ' '.join(f"{k}={mean(k):.2f}" for k in REND))
    if work > 0:
        print('  share  ' + ' '.join(f"{k}={100 * mean(k) / work:.0f}%" for k in WORK))
    if steal:
        s = sum(x['steal'] for x in steal) / len(steal)
        g = sum(x['gaps'] for x in steal) / len(steal)
        print(f"  steal  {s:.2f}% of core 0 (mean over {len(steal)} probes, {g:.1f} gaps/probe)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--md', type=int, action='append')
    ap.add_argument('--race', action='store_true')
    ap.add_argument('--clean', action='store_true')
    ap.add_argument('--median', action='store_true')
    args = ap.parse_args()
    gpu, windows, steal = parse(args.log)
    blocks = defaultdict(list)
    for win in windows:
        md = win['_md']
        if args.md and md not in args.md:
            continue
        if keep(win, args):
            blocks[md].append(win)
    if not blocks:
        print('no [c5cat] windows matched')
        return
    for md in sorted(blocks, key=lambda x: (x is None, x)):
        report(md, blocks[md], steal, args.median)


if __name__ == '__main__':
    main()
