"""`gdx-dev hw-run` — unattended hardware config matrix over the WiFi dev link (A9).

Replaces the throwaway `artifacts/hwprof.sh` / `artifacts/hwfps3.sh` pair that produced
the 2026-09-20 hardware A/B (integration-verify-2026-09-17.md §"Hardware profile over the
WiFi link"). What those scripts got wrong, and what this fixes:

1. **Log slicing by byte offset.** They did `OFF=$(wc -c < live.log)` before a config and
   `tail -c +$OFF` after. Any other command appending to that log — a second `hw.py`, the
   user's own terminal — shifted the offset, and a slow flush put the PREVIOUS config's
   tail inside the next slice (visible in `artifacts/hwfps3-r1-all1-interp1.log`, whose
   first two lines are the prior run's `race03`/`race04` SHOTs). Here every slice is
   delimited by a **marker the guest itself emits**: `INI debug hwrun_mark <token>` makes
   the console log `[net] cmd INI [debug] hwrun_mark=<token> rc=0`
   (port/3ds/net/gdx3ds_net.c:476), so the boundary travels in the same stream as the
   data and cannot drift. The key is deleted straight afterwards so no `INI SAVE` ever
   persists it. When we also own port 17491 the sink is rotated per slice as well, and
   the two boundaries are cross-checked.
2. **Fixed `sleep 150`.** Here the runner waits for the replay's own `[rec] end` receipt
   with `--seconds` as a ceiling, so a fast config does not idle and a stalled one is
   reported as incomplete instead of silently truncated.
3. **No completion check.** A guest that dies at frame 433 of a 6001-frame replay still
   logs an advancing heartbeat and zero `[fatal]` lines. Completion here is
   `[rec] end` + every scheduled SHOT, the same rule `gdx-dev emu run` uses
   (gdxdev/replay.py:321).
4. **No manifest, no summary.** Every slice now gets a schema-1 manifest (build sha256,
   ini, replay, gputrace, uptimes) and the run prints the `[prof]` bucket tables, the
   in-race fps percentiles, the error census and an A/B delta against the first config.

Stdlib only. The control protocol is NOT reimplemented: `tools/dev/hw.py` (the hwlink
track's deliverable) is located through `delegate.require` and imported, so there is one
implementation of `Control`, `LogServer` and the IP lookup.
"""

import os
import re
import subprocess
import socket
import time

from . import build as buildmod
from . import delegate, inifile, prof, replay, runs, util
from .util import GdxError

# --------------------------------------------------------------------------- markers --
# A real ini key in a real section, because gdx3ds_config_set_string() stores anything
# (port/3ds/os/gdx3ds_config.c:159) and the receipt is logged whatever the return code is.
# Nothing in the guest reads `[debug] hwrun_mark`; it is deleted after every use so a
# later `INI SAVE` cannot write it into the user's gdiffuser.ini.
MARK_SECTION = "debug"
MARK_KEY = "hwrun_mark"


def mark_needle(token):
    """The substring of the guest's receipt that identifies one marker uniquely."""
    return "%s=%s" % (MARK_KEY, token)


def mark_tokens(run_id, cfg_index, rep):
    """(begin, end) tokens for one slice. Short: netLog's buffer is msg[224]."""
    base = "%s-c%02dr%02d" % (re.sub(r"[^A-Za-z0-9]", "", run_id)[-12:], cfg_index, rep)
    return base + "-b", base + "-e"


def slice_marked(text, begin_token, end_token):
    """Text strictly between the two marker receipts. -> (text, status).

    status: 'ok' both markers seen · 'open' begin seen, no end (take to EOF) ·
            'missing' the begin marker never arrived.

    Line-based and token-based on purpose: it is immune to another writer appending to
    the same file, which is exactly what the byte-offset version was not.
    """
    beg, end = mark_needle(begin_token), mark_needle(end_token)
    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if beg in line:
            start = i + 1
            break
    if start is None:
        return "", "missing"
    for j in range(start, len(lines)):
        if end in lines[j]:
            return "\n".join(lines[start:j]), "ok"
    return "\n".join(lines[start:]), "open"


# ------------------------------------------------------------------- config matrix --
def parse_config(spec):
    """`'native.all=0,native.interp=1'` -> [(section, key, value_or_None)].

    An empty value means DELETE the key (`hw.py ini --delete`), which is not the same as
    setting it empty: an empty entry is still an entry, the getters return "" instead of
    the DEFAULT, and `INI SAVE` writes it back out — that is how a replay stayed armed
    across reloads (tools/dev/hw.py:cmd_ini). Values may not contain a comma.
    """
    items = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        section, key, value = inifile.parse_spec(part)
        items.append((section, key, value if value != "" else None))
    if not items:
        raise GdxError("empty --config %r" % spec, util.EXIT_USAGE)
    return items


def config_label(items):
    """Short header for the summary tables: `all=0,interp=1`."""
    return ",".join("%s=%s" % (k, "DEL" if v is None else v) for _, k, v in items)


