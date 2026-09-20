"""The machine-wide Azahar lock (/tmp/azahar.lock).

PROTOCOL.md lines 13-14: ONE Azahar instance machine-wide; `mkdir` the lock, touch it
every <=25 s while held, `rm -rf` when done, and NEVER kill another agent's Azahar.

What the lock actually protects (05-devcli-plan.md §3 item 1): Azahar has no process-level
single-instance lock, but there is exactly one `~/Library/Application Support/Azahar`
user-data directory and no flag to move it — one virtual SD, one log. So the lock is a
data-directory lock, and it cannot simply be dropped.

Improvement over tools/ci-3ds.sh:402-425, which steals on mtime age alone and so can kill
a legitimate long run: we write pid+track into the lock and steal only when the owning
process is *dead* and the lock is stale.
"""

import json
import os
import shutil
import time

from . import util
from .util import GdxError

LOCK_DIR = util.LOCK_DIR
OWNER = "owner.json"
HEARTBEAT_S = 20            # <= 25 s, PROTOCOL.md line 14
STALE_OWNED_S = 90          # owner file present and pid dead: steal after this
STALE_LEGACY_S = 900        # no owner file (bash ci-3ds.sh holder): ci-3ds.sh's threshold
POLL_S = 5


def _owner_path():
    return os.path.join(LOCK_DIR, OWNER)


def read_owner():
    try:
        with open(_owner_path()) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def age_s():
    try:
        return time.time() - os.stat(LOCK_DIR).st_mtime
    except OSError:
        return None


def _write_owner(track, note):
    with open(_owner_path(), "w") as f:
        json.dump({"track": track, "pid": os.getpid(), "started": util.iso_now(),
                   "note": note, "host": os.uname().nodename}, f)
        f.write("\n")


def status():
    if not os.path.isdir(LOCK_DIR):
        return {"held": False}
    owner = read_owner()
    a = age_s()
    st = {"held": True, "age_s": round(a, 1) if a is not None else None,
          "owner": owner, "legacy": owner is None}
    if owner:
        st["pid_alive"] = util.pid_alive(owner.get("pid"))
        st["stealable"] = (not st["pid_alive"]) and (a or 0) > STALE_OWNED_S
    else:
        st["pid_alive"] = None
        st["stealable"] = (a or 0) > STALE_LEGACY_S
    return st


def touch():
    """Refresh the lock's mtime (and the owner file's) — the liveness signal."""
    now = time.time()
    try:
        os.utime(LOCK_DIR, (now, now))
        if os.path.exists(_owner_path()):
            os.utime(_owner_path(), (now, now))
    except OSError:
        pass


def acquire(track, wait_s=600, note="", out=None):
    """Blocking acquire. Raises GdxError(EXIT_LOCK) on timeout."""
    waited = 0
    announced = False
    while True:
        try:
            os.mkdir(LOCK_DIR)
            _write_owner(track, note)
            return True
        except FileExistsError:
            st = status()
            if st.get("stealable"):
                if out:
                    who = (st.get("owner") or {}).get("track", "legacy holder")
                    out.warn("stealing stale lock from %s (age %ss, pid dead)"
                             % (who, st.get("age_s")))
                release(force=True)
                continue
            if waited >= wait_s:
                who = (st.get("owner") or {}).get("track", "unknown/legacy")
                raise GdxError(
                    "azahar lock held by %r for %ss; gave up after %ss "
                    "(gdx-dev emu status)" % (who, st.get("age_s"), wait_s),
                    util.EXIT_LOCK)
            if out and not announced:
                who = (st.get("owner") or {}).get("track", "unknown/legacy")
                out.say("azahar lock held by %s (age %ss) — waiting up to %ss"
                        % (who, st.get("age_s"), wait_s))
                announced = True
            time.sleep(POLL_S)
            waited += POLL_S


def owned_by_me():
    o = read_owner()
    return bool(o) and o.get("pid") == os.getpid()


def release(force=False):
    """Drop the lock. Refuses to drop somebody else's unless forced."""
    if not os.path.isdir(LOCK_DIR):
        return False
    if not force and not owned_by_me():
        o = read_owner() or {}
        raise GdxError(
            "refusing to unlock: held by track %r pid %s (use --force if it is dead)"
            % (o.get("track"), o.get("pid")), util.EXIT_LOCK)
    shutil.rmtree(LOCK_DIR, ignore_errors=True)
    return True


class Held:
    """Context manager that holds the lock and heartbeats it from a daemon thread."""

    def __init__(self, track, wait_s=600, note="", out=None):
        self.track = track
        self.wait_s = wait_s
        self.note = note
        self.out = out
        self._stop = None
        self._thread = None

    def __enter__(self):
        import threading

        acquire(self.track, self.wait_s, self.note, self.out)
        self._stop = threading.Event()

        def beat():
            while not self._stop.wait(HEARTBEAT_S):
                touch()

        self._thread = threading.Thread(target=beat, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *exc):
        if self._stop:
            self._stop.set()
        try:
            release()
        except GdxError:
            pass
        return False
