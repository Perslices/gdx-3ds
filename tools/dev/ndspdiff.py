#!/usr/bin/env python3
"""ndspdiff.py — compare the [ndsphash] render digests of two G-Diffuser logs.

    tools/dev/ndspdiff.py a.log b.log [--min-shared N] [--min-shared-pct P] [--context N] [--quiet]

This is to audio what tools/dev/fhdiff.py is to frames, and it exists because fhdiff cannot
gate an audio change and never could. Four tracks changed audio behaviour in this port and
every one of them signed off with the same sentence: nothing listened. `audiodiet` §6.6 proved
only that "the driver asked for the same bytes and got identical bytes" and called the
rendering an INFERENCE. `covfinish` refused an audio census family outright, because a feature
"could be recorded but never SAMPLED, so it would read as covered with no gate behind it".
`audioasync` §5 added a level census and quoted it as weak in the same breath.

WHAT IS COMPARED, and why it is not the PCM stream. The audio producer is a free-running OS
thread whose per-tick AI buffer length is derived from the 3DS ring occupancy, so two identical
runs cut the same music into different-sized chunks: `audiodiet` measured 3452 vs 3439 reads on
an identical pair, `audioasync` measured sil_ppk at 37,056 vs 37,648. A sample-exact hash of
the output would therefore fail at random, which is worse than no gate.

So the guest emits a MAP instead, one entry per audio task:

    [ndsphash] e 1234 A1B2C3D4E5F60718 9ABCDEF0
               |    |                  |
               |    |                  value: a digest of every byte that task wrote back
               |    key: a CONTENT-ADDRESSED digest of that tick's ABI command list --
               |         what to decode, at what pitch, at what envelope volume, into what
               |         mix, plus the CONTENT of every sample byte, codebook and carry-in
               |         state it named. RDRAM addresses are masked out, because a
               |         sample-cache slot moves between runs and moves again when
               |         [debug] audio_stream changes the byte source.
               task index (not matched on; the streams are matched by KEY)

Two runs are compared on the keys they SHARE: the same render request must produce the same
bytes, in any run, at any tick index, at any wall-clock. That is timing-independent BY
CONSTRUCTION, which is the property every previous attempt lacked. Keys only one run produced
are drift -- a tick the other run never performed -- and are reported separately.

    ndspdiff: compared=5909 divergent=0 only_in_a=0 only_in_b=0 shared=100.0% first_divergent=none

THE TWO FAILURE CLASSES LAND IN DIFFERENT FIELDS, and a verdict needs both:

  * `divergent` > 0 -- the same request rendered differently. A decoder, resampler, mixer or
    reverb regression. This is the headline.
  * `shared` collapsing -- the requests themselves changed, so there is little left to
    compare. Wrong bytes delivered, a dropped sample, a swapped instrument, a pitch error.
    Content-addressing is what puts this class here rather than hiding it.

VACUITY IS A FAILURE, NOT A PASS. `divergent=0` over four shared keys means nothing, and that
is precisely the "reads as covered with no gate behind it" failure covfinish refused a census
family over. `--min-shared` (default 500 keys) and `--min-shared-pct` (default 50%) are floors;
below either one this exits 2 with NOTHING COMPARED rather than printing a green line. Take the
floors from a measured identical pair on your own lane, not from these defaults.

WHAT THIS STILL CANNOT CATCH is the SPLICE. Two runs cut the stream at different samples, so
this says nothing about WHERE in time a note landed, and it cannot see the ring, the drain, the
underrun padding or anything downstream of the DSP -- [audioring]'s sil_ppk is the instrument
for that half and it is not deterministic. See docs/research/native/ndsphash-progress.md §6.

STRICT MODE, which is what the two determinism pins buy. With `[debug] ndsphash_pin=1
ndsphash_lockstep=1` on both lanes the audio render is reproducible, an identical pair produces
only_in_a=0 only_in_b=0 EXACTLY, and the whole session collapses to one 64-bit `fold=`. So
`only_in_*` stops being drift and becomes the signal for the second failure class, and this
tool fails on it (status=KEYSET-DIVERGENT) as well as on `divergent` and on a fold mismatch.
Strict mode turns itself on when both lanes report the pins; --no-strict-keys overrides.

Measured on the canonical replay, one binary at one HEAD:

  two identical runs   compared=5909 divergent=0 only_in_a=0 only_in_b=0  fold identical
  one flipped bit of
    one rendered sample  divergent=1 first_divergent=...@task3000         FIRES
  one flipped bit of
    one delivered byte   divergent=0 only_in_a=1 only_in_b=1 fold differs FIRES (strict)

Exit status: 0 identical, 1 divergent / keyset-divergent / fold mismatch, 2 usage or nothing
to compare. Stdlib only.
"""
import argparse
import re
import sys

