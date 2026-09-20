#!/usr/bin/env python3
"""refrun.py — drive the Linux reference renderer (tools/refbuild) from this Mac.

The desktop build is the only oracle that can say "the 3DS renderer is wrong" rather than
"the two 3DS builds differ" (docs/research/native/00-master-plan.md 4.3). It reads the SAME
fzerox.o2r/gdiffuser.o2r the console reads, is driven by the SAME scripted inputs, and dumps
the SAME 320x240 gFrameMirrorFb through the same gdx_request_frame_dump path
(port/n64_gfx_bridge.cpp) — so a difference in the BMPs is a difference in rendering and
nothing else.

  refrun.py build [--fresh] [-j N]           configure+build G-Diffuser inside the container
  refrun.py run   <scenario> [options]       stage a run dir, run it headless, collect BMPs
  refrun.py emit-3ds <scenario> [-o FILE]    derive the matching sdmc:/gdx-autoinput.txt
  refrun.py list                             list the known scenarios

Nothing here ever writes into the repository: run directories live under the scratch root
(--runs-root, default $GDX_REF_RUNS or /tmp/gdx-refrun), and the game data is COPIED out of the
Azahar virtual SD card. fzerox.o2r is ROM-derived and must never be committed (PROTOCOL.md).

Scenario JSON (tools/dev/refscenarios/*.json):
  id               str   run-directory / manifest name (required)
  title            str   human label
  script           str   GDX_INPUT_SCRIPT file, relative to the scenario file (required)
  shots            list  SHOT labels that MUST appear as autotest/<label>.bmp, else the run fails
  timeout_seconds  int   hard kill deadline (default 300)
  env              obj   extra environment for the child
  cfg              obj   gdiffuser.cfg.json overrides, dotted keys ("Window.Width": 320)
  cvars            obj   CVar overrides, dotted names ("gEnhancements.Graphics.Widescreen": 0)
  emit_3ds         obj   options for emit-3ds (waitmode_ticks, poll_hz)
  notes            list  free text, copied into the manifest
"""

import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SCENARIO_DIR = os.path.join(HERE, "refscenarios")

IMAGE = os.environ.get("GDX_REF_IMAGE", "gdx-ref")
VOLUME = os.environ.get("GDX_REF_VOLUME", "gdx-refbuild")
# NOT /tmp: macOS reaps it, and a run directory that evaporates takes the BMPs a comparison
# cites with it. Outside the repo either way — fzerox.o2r is ROM-derived (PROTOCOL.md).
RUNS_ROOT = os.environ.get("GDX_REF_RUNS", os.path.expanduser("~/gdx-refrun"))
# The virtual SD card the 3DS build reads under Azahar. Copied, never mounted: a crashing
# reference run must not be able to corrupt the console side's game data.
SDMC = os.path.expanduser("~/Library/Application Support/Azahar/sdmc/3ds/gdiffuser")
ARCHIVES = ("fzerox.o2r", "gdiffuser.o2r")

