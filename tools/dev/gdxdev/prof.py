"""Log parsing: [prof]/[prof!]/[profop]/[gpu]/[fill]/[mprof]/[rt]/[watchdog] + error census.

The receipt formats are FIXED in the source and must not be re-guessed
(port/3ds/gfx/gdx3ds_gpu_prof.c:211,428,432; port/3ds/gdx3ds_renderthread.cpp:512;
port/3ds/gdx3ds_mainprof.c:154,165; port/3ds/main_3ds.cpp:208). Everything here is a
tolerant key=value scan rather than a positional regex, so a build that adds a column at
the end does not break the parser.
"""

import math
import re

from . import util
from .util import GdxError

# Error receipts to count. `[fatal]`/`[fatal-trace]` fail a run; the rest are census.
ERROR_TAGS = [
    "[fatal]", "[fatal-trace]", "[gfxfail]", "[gdl-bad]", "[gdl-miss]",
    "[datafail]", "[texdiag]", "bad_alloc",
]
FATAL_TAGS = ("[fatal]", "[fatal-trace]")

# Receipts that carry an error TAG but are emitted on a healthy run. The atexit tracer
# fires from the normal exit() path (port/3ds/main_3ds.cpp:815), so counting it made
# EVERY clean run FAIL on "1 [fatal] line(s)". Counted separately, never as a fatal.
BENIGN_RECEIPTS = (
    "[fatal-trace] atexit:",
)

PROF_SECTIONS = ["br", "dsp", "vtx", "tri", "imp", "drw", "mtx", "oth"]
PROF_COUNTS = ["vtxN", "nV", "nT", "nI", "nD", "nM", "nRun"]

# [mprof] MAIN-THREAD sections (port/3ds/gdx3ds_mainprof.h). The order here is the order
# the columns print in; it is grouped, not ranked — `--main` ranks by value.
# Game-fiber (the submitting side) first, then the host loop, then the residual.
MPROF_FIB = ["logic", "dlb", "brm", "aud", "lseg", "sla", "seg", "dma", "stask", "sub"]
MPROF_HOST = ["apt", "pace", "inp", "beg", "vi", "disp", "jdp", "jtop", "end", "tail"]
MPROF_SECTIONS = MPROF_FIB + MPROF_HOST + ["oth"]
# Buckets in which main is BLOCKED rather than working. `sub` and `jdp` are waits for the
# render thread, `jtop` is the APT frame-boundary join, `pace` is the deliberate vblank
# alignment. Separated because they answer a different question from the work buckets: the
# work columns say what main would have to get faster, the wait columns say whether main is
# the bottleneck at all. In the emulator the wait columns are inflated by host contention
# (core 2 is time-sliced onto the same host thread), so a lever decision reads the work
# columns and the hardware [rt] receipt, never the emulator's waits.
MPROF_WAIT = ["sub", "jdp", "jtop", "pace"]
MPROF_WORK = [s for s in MPROF_SECTIONS if s not in MPROF_WAIT]
# Emitted over two lines sharing one frame= (each stays inside the 224-byte log line cap).
MPROF_PARTS = 2

# `key=value` where value is a number, optionally a/b, optionally %-suffixed.
_KV = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(-?[0-9]+(?:\.[0-9]+)?)")
# [watchdog] frame=214(+214)
_WD_FRAME = re.compile(r"frame=(\d+)\(\+(\d+)\)")
# [profop] top entries: `06=7.44/256`
_OPENT = re.compile(r"\b([0-9A-Fa-f]{2})=([0-9.]+)/(\d+)\b")

WATCHDOG_PERIOD_S = 5.0  # port/3ds/main_3ds.cpp: "~1 svc line per 5 s"


def is_benign(line):
    """True for a receipt that carries an error TAG but is emitted on a healthy run."""
    return any(b in line for b in BENIGN_RECEIPTS)


