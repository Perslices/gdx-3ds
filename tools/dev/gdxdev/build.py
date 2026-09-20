"""`gdx-dev build` — the cmake/ninja/ccache wrapper.

Owns three traps the plan calls out (05-devcli-plan.md §4):
  1. GDX3DS_BUILD_ID is a *configure-time* value (port/3ds/CMakeLists.txt:59-78). After a
     merge, a plain rebuild keeps the OLD id and every report cites the wrong build. We
     compare HEAD to the id baked into the generated build system and reconfigure.
  2. The stale-object trap (PROTOCOL.md line 11): after re-applying patches, touch the
     patched sources and assert each .o actually rebuilt. `--touch-patched`.
  3. Ninja + ccache (master plan §4.5) with configure/build seconds recorded.
"""

import os
import re
import shutil
import subprocess
import time

from . import util
from .util import GdxError

DEFAULT_BUILD_DIR = "build-3ds"
DEFAULT_TARGET = "G-Diffuser-3DS_3dsx"
ARTIFACT_REL = os.path.join("port", "3ds", "G-Diffuser-3DS.3dsx")
TOOLCHAIN = "/opt/devkitpro/cmake/3DS.cmake"

_BUILD_ID_RE = re.compile(r'GDX3DS_BUILD_ID=\\?"([^"\\]+)\\?"')


def git(args, root, default=""):
    p = subprocess.run(["git"] + args, cwd=root, stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, text=True)
    if p.returncode != 0:
        return default
    return p.stdout.strip()


def head_build_id(root):
    branch = git(["rev-parse", "--abbrev-ref", "HEAD"], root, "unknown")
    h = git(["rev-parse", "--short=8", "HEAD"], root, "nohash")
    return "%s@%s" % (branch, h)


def is_dirty(root):
    """Uncommitted changes in the SUPERPROJECT only.

    The submodules are permanently "modified" on this fork — tools/ci-3ds.sh applies the
    48-patch stack into them by design — so counting them would mark every single build
    dirty and the flag would carry no information.
    """
    return bool(git(["status", "--porcelain", "--ignore-submodules=all"], root))


def configured_build_id(build_dir):
    """The id baked into the generated build system (ninja file or flags.make)."""
    candidates = [os.path.join(build_dir, "build.ninja")]
    fm = os.path.join(build_dir, "port", "3ds", "CMakeFiles",
                      "G-Diffuser-3DS.dir", "flags.make")
    candidates.append(fm)
    for c in candidates:
        if not os.path.exists(c):
            continue
        m = _BUILD_ID_RE.search(util.read_text(c))
        if m:
            return m.group(1)
    return None


def cache_generator(build_dir):
    cache = os.path.join(build_dir, "CMakeCache.txt")
    if not os.path.exists(cache):
        return None
    m = re.search(r"^CMAKE_GENERATOR:INTERNAL=(.*)$", util.read_text(cache), re.M)
    return m.group(1).strip() if m else None


def configure(root, build_dir, generator, out, extra=None):
    if not os.path.exists(TOOLCHAIN):
        raise GdxError("devkitPro toolchain missing: %s (export DEVKITPRO)" % TOOLCHAIN,
                       util.EXIT_PREFLIGHT)
    cmd = ["cmake", "-S", root, "-B", build_dir,
           "-DCMAKE_TOOLCHAIN_FILE=" + TOOLCHAIN,
           "-DGDX_PLATFORM_3DS=ON", "-DCMAKE_BUILD_TYPE=Release"]
    if generator:
        cmd += ["-G", generator]
    if util.which("ccache"):
        cmd += ["-DCMAKE_C_COMPILER_LAUNCHER=ccache",
                "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"]
    cmd += list(extra or [])
    env = dict(os.environ)
    env.setdefault("DEVKITPRO", "/opt/devkitpro")
    t0 = time.time()
    p = util.run(cmd, cwd=root, env=env, check=False)
    dt = time.time() - t0
    if p.returncode != 0:
        out.say(p.stdout or "")
        raise GdxError("cmake configure failed", util.EXIT_PREFLIGHT)
    return dt


def patched_sources(root):
    """Sources named by the patch stack (port/3ds/patches/README.md order)."""
    pdir = os.path.join(root, "port", "3ds", "patches")
    if not os.path.isdir(pdir):
        return []
    files = set()
    for name in sorted(os.listdir(pdir)):
        if not name.endswith(".patch"):
            continue
        for line in util.read_text(os.path.join(pdir, name)).splitlines():
            if line.startswith("+++ b/"):
                files.add(line[6:].strip())
    return sorted(files)