# ------------------------------------------------------------- hardware log receipts --
# The injector's own SHOT receipt (port/gdx_input_rec.c:315). This is the one that exists
# on hardware in every configuration: `[fh] tag=` needs fh=1 (off for perf runs) and the
# 3DS bridge writes "SHOT dumped", not the "SHOT captured" wording gdx3ds_oracle.c uses.
_REC_SHOT_RE = re.compile(r"\[rec\] SHOT '([^']+)' at tick (\d+)")
_DUMP_SHOT_RE = re.compile(r"\[autotest\] SHOT (?:scanout )?dumped -> autotest/([^_\s.]+)")

# The canonical replay's first in-race SHOT. Hand-placed schedules named it `race00`;
# coverage2 (B15/B43) rederived every corpus schedule from a `[debug] cov=1` census and the
# labels are now `m<mode>_<frame>`, so a literal label is a merge hazard -- it silently
# resolved to nothing and fps_window returned an empty series, which is exactly the shape
# of the truncated A5 slices. Prefer the `mode:NN` form, which asks the recording what its
# first in-race SHOT is instead of hardcoding the answer.
IN_RACE_ANCHOR = "mode:01"


def shots_seen(text):
    """SHOT labels the guest reached, from every receipt wording that exists.

    Unions gdxdev.replay.shots_seen (the emulator wordings) with the two hardware ones,
    so one function answers the question on both lanes.
    """
    seen = set(replay.shots_seen(text))
    for m in _REC_SHOT_RE.finditer(text):
        seen.add(m.group(1))
    for m in _DUMP_SHOT_RE.finditer(text):
        seen.add(m.group(1))
    return seen


def completion(text, expected_shots, boot_text=None):
    """Did this slice's replay run to the end? -> dict, same shape as replay.completion.

    `boot_text` is the slice's WHOLE link log, marker window included. The arming receipt
    is printed while the relaunched guest boots -- before hw-run can set the begin marker,
    because setting it needs a control connection that does not exist yet -- so it is never
    inside `text`. Looking for it there scored a slice with 49/49 SHOTs and a `[rec] end`
    as "the replay never loaded".
    """
    seen = shots_seen(text)
    expected = list(expected_shots or [])
    missing = [s for s in expected if s not in seen]
    ended = replay.END_RECEIPT in text
    armed = (replay.LOADED_RECEIPTS["gdxrec"] in text
             or replay.LOADED_RECEIPTS["gdxrec"] in (boot_text or ""))
    problems = []
    if not armed:
        problems.append("no %r receipt — the replay never loaded"
                        % replay.LOADED_RECEIPTS["gdxrec"])
    if not ended:
        problems.append("no %r receipt — the replay did not reach its end"
                        % replay.END_RECEIPT)
    if missing:
        problems.append("%d of %d SHOT(s) never captured: %s"
                        % (len(missing), len(expected), ", ".join(missing[:6])))
    return {"replay_armed": armed, "end_receipt": ended,
            "shots_expected": len(expected), "shots_seen": len([s for s in expected
                                                                if s in seen]),
            "missing_shots": missing,
            "replay_complete": not problems, "detail": "; ".join(problems)}



def find_anchor(lines, anchor):
    """Index of the `[rec] SHOT` line the fps window starts after, or None.

    Two spellings. A bare string is an exact label match, which is what the hand-placed
    schedules wanted. `mode:NN` matches the first SHOT of game mode NN under the derived
    naming scheme (`m<mode>_<frame>`, tools/dev/covsched.py) -- the label a rederived
    schedule happens to mint for the first in-race frame is not stable across a rederive,
    but the mode is, so this survives the next census.
    """
    want_mode = None
    if anchor.startswith("mode:"):
        try:
            want_mode = int(anchor.split(":", 1)[1], 10)
        except ValueError:
            raise GdxError("bad --fps-anchor %r: expected mode:NN" % anchor)
        mode_re = re.compile(r"^m0*%d_\d+$" % want_mode)
    for i, line in enumerate(lines):
        m = _REC_SHOT_RE.search(line)
        if not m:
            continue
        label = m.group(1)
        if want_mode is None:
            if label == anchor:
                return i
        elif mode_re.match(label):
            return i
    return None


def anchor_candidates(expected_shots):
    """The labels an unresolved anchor could have meant, for the error message."""
    return sorted({re.sub(r"_\d+$", "", s) for s in (expected_shots or [])
                   if re.match(r"^m\d+_\d+$", s)})

# ------------------------------------------------------------------------ fps window --
DEFAULT_FPS_WINDOW_S = 60.0


def fps_window(text, anchor=IN_RACE_ANCHOR, seconds=DEFAULT_FPS_WINDOW_S):
    """fps for the first `seconds` of racing, pooled from `[watchdog]` frame deltas.

    The window is counted in BEATS, not host timestamps. The log link flushes in bursts —
    beats 30, 31 and 32 of `artifacts/hwfps3-r1-all0-interp0.log` all carry the host
    timestamp 19:16:56 — so a wall-clock window would slice the run wherever the network
    happened to hiccup. A beat is one `~5 s` heartbeat of GUEST time
    (port/3ds/main_3ds.cpp), so `60 s` is exactly the first 12 beats after the
    `[rec] SHOT 'race00'` receipt, which is the method that produced the published
    sequence `21 51 59 30 46 60 55 39 15 43 43 43`.

    Unlike prof.fps_series the first beat is NOT dropped: that rule exists because beat 0
    covers boot, which is far behind us by the time racing starts.
    """
    lines = text.splitlines()
    anchor_at = find_anchor(lines, anchor)
    if anchor_at is None:
        return [], None
    want = max(1, int(round(seconds / prof.WATCHDOG_PERIOD_S)))
    out = []
    for line in lines[anchor_at + 1:]:
        if "[watchdog]" not in line:
            continue
        m = re.search(r"frame=(\d+)\(\+(\d+)\)", line)
        if not m:
            continue
        out.append(int(m.group(2)) / prof.WATCHDOG_PERIOD_S)
        if len(out) >= want:
            break
    return out, anchor_at


