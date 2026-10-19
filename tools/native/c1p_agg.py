#!/usr/bin/env python3
"""c1p_agg.py -- summarise the [c1p] receipts of the core-1 probe ([debug] core1_probe=1).

    tools/native/c1p_agg.py LOG [--lines A-B] [--mode N]

One row per (pct, phase): windows, the APT readback (rc/got), job wall median/p90 (us) and
wall/cpu0 ratio, start latency, the below-audio spin share, and the GSP/FS latency medians
(us) from core 0, plus the audio ring minimum. Medians over windows. Contract and the go/no-go
reading: docs/research/native/n60tick-progress.md section 3.

N60CORE1 columns (docs/research/native/n60core1-progress.md): what the share costs the GAME --
`loop50/90` = host-loop period median/p90 (us; the whole iteration, so a starved GSP shows here),
`rep%` = share of loop periods > 20 ms (a repeated vblank), `under`/`ndrop` = audio drain underrun
edges and libctru ndsp dropped DSP frames summed over the windows. `[core1]` watch lines (ndsp_core1
/ syscore_pct set, probe off) are summarised after the table.
"""
import argparse
import re
import statistics as st
from collections import defaultdict


def kv_triplet(line, key):
    m = re.search(r'\b%s=(\d+)/(\d+)/(\d+)' % key, line)
    return tuple(int(x) for x in m.groups()) if m else None


def num(line, key):
    m = re.search(r'\b%s=\+?(-?[\d.]+)' % key, line)
    return float(m.group(1)) if m else None


def med(vals):
    vals = [v for v in vals if v is not None]
    return st.median(vals) if vals else float('nan')


def parse(path, lines):
    rows = defaultdict(list)
    meta = []
    watch = []
    for no, line in enumerate(open(path, errors='replace'), 1):
        if lines and not (lines[0] <= no <= lines[1]):
            continue
        if line.startswith('[core1] frame='):
            watch.append((no, line.rstrip()))
            continue
        if line.startswith('[core1]'):
            meta.append((no, line.rstrip()))
            continue
        if not line.startswith('[c1p]'):
            continue
        if not line.startswith('[c1p] w='):
            meta.append((no, line.rstrip()))
            continue
        pct = int(num(line, 'pct'))
        ph = re.search(r'\bph=(\w+)', line).group(1)
        rc = re.search(r'\brc=(0x[0-9A-Fa-f]+)', line).group(1)
        got = int(num(line, 'got'))
        jw = kv_triplet(line, 'wall')
        js = kv_triplet(line, 'start')
        g = kv_triplet(line, 'gsp4k')
        fi = kv_triplet(line, 'fsipc')
        fr = kv_triplet(line, 'fsrd')
        rows[(pct, ph)].append({
            'line': no, 'rc': rc, 'got': got, 'jn': num(line, 'n'),
            'jw50': jw[0] if jw and jw[0] else None, 'jw90': jw[1] if jw and jw[1] else None,
            'js50': js[0] if js and js[0] else None, 'ratio': num(line, 'ratio') or None,
            'share': num(line, 'share') if ph == 'spin' else None,
            'runavg': num(line, 'runavg') if ph == 'spin' else None,
            'gapavg': num(line, 'gapavg') if ph == 'spin' else None,
            'g50': g[0] if g and g[0] else None, 'g90': g[1] if g and g[1] else None,
            'fi50': fi[0] if fi and fi[0] else None, 'fr50': fr[0] if fr and fr[0] else None,
            'abuf': num(line, 'abufmin'),
            'loop': kv_triplet(line, 'loop'),
            'rep': re.search(r'\brep=(\d+)/(\d+)', line),
            'under': num(line, 'under'), 'ndrop': num(line, 'ndrop'),
        })
    return rows, meta, watch


def rep_pct(r):
    tot = sum(int(x['rep'].group(2)) for x in r if x['rep'])
    hit = sum(int(x['rep'].group(1)) for x in r if x['rep'])
    return 100.0 * hit / tot if tot else float('nan')


def total(r, key):
    return sum(x[key] for x in r if x[key] is not None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--lines', help='A-B: keep receipts by log line')
    args = ap.parse_args()
    lines = tuple(int(x) for x in args.lines.split('-')) if args.lines else None
    rows, meta, watch = parse(args.log, lines)
    for no, line in meta:
        print('L%d %s' % (no, line))
    order = {'idle': 0, 'job_lo': 1, 'job_eq': 2, 'spin': 3, 'cool': 4}
    print('pct  phase   win  rc          got  jobwall50/90  ratio  start50  share%  run/gap us  gsp50/90  fsipc50  fsrd50  abufmin  loop50/90    rep%  under ndrop  lines')
    for key in sorted(rows, key=lambda k: (k[0], order.get(k[1], 9))):
        r = rows[key]
        rcs = sorted(set(x['rc'] for x in r))
        gots = sorted(set(x['got'] for x in r))
        print('%-4d %-7s %3d  %-11s %-4s %5.0f/%-6.0f %5.2f  %7.0f  %6.1f  %4.0f/%-5.0f %4.0f/%-4.0f %7.0f  %6.0f  %7.0f  %5.0f/%-5.0f %5.1f  %5.0f %5.0f  %d-%d' % (
            key[0], key[1], len(r), ','.join(rcs), ','.join(str(g) for g in gots),
            med([x['jw50'] for x in r]), med([x['jw90'] for x in r]), med([x['ratio'] for x in r]),
            med([x['js50'] for x in r]), med([x['share'] for x in r]), med([x['runavg'] for x in r]),
            med([x['gapavg'] for x in r]), med([x['g50'] for x in r]), med([x['g90'] for x in r]),
            med([x['fi50'] for x in r]), med([x['fr50'] for x in r]),
            min([x['abuf'] for x in r if x['abuf'] is not None] or [float('nan')]),
            med([x['loop'][0] if x['loop'] else None for x in r]),
            med([x['loop'][1] if x['loop'] else None for x in r]), rep_pct(r),
            total(r, 'under'), total(r, 'ndrop'),
            r[0]['line'], r[-1]['line']))
    if watch:
        avgs = [num(l, 'avg') for _, l in watch]
        reps = [num(l, 'rep') for _, l in watch]
        unders = [num(l, 'under') for _, l in watch]
        ndrops = [num(l, 'ndrop') for _, l in watch]
        print('[core1] watch: %d windows (L%d-L%d) loop avg median %.2f ms, rep total %d, under +%d, '
              'ndrop +%d' % (len(watch), watch[0][0], watch[-1][0], med(avgs), sum(x or 0 for x in reps),
                             sum(x or 0 for x in unders), sum(x or 0 for x in ndrops)))


if __name__ == '__main__':
    main()