# ── Geometry / fold matching (04-refbuild-plan.md 3.1) ───────────────────────────────────────
#
# Every one of these changes the viewport, the culling or the resample, so a mismatch reads as a
# renderer bug. They are the FLOOR: a scenario's "cfg"/"cvars" may override any of them, and the
# manifest records what was actually used, so a comparison can never silently be run against a
# different geometry than it claims.
BASE_CFG = {
    # 1:1 with gFrameMirrorFb, so CopyFrameBuffer does no resampling at all.
    "Window.Width": 320,
    "Window.Height": 240,
    "Window.PositionX": 0,
    "Window.PositionY": 0,
    "Window.Fullscreen.Enabled": False,
    "Window.Maximized": False,
    # 0 = the SDL2/OpenGL Fast3D backend (the released Linux renderer).
    "Window.Backend.Id": 0,
    "Window.Backend.Name": "OpenGL",
}
BASE_CVARS = {
    # Default is 1 (port/gdx_menu.cpp). Hor+ x-compression on 2D geometry is the single most
    # likely silent geometry mismatch against the console.
    "gEnhancements.Graphics.Widescreen": 0,
    "gEnhancements.Graphics.WidescreenUI": 0,
    "gEnhancements.Graphics.WidescreenSplitUI": 0,
    # gdx_menu.cpp force-sets WidescreenUI back ON (and CVarSave()s) while this migration flag is
    # 0. Without it the "WidescreenUI": 0 above is overwritten before the first frame.
    "gdx.Migrations.WidescreenUiDefaultOn": 1,
    "gEnhancements.Graphics.UltrawideMode": 0,
    "gEnhancements.Graphics.RemoveBorders": 0,
    "gEnhancements.Graphics.HideRaceCurtain": 0,
    "gInternalResolution": 1.0,
    "gMSAAValue": 1,
    "gEnhancements.Graphics.FrameInterpolation": 0,
    "gInterpolationFPS": 30,
    # ON, against the shipped default. Without it the host loop runs as fast as llvmpipe allows,
    # and the boot timeline — which is wall-clock-coupled, not poll-counted — reaches the title
    # at a different POLL INDEX every run: the same `WAIT 1500` landed on the title art in one
    # run and on a black boot frame in the next. Pinning the loop to 59.94 Hz makes polls
    # proportional to wall time, which puts the title at a repeatable poll index. It does NOT
    # make frames bit-identical (the PUSH START blink phase still moves) and it does not touch
    # rendering — it is a sleep+spin in the host loop (port/gdx_frame_pacer.c). Costs ~25 s for
    # a 1500-poll scenario instead of ~8 s.
    "gEnhancements.Graphics.FramePacing": 1,
    # main.cpp force-sets FramePacing back to 0 and CVarSave()s it while this migration marker is
    # 0 — the identical trap as gdx.Migrations.WidescreenUiDefaultOn above.
    "gdx.Migrations.FramePacingDefaultOff": 1,
    # No ImGui furniture in the captured frame.
    "gOpenMenuBar": 0,
}
BASE_ENV = {
    # Determinism pins, proven by the PCM harness (tools/audio_pcm_harness/README.md).
    "GDX_RAND_SEED1": "1",
    "GDX_RAND_SEED2": "2",
    "GDX_AUDIO_THREAD": "0",
    "SDL_AUDIODRIVER": "dummy",
    "SDL_VIDEODRIVER": "x11",
}


def die(msg):
    print("refrun: %s" % msg, file=sys.stderr)
    sys.exit(1)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def docker(args, capture=False, check=True):
    cmd = ["docker"] + args
    if capture:
        p = subprocess.run(cmd, capture_output=True, text=True)
        if check and p.returncode != 0:
            die("docker failed: %s\n%s%s" % (" ".join(cmd), p.stdout, p.stderr))
        return p
    p = subprocess.run(cmd)
    if check and p.returncode != 0:
        die("docker failed: %s" % " ".join(cmd))
    return p


def nest(flat):
    """Dotted keys -> the nested object libultraship's Config actually reads.

    Config::FormatNestedKey (libultraship/src/ship/config/Config.cpp) splits every key on '.'
    and walks the JSON, so "Window.Width" is {"Window": {"Width": ...}}, and a CVar named
    "gEnhancements.Graphics.Widescreen" lives at CVars/gEnhancements/Graphics/Widescreen.
    """
    out = {}
    for key, value in flat.items():
        node = out
        parts = key.split(".")
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = value
    return out


def deep_merge(dst, src):
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            deep_merge(dst[k], v)
        else:
            dst[k] = v
    return dst


# ── scenarios ────────────────────────────────────────────────────────────────────────────────

