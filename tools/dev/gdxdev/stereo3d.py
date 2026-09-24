"""`--3d`: make the HOST emulator render stereoscopic 3D, so the guest's right-eye pass runs.

Why this module exists
----------------------
`[stereo] enabled=1` on the guest is NOT enough. `Gdx3dsStereo::FrameBegin` gates the
right-eye pass on `osGet3DSliderState() > 0` (port/3ds/gfx/gdx3ds_stereo.cpp), and the
emulated slider lives in the 3DS shared page, which Azahar drives from its OWN settings:
the slider reads 0 whenever `render_3d == Off` or `factor_3d == 0`. Every emulator parity
run before this lane therefore ran `[stereo] enabled=1` with the slider at 0 and no
right-eye pass ever executed — that is why no `anchor=` / `eyeR=` receipt had ever been
seen (docs/research/native/integration-verify-2026-09-17.md, "Stereo").

So `--3d` sets the HOST knob, per lane:

* Azahar Qt lane — `qt-config.ini` `[Layout] render_3d` + `factor_3d`.
* RetroArch lane — the `citra_render_3d` / `citra_factor_3d` CORE OPTIONS.

The RetroArch catch, measured 2026-09-23
----------------------------------------
The core pinned by `retro.py` (the azahar 2126.0 release, sha256 c335492…) exposes 30
`citra_*` options and NOT ONE of them is stereo: `src/citra_libretro/core_settings.cpp`
@2126.0 parses cpu/system/audio/graphics/layout/storage/input and never touches
`render_3d`. The anaglyph and interlaced shaders are compiled into that dylib — only the
option plumbing is missing — and the core reads no ini, so there is no frontend-side
workaround for it. The libretro NIGHTLY (buildbot, 2026-09-22 build) *does* expose
`citra_render_3d` and `citra_factor_3d`. `--3d` on the retro lane therefore runs against a
separately installed nightly core (`retro.core_3d_path()`), which leaves the shared
release core other agents are mid-run against completely untouched.

Hashes from the two cores are different emulators, so a `--3d` parity matrix compares
`--3d` runs with each other — the same-platform rule (PROTOCOL.md line 20) applied one
level down.

The qt-config trap
------------------
`QtConfig::ReadBasicSetting` (src/citra_qt/configuration/config.cpp @2126.0) reads the
sibling key `<name>\\default` FIRST and, when it is true, throws the stored value away and
uses the compiled default. So writing `render_3d=1` on its own does nothing at all —
`render_3d\\default=false` has to be written with it. `inifile.merge` cannot do this: its
`_KEY_RE` does not admit a backslash in a key name (and must not, for the guest ini), so
this module carries its own minimal line-patcher for that one file.
"""

import os

from . import util
from .util import GdxError

# mode name -> (Azahar Qt `[Layout] render_3d` integer, libretro `citra_render_3d` value).
#
# The Qt integers are Settings::StereoRenderOption (src/common/settings.h:86 @2126.0):
# Off=0 SideBySide=1 SideBySideFull=2 Anaglyph=3 Interlaced=4 ReverseInterlaced=5
# CardboardVR=6. Azahar inserted SideBySideFull at 2, so this is NOT the historical Citra
# ordering — do not carry an enum over from a Citra-era note. The libretro value strings
# are the nightly core's option-value KEYS (not its UI labels).
MODES = {
    "off":               (0, "off"),
    "side-by-side":      (1, "side-by-side"),
    "side-by-side-full": (2, "side-by-side-full"),
    "anaglyph":          (3, "anaglyph"),
    "interlaced":        (4, "interlaced"),
    "reverse-interlaced": (5, "reverse-interlaced"),
    "cardboard":         (6, "cardboard"),
}

# Side by side is the default because it is the only mode where a single captured host
# frame VISIBLY contains both eyes — which is the evidence the stereo gates ask for.
DEFAULT_MODE = "side-by-side"
DEFAULT_DEPTH = 100          # factor_3d, percent; 0 would leave the slider at 0.

# The guest-side ini `--3d` implies. Applied BEFORE the explicit `--ini` flags so an
# explicit one always wins.
#   [stereo] enabled   — without it the backend never builds the right target at all.
#   [debug]  fh_eye    — both eyes hashed, so `fhdiff --eye both` has an R stream. The
#                        guest already supported `both` (gdx3ds_oracle.c:280); nothing had
#                        ever asked for it.
#   [debug] c3d_filelog — mirror the per-window [c3d] line to the SD filelog. That line is
#                        where `eyeR=<n> stereo=<0|1>` and `anchor=<draws>/<dmin>/<dmax>`
#                        live, and it otherwise goes ONLY to svcOutputDebugString, which is
#                        Azahar Qt's log and nothing else — invisible on the RetroArch lane,
#                        on hardware and over hwlink. Default 0 in the guest.
GUEST_INI = {
    ("stereo", "enabled"): "1",
    ("debug", "fh_eye"): "both",
    ("debug", "c3d_filelog"): "1",
}


def mode_or_die(name):
    key = str(name).strip().lower()
    if key not in MODES:
        raise GdxError("unknown --3d mode %r (choose from: %s)"
                       % (name, ", ".join(sorted(MODES))), util.EXIT_USAGE)
    return key


