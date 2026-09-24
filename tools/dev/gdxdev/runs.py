"""Run directories and the versioned manifest (05-devcli-plan.md §4).

Layout: $run_root/<track>/<run_id>/
    manifest.json  log.txt  emu.log  console.txt  VERDICT  shots/  capture/  prof/

/tmp/<track>-art is a symlink to the newest run dir, which is what satisfies
PROTOCOL.md line 20 ("snapshot your artifacts every 25 s") with no bookkeeping.
"""

import json
import os
import shutil

from . import util
from .util import GdxError

SCHEMA = 1

# Every key is required in a written manifest; `null` where inapplicable. Keeping the
# template here (rather than scattered through emu.py) is what makes the schema check
# in tools/dev/tests/ meaningful.
_TEMPLATE = {
    "schema": SCHEMA,
    "run_id": None,
    "track": None,
    "lane": None,            # azahar | retro | hw
    "started": None,
    "ended": None,
    "wall_s": None,
    "build": None,           # {id,dirty,artifact,sha256,generator,configure_s,build_s}
    # WHICH TREE produced this run (NEXT.md B46, PROTOCOL.md "a parity cell is a claim
    # about a tree"). {branch,head,base,base_head,branch_point,behind,ahead,dirty,current}.
    # Required so that no parity number can be quoted out of a manifest without the
    # branch point it was measured at sitting beside it.
    "tree": None,
    "ini": None,             # merged [debug]/[native] keys this run set
    "replay": None,          # {id,sha256,kind,events}
    "gputrace": None,        # promoted out of `ini`: PROTOCOL.md requires every perf
                             # claim to state it, so no report can omit it
    "seconds": None,
    "platform": None,        # {host,azahar,retroarch,model}
    # {verdict,fatals,max_frame,distinct_frames,errors{},replay_complete,
    #  shots_expected,shots_seen,missing_shots}
    "result": None,
    "artifacts": None,       # {log,emu_log,shots[]}
    # Optional: the retro lane's per-renderer boot attempts, so an intermittent core
    # crash is visible in the manifest rather than only in the console.
    "attempts": None,
}

# `attempts` is additive: manifests written before it existed are still schema-1 valid.
OPTIONAL_KEYS = {"attempts"}
REQUIRED_KEYS = sorted(k for k in _TEMPLATE if k not in OPTIONAL_KEYS)


def new_manifest(**kw):
    m = dict(_TEMPLATE)
    m.update(kw)
    return m


# TIMEOUT is a FAIL for exit-code purposes (only PASS exits 0) but a different FACT: the
# run was cut short by `--seconds`, not by a crash. NEXT.md B45: the two were reported
# identically, and "RESULT FAIL" on a truncated corpus replay reads like a crash.
VERDICTS = ("PASS", "FAIL", "TIMEOUT", "UNKNOWN")

# Every field of the `tree` block, so a half-filled one is refused as loudly as a missing
# one. `behind`/`ahead` may be null only when the base branch is not resolvable at all.
TREE_KEYS = ("branch", "head", "base", "base_head", "branch_point", "behind", "ahead",
             "dirty", "current")


def _tree_problems(tree):
    """PROTOCOL.md: a parity cell is a claim about a tree, so every manifest names one."""
    if tree is None:
        return ["tree is missing: every run must record its branch point "
                "(PROTOCOL.md, a parity cell is a claim about a tree)"]
    if not isinstance(tree, dict):
        return ["tree must be an object"]
    missing = [k for k in TREE_KEYS if k not in tree]
    out = ["tree.%s is missing" % k for k in missing]
    if not tree.get("branch_point"):
        out.append("tree.branch_point must be non-empty: without it a parity number "
                   "cannot be attributed to a tree")
    return out