def load_scenario(path):
    if not os.path.isfile(path):
        alt = os.path.join(SCENARIO_DIR, path)
        if os.path.isfile(alt):
            path = alt
        elif os.path.isfile(alt + ".json"):
            path = alt + ".json"
        else:
            die("scenario not found: %s" % path)
    with open(path, "r", encoding="utf-8") as f:
        sc = json.load(f)
    sc["_path"] = os.path.abspath(path)
    sc["_dir"] = os.path.dirname(os.path.abspath(path))
    for required in ("id", "script"):
        if required not in sc:
            die("scenario %s is missing required key %r" % (path, required))
    return sc


# ── build ────────────────────────────────────────────────────────────────────────────────────

def cmd_build(args):
    inner = "gdx-refbuild -j %d" % args.jobs
    if args.fresh:
        inner += " --fresh"
    if args.target:
        inner += " --target %s" % args.target
    t0 = time.time()
    docker(["run", "--rm",
            "-v", "%s:/src:ro" % REPO,
            "-v", "%s:/build" % VOLUME,
            IMAGE, inner])
    print("[refrun] build wall time: %.1fs" % (time.time() - t0))
    return 0


# ── run ──────────────────────────────────────────────────────────────────────────────────────

def stage(scenario, run_dir, cfg_over, cvar_over):
    """Build the run directory: exe + archives + config + script. Returns a staging record."""
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(os.path.join(run_dir, "autotest"))

    # 1. The binary, out of the build volume (the volume is the build dir; nothing on the host
    #    tree is touched by a build, so this copy is the only way it reaches a run dir).
    docker(["run", "--rm", "-v", "%s:/build" % VOLUME, "-v", "%s:/run" % run_dir, IMAGE,
            "cp /build/port/G-Diffuser /run/G-Diffuser && chmod +x /run/G-Diffuser"])
    exe = os.path.join(run_dir, "G-Diffuser")
    if not os.path.isfile(exe):
        die("no G-Diffuser in the %s volume — run `refrun.py build` first" % VOLUME)

    # 2. Game data, copied out of the Azahar virtual SD so both renderers read the same bytes.
    archive_shas = {}
    for name in ARCHIVES:
        src = os.path.join(SDMC, name)
        if not os.path.isfile(src):
            die("missing %s — the reference must read the SAME archive the 3DS reads" % src)
        shutil.copyfile(src, os.path.join(run_dir, name))
        archive_shas[name] = sha256_file(src)

    # 2b. Mark the run dir as a DEVELOPMENT TREE by putting the same archive at
    #     assets/extracted/generic.o2r. This is not cosmetic: main.cpp runs
    #     GdxExtractEnsureArchive on every non-dev boot, ensureCartArchive re-hashes fzerox.o2r
    #     against this build's golden (port/gen/gdx_o2r_expected.h), and the SD card's archive
    #     legitimately fails it — tools/prebake re-STORES audio_table and
    #     common_assets_compressed uncompressed after the golden gate has passed, so the console's
    #     copy has the same entries, order, sizes and CRCs but a different container hash. Without
    #     this the reference quarantines the console's archive as fzerox.o2r.bad and then dies with
    #     "no ROM file found". DevelopmentTreeProvidesArchive (port/gdx_firstboot.cpp) short-circuits
    #     extraction entirely, and findArchivePaths still prefers fzerox.o2r over generic.o2r, so
    #     the bytes actually mounted are the console's.
    extracted = os.path.join(run_dir, "assets", "extracted")
    os.makedirs(extracted)
    os.link(os.path.join(run_dir, "fzerox.o2r"), os.path.join(extracted, "generic.o2r"))

    # 3. First-boot state. The archive-only boot path (port/gdx_firstboot.cpp FirstBootRun,
    #    "booting archive-only") is what lets a run directory hold no ROM at all: setup is
    #    marked complete and fzerox.o2r satisfies the game-archive gate. There is no
    #    gdx_extract_state.cfg, so no recorded archive_sha256, so acceptance is by presence —
    #    which is correct here, the archive is the one the console just booted.
    with open(os.path.join(run_dir, "gdx_firstboot.cfg"), "w", encoding="utf-8") as f:
        f.write("# Written by tools/dev/refrun.py — headless reference run, archive-only boot.\n")
        f.write("Setup.Complete=1\n")
        f.write("Game.RomPath=\n")
        f.write("Game.DiskPath=\n")
        f.write("Game.DdIplPath=\n")
        f.write("Game.RomSha1=\n")

    # 4. Config.
    cfg_flat = dict(BASE_CFG)
    cfg_flat.update(cfg_over)
    cvar_flat = dict(BASE_CVARS)
    cvar_flat.update(cvar_over)
    cfg = nest(cfg_flat)
    deep_merge(cfg, {"CVars": nest(cvar_flat)})
    with open(os.path.join(run_dir, "gdiffuser.cfg.json"), "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=4, sort_keys=True)

    # 5. Input script.
    src_script = os.path.join(scenario["_dir"], scenario["script"])
    if not os.path.isfile(src_script):
        die("scenario script not found: %s" % src_script)
    shutil.copyfile(src_script, os.path.join(run_dir, "script.txt"))

    return {
        "exe_sha256": sha256_file(exe),
        "archives": archive_shas,
        "script_sha256": sha256_file(src_script),
        "cfg": cfg_flat,
        "cvars": cvar_flat,
    }


def cmd_run(args):
    scenario = load_scenario(args.scenario)
    label = args.label or scenario["id"]
    run_dir = os.path.join(args.runs_root, label)
    os.makedirs(args.runs_root, exist_ok=True)

    cfg_over = dict(scenario.get("cfg", {}))
    cvar_over = dict(scenario.get("cvars", {}))
    for kv in args.set_cfg or []:
        k, v = kv.split("=", 1)
        cfg_over[k] = json.loads(v)
    for kv in args.set_cvar or []:
        k, v = kv.split("=", 1)
        cvar_over[k] = json.loads(v)

    staged = stage(scenario, run_dir, cfg_over, cvar_over)

    env = dict(BASE_ENV)
    env.update({str(k): str(v) for k, v in scenario.get("env", {}).items()})
    for kv in args.extra_env or []:
        k, v = kv.split("=", 1)
        env[k] = v
    env["GDX_INPUT_SCRIPT"] = "/run/script.txt"

    docker_args = ["run", "--rm", "-v", "%s:/run" % run_dir]
    for k, v in sorted(env.items()):
        docker_args += ["-e", "%s=%s" % (k, v)]
    docker_args += ["-e", "GDX_REF_SCREEN=%s" % args.screen, IMAGE, "gdx-refrun"]

    timeout = int(scenario.get("timeout_seconds", 300))
    print("[refrun] scenario=%s  run dir=%s  timeout=%ds" % (scenario["id"], run_dir, timeout))
    t0 = time.time()
    log_path = os.path.join(run_dir, "run.log")
    with open(log_path, "wb") as log:
        proc = subprocess.Popen(["docker"] + docker_args, stdout=log, stderr=subprocess.STDOUT)
        try:
            rc = proc.wait(timeout=timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            rc, timed_out = -1, True
    elapsed = time.time() - t0

    shots = {}
    missing = []
    for lbl in scenario.get("shots", []):
        bmp = os.path.join(run_dir, "autotest", "%s.bmp" % lbl)
        if os.path.isfile(bmp) and os.path.getsize(bmp) > 0:
            shots[lbl] = {"path": bmp, "bytes": os.path.getsize(bmp), "sha256": sha256_file(bmp)}
        else:
            missing.append(lbl)

    manifest = {
        "scenario": scenario["id"],
        "title": scenario.get("title", ""),
        "label": label,
        "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "run_dir": run_dir,
        "image": IMAGE,
        "volume": VOLUME,
        "screen": args.screen,
        "exit_code": rc,
        "timed_out": timed_out,
        "wall_seconds": round(elapsed, 2),
        "env": env,
        "shots": shots,
        "missing_shots": missing,
        "notes": scenario.get("notes", []),
    }
    manifest.update(staged)
    with open(os.path.join(run_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)

    print("[refrun] exit=%s timed_out=%s wall=%.1fs shots=%d/%d" %
          (rc, timed_out, elapsed, len(shots), len(scenario.get("shots", []))))
    for lbl, info in sorted(shots.items()):
        print("[refrun]   %-16s %s  %s" % (lbl, info["sha256"][:16], info["path"]))
    for lbl in missing:
        print("[refrun]   %-16s MISSING" % lbl)
    print("[refrun] log: %s" % log_path)
    print("[refrun] manifest: %s" % os.path.join(run_dir, "manifest.json"))

    if args.emit_3ds_script:
        out = args.emit_3ds_script
        emit_3ds(scenario, out)
    return 1 if (missing or timed_out) else 0


# ── emit-3ds ─────────────────────────────────────────────────────────────────────────────────

# port/input_bridge.c's tick-mode vocabulary. Deliberately smaller than GDX_INPUT_SCRIPT's:
# whatever cannot be expressed here cannot be replayed identically on the console, and a
# silently-dropped input would make a frame comparison a lie.
_3DS_BUTTONS = {"START", "A", "B", "UP", "DOWN", "LEFT", "RIGHT"}
_3DS_STICK = {(0, 80): "STICK_UP", (0, -80): "STICK_DOWN", (-80, 0): "STICK_LEFT", (80, 0): "STICK_RIGHT"}
_3DS_MAX_EVENTS = 64


def emit_3ds(scenario, out_path, waitmode_ticks=None):
    """GDX_INPUT_SCRIPT -> sdmc:/gdx-autoinput.txt (tick mode), so ONE source file drives both.

    Both sides advance their counter once per controller poll (port/gdx_input_script.c's frame
    counter and port/input_bridge.c's s_tick), so a WAIT/PRESS frame count maps 1:1 onto ticks.
    WAITMODE has no console equivalent — it is a live poll on gGameMode — so it becomes a fixed
    tick budget, declared per scenario, and the emitter says so on stderr and in the file.
    """
    opts = scenario.get("emit_3ds", {})
    if waitmode_ticks is None:
        waitmode_ticks = int(opts.get("waitmode_ticks", 300))
    script = os.path.join(scenario["_dir"], scenario["script"])
    lines = []
    warnings = []
    tick = 0
    with open(script, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            s = raw.split("#", 1)[0].strip()
            if not s:
                continue
            tok = s.split()
            op = tok[0].upper()
            if op == "WAIT":
                tick += int(tok[1])
            elif op == "WAITMODE":
                lines.append("# WAITMODE %s -> %d ticks (no console equivalent; see refrun.py)" %
                             (tok[1], waitmode_ticks))
                warnings.append("line %d: WAITMODE %s became a fixed %d-tick wait" %
                                (lineno, tok[1], waitmode_ticks))
                tick += waitmode_ticks
            elif op in ("PRESS", "INPUT", "STICK"):
                if op == "PRESS":
                    buttons, sx, sy, frames = tok[1], 0, 0, int(tok[2])
                elif op == "STICK":
                    buttons, sx, sy, frames = "-", int(tok[1]), int(tok[2]), int(tok[3])
                else:
                    buttons, sx, sy, frames = tok[1], int(tok[2]), int(tok[3]), int(tok[4])
                names = [] if buttons == "-" else buttons.upper().split("+")
                for name in names:
                    if name not in _3DS_BUTTONS:
                        die("%s:%d: button %r has no gdx-autoinput.txt equivalent "
                            "(port/input_bridge.c knows only %s)" %
                            (script, lineno, name, " ".join(sorted(_3DS_BUTTONS))))
                    lines.append("%d %s %d" % (tick, name, frames))
                if (sx, sy) != (0, 0):
                    key = _3DS_STICK.get((sx, sy))
                    if key is None:
                        die("%s:%d: stick (%d,%d) is not one of the four cardinal +-80 values "
                            "port/input_bridge.c can express" % (script, lineno, sx, sy))
                    lines.append("%d %s %d" % (tick, key, frames))
                tick += frames
            elif op == "SHOT":
                lines.append("%d SHOT %s" % (tick, tok[1]))
            elif op == "LOG":
                pass  # console side has no LOG opcode; the log line is not an input
            elif op == "QUIT":
                warnings.append("line %d: QUIT dropped — the console run is ended by the user/lock"
                                % lineno)
            else:
                die("%s:%d: unknown opcode %r" % (script, lineno, op))

    events = [ln for ln in lines if not ln.startswith("#")]
    if len(events) > _3DS_MAX_EVENTS:
        die("%d events exceeds port/input_bridge.c's %d-event limit" % (len(events), _3DS_MAX_EVENTS))

    header = [
        "# AUTO-GENERATED by tools/dev/refrun.py emit-3ds from %s" % os.path.basename(script),
        "# Copy to the SD card root as gdx-autoinput.txt. Tick mode: one tick per",
        "# gdx_controller_poll(), the same counter GDX_INPUT_SCRIPT frames advance.",
    ] + ["# WARNING: %s" % w for w in warnings] + ["ticks"]
    text = "\n".join(header + lines) + "\n"
    if out_path == "-":
        sys.stdout.write(text)
    else:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(text)
        print("[refrun] wrote %s (%d events)" % (out_path, len(events)))
    for w in warnings:
        print("[refrun] WARNING: %s" % w, file=sys.stderr)
    return 0


def cmd_emit_3ds(args):
    scenario = load_scenario(args.scenario)
    out = args.out or os.path.join(args.runs_root, scenario["id"], "gdx-autoinput.txt")
    if out != "-":
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    return emit_3ds(scenario, out, args.waitmode_ticks)


def cmd_list(args):
    for name in sorted(os.listdir(SCENARIO_DIR)):
        if not name.endswith(".json"):
            continue
        sc = load_scenario(os.path.join(SCENARIO_DIR, name))
        print("%-24s %s" % (sc["id"], sc.get("title", "")))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--runs-root", default=RUNS_ROOT,
                    help="where run directories are staged (never inside the repo)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build", help="configure + build G-Diffuser in the container")
    b.add_argument("--fresh", action="store_true", help="wipe the build volume first")
    b.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    b.add_argument("--target", default="G-Diffuser")
    b.set_defaults(func=cmd_build)

    r = sub.add_parser("run", help="run one scenario headless and collect its BMPs")
    r.add_argument("scenario")
    r.add_argument("--label", help="run-directory name (default: the scenario id)")
    r.add_argument("--screen", default="640x480x24", help="Xvfb screen geometry")
    r.add_argument("--extra-env", action="append", metavar="K=V")
    r.add_argument("--set-cfg", action="append", metavar="K=JSON")
    r.add_argument("--set-cvar", action="append", metavar="K=JSON")
    r.add_argument("--emit-3ds-script", metavar="FILE",
                   help="also write the matching gdx-autoinput.txt here")
    r.set_defaults(func=cmd_run)

    e = sub.add_parser("emit-3ds", help="derive the console-side gdx-autoinput.txt")
    e.add_argument("scenario")
    e.add_argument("-o", "--out", help="output path, or - for stdout")
    e.add_argument("--waitmode-ticks", type=int,
                   help="tick budget a WAITMODE becomes (default: the scenario's)")
    e.set_defaults(func=cmd_emit_3ds)

    l = sub.add_parser("list", help="list known scenarios")
    l.set_defaults(func=cmd_list)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