AT_CAP_FPS = 59.9   # the 3DS VI cap is 60; a beat at 60.0 means "capped for all 5 s"


def fps_stats(values):
    """Percentiles are NEAREST-RANK (prof.percentile), the convention the whole CLI uses:
    a printed p50 is a value that actually occurred."""
    if not values:
        return None
    return {
        "n": len(values),
        "min": min(values), "max": max(values),
        "p10": prof.percentile(values, 10), "p25": prof.percentile(values, 25),
        "p50": prof.percentile(values, 50), "p90": prof.percentile(values, 90),
        "mean": prof.mean(values),
        "at_cap": sum(1 for v in values if v >= AT_CAP_FPS) / float(len(values)),
    }


# --------------------------------------------------------------------------- uptimes --
def host_uptime_s():
    """Seconds since the Mac booted, or None. `kern.boottime` is stdlib-free to read."""
    try:
        p = subprocess.run(["sysctl", "-n", "kern.boottime"], stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, text=True, timeout=5)
        m = re.search(r"sec\s*=\s*(\d+)", p.stdout or "")
        if m:
            return round(time.time() - int(m.group(1)), 1)
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    return None


def parse_pong(reply):
    """`OK PONG <build> <frame> <uptime_ms>` (gdx3ds_net.c:656) -> dict or None."""
    if not reply or not reply.startswith("OK PONG"):
        return None
    parts = reply.split()
    if len(parts) < 5:
        return None
    try:
        return {"build": parts[2], "frame": int(parts[3]), "uptime_ms": int(parts[4])}
    except ValueError:
        return None


# ------------------------------------------------------------------ console adapter --
class Console:
    """One control command per connection, exactly as hw.py's subcommands do.

    Every line sent is recorded, so `--dry-run` and the mocked test see the same
    sequence the console would.
    """

    def __init__(self, hw, ip, out=None, dry_run=False, timeout=15.0):
        self.hw = hw
        self.ip = ip
        self.out = out
        self.dry_run = dry_run
        self.timeout = timeout
        self.sent = []

    def _say(self, msg):
        if self.out:
            self.out.say(msg)

    # A relaunched guest opens its control socket before it can serve on it: the accept
    # lands, the first command gets RST. `[net] link up` is not a promise that the control
    # port is ready, so every transport error here is retried rather than raised -- an
    # unretried one aborted a whole six-slice matrix twice, losing the remaining slices AND
    # the restore. A protocol-level ERR is a real refusal and is never retried.
    TRANSPORT_RETRIES = 6
    TRANSPORT_BACKOFF_S = 2.0

    def command(self, line):
        self.sent.append(line)
        if self.dry_run:
            self._say("    hw> %s" % line)
            return "OK (dry-run)"
        last = None
        for attempt in range(self.TRANSPORT_RETRIES):
            try:
                with self.hw.Control(self.ip, timeout=self.timeout) as c:
                    reply = c.command(line)
                break
            except (ConnectionResetError, ConnectionRefusedError,
                    BrokenPipeError, socket.timeout, OSError) as exc:
                last = exc
                if attempt == self.TRANSPORT_RETRIES - 1:
                    raise GdxError(
                        "console control link failed %d times for %r (last: %s)"
                        % (self.TRANSPORT_RETRIES, line, exc), util.EXIT_STAGING)
                self._say("    hw> %-44s retry %d/%d (%s)"
                          % (line, attempt + 1, self.TRANSPORT_RETRIES - 1, exc))
                time.sleep(self.TRANSPORT_BACKOFF_S)
        self._say("    hw> %-44s %s" % (line, reply))
        if reply.startswith("ERR"):
            raise GdxError("console refused %r: %s" % (line, reply), util.EXIT_STAGING)
        return reply

    def ping(self):
        if self.dry_run:
            self.sent.append("PING")
            self._say("    hw> PING")
            return None
        return parse_pong(self.command("PING"))

    def ini_set(self, section, key, value):
        return self.command("INI %s %s %s" % (section, key, value))

    def ini_del(self, section, key):
        return self.command("INI DEL %s %s" % (section, key))

    def ini_save(self):
        return self.command("INI SAVE")

    def apply(self, items, save=True):
        """items: [(section, key, value_or_None)]. None deletes (see parse_config)."""
        for section, key, value in items:
            if value is None:
                self.ini_del(section, key)
            else:
                self.ini_set(section, key, value)
        if save:
            self.ini_save()

    def mark(self, token):
        """Make the GUEST write a unique line into the log stream, then clean up."""
        self.ini_set(MARK_SECTION, MARK_KEY, token)
        self.ini_del(MARK_SECTION, MARK_KEY)

    def put(self, local_path, dest):
        self.sent.append(("PUT", dest, local_path))
        if self.dry_run:
            self._say("    hw> PUT %s  <- %s" % (dest, local_path))
            return "OK (dry-run)"
        with self.hw.Control(self.ip, timeout=60.0) as c:
            reply = c.put_file("PUT %s" % dest, local_path)
        self._say("    hw> PUT %s -> %s" % (dest, reply))
        return reply

    def reload(self, threedsx):
        """Stream a new .3dsx and let the guest self-relaunch through hb:ldr."""
        self.sent.append(("RELOAD", threedsx))
        if self.dry_run:
            self._say("    hw> RELOAD %s" % threedsx)
            return None
        with self.hw.Control(self.ip, timeout=120.0) as c:
            before = parse_pong(c.command("PING"))
            reply = c.put_file("RELOAD", threedsx)
        self._say("    hw> RELOAD -> %s" % reply)
        return before