def validate(manifest):
    """Return a list of problems; empty means the manifest is schema-valid."""
    problems = []
    if not isinstance(manifest, dict):
        return ["manifest is not an object"]
    if manifest.get("schema") != SCHEMA:
        problems.append("schema is %r, expected %d" % (manifest.get("schema"), SCHEMA))
    for k in REQUIRED_KEYS:
        if k not in manifest:
            problems.append("missing required key: %s" % k)
    for k in ("run_id", "track", "lane"):
        if k in manifest and not manifest[k]:
            problems.append("%s must be non-empty" % k)
    problems += _tree_problems(manifest.get("tree"))
    res = manifest.get("result")
    if isinstance(res, dict):
        if res.get("verdict") not in VERDICTS + (None,):
            problems.append("result.verdict is %r" % res.get("verdict"))
    if manifest.get("gputrace") not in (True, False, None):
        problems.append("gputrace must be a bool (PROTOCOL.md: perf claims state it)")
    return problems


class RunDir:
    """One run's directory. Created up-front so a crashed agent still leaves evidence."""

    def __init__(self, path):
        self.path = path

    @property
    def run_id(self):
        return os.path.basename(self.path)

    @property
    def manifest_path(self):
        return os.path.join(self.path, "manifest.json")

    def sub(self, *parts):
        return os.path.join(self.path, *parts)

    def write_manifest(self, manifest):
        problems = validate(manifest)
        if problems:
            raise GdxError("manifest is not schema-%d valid: %s" % (SCHEMA, "; ".join(problems)))
        tmp = self.manifest_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(manifest, f, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(tmp, self.manifest_path)

    def read_manifest(self):
        if not os.path.exists(self.manifest_path):
            raise GdxError("no manifest.json in %s" % self.path, util.EXIT_NODATA)
        with open(self.manifest_path) as f:
            return json.load(f)

    def write_verdict(self, verdict, detail=""):
        with open(self.sub("VERDICT"), "w") as f:
            f.write("%s %s\n" % (verdict, detail))

    def snapshot(self, src, dest_name):
        """Copy to a temp name then rename: never overwrite in place.

        The emulator writes log.txt while we read it; a half-copied file that replaced a
        good one loses the evidence (05-devcli-plan.md §6).
        """
        if not os.path.exists(src):
            return False
        dst = self.sub(dest_name)
        tmp = dst + ".part"
        try:
            shutil.copy2(src, tmp)
            os.replace(tmp, dst)
        except OSError:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            return False
        return True


def track_root(run_root, track):
    return os.path.join(run_root, track)


def create(run_root, track, slug):
    d = RunDir(os.path.join(track_root(run_root, track), util.run_id(slug)))
    for sub in ("", "shots", "capture", "prof"):
        os.makedirs(d.sub(sub) if sub else d.path, exist_ok=True)
    point_art_symlink(track, d.path)
    return d


def point_art_symlink(track, target):
    """/tmp/<track>-art -> newest run dir. Best-effort; never fails a run."""
    link = "/tmp/%s-art" % track
    try:
        if os.path.islink(link) or os.path.exists(link):
            if os.path.islink(link):
                os.unlink(link)
            else:
                return link  # a real directory from an older agent: leave it alone
        os.symlink(target, link)
    except OSError:
        return None
    return link


def listing(run_root, track):
    root = track_root(run_root, track)
    if not os.path.isdir(root):
        return []
    out = []
    for name in sorted(os.listdir(root)):
        p = os.path.join(root, name)
        if os.path.isdir(p):
            out.append(RunDir(p))
    return out


def resolve(run_root, track, ident):
    """Accept a run id, a path to a run dir, or a prefix of a run id."""
    if os.path.isdir(ident) and os.path.exists(os.path.join(ident, "manifest.json")):
        return RunDir(os.path.abspath(ident))
    direct = os.path.join(track_root(run_root, track), ident)
    if os.path.isdir(direct):
        return RunDir(direct)
    matches = [r for r in listing(run_root, track) if r.run_id.startswith(ident)]
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        raise GdxError(
            "run id %r is ambiguous (%s)" % (ident, ", ".join(m.run_id for m in matches)),
            util.EXIT_USAGE,
        )
    raise GdxError("no such run: %s (try `gdx-dev runs list`)" % ident, util.EXIT_USAGE)