def depth_or_die(value):
    try:
        depth = int(value)
    except (TypeError, ValueError):
        raise GdxError("--depth-3d must be an integer percentage, got %r" % (value,),
                       util.EXIT_USAGE)
    if not 1 <= depth <= 100:
        # 0 is rejected rather than silently accepted: it is exactly the state this whole
        # lane exists to escape (slider 0 -> no right-eye pass).
        raise GdxError("--depth-3d must be 1..100 (%d would leave the 3D slider at 0, "
                       "which is what `--3d` is for avoiding)" % depth, util.EXIT_USAGE)
    return depth


def guest_settings():
    return dict(GUEST_INI)


# ---------------------------------------------------------------- Azahar Qt lane --
# Keys are written as `key=value` with no spaces, which is what QSettings itself emits.
def qt_keys(mode, depth):
    render, _ = MODES[mode]
    return [
        ("render_3d", str(render)),
        ("render_3d\\default", "true" if render == 0 else "false"),
        ("factor_3d", str(depth)),
        ("factor_3d\\default", "false"),
    ]


def _patch_qt_config(path, pairs, remove=()):
    """Set `pairs` (and delete `remove`) inside [Layout] of a Qt ini.

    -> {key: previous literal value, or None when the key was absent}.

    Line-based on purpose: qt-config.ini has ~700 keys with backslash-separated groups and
    percent-escaped names (`Shortcuts\\Main%20Window\\...`), and a round-trip through any
    ini parser would rewrite the lot. This touches only the named keys, in place.
    """
    if not os.path.isfile(path):
        raise GdxError("no qt-config.ini at %s" % path, util.EXIT_PREFLIGHT)
    lines = util.read_text(path).splitlines()
    want = dict(pairs)
    drop = set(remove)
    before = dict.fromkeys(list(want) + list(drop))
    out = []
    section = None
    layout_last = None
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
        elif section == "Layout" and "=" in line:
            key = line.split("=", 1)[0].strip()
            if key in want or key in drop:
                before[key] = line.split("=", 1)[1].strip()
                if key in drop:
                    continue
                out.append("%s=%s" % (key, want[key]))
                layout_last = len(out) - 1
                continue
        out.append(line)
        if section == "Layout" and stripped:
            layout_last = len(out) - 1
    missing = sorted(k for k in want if before[k] is None)
    if missing:
        if layout_last is None:
            raise GdxError("qt-config.ini has no [Layout] section (%s)" % path,
                           util.EXIT_PREFLIGHT)
        for k in missing:
            out.insert(layout_last + 1, "%s=%s" % (k, want[k]))
            layout_last += 1
    tmp = path + ".gdxtmp"
    with open(tmp, "w") as f:
        f.write("\n".join(out).rstrip("\n") + "\n")
    os.replace(tmp, path)
    return before


class QtStereo:
    """Context manager: qt-config.ini in 3D for the run, restored exactly afterwards.

    Azahar rewrites the whole file when it exits, so this must wrap a window in which no
    Azahar is running at either end — i.e. inside `emu run`'s lock, after the stale-process
    kill and after the emulator is confirmed down. It restores the PREVIOUS literal values
    (including a key that was absent, which is removed again), because qt-config.ini is the
    user's own Azahar preference file and a run must not redecorate it.
    """

    def __init__(self, mode, depth, out, path=None):
        self.mode = mode
        self.depth = depth
        self.out = out
        self.path = path or util.AZAHAR_CFG
        self.before = None

    def __enter__(self):
        pairs = qt_keys(self.mode, self.depth)
        self.before = _patch_qt_config(self.path, pairs)
        self.out.say("3D on (Azahar Qt): [Layout] %s"
                     % ", ".join("%s=%s" % (k, v) for k, v in pairs))
        for k, v in pairs:
            if self.before.get(k) != v:
                self.out.say("  %s: %s -> %s" % (k, self.before.get(k), v))
        return self

    def __exit__(self, *exc):
        if self.before is None:
            return False
        restore = sorted((k, v) for k, v in self.before.items() if v is not None)
        # A key this run INVENTED goes away again; leaving it behind would pin a
        # preference the user never set.
        drop = sorted(k for k, v in self.before.items() if v is None)
        _patch_qt_config(self.path, restore, remove=drop)
        self.out.say("3D restored in qt-config.ini (%s%s)"
                     % (", ".join("%s=%s" % (k, v) for k, v in restore),
                        ("; removed " + ", ".join(drop)) if drop else ""))
        return False


def qt_receipt(mode, depth):
    render, _ = MODES[mode]
    return {"lane": "azahar", "mode": mode, "render_3d": render, "factor_3d": depth}


# -------------------------------------------------------------- RetroArch lane --
CORE_OPTION_KEY = "citra_render_3d"      # absent from the 2126.0 release core; nightly-only
CORE_DEPTH_KEY = "citra_factor_3d"


def core_options(mode, depth):
    _, value = MODES[mode]
    return [(CORE_OPTION_KEY, value), (CORE_DEPTH_KEY, str(depth))]


def core_exposes_3d(core_path):
    """True iff this core build declares `citra_render_3d`.

    A byte scan rather than `strings`: the option key is a plain NUL-terminated C string in
    the option-definition array, so its presence in the file is exactly the question.
    """
    if not core_path or not os.path.isfile(core_path):
        return False
    needle = CORE_OPTION_KEY.encode()
    with open(core_path, "rb") as f:
        tail = b""
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                return False
            if needle in tail + chunk:
                return True
            tail = chunk[-len(needle):]


def retro_receipt(mode, depth, core_path):
    return {"lane": "retro", "mode": mode, "core": core_path,
            "core_options": dict(core_options(mode, depth))}
