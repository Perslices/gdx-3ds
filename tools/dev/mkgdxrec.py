#!/usr/bin/env python3
"""mkgdxrec.py — author a `.gdxrec` tick-exact input recording from a readable script.

Why this exists: the autoinput grammar (`port/input_bridge.c`, `tools/dev/replays/*.txt`) is
capped at 64 events, holds exactly ONE named input per event and cannot express a button+stick
combination. The long-tail menu replays this repo needs (records, machine creator, course editor,
4P VS, a second GP course) are 20-60 discrete presses plus a driving tail, so they do not fit.
`.gdxrec` (format: `port/gdx_input_rec.h`, documented in docs/research/native/oracle-progress.md)
has no such limits: it is one ASCII line per input tick, which is exactly what a deterministic
menu walk wants. This script turns a step list into that file.

The output is byte-for-byte what the in-game recorder (`[debug] input_record=1`) would write for
the same inputs minus the `# mode` markers, so the canonical workflow is:

    mkgdxrec.py x.gdxs x.gdxrec      # author
    gdx-dev retro run ... --replay x --ini input_record=1 --ini input_record_name=xr
                                     # replay it, recorder re-emits it with `# mode` markers
    (commit the recorder's file: it carries the observed mode timeline)

Script grammar (`.gdxs`), one command per line, `#` comments, blank lines ignored:

    build  <str>          header keys, emitted verbatim (seed1/seed2 accept `none`)
    seed1  <hex|none>
    seed2  <hex|none>
    pins   <str>
    note   <str>          extra `#` comment line in the output header

    wait   N              N idle ticks
    at     N              idle ticks until absolute tick N is the next tick to emit
    press  BTNS [H] [G]   H ticks holding BTNS then G idle ticks   (defaults 12, 30)
    hold   BTNS N [SX SY] N ticks holding BTNS (BTNS may be `-` for none)
    stick  SX SY N        N ticks of stick only
    shot   LABEL          attach `SHOT LABEL` to the NEXT tick emitted
    len    N              pad idle ticks until the file has N data lines (ticks 0..N-1)

BTNS is one or more names joined by `+`: A B Z START UP DOWN LEFT RIGHT L R CUP CDOWN CLEFT
CRIGHT (N64 OSContPad mask). Sticks are the N64 convention, -80..80, +Y up.

Stdlib only (PROTOCOL.md: host tools are stdlib-only).
"""

import argparse
import sys

BTN = {
    "A": 0x8000, "B": 0x4000, "Z": 0x2000, "START": 0x1000,
    "UP": 0x0800, "DOWN": 0x0400, "LEFT": 0x0200, "RIGHT": 0x0100,
    "L": 0x0020, "R": 0x0010,
    "CUP": 0x0008, "CDOWN": 0x0004, "CLEFT": 0x0002, "CRIGHT": 0x0001,
}

MAX_LINE = 150  # the guest replayer reads with a 160-byte line buffer (gdx_input_rec.c)


class ScriptError(Exception):
    pass


def parse_buttons(tok):
    if tok in ("-", "0", "NONE"):
        return 0
    mask = 0
    for name in tok.upper().split("+"):
        if name not in BTN:
            raise ScriptError("unknown button %r" % name)
        mask |= BTN[name]
    return mask


