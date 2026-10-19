#!/usr/bin/env python3
"""hwprof_agg.py -- summarise the hwprof receipts ([c5pmu], [hwsite], [nrr]) of a hardware log.

    tools/native/hwprof_agg.py LOG [--race] [--lines A-B] [--penalty N] [--md]

[c5pmu] (debug.c5pmu=1): each window counts ONE event pair per [c5cat] bucket (k-events per tick
for logic/dlb/oth, per rendered frame for the render columns). The pair rotates every window, so
each pair is summarised over its own windows (medians), and the cycles come from the [c5cat] line
with the same frame= (the clock is the core clock: 1 ms = 268,112 cycles). Derived per category:
    CPI          cycles / instructions
    imiss/ki     I-cache misses per 1000 instructions (and drmiss/ki, dwmiss/ki)
    if%  dh%     cycles stalled on instruction fetch / on a data hazard, share of the bucket
    lsu%         cycles stalled with the load/store unit full (D-side misses), share
    ext/ki       external memory requests (line fills + non-cacheable) per 1000 instructions
With --penalty N, `miss%` estimates (imiss + drmiss) * N cycles as a share of the bucket.

[hwsite] (debug.hwsite=1): per site, the median calls and us per rendered frame, total and per
bucket, and each site's share of the bucket's own [c5cat] ms.

[nrr] ([native] nrr, n60): per NRR category, the median markers / draws / state applies / vertices
and us per frame of its windows, and the share of the [c5cat] bucket the category draws in (an NRR
marker sits where the section it replaces stood, so its time is already inside that bucket: hud ->
hud, trk -> trk, mach -> mach, sky -> sky, dress -> dress, shd -> shd, fx -> fx). us is filled only
with the [c5cat] clock armed (gputrace / mainprof); c5pmu then attributes the NRR's cycles through
the same buckets.

--race keeps windows whose [c5cat] line drew the race scene (sky + trk >= 0.5 ms, misc < 5).
--lines A-B keeps windows by log line (one play segment).
"""
import argparse
import re
import statistics as st
from collections import defaultdict

KCYC_PER_MS = 268111.856 / 1000.0  # thousands of cycles per ms
TICK = ['logic', 'dlb', 'oth']
REND = ['fbeg', 'misc', 'sky', 'trk', 'dress', 'mach', 'mesh', 'shd', 'fx', 'hud', 'mir', 'swp', 'fend']
OTH_TICK = ['aud', 'ld', 'vi', 'inp', 'apt', 'tail', 'task']


def kv(line):
    return {k: float(v) for k, v in re.findall(r'(\w+)=(-?[\d.]+)', line)}


def parse(path, lines=None):
    cat = {}
    pmu = []
    hws = []
    nrr = []
    for no, line in enumerate(open(path, errors='replace'), 1):
        if lines and not (lines[0] <= no <= lines[1]):
            continue
        if line.startswith('[c5cat] frame=') and 'wall=' in line:
            d = kv(line)
            d['_line'] = no
            cat[int(d['frame'])] = d
        elif line.startswith('[c5pmu] frame='):
            head, _, rest = line.partition('|')
            m = re.search(r'frame=(\d+) n=(\d+) r=(\d+) pair=(\d+) ev=(\w+),(\w+)', head)
            if not m:
                continue
            row = {'frame': int(m.group(1)), 'pair': int(m.group(4)), 'ev': (m.group(5), m.group(6)), 'v': {}}
            for name, a, b in re.findall(r'(\w+)=(-?[\d.]+),(-?[\d.]+)', rest):
                row['v'][name] = (float(a), float(b))
            pmu.append(row)
        elif line.startswith('[nrr] frame='):
            m = re.match(r'\[nrr\] frame=(\d+) tasks=(\d+)', line)
            if not m:
                continue
            row = {'frame': int(m.group(1)), 'line': no, 'cats': {}}
            for name, body in re.findall(r'\| (\w+) (mk=[^|]*)', line):
                row['cats'][name] = kv(body)
            nrr.append(row)
        elif line.startswith('[hwsite] frame='):
            head, _, rest = line.partition('|')
            m = re.search(r'frame=(\d+) r=(\d+) site=(\w+) n/f=([\d.]+) us/f=([\d.]+) ovh/f=([\d.]+)', head)
            if not m:
                continue
            row = {'frame': int(m.group(1)), 'site': m.group(3), 'n': float(m.group(4)), 'us': float(m.group(5)),
                   'ovh': float(m.group(6)), 'cats': {}}
            for name, n, us in re.findall(r'(\w+)=([\d.]+)/([\d.]+)', rest):
                row['cats'][name] = (float(n), float(us))
            hws.append(row)
    return cat, pmu, hws, nrr