# ------------------------------------------------------------------------ log sinks --
class LogSink:
    """The runner needs exactly two things from the log: its text, and a place to put
    each slice. Subclasses differ only in who owns port 17491."""

    mode = "none"

    def text(self):
        return ""

    def rotate(self, path):
        pass

    def close(self):
        pass


class OwnedLogSink(LogSink):
    """We bound 17491 ourselves: rotate the sink file per slice (belt) AND keep the
    markers in the stream (braces). A disagreement between the two is reported."""

    mode = "owned"

    def __init__(self, hw, path):
        self.srv = hw.LogServer(port=hw.LOG_PORT, out_path=path, quiet=True)
        self.srv.start_thread()
        self.mark_index = 0

    def text(self):
        return "\n".join(self.srv.lines[self.mark_index:])

    def rotate(self, path):
        self.mark_index = len(self.srv.lines)
        out = self.srv.out
        self.srv.out = None
        if out:
            try:
                out.close()
            except OSError:
                pass
        self.srv.out_path = path

    def close(self):
        self.srv.stop.set()

    def wait_for(self, needle, timeout):
        return self.srv.wait_for(needle, timeout)


class SharedLogSink(LogSink):
    """Someone else (the user's `hw.py log`) owns 17491. We read their file and rely on
    the markers alone. No byte offsets: the file may be appended to while we read it."""

    mode = "shared"

    def __init__(self, path):
        self.path = path

    def text(self):
        return util.read_text(self.path)


def newest_live_log(root):
    cands = []
    art = os.path.join(root, "artifacts")
    if os.path.isdir(art):
        for name in os.listdir(art):
            if name.startswith("hw-") and name.endswith(".log"):
                p = os.path.join(art, name)
                cands.append((os.path.getmtime(p), p))
    return max(cands)[1] if cands else None


def open_sink(hw, root, run_dir, out, dry_run=False):
    """Own the log link if we can; otherwise fall back to the live file + markers."""
    if dry_run:
        return LogSink()
    try:
        sink = OwnedLogSink(hw, os.path.join(run_dir, "hw-link.log"))
        out.say("log link: serving 0.0.0.0:%d ourselves — slices are rotated per config"
                % hw.LOG_PORT)
        return sink
    except OSError as e:
        live = newest_live_log(root)
        if not live:
            raise GdxError(
                "port %d is already served (%s) and no artifacts/hw-*.log exists to read "
                "instead. Stop the other `hw.py log`, or point one at a file first."
                % (hw.LOG_PORT, e), util.EXIT_PREFLIGHT)
        out.say("log link: port %d is held by another process (%s); reading %s and "
                "slicing by guest marker" % (hw.LOG_PORT, e, live))
        return SharedLogSink(live)


# ---------------------------------------------------------------------- the runner --
DEFAULT_BASE_INI = "debug.det=1,debug.fh=0"
DEFAULT_RESTORE = "debug.input_replay=,debug.gputrace=0,debug.det=0"
REC_DEST_DIR = "/3ds/gdiffuser/rec"      # port/3ds/main_3ds.cpp:954 `input_replay=X`


def _slug(cfg_index, rep):
    return "cfg%02d-rep%02d" % (cfg_index, rep)


