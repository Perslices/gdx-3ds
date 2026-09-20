"""`gdx-dev sd` — the physical SD-card loop (RESUME-2026-08-22.md:11-12).

This touches the user's real hardware card, so it is **dry-run by default**: `sd stage`
prints the plan and changes nothing until `--apply`. Two further guards:
  - every destination is forced under `3ds/` on the card (no writes to the card root or
    to another homebrew app's directory);
  - the ini is MERGED, never rewritten, so the user's own keys (border_mode, deadzone,
    stereo, …) survive — PROTOCOL.md line 17.
"""

import os
import shutil
import subprocess

from . import inifile, util
from .util import GdxError

DEFAULT_VOLUME = "/Volumes/NO NAME"
CARD_APP_DIR = "3ds"
CARD_GDX_DIR = os.path.join(CARD_APP_DIR, "gdiffuser")


def _volume(args):
    vol = args.volume or DEFAULT_VOLUME
    if not os.path.isdir(vol):
        vols = [os.path.join("/Volumes", v) for v in sorted(os.listdir("/Volumes"))] \
            if os.path.isdir("/Volumes") else []
        raise GdxError("no SD card at %s (mounted: %s) — insert it or pass --volume"
                       % (vol, ", ".join(vols) or "none"), util.EXIT_PREFLIGHT)
    return vol


def _safe_dest(vol, rel):
    """Every write must land under <vol>/3ds/. Refuse anything that escapes."""
    dest = os.path.abspath(os.path.join(vol, rel))
    root = os.path.abspath(os.path.join(vol, CARD_APP_DIR))
    if not (dest == root or dest.startswith(root + os.sep)):
        raise GdxError("refusing to write outside %s: %s" % (root, dest), util.EXIT_STAGING)
    return dest


def do_stage(args, out, ctx):
    vol = _volume(args)
    src = os.path.abspath(args.source)
    apply_ = bool(args.apply)
    if not apply_:
        out.say("DRY RUN (pass --apply to actually write to the card)")

    plan = []
    if os.path.isfile(src):
        files = [(src, os.path.join(CARD_APP_DIR, os.path.basename(src)))]
    elif os.path.isdir(src):
        files = []
        for dirpath, _d, names in os.walk(src):
            for n in names:
                if n.startswith("."):
                    continue
                p = os.path.join(dirpath, n)
                rel = os.path.relpath(p, src)
                files.append((p, os.path.join(CARD_GDX_DIR, rel)))
    else:
        raise GdxError("no such file or directory: %s" % src, util.EXIT_USAGE)

    for s, rel in files:
        dest = _safe_dest(vol, rel)
        plan.append(("copy", s, dest, os.path.getsize(s)))

    settings = {}
    for spec in (args.ini or []):
        section, key, value = inifile.parse_spec(spec)
        settings[(section, key)] = value
    card_ini = _safe_dest(vol, os.path.join(CARD_GDX_DIR, "gdiffuser.ini"))
    if settings:
        _c, before, _a = inifile.merge(card_ini, settings, dry_run=True)
        for k, v in sorted(settings.items()):
            plan.append(("ini", "[%s] %s" % k, "%s -> %s" % (before.get(k), v), 0))

    card_log = _safe_dest(vol, os.path.join(CARD_GDX_DIR, "log.txt"))
    if args.wipe_log and os.path.exists(card_log):
        plan.append(("rm", card_log, "", os.path.getsize(card_log)))

    out.table(["action", "from", "to", "bytes"],
              [[a, _short(b), _short(c), d or ""] for a, b, c, d in plan])

    if not apply_:
        out.emit({"dry_run": True, "volume": vol, "plan": len(plan)})
        return util.EXIT_OK

    for a, b, c, _n in plan:
        if a == "copy":
            os.makedirs(os.path.dirname(c), exist_ok=True)
            shutil.copy2(b, c + ".part")
            os.replace(c + ".part", c)
        elif a == "rm":
            os.unlink(b)
    if settings:
        inifile.merge(card_ini, settings)
        out.say("merged %d ini key(s) into %s (other keys untouched)"
                % (len(settings), card_ini))
    subprocess.run(["sync"])
    out.say("wrote %d file(s) to %s and synced" % (sum(1 for p in plan if p[0] == "copy"), vol))

    if not args.no_eject:
        p = subprocess.run(["diskutil", "eject", vol], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        out.say((p.stdout or "").strip())
        if p.returncode != 0:
            raise GdxError("eject failed — do NOT pull the card yet", util.EXIT_STAGING)
    else:
        out.warn("card NOT ejected (--no-eject); run `diskutil eject %r` before pulling it"
                 % vol)
    out.emit({"dry_run": False, "volume": vol, "files": len(plan)})
    return util.EXIT_OK


def do_pull(args, out, ctx):
    vol = _volume(args)
    dest = os.path.abspath(args.dest)
    os.makedirs(dest, exist_ok=True)
    got = []
    srcdir = os.path.join(vol, CARD_GDX_DIR)
    for name in ("log.txt", "log-prev.txt"):
        p = os.path.join(srcdir, name)
        if os.path.exists(p):
            shutil.copy2(p, os.path.join(dest, name))
            got.append(name)
    shots = os.path.join(vol, "autotest")
    if os.path.isdir(shots):
        d = os.path.join(dest, "shots")
        os.makedirs(d, exist_ok=True)
        for n in sorted(os.listdir(shots)):
            shutil.copy2(os.path.join(shots, n), os.path.join(d, n))
            got.append("shots/" + n)
    if not got:
        raise GdxError("nothing to pull from %s" % srcdir, util.EXIT_NODATA)
    out.say("pulled %d artifact(s) to %s" % (len(got), dest))
    for g in got:
        out.say("  " + g)
    out.emit({"dest": dest, "files": got})
    return util.EXIT_OK


def _short(p):
    home = os.path.expanduser("~")
    s = str(p)
    return s.replace(home, "~")
