#!/usr/bin/env bash
# tools/native/nrr_size.sh ELF [--list FILE] [--md] [--direct]
#
# Hot-code size receipts of the NRR (n60 design 1.1): per category, the bytes of the functions its
# steady-state loop runs, plus the citro3d functions they reach, against the budget.
#
# Which functions are hot (the convention of port/3ds/gfx/nrr/gdx3ds_nrr.h, "how to add a category"):
#   * core: every `NrrCoreHot*` function, the public hot entries (NrrStateApply NrrSetInputs
#     NrrSetFog NrrRingAlloc NrrDraw2D NrrDrawXf NrrBegin NrrEnd gdx3ds_nrr_marker gdx3ds_nrr_draw)
#     and the backend bridge calls a steady frame makes (gdx3ds_nrr_bk_tex / _alloc /
#     _frame_active / _proj3d, GfxRenderingAPIC3D::NrrTex); plus the emitter's own functions:
#     `NrrCoreC3d*` (the citro3d path, [native] nrr_direct=0, the default view) or, with --direct,
#     `NrrCoreDx*` (the direct command words, nrr_direct=1: n60core2-progress.md);
#   * category C: every `Nrr<C>Hot*` function (C = Hud Trk Mach Sky Dress Shd Fx Selftest Deco Race Menu;
#     race = nirace's span programs between the markers, gdx_nirace.cpp; menu = nimenu's non-race 2D
#     screen runner, which shares the HUD's op bodies through nrr_hot.list);
#   * plus `cat symbol` lines of tools/native/nrr_hot.list (or --list FILE) for anything a
#     category cannot rename (a C++ method, a shared helper).
# The citro3d part: the functions with a citro3d name (C3D_* C3Di_* BufInfo_* AttrInfo_* Mtx_*
# gdx_c3d_* / C3Dgdx_*) reachable from the category's hot set through direct `bl`/`b` calls,
# followed transitively inside citro3d. Function sizes are an upper bound on the lines a loop
# fetches (cold blocks sit inside a function), same caveat as hudsky-progress 5.
#
# What has to fit the 16 KB L1-I at once is core + one category + its citro3d path (design 1.1:
# ~8 KB); the per-category line prints that sum.
set -euo pipefail

ELF=""
LIST=""
MD=0
DIRECT=0
while [ $# -gt 0 ]; do
    case "$1" in
        --list) LIST="$2"; shift 2 ;;
        --md) MD=1; shift ;;
        --direct) DIRECT=1; shift ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *) ELF="$1"; shift ;;
    esac
done
if [ -z "$ELF" ] || [ ! -f "$ELF" ]; then
    echo "usage: $0 ELF [--list FILE] [--md]" >&2
    exit 2
fi
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$LIST" ] && [ -f "$HERE/nrr_hot.list" ]; then
    LIST="$HERE/nrr_hot.list"
fi
BIN="${DEVKITARM:-/opt/devkitpro/devkitARM}/bin"
NM="$BIN/arm-none-eabi-nm"
OBJDUMP="$BIN/arm-none-eabi-objdump"

exec python3 - "$ELF" "$LIST" "$MD" "$NM" "$OBJDUMP" "$DIRECT" <<'PY'
import re, subprocess, sys
from collections import defaultdict

elf, listfile, md, nm, objdump = sys.argv[1], sys.argv[2], sys.argv[3] == "1", sys.argv[4], sys.argv[5]
direct = sys.argv[6] == "1"
EMITTER = "NrrCoreDx" if direct else "NrrCoreC3d"
print("emitter: %s (%s)" % (EMITTER + "*", "direct command words, nrr_direct=1" if direct
                            else "citro3d path, nrr_direct=0"))

# design 1.1 budgets (bytes); the citro3d path is shared (~3 KB)
BUDGET = {"core": 2048, "hud": 3072, "menu": 5120, "trk": 3072, "mach": 2560, "sky": 2048,
          "dress": 3072, "shd": 3072, "fx": 3072, "selftest": 1024, "deco": 1024, "race": 2048,
          "machmenu": 3072, "course": 6144, "c3d": 3072}
