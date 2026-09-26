#!/usr/bin/env python3
"""lodburst.py — line the `[cov]` census and the per-window renderer receipts up against the
`[dynlod]` wall, and say what a burst frame has that a quiet one does not (NEXT.md B38).

Why this exists. `vtxmath` closed the *controller* half of B38: the threshold pair is
hysteretic, no transition in the hardware log acted on ambiguous evidence, and the ~200-300
frame period is the controller's own LOWER floor (194) plus the raise latency. What it could
not answer is the half that matters — **what makes the load move in the first place**. It
named the cheap next measurement: correlate `[cov]` against the `[dynlod]` wall. This is that
tool, and it also reads the two receipt families that carry the same window as `[dynlod]` and
are present on HARDWARE, where `[cov]` is not usually on:

    [ninterp] tasks=64 ...            the interpreter's 64-task window census
    [dynlod]  auto=... ms= max= n=64  the render task's own wall, same window
    [native]  machines=1 lists=...    the native mesh walker, same window
    [native]  meshlife ...            bake/retire bookkeeping, same window

THE WINDOW ALIGNMENT IS THE WHOLE TOOL, and it has one trap that inverts a conclusion.
`gdx3ds_dynlod_receipt()` is called at the END of a 64-frame window and resets its
accumulators there, while `DynLodLogTransition()` prints the instant a tier changes — inside
the window. So a `raise`/`lower` line that appears BELOW window N's receipt was decided by
frames belonging to window N+1, not N. Attributing it to the line above it (the obvious
reading) moves every transition one window early, which is exactly the distance between "the
raise fired in the quiet menu window" and "the raise fired in the course-load window that
follows it". Both readings were produced during B38; only the second is right, and
`lodburst.py tx` prints the receipts on both sides so the reader can check.

`[cov]` is per FRAME and the receipts are per 64-frame WINDOW, so `cov` aggregates the census
to window granularity (union of the live sets, and the count of frames each key was live).
Frame k is assigned to window k // 64: dynlod's `n=` is 64 on every full window and the two
counters advance together, which the tool asserts (`cov` prints the frame-span check).

    lodburst.py windows <log> [--race]     the joined per-window table
    lodburst.py corr    <log> [--race]     every counter's correlation with the wall
    lodburst.py tx      <log>              every transition with the window that decided it
    lodburst.py events  <log>              the wall in the windows carrying each event
    lodburst.py period  <log>              is the load periodic, or a drift with no clock?
    lodburst.py shots   <log>              do SHOT ticks inflate the wall? (see below)
    lodburst.py cov     <log> [--top N]    which `[cov]` keys separate burst windows

`--wall dynlod|prof` picks the frame-time signal; the default takes `[dynlod] ms=` when the
controller is live and `[prof]`'s bucket sum when it is not. **A replay pins
`rival_detail_auto=0` unless `[debug] replay_dynlod=1`** (`main_3ds.cpp`, so a host-speed tier
cannot make `[fh]` parity meaningless), so every deterministic hardware run the harness
produces reads `ms=0.0 max=0.0 n=0` on every dynlod receipt and is blind to B38. That is why
the only hardware evidence for B38 is a hand-played session.

**`shots` is not optional bookkeeping.** A previous investigation on this fork attributed an
fps dip to a load stall and it was the replay's own screenshot writes; the corpus schedules
now carry 329 SHOTs and each `[autotest] SHOT captured` line states its own cost in `ms=`.
Any claim about the game made from an emulator log has to clear this first, which is why
`shots` reports the with-SHOT and without-SHOT distributions of `max=` side by side rather
than a single number.

Stdlib only (PROTOCOL.md: host tools are stdlib-only).
"""

import argparse
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import covsched  # noqa: E402  (same directory, stdlib-only, for the [cov] delta decoder)

KV = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(-?[0-9]+(?:\.[0-9]+)?)")
NI_TEX = re.compile(r"tex hit=(\d+) miss=(\d+) evict=(\d+) clr=(\d+) inval=(\d+)/(\d+)")
MC_US = re.compile(r"us=(\d+)/f \(tnl=(\d+) bind=(\d+) emit=(\d+) draw=(\d+)\)")
# `ms=` is recent: older logs (before the capture learned to time itself) stop at `frame=`.
# Making it optional is what keeps `shots` honest on an old log — the alternative is a
# silent shots=0 next to a tag list that says SHOT, which this tool produced once.
SHOT = re.compile(r"\[autotest\] SHOT captured -> (\S+) bytes=(\d+) frame=(\d+)(?: ms=(\d+))?")
TX = re.compile(r"^\[dynlod\] (raise|lower) (\d)->(\d) (.*)$")

