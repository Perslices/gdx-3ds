#!/usr/bin/env python3
"""c5tick_agg.py -- read the C5TICK receipts (port/3ds/gdx3ds_c5tick.h).

    tools/native/c5tick_agg.py dlb LOG [--race] [--from FRAME] [--to FRAME]
    tools/native/c5tick_agg.py dlw LOG --elf G-Diffuser-3DS.elf [--all]
    tools/native/c5tick_agg.py hollow LOG

dlb     means of the [c5dlb] windows (64 host iterations each), split by kind (rend = a tick
        whose display list is walked, skip = a tick frameskip dropped; hollow ticks are skip
        ticks). ms per tick of that kind. --race keeps windows whose sky+trk >= 0.2 ms (the
        race scene was drawn), per kind.
dlw     the write-set check: every [dlw] block mapped to the symbols it overlaps (nm -S -l on
        the ELF, so each symbol carries its source file), grouped by section. Blocks the NOISE
        control saw (written concurrently by another thread) are listed separately; a section's
        remaining blocks are what it CHANGED in the game's static image. By default symbols from
        the instrumentation itself (gdx3ds_c5tick.c, the file log) are dropped; --all keeps them.
hollow  the [c5tick] hollow / leak counters.
gstate  A B: join two runs' [gstate] lines on tick= and compare hash= (and ext= when both
        carry it, [debug] gstate_ext=1). Prints compared / divergent / the first divergences.
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys

SECS = ['other', 'rhead', 'sky', 'trk', 'dress', 'rmid', 'cam', 'vis', 'shd', 'body', 'boost',
        'efx', 'rhud', 'p1hud', 'menus', 'trans']
NM = '/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm'
INSTRUMENT_FILES = ('gdx3ds_c5tick.c', 'gdx3ds_filelog.c', 'gdx3ds_mainprof.c', 'gdx3ds_hitch')


def parse_dlb(path):
    wins = []
    for line in open(path, errors='replace'):
        if not line.startswith('[c5dlb] frame='):
            continue
        kv = dict(re.findall(r'(\w+)=([\w.\-]+)', line))
        wins.append(kv)
    return wins


def cmd_dlb(args):
    wins = parse_dlb(args.log)
    by_kind = collections.defaultdict(list)
    for w in wins:
        frame = int(w['frame'])
        if args.start is not None and frame < args.start:
            continue
        if args.end is not None and frame > args.end:
            continue
        if args.race and float(w['sky']) + float(w['trk']) < 0.2:
            continue
        by_kind[w['kind']].append(w)
    for kind in ('rend', 'skip'):
        rows = by_kind.get(kind, [])
        if not rows:
            print(f'{kind}: no windows')
            continue
        ticks = sum(int(r['n']) for r in rows)
        means = {}
        for key in ['tot'] + SECS:
            means[key] = sum(float(r[key]) * int(r['n']) for r in rows) / ticks
        hollow = sum(int(r['hollow']) for r in rows)
        print(f'{kind}: windows={len(rows)} ticks={ticks} hollow={hollow} tot={means["tot"]:.3f} ms/tick')
        ranked = sorted(SECS, key=lambda k: -means[k])
        print('   ' + ' '.join(f'{k}={means[k]:.3f}' for k in ranked if means[k] >= 0.0005))


def load_symbols(elf):
    out = subprocess.run([NM, '-S', '-l', '-n', elf], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(None, 4)
        if len(parts) < 4 or parts[2] not in 'bBdDgGsS':
            continue
        addr, size, name = int(parts[0], 16), int(parts[1], 16), parts[3]
        src = ''
        if len(parts) == 5:
            src = parts[4]
        elif '\t' in name:
            name, src = name.split('\t', 1)
        if '\t' in name:
            name, src = name.split('\t', 1)
        syms.append((addr, size, name, src.strip()))
    syms.sort()
    return syms


def overlapping(syms, starts, lo, hi):
    i = bisect.bisect_right(starts, lo) - 1
    i = max(i, 0)
    found = []
    while i < len(syms) and syms[i][0] < hi:
        addr, size, name, src = syms[i]
        if addr + max(size, 1) > lo:
            found.append(syms[i])
        i += 1
    return found


def short_src(src):
    src = src.split(':')[0]
    for mark in ('/decomp/', '/port/', '/libultraship/', '/vendor/'):
        if mark in src:
            return mark.strip('/') + '/' + src.split(mark, 1)[1]
    return src.rsplit('/', 1)[-1] if src else '?'


def cmd_dlw(args):
    blk = None
    hits = collections.defaultdict(dict)  # sec -> block -> (frame, mode)
    for line in open(args.log, errors='replace'):
        m = re.search(r'\[c5tick\] dlwrite armed: .* blk=(\d+)', line)
        if m:
            blk = int(m.group(1))
        m = re.match(r'\[dlw\] sec=(\w+) blk=0x([0-9a-f]+) frame=(\d+) mode=(-?\d+)', line)
        if m:
            hits[m.group(1)].setdefault(int(m.group(2), 16), (int(m.group(3)), int(m.group(4))))
    if blk is None:
        sys.exit('no "[c5tick] dlwrite armed" line: not a dlwrite run')
    syms = load_symbols(args.elf)
    starts = [s[0] for s in syms]
    noise = set(hits.get('noise', {}))
    print(f'block={blk} B, noise blocks={len(noise)}')
    for sec in SECS + ['noise']:
        blocks = hits.get(sec, {})
        if not blocks:
            continue
        per_sym = collections.OrderedDict()
        noisy = 0
        for b in sorted(blocks):
            if sec != 'noise' and b in noise:
                noisy += 1
                continue
            frame, mode = blocks[b]
            for addr, size, name, src in overlapping(syms, starts, b, b + blk) or [(b, 0, '?', '')]:
                if not args.all and any(f in src for f in INSTRUMENT_FILES):
                    continue
                key = (name, short_src(src))
                if key not in per_sym:
                    per_sym[key] = (frame, mode, b)
        print(f'\n== {sec}: {len(blocks)} blocks ({noisy} also in noise)')
        for (name, src), (frame, mode, b) in per_sym.items():
            print(f'   {name:40s} {src:50s} first frame={frame} mode={mode} blk=0x{b:08x}')


def cmd_hollow(args):
    last = None
    leaks = []
    for line in open(args.log, errors='replace'):
        if line.startswith('[c5tick] frame='):
            last = line.strip()
        if 'LEAK' in line and line.startswith('[c5tick]'):
            leaks.append(line.strip())
        if line.startswith('[c5tick] dl_hollow='):
            print(line.strip())
    print(last or 'no [c5tick] window line')
    print(f'leak lines: {len(leaks)}')
    for line in leaks[:10]:
        print('  ' + line)


def parse_gstate(path):
    rows = {}
    for line in open(path, errors='replace'):
        if not line.startswith('[gstate] frame='):
            continue
        kv = dict(re.findall(r'(\w+)=(\w+)', line))
        rows[int(kv['tick'])] = kv
    return rows


def cmd_gstate(args):
    a = parse_gstate(args.a)
    b = parse_gstate(args.b)
    common = sorted(set(a) & set(b))
    ext = all('ext' in a[t] and 'ext' in b[t] for t in common) and bool(common)
    bad = []
    for t in common:
        keys = ['hash', 'mode', 'racers'] + (['ext'] if ext else [])
        diff = [k for k in keys if a[t].get(k) != b[t].get(k)]
        if diff:
            bad.append((t, diff))
    print(f'gstate: a={len(a)} b={len(b)} compared={len(common)} divergent={len(bad)} '
          f'ext={"yes" if ext else "no"} last_common={common[-1] if common else None}')
    for t, diff in bad[:10]:
        print(f'  tick={t} differs in {",".join(diff)}: a={a[t]} b={b[t]}')
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('dlb')
    p.add_argument('log')
    p.add_argument('--race', action='store_true')
    p.add_argument('--from', dest='start', type=int)
    p.add_argument('--to', dest='end', type=int)
    p = sub.add_parser('dlw')
    p.add_argument('log')
    p.add_argument('--elf', required=True)
    p.add_argument('--all', action='store_true')
    p = sub.add_parser('hollow')
    p.add_argument('log')
    p = sub.add_parser('gstate')
    p.add_argument('a')
    p.add_argument('b')
    args = ap.parse_args()
    rc = {'dlb': cmd_dlb, 'dlw': cmd_dlw, 'hollow': cmd_hollow, 'gstate': cmd_gstate}[args.cmd](args)
    sys.exit(rc or 0)


if __name__ == '__main__':
    main()