def do_hw_run(args, out, ctx):
    hw = _import_hw()
    root = ctx["root"]

    configs = [parse_config(spec) for spec in (args.config or [])]
    if not configs:
        raise GdxError("hw-run needs at least one --config "
                       "(e.g. --config 'native.all=0,native.interp=0')", util.EXIT_USAGE)
    base_ini = parse_config(args.base_ini) if args.base_ini else parse_config(DEFAULT_BASE_INI)
    base_ini.append(("debug", "gputrace", "1" if args.gputrace else "0"))

    rec_path, kind = replay.resolve(args.replay)
    if kind != "gdxrec":
        raise GdxError("hardware replay must be a .gdxrec (the tick-exact injector); %s "
                       "is an autoinput script" % rec_path, util.EXIT_STAGING)
    # `[debug] input_replay = X` makes the guest open `rec/X.gdxrec`
    # (port/3ds/main_3ds.cpp:954), so the stem on the card must be the stem we arm.
    stem = os.path.splitext(os.path.basename(rec_path))[0]
    dest = "%s/%s" % (REC_DEST_DIR, os.path.basename(rec_path))
    expected_shots = replay.shot_labels(rec_path, kind)
    base_ini.append(("debug", "input_replay", stem))

    threedsx = args.threedsx or hw.DEFAULT_3DSX
    if not os.path.exists(threedsx) and not args.dry_run:
        raise GdxError("no .3dsx at %s (pass --3dsx)" % threedsx, util.EXIT_PREFLIGHT)
    build_sha = util.sha256_file(threedsx) if os.path.exists(threedsx) else None

    ip = args.ip or ("(from tools/dev/hw.ini)" if args.dry_run else hw.load_ip(None))

    out.say("hw-run: %d config(s) x %d repeat(s) = %d slice(s), replay %s (%d SHOTs), "
            "gputrace=%s" % (len(configs), args.repeats, len(configs) * args.repeats,
                             stem, len(expected_shots), bool(args.gputrace)))
    out.say("  console %s · .3dsx %s · sha256 %s"
            % (ip, threedsx, (build_sha or "?")[:16]))
    for i, cfg in enumerate(configs):
        out.say("  config %d: %s" % (i, config_label(cfg)))

    if args.dry_run:
        return _dry_run(args, out, hw, ip, configs, base_ini, rec_path, dest, threedsx)

    rd = runs.create(ctx["run_root"], ctx["track"], args.name or "hw-run")
    out.say("  run dir %s" % rd.path)
    sink = open_sink(hw, root, rd.path, out)
    console = Console(hw, ip, out=out)

    started = util.iso_now()
    t0 = time.time()
    host_up_start = host_uptime_s()
    pong_start = console.ping()
    if pong_start:
        out.say("  console build %s frame %d up %.0fs"
                % (pong_start["build"], pong_start["frame"],
                   pong_start["uptime_ms"] / 1000.0))

    console.put(rec_path, dest)
    console.apply(base_ini)

    records = []
    try:
        # Repeats are the OUTER loop, as in artifacts/hwfps3.sh: a thermal or cache drift
        # over the session then hits every config equally instead of loading onto the one
        # that happened to run last.
        for rep in range(1, args.repeats + 1):
            for idx, cfg in enumerate(configs):
                records.append(_run_slice(args, out, ctx, hw, rd, sink, console,
                                          idx, rep, cfg, base_ini, threedsx, build_sha,
                                          stem, rec_path, expected_shots))
    finally:
        _restore(args, out, console, threedsx, sink)
        sink.close()

    ended = util.iso_now()
    wall = round(time.time() - t0, 1)
    host_up_end = host_uptime_s()
    pong_end = console.ping()

    _write_matrix_manifest(rd, ctx, started, ended, wall, build_sha, threedsx, base_ini,
                           stem, rec_path, args, records,
                           host_up_start, host_up_end, pong_start, pong_end)

    return _summarise(args, out, rd, records, configs)


