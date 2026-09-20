#!/usr/bin/env python3
"""fhdiff.py — compare the [fh] frame-hash sequences of two G-Diffuser logs.

    tools/dev/fhdiff.py a.log b.log [--eye L] [--key frame|tick|seq] [--context N] [--quiet]

Parses every line of the form

    [fh] frame=1234 tick=1233 eye=L w=400 h=240 hash=8f2a41c9 tag=race00 mode=1

(any extra key=value tokens are carried along; Azahar's azahar_log.txt prefixes are tolerated
because the parser looks for the "[fh] " marker anywhere in the line). Frames are matched by
`--key` (default `frame`, the host loop counter; `tick` matches the input tick; `seq` matches
the Nth receipt of each log regardless of numbering). Reports:

  * the first divergent frame (with the tags/modes around it, ±--context receipts),
  * the total number of compared / divergent / only-in-one-log frames,
  * a platform warning when both logs carry a "[fh] init ... platform=" receipt and disagree
    (Azahar vs hardware hashes never match — parity is always same-platform).

`--known 1702,1703` names frames with a DOCUMENTED single-frame render transient (see
oracle-progress.md "frame 1702"); they are still compared and printed, but a divergence there
is reported as "known" and does not fail the run. Default: none — strict.

Exit status: 0 identical over the compared frames, 1 divergent, 2 usage / nothing to compare.
Stdlib only.
"""
import argparse
import re
import sys

FH_RE = re.compile(r"\[fh\] (.*)$")
KV_RE = re.compile(r"(\w+)=(\S+)")


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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--eye", default="L", help="eye to compare (L, R); default L")
    ap.add_argument("--key", default="frame", choices=["frame", "tick", "seq"])
    ap.add_argument("--context", type=int, default=3)
    ap.add_argument("--quiet", action="store_true", help="only the summary line")
    ap.add_argument("--known", default="", help="comma-separated frame keys whose divergence is a documented transient")
    args = ap.parse_args()
    known = set(int(k) for k in args.known.split(",") if k.strip())

    ra, pa, ba = parse_log(args.a)
    rb, pb, bb = parse_log(args.b)
    ra = [r for r in ra if r.get("eye", "L") == args.eye]
    rb = [r for r in rb if r.get("eye", "L") == args.eye]
    if not ra or not rb:
        print("fhdiff: no [fh] receipts for eye=%s in %s" % (args.eye, args.a if not ra else args.b))
        return 2
    if pa and pb and pa != pb:
        print("fhdiff: WARNING platform mismatch: %s=%s %s=%s — cross-platform hashes never match" % (args.a, pa, args.b, pb))
    if ba and bb and ba != bb:
        print("fhdiff: WARNING build mismatch: %s=%s %s=%s" % (args.a, ba, args.b, bb))

    da = {}
    db = {}
    order_a = []
    for i, r in enumerate(ra):
        k = key_of(r, args.key, i)
        da[k] = r
        order_a.append(k)
    for i, r in enumerate(rb):
        db[key_of(r, args.key, i)] = r

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
            print("first divergence at %s=%s" % (args.key, first))
            lo = max(0, idx - args.context)
            hi = min(len(common), idx + args.context + 1)
            for k in common[lo:hi]:
                print(("  > " if k == first else "    ") + describe(k))
    print("fhdiff: compared=%d divergent=%d known_transient=%d only_in_a=%d only_in_b=%d first_divergent=%s last_common=%s"
          % (len(common), len(divergent), len(known_hits), len(only_a), len(only_b),
             divergent[0] if divergent else "none", common[-1] if common else "none"))
    if known_hits and not args.quiet:
        print("  known transient frames that differed: %s" % ", ".join(str(k) for k in known_hits))
    if not common:
        return 2
    return 1 if divergent else 0


if __name__ == "__main__":
    sys.exit(main())