# Events that are worth naming beside a transition. Each entry is (prefix, short tag).
EVENTS = [
    ("[transition] capture", "xcapture"),     # LCD-scanout readback for the screen wipe
    ("[segment] loaded venue", "venue"),      # course/venue segment swapped in
    ("[segment] building texture", "segtex"),
    ("[seg-src] blob loaded", "blob"),
    ("[native] bake ", "bake"),
    ("[autotest] SHOT captured", "SHOT"),
    ("[fatal]", "FATAL"),
]


class Window(object):
    """One 64-frame window: the receipts that close it, plus what happened inside it."""

    __slots__ = ("i", "ni", "dl", "mc", "ml", "pf", "events", "tx", "shots", "shot_ms")

    def __init__(self, i):
        self.i = i
        self.ni = {}
        self.dl = {}
        self.pf = {}
        self.mc = {}
        self.ml = {}
        self.events = []
        self.tx = []
        self.shots = 0
        self.shot_ms = 0

    def get(self, dotted, default=0.0):
        pre, _, key = dotted.partition(".")
        return getattr(self, pre, {}).get(key, default)

    @property
    def racing(self):
        return self.mc.get("draws", 0.0) > 0.0

    def tags(self):
        out = []
        for pre, tag in EVENTS:
            n = sum(1 for e in self.events if e.startswith(pre))
            if n:
                out.append(tag if n == 1 else "%s*%d" % (tag, n))
        return out


def _close(windows, w):
    """Finish a window: count the SHOTs that happened inside it and file it."""
    for e in w.events:
        m = SHOT.search(e)
        if m:
            w.shots += 1
            if m.group(4) is not None:
                w.shot_ms += int(m.group(4))
    w.i = len(windows)
    windows.append(w)