E_RE = re.compile(r"\[ndsphash\] e (\d+) ([0-9A-Fa-f]{16}) ([0-9A-Fa-f]{8})")
INIT_RE = re.compile(r"\[ndsphash\] init (.*)$")
FINAL_RE = re.compile(r"\[ndsphash\] FINAL (.*)$")
SELFDIV_RE = re.compile(r"\[ndsphash\] SELFDIV (.*)$")
FOLD_RE = re.compile(r"\bfold=([0-9A-Fa-f]{16})")
LLE_RE = re.compile(r"\[ndsphash\] executor=LLE")

# The lever banner. PROTOCOL: read the banner for both lanes before believing a result, and
# check [det] init det=1 before reporting any divergence. An --ini typo that silently disabled
# the determinism pins once cost a three-commit bisect of a divergence that was not one.
DET_RE = re.compile(r"\[det\] init (det=\d+.*?)$")
DIET_RE = re.compile(r"\[audio-diet\] (mode=\S+)")
STREAM_RE = re.compile(r"\[audio-diet\] .*?\b(verify=\S+)")
RING_RE = re.compile(r"\[audioring\] FINAL .*?(sil_ppk=\d+)")
RT_RE = re.compile(r"\[rt\] (mode=\w+) core=")


def parse_log(path):
    """-> (pairs, meta). `pairs` maps key -> (value, first task index, count)."""
    pairs = {}
    dup_conflict = 0
    meta = {
        "init": None, "final": [], "selfdiv": 0, "lle": False,
        "det": None, "diet": None, "verify": None, "ring": None, "rt": None,
        "fold": None, "pinned": False, "lines": 0,
    }
    with open(path, "r", errors="replace") as f:
        for line in f:
            meta["lines"] += 1
            m = E_RE.search(line)
            if m:
                tick = int(m.group(1))
                key = m.group(2).upper()
                val = m.group(3).upper()
                prev = pairs.get(key)
                if prev is None:
                    pairs[key] = [val, tick, 1]
                else:
                    prev[2] += 1
                    if prev[0] != val:
                        # The same request rendered twice differently INSIDE one log. The
                        # guest reports this as `selfdiv`; counting it here too means a
                        # single-log run is still checkable when the pair stream is all you
                        # have. Keep the first value so the cross-run match stays defined.
                        dup_conflict += 1
                continue
            m = INIT_RE.search(line)
            if m and meta["init"] is None:
                meta["init"] = m.group(1).strip()
                # Both determinism pins on: this lane's audio render is reproducible, so the
                # KEY SET is signal too and not only the values. See STRICT MODE below.
                meta["pinned"] = ("aipin=1" in meta["init"]
                                  and "lockstep=1" in meta["init"])
                continue
            m = FINAL_RE.search(line)
            if m:
                body = m.group(1).strip()
                meta["final"].append(body)
                mf = FOLD_RE.search(body)
                if mf:
                    meta["fold"] = mf.group(1).upper()
                continue
            if SELFDIV_RE.search(line):
                meta["selfdiv"] += 1
                continue
            if LLE_RE.search(line):
                meta["lle"] = True
                continue
            for name, rx in (("det", DET_RE), ("diet", DIET_RE), ("verify", STREAM_RE),
                             ("ring", RING_RE), ("rt", RT_RE)):
                if meta[name] is None:
                    m = rx.search(line)
                    if m:
                        meta[name] = m.group(1).strip()
    meta["dup_conflict"] = dup_conflict
    return pairs, meta