CATS = ["hud", "trk", "mach", "sky", "dress", "shd", "fx", "selftest", "deco", "race", "menu",
        "machmenu", "course"]
# nimach: the menus' machines (NrrMachMenuHot*; prefix not derivable from the category name)
CAT_PREFIX = {"machmenu": "NrrMachMenuHot"}
CORE_PUBLIC = {"NrrStateApply", "NrrSetInputs", "NrrSetFog", "NrrRingAlloc", "NrrDraw2D", "NrrDrawXf",
               "NrrBegin", "NrrEnd", "gdx3ds_nrr_marker", "gdx3ds_nrr_draw", "gdx3ds_nrr_bk_tex",
               "gdx3ds_nrr_bk_alloc", "gdx3ds_nrr_bk_frame_active", "gdx3ds_nrr_bk_proj3d", "gdx3ds_nrr_bk_proj2d",
               "Fast::GfxRenderingAPIC3D::NrrTex"}
C3D_RE = re.compile(r"^(C3D_|C3Di_|BufInfo_|AttrInfo_|Mtx_|gdx_c3d_|C3Dgdx_|GPUCMD_)")

# name -> (addr, size); demangled names, clone suffixes folded into the base name's entry list
syms = {}
for line in subprocess.run([nm, "-C", "-S", "--defined-only", elf], capture_output=True, text=True,
                           check=True).stdout.splitlines():
    parts = line.split(None, 3)
    if len(parts) < 4 or parts[2].lower() not in ("t", "w"):
        continue
    addr, size, name = int(parts[0], 16), int(parts[1], 16), parts[3]
    syms[name] = (addr, size)


def base(name):
    n = name.replace("(anonymous namespace)::", "")
    n = n.split("(")[0]
    return n


def classify(name):
    b = base(name)
    short = b.split("::")[-1]
    if short.startswith("NrrCoreHot") or short.startswith(EMITTER) or b in CORE_PUBLIC or short in CORE_PUBLIC:
        return "core"
    for c in CATS:
        if short.startswith(CAT_PREFIX.get(c, "Nrr%sHot" % c.capitalize())):
            return c
    return None


hot = defaultdict(set)
for name in syms:
    c = classify(name)
    if c:
        hot[c].add(name)
if listfile:
    for lineno, line in enumerate(open(listfile), 1):
        line = line.split("#")[0].strip()
        if not line:
            continue
        cat, sym = line.split(None, 1)
        matched = False
        for name in syms:
            if name == sym or base(name) == sym or base(name).split("::")[-1] == sym:
                hot[cat].add(name)
                matched = True
        if not matched:
            # nimenu W4 (review §4 hot trims): a listed symbol that matches nothing in the linked
            # binary silently stopped contributing to its category's count -- exactly how a stale
            # entry (removed/renamed function) could sit here understating nothing visibly. Warn
            # instead of staying silent, same spirit as the `race SpDmaIo` entry this round's own
            # check found and dropped.
            print("WARNING: %s:%d: %r (category %r) matches no symbol in the linked binary -- "
                  "stale listing?" % (listfile, lineno, sym, cat), file=sys.stderr)

# direct call graph of the hot functions + citro3d, from the disassembly of each one
addr2name = {a: n for n, (a, s) in syms.items()}
CALL_RE = re.compile(r"\s(?:bl|blx|b)\s+([0-9a-f]+)\s+<([^>]+)>")


def callees(name):
    addr, size = syms[name]
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", "--start-address=0x%x" % addr,
                          "--stop-address=0x%x" % (addr + size), elf], capture_output=True, text=True).stdout
    res = set()
    for m in CALL_RE.finditer(out):
        tgt = int(m.group(1), 16)
        if tgt in addr2name and not (addr <= tgt < addr + size):
            res.add(addr2name[tgt])
    return res


cache = {}


def calls(name):
    if name not in cache:
        cache[name] = callees(name)
    return cache[name]


def c3d_closure(roots):
    seen, stack = set(), []
    for r in roots:
        for c in calls(r):
            if C3D_RE.match(base(c)) and c not in seen:
                seen.add(c)
                stack.append(c)
    while stack:
        n = stack.pop()
        for c in calls(n):
            if C3D_RE.match(base(c)) and c not in seen:
                seen.add(c)
                stack.append(c)
    return seen