def parse(path):
    """Walk the log once and return the list of windows, in order.

    The receipt order inside one window is [ninterp] ... [prof] ... [dynlod auto=] ...
    [native machines] ... [native meshlife]. `[dynlod] auto=` closes the window when it is
    there, because it is emitted for every window whether or not the renderer drew anything;
    a log with dynlod pinned off (every replay, unless [debug] replay_dynlod=1) has no such
    fence, so the next `[ninterp]` closes the previous window instead.
    """
    windows = []
    cur = None
    pending_events = []
    pending_tx = []
    with open(path, errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if line.startswith("[ninterp]"):
                if cur is not None:
                    _close(windows, cur)      # a log with no [dynlod] fence
                cur = Window(len(windows))
                cur.ni = {k: float(v) for k, v in KV.findall(line)}
                m = NI_TEX.search(line)
                if m:
                    cur.ni.update(zip(("texhit", "texmiss", "texevict", "texclr",
                                       "texinvA", "texinvB"), map(float, m.groups())))
                cur.events = pending_events
                cur.tx = pending_tx
                pending_events, pending_tx = [], []
                continue
            if line.startswith("[prof] frame="):
                # Same 64-frame window, and the only wall left when dynlod is pinned off:
                # then every `[dynlod]` receipt reads `ms=0.0 max=0.0 n=0`. [prof] is emitted
                # AFTER [dynlod] closes the window, so it lands on the window just filed; a
                # `path=lus` run emits no [ninterp] at all and [prof] opens the window itself.
                pf = {k: float(v) for k, v in KV.findall(line)}
                pf["total"] = sum(pf.get(k, 0.0) for k in
                                  ("br", "dsp", "vtx", "tri", "imp", "drw", "mtx", "oth"))
                if cur is not None:
                    cur.pf = pf
                elif windows and not windows[-1].pf:
                    windows[-1].pf = pf
                else:
                    cur = Window(len(windows))
                    cur.pf = pf
                    cur.events = pending_events
                    cur.tx = pending_tx
                    pending_events, pending_tx = [], []
                continue
            if line.startswith("[dynlod] auto=") and cur is not None:
                cur.dl = {k: float(v) for k, v in KV.findall(line)}
                _close(windows, cur)
                cur = None
                continue
            m = TX.match(line)
            if m:
                # Decided INSIDE the window whose receipts have not been printed yet — see
                # the module docstring. Held until that window opens.
                pending_tx.append(line)
                continue
            if line.startswith("[native] machines=1 lists=") and (cur or windows):
                d = {k: float(v) for k, v in KV.findall(line)}
                m = MC_US.search(line)
                if m:
                    d.update(zip(("us", "tnl", "bind", "emit", "draw"), map(float, m.groups())))
                (cur or windows[-1]).mc = d
                continue
            if line.startswith("[native] meshlife") and (cur or windows):
                (cur or windows[-1]).ml = {k: float(v) for k, v in KV.findall(line)}
                continue
            if line.startswith("[dynlod]"):
                continue  # the init receipt
            pending_events.append(line)
    if cur is not None:
        _close(windows, cur)
    # A transition still pending at EOF was decided in a window whose receipts never printed.
    if pending_tx and windows:
        windows[-1].tx.extend("(after last receipt) " + t for t in pending_tx)
    return windows


def wall(w, which):
    """The window's frame-time signal: dynlod's own mean, or [prof]'s bucket sum."""
    if which == "prof":
        return w.pf.get("total", 0.0)
    return w.dl.get("ms", 0.0)


def wall_max(w, which):
    if which == "prof":
        return w.pf.get("total", 0.0)   # [prof] has no per-frame worst case
    return w.dl.get("max", 0.0)


def pick_wall(windows, asked):
    """`dynlod` unless it is flat zero (dynlod pinned off), in which case `[prof]`."""
    if asked:
        return asked
    # A replay pins rival_detail_auto=0, and then EVERY dynlod receipt reads ms=0.0 — except
    # the handful emitted before the pin lands. "any window is non-zero" would therefore pick
    # a wall that is flat zero over the whole race; require a majority.
    live = sum(1 for w in windows if w.dl.get("ms", 0.0) > 0.0)
    if live * 2 > len(windows):
        return "dynlod"
    if any(w.pf for w in windows):
        return "prof"
    return "dynlod"


def pearson(xs, ys):
    n = len(xs)
    if n < 3:
        return 0.0
    mx, my = sum(xs) / n, sum(ys) / n
    sx = sum((x - mx) ** 2 for x in xs) ** 0.5
    sy = sum((y - my) ** 2 for y in ys) ** 0.5
    if sx == 0.0 or sy == 0.0:
        return 0.0
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / (sx * sy)


def counters(windows):
    keys = set()
    for w in windows:
        for pre in ("ni", "mc", "ml", "pf"):
            for k in getattr(w, pre):
                keys.add(pre + "." + k)
    return sorted(keys)


def need(windows, path):
    if not windows:
        sys.stderr.write("lodburst: no [ninterp]/[dynlod] window pairs in %s\n" % path)
        sys.stderr.write("          ([debug] verbose receipts off, or dynlod pinned: a replay\n"
                         "           pins rival_detail_auto=0 unless [debug] replay_dynlod=1)\n")
        raise SystemExit(6)


def cmd_windows(args):
    ws = parse(args.log)
    need(ws, args.log)
    print("%-5s %-5s %-6s %-6s %-8s %-6s %-7s %-6s %-6s %-6s %s" % (
        "win", "tier", "ms", "max", "cmds", "lists", "ldblk", "tmiss", "mc.us", "draws", "events"))
    for w in ws:
        if args.race and not w.racing:
            continue
        print("%-5d %-5.0f %-6.1f %-6.1f %-8.0f %-6.0f %-7.0f %-6.0f %-6.0f %-6.0f %s" % (
            w.i, w.dl.get("tier", -1), w.dl.get("ms", 0), w.dl.get("max", 0),
            w.ni.get("cmds", 0), w.ni.get("lists", 0), w.ni.get("ldblk", 0),
            w.ni.get("texmiss", 0), w.mc.get("us", 0), w.mc.get("draws", 0),
            " ".join(w.tags())))
        for t in w.tx:
            print("      >>> %s" % t)


def cmd_corr(args):
    ws = parse(args.log)
    need(ws, args.log)
    sel = [w for w in ws if w.racing] if args.race else ws
    if len(sel) < 5:
        sys.stderr.write("lodburst: only %d window(s) selected\n" % len(sel))
        raise SystemExit(6)
    which = pick_wall(ws, args.wall)
    ms = [wall(w, which) for w in sel]
    mx = [wall_max(w, which) for w in sel]
    rows = []
    for k in counters(sel):
        if which == "prof" and k.startswith("pf."):
            continue            # [prof]'s own buckets sum to the wall; that is not a finding
        xs = [w.get(k) for w in sel]
        if len(set(xs)) < 5:
            continue
        rows.append((pearson(xs, ms), pearson(xs, mx), k, min(xs), max(xs)))
    rows.sort(key=lambda r: -abs(r[0]))
    print("%d window(s)%s; wall=%s  %.1f..%.1f ms (median %.1f)" % (
        len(sel), " (race only)" if args.race else "", which,
        min(ms), max(ms), statistics.median(ms)))
    print("%-26s %8s %8s  %s" % ("counter", "r(wall)", "r(max)", "range"))
    for r_ms, r_max, k, lo, hi in rows[:args.top]:
        print("%-26s %+8.3f %+8.3f  %.0f..%.0f" % (k, r_ms, r_max, lo, hi))


def classify(w, prev):
    """Name the event that plausibly produced this window's stall, or 'in-race load'."""
    tags = set(w.tags()) | set(prev.tags() if prev else [])
    for tag in ("venue", "segtex", "blob"):
        if tag in tags or any(t.startswith(tag) for t in tags):
            return "course load"
    if any(t.startswith("xcapture") for t in tags):
        return "screen-transition capture"
    if any(t.startswith("SHOT") for t in tags):
        return "SHOT capture"
    return "in-race load"


def cmd_tx(args):
    ws = parse(args.log)
    need(ws, args.log)
    n = 0
    for w in ws:
        for t in w.tx:
            n += 1
            prev = ws[w.i - 1] if w.i else None
            print("%-6s decided in w%-4d  %s" % (
                "RAISE" if " raise " in t else "LOWER", w.i, classify(w, prev)))
            print("        %s" % t)
            for label, v in (("prev", prev), ("this", w)):
                if v is None:
                    continue
                print("        %s w%-4d ms=%5.1f max=%5.1f cmds=%8.0f ldblk=%6.0f tmiss=%5.0f "
                      "mc.us=%6.0f draws=%5.0f  %s" % (
                          label, v.i, v.dl.get("ms", 0), v.dl.get("max", 0), v.ni.get("cmds", 0),
                          v.ni.get("ldblk", 0), v.ni.get("texmiss", 0), v.mc.get("us", 0),
                          v.mc.get("draws", 0), " ".join(v.tags())))
            print()
    print("%d transition(s) in %d window(s)" % (n, len(ws)))


def describe(vals):
    if not vals:
        return "n=0"
    return "n=%-4d med %6.1f  mean %6.1f  p90 %6.1f  max %6.1f" % (
        len(vals), statistics.median(vals), sum(vals) / len(vals),
        sorted(vals)[int(0.9 * (len(vals) - 1))], max(vals))


def cmd_shots(args):
    ws = parse(args.log)
    need(ws, args.log)
    total = sum(w.shots for w in ws)
    if total == 0:
        print("no `[autotest] SHOT captured` lines in this log — this run takes no screenshots,")
        print("so nothing here can be the replay's own capture cost. (%d window(s).)" % len(ws))
        return
    a = [w for w in ws if w.shots]
    b = [w for w in ws if not w.shots]
    timed = [w for w in a if w.shot_ms]
    cost = [w.shot_ms / w.shots for w in timed]
    print("%d SHOT(s) over %d window(s); %d window(s) carry at least one." % (total, len(ws), len(a)))
    if cost:
        print("self-reported capture cost, `[autotest] SHOT captured ... ms=`: %s" % describe(cost))
    else:
        print("no `ms=` on any capture receipt (log predates the timed capture) — cost unknown")
    print()
    print("%-22s %s" % ("dynlod max= (worst single frame in the window)", ""))
    print("  with a SHOT   %s" % describe([w.dl.get("max", 0.0) for w in a]))
    print("  without       %s" % describe([w.dl.get("max", 0.0) for w in b]))
    print("%-22s %s" % ("dynlod ms= (64-frame mean)", ""))
    print("  with a SHOT   %s" % describe([w.dl.get("ms", 0.0) for w in a]))
    print("  without       %s" % describe([w.dl.get("ms", 0.0) for w in b]))
    print()
    tx_shot = sum(1 for w in ws if w.tx and w.shots)
    tx_all = sum(1 for w in ws if w.tx)
    print("tier transitions decided in a window carrying a SHOT: %d of %d" % (tx_shot, tx_all))
    print("top windows by max=:")
    for w in sorted(ws, key=lambda w: -w.dl.get("max", 0.0))[:args.top]:
        print("  w%-4d max=%6.1f ms=%5.1f shots=%d shot_ms=%4d cmds=%8.0f %s" % (
            w.i, w.dl.get("max", 0), w.dl.get("ms", 0), w.shots, w.shot_ms,
            w.ni.get("cmds", 0), " ".join(w.tags())))


def cmd_events(args):
    """For every event tag: the wall in the windows that carry it, against the ones that do not.

    This is `shots` generalised, and it is the test that names the burst. A tag whose windows
    are indistinguishable from the rest is not a cause however plausible it sounds; a tag whose
    windows carry a `max=` several times the population's is.
    """
    ws = parse(args.log)
    need(ws, args.log)
    print("%d window(s); wall median ms=%.1f max=%.1f" % (
        len(ws), statistics.median([w.dl.get("ms", 0.0) for w in ws]),
        statistics.median([w.dl.get("max", 0.0) for w in ws])))
    print()
    for _, tag in EVENTS:
        a = [w for w in ws if any(t == tag or t.startswith(tag + "*") for t in w.tags())]
        if not a:
            continue
        b = [w for w in ws if w not in a]
        tx = sum(1 for w in a if w.tx)
        print("%-10s carried by %3d window(s); %d of them decided a tier transition" % (
            tag, len(a), tx))
        print("             ms  with %s" % describe([w.dl.get("ms", 0.0) for w in a]))
        print("             ms  w/o  %s" % describe([w.dl.get("ms", 0.0) for w in b]))
        print("             max with %s" % describe([w.dl.get("max", 0.0) for w in a]))
        print("             max w/o  %s" % describe([w.dl.get("max", 0.0) for w in b]))
        print()


def autocorr(xs, lag):
    if len(xs) - lag < 4:
        return None
    a, b = xs[:len(xs) - lag], xs[lag:]
    return pearson(a, b)


def cmd_period(args):
    """Is the load periodic, or is it a drift the controller's own floor samples?

    B38 reports a "~200-300 frame period". `vtxmath` showed the period is the controller's
    LOWER floor (194) plus the raise latency. That leaves the load itself: if the LOAD had a
    period of its own near that scale, the per-window wall would autocorrelate with a peak at
    lag 3-5 (192-320 frames at 64 frames/window). A monotone decay is a drift with no clock.
    Run per contiguous race segment, because a segment boundary is a screen change and would
    manufacture a period out of the session's structure.
    """
    ws = parse(args.log)
    need(ws, args.log)
    segs, cur = [], []
    for w in ws:
        if w.racing:
            cur.append(w)
        else:
            if len(cur) >= args.min:
                segs.append(cur)
            cur = []
    if len(cur) >= args.min:
        segs.append(cur)
    if not segs:
        print("no race segment of >= %d windows (no [native] machines receipts?)" % args.min)
        return
    print("%d contiguous race segment(s) of >= %d windows (1 window = 64 frames); wall=%s" % (
        len(segs), args.min, pick_wall(ws, args.wall)))
    peaks = 0
    which = pick_wall(ws, args.wall)
    for g in segs:
        ms = [wall(w, which) for w in g]
        cmds = [w.ni.get("cmds", 0.0) for w in g]
        lags = [(l, autocorr(ms, l)) for l in range(1, min(args.lags + 1, len(g) - 3))]
        lags = [(l, v) for l, v in lags if v is not None]
        body = [v for l, v in lags if l >= 2]
        peak = bool(body) and max(body) > lags[0][1] and max(body) > 0.3
        peaks += peak
        print("\nw%d-%d  n=%d  ms %.1f..%.1f  r(ms,cmds)=%+.3f%s" % (
            g[0].i, g[-1].i, len(g), min(ms), max(ms), pearson(ms, cmds),
            "   <-- SECONDARY PEAK" if peak else ""))
        print("    " + "  ".join("L%d %+.2f" % (l, v) for l, v in lags))
    print("\n%d of %d segment(s) show a secondary autocorrelation peak beyond lag 1." % (
        peaks, len(segs)))


def cmd_cov(args):
    ws = parse(args.log)
    need(ws, args.log)
    c = covsched.parse(args.log)
    if not c.frames:
        raise SystemExit(6)
    span = max(c.frames)
    print("census frames 1..%d, %d dynlod window(s) x 64 = %d frames  (ratio %.3f)" % (
        span, len(ws), 64 * len(ws), span / float(64 * len(ws) or 1)))
    if not 0.9 <= span / float(64 * len(ws) or 1) <= 1.1:
        print("WARNING: the census and the dynlod window count disagree by more than 10% —")
        print("         the per-window join below is not trustworthy on this log.")
    live = [{} for _ in ws]          # window -> key -> frames live
    for f in c.frames:
        w = (f - 1) // 64
        if w >= len(ws):
            break
        for k in c.sets[f]:
            live[w][k] = live[w].get(k, 0) + 1
    ms = [wall(w, pick_wall(ws, None)) for w in ws]
    hi = statistics.median(ms)
    burst = [i for i, v in enumerate(ms) if v > hi]
    quiet = [i for i, v in enumerate(ms) if v <= hi]
    print("split at the median wall %.1f ms: %d burst window(s), %d quiet" % (
        hi, len(burst), len(quiet)))
    rows = []
    allkeys = set()
    for d in live:
        allkeys |= set(d)
    for k in allkeys:
        xs = [live[i].get(k, 0) / 64.0 for i in range(len(ws))]
        if len(set(xs)) < 3:
            continue
        pb = sum(live[i].get(k, 0) for i in burst) / float(64 * len(burst) or 1)
        pq = sum(live[i].get(k, 0) for i in quiet) / float(64 * len(quiet) or 1)
        rows.append((pearson(xs, ms), pb, pq, covsched.key_name(k)))
    rows.sort(key=lambda r: -abs(r[0]))
    print()
    print("%-34s %8s %8s %8s" % ("[cov] key (residency per window)", "r(ms)", "burst", "quiet"))
    for r, pb, pq, name in rows[:args.top]:
        print("%-34s %+8.3f %7.0f%% %7.0f%%" % (name, r, 100 * pb, 100 * pq))
    fam = {}
    for k in allkeys:
        fam.setdefault(k >> 24, []).append(k)
    print()
    print("%-12s %6s %10s" % ("family", "keys", "best |r|"))
    for f in sorted(fam):
        best = 0.0
        for k in fam[f]:
            xs = [live[i].get(k, 0) / 64.0 for i in range(len(ws))]
            if len(set(xs)) >= 3:
                best = max(best, abs(pearson(xs, ms)))
        print("%-12s %6d %10.3f" % (covsched.FAMILY.get(f, "fam%02x" % f), len(fam[f]), best))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    for name, fn, extra in (("windows", cmd_windows, "race"), ("corr", cmd_corr, "race+top+wall"),
                            ("tx", cmd_tx, ""), ("shots", cmd_shots, "top"),
                            ("events", cmd_events, ""), ("period", cmd_period, "period+wall"),
                            ("cov", cmd_cov, "top")):
        p = sub.add_parser(name)
        p.add_argument("log")
        if "race" in extra:
            p.add_argument("--race", action="store_true",
                           help="only windows where the native mesh walker drew machines")
        if "top" in extra:
            p.add_argument("--top", type=int, default=20)
        if "wall" in extra:
            p.add_argument("--wall", choices=("dynlod", "prof"), default=None,
                           help="which frame-time signal to correlate against; the default "
                                "picks [dynlod] when it is live and [prof] when it is not")
        if "period" in extra:
            p.add_argument("--min", type=int, default=8, help="shortest race segment to use")
            p.add_argument("--lags", type=int, default=8, help="lags to report (1 lag = 64 frames)")
        p.set_defaults(fn=fn)
    args = ap.parse_args(argv)
    if not args.cmd:
        ap.print_help()
        return 2
    args.fn(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