def do_build(args, out, ctx):
    root = ctx["root"]
    build_dir = os.path.join(root, args.build_dir)
    target = args.target or DEFAULT_TARGET

    if args.clean and os.path.isdir(build_dir):
        out.say("removing %s" % build_dir)
        shutil.rmtree(build_dir)

    head_id = head_build_id(root)
    gen = cache_generator(build_dir)
    reconfigure = args.reconfigure or not os.path.exists(os.path.join(build_dir, "CMakeCache.txt"))

    if not reconfigure:
        cfg_id = configured_build_id(build_dir)
        if cfg_id and cfg_id != head_id:
            # The documented post-merge trap: the HUD/report would name the old build.
            out.warn("configured build id %s != HEAD %s — reconfiguring "
                     "(GDX3DS_BUILD_ID is configure-time, port/3ds/CMakeLists.txt:59-78)"
                     % (cfg_id, head_id))
            reconfigure = True

    configure_s = 0.0
    if reconfigure:
        want_gen = gen
        if args.reconfigure or gen is None:
            want_gen = "Ninja" if util.which("ninja") else gen
        if args.reconfigure and os.path.isdir(build_dir) and want_gen != gen:
            # Changing generator in place is a hard cmake error; drop the cache only.
            for stale in ("CMakeCache.txt", "CMakeFiles"):
                p = os.path.join(build_dir, stale)
                if os.path.isdir(p):
                    shutil.rmtree(p, ignore_errors=True)
                elif os.path.exists(p):
                    os.unlink(p)
        out.say("configuring (%s)%s" % (want_gen or "default generator",
                                        ", ccache" if util.which("ccache") else ""))
        configure_s = configure(root, build_dir, want_gen, out)

    touched = []
    if args.touch_patched:
        touched = patched_sources(root)
        now = time.time()
        for rel in touched:
            p = os.path.join(root, rel)
            if os.path.exists(p):
                os.utime(p, (now, now))
        out.say("touched %d patched source(s) for the stale-object check" % len(touched))

    obj_before = _object_mtimes(build_dir)
    cmd = ["cmake", "--build", build_dir, "--target", target, "--parallel", str(args.jobs)]
    env = dict(os.environ)
    env.setdefault("DEVKITPRO", "/opt/devkitpro")
    t0 = time.time()
    p = util.run(cmd, cwd=root, env=env, check=False)
    build_s = time.time() - t0
    if p.returncode != 0:
        out.say("\n".join((p.stdout or "").splitlines()[-40:]))
        raise GdxError("build failed (target %s)" % target)

    if args.touch_patched and touched:
        obj_after = _object_mtimes(build_dir)
        stale = [o for o, m in obj_before.items()
                 if o in obj_after and obj_after[o] <= m]
        if stale and len(stale) == len(obj_before):
            raise GdxError("stale-object trap: no .o rebuilt after touching %d patched "
                           "sources (PROTOCOL.md line 11)" % len(touched))

    artifact = os.path.join(build_dir, ARTIFACT_REL)
    if not os.path.exists(artifact):
        raise GdxError("build reported success but %s is missing" % artifact)
    sha = util.sha256_file(artifact)

    info = {
        "id": head_id,
        "dirty": is_dirty(root),
        "artifact": artifact,
        "sha256": sha,
        "generator": cache_generator(build_dir),
        "configure_s": round(configure_s, 2),
        "build_s": round(build_s, 2),
    }
    out.say("")
    out.table(["field", "value"], [
        ["build id", info["id"] + (" (dirty tree)" if info["dirty"] else "")],
        ["generator", info["generator"]],
        ["configure", "%.1fs" % info["configure_s"]],
        ["build", "%.1fs" % info["build_s"]],
        ["artifact", info["artifact"]],
        ["size", "%.1f MB" % (os.path.getsize(artifact) / 1e6)],
        ["sha256", info["sha256"]],
    ])
    out.emit(info)
    ctx["last_build"] = info
    return util.EXIT_OK


def _object_mtimes(build_dir):
    out = {}
    for dirpath, _dirs, files in os.walk(build_dir):
        for f in files:
            if f.endswith(".o"):
                p = os.path.join(dirpath, f)
                try:
                    out[p] = os.path.getmtime(p)
                except OSError:
                    pass
    return out


def build_info_for_manifest(root, build_dir, artifact):
    """Cheap build block for a run that did not just call `build`."""
    bd = os.path.join(root, build_dir)
    info = {
        "id": configured_build_id(bd) or head_build_id(root),
        "dirty": is_dirty(root),
        "artifact": artifact,
        "sha256": util.sha256_file(artifact) if os.path.exists(artifact) else None,
        "generator": cache_generator(bd),
        "configure_s": None,
        "build_s": None,
    }
    return info
