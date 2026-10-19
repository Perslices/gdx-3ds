#!/usr/bin/env python3
"""stereo_disparity.py RUNDIR SHOT...: per horizontal band, the integer x shift s (R(x) ~ L(x - s))
minimising mean |luma| difference; s > 0 = the right eye sees it further right = behind the screen.
Total disparity between the eyes, in top-screen px (each eye moves s/2)."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dev"))
import framediff
BANDS = [('top 16-56', 16, 56), ('mid 96-136', 96, 136), ('low 176-216', 176, 216)]
def load(path):
    w, h, raw = framediff.read_bmp(path)
    return w, h, raw
def best(L, R, y0, y1, w):
    res = []
    for s in range(-40, 41):
        tot = 0.0; n = 0
        for y in range(y0, y1, 2):
            rl, rr = L[y], R[y]
            for x in range(60, w - 60, 2):
                tot += abs(rr[x] - rl[x - s]); n += 1
        res.append((tot / n, s))
    res.sort()
    return res[0][1], res[0][0]
run = sys.argv[1]
for shot in sys.argv[2:]:
    out = [shot]
    w, h, a = load(os.path.join(run, 'shots', shot + '_L.bmp'))
    _, _, b = load(os.path.join(run, 'shots', shot + '_R.bmp'))
    def tol(rows):
        return [[0.299 * r[x * 3] + 0.587 * r[x * 3 + 1] + 0.114 * r[x * 3 + 2] for x in range(w)] for r in rows]
    L, R = tol(a), tol(b)
    for name, y0, y1 in BANDS:
        s, err = best(L, R, y0, y1, w)
        out.append(f'{name}: {s:+d}px (err {err:.1f})')
    print(' | '.join(out))
