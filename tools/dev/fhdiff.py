#!/usr/bin/env python3
"""fhdiff.py — compare the [fh] frame-hash sequences of two G-Diffuser logs.

    tools/dev/fhdiff.py a.log b.log [--eye L|R|both] [--key frame|tick|seq] [--context N] [--quiet]

Parses every line of the form

    [fh] frame=1234 tick=1233 eye=L w=400 h=240 hash=8f2a41c9 tag=race00 mode=1

(any extra key=value tokens are carried along; Azahar's azahar_log.txt prefixes are tolerated
because the parser looks for the "[fh] " marker anywhere in the line). Frames are matched by
`--key` (default `frame`, the host loop counter; `tick` matches the input tick; `seq` matches
the Nth receipt of each log regardless of numbering). Reports:

  * the first divergent frame (with the tags/modes around it, ±--context receipts),
  * the total number of compared / divergent / only-in-one-log frames,
  * a platform warning when both logs carry a "[fh] init ... platform=" receipt and disagree
    (Azahar vs hardware hashes never match — parity is always same-platform),
  * a `lane:` line naming each log's RENDER-THREAD MODE and `[native]` levers, always — and a
    warning when the two render-thread modes differ.

The render-thread line is unconditional on purpose. Every frame-hash parity run this fork made
before 2026-09-23 was reported as `renderthread=0` on the strength of a `.gdxrec` header field
that is descriptive only and was never applied; the RetroArch lane had in fact been running the
thread ON since the `[debug]` strip landed, and nobody could tell from a transcript. A parity
claim has to carry the state of the thread it was measured with, so fhdiff prints it.

`--eye both` is the stereo form: a run with `[debug] fh_eye=both` emits one `eye=L` and one
`eye=R` receipt per captured frame, and this compares the two streams SEPARATELY (an eye is
only ever matched against the same eye) and prints one summary line per eye plus a combined
`eyes=L,R status=` verdict. It fails if either eye diverges, and says so explicitly when the
right-eye stream is missing entirely (the right-eye pass never ran).

`--known 1702,1703` names frames with a DOCUMENTED single-frame render transient (see
oracle-progress.md "frame 1702"); they are still compared and printed, but a divergence there
is reported as "known" and does not fail the run. Default: none — strict.

Exit status: 0 identical over the compared frames, 1 divergent, 2 usage / nothing to compare.
Stdlib only.
"""
import argparse
import os
import re
import sys

FH_RE = re.compile(r"\[fh\] (.*)$")
KV_RE = re.compile(r"(\w+)=(\S+)")


RT_RE = re.compile(r"\[rt\] (mode=\w+) core=")
RT_OFF_RE = re.compile(r"\[rt\] (off|unavailable)\b")
NATIVE_RE = re.compile(r"\[native\] (all=.*?)\s*\(resolved")


def parse_lane(path):
    """The render-thread mode and [native] lever set a log was produced with.

    Both come from boot receipts, so this is a cheap second pass over the same file.
    `rt` is 'mode=pipe' / 'mode=ahead' / 'mode=sync' / 'off' / 'unavailable', or None when
    the log carries no [rt] receipt at all (a build without the thread, or a truncated log).
    """
    rt = None
    levers = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            if rt is None:
                m = RT_RE.search(line)
                if m:
                    rt = m.group(1)
                else:
                    m = RT_OFF_RE.search(line)
                    if m:
                        rt = m.group(1)
            if levers is None:
                m = NATIVE_RE.search(line)
                if m:
                    levers = m.group(1)
            if rt is not None and levers is not None:
                break
    return rt, levers


def parse_log(path):
    receipts = []
    platform = None
    build = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = FH_RE.search(line)
            if not m:
                continue
            body = m.group(1).strip()
            kv = dict(KV_RE.findall(body))
            if body.startswith("init "):
                platform = kv.get("platform")
                build = kv.get("build")
                continue
            if "hash" not in kv or "frame" not in kv:
                continue
            receipts.append(kv)
    return receipts, platform, build


