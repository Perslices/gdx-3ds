#!/usr/bin/env python3
"""gdxrecinfo.py — inspect a .gdxrec: header, frame count, game-mode timeline, SHOTs, input runs.

    tools/dev/gdxrecinfo.py rec.gdxrec [--runs]

Prints the header lines, the number of data lines (ticks), every `# mode <m> tick <t>` marker the
recorder inserted (with the GAMEMODE_* name from decomp/include/fzx_game.h), every SHOT, and with
--runs the compressed input timeline (tick ranges with an identical pad). Stdlib only.
"""
import sys

MODES = {0: "TITLE", 1: "GP_RACE", 2: "PRACTICE", 3: "VS_2P", 4: "VS_3P", 5: "VS_4P", 6: "RECORDS",
         7: "MAIN_MENU", 8: "MACHINE_SELECT", 9: "MACHINE_SETTINGS", 10: "COURSE_SELECT",
         11: "SKIPPABLE_CREDITS", 12: "UNSKIPPABLE_CREDITS", 13: "COURSE_EDIT", 14: "TIME_ATTACK",
         15: "GP_RACE_NEXT_COURSE", 16: "CREATE_MACHINE", 17: "GP_END_CS",
         18: "GP_RACE_NEXT_MACHINE_SETTINGS", 19: "RECORDS_COURSE_SELECT", 20: "OPTIONS_MENU",
         21: "DEATH_RACE", 22: "EAD_DEMO"}
BTN = [("A", 0x8000), ("B", 0x4000), ("Z", 0x2000), ("START", 0x1000), ("UP", 0x0800), ("DOWN", 0x0400),
       ("LEFT", 0x0200), ("RIGHT", 0x0100), ("L", 0x0020), ("R", 0x0010), ("CUP", 0x0008),
       ("CDOWN", 0x0004), ("CLEFT", 0x0002), ("CRIGHT", 0x0001)]


def btn_names(mask):
    return "+".join(n for n, b in BTN if mask & b) or "-"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    show_runs = "--runs" in sys.argv
    header = []
    modes = []
    shots = []
    frames = 0
    runs = []
    cur = None
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s:
                continue
            if s.startswith("# mode "):
                tok = s.split()
                modes.append((int(tok[4]), int(tok[2])))
                continue
            if s.startswith("#"):
                continue
            tok = s.split()
            if not tok[0].isdigit():
                header.append(s)
                continue
            frames += 1
            tick, btn, sx, sy = int(tok[0]), int(tok[1], 16), int(tok[2]), int(tok[3])
            if "SHOT" in tok:
                shots.append((tick, tok[tok.index("SHOT") + 1]))
            pad = (btn, sx, sy)
            if cur is None or cur[0] != pad:
                if cur is not None:
                    runs.append((cur[1], tick - 1, cur[0]))
                cur = (pad, tick)
    if cur is not None:
        runs.append((cur[1], frames - 1, cur[0]))
    print("file: %s" % path)
    for h in header:
        print("  " + h)
    print("frames (data lines): %d" % frames)
    print("mode timeline:")
    for tick, m in modes:
        print("  tick %6d  mode %2d %s" % (tick, m, MODES.get(m, "?")))
    print("SHOTs: " + (", ".join("%s@%d" % (l, t) for t, l in shots) if shots else "none"))
    if show_runs:
        print("input runs (tick range: pad):")
        for a, b, (btn, sx, sy) in runs:
            if btn == 0 and sx == 0 and sy == 0:
                continue
            print("  %6d-%6d  %s stick=(%d,%d)" % (a, b, btn_names(btn), sx, sy))
    return 0


if __name__ == "__main__":
    sys.exit(main())