def error_census(*texts):
    """THE error classifier. Every verdict and report must go through this one function.

    Two things it gets right that a per-sink sum does not:

    1. Benign receipts are excluded (see BENIGN_RECEIPTS). emu.judge used to re-derive
       its own fatal count from the raw tag totals, which is exactly how the two paths
       drifted apart and why `[fatal-trace] atexit` kept failing clean runs after prof.py
       had been fixed.
    2. **Sinks are deduplicated.** port/3ds/main_3ds.cpp:412 `logFatal()` writes the same
       string to svcOutputDebugString (the Azahar log) AND the SD filelog, so one crash
       lands in both texts. Summing them reported "2 [fatal] line(s)" for a single
       assert. Counting the max across sinks gives the true count when both captured it
       and still the best estimate when only one did.
    """
    per_sink = []
    benign = 0
    lines = []
    for text in texts:
        counts = {t: 0 for t in ERROR_TAGS}
        for raw in (text or "").splitlines():
            if is_benign(raw):
                benign += 1
                continue
            for tag in ERROR_TAGS:
                if tag in raw:
                    counts[tag] += 1
                    if len(lines) < 200:
                        lines.append(raw.strip())
        per_sink.append(counts)
    errors = {t: max((c[t] for c in per_sink), default=0) for t in ERROR_TAGS}
    return {"errors": errors,
            "fatals": sum(errors[t] for t in FATAL_TAGS),
            "benign": benign,
            "lines": lines}


def _kv(line):
    return {m.group(1): float(m.group(2)) for m in _KV.finditer(line)}


def _tag_of(line):
    """Leading [tag] of a log line, ignoring any emulator log prefix."""
    i = line.find("[")
    if i < 0:
        return None
    j = line.find("]", i)
    if j < 0:
        return None
    return line[i:j + 1]


class Parsed:
    def __init__(self):
        self.prof = []       # [prof] windows: dict(frame,n,sections...,counts...)
        self.prof_storm = [] # [prof!] windows (storm frames only)
        self.profop = []     # dict(n=..., ops=[(op,ms,calls)...])
        self.gpu = []
        self.fill = []
        self.rt = []
        self.mprof = []      # [mprof] windows: dict(frame,n,nD,dl,tot,<sections>)
        self.watchdog = []   # dict(beat,frame,delta,stage,shot,...)
        self.errors = {t: 0 for t in ERROR_TAGS}
        self.benign = 0
        self.error_lines = []
        self.lines = 0
        self.gputrace = None  # True if "[gpu] tracing enabled" seen

    @property
    def fatals(self):
        return sum(self.errors[t] for t in FATAL_TAGS)

    def is_empty(self):
        return not (self.prof or self.gpu or self.watchdog or self.rt or self.fill
                    or self.mprof)


