#!/usr/bin/env python3
"""verifymatrix_report.py — pull the verify-mode coverage receipts out of one guest log.

    tools/dev/verifymatrix_report.py <guest log.txt>

Called by tools/dev/verifymatrix.sh. Separate from it because each receipt family has its
own accumulation rule and getting one wrong is exactly how a "silent pass with zero
coverage" gets reported as a pass:

  [nverify] tasks=...      sVerifyChecked / sVerifyMismatches in gdx_rt_glue.cpp are never
                           reset -> RUN-CUMULATIVE, take the LAST line.
  [ninterp] tasks=...      gdxn::Counters are reset at every receipt -> SUM the windows.
  [trk] replay(prev)       the PREVIOUS pool-frame's replay counters, sampled every 64
                           gdxtrk_begin calls -> a SAMPLED SUM, not a run total.
  [native] verify/machines sWin is zeroed after every receipt -> SUM the windows.
  [fx] agg2                sTotRp is run-wide -> RUN-CUMULATIVE, take the LAST line.

Stdlib only.
"""
import re
import sys


def load(path):
    with open(path, errors="replace") as f:
        return f.read().splitlines()


def vals(log, pat, keys):
    """(lines matching pat, summed values, last line's values)."""
    tot = {k: 0 for k in keys}
    last = {}
    n = 0
    for ln in log:
        if pat not in ln:
            continue
        n += 1
        cur = {}
        for k in keys:
            m = re.search(r"\b%s=(\d+)" % re.escape(k), ln)
            if m:
                cur[k] = int(m.group(1))
                tot[k] += int(m.group(1))
        last = cur
    return n, tot, last


def fmt(d, keys):
    return " ".join("%s=%d" % (k, d.get(k, 0)) for k in keys)


def main(path):
    log = load(path)
    out = []

    k = ["tasks", "batch_mismatch_tasks", "hist_mismatch_tasks"]
    n, _, last = vals(log, "[nverify] tasks=", k)
    if n:
        out.append("nverify  receipts=%d (run-cumulative) %s" % (n, fmt(last, k)))

    k = ["tasks", "vtx", "tri", "unknown", "dropped", "runaway", "rootrej"]
    n, tot, _ = vals(log, "[ninterp] tasks=", k)
    if n:
        out.append("ninterp  windows=%d (sum) %s" % (n, fmt(tot, k)))

    k = ["markers", "runs", "tris", "vok", "vbad", "fb"]
    n, tot, _ = vals(log, "[trk] replay", k)
    if n:
        out.append("trk      samples=%d (sampled sum) %s" % (n, fmt(tot, k)))

    k = ["draws", "tris", "triMismatch", "countMismatch", "matMismatch", "auxDiff"]
    n, tot, _ = vals(log, "[native] verify draws=", k)
    if n:
        out.append("machines windows=%d (sum) %s" % (n, fmt(tot, k)))

    k = ["draws", "walk", "runs", "tris", "fails", "fb"]
    n, tot, _ = vals(log, "[native] machines=", k)
    if n:
        out.append("machdraw windows=%d (sum) %s" % (n, fmt(tot, k)))

    for ln in log:
        if "mask=" in ln and "[native] machines=" in ln:
            out.append("machcfg  " + ln.split("[native] ", 1)[1].strip())
            break

    k = ["markers", "tris", "fast", "exact", "vok", "vbad", "fb"]
    n, _, last = vals(log, "[fx] agg2", k)
    if n:
        out.append("fx       receipts=%d (run-cumulative) %s" % (n, fmt(last, k)))

    mismatch_tags = ("[trk] verify MISMATCH", "[native] verify MISMATCH", "[fx] verify MISMATCH",
                     "[nverify] task=", "[nverify] startstate", "[nverify] endstate")
    error_tags = ("[fatal]", "[gfxfail]", "[gdl-bad]", "[gdl-miss]", "[datafail]", "bad_alloc",
                  "bake FAIL", "marker with bad record")
    mis, err = {}, {}
    for ln in log:
        for t in mismatch_tags:
            if t in ln:
                mis[t] = mis.get(t, 0) + 1
        for t in error_tags:
            if t in ln:
                err[t] = err.get(t, 0) + 1
    out.append("mismatch lines: " + (", ".join("%s x%d" % kv for kv in mis.items()) if mis else "none"))
    out.append("errors: " + (", ".join("%s x%d" % kv for kv in err.items()) if err else "none"))
    out.append("fh receipts: %d" % sum(1 for ln in log if "[fh] frame=" in ln))

    for ln in out:
        print("    " + ln)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
