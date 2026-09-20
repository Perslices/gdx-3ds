# oracle — progress (frame oracle, .gdxrec record/replay, determinism pins)

Track: **oracle**, branch `native/oracle`, worktree `~/code/gdx-3ds/oracle`. Plan:
`03-oracle-plan.md`; master plan §4.2. Protocol: `<scratch>/native/PROTOCOL.md`.

## State (keep this current)

| Step | Status |
|---|---|
| O1 capture entry point `gdx3ds_gfx_capture_target()` factored out of `ReadFramebufferToCPU` | done (a40586b) |
| O2 `gdx3ds_oracle_after_endframe()` + `[fh]` receipt + BMP, SHOT rerouted | done (a40586b), Azahar verification: see "Runs" |
| O3 `fh_every=N` hash-only, zero idle cost | done (a40586b) |
| O4 `.gdxrec` recorder | done (a40586b) |
| O5 `.gdxrec` replay through the unified injector | done (a40586b) |
| O6 `decomp-port-determinism-pins.patch` + README + ci list | done (56f0980); `tools/ci-3ds.sh` PASS twice on Sep 13 (pass 1 `applied: decomp (11 patches, clean tree)`, pass 2 `already applied: decomp (11 patches, full-stack roundtrip)`, build PASS) |
| O7 twin-run in-race gate | done — A vs B: 6002 frames, 1 known single-frame render transient (1702), 0 state divergence; see "Runs" |
| O8 `framediff.py` / `fhdiff.py` / mask format | done (56f0980), synthetic unit checks pass |
| O9 `gdxrec2ir.py` | done (e2b4681), packet layout only — hwlink owns the lane |

Deviations from the plan (with reasons):

- `gdx_oracle_capture_request(const char* tag)` has ONE argument, not `(tag, flags)`: the hwlink
  track declared that exact signature as its cross-track contract (coordinator message) and calls
  it from its net control thread. Flags were never needed (eye selection is the `fh_eye` key).
- The after-endframe hook increments its frame counter BEFORE the idle early-return (one memory
  increment; still no sync / config / allocation). Reason: in render-thread `ahead` mode the End
  command of frame N can execute after main has already stamped frame N+1, so the receipt's
  (frame, tick, mode) comes from a 16-entry ring keyed by frame number, which needs the count.
- `[fh]` lines carry a trailing `mode=<GET_MODE(gGameMode)>` field (plan format + one token):
  it is what lets `fhdiff` say *where* a divergence starts. Extra tokens: `stale=1` (display
  transfer never landed — the read is suspect) and `mark=miss` (ring miss — frame/tick drift).
- Host tools are stdlib-only (task requirement); `framediff.py` writes its contact sheet as PNG
  through `zlib`, no Pillow.

## Cross-track API (hwlink) — exported, `extern "C"`, non-static

```c
void gdx_oracle_capture_request(const char* tag);   // port/3ds/gdx3ds_oracle.c
int  gdx_replay_start(const char* sdmc_path);       // port/gdx_input_rec.c
```

- `gdx_oracle_capture_request(tag)`: schedules a true capture + `[fh]` receipt + BMP
  `autotest/<tag>_L.bmp` of the next presented frame. Callable from any thread (tag copied into
  a static buffer, then an atomic release-store arms the hook; the hook acquires with an
  atomic exchange). `tag == NULL` or `""` → `req`. Characters outside `[A-Za-z0-9_.-]` become
  `_` (it is a filename). A second request before the frame ends replaces the tag; one capture
  per frame. With `[debug] fh=0` the request logs `[fh] capture request ignored` and does nothing.
- `gdx_replay_start(path)`: opens `path` (anything `fopen` accepts on the sdmc cwd, e.g.
  `rec/race.gdxrec` or `/rec/race.gdxrec`), validates the `gdxrec 1` header, and starts replay
  at the **next input tick** (main thread poll). Returns 0 ok, -1 open failed, -2 not a gdxrec.
  Receipts: `[rec] replay armed <path> frames=N (starts at the next tick)`, then
  `[rec] replay <path> frames=N base=<tick>`, and `[rec] end tick=N lines=M` at EOF (physical
  input returns). Replaces a replay already in progress. A replay started at tick 0 by
  `[debug] input_replay=<name>` is the deterministic path; a mid-run `gdx_replay_start` applies
  the file relative to its start tick and is NOT a parity oracle (the game state it lands on
  is whatever it is).

