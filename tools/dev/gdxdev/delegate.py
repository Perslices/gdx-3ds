"""`gdx-dev diff` and `gdx-dev hw` — thin delegations to the peer tracks.

Deliberately thin (05-devcli-plan.md §6, "Scope creep into the oracle/hwlink tracks"):
this file must never grow an image comparator or a 3dslink path. It resolves the peer
tool by path, execs it, and forwards its exit code. If the peer track has not merged yet,
it says so and exits 7 — an agent then knows to ask, not to reimplement.

  diff  -> tools/dev/framediff.py (images) / tools/dev/fhdiff.py ([fh] logs)  [oracle]
  hw    -> tools/dev/hw.py                                                    [hwlink]
"""

import os
import subprocess
import sys

from . import util
from .util import GdxError

PEERS = {
    "framediff.py": ("oracle", "03-oracle-plan.md §5 (framediff.py)"),
    "fhdiff.py": ("oracle", "03-oracle-plan.md §5 (fhdiff.py)"),
    "hw.py": ("hwlink", "02-hwlink-plan.md §4 (tools/dev/hw.py)"),
}


def peer_path(name):
    return os.path.join(util.tools_dev(), name)


def require(name):
    p = peer_path(name)
    if os.path.exists(p):
        return p
    track, ref = PEERS.get(name, ("?", "?"))
    raise GdxError(
        "%s tools not merged yet: %s is absent.\n"
        "  It is the `%s` track's deliverable (%s).\n"
        "  Merge native/%s into this branch, or ask that track's agent for it.\n"
        "  gdx-dev deliberately does not reimplement it (05-devcli-plan.md §6)."
        % (track, p, track, ref, track), util.EXIT_NODELEGATE)


def _exec(path, argv, out):
    cmd = [sys.executable, path] + list(argv)
    out.say("+ %s" % " ".join(cmd))
    p = subprocess.run(cmd)
    return p.returncode


def do_diff(args, out, ctx):
    argv = list(args.rest or [])
    if args.mode == "hashes":
        tool = require("fhdiff.py")
    else:
        tool = require("framediff.py")
        if args.mask:
            argv += ["--mask", args.mask]
        if args.sheet:
            argv += ["--sheet"]
    return _exec(tool, argv, out)


def do_hw(args, out, ctx):
    tool = require("hw.py")
    return _exec(tool, list(args.rest or []), out)