def race(c):
    return c is not None and c.get('sky', 0) + c.get('trk', 0) >= 0.5 and c.get('misc', 0) < 5


def ms_of(c, name):
    if name == 'oth':
        return sum(c.get(k, 0.0) for k in OTH_TICK)
    return c.get(name, 0.0)


def med(vals):
    return st.median(vals) if vals else float('nan')


def pmu_report(cat, pmu, args):
    # per event name, per category: list of (events_k, kcycles) from windows that counted it
    ev = defaultdict(lambda: defaultdict(list))
    windows = defaultdict(int)
    for row in pmu:
        c = cat.get(row['frame'])
        if c is None or (args.race and not race(c)):
            continue
        windows[row['pair']] += 1
        for name in TICK + REND:
            if name not in row['v']:
                continue
            kc = ms_of(c, name) * KCYC_PER_MS
            for i in (0, 1):
                ev[row['ev'][i]][name].append((row['v'][name][i], kc))
    if not windows:
        print('[c5pmu] no windows matched')
        return
    print(f"[c5pmu] windows per pair: {dict(sorted(windows.items()))}  (race={args.race})")

    def ratio(e, name, denom_ev=None):
        """median over windows of e/(denominator): denominator = cycles, or another event (k)."""
        rows = ev.get(e, {}).get(name, [])
        if not rows:
            return None
        if denom_ev is None:
            vals = [v / kc for v, kc in rows if kc > 0]
        else:
            # the two events of a pair share windows; across pairs, use medians of each
            other = ev.get(denom_ev, {}).get(name, [])
            if not other:
                return None
            d = med([v for v, _ in other])
            return med([v for v, _ in rows]) / d if d > 0 else None
        return med(vals) if vals else None

    cols = ['ms', 'kcyc', 'kinst', 'CPI', 'imiss/ki', 'drmiss/ki', 'dwmiss/ki', 'if%', 'dh%', 'lsu%', 'ext/ki',
            'tlb/ki', 'brmiss/ki']
    if args.penalty:
        cols.append('miss%')
    sep = ' | ' if args.md else '  '
    print(sep.join(['cat'] + cols))
    if args.md:
        print('|'.join(['---'] * (len(cols) + 1)))
    for name in TICK + REND:
        kcs = [kc for e in ev.values() for v, kc in e.get(name, [])]
        if not kcs:
            continue
        kc = med(kcs)
        ms = kc / KCYC_PER_MS
        inst = med([v for v, _ in ev.get('inst', {}).get(name, [])]) if ev.get('inst', {}).get(name) else None

        def per_ki(e):
            x = ev.get(e, {}).get(name)
            if not x or not inst:
                return None
            return 1000.0 * med([v for v, _ in x]) / inst

        def share(e):
            r = ratio(e, name)
            return None if r is None else 100.0 * r

        cpi = (kc / inst) if inst else None
        out = [ms, kc, inst, cpi, per_ki('imiss'), per_ki('drmiss'), per_ki('dwmiss'), share('stall_if'),
               share('stall_dh'), share('stall_lsu'), per_ki('ext'), per_ki('tlbmiss'), per_ki('brmiss')]
        if args.penalty:
            im = ev.get('imiss', {}).get(name)
            dm = ev.get('drmiss', {}).get(name)
            if im and dm and kc > 0:
                out.append(100.0 * (med([v for v, _ in im]) + med([v for v, _ in dm])) * args.penalty / kc)
            else:
                out.append(None)
        cells = [name] + ['-' if x is None else (f'{x:.2f}' if abs(x) < 100 else f'{x:.0f}') for x in out]
        print(sep.join(cells))