## `.gdxrec` format (v1)

ASCII, `#` comment lines allowed anywhere, blank lines ignored, LF line ends.

```
gdxrec 1
build native/oracle@a40586b1
seed1 0x1234abcd            # or: seed1 none
seed2 0x0fedcba9
pins renderthread=0 rival_detail_auto=0 stereo=1 fh_every=1
# tick btn stickx sticky [touch tx ty] [SHOT label]
0 0000 0 0
1 0000 0 0
...
# mode 7 tick 612           # recorder marker: GET_MODE(gGameMode) changed (diagnostic only)
4600 8000 -34 12 SHOT race00
```

- Header: `gdxrec 1` first (non-comment) line, then `key value` lines (`build`, `seed1`,
  `seed2`, `pins`) in any order until the first data line.
- Data line K (0-based, comments excluded) is tick K: `<tick> <btn> <stickx> <sticky>`; `btn`
  is 4 hex digits of the N64 `OSContPad` mask (`A=8000 B=4000 Z=2000 START=1000 UP=0800
  DOWN=0400 LEFT=0200 RIGHT=0100 L=0020 R=0010 CUP=0008 CDOWN=0004 CLEFT=0002 CRIGHT=0001`),
  sticks decimal -80..80 (N64 convention, +Y up).
- Optional trailing tokens: `touch tx ty` (parsed and ignored by this build — no touch consumer
  in the replay path; the recorder never emits it) and `SHOT <label>` (fires
  `gdx_request_frame_dump(label)` → oracle capture on that tick's frame).
- The replayer streams with `fgets` (one line per poll, 160-byte line buffer); the tick column
  is verified against `tick - base` and a mismatch is logged ONCE (`[rec] tick mismatch`), after
  which lines keep applying one per tick.
- The recorder writes the FINAL pad state of the poll (after autoinput / script / replay), so
  recording a replay reproduces the file's data lines byte for byte (header aside).

Files live at sdmc `rec/<name>.gdxrec`. Committed canonical replays: `replays/*.gdxrec` (input
data, not game assets); `tools/dev/oracle-run.sh --replay <name>` stages them to the SD.

## ini keys ([debug]) and receipts

| Key | Default | Effect | Receipt |
|---|---|---|---|
| `fh` | 1 | oracle killswitch (0 → SHOT falls back to the legacy mirror + scanout dumps) | `[fh] init enabled=… every=… eye=… scale=… every_bmp=… legacy_shot=… build=… platform=…` |
| `fh_every` | 0 | hash-only capture every Nth frame, `tag=auto` (1 = every frame) | `[fh] frame=N tick=T eye=L w=400 h=240 hash=XXXXXXXX tag=auto mode=M` |
| `fh_start` | 0 | (fx) auto captures begin at this frame; SHOTs unaffected. The RetroArch/Azahar-libretro core crashes when captures land in the boot window (fx-progress.md gotchas) — parity runs there use `fh_start=300` |
| `fh_eye` | L | `L`, `R`, `both` (R = stereo right target, only while the right pass is active; else `[fh] … eye=R inactive`) | |
| `fh_scale` | 0 | BMP-only width downscale (160..400); hashes are always 400x240 | |
| `fh_every_bmp` | 0 | also write BMPs for `fh_every` captures (SD cost: 288 kB each) | `[autotest] SHOT captured -> autotest/<tag>_L.bmp bytes=288054 frame=N` |
| `shot_legacy` | 0 | keep the old `<label>.bmp` / `<label>_scan.bmp` dumps in addition | |
| `platform` | unknown | free text carried in the init receipt (`azahar`, `hw`); `fhdiff` warns on mismatch | |
| `det` | 0 | master determinism switch: seeds `0x1234abcd`/`0x0fedcba9`, `boot_hold_ticks=230`, `det_audio=1` unless set individually | `[det] init det=… seed1=… seed2=… boot_hold_ticks=… audio=…` |
| `rand_seed1`, `rand_seed2` | unset | Math_Rand1/2Init seeds (`0x` hex or decimal) | |
| `boot_hold_ticks` | 0 | boot-logo hold as N host ticks instead of 230 wall-clock frames | |
| `det_audio` | 0 | deterministic `audioRandom` counter (both audio libs) | |
| `input_record`, `input_record_name` | 0, rec0 | record `rec/<name>.gdxrec` from tick 0 | `[rec] recording -> rec/x.gdxrec`, `[rec] recording closed … lines=N` |
| `input_replay` | unset | replay `rec/<name>.gdxrec` from tick 0; pins `[perf] rival_detail_auto=0` for the run | `[rec] replay …`, `[rec] end tick=N`, `[rec] replay: [perf] rival_detail_auto pinned to 0` |
| `replay_quit` | 0 | leave the frame loop at `[rec] end` (self-terminating runs) | `[oracle] quit requested: …` |
| `quit_at_tick` | 0 | leave the frame loop once the input tick reaches N (self-terminating recording runs; Azahar ignores an external quit while emulation runs) | `[oracle] quit requested: [debug] quit_at_tick reached` |

First capture of a run also logs `[fh] armed: captured frames are readback-stalled and
are NOT perf samples` — never profile a window with `[fh]` lines in it.

## Determinism sources (plan §3 table) — what landed

| # | Source | Pin | Where |
|---|---|---|---|
| 1 | `Math_Rand1/2Init(osGetTime())` (`sys_gfx.c`) | `gdx_det_rand_seed()` ← `[debug] rand_seed1/2` (desktop: `GDX_RAND_SEED1/2` env fallback inside the accessor) | decomp patch |
| 2 | boot-logo hold, wall-clock → wall-clock-dependent tick count | `gdx_det_boot_hold_ticks()` ← `[debug] boot_hold_ticks` (230 under `det=1`): exactly N `gdx_yield()`s | decomp patch |
| 3 | DYNLOD tier from `svcGetSystemTick` | `gdx3ds_dynlod_set_auto(0)` forced whenever `input_replay` starts | main_3ds.cpp |
| 4 | `audioRandom *= osGetCount()` (disk + ROM audio libs) | `gdx_det_audio_pin()` OR'd into the existing capture condition; ROM lib gets the same block | decomp patch |
| 6 | stereo slider | not recorded in `.gdxrec`; parity runs pin `[stereo] enabled` in the `pins` line | — |

Findings with evidence: see "Runs" below (filled in as they land).

## Host tools

- `tools/dev/fhdiff.py a.log b.log [--eye L] [--key frame|tick|seq] [--context N]` — exit 0
  identical / 1 divergent / 2 nothing to compare; prints the first divergent frame with ±N
  receipts (hash, tag, mode, tick) and the totals; warns on `platform=` / `build=` mismatch.
- `tools/dev/framediff.py a.bmp b.bmp [--mask f] [--perceptual T] [--sheet out.png]` — exit 0/1;
  differing bytes / pixels / bbox / per-mask breakdown; contact sheet (A | B | amplified diff,
  masks outlined) only on failure. Mask format: `mask 400 240` + `rect x y w h name` lines,
  `#` comments, landscape 400x240, top-left origin. Masked diffs are still counted and printed.
- `tools/dev/inimerge.py ini --set s.k=v --del s.k` — key-level merge, never overwrites.
- `tools/dev/oracle-run.sh --name X [--replay R] [--record N] [--every N] [--det 0|1] [--autoinput f] [--timeout s] [--set s.k=v]`
  — one lock-protocol Azahar run; artifacts in `/tmp/oracle-art/X/` (`log.txt`, `azahar_log.txt`,
  `autotest/*_L.bmp`, `rec/*.gdxrec`). Restores the shared ini + `gdx-autoinput.txt` on exit.
- `tools/dev/gdxrec2ir.py rec.gdxrec out.bin | --send <ip>` — InputRedirection packets; NOT
  tick-exact, not a parity oracle (header says so).

Parity check in one command (two runs, same build, same replay, same pins):

```sh
tools/dev/oracle-run.sh --name A --replay race-mutecity-start --every 1 --det 1 && \
tools/dev/oracle-run.sh --name B --replay race-mutecity-start --every 1 --det 1 && \
tools/dev/fhdiff.py /tmp/oracle-art/A/log.txt /tmp/oracle-art/B/log.txt --known 1702
```

(`--known 1702` tolerates the documented course-name glyph transient; drop it, or add
`--set debug.trectbatch=0 --set debug.trifast=0 --set debug.tmemfast=0` to both runs, for a
strict gate. A run whose frame-351 title hash is not `4ee0d16f` hit the boot residual: re-run
it. To hold the lock across the two runs, `mkdir /tmp/azahar.lock` + a 20 s touch loop and
export `ORACLE_LOCK_OWNED=1`.)

## Host verification (no emulator)

- `.gdxrec` recorder/replayer roundtrip, `gdx_input_rec.c` compiled with clang on the Mac
  against a stub harness (`<scratch>/rectest/harness.c`): record 100 ticks with a pattern +
  `SHOT mid` at tick 50 → replay into a second recording with garbage physical input → the
  100 data lines are byte-identical, `gdx_request_frame_dump("mid")` fired exactly once at
  tick 50, `[rec] end tick=100` fired the end callback, physical input returned afterwards.
- `framediff.py` / `fhdiff.py` / `inimerge.py` / `gdxrec2ir.py` on synthetic inputs
  (`<scratch>/tooltest/`): identical BMPs → exit 0; 1-pixel diff → exit 1 + contact sheet;
  100-pixel diff inside the `timer` mask → PASS with `mask timer differing_pixels=100`
  reported; the same diff without the mask → DIFF bbox `300,10-309,19`; fhdiff finds a
  synthetic divergence at frame 50 with context; platform mismatch warns.
- `decomp-port-determinism-pins.patch`: `git -C decomp apply -R` → `apply --check` →
  `apply` → `apply --check -R` all clean; `sys_gfx.o` rebuilt after touching the sources.
  The `ci-3ds.sh` per-submodule gate replicated on a scratch clone of the decomp submodule
  (`<scratch>/gate-test.sh`, bash): combined forward apply of all 11 decomp patches on a clean
  tree OK; combined reverse in undo order → 0 dirty files; forward re-apply OK (22 files,
  +1361/-106). Note: do the undo-order loop in bash — zsh's 1-based arrays silently drop the
  last patch and the reverse then fails inside `rom/lib/thread.c` / the ilp32 headers.

## Merge notes for the coordinator (devcli / vendor reconciliation)

- **Replay staging convention is identical to devcli's**: file at sdmc `rec/<name>.gdxrec`, key
  `[debug] input_replay=<name>` (name without extension). Recording: `[debug] input_record=1`
  + `input_record_name=<name>` → `rec/<name>.gdxrec`. No delta to reconcile.
- **`port/input_bridge.c` edits (expect a conflict with devcli 39b36f5's line-buffer fix in
  `gdx_autoinput_apply`)** — the exact delta, all in this one file:
  1. `#include "gdx_input_rec.h"` after the `gdx_input_script.h` include.
  2. New file-scope `static unsigned long long s_input_tick` + exported
     `unsigned long long gdx_input_tick(void)` placed just above `gdx_autoinput_apply`; a
     forward declaration `int gdx_input_game_mode(void);` next to it, and the definition
     (`return (int) GET_MODE(gGameMode);`) right after the `extern s32 gGameMode; extern s8
     gGamePaused;` pair.
  3. Inside `gdx_autoinput_apply`: the private `static unsigned long long s_tick` became
     `const unsigned long long s_tick = s_input_tick;` (same name, so the `now` expression is
     untouched); the trailing `s_tick++;` at the end of the function was REMOVED; the SHOT
     firing branch gained one line `gdx_input_rec_note_shot(s_events[i].shot);` after
     `gdx_request_frame_dump(...)`. The parse loop (where devcli's 64-byte `line[]` fix lives)
     is untouched by me — take devcli's version of that hunk.
  4. New `void gdx_input_inject(unsigned long long tick, GdxInputPad* pad)` right after
     `gdx_autoinput_apply` (autoinput unless a replay is armed → `gdx_input_script_override`
     → `gdx_input_rec_poll`).
  5. In `gdx_controller_poll`: the `gdx_autoinput_apply(&buttons[0], ...)` call and the
     GDX_INPUT_SCRIPT block were replaced by one block that builds `scriptPad`, calls
     `gdx_input_inject(s_input_tick, &scriptPad)`, copies back, then `s_input_tick++;`.
- `gdx_request_frame_dump` (n64_gfx_bridge.cpp) routes to the oracle through two weak symbols;
  desktop builds are unaffected (both null).

## Runs (Azahar, New3DS profile, `platform=azahar`, build `native/oracle@c3021cfb`, `renderthread=0`, `stereo=1` with slider 0)

Runner: `tools/dev/oracle-run.sh` under `ORACLE_LOCK_OWNED=1` (lock held across the whole
sequence by `<scratch>/runs/held-run.sh`). Azahar runs ~8 input ticks/s here (boot ~10 s).

### smoke (23:00, 101 s) — O2/O3/O4 verification

`--record smoke --every 1 --det 1 --quit-tick 800`, autoinput `title@350 SHOT, START@400,
mainmenu@700 SHOT`. Receipts (`/tmp/oracle-art/smoke/log.txt`):

```
[fh] init enabled=1 every=1 eye=L scale=0 every_bmp=0 legacy_shot=0 quit_at_tick=800 build=native/oracle@c3021cfb platform=azahar
[det] init det=1 seed1=0x1234abcd seed2=0x0fedcba9 boot_hold_ticks=230 audio=1
[rec] recording -> rec/smoke.gdxrec
[fh] armed: captured frames are readback-stalled and are NOT perf samples
[fh] frame=1 tick=0 eye=L w=400 h=240 hash=f290da49 tag=auto mode=0
[fh] frame=351 tick=350 eye=L w=400 h=240 hash=4ee0d16f tag=title mode=0
[autotest] SHOT captured -> autotest/title_L.bmp bytes=288054 frame=351
[fh] frame=701 tick=700 eye=L w=400 h=240 hash=febe2298 tag=mainmenu mode=7
[autotest] SHOT captured -> autotest/mainmenu_L.bmp bytes=288054 frame=701
[oracle] quit requested: [debug] quit_at_tick reached
[rec] recording closed rec/smoke.gdxrec lines=801
```

801 `[fh]` lines, `frame == tick + 1` on all 801, 0 `stale=1`, 0 `mark=miss`, 0 capture
failures. Both BMPs 288,054 B and non-black (title: 50,079 / 96,000 non-black pixels, mean
41.3; main menu: 48,472, mean 19.8) — the in-game-black mirror problem is gone. `smoke.gdxrec`:
801 data lines, header as specified, mode markers `0@0`, `7@460`, SHOTs `title@350`,
`mainmenu@700`.

### Twin-run parity gate (O7) — Sep 12, 23:02–00:01, all runs back-to-back under one lock

Canonical replay `replays/race-mutecity-start.gdxrec` (6001 ticks; race entered at tick 3226,
in-race until the retire/results transition at 4895). Every run: `--every 1 --det 1`,
`renderthread=0`, `rival_detail_auto` pinned 0 by the replay, stereo enabled with slider 0.
`rec` is the recording run (autoinput-driven, `rival_detail_auto=1` since it is not a replay);
`A`/`B` are full replays; `C`/`E`/`F` are replays of `probe1702.gdxrec` (the same file with
`SHOT f1700..f1704` trailing tokens at ticks 1699-1703, quit at tick 1800); `D` is a probe run
that diverged at boot (below). Receipts: `oracle-evidence/fh-receipts-2026-09-12.txt.gz`,
transcript: `oracle-evidence/fhdiff-2026-09-12.txt`.

| run | ticks | title hash (frame 351) | frame 1702 | notes |
|---|---|---|---|---|
| rec | 6001 | 4ee0d16f | e06e08d5 | 8 SHOTs, all 288,054 B |
| A | 6002 | 4ee0d16f | 38435f61 | replay; ends at `[rec] end tick=6001` + `replay_quit` |
| B | 6002 | 4ee0d16f | e06e08d5 | replay |
| C | 1801 | 4ee0d16f | 38435f61 | probe |
| D | 1801 | **5ac9ed4c** | (n/a: different timeline) | **boot divergence at frame 213** |
| G | 1801 | 4ee0d16f | ce0f58cb | probe, fast paths off |
| H | 1801 | 4ee0d16f | ce0f58cb | probe, fast paths off — identical to G on all 1801 frames |
| E | 1801 | 4ee0d16f | 38435f61 | probe |
| F | 1801 | 4ee0d16f | e06e08d5 | probe |

**Headline (`fhdiff A B`):**

```
fhdiff: compared=6002 divergent=1 known_transient=0 only_in_a=0 only_in_b=0 first_divergent=1702 last_common=6002
  > frame=1702 A=38435f61 B=e06e08d5 tag=auto/auto mode=10/10 tick=1701/1701 DIFF
fhdiff --known 1702: compared=6002 divergent=0 known_transient=1
```

Boot (frames 1-460), title, main menu (460-1417), course select (1417-2046), machine select
(2046-2604), machine settings (2604-3226), **the whole race (3226-4895)** and the results screen
(4895-6002) hash-identical between two independent Azahar runs. Same for `rec` vs `B`
(6001/6001 identical, no exception), `rec` vs `A` (1 frame: 1702), `C` vs `E` (1801/1801).

**SHOT images:** all 8 SHOT BMPs (title, mainmenu, machsel, race00..race04) byte-identical
between `rec` and `A` (`framediff` IDENTICAL, `cmp` clean); `race00` vs `race01` of the same
run → DIFF 70,974 px, contact sheet written. Zero diff on the same frame, a sheet on different
frames — the O8 acceptance.

**What is deterministic:** with the pins on, game state is bit-reproducible from boot through
a race with the pack: 6001 consecutive frames including every in-race frame. The "timer digits +
pack positions" band of the old control-vs-control comparisons is gone; source #2 (the wall-clock
boot hold) was the cause — the seeds alone would not have fixed it.

**What is not (two residuals, both characterised):**

1. **Frame 1702 — a single-frame, two-state render transient, game state unaffected.** It is
   the second frame after the A press that confirms the cup on course select, the first frame
   the course name is drawn. `framediff C/f1702 F/f1702`: 56 pixels in the box
   (251,185)-(263,197), max channel delta 82 — the last glyph "Y" of "MUTE CITY"
   (`oracle-evidence/f1702-C-vs-F.png`). Frames 1701 and 1703 are identical in every pair, and
   4300 frames of race/results after it match, so this is a texture-path artefact (a glyph texrect
   on its first frame), not a divergence of state. Observed states: `e06e08d5` (rec, B, F),
   `38435f61` (A, C, E). **Attributed to the patched-LUS render fast paths, not the capture:**
   runs G and H replayed the same probe with `[debug] trectbatch=0 trifast=0 tmemfast=0` and
   are identical to each other on all 1801 frames INCLUDING 1702 (`ce0f58cb` both; the two
   `f1702_L.bmp` byte-identical), i.e. with the fast paths off the frame is single-state and
   reproducible; with them on it is two-state. It repeats at the same frame in every run (the
   first frame the course name is drawn after the cup confirm) and never anywhere else in a
   6002-frame run. The capture path is not the cause: 0 `stale=1` / `mark=miss` in all 9 runs,
   and every other frame hashes identically. Side finding for the texrect/tmem owners: the fast
   paths are not pixel-transparent — C (on) vs G (off) diverge from frame 475 (1037 of 1801
   frames; e.g. frame 1703 differs by 782 px in the course-name text box (142,185)-(263,208),
   `oracle-evidence/f1702-C-vs-G.png`). Bisect (one flag at a time, 3 runs each because the
   fault is two-state) is the next step for whoever owns `lus-trectbatch-atlas` /
   `lus-texrect-run-memo` / `lus-tmem-same-content-skip`. Gate policy meanwhile: `fhdiff --known
   1702` for this replay (documented, mechanical), or `framediff --mask` with
   `rect 248 182 20 20 coursename_y`, or run the gate with the three fast paths off.
2. **Boot residual — 1 run in 7 (D) diverged at frame 213, before any input.** Both C and D
   hold the same title-logo frame `7d4bcfa1` from frame 187; C leaves it at frame 221 into the
   "press start" loop (period 75 frames: 221/296/371 repeat) and the scripted START at 400 lands;
   D leaves it at 213 straight into the attract demo (`mode 1` at 270, then 0/3/0/5/0, main menu
   only at 1460), so the whole timeline shifts. No `[rec]`/`[det]` difference; the only boot
   difference in the logs is the audio thread's wall-clock progress at the same frame
   (`[audioprime2] ... ticks=8551` in C vs `ticks=8800` in D at frame 114, and
   `[sched] thread id=5 entry` reordered against the core-2 asset preload). Every decomp
   `osGetTime`/`osGetCount` consumer is pinned or cleared (plan §3), so the remaining
   wall-clock coupling is game logic observing the audio thread / core-2 preload progress
   (which tick the title's "logo done" condition fires on). Mechanically caught: `fhdiff`
   flags it at frame 213 (title hash `5ac9ed4c` ≠ `4ee0d16f`), so a parity run with a
   mismatching title hash is rejected and re-run. Next step (audio/boot owner): under `det=1`,
   make the audio thread's work per VI tick fixed (drive `gdx_audio_thread_notify_frame`
   deterministically) and gate the boot preload wait on tick count, then re-run this gate.

### discovery (Sep 7, 04:44) — mode timeline for the race script

A-tap autoinput (START@400, A every 300 from 800), `--record discovery --every 1 --det 1`.
Its log was lost (`/tmp` cleanup + another agent's run rotated the filelog), but
`rec/discovery.gdxrec` (16,800 ticks) survived and gives the canonical timeline:
`TITLE@0 → MAIN_MENU@460 → COURSE_SELECT@1417 → MACHINE_SELECT@2046 → MACHINE_SETTINGS@2604 →
GP_RACE@3226 → GP_RACE_NEXT_COURSE@9183` (A-tapping through the defaults enters the first GP
cup's first course = Mute City).

## Next steps

1. Boot residual (1 in 7 runs): pin the audio thread's per-VI-tick work and the core-2 preload
   wait under `det=1` (audio/boot owner), then re-run the gate 5x and expect 0 title-hash misses.
2. Frame-1702 transient: bisect `trectbatch` / `trifast` / `tmemfast` one at a time (3 probe
   runs each) — texrect/tmem owners; also decide whether the non-pixel-transparent fast paths
   (C vs G from frame 475) are acceptable.
3. Hardware confirmation of the capture point (plan risk 3): one SD-card run with
   `fh_every=60` + a SHOT; compare the BMP to the LCD by eye and check `stale=0`.
4. Extend the canonical replay to a full lap once a script that keeps the player alive exists
   (A-hold + steering retires at ~28 s; A-tap parks the player).

## Gotchas

- **A failed `git apply` over several patch files is NOT atomic**: when `ci-3ds.sh`'s combined
  reverse fails midway it leaves the files it had already rewritten reverted (seen Sep 13:
  `sys_gfx.c`, `disk/lib/thread.c`, `course_select.c` pristine, 19 of 22 files patched). The
  gate then reports "neither applies nor reverses" on every later run. Recovery:
  `git -C decomp checkout -- .` and re-run `tools/ci-3ds.sh` (it applies the clean stack); the
  second run must say `already applied: decomp (11 patches, full-stack roundtrip)`. Never
  hand-run partial reverse experiments inside the real submodule — use a scratch clone.

- macOS purges `/tmp` (and this session's `/private/tmp` scratchpad) after ~3 days: copy any
  run artefacts you still need into the repo (`docs/research/native/oracle-evidence/`) or
  `~/Desktop` before ending a session. The `disc1` artefacts were lost this way.
- `oracle-run.sh` reads the filelog at `sdmc/3ds/gdiffuser/log.txt` (not the sdmc root); the
  game rotates it to `log-prev.txt` at every boot, so another agent's run after yours
  destroys your log — snapshot immediately (the runner does, every 25 s and at exit).

- `tools/dev/oracle-run.sh` must never be SIGTERM'd while WAITING for the lock in a version
  before the `HAVE_LOCK` guard — its EXIT trap would `rm -rf` another agent's lock. Fixed in
  the committed script; the guard also keeps it from quitting an Azahar it did not launch.
- Bash-tool background commands are capped at 10 min: launch long runs with `nohup … &` and
  watch the log.
- The shared SD ini is restored from a backup at run end: do not edit it by hand while a run
  holds the lock.
- The decomp pin patch is LAST in the decomp series (touches `sys_gfx.c` after `decomp-ilp32`).
  After `tools/ci-3ds.sh` re-applies patches, touch `decomp/src/sys/sys_gfx.c` and both
  `audio/*/lib/thread.c` and confirm the `.o` rebuilds (stale-object trap).