class Builder:
    def __init__(self):
        self.header = {"build": "unknown", "seed1": "none", "seed2": "none", "pins": ""}
        self.notes = []
        self.lines = []          # (btn, sx, sy, shot|None)
        self.pending_shot = None
        self.shots = []

    def emit(self, n, btn=0, sx=0, sy=0):
        for _ in range(int(n)):
            shot = None
            if self.pending_shot is not None:
                shot = self.pending_shot
                self.shots.append((len(self.lines), shot))
                self.pending_shot = None
            self.lines.append((btn, sx, sy, shot))

    def run(self, text):
        for lineno, raw in enumerate(text.splitlines(), 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            cmd, args = parts[0].lower(), parts[1:]
            try:
                self.step(cmd, args, raw)
            except ScriptError as exc:
                raise ScriptError("line %d: %s  (%s)" % (lineno, exc, raw.strip()))
        if self.pending_shot is not None:
            raise ScriptError("dangling `shot %s`: no tick follows it" % self.pending_shot)
        if not self.lines:
            raise ScriptError("script produced no ticks")

    def step(self, cmd, args, raw):
        if cmd in ("build", "pins", "note"):
            value = raw.strip().split(None, 1)[1] if len(raw.strip().split(None, 1)) > 1 else ""
            if cmd == "note":
                self.notes.append(value)
            else:
                self.header[cmd] = value
            return
        if cmd in ("seed1", "seed2"):
            self.header[cmd] = args[0]
            return
        if cmd == "wait":
            self.emit(args[0])
            return
        if cmd == "at":
            target = int(args[0], 0)
            if target < len(self.lines):
                raise ScriptError("`at %d` is already past (tick %d)" % (target, len(self.lines)))
            self.emit(target - len(self.lines))
            return
        if cmd == "press":
            btn = parse_buttons(args[0])
            hold = int(args[1]) if len(args) > 1 else 12
            gap = int(args[2]) if len(args) > 2 else 30
            self.emit(hold, btn)
            self.emit(gap)
            return
        if cmd == "hold":
            btn = parse_buttons(args[0])
            n = int(args[1])
            sx = int(args[2]) if len(args) > 2 else 0
            sy = int(args[3]) if len(args) > 3 else 0
            self.emit(n, btn, sx, sy)
            return
        if cmd == "stick":
            self.emit(int(args[2]), 0, int(args[0]), int(args[1]))
            return
        if cmd == "shot":
            label = args[0]
            if self.pending_shot is not None:
                raise ScriptError("`shot %s` overwrites pending `shot %s`" % (label, self.pending_shot))
            if not all(c.isalnum() or c in "_.-" for c in label):
                raise ScriptError("SHOT label %r must match [A-Za-z0-9_.-]" % label)
            self.pending_shot = label
            return
        if cmd == "len":
            target = int(args[0], 0)
            if target < len(self.lines):
                raise ScriptError("`len %d` is shorter than the script (%d)" % (target, len(self.lines)))
            self.emit(target - len(self.lines))
            return
        raise ScriptError("unknown command %r" % cmd)

    def render(self):
        out = ["gdxrec 1"]
        out.append("build %s" % self.header["build"])
        out.append("seed1 %s" % self.header["seed1"])
        out.append("seed2 %s" % self.header["seed2"])
        out.append("pins %s" % self.header["pins"])
        for note in self.notes:
            out.append("# %s" % note)
        out.append("# tick btn stickx sticky [touch tx ty] [SHOT label]")
        for tick, (btn, sx, sy, shot) in enumerate(self.lines):
            row = "%d %04x %d %d" % (tick, btn, sx, sy)
            if shot:
                row += " SHOT %s" % shot
            out.append(row)
        return "\n".join(out) + "\n"


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("script")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--build", help="override the `build` header key")
    args = ap.parse_args(argv)

    with open(args.script) as fh:
        text = fh.read()
    builder = Builder()
    try:
        builder.run(text)
    except ScriptError as exc:
        sys.stderr.write("mkgdxrec: %s\n" % exc)
        return 2
    if args.build:
        builder.header["build"] = args.build
    body = builder.render()
    for line in body.splitlines():
        if len(line) > MAX_LINE:
            sys.stderr.write("mkgdxrec: line over %d bytes: %s\n" % (MAX_LINE, line))
            return 2
    if args.out:
        with open(args.out, "w") as fh:
            fh.write(body)
    else:
        sys.stdout.write(body)
    sys.stderr.write("mkgdxrec: %s -> %d ticks, %d SHOTs (%s)\n" % (
        args.script, len(builder.lines), len(builder.shots),
        ", ".join("%s@%d" % (lbl, t) for t, lbl in builder.shots) or "none"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
