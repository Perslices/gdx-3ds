"""`gdx-dev emu shot` / `emu click` — host-side window capture and click injection.

Azahar has no --screenshot flag (05-devcli-plan.md §3 item 1: confirmed absent from
`azahar --help` on the installed 2126.0 build), so capture must be host-side. The two
helpers live in tools/dev/mac/ as vendored Swift because a CGWindowID cannot be obtained
from any shell command, and because the /tmp copies of exactly these two files have been
lost at least three times (RESUME-2026-09-05.md:25).

Both screens in one image needs Azahar's `layout_option=0` (single window, stacked
screens) — checked and reported here rather than silently capturing one screen.
"""

import os
import re
import subprocess

from . import util
from .util import GdxError


def mac_dir():
    return os.path.join(util.tools_dev(), "mac")


def window_ids(owner="azahar"):
    script = os.path.join(mac_dir(), "wid.swift")
    if not os.path.exists(script):
        raise GdxError("missing %s" % script, util.EXIT_PREFLIGHT)
    if not util.which("swift"):
        return []
    p = subprocess.run(["swift", script, owner], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True)
    out = []
    for line in p.stdout.splitlines():
        parts = line.split()
        if parts and parts[0].isdigit():
            out.append({"id": int(parts[0]), "pid": int(parts[1]), "size": parts[2],
                        "line": line.strip()})
    return out


def layout_option():
    m = re.search(r"^layout_option=(\d+)", util.read_text(util.AZAHAR_CFG), re.M)
    return int(m.group(1)) if m else None


def do_shot(args, out, ctx):
    dest = args.out or os.path.join(os.getcwd(), "%s-shot.png" % ctx["track"])
    dest = os.path.abspath(dest)
    os.makedirs(os.path.dirname(dest), exist_ok=True)

    lo = layout_option()
    if lo not in (0, None):
        out.warn("qt-config.ini layout_option=%s; only 0 puts BOTH screens in one window "
                 "(locked60-round2.md:35)" % lo)

    wins = window_ids(args.owner)
    if wins:
        wid = wins[0]["id"]
        cmd = ["screencapture", "-x", "-o", "-l", str(wid), dest]
        note = "window %d (%s)" % (wid, wins[0]["size"])
    else:
        if not util.which("swift"):
            out.warn("no swift interpreter; falling back to a full-screen capture")
        else:
            out.warn("no %r window on screen; falling back to a full-screen capture"
                     % args.owner)
        cmd = ["screencapture", "-x", dest]
        note = "full screen"
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if p.returncode != 0 or not os.path.exists(dest):
        raise GdxError("screencapture failed: %s" % (p.stdout or "").strip(),
                       util.EXIT_PREFLIGHT)
    size = os.path.getsize(dest)
    out.say("captured %s -> %s (%.0f KB)" % (note, dest, size / 1024.0))
    if size < 4096:
        out.warn("the capture is tiny (%d bytes) — screen recording permission missing?"
                 % size)
    out.emit({"path": dest, "bytes": size, "source": note, "layout_option": lo})
    return util.EXIT_OK


def do_click(args, out, ctx):
    script = os.path.join(mac_dir(), "click.swift")
    if not util.which("swift"):
        raise GdxError("no swift interpreter (install Xcode command line tools)",
                       util.EXIT_PREFLIGHT)
    m = re.match(r"^\s*(-?\d+)\s*,\s*(-?\d+)\s*$", args.xy)
    if not m:
        raise GdxError("expected X,Y (e.g. `emu click 640,520`), got %r" % args.xy,
                       util.EXIT_USAGE)
    cmd = ["swift", script, m.group(1), m.group(2)]
    if args.double:
        cmd.append("--double")
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out.say((p.stdout or "").strip())
    if p.returncode != 0:
        raise GdxError("click helper failed (Accessibility permission for your terminal?)",
                       util.EXIT_PREFLIGHT)
    return util.EXIT_OK
