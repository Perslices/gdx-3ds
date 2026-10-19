"""Shared helpers: exit codes, paths, process wrappers, output formatting.

Python 3 stdlib only (docs/research/native/05-devcli-plan.md §6: no numpy, no Pillow,
no pip). CI asserts `python3 -S -c "import gdxdev"` still works.
"""

import json
import os
import shutil
import subprocess
import sys
import time

# ---------------------------------------------------------------- exit codes --
# Uniform across every subcommand (05-devcli-plan.md §4).
EXIT_OK = 0            # OK / PASS
EXIT_FAIL = 1          # assertion failed: [fatal], no heartbeat, frames differ
EXIT_USAGE = 2         # bad invocation
EXIT_PREFLIGHT = 3     # environment/preflight problem
EXIT_LOCK = 4          # lock timeout
EXIT_STAGING = 5       # staging failure (SD, ini, replay)
EXIT_NODATA = 6        # nothing parsed
EXIT_NODELEGATE = 7    # delegate tool (oracle/hwlink) absent


class GdxError(Exception):
    """Fatal, with an exit code. cli.py turns this into a message + status."""

    def __init__(self, message, code=EXIT_FAIL):
        super().__init__(message)
        self.code = code


# ---------------------------------------------------------------------- env --
AZAHAR_APP = "/Applications/Azahar.app"
AZAHAR_BIN = AZAHAR_APP + "/Contents/MacOS/azahar"
AZAHAR_ROOT = os.path.expanduser("~/Library/Application Support/Azahar")
AZAHAR_LOG = os.path.join(AZAHAR_ROOT, "log", "azahar_log.txt")
AZAHAR_CFG = os.path.join(AZAHAR_ROOT, "config", "qt-config.ini")
AZAHAR_SDMC = os.path.join(AZAHAR_ROOT, "sdmc")
GDX_SDMC_DIR = os.path.join(AZAHAR_SDMC, "3ds", "gdiffuser")
GDX_INI = os.path.join(GDX_SDMC_DIR, "gdiffuser.ini")
GDX_SD_LOG = os.path.join(GDX_SDMC_DIR, "log.txt")
AUTOINPUT_SD = os.path.join(AZAHAR_SDMC, "gdx-autoinput.txt")

LOCK_DIR = "/tmp/azahar.lock"
DEFAULT_RUN_ROOT = "/tmp/gdx-runs"

RETROARCH_APP = "/Applications/RetroArch.app"
RETROARCH_BIN = RETROARCH_APP + "/Contents/MacOS/RetroArch"


def repo_root():
    """Worktree root: walk up from this file until a .git entry appears."""
    here = os.path.dirname(os.path.abspath(__file__))
    d = here
    while d != "/":
        if os.path.exists(os.path.join(d, ".git")):
            return d
        d = os.path.dirname(d)
    # tools/dev/gdxdev/util.py -> repo
    return os.path.abspath(os.path.join(here, "..", "..", ".."))


def default_track():
    """Track name = worktree basename (PROTOCOL.md: ~/code/gdx-3ds/<name>)."""
    return os.path.basename(repo_root())


def tools_dev():
    return os.path.join(repo_root(), "tools", "dev")


# ------------------------------------------------------------------- output --
class Out:
    """One place for --quiet / --json so subcommands never touch sys.stdout directly."""

    def __init__(self, quiet=False, as_json=False):
        self.quiet = quiet
        self.json = as_json

    def say(self, msg=""):
        if not self.quiet and not self.json:
            print(msg, flush=True)

    def warn(self, msg):
        print("warning: " + msg, file=sys.stderr, flush=True)

    def err(self, msg):
        print("error: " + msg, file=sys.stderr, flush=True)

    def emit(self, obj):
        """Terminal structured result. Only prints under --json."""
        if self.json:
            print(json.dumps(obj, indent=2, sort_keys=True), flush=True)

    def table(self, headers, rows):
        if self.quiet or self.json:
            return
        if not rows:
            return
        cols = len(headers)
        widths = [len(str(h)) for h in headers]
        for r in rows:
            for i in range(cols):
                widths[i] = max(widths[i], len(str(r[i])))
        fmt = "  ".join("{:<%d}" % w for w in widths)
        print(fmt.format(*[str(h) for h in headers]), flush=True)
        print(fmt.format(*["-" * w for w in widths]), flush=True)
        for r in rows:
            print(fmt.format(*[str(c) for c in r]), flush=True)


# ------------------------------------------------------------------ process --
def run(cmd, cwd=None, env=None, check=True, capture=True, timeout=None):
    """Thin subprocess wrapper. Returns CompletedProcess with text streams."""
    p = subprocess.run(
        cmd,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
        text=True,
        timeout=timeout,
    )
    if check and p.returncode != 0:
        out = (p.stdout or "").strip()
        tail = "\n".join(out.splitlines()[-20:])
        raise GdxError("command failed (%d): %s\n%s" % (p.returncode, " ".join(cmd), tail))
    return p


def which(name):
    return shutil.which(name)


def pgrep(name):
    """PIDs of processes whose executable name matches exactly."""
    p = subprocess.run(["pgrep", "-x", name], stdout=subprocess.PIPE, text=True)
    if p.returncode != 0:
        return []
    return [int(x) for x in p.stdout.split() if x.strip().isdigit()]


def pid_alive(pid):
    try:
        os.kill(int(pid), 0)
    except (OSError, ValueError, TypeError):
        return False
    return True


def sha256_file(path):
    import hashlib

    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def iso_now():
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime())


def run_id(slug):
    safe = "".join(c if (c.isalnum() or c in "-_") else "-" for c in (slug or "run"))
    return time.strftime("%Y%m%d-%H%M%S-") + safe


def read_text(path, limit=None):
    """Read a possibly-binary-tainted log: the guest writes raw bytes on crash."""
    try:
        with open(path, "rb") as f:
            if limit:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                f.seek(max(0, size - limit))
            data = f.read()
    except OSError:
        return ""
    return data.decode("utf-8", errors="replace")
