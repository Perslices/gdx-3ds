#!/usr/bin/env python3
"""C4o: turn PC samples into the Old 3DS hot-first .text order (port/3ds/codegen/o3ds-hot.ld).

Inputs:
  --map    a GNU ld map of the SAME .elf the samples were taken from (relink the
           G-Diffuser-3DS.elf command with -Wl,-Map=<file>; `ninja -t commands` prints it)
  samples  one or more files (one GROUP each, emitted in order) of `<pc-hex> <count> [thread] [lr-hex]` lines, e.g. from a
           PC sampler over Azahar's GDB stub (`azahar -g <port>`; the stub reports every
           thread's saved PC, so samples inside the svc wrappers are threads BLOCKED in the
           kernel and are dropped here).

Output: a --section-ordering-file fragment listing the hot .text input sections by
descending sample weight until --coverage of the non-idle samples is reached, plus a
per-TU summary (stderr) that the hot-source list in Gdx3dsCodegen.cmake is picked from.

Section names are matched with a trailing `.*` alternative so GCC clone suffixes that
appear or vanish between -O levels (.isra.0, .part.0, .constprop.0) still match.
"""
import argparse
import bisect
import collections
import re
import sys


def parse_map(path):
    secs = []
    lines = open(path, errors="replace").read().split("\n")
    inmem = False
    outsec = None
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("Linker script and memory map"):
            inmem = True
        if inmem:
            m = re.match(r"^(\.\S+)", line)
            if m:
                outsec = m.group(1)
            m = re.match(r"^ (\.\S+)\s*$", line)
            if m and i + 1 < len(lines):
                m2 = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.*)$", lines[i + 1])
                if m2 and outsec == ".text":
                    secs.append((int(m2.group(1), 16), int(m2.group(2), 16), m.group(1),
                                 m2.group(3).strip()))
                    i += 2
                    continue
            m = re.match(r"^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.*)$", line)
            if m and outsec == ".text":
                secs.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1),
                             m.group(4).strip()))
        i += 1
    secs = sorted(s for s in secs if s[1] > 0)
    return secs


def func_name(isec):
    """.text.unlikely.foo.isra.0 -> foo: the clone suffixes and the hot/unlikely/startup
    prefix both vary with the -O level, so the order file matches the bare name."""
    n = re.sub(r"^\.text\.", "", isec)
    n = re.sub(r"^(hot|unlikely|startup|exit)\.", "", n)
    return re.sub(r"(\.(isra|part|constprop|cold|localalias|lto_priv)(\.\d+)?)+$", "", n)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--map", required=True)
    ap.add_argument("--coverage", type=float, default=0.995)
    ap.add_argument("--out", required=True)
    ap.add_argument("--note", default="", help="provenance line written into the header")
    ap.add_argument("samples", nargs="+")
    args = ap.parse_args()

    secs = parse_map(args.map)
    starts = [s[0] for s in secs]
    chosen = []
    names = set()
    grand_total = grand_acc = grand_idle = hot_bytes = 0
    per_tu = collections.Counter()
    # Each positional file is one GROUP, emitted contiguously in argument order: pass the
    # core-0 threads (render/interp, main, game fiber) first and the core-1 audio producer
    # second, so each core's hot set is one run of pages rather than interleaved with the
    # other core's (every ARM11 core has its own 16 KiB L1 I-cache).
    for path in args.samples:
        weight = collections.Counter()
        total = 0
        for line in open(path):
            f = line.split()
            if len(f) < 2:
                continue
            pc, cnt = int(f[0], 16), int(f[1])
            j = bisect.bisect_right(starts, pc) - 1
            if j < 0 or not (secs[j][0] <= pc < secs[j][0] + secs[j][1]):
                continue
            a, sz, isec, obj = secs[j]
            if isec.startswith(".text.svc") or (isec == ".text" and "svc.o" in obj):
                grand_idle += cnt
                continue
            weight[j] += cnt
            total += cnt
        acc = 0
        for j, cnt in weight.most_common():
            if acc >= args.coverage * total:
                break
            a, sz, isec, obj = secs[j]
            acc += cnt
            per_tu[obj] += cnt
            if isec == ".text":
                continue  # an assembler .text blob (libgcc/libc asm); cannot be named alone
            bn = func_name(isec)
            if bn not in names:
                names.add(bn)
                chosen.append(bn)
                hot_bytes += sz
        grand_total += total
        grand_acc += acc
    total, acc, idle = grand_total, grand_acc, grand_idle
    with open(args.out, "w") as f:
        f.write("/* C4o hot-first .text order -- GENERATED by tools/native/o3ds_hotorder.py.\n"
                "   %d sections = %.1f%% of %d non-idle samples, %d bytes at -O2 (map input).\n"
                "   Used as: ld --section-ordering-file (port/3ds/codegen/Gdx3dsCodegen.cmake).\n"
                "   .crt0 stays first: the 3dsx loader enters at the start of .text.\n"
                "   Profile: %s */\n"
                % (len(chosen), 100.0 * acc / max(total, 1), total, hot_bytes, args.note or "-"))
        f.write(".text : {\n    KEEP(*(.crt0))\n")
        for n in chosen:
            f.write("    *(.text.{0} .text.{0}.* .text.hot.{0} .text.unlikely.{0} .text.unlikely.{0}.* "
                    ".text.startup.{0})\n".format(n))
        f.write("}\n")
    print("samples: %d non-idle, %d idle (svc); hot set %d sections, %d bytes, %.1f%%"
          % (total, idle, len(chosen), hot_bytes, 100.0 * acc / max(total, 1)), file=sys.stderr)
    for obj, cnt in per_tu.most_common():
        print("%7d %5.1f%%  %s" % (cnt, 100.0 * cnt / total, obj), file=sys.stderr)


if __name__ == "__main__":
    main()