def parse_text(text):
    p = Parsed()
    # A10: a SHOT is a screenshot the REPLAY asked for -- a readback plus a 230-288 KB BMP
    # write, all on the main thread. On hardware that cost ~2 s per image, so every beat that
    # carried one collapsed into the teens in EVERY lever configuration and the fps table was
    # measuring the harness. Beats are tagged with whether a SHOT landed inside them (by log
    # position: a SHOT line after beat k-1 and before beat k belongs to beat k) so a caller
    # can price the run without them.
    shots_pending = 0
    for raw in text.splitlines():
        p.lines += 1
        line = raw.rstrip()
        if not line:
            continue
        if is_benign(line):
            p.benign += 1
        else:
            for tag in ERROR_TAGS:
                if tag in line:
                    p.errors[tag] += 1
                    if len(p.error_lines) < 200:
                        p.error_lines.append(line.strip())
        if "SHOT captured" in line or "SHOT dumped" in line or "SHOT scanout dumped" in line:
            shots_pending += 1
        tag = _tag_of(line)
        if tag is None:
            continue
        if tag == "[prof]" or tag == "[prof!]":
            d = _kv(line)
            if "frame" not in d or "n" not in d:
                continue
            rec = {"frame": int(d["frame"]), "n": int(d["n"])}
            for k in PROF_SECTIONS:
                rec[k] = d.get(k, 0.0)
            for k in PROF_COUNTS:
                rec[k] = int(d.get(k, 0))
            rec["total"] = sum(rec[s] for s in PROF_SECTIONS)
            (p.prof_storm if tag == "[prof!]" else p.prof).append(rec)
        elif tag == "[profop]":
            d = _kv(line.split("top:")[0])
            ops = [(m.group(1).upper(), float(m.group(2)), int(m.group(3)))
                   for m in _OPENT.finditer(line.split("top:", 1)[-1])]
            p.profop.append({"n": int(d.get("n", 0)), "ops": ops})
        elif tag == "[gpu]":
            if "tracing enabled" in line:
                p.gputrace = True
                continue
            d = _kv(line)
            if "frame" in d:
                p.gpu.append({k: d[k] for k in d})
        elif tag == "[fill]":
            d = _kv(line)
            if "frame" in d:
                p.fill.append(d)
        elif tag == "[mprof]":
            d = _kv(line)
            if "frame" not in d or "n" not in d or "part" not in d:
                continue  # the arming ack ("[mprof] main-thread profiling enabled ...")
            frame, n = int(d["frame"]), int(d["n"])
            # Merge the two parts of one window. They are emitted back to back from the
            # same MProfEmit call, so "the open record with this frame" is unambiguous;
            # a truncated log that lost one part simply leaves those columns at 0.
            rec = None
            if p.mprof and p.mprof[-1]["frame"] == frame:
                rec = p.mprof[-1]
            else:
                rec = {"frame": frame, "n": n, "parts": 0}
                for k in MPROF_SECTIONS:
                    rec[k] = 0.0
                for k in ("nD", "dl", "tot", "fib", "host", "tasks"):
                    rec[k] = 0.0
                p.mprof.append(rec)
            rec["parts"] += 1
            for k in MPROF_SECTIONS + ["nD", "dl", "tot", "fib", "host", "tasks"]:
                if k in d:
                    rec[k] = d[k]
            rec["nD"] = int(rec["nD"])
        elif tag == "[rt]":
            d = _kv(line)
            if "frame" in d and "n" in d:
                m = re.search(r"mode=(\S+)", line)
                d["mode"] = m.group(1) if m else "?"
                p.rt.append(d)
        elif tag == "[watchdog]":
            m = _WD_FRAME.search(line)
            if not m:
                continue
            d = _kv(line)
            p.watchdog.append({
                "beat": int(d.get("beat", 0)),
                "frame": int(m.group(1)),
                "delta": int(m.group(2)),
                "stage": int(d.get("stage", -1)),
                "heapUsed": int(d.get("heapUsed", 0)),
                "shots": shots_pending,
            })
            shots_pending = 0
    return p


def parse_file(path):
    text = util.read_text(path)
    if not text:
        raise GdxError("log is empty or unreadable: %s" % path, util.EXIT_NODATA)
    return parse_text(text)


# --------------------------------------------------------------------- stats --
def percentile(values, pct):
    """Nearest-rank percentile (ceil(P/100 * N)), 1-indexed then clamped.

    Deterministic and stdlib-only; no interpolation, so a printed p50 is always a value
    that actually occurred in the run.
    """
    if not values:
        return None
    s = sorted(values)
    k = int(math.ceil((pct / 100.0) * len(s)))
    k = max(1, min(len(s), k))
    return s[k - 1]


def mean(values):
    return (sum(values) / float(len(values))) if values else None


def fps_series(parsed, skip_first=1, exclude_shots=False):
    """fps from [watchdog] frame deltas / 5 s (docs/3DS-HARDWARE.md:177-180).

    The first beat's delta counts everything since process start (boot + asset load), so
    it is dropped by default — it is not a frame rate.

    exclude_shots drops every beat a replay SHOT landed in, for the same reason: the
    screenshot's readback and BMP write are harness cost on the main thread, not the
    game's frame rate.
    """
    beats = parsed.watchdog[skip_first:]
    if exclude_shots:
        beats = [b for b in beats if not b.get("shots")]
    return [b["delta"] / WATCHDOG_PERIOD_S for b in beats]


def fps_summary(parsed, skip_first=1, exclude_shots=False):
    series = fps_series(parsed, skip_first, exclude_shots)
    if not series:
        return None
    shot_beats = sum(1 for b in parsed.watchdog[skip_first:] if b.get("shots"))
    return {
        "shot_beats": shot_beats,
        "beats": len(series),
        "p10": percentile(series, 10),
        "p50": percentile(series, 50),
        "p90": percentile(series, 90),
        "min": min(series),
        "max": max(series),
        "mean": mean(series),
        "max_frame": parsed.watchdog[-1]["frame"] if parsed.watchdog else None,
    }


