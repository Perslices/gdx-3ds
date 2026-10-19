#!/usr/bin/env python3
"""nicensus_tables.py -- markdown tables from [debug] nicensus receipts (gdx_nicensus.cpp).

    tools/native/nicensus_tables.py LOG [LOG...] [--label NAME ...] [--merge]

Reads the LAST cumulative dump of each log (`[nicensus] final ...` at exit, else the last
`[nicensus] sum ...`), and prints per-log tables (overview, time, geometry, top opcodes,
features). --merge also prints one overview across all logs, frame-weighted per bucket.
Continuation lines (`... b=XX.S + ...`) are joined to their head line. Stdlib only.
"""
import argparse
import re
import sys
from collections import OrderedDict, defaultdict

SLOTS = ["walkB", "walkW", "nrr", "nrrhud", "hud", "seam", "mesh", "frame"]
HEAD = re.compile(r"\[nicensus\] (final|sum) b=([0-9A-F]{2})\.(\d)( \+)? (.*)$")
BEGIN = re.compile(r"\[nicensus\] (final|sum) begin dump=(\d+) frames=(\d+)")


def last_dump(path):
    """Lines of the last complete dump (final preferred)."""
    dumps = []
    cur = None
    with open(path, "r", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            i = line.find("[nicensus]")
            if i < 0:
                continue
            line = line[i:]
            m = BEGIN.match(line)
            if m:
                cur = {"kind": m.group(1), "frames": int(m.group(3)), "lines": []}
                dumps.append(cur)
                continue
            if cur is not None and line.startswith("[nicensus] %s end" % cur["kind"]):
                cur["done"] = True
                cur = None
                continue
            if cur is not None:
                cur["lines"].append(line)
    done = [d for d in dumps if d.get("done")]
    finals = [d for d in done if d["kind"] == "final"]
    if finals:
        return finals[-1]
    return done[-1] if done else None


def parse_buckets(dump):
    """bucket -> list of (section, text) with continuation lines joined."""
    out = OrderedDict()
    for line in dump["lines"]:
        m = HEAD.match(line)
        if not m:
            continue
        key = (m.group(2), int(m.group(3)))
        body = m.group(5)
        cont = m.group(4) is not None
        out.setdefault(key, [])
        if cont and out[key]:
            out[key][-1] = out[key][-1] + " " + body
        else:
            out[key].append(body)
    return out


def kv(text):
    return dict(re.findall(r"([A-Za-z0-9_/()+\-]+)=([^ |]+)", text))


def num(s, default=0.0):
    try:
        return float(s)
    except (TypeError, ValueError):
        return default


def bucket_record(key, sections):
    rec = {"key": "%s.%d" % key, "ops": {}, "geo": {}, "nrr": {}}
    for sec in sections:
        if sec.startswith("ms "):
            d = kv(sec.split("|")[0])
            rec["ms"] = {k: num(d.get(k)) for k in ["task"] + SLOTS}
        elif sec.startswith("geo "):
            for slot, val in re.findall(r" (\w+)=([0-9.]+/[0-9./]+)", sec):
                rec["geo"][slot] = [num(x) for x in val.split("/")]
        elif sec.startswith("nrr "):
            for cat, val in re.findall(r" (\w+)=([0-9.]+/[0-9.]+/[0-9.]+)", sec):
                rec["nrr"][cat] = [num(x) for x in val.split("/")]
        elif sec.startswith("ops "):
            m = re.match(r"ops (\w+) n=([0-9.]+)(.*)", sec)
            if m:
                rec["ops"][m.group(1)] = (num(m.group(2)), re.findall(r"([0-9A-F]{2})=([0-9.]+)", m.group(3)))
        elif sec.startswith("allops"):
            rec["allops"] = dict((o, int(v)) for o, v in re.findall(r"([0-9A-F]{2})=(\d+)", sec))
        elif sec.startswith("feat "):
            rec["feat"] = sec
        elif sec.startswith("mw/f"):
            rec["mw"] = sec
        elif sec.startswith("settile"):
            rec["tile"] = sec
        else:
            m = re.match(r"(\S+) frames=(\d+) nrrframes=(\d+) lus=(\d+) cmds/f=([0-9.]+)(.*)", sec)
            if m:
                rec["name"] = m.group(1)
                rec["frames"] = int(m.group(2))
                rec["nrrframes"] = int(m.group(3))
                rec["lus"] = int(m.group(4))
                rec["cmds"] = num(m.group(5))
                d = kv(m.group(6).split("|")[0])
                rec["cmd"] = {k: num(d.get(k)) for k in SLOTS}
                rec["words_nrrhud"] = num(re.search(r"words nrrhud=([0-9.]+)", m.group(6)).group(1))
                rec["words_hudret"] = num(re.search(r"hudret=([0-9.]+)", m.group(6)).group(1))
                rec["f3d"] = num(re.search(r"f3d=([0-9.]+)", m.group(6)).group(1))
    return rec


OPNAMES = {
    "00": "NOOP", "01": "VTX", "02": "MODVTX", "03": "CULLDL", "04": "BRZ", "05": "TRI1", "06": "TRI2",
    "07": "QUAD", "08": "LINE3D", "3A": "XGEOM", "D6": "DMAIO", "D7": "TEXTURE", "D8": "POPMTX",
    "D9": "GEOM", "DA": "MTX", "DB": "MOVEWORD", "DC": "MOVEMEM", "DD": "LDUCODE", "DE": "DL", "DF": "ENDDL",
    "E0": "SPNOOP", "E1": "HALF1", "E2": "OML", "E3": "OMH", "E4": "TEXRECT", "E5": "TEXRECTF", "E6": "LDSYNC",
    "E7": "PIPESYNC", "E8": "TILESYNC", "E9": "FULLSYNC", "ED": "SCISSOR", "EE": "PRIMDEPTH", "EF": "RDPOM",
    "F0": "LOADTLUT", "F1": "HALF2", "F2": "TILESIZE", "F3": "LOADBLOCK", "F4": "LOADTILE", "F5": "SETTILE",
    "F6": "FILLRECT", "F7": "FILLCOL", "F8": "FOGCOL", "F9": "BLENDCOL", "FA": "PRIMCOL", "FB": "ENVCOL",
    "FC": "COMBINE", "FD": "SETTIMG", "FE": "ZIMG", "FF": "CIMG",
}


def fmt(v, nd=1):
    if v == 0:
        return "0"
    return ("%%.%df" % nd) % v


def print_tables(label, recs, out):
    out.write("\n#### %s\n\n" % label)
    out.write("| bucket | mode/sub | frames | NRR fr | LUS | cmds/f | walkB | walkW | nrr | nrrhud | hud | seam | mesh "
              "| NRR-HUD words/f | HUD retained words/f |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
    for r in recs:
        c = r["cmd"]
        out.write("| %s | %s | %d | %d | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n" % (
            r["key"], r["name"], r["frames"], r["nrrframes"], r["lus"], fmt(r["cmds"]), fmt(c["walkB"]),
            fmt(c["walkW"]), fmt(c["nrr"]), fmt(c["nrrhud"]), fmt(c["hud"]), fmt(c["seam"]), fmt(c["mesh"]),
            fmt(r["words_nrrhud"]), fmt(r["words_hudret"])))
    out.write("\nSelf ms per rendered frame (emulator, Old 3DS model; relative only):\n\n")
    out.write("| bucket | mode/sub | task | walkB | walkW | nrr | nrrhud | hud | seam | mesh | frame |\n"
              "|---|---|---|---|---|---|---|---|---|---|---|\n")
    for r in recs:
        m = r.get("ms", {})
        out.write("| %s | %s | %s |" % (r["key"], r["name"], fmt(m.get("task", 0), 2)))
        out.write("".join(" %s |" % fmt(m.get(k, 0), 2) for k in SLOTS) + "\n")
    out.write("\nInterpreter geometry per frame, by context (tris submitted / emitted / rects / TMEM loads / "
              "texture imports / batches drawn / vertices) and NRR draws (markers/draws/verts):\n\n")
    out.write("| bucket | mode/sub | walk (B+W) | hud | seam | mesh | nrrhud | NRR total mk/dr/vx |\n"
              "|---|---|---|---|---|---|---|---|\n")
    for r in recs:
        g = r["geo"]

        def gs(slots):
            vals = [0.0] * 7
            for s in slots:
                for i, v in enumerate(g.get(s, [0.0] * 7)):
                    vals[i] += v
            if sum(vals) == 0:
                return "-"
            return "/".join(fmt(v) for v in vals)

        nmk = sum(v[0] for v in r["nrr"].values())
        ndr = sum(v[1] for v in r["nrr"].values())
        nvx = sum(v[2] for v in r["nrr"].values())
        out.write("| %s | %s | %s | %s | %s | %s | %s | %s |\n" % (
            r["key"], r["name"], gs(["walkB", "walkW"]), gs(["hud"]), gs(["seam"]), gs(["mesh"]), gs(["nrrhud"]),
            "-" if nmk == 0 else "%s/%s/%s" % (fmt(nmk), fmt(ndr), fmt(nvx, 0))))
    out.write("\nTop opcodes per frame (walk = walkB+walkW as printed; other contexts when non-zero):\n\n")
    out.write("| bucket | mode/sub | context | n/f | top opcodes |\n|---|---|---|---|---|\n")
    for r in recs:
        for slot, (n, ops) in r["ops"].items():
            top = ", ".join("%s %s" % (OPNAMES.get(o, o), v) for o, v in ops[:10])
            out.write("| %s | %s | %s | %s | %s |\n" % (r["key"], r["name"], slot, fmt(n), top))
    out.write("\nMicrocode features (fractions are of interpreted draw commands; /f per frame):\n\n")
    out.write("| bucket | mode/sub | features | moveword / movemem / variant | tile formats |\n|---|---|---|---|---|\n")
    for r in recs:
        out.write("| %s | %s | %s | %s | %s |\n" % (r["key"], r["name"], r.get("feat", "").replace("feat ", ""),
                                                  r.get("mw", ""), r.get("tile", "")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--label", action="append", default=[])
    ap.add_argument("--merge", action="store_true")
    a = ap.parse_args()
    allrecs = []
    for i, path in enumerate(a.logs):
        label = a.label[i] if i < len(a.label) else path
        d = last_dump(path)
        if d is None:
            sys.stdout.write("\n#### %s\n\nno complete [nicensus] dump\n" % label)
            continue
        recs = [bucket_record(k, v) for k, v in parse_buckets(d).items()]
        recs = [r for r in recs if "frames" in r]
        allrecs.append((label, recs))
        print_tables("%s (%s dump, %d frames)" % (label, d["kind"], d["frames"]), recs, sys.stdout)
    if a.merge and allrecs:
        acc = defaultdict(lambda: defaultdict(float))
        names = {}
        for _, recs in allrecs:
            for r in recs:
                k = r["key"]
                names[k] = r["name"]
                f = r["frames"]
                acc[k]["frames"] += f
                acc[k]["nrrframes"] += r["nrrframes"]
                acc[k]["cmds"] += r["cmds"] * f
                for s in SLOTS:
                    acc[k]["c_" + s] += r["cmd"].get(s, 0) * f
                    acc[k]["m_" + s] += r.get("ms", {}).get(s, 0) * f
                acc[k]["m_task"] += r.get("ms", {}).get("task", 0) * f
                acc[k]["w_nrrhud"] += r["words_nrrhud"] * f
                g = r["geo"]
                for s in ("walkB", "walkW", "hud", "seam", "mesh", "nrrhud"):
                    v = g.get(s, [0.0] * 7)
                    acc[k]["tri"] += v[0] * f
                    acc[k]["rect"] += v[2] * f
                    acc[k]["ld"] += v[3] * f
                    acc[k]["draw"] += v[5] * f
                acc[k]["nrrdr"] += sum(v[1] for v in r["nrr"].values()) * f
        sys.stdout.write("\n#### merged (frame-weighted over all logs)\n\n")
        sys.stdout.write("| bucket | mode/sub | frames | NRR fr | cmds/f | walkB | walkW | nrrhud | hud | seam | mesh "
                         "| NRR-HUD words/f | interp tris/rects/loads/batches /f | NRR draws/f | ms task | ms walk | "
                         "ms nrr | ms nrrhud | ms hud | ms seam | ms mesh | ms frame |\n")
        sys.stdout.write("|" + "---|" * 22 + "\n")
        for k in sorted(acc):
            v = acc[k]
            f = v["frames"] or 1.0
            sys.stdout.write("| %s | %s | %d | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s/%s/%s/%s | %s | %s | %s "
                             "| %s | %s | %s | %s | %s | %s |\n" % (
                                 k, names[k], v["frames"], v["nrrframes"], fmt(v["cmds"] / f), fmt(v["c_walkB"] / f),
                                 fmt(v["c_walkW"] / f), fmt(v["c_nrrhud"] / f), fmt(v["c_hud"] / f),
                                 fmt(v["c_seam"] / f), fmt((v["c_mesh"] + v["c_nrr"]) / f), fmt(v["w_nrrhud"] / f),
                                 fmt(v["tri"] / f), fmt(v["rect"] / f), fmt(v["ld"] / f), fmt(v["draw"] / f),
                                 fmt(v["nrrdr"] / f), fmt(v["m_task"] / f, 2),
                                 fmt((v["m_walkB"] + v["m_walkW"]) / f, 2), fmt(v["m_nrr"] / f, 2),
                                 fmt(v["m_nrrhud"] / f, 2), fmt(v["m_hud"] / f, 2), fmt(v["m_seam"] / f, 2),
                                 fmt(v["m_mesh"] / f, 2), fmt(v["m_frame"] / f, 2)))


if __name__ == "__main__":
    main()
