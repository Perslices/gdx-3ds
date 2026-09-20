# Committed replays (`gdx-dev emu run --replay <id>`)

Scripts here replace the `/tmp/*-autoinput.txt` files that every agent rewrote and lost
(05-devcli-plan.md §2 item 6). Grammar: `port/input_bridge.c:208-236`, validated at stage
time by `gdxdev/replay.py` — first non-blank line must be `ticks` (seconds mode is
wall-clock and not deterministic), events tick-ascending, at most 64.

Three hard-won rules. The first two are from `docs/research/RESUME.md:150-156`; the third
was found here on 2026-09-07 and cost a whole emulator run:

1. **Interleave `SHOT` lines among the button lines.** A trailing block of SHOTs after the
   last button line is silently dropped at parse: the log says "38 events", the file has
   more, and you get zero BMPs.
2. **Tick budget.** `s_tick` ≈ the `[c3d] frame` counter 1:1. A scripted race reaches the
   course-intro card around tick 3300 and actual driving (`[race-dl]` present) around
   4600, i.e. 6-7 minutes of wall clock in the emulator. Drive SHOTs belong at 4600+.

3. **Keep every line under 62 bytes — comments included.** Builds predating the
   `input_bridge.c` line-buffer fix read the script with `char line[64]`, so a longer
   line was split across two `fgets` calls; the tail does not start with `#`, so an
   over-long COMMENT was parsed as the first data line, the `ticks` sniff failed, and the
   whole script silently ran on the wall-clock seconds timebase with every SHOT dropped.
   The symptom is a log line like `[input] autoinput script loaded: 1 events (seconds
   timebase)` from a file that plainly starts with `ticks`. The port is fixed, but
   replays are also staged onto SD cards and older builds, so `gdx-dev` refuses to stage
   a file with a long line.

`<label>_scan.bmp` (the scanout twin) is the reliable in-game oracle; the plain
`<label>.bmp` frame mirror is black in-game.

| id | what it does | typical `--seconds` |
|---|---|---|
| `boot` | boots, one title-screen SHOT | 60-90 |
| `menu` | A-mash through title into the menus, 3 menu SHOTs | 180 |
| `storm2` | A-mash 30-machine GP, card + 5 drive SHOTs — the campaign's A/B replay | 420 |