def section_summary(windows):
    """Mean/p50 per [prof] section over a set of windows, weighted by nothing:
    each window is already a per-frame average over its own n frames."""
    if not windows:
        return None
    out = {"windows": len(windows), "frames": sum(w["n"] for w in windows)}
    for s in PROF_SECTIONS + ["total"]:
        vals = [w[s] for w in windows]
        out[s] = {"mean": mean(vals), "p50": percentile(vals, 50),
                  "min": min(vals), "max": max(vals)}
    for c in PROF_COUNTS:
        out[c] = mean([w[c] for w in windows])
    return out


# nD buckets. The campaign's A/B convention is "crowd (nD>=100) vs menu"
# (05-devcli-plan.md §4); the finer edges keep the transitional windows visible
# instead of smearing them into one of the two ends.
VTXN_BUCKETS = [(0, 1, "idle"), (1, 16, "menu"), (16, 50, "light"),
                (50, 100, "mid"), (100, 10 ** 9, "crowd")]


def bucket_by_nd(windows):
    """Group [prof] windows by nD (draw count), the load proxy an A/B must hold fixed."""
    groups = []
    for lo, hi, name in VTXN_BUCKETS:
        sel = [w for w in windows if lo <= w["nD"] < hi]
        if sel:
            label = "%s (nD %d-%s)" % (name, lo, "inf" if hi > 10 ** 8 else str(hi - 1))
            groups.append((label, sel))
    return groups


def mprof_summary(windows):
    """Mean/p50 per [mprof] section over a set of windows. `total` is the SUM of the
    sections, which by construction is the main thread's wall ms/frame — the buckets
    partition the frame rather than sampling it (gdx3ds_mainprof.h, "THE MODEL"), so a
    total that drifts from the emitted tot= column would mean a lost part= line."""
    if not windows:
        return None
    out = {"windows": len(windows), "frames": sum(w["n"] for w in windows)}
    for s in MPROF_SECTIONS:
        vals = [w[s] for w in windows]
        out[s] = {"mean": mean(vals), "p50": percentile(vals, 50),
                  "min": min(vals), "max": max(vals)}
    totals = [sum(w[s] for s in MPROF_SECTIONS) for w in windows]
    out["total"] = {"mean": mean(totals), "p50": percentile(totals, 50),
                    "min": min(totals), "max": max(totals)}
    out["wait"] = mean([sum(w[s] for s in MPROF_WAIT) for w in windows])
    out["work"] = mean([sum(w[s] for s in MPROF_WORK) for w in windows])
    out["fib"] = mean([sum(w[s] for s in MPROF_FIB) for w in windows])
    out["host"] = mean([sum(w[s] for s in MPROF_HOST) for w in windows])
    out["emitted_tot"] = mean([w["tot"] for w in windows])
    out["nD"] = mean([w["nD"] for w in windows])
    out["dl"] = mean([w["dl"] for w in windows])
    return out


def mprof_ranked(summary, top=None, sections=None, denom=None):
    """(section, ms, pct-of-denominator) rows, biggest first — the A8 deliverable.

    `sections=MPROF_WORK, denom="work"` gives the ranking that transfers off the
    emulator: what main actually SPENDS, with the host-contention-inflated waits out."""
    sections = sections or MPROF_SECTIONS
    total = summary[denom] if denom else summary["total"]["mean"]
    total = total or 1.0
    rows = [(s, summary[s]["mean"], 100.0 * summary[s]["mean"] / total) for s in sections]
    rows.sort(key=lambda r: -(r[1] or 0))
    return rows[:top] if top else rows


def op_summary(parsed, top=8):
    """Accumulate [profop] top-N entries across windows: mean ms/frame per opcode."""
    acc = {}
    for w in parsed.profop:
        for op, ms, calls in w["ops"]:
            a = acc.setdefault(op, {"ms": [], "calls": []})
            a["ms"].append(ms)
            a["calls"].append(calls)
    rows = [(op, mean(v["ms"]), mean(v["calls"]), len(v["ms"])) for op, v in acc.items()]
    rows.sort(key=lambda r: -(r[1] or 0))
    return rows[:top]
