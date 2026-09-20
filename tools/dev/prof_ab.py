#!/usr/bin/env python3
"""prof_ab.py A.log B.log [--min-nt N] [--min-nd N] [--from-frame F] [--to-frame F]

Compare two [prof] window logs (port/3ds/gfx/gdx3ds_gpu_prof.c, gputrace=1) run A vs run B.
Prints per-bucket means (ms/frame) over the selected windows of each log, the delta, and the
[profop] opcode totals for the track-relevant opcodes (01 VTX, 06 TRI2, DE DL, 00 NOOP).
Windows are selected by triangle count (nT) / draw count (nD) so runs need not be frame-
aligned (boot timing shifts the frame numbers between runs); pass --from/--to-frame to pin a
band instead. gputrace inflates every number: only A-vs-B deltas under the same setting mean
anything, never the absolute values."""
import argparse, re, statistics, sys

KEYS = ['br', 'dsp', 'vtx', 'tri', 'imp', 'drw', 'mtx', 'oth', 'vtxN', 'nV', 'nT', 'nI', 'nD', 'nM', 'nRun']

def parse(path):
    wins, ops = [], {}
    for line in open(path, errors='replace'):
        if line.startswith('[prof] frame='):
            d = {k: float(v) for k, v in re.findall(r'(\w+)=([-\d.]+)', line)}
            wins.append(d)
        elif line.startswith('[profop] n='):
            for op, ms, calls in re.findall(r' ([0-9A-F]{2})=([\d.]+)/(\d+)', line):
                a = ops.setdefault(op, [0.0, 0, 0])
                a[0] += float(ms); a[1] += int(calls); a[2] += 1
    return wins, ops

def select(wins, a):
    out = []
    for w in wins:
        if w.get('nT', 0) < a.min_nt or w.get('nD', 0) < a.min_nd:
            continue
        f = w.get('frame', 0)
        if a.from_frame is not None and f < a.from_frame:
            continue
        if a.to_frame is not None and f > a.to_frame:
            continue
        out.append(w)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a'); ap.add_argument('b')
    ap.add_argument('--min-nt', type=float, default=150)
    ap.add_argument('--min-nd', type=float, default=0)
    ap.add_argument('--from-frame', type=float); ap.add_argument('--to-frame', type=float)
    a = ap.parse_args()
    (wa, oa), (wb, ob) = parse(a.a), parse(a.b)
    sa, sb = select(wa, a), select(wb, a)
    print('windows: A=%d/%d B=%d/%d (nT>=%g nD>=%g)' % (len(sa), len(wa), len(sb), len(wb), a.min_nt, a.min_nd))
    if not sa or not sb:
        sys.exit('not enough windows')
    print('%-6s %9s %9s %9s' % ('bucket', 'A', 'B', 'B-A'))
    for k in KEYS:
        ma = statistics.mean(w.get(k, 0) for w in sa)
        mb = statistics.mean(w.get(k, 0) for w in sb)
        print('%-6s %9.2f %9.2f %+9.2f' % (k, ma, mb, mb - ma))
    print('[profop] ms/frame (and calls/frame) over all emitted windows:')
    for op in ['01', '06', 'DE', '00', 'F3', 'FD', 'D7']:
        xa, xb = oa.get(op), ob.get(op)
        fa = lambda x: ('%.2f/%.0f' % (x[0] / max(x[2], 1), x[1] / max(x[2], 1) / 64.0)) if x else '-'
        print('  %s: A=%s B=%s' % (op, fa(xa), fa(xb)))

if __name__ == '__main__':
    main()