def key_of(kv, key, seq):
    if key == "seq":
        return seq
    return int(kv.get(key, -1))


def _run_renderer(log_path):
    """Best-effort: the renderer a log's run used, from the run dir's manifest.


    """
    import json
    d = os.path.dirname(os.path.abspath(log_path))
    for _ in range(3):
        m = os.path.join(d, "manifest.json")
        if os.path.isfile(m):
            try:
                with open(m) as f:
                    return ((json.load(f).get("platform") or {}).get("gfx"), m)
            except (ValueError, OSError):
                return (None, None)
        d = os.path.dirname(d)
    return (None, None)


def compare_eye(eye, all_a, all_b, args, known):
    """One eye's stream. -> exit status (0 same, 1 divergent, 2 nothing to compare).

    With `--eye both` this runs once per eye; the [fh] receipts of the two eyes share a
    `frame`/`tick` number (one capture, two readbacks: gdx3ds_oracle.c CaptureEye), so
    filtering by eye first is what keeps the two streams from being matched against each
    other.
    """
    label = "[eye=%s] " % eye if args.eye == "both" else ""
    ra = [r for r in all_a if r.get("eye", "L") == eye]
    rb = [r for r in all_b if r.get("eye", "L") == eye]
    if not ra or not rb:
        missing = args.a if not ra else args.b
        print("fhdiff: %sno [fh] receipts for eye=%s in %s" % (label, eye, missing))
        if eye == "R":
            # The usual cause, worth naming: the right-eye pass never ran, so the capture
            # logged "eye=R inactive" (no hash). Stereo needs [stereo] enabled=1 AND a
            # nonzero emulated 3D slider (`retro run --3d` / `emu run --3d`).
            print("fhdiff: %s  -> the right-eye pass did not run (look for `eye=R inactive` "
                  "and `[stereo] right-eye pass ACTIVE` in the log)" % label)
        return 2

    rta, leva = parse_lane(args.a)
    rtb, levb = parse_lane(args.b)
    if not args.quiet:
        print("fhdiff: lane a: renderthread %s | native %s" % (rta or "unknown", leva or "unknown"))
        print("fhdiff: lane b: renderthread %s | native %s" % (rtb or "unknown", levb or "unknown"))
    if rta and rtb and rta != rtb:
        print("fhdiff: WARNING render-thread mismatch: %s=%s %s=%s — the pipelined renderer is "
              "a different execution, not a different setting" % (args.a, rta, args.b, rtb))
        if args.key == "frame" and getattr(args, "shift", 0) == 0 and ("off" in (rta, rtb)):
            print("fhdiff: NOTE off-vs-pipe/ahead on --key frame needs --shift: the thread-on "
                  "lane presents each game frame one host frame later (latency=1). Try "
                  "--shift 1 if B is the thread-on log, -1 if A is.")
            if len(ra) < 200:
                print("fhdiff: NOTE these logs look SHOT-sampled, and a shift cannot be "
                      "expressed on sparse samples — a cross-mode comparison needs fh_every=1. "
                      "Compare thread-on against thread-on instead.")

    da = {}
    db = {}
    order_a = []
    for i, r in enumerate(ra):
        k = key_of(r, args.key, i)
        da[k] = r
        order_a.append(k)
    for i, r in enumerate(rb):
        db[key_of(r, args.key, i) + getattr(args, "shift", 0)] = r
    if getattr(args, "shift", 0) and not args.quiet:
        print("fhdiff: --shift %+d applied to %s's %s keys" % (getattr(args, "shift", 0), args.b, args.key))

    common = [k for k in order_a if k in db]
    only_a = [k for k in order_a if k not in db]
    only_b = [k for k in sorted(db) if k not in da]
    divergent_all = [k for k in common if da[k]["hash"] != db[k]["hash"]]
    known_hits = [k for k in divergent_all if k in known]
    divergent = [k for k in divergent_all if k not in known]

    def describe(k):
        a = da.get(k)
        b = db.get(k)
        parts = ["%s=%s" % (args.key, k)]
        if a and b:
            parts.append("A=%s B=%s" % (a["hash"], b["hash"]))
            parts.append("tag=%s/%s" % (a.get("tag"), b.get("tag")))
            parts.append("mode=%s/%s" % (a.get("mode", "?"), b.get("mode", "?")))
            parts.append("tick=%s/%s" % (a.get("tick", "?"), b.get("tick", "?")))
            parts.append("SAME" if a["hash"] == b["hash"] else "DIFF")
        return " ".join(parts)

    if divergent:
        first = divergent[0]
        idx = common.index(first)
        if not args.quiet:
            print("%sfirst divergence at %s=%s" % (label, args.key, first))
            lo = max(0, idx - args.context)
            hi = min(len(common), idx + args.context + 1)
            for k in common[lo:hi]:
                print(("  > " if k == first else "    ") + describe(k))
    print("fhdiff: %scompared=%d divergent=%d known_transient=%d only_in_a=%d only_in_b=%d first_divergent=%s last_common=%s"
          % (label, len(common), len(divergent), len(known_hits), len(only_a), len(only_b),
             divergent[0] if divergent else "none", common[-1] if common else "none"))
    if known_hits and not args.quiet:
        print("  %sknown transient frames that differed: %s" % (label, ", ".join(str(k) for k in known_hits)))
    if not common:
        return 2
    return 1 if divergent else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--eye", default="L",
                    help="eye to compare: L, R, or `both` — with `both` the two per-eye "
                         "streams are compared independently and the run fails if EITHER "
                         "diverges (default L)")
    ap.add_argument("--key", default="frame", choices=["frame", "tick", "seq"])
    ap.add_argument("--context", type=int, default=3)
    ap.add_argument("--quiet", action="store_true", help="only the summary line")
    ap.add_argument("--known", default="", help="comma-separated frame keys whose divergence is a documented transient")
    ap.add_argument("--shift", type=int, default=0, metavar="N",
                    help="add N to every key in B before matching. The pipelined renderer "
                         "presents each game frame one host frame later than the sequential "
                         "one (the [rt] receipt states latency=1), so with A=thread-off and "
                         "B=thread-on a --key frame gate is --shift 1. Measured, not assumed: "
                         "rtparity-progress.md.")
    args = ap.parse_args()
    known = set(int(k) for k in args.known.split(",") if k.strip())

    ga, _ = _run_renderer(args.a)
    gb, _ = _run_renderer(args.b)
    if ga and gb and ga != gb:
        print("fhdiff: WARNING renderer differs (%s vs %s) — `retro run` falls back on a "
              "boot crash and frame hashes are renderer-specific" % (ga, gb))

    ra, pa, ba = parse_log(args.a)
    rb, pb, bb = parse_log(args.b)
    if pa and pb and pa != pb:
        print("fhdiff: WARNING platform mismatch: %s=%s %s=%s — cross-platform hashes never match" % (args.a, pa, args.b, pb))
    if ba and bb and ba != bb:
        print("fhdiff: WARNING build mismatch: %s=%s %s=%s" % (args.a, ba, args.b, bb))

    eyes = ["L", "R"] if args.eye == "both" else [args.eye]
    codes = [compare_eye(eye, ra, rb, args, known) for eye in eyes]
    if len(eyes) > 1:
        verdict = "PASS" if codes == [0] * len(codes) else "FAIL"
        print("fhdiff: eyes=%s status=%s (%s)"
              % (",".join(eyes), verdict,
                 " ".join("%s:%s" % (e, {0: "same", 1: "divergent", 2: "no-data"}[c])
                          for e, c in zip(eyes, codes))))
    # Worst status wins: divergence (1) is a harder failure than nothing to compare (2)
    # only in the sense that it is a real answer — report 1 if any eye diverged, else 2 if
    # any eye had no data, else 0.
    if 1 in codes:
        return 1
    if 2 in codes:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