def _import_hw():
    """hw.py is the hwlink track's deliverable. delegate.require gives the exit-7 message
    naming the track when it has not merged, instead of an ImportError."""
    path = delegate.require("hw.py")
    import importlib.util

    spec = importlib.util.spec_from_file_location("gdxdev_hw", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _dry_run(args, out, hw, ip, configs, base_ini, rec_path, dest, threedsx):
    console = Console(hw, ip, out=out, dry_run=True)
    out.say("")
    out.say("--dry-run: the exact sequence, no sockets opened, nothing written")
    out.say("  once, up front:")
    console.ping()
    console.put(rec_path, dest)
    console.apply(base_ini)
    for rep in range(1, args.repeats + 1):
        for idx, cfg in enumerate(configs):
            beg, end = mark_tokens("dryrun", idx, rep)
            out.say("  %s  (%s)" % (_slug(idx, rep), config_label(cfg)))
            console.apply(cfg)
            out.say("    rotate log sink -> <run>/%s.log" % _slug(idx, rep))
            console.reload(threedsx)
            out.say("    wait <=%ds for '[net] link up'" % args.wait)
            console.mark(beg)
            out.say("    wait <=%ds for '%s' (poll every %ds), then +%ds settle"
                    % (args.seconds, replay.END_RECEIPT, args.poll, args.settle))
            console.mark(end)
            out.say("    slice between the markers -> <run>/%s/log.txt + manifest.json"
                    % _slug(idx, rep))
    out.say("  restore:")
    console.apply(parse_config(args.restore or DEFAULT_RESTORE))
    console.reload(threedsx)
    out.say("")
    out.say("then: [prof] --by-vtxn buckets, in-race fps percentiles, error census, "
            "A/B vs config 0")
    out.emit({"dry_run": True, "commands": [str(c) for c in console.sent]})
    return util.EXIT_OK


def _run_slice(args, out, ctx, hw, rd, sink, console, idx, rep, cfg, base_ini,
               threedsx, build_sha, stem, rec_path, expected_shots):
    slug = _slug(idx, rep)
    label = config_label(cfg)
    out.say("")
    out.say("=== %s  %s" % (slug, label))
    slice_dir = rd.sub(slug)
    os.makedirs(slice_dir, exist_ok=True)

    console.apply(cfg)
    sink.rotate(os.path.join(slice_dir, "link.log"))

    t0 = time.time()
    started = util.iso_now()
    host_up_start = host_uptime_s()
    pong_before = console.reload(threedsx)
    relaunched = _wait_for_relaunch(hw, sink, console, args.wait,
                                    (pong_before or {}).get("build"))
    beg, end = mark_tokens(rd.run_id, idx, rep)
    if not relaunched:
        # hb:ldr can take longer than the receipt wait; marking now hits a closed control
        # port and used to raise straight out of the matrix, losing every remaining slice
        # AND skipping the restore. Give it a second chance on the control port, then
        # score this slice and carry on.
        relaunched = _wait_for_control(console, args.wait)
    if not relaunched:
        out.say("  FAIL — console never came back after RELOAD (%ds); skipping this slice"
                % args.wait)
        return {"slug": slug, "config": label, "verdict": "FAIL", "fps": [],
                "detail": "console did not relaunch within %ds after RELOAD" % args.wait,
                "started": started, "wall_s": round(time.time() - t0, 1),
                "parsed": {}, "anchor_at": None, "fps_note": "",
                "completion": {"replay_complete": False,
                               "detail": "no relaunch"}}
    console.mark(beg)

    text, status, waited = _wait_for_end(sink, beg, end, args)
    console.mark(end)
    time.sleep(min(2.0, args.settle))
    text, status = slice_marked(sink.text(), beg, end)

    with open(os.path.join(slice_dir, "log.txt"), "w") as f:
        f.write(text + "\n")

    parsed = prof.parse_text(text)
    comp = completion(text, expected_shots, boot_text=sink.text())
    fps_vals, anchor_at = fps_window(text, args.fps_anchor, args.fps_window)
    # An anchor that resolves to nothing used to return an empty series in silence, which
    # looks identical to a slice that simply never raced. Say which it was.
    fps_note = ""
    if anchor_at is None:
        cands = anchor_candidates(expected_shots)
        fps_note = ("fps window empty: --fps-anchor %r matched no [rec] SHOT receipt%s"
                    % (args.fps_anchor,
                       ("; this replay's SHOT modes are " + ", ".join(cands)) if cands
                       else ""))
        out.say("  WARN %s" % fps_note)
    census = prof.error_census(text)
    pong_after = console.ping()
    host_up_end = host_uptime_s()

    verdict = "PASS" if (comp["replay_complete"] and not census["fatals"]) else "FAIL"
    detail = comp["detail"] or ("%d fatal receipt(s)" % census["fatals"]
                                if census["fatals"] else "")
    out.say("    %s — %d lines, %d [prof], %d beats, %d fps beats in window, %d fatal%s"
            % (verdict, parsed.lines, len(parsed.prof), len(parsed.watchdog),
               len(fps_vals), census["fatals"], ("; " + detail) if detail else ""))
    if status != "ok":
        out.warn("%s: marker slice status %r — the %s marker never arrived"
                 % (slug, status, "begin" if status == "missing" else "end"))
    if not relaunched:
        out.warn("%s: no relaunch receipt within %ds; the slice may be the OLD process"
                 % (slug, args.wait))

    manifest = runs.new_manifest(
        run_id="%s-%s" % (rd.run_id, slug), track=ctx["track"], lane="hw",
        started=started, ended=util.iso_now(), wall_s=round(time.time() - t0, 1),
        build={"id": (pong_after or pong_before or {}).get("build"),
               "dirty": None, "artifact": threedsx, "sha256": build_sha,
               "generator": None, "configure_s": None, "build_s": None},
        tree=buildmod.tree_info(ctx["root"]),      # NEXT.md B46 / PROTOCOL.md
        ini={"%s.%s" % (s, k): ("(deleted)" if v is None else v)
             for s, k, v in list(base_ini) + list(cfg)},
        replay={"id": stem, "sha256": util.sha256_file(rec_path), "kind": "gdxrec",
                "events": len(expected_shots)},
        gputrace=bool(args.gputrace),
        seconds=args.seconds,
        platform={"host": os.uname().sysname + " " + os.uname().release,
                  "host_uptime_s_start": host_up_start,
                  "host_uptime_s_end": host_up_end,
                  "guest_uptime_ms_start": (pong_before or {}).get("uptime_ms"),
                  "guest_uptime_ms_end": (pong_after or {}).get("uptime_ms"),
                  "guest_frame_end": (pong_after or {}).get("frame"),
                  "model": "3DS (hardware)", "azahar": None, "retroarch": None},
        result={"verdict": verdict, "fatals": census["fatals"],
                "max_frame": parsed.watchdog[-1]["frame"] if parsed.watchdog else None,
                "distinct_frames": None, "errors": census["errors"],
                "replay_complete": comp["replay_complete"],
                "shots_expected": comp["shots_expected"],
                "shots_seen": comp["shots_seen"],
                "missing_shots": comp["missing_shots"],
                "marker_status": status, "relaunched": relaunched,
                "waited_s": waited},
        artifacts={"log": "log.txt", "emu_log": None, "shots": []},
    )
    slice_rd = runs.RunDir(slice_dir)
    slice_rd.write_manifest(manifest)
    slice_rd.write_verdict(verdict, detail)

    return {"slug": slug, "cfg": idx, "rep": rep, "label": label, "dir": slice_dir,
            "parsed": parsed, "fps": fps_vals, "anchor_at": anchor_at,
            "fps_note": fps_note,
            "completion": comp, "census": census, "verdict": verdict,
            "marker_status": status, "manifest": manifest}



def _wait_for_control(console, timeout):
    """Poll the control port until the relaunched guest answers PING, or give up."""
    deadline = time.time() + max(1, timeout)
    while time.time() < deadline:
        try:
            if console.ping():
                return True
        except Exception:
            pass
        time.sleep(2.0)
    return False


def _wait_for_relaunch(hw, sink, console, timeout, old_build):
    """A reload quits the process and Luma relaunches it. Prefer the guest's own
    `[net] link up` receipt; fall back to hw.py's PING poll when we do not own 17491."""
    if isinstance(sink, OwnedLogSink):
        return sink.wait_for("[net] link up", timeout) is not None
    line, _tail = hw.wait_for_relaunch_via_ping(console.ip, timeout, old_build)
    return line is not None


def _wait_for_end(sink, beg, end, args):
    """Poll until the replay's own end receipt shows up inside our slice, or give up.

    This is what replaces `sleep 150`: a config that finishes early does not idle, and a
    config that never finishes is reported rather than silently cut short.
    """
    deadline = time.time() + args.seconds
    t0 = time.time()
    text, status = "", "missing"
    while time.time() < deadline:
        text, status = slice_marked(sink.text(), beg, end)
        if status != "missing" and replay.END_RECEIPT in text:
            time.sleep(args.settle)     # let the trailing [prof] window land
            text, status = slice_marked(sink.text(), beg, end)
            break
        time.sleep(args.poll)
    return text, status, round(time.time() - t0, 1)


def _restore(args, out, console, threedsx, sink):
    """Leave the console in a sane state whatever happened: replay disarmed, trace off.

    `input_replay` is DELETED, not emptied — an empty entry survives `INI SAVE` and the
    next boot would still be replaying (tools/dev/hw.py:cmd_ini)."""
    if args.no_restore:
        out.say("")
        out.say("--no-restore: the last config's ini is still armed on the console")
        return
    out.say("")
    out.say("restoring: %s" % (args.restore or DEFAULT_RESTORE))
    try:
        console.apply(parse_config(args.restore or DEFAULT_RESTORE))
        console.reload(threedsx)
    except (GdxError, OSError) as e:
        out.warn("restore failed (%s) — the console may still have the last config's "
                 "levers and an armed replay" % e)


def _write_matrix_manifest(rd, ctx, started, ended, wall, build_sha, threedsx, base_ini,
                           stem, rec_path, args, records, up_start, up_end,
                           pong_start, pong_end):
    fatals = sum(r["census"]["fatals"] for r in records)
    incomplete = [r["slug"] for r in records if not r["completion"]["replay_complete"]]
    rd.write_manifest(runs.new_manifest(
        run_id=rd.run_id, track=ctx["track"], lane="hw",
        started=started, ended=ended, wall_s=wall,
        build={"id": (pong_start or {}).get("build"), "dirty": None,
               "artifact": threedsx, "sha256": build_sha, "generator": None,
               "configure_s": None, "build_s": None},
        tree=buildmod.tree_info(ctx["root"]),      # NEXT.md B46 / PROTOCOL.md
        ini={"%s.%s" % (s, k): ("(deleted)" if v is None else v) for s, k, v in base_ini},
        replay={"id": stem, "sha256": util.sha256_file(rec_path), "kind": "gdxrec",
                "events": None},
        gputrace=bool(args.gputrace), seconds=args.seconds,
        platform={"host": os.uname().sysname + " " + os.uname().release,
                  "host_uptime_s_start": up_start, "host_uptime_s_end": up_end,
                  "guest_uptime_ms_start": (pong_start or {}).get("uptime_ms"),
                  "guest_uptime_ms_end": (pong_end or {}).get("uptime_ms"),
                  "model": "3DS (hardware)", "azahar": None, "retroarch": None},
        result={"verdict": "PASS" if not (fatals or incomplete) else "FAIL",
                "fatals": fatals, "max_frame": None, "distinct_frames": None,
                "errors": None, "replay_complete": not incomplete,
                "shots_expected": None, "shots_seen": None,
                "missing_shots": incomplete,
                "slices": [r["slug"] for r in records]},
        artifacts={"log": None, "emu_log": None, "shots": []},
    ))
    rd.write_verdict("PASS" if not (fatals or incomplete) else "FAIL",
                     "%d slice(s), %d fatal, incomplete: %s"
                     % (len(records), fatals, ", ".join(incomplete) or "none"))


# --------------------------------------------------------------------- the summary --
def _f(v, nd=2):
    if v is None:
        return "-"
    return ("%%.%df" % nd) % v if isinstance(v, float) else str(v)


def pool_by_config(records):
    """[(cfg_index, label, [record, ...])] in config order — the repeats pooled."""
    order, groups = [], {}
    for r in records:
        if r["cfg"] not in groups:
            order.append(r["cfg"])
            groups[r["cfg"]] = []
        groups[r["cfg"]].append(r)
    return [(i, groups[i][0]["label"], groups[i]) for i in order]


def pooled_windows(group):
    wins = []
    for r in group:
        wins.extend(r["parsed"].prof)
    return wins


def pooled_fps(group):
    vals = []
    for r in group:
        vals.extend(r["fps"])
    return vals


def _summarise(args, out, rd, records, configs):
    groups = pool_by_config(records)

    out.say("")
    out.say("slices")
    out.table(["slice", "config", "verdict", "lines", "[prof]", "beats", "fps beats",
               "fatal", "marker", "detail"],
              [[r["slug"], r["label"], r["verdict"], r["parsed"].lines,
                len(r["parsed"].prof), len(r["parsed"].watchdog), len(r["fps"]),
                r["census"]["fatals"], r["marker_status"],
                (r["completion"]["detail"] or "")[:48]] for r in records])

    for idx, label, group in groups:
        wins = pooled_windows(group)
        if not wins:
            out.say("")
            out.say("config %d (%s): no [prof] windows" % (idx, label))
            continue
        out.say("")
        out.say("[prof] mean ms/frame by nD bucket — config %d (%s), %d repeat(s)"
                % (idx, label, len(group)))
        header = ["bucket", "wins", "frames"] + prof.PROF_SECTIONS + ["total", "vtxN"]
        rows = []
        for name, sel in prof.bucket_by_nd(wins):
            s = prof.section_summary(sel)
            rows.append([name, s["windows"], s["frames"]]
                        + [_f(s[k]["mean"]) for k in prof.PROF_SECTIONS]
                        + [_f(s["total"]["mean"]), _f(s["vtxN"], 0)])
        out.table(header, rows)

    out.say("")
    out.say("in-race fps — first %.0fs after the '%s' SHOT, %.0fs beats, repeats pooled"
            % (args.fps_window, args.fps_anchor, prof.WATCHDOG_PERIOD_S))
    rows = []
    for idx, label, group in groups:
        st = fps_stats(pooled_fps(group))
        if not st:
            rows.append([label, 0] + ["-"] * 7)
            continue
        rows.append([label, st["n"], _f(st["min"], 1), _f(st["p10"], 1), _f(st["p25"], 1),
                     _f(st["p50"], 1), _f(st["p90"], 1), _f(st["mean"], 1),
                     "%.0f%%" % (100.0 * st["at_cap"])])
    out.table(["config", "n", "min", "p10", "p25", "p50", "p90", "mean", "at 60"], rows)

    out.say("")
    out.say("error receipts")
    tags = [t for t in prof.ERROR_TAGS
            if any(g[2] and any(r["census"]["errors"][t] for r in g[2]) for g in groups)]
    if not tags:
        out.say("  none of %s in any slice" % " ".join(prof.ERROR_TAGS))
    else:
        out.table(["config"] + tags,
                  [[label] + [sum(r["census"]["errors"][t] for r in group) for t in tags]
                   for _, label, group in groups])

    if len(groups) > 1:
        _ab_tables(out, groups, args)

    fails = [r["slug"] for r in records if r["verdict"] != "PASS"]
    out.say("")
    out.say("run dir: %s" % rd.path)
    out.say("re-read any slice with:  tools/dev/gdx-dev prof %s/<slice> --by-vtxn"
            % rd.path)
    out.emit({"run_dir": rd.path, "slices": [r["slug"] for r in records],
              "failed": fails,
              "fps": {label: fps_stats(pooled_fps(g)) for _, label, g in groups}})
    if fails:
        out.err("%d of %d slice(s) FAILED: %s" % (len(fails), len(records),
                                                  ", ".join(fails)))
        return util.EXIT_FAIL
    return util.EXIT_OK


def _ab_tables(out, groups, args):
    """A (config 0) vs every later config: the delta table the A/B actually wants."""
    ai, alabel, agroup = groups[0]
    awins = pooled_windows(agroup)
    for bi, blabel, bgroup in groups[1:]:
        bwins = pooled_windows(bgroup)
        out.say("")
        out.say("A/B  A = config %d (%s)   B = config %d (%s)"
                % (ai, alabel, bi, blabel))
        sa, sb = prof.section_summary(awins), prof.section_summary(bwins)
        if sa and sb:
            rows = []
            for k in prof.PROF_SECTIONS + ["total"]:
                va, vb = sa[k]["mean"], sb[k]["mean"]
                d = vb - va
                rows.append([k, _f(va), _f(vb), "%+.2f" % d,
                             ("%+.1f%%" % (100.0 * d / va)) if va else "-"])
            out.say("  all [prof] windows, mean ms/frame")
            out.table(["section", "A", "B", "delta", "pct"], rows)

        ba = dict(prof.bucket_by_nd(awins))
        bb = dict(prof.bucket_by_nd(bwins))
        rows = []
        for name, _sel in prof.bucket_by_nd(awins):
            if name not in bb:
                continue
            x, y = prof.section_summary(ba[name]), prof.section_summary(bb[name])
            va, vb = x["total"]["mean"], y["total"]["mean"]
            rows.append([name, _f(va), _f(vb), "%+.2f" % (vb - va),
                         ("%+.1f%%" % (100.0 * (vb - va) / va)) if va else "-"])
        if rows:
            out.say("  total ms/frame per nD bucket")
            out.table(["bucket", "A", "B", "delta", "pct"], rows)

        fa, fb = fps_stats(pooled_fps(agroup)), fps_stats(pooled_fps(bgroup))
        if fa and fb:
            out.say("  in-race fps")
            out.table(["stat", "A", "B", "delta"],
                      [[k, _f(fa[k], 1), _f(fb[k], 1), "%+.1f" % (fb[k] - fa[k])]
                       for k in ("min", "p10", "p25", "p50", "p90", "mean")])