def total(names):
    return sum(syms[n][1] for n in names)


core = hot.get("core", set())
core_c3d = c3d_closure(core)
rows = []
for cat in ["core"] + CATS:
    names = hot.get(cat, set())
    if cat != "core" and not names:
        continue
    c3d = c3d_closure(names) | (core_c3d if cat != "core" else set())
    own = total(names)
    rows.append((cat, own, BUDGET.get(cat, 0), total(c3d), names, c3d))

fmt_row = "| %s | %d | %d | %s | %d | %d |" if md else "%-9s hot=%6d B budget=%6d %-4s c3d=%6d B  core+cat+c3d=%6d B"
if md:
    print("| set | hot bytes | budget | ok | citro3d reached | core + set + citro3d |")
    print("|---|---|---|---|---|---|")
core_own = rows[0][1] if rows else 0
for cat, own, bud, c3d, names, c3dn in rows:
    together = own + c3d + (core_own if cat != "core" else 0)
    ok = "ok" if own <= bud else "OVER"
    print(fmt_row % (cat, own, bud, ok, c3d, together))
# the core split: what runs once per marker (hand-off) vs inside a category's draw loop
HANDOFF = {"gdx3ds_nrr_marker", "gdx3ds_nrr_draw", "NrrCoreHotRun", "NrrBegin", "NrrEnd", "NrrCoreC3dRestore",
           "NrrCoreC3dFirstTouch", "NrrCoreC3dSaveGroup", "gdx3ds_nrr_bk_frame_active", "NrrCoreC3dProj",
           "NrrCoreC3dProj3d", "gdx3ds_nrr_bk_proj3d", "gdx3ds_nrr_bk_proj2d", "NrrCoreDxBegin", "NrrCoreDxEnd", "NrrCoreDxEndUniforms",
           "NrrCoreDxFirstTouch", "NrrCoreDxProj", "NrrCoreDxProj3d", "NrrCoreDxAttr"}
ONLY2D = {"NrrDraw2D", "NrrCoreDxDraw2D", "NrrCoreC3dDraw2D", "NrrCoreC3dProj", "gdx3ds_nrr_bk_proj2d"}
ONLY3D = {"NrrDrawXf", "NrrCoreDxDrawXf", "NrrCoreC3dDrawXf", "NrrCoreC3dProj3d", "gdx3ds_nrr_bk_proj3d",
          "NrrCoreDxXfUniforms"}


def short(n):
    return base(n).split("::")[-1]


if rows:
    core_names = rows[0][4]
    per_marker = [n for n in core_names if short(n) in HANDOFF]
    only2d = [n for n in core_names if short(n) in ONLY2D and short(n) not in HANDOFF]
    only3d = [n for n in core_names if short(n) in ONLY3D and short(n) not in HANDOFF]
    shared = [n for n in core_names if n not in per_marker and n not in only2d and n not in only3d]
    c3d_loop = total(c3d_closure([n for n in core_names if n not in per_marker]))
    print("core split: per-draw loop %d B, once per marker (hand-off, dispatch, restore) %d B"
          % (core_own - total(per_marker), total(per_marker)))
    print("core per-draw loop by draw kind: 2D %d B, gpuxf %d B (shared %d + 2D-only %d / xf-only %d);"
          " + citro3d reached from the loop %d B -> 2D loop + emission %d B, gpuxf loop + emission %d B"
          % (total(shared) + total(only2d), total(shared) + total(only3d), total(shared), total(only2d),
             total(only3d), c3d_loop, total(shared) + total(only2d) + c3d_loop,
             total(shared) + total(only3d) + c3d_loop))
if not md:
    for cat, own, bud, c3d, names, c3dn in rows:
        print("\n[%s] hot functions:" % cat)
        for n in sorted(names, key=lambda n: -syms[n][1]):
            print("  %6d  %s" % (syms[n][1], n))
        print("[%s] citro3d reached:" % cat)
        for n in sorted(c3dn, key=lambda n: -syms[n][1]):
            print("  %6d  %s" % (syms[n][1], n))
PY