def hws_report(cat, hws, args):
    per = defaultdict(lambda: {'n': [], 'us': [], 'ovh': [], 'cats': defaultdict(list), 'share': defaultdict(list)})
    for row in hws:
        c = cat.get(row['frame'])
        if c is None or (args.race and not race(c)):
            continue
        s = per[row['site']]
        s['n'].append(row['n'])
        s['us'].append(row['us'])
        s['ovh'].append(row['ovh'])
        for name, (n, us) in row['cats'].items():
            s['cats'][name].append(us)
            ms = c.get('fend' if name == 'end' else name)
            if ms:
                s['share'][name].append(100.0 * us / 1000.0 / ms)
    if not per:
        print('[hwsite] no windows matched')
        return
    print(f"[hwsite] per rendered frame, medians (race={args.race}); share = site us / the bucket's [c5cat] ms")
    for site, s in per.items():
        top = sorted(s['cats'].items(), key=lambda kvp: -med(kvp[1]))[:8]
        cells = ' '.join(f"{name}={med(v):.0f}us({med(s['share'][name]):.0f}%)" if s['share'].get(name)
                         else f"{name}={med(v):.0f}us" for name, v in top)
        print(f"  {site:9s} n/f={med(s['n']):6.1f} us/f={med(s['us']):7.0f} ovh/f={med(s['ovh']):5.0f} | {cells}")


NRR_BUCKET = {'hud': 'hud', 'trk': 'trk', 'mach': 'mach', 'sky': 'sky', 'dress': 'dress', 'shd': 'shd',
              'fx': 'fx', 'selftest': 'fx'}


def containing_window(cat, line):
    """The [c5cat] window a row was logged inside: the first [c5cat] line after it. [nrr] frame= counts
    render tasks, not host iterations, so it rarely equals a [c5cat] frame= (n60log: with
    [debug] receipt_every > 64 the [c5cat] window is longer than the [nrr] one and never does)."""
    after = [c for c in cat.values() if c['_line'] > line]
    return min(after, key=lambda c: c['_line']) if after else None


def nrr_report(cat, nrr, args):
    per = defaultdict(lambda: defaultdict(list))
    for row in nrr:
        c = cat.get(row['frame']) or containing_window(cat, row['line'])
        if args.race and not race(c):
            continue
        for name, v in row['cats'].items():
            for k in ('mk', 'dr', 'st', 'vx', 'us', 'fb', 'lag'):
                per[name][k].append(v.get(k, 0.0))
            ms = c.get(NRR_BUCKET.get(name, ''), 0.0) if c else 0.0
            if ms:
                per[name]['share'].append(100.0 * v.get('us', 0.0) / 1000.0 / ms)
    if not per:
        print('[nrr] no windows matched')
        return
    print(f"[nrr] per rendered frame, medians (race={args.race}); share = NRR us / its [c5cat] bucket's ms")
    for name, d in per.items():
        share = f" share={med(d['share']):.0f}%({NRR_BUCKET.get(name)})" if d['share'] else ''
        print(f"  {name:8s} markers={med(d['mk']):5.1f} draws={med(d['dr']):6.1f} states={med(d['st']):5.1f} "
              f"verts={med(d['vx']):7.0f} us={med(d['us']):7.1f} fb_max={max(d['fb']):.0f} "
              f"lag_max={max(d['lag']):.0f}{share}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--race', action='store_true')
    ap.add_argument('--lines', default=None, help='A-B: keep log lines A..B')
    ap.add_argument('--penalty', type=float, default=0.0, help='cycles per I/D miss for the miss%% estimate')
    ap.add_argument('--md', action='store_true', help='markdown table')
    args = ap.parse_args()
    lines = None
    if args.lines:
        a, b = args.lines.split('-')
        lines = (int(a), int(b))
    cat, pmu, hws, nrr = parse(args.log, lines)
    if pmu:
        pmu_report(cat, pmu, args)
    if hws:
        hws_report(cat, hws, args)
    if nrr:
        nrr_report(cat, nrr, args)
    if not pmu and not hws and not nrr:
        print('no [c5pmu] / [hwsite] / [nrr] receipts in this log')


if __name__ == '__main__':
    main()