def banner(tag, path, meta):
    print("ndspdiff: lane %s: %s" % (tag, path))
    print("ndspdiff: lane %s: ndsphash %s | %s | audio-diet %s %s | %s | audioring %s" % (
        tag,
        meta["init"] or "NO INIT RECEIPT",
        meta["det"] or "NO [det] RECEIPT",
        meta["diet"] or "?",
        meta["verify"] or "",
        meta["rt"] or "no [rt] receipt",
        meta["ring"] or "?",
    ))
    for fin in meta["final"]:
        print("ndspdiff: lane %s: FINAL %s" % (tag, fin))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--min-shared", type=int, default=500,
                    help="refuse a verdict below this many shared keys (default 500). "
                         "A green line over a handful of keys is the vacuous-gate failure.")
    ap.add_argument("--min-shared-pct", type=float, default=50.0,
                    help="refuse a verdict when shared keys are below this %% of the larger "
                         "log's key count (default 50).")
    ap.add_argument("--context", type=int, default=4,
                    help="divergent keys to print in full (default 4)")
    ap.add_argument("--strict-keys", dest="strict", action="store_true", default=None,
                    help="require only_in_a == only_in_b == 0. Default: ON when BOTH lanes "
                         "report aipin=1 lockstep=1, because under those two pins an identical "
                         "pair produces IDENTICAL KEY SETS and any difference is signal.")
    ap.add_argument("--no-strict-keys", dest="strict", action="store_false",
                    help="report only_in_* without failing on it, even under the pins.")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    pa, ma = parse_log(args.a)
    pb, mb = parse_log(args.b)

    if not args.quiet:
        banner("a", args.a, ma)
        banner("b", args.b, mb)

    # --- the refusals, before any number is quoted ---------------------------------
    for tag, m, path in (("a", ma, args.a), ("b", mb, args.b)):
        if m["lle"]:
            print("ndspdiff: REFUSING — lane %s ran the LLE executor ([audio] lle=1). The "
                  "value side of the digest is folded inside the HLE interpreter, so that "
                  "lane recorded no pairs." % tag, file=sys.stderr)
            return 2
        if m["init"] is None and not m["final"]:
            print("ndspdiff: REFUSING — lane %s (%s) carries no [ndsphash] receipt at all. "
                  "Was [debug] ndsphash=2 set? Mode 1 counts but does not stream pairs."
                  % (tag, path), file=sys.stderr)
            return 2

    det_bad = [t for t, m in (("a", ma), ("b", mb))
               if m["det"] is not None and not m["det"].startswith("det=1")]
    if det_bad:
        print("ndspdiff: WARNING — lane(s) %s report det=0. Unpinned seeds explain a "
              "divergence by themselves; re-run with [debug] det=1 explicit."
              % ",".join(det_bad), file=sys.stderr)

    for tag, m in (("a", ma), ("b", mb)):
        n = max(m["selfdiv"], m["dup_conflict"])
        if n:
            print("ndspdiff: WARNING — lane %s contradicts ITSELF on %d key(s): one command "
                  "list rendered two different results inside a single run. That is a "
                  "finding on its own and does not need a second log." % (tag, n),
                  file=sys.stderr)

    ka, kb = set(pa), set(pb)
    shared = ka & kb
    only_a = ka - kb
    only_b = kb - ka
    larger = max(len(ka), len(kb))

    divergent = sorted(
        (k for k in shared if pa[k][0] != pb[k][0]),
        key=lambda k: pa[k][1])

    pct = (100.0 * len(shared) / larger) if larger else 0.0
    first = divergent[0] if divergent else None

    print("ndspdiff: keys_a=%d keys_b=%d" % (len(ka), len(kb)))
    print("ndspdiff: compared=%d divergent=%d only_in_a=%d only_in_b=%d shared=%.1f%% "
          "first_divergent=%s"
          % (len(shared), len(divergent), len(only_a), len(only_b), pct,
             ("%s@task%d" % (first, pa[first][1])) if first else "none"))

    if divergent and not args.quiet:
        print("ndspdiff: the same render request produced different bytes:")
        for k in divergent[:args.context]:
            print("  key=%s  a: task=%-6d val=%s   b: task=%-6d val=%s"
                  % (k, pa[k][1], pa[k][0], pb[k][1], pb[k][0]))
        if len(divergent) > args.context:
            print("  ... and %d more" % (len(divergent) - args.context))

    # --- the vacuity guard ---------------------------------------------------------
    if len(shared) < args.min_shared or pct < args.min_shared_pct:
        print("ndspdiff: NOTHING COMPARED — %d shared keys (%.1f%%) is below the floor "
              "(--min-shared %d, --min-shared-pct %.1f). This is NOT a pass: either the two "
              "runs diverged so far that no tick matched, or one of them barely produced "
              "audio. Check [audioring] sil_ppk and the FINAL pair counts above."
              % (len(shared), pct, args.min_shared, args.min_shared_pct), file=sys.stderr)
        return 2

    # --- the session fold: one number for the whole render ---------------------------
    # Under both pins this is byte-identical between two identical runs (measured:
    # fold=99EA7CAFB387EE40 on both sides of a 5,999-task session), which makes it a complete
    # gate on its own and means [debug] ndsphash=1 -- no pair stream, no 600 KB of log -- is
    # enough to CHECK a change. The pair stream is what LOCALISES one.
    fold_mismatch = False
    if ma["fold"] and mb["fold"]:
        if ma["fold"] == mb["fold"]:
            print("ndspdiff: fold=%s (identical)" % ma["fold"])
        else:
            fold_mismatch = True
            print("ndspdiff: fold a=%s b=%s MISMATCH" % (ma["fold"], mb["fold"]))

    # --- STRICT MODE ------------------------------------------------------------------
    # Without the pins, only_in_* is drift and means nothing. With them, an identical pair
    # produces only_in_a=0 only_in_b=0 exactly, so a nonzero count is a CHANGED RENDER
    # REQUEST: different sample bytes delivered, a different pitch, a different instrument.
    # That class never shows up in `divergent` -- content-addressing puts it here instead --
    # and a source-fault run measured exactly one of each while divergent stayed 0. Reporting
    # that as IDENTICAL, which this tool did before the fault run was taken, would have been
    # a gate that passes a corrupted sample.
    strict = args.strict
    if strict is None:
        strict = ma["pinned"] and mb["pinned"]
        if not strict and not args.quiet:
            print("ndspdiff: note: strict key matching is OFF (one or both lanes lack "
                  "aipin=1 lockstep=1). only_in_* is drift here, not signal, and a changed "
                  "render request can hide in it. Pin both lanes for a real gate.")

    keyset_bad = strict and (only_a or only_b)
    if keyset_bad:
        print("ndspdiff: KEY SET differs under the determinism pins: %d request(s) only in a, "
              "%d only in b. Something asked the DSP for different work." % (len(only_a), len(only_b)))
        for k in sorted(only_a, key=lambda k: pa[k][1])[:args.context]:
            print("  only_in_a key=%s task=%d val=%s" % (k, pa[k][1], pa[k][0]))
        for k in sorted(only_b, key=lambda k: pb[k][1])[:args.context]:
            print("  only_in_b key=%s task=%d val=%s" % (k, pb[k][1], pb[k][0]))

    if divergent or keyset_bad or fold_mismatch:
        print("ndspdiff: status=%s" % (
            "DIVERGENT" if divergent else
            ("KEYSET-DIVERGENT" if keyset_bad else "FOLD-MISMATCH")))
        return 1
    print("ndspdiff: status=IDENTICAL over %d shared render requests%s"
          % (len(shared), " (strict key matching)" if strict else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
