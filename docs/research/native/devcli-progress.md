# Track `devcli` — progress

Branch `native/devcli`, worktree `~/code/gdx-3ds/devcli`.
Plan: `docs/research/native/05-devcli-plan.md`. User-facing reference: `tools/dev/README.md`.

## State (2026-09-12)

`tools/dev/gdx-dev` is complete and verified for plan steps 1-8:
`build`, `emu run/lock/unlock/status/shot/click`, `prof` (`--fps --sections --ops
--by-vtxn --ab`), `runs list/show/path`, `report --markdown`, `sd stage/pull`,
`diff`/`hw` delegation, and the `retro` lane. 32 unit tests, stdlib only.

**Step 9 (adoption) is deliberately NOT done yet.** Rewiring `tools/ci-3ds.sh --emu` to
call `gdx-dev emu run` and deleting `ci-3ds.sh:401-481` would change the script that
three other tracks are running smokes with right now. It is a one-commit change once the
concurrent tracks are done; the semantics already agree (see "verdict parity" below).

## RetroArch spike — VERDICT: PASSES, the lane is real

All three gates from the plan's §5 step 8 held on the first serious attempt.

**Working configuration** (every value measured here, not assumed):

| thing | value |
|---|---|
| frontend | RetroArch 1.22.2 (Git 69a4f0ea) at `/Applications/RetroArch.app` |
| core | Azahar **2126.0 release** build, `azahar-libretro-macos-arm64-2126.0.zip` → 19,160,008 B Mach-O arm64. Preferred over the nightly so the core pins to the installed Qt version |
| renderer | `citra_graphics_api = "Vulkan"` with RetroArch `video_driver = "vulkan"` — **worked first try** on MoltenVK. OpenGL and Software were never needed |
| New3DS | `citra_is_new_3ds = "enabled"` (mandatory — the render thread is New3DS-only) |
| private SD | `<savefile_directory>/Azahar/Azahar/sdmc` — the `Azahar` segment appears **twice** |
| per-instance | own `retroarch.cfg`, `system/`, `saves/`, `screenshots/`, `retroarch.log`, UDP port |

**Evidence — two instances, concurrently, no lock:**

| | instance a | instance b |
|---|---|---|
| guest `log.txt` | 91,818 B | 91,720 B |
| end screenshot | `a-end.png` 900x1080 | `b-end.png` 900x1080 |
| state at capture | in-race, 58.8 fps, lap 00'12"15 | in-race, 57.7 fps, lap 00'12"77 |
| `[fatal]` lines | 0 | 0 |

Different race clocks in the same attract demo prove the two emulations are genuinely
independent, not two views of one process. Both bottom screens show the HUD's build id
`native/devcli@21647379`, i.e. both really ran our `.3dsx`.

**Guest log reaches BOTH sinks.** The plan listed this as unverified (§3 item 7b). It
turns out `svcOutputDebugString` reaches the core's stderr (visible with `-v`), *and*
`[debug] filelog=1` writes `log.txt` into the instance's private SD. We parse the SD log,
which is lane-independent and is what the Qt lane parses too.

**UDP network commands work** on the per-instance port (`network_cmd_port`,
55355 + a hash of the instance name). `VERSION` → `1.22.2`; `GET_STATUS` →
`GET_STATUS PLAYING 3ds,G-Diffuser-3DS,crc32=c943291`; `SCREENSHOT` writes into the
instance's own `screenshots/`. gdx-dev sends them with a stdlib UDP socket, not by
spawning a second RetroArch.

### Two gotchas the spike cost time on

1. **The first round of UDP commands got no replies at all** — `VERSION`, `GET_STATUS`
   and even `QUIT` were ignored although `lsof` showed the port bound. The identical
   commands worked on the next launch with the same config. Almost certainly macOS 15's
   Local Network privacy gate registering RetroArch on first use. If UDP looks dead on a
   fresh machine, relaunch once before concluding anything.
2. **`--max-frames N` counts FRONTEND frames, which race far ahead of guest time while
   the guest is loading.** A `--max-frames 2400` run ended after ~1.6 s of guest time,
   mid-`bootproc`, before the watchdog's first beat — and `gdx-dev` correctly called it
   FAIL ("heartbeat not advancing"). `--seconds` is therefore the default bound, with the
   end screenshot taken over UDP; `--frames` is opt-in for deterministic bounds.

### What the lane does not change

Emulator numbers stay relative-only; hardware is truth. Nothing here is a perf claim.

## Verified subcommands (evidence)

| command | evidence |
|---|---|
| `build` | Caught a real stale build id on its first run: configured `native/devcli@767b6c03` vs HEAD `21647379`, reconfigured automatically, and the new id is baked into `flags.make`. Incremental build 3.0 s. |
| `emu status` / lock | Correctly refused to steal another track's live legacy lock; a second acquire timed out with exit 4. |
| `emu run` (azahar) | See "Azahar lane" below. |
| `emu shot` | Captured the live 1706x748 "Azahar 2126.0" window; falls back to full screen when no window matches, and warns when `layout_option != 0`. |
| `prof` | 302 `[prof]` windows parsed from a real emulator log; the `nD>=100` bucket reproduces the crowd-window shape of `locked60-campaign.md:9`. |
| `prof --by-vtxn` | 4 buckets (menu/light/mid/crowd) on run `20260907-043912-retro-trace2`. |
| `runs list` / `report --markdown` | Both render the manifest for every run made here. |
| `sd stage` | Dry run by default; `--apply` produced a byte-identical `.3dsx` on a scratch volume and merged `mylever=1` while `someone_elses_key = 7` survived. |
| `diff` / `hw` | Exit 7 with the owning track and plan section named. |
| `retro run` | PASS, guest frame 9135, 0 fatals, 388 KB guest log, 144 `[prof]` windows. |

Azahar lane, run `20260912-225032-shotfix` (`/tmp/gdx-runs/devcli/20260912-225032-shotfix`):
`emu run --replay boot --seconds 100 --shots` → **PASS**, heartbeat to frame 5173, 86
distinct frames, 0 `[fatal]`, fps p10/p50/p90 = 31.2/54.2/59.8, and **2 shots**
(`title.bmp` + `title_scan.bmp`, the scanout twin showing the F-Zero X title screen at
the scripted tick). The guest log confirms `[input] autoinput script loaded: 2 events
(tick timebase)` — the same command produced `1 events (seconds timebase)` and zero BMPs
before the `input_bridge.c` fix below. The lock was taken, heartbeated and released; the
oracle track picked it up immediately afterwards.

### `prof --ab` on two real runs (2026-09-12)

Two `retro` instances run **concurrently**, same build `native/devcli@de8b8825`, same
`gputrace=1`, differing in exactly one key (`[stereo] enabled` 1 vs 0):
`20260912-225534-ab-on` (139 `[prof]` windows, PASS, frame 8927) vs
`20260912-225534-ab-off` (137 windows, PASS, frame 8642). `prof --ab … --by-vtxn` printed
the per-section delta table for all windows and per nD bucket. The deltas are noise
(total +0.5%): the attract demo is not frame-aligned between two independent instances,
which is precisely why the plan says emulator numbers are relative-only and why a real
lever A/B needs a replay driving both sides to the same frames. The tool is verified; the
experiment was not a controlled one.

Both refusal paths fire on real manifests: A/B-ing a `gputrace=0` run against a
`gputrace=1` run exits 2 with "gputrace inflates profiles (PROTOCOL.md line 22)".

This also re-demonstrated the lane's point: two instances ran the whole time while
**another track held `/tmp/azahar.lock`** for its own Qt run.

### Exit-code contract, verified end to end

| code | verified by |
|---|---|
| 0 PASS | `emu run --replay boot`, `retro run` ×3 |
| 1 FAIL | `retro run` with `--max-frames 2400` (ended mid-boot, no heartbeat) |
| 2 usage | `prof` with no target; `runs show nosuchrun`; A/B with mismatched gputrace |
| 3 preflight | `emu run /nope.3dsx` |
| 5 staging | `emu run --replay nosuch` |
| 6 no data | `prof` over a file with no receipts |
| 7 no delegate | `diff`, `diff hashes`, `hw` |

(4 lock-timeout was verified separately: a second acquire against a live foreign lock.)

### Bug fixed: `retro run --replay` was a no-op (reported by refbuild, 2026-09-12)

`retro run` declared `--replay` but never called `replay.stage()`, so the manifest showed
`"replay": null` and the guest never loaded a script. Two causes, both fixed:

1. `retro.do_run` simply did not stage. It now stages before `seed_sd()` (the ini keys
   must exist before the instance ini is written) and records the block in the manifest.
2. `replay.stage()` was hardcoded to the shared Azahar SD. It now takes `sdmc_root` and
   `gdx_dir`, so each lane passes its own — `emu run` the shared Qt SD, `retro run` its
   instance's private one. Both lanes stage by identical rules, which is what lets a
   brief say `--replay storm2` without naming a lane.

A third problem surfaced while verifying: `_retro_snapshot` collected only RetroArch's
own screenshot dir, so a replay's `SHOT` BMPs stayed stranded on the private SD. It now
also collects the guest's `autotest/`.

Verified by run `20260912-233857-replayfix` (instance a, 100 s, `--replay boot`):
`[input] autoinput script loaded: 2 events (tick timebase)` in the guest log, manifest
`replay` block populated (`kind: autoinput, events: 2`), `title.bmp` + `title_scan.bmp`
written, PASS at frame 11678 with 0 `[fatal]`. Four regression tests added (36 total).

### Bug fixed: `retro run` leaked RetroArch processes (reported by the coordinator, 2026-09-13)

Nine orphans (ppid 1, 46-65 min old) survived their drivers being killed by rate limits,
and several sat on the same instance names as live runs — so an orphan and a live run
shared one private SD, which is the exact failure this lane exists to prevent. Five fixes:

1. **Per-instance lock + pid file** at `<instance>/driver.json`, created with `O_EXCL` so
   claim and check are one atomic step. `retro run` refuses a name whose driver is alive
   and names the owning pid, track, start time and run dir (exit 4). A lock whose driver
   is dead is auto-cleared, reaping any emulator it left behind first.
2. **The driver kills its child on every exit path it can see** — `atexit` plus
   SIGTERM/SIGINT/SIGHUP handlers that clean up then re-raise the default action — and the
   emulator is launched with `start_new_session=True`, so one `killpg` takes down it and
   anything it spawned.
3. **The default instance is now the run id**, which is unique. A fixed default `a` meant
   two agents that both omitted `--instance` shared a private SD. Named instances behave
   exactly as before, which matters: 25 instance dirs from five other tracks were already
   on this machine when the bug was reported.
4. **`gdx-dev retro reap`** kills RetroArch whose driver is gone and clears stale locks.
5. **`gdx-dev retro status`** lists instances with state, owner pid, track, age, run dir.

Because unique instances multiply the private SDs, `seed_sd` now **hardlinks** the 16 MB
`fzerox.o2r` instead of copying it (the guest only reads it), so a per-run instance is
nearly free. Verified: 4 links on one inode.

**Two bugs found while testing this fix, both caught by the new tests:**

- `kill_group`'s "never signal our own group" branch fell through to `os.kill(pid)`, so
  `kill_group(os.getpid())` SIGTERMed the driver itself. There is now an explicit suicide
  guard; the group path is used only when the target really leads its own group.
- A terminated child stays visible to `kill(pid, 0)` until reaped, so polling `pid_alive`
  made every successful kill look like a failure and burned the full grace period before a
  pointless SIGKILL. `_gone()` treats a zombie as dead; the test suite went from 2.19 s to
  0.19 s as a side effect.

**Verified on the live machine**, with seven other tracks' runs going the whole time:
collision refused with exit 4 naming the owner; SIGTERM to the driver took the child down
and removed the lock (child pgid == child pid, so the session split worked); a deliberate
SIGKILL of the driver produced a real orphan at ppid 1, which `retro status` flagged and
`retro reap` killed — while sparing all nine of the other tracks' live processes.

### Four fixes from the integration-verify agent (2026-09-13)

Merged `native/main` first, which also made the `diff`/`hw` delegations resolve for real
(the oracle and hwlink tools are now on this branch) and required a rebuild — the binary
under test predated the merge and had no `.gdxrec` support compiled in at all, which
briefly looked like a fifth bug.

**1. `--replay <x>.gdxrec` was a silent no-op in both lanes.** The guest builds the
RELATIVE path `rec/<name>.gdxrec` (`port/3ds/main_3ds.cpp:794`) and a `.3dsx` runs with
the SD root as its cwd, so the file must sit at `<sd>/rec/`. `rec_dir()` staged it under
`<sd>/3ds/gdiffuser/rec/`, so the ini key was set, the guest logged `[rec] replay open
failed`, and the run carried on unscripted **and still PASSed**. Fixed to the root
(`tools/dev/oracle-run.sh:129` is the reference). Two further parts:

- Replay ids now resolve from `<repo>/replays/` as well as `tools/dev/replays/`. The
  oracle track keeps its `.gdxrec` corpus in the former, so half of it was unreachable
  by name.
- **Both lanes now FAIL when a staged replay does not load**: `replay.verify_receipts()`
  requires `[rec] replay armed` (gdxrec) or `autoinput script loaded:` (script), and
  fails on `[rec] replay open failed`, on a missing receipt, or on a script that fell
  back to the seconds timebase. Without this a staging mistake is invisible, because the
  run boots fine and the heartbeat advances.

The old unit test asserted the *wrong* path — it encoded the bug, which is how it
survived. It now asserts `<sd>/rec/` and a companion test pins the expectation to the
literal `"rec/%s.gdxrec"` in the guest source so the two cannot drift apart.

**2. RetroArch hung on an invisible NSAlert.** After an abnormal exit macOS offers to
restore the previous session; RetroArch has no window to host the alert, so it is
invisible and the process waits forever. `retro run` now writes
`ApplePersistenceIgnoreState` on the bundle before **every** launch.

**3. Intermittent core crash on boot.** azahar-libretro dies on its VulkanWorker
(SIGBUS/SIGSEGV) around guest frame 35-40, independent of the build under test. `retro
run` now treats "the heartbeat never advanced" as an early crash and retries the same
renderer up to 3 attempts before falling back, recording every attempt in the manifest's
`attempts` array. Subtlety worth keeping: a *single* `[watchdog]` beat is not "booted" —
the crash lands after the first beat, and an earlier version of this check accepted it as
success so the retry never fired. `--gfx vulkan|opengl|software` is now case-insensitive;
measured recommendation below.

**Measured renderer stability** (five consecutive 60 s boots each, clean instances):
Vulkan 4/5 runs PASS with **0 of 5 boot attempts crashing**; OpenGL 5/5 runs PASS but
**2 of 7 boot attempts crashed** (the retry rescued it). Both reach ~guest frame 4600 in
60 s. So the documented recommendation is to leave `--gfx` unset — the existing
Vulkan-first default is the stable one. Vulkan's one FAIL produced no guest log at all,
a case that now retries too. Small sample, other tracks' emulators running concurrently.

**A fifth bug, found while measuring.** The first sweep reported Vulkan 0/5 and OpenGL 1/5
with a ~100% crash rate, which was wrong: `seed_sd` copies the shared Qt ini into a new
instance, so every instance inherited `input_replay = race-mutecity-start`, `replay_quit`
and `fh_every = 1` from whichever agent used the Qt SD last. Every boot logged
`[rec] replay open failed` for a replay it was never given, and frame-hashed every frame.
`strip_run_control()` now removes run-control keys when seeding (preferences are kept),
and the re-measured numbers above are from clean instances. Two smaller ones with it: the
crash detector matched Azahar's benign `"... with signal 00000001"` APT tracing, and
staging an autoinput script did not clear a stale `input_replay` — which wins over
autoinput (`main_3ds.cpp:772`), so the script would never have run.

**4. `[fatal-trace] atexit` failed every clean run.** `port/3ds/main_3ds.cpp:815` emits it
from the normal `exit()` path, and `emu.judge` counted it as fatal, so a healthy run
reported "1 [fatal] line(s)". It is now classified benign in `prof.py` (counted separately
as `parsed.benign`), which fixes `emu run`, `retro run` and `prof --fail-on-error` at once.
A real `[fatal-trace]` backtrace still counts.

### A crashed core reported PASS (reported 2026-09-17)

An `EXC_BAD_ACCESS` at guest frame ~433, 8 s into a 6000-frame replay, was reported
`PASS heartbeat reached frame 432, 0 [fatal] lines`. The heartbeat advanced and nothing
logged `[fatal]`, so every check the verdict had was satisfied — the run simply stopped.

The verdict now requires **completion** whenever a replay is staged: `[rec] end` plus
every scheduled SHOT for a `.gdxrec`; the last scheduled tick plus every SHOT for an
autoinput script; and in both cases the emulator still alive at the end of `--seconds`
unless it exited deliberately via `replay_quit`/`quit_at_tick`. SHOTs are counted by label
from `[fh] … tag=` and `[autotest] SHOT captured ->`, skipping the periodic `tag=auto`
captures.

`replay_complete`, `shots_expected`, `shots_seen`, `missing_shots` and `wall_s` are in the
manifest, and every run writes a one-line `RESULT` carrying the **artifact sha256** rather
than the build id — `GDX3DS_BUILD_ID` is configure-time and names the wrong commit after a
merge-then-rebuild.

Verified on one binary (`sha256=2be07093d4b47553`), both directions:

```
RESULT FAIL frame=4969 ... complete=False shots=5/8 wall_s=57.3    # --seconds 50
  detail: no '[rec] end' receipt; 3 of 8 SHOT(s) never captured: race02, race04, race03
RESULT PASS frame=11863 ... complete=True shots=8/8 wall_s=157.9   # --seconds 150
  guest log: [rec] end tick=6001 lines=6001
```

The 50 s run is exactly the shape of the reported bug and previously reported PASS.

One trap this created and closed: SHOT capture is gated on `[debug] fh`, which
`strip_run_control` strips from a seeded ini, so staging a `.gdxrec` with SHOTs now arms
`fh=1` (as `oracle-run.sh` does). Without it the guest logs "capture request ignored" and
a perfectly good run fails as incomplete.

### `retro reap --prune`

Deletes idle instance dirs older than `--days` (default 2); never touches one with a
running RetroArch or a live driver lock. There were **74 dirs, 852 MB** here within days.

The age heuristic needed a second attempt. "Newest file in the tree" pruned nothing at any
threshold, including `--days 0`, because `seed_sd` copies the user's `dspfirm.cdc` — dated
**2035** on this machine — into every instance, putting each one nine years in the future.
Age now comes from `retroarch.cfg`, which gdx-dev rewrites at every launch. Only files this
tool writes itself are trustworthy clocks.

### Four items from integration-verify-2 (2026-09-17)

**1. `emu run` idled out the full `--seconds` after the guest exited.** The loop watched
the emulator PROCESS, but the emulator routinely outlives the guest: a `.3dsx` that exits
leaves Azahar up with a dead window. Both lanes now watch the guest log for
`[fatal-trace] atexit:` / `[oracle] quit requested` and return immediately, and poll every
2 s instead of sleeping a whole snapshot interval between checks. Measured on a
`quit_at_tick` run with `--seconds 240`: retro returns in **24 s**, Qt in **168 s**.

**2. `quit_at_tick` now counts as complete.** The receipt only exists because the tick was
reached, so it satisfies the end requirement exactly as `replay_quit` does, and SHOTs
scheduled after the cut are dropped from the expectation instead of failing a run that did
what it was told. This needed SHOT *ticks*, not just labels, so the replay block carries a
`schedule` now.

**3. One error classifier.** `emu.judge` re-derived its own fatal count from the raw tag
totals instead of using the benign filter — which is exactly how the two paths drifted and
why `[fatal-trace] atexit` kept failing clean runs after `prof.py` was fixed. Everything
goes through `prof.error_census()` now.

Chasing that turned up a **worse bug the report did not mention**: `logFatal()`
(`main_3ds.cpp:412`) writes the same string to `svcOutputDebugString` *and* the SD
filelog, and `judge` **summed the two sinks** — so a single assert was reported as
"2 [fatal] line(s)". The census deduplicates by taking the max across sinks.

**4. Lever inheritance, properly closed.** The previous fix used a key list, and
`trectbatch` was not on it — a key list has to be chased every time another track invents
a lever. The rule is now by SECTION: strip every key in `[debug]` and `[native]`, keep
every other section as user preferences. Against the real shared ini that removes **33
keys**, including `quit_at_tick = 3400` (every seeded run would have stopped at tick 3400),
`tmemfast`, `trifast` and the whole `[native]` block, while `border_mode`, `deadzone`,
`stereo` and `[net]` survive. Since nothing is inherited any more, the retro lane now sets
the same `MEASUREMENT_INI` as the Qt lane explicitly.

Verified end to end on both lanes with one binary (`sha256=10e0c78e6d3d47a7`):

```
retro, --seconds 240, --ini quit_at_tick=1500:
  guest exited after 16s (exited via exit()) — stopping early
  RESULT PASS frame=1300 fatals=0 replay=race-mutecity-start(gdxrec) complete=True
         shots=2/2 wall_s=24.4
qt,    --seconds 240, --ini quit_at_tick=1200:
  guest exited after 163s — stopping early
  RESULT PASS frame=1184 fatals=0 ... complete=True shots=2/2 wall_s=167.7
```

Both runs carry `[fatal-trace] atexit` and report `fatals=0`, and both expected exactly
the 2 SHOTs scheduled before the cut (title@350, mainmenu@1000) out of the replay's 8.
110 tests.

## Gotchas found (for whoever inherits this)

- **`gdx-autoinput.txt` lines must stay under 62 bytes — comments included.** This was a
  real port bug, found because `emu run --replay boot` PASSed the heartbeat yet produced
  zero BMPs. `port/input_bridge.c` read the script with `char line[64]`, so a longer line
  came back from `fgets` without its newline and the next call returned the tail; the
  tail does not start with `#`, so an over-long COMMENT became the first data line, the
  `ticks` sniff failed, and the script ran on the wall-clock seconds timebase with every
  SHOT dropped. A 91-character comment header was enough. Fixed in the port (bigger
  buffer plus draining the remainder, so one physical line is one logical line at any
  length) and guarded in `gdx-dev`, which refuses to stage a replay with a long line
  because cards and older builds still have the old parser.
- **`retro reap` must never treat "no lock file" as orphan evidence.** Other tracks run
  on this machine, and processes predating the lock feature have live drivers and no lock.
  Only two things prove a driver is gone: reparenting to pid 1, or a lock naming a dead
  pid. An early draft of `reap` classified all nine of another track's live runs as
  orphans; the dry-run caught it before anything was killed.
- **Run directories under `/tmp` do not survive a long gap.** The 2026-09-07 run dirs
  came back with their `manifest.json` reaped; `runs list` prints "no manifest" rather
  than crashing, but anything you want to keep must be copied out of `/tmp`.
- **Running the `azahar` binary directly pops a modal alert** ("The `azahar` executable is
  being run directly rather than via the Azahar.app bundle"). Emulation continues behind
  it — which is why `tools/ci-3ds.sh` never noticed — but the alert is the frontmost
  window, so a naive `screencapture -l` returns a picture of the dialog. `emu run` uses
  `open -n -a Azahar.app` (PROTOCOL.md line 15) and `wid.swift` sorts windows by area.
- **The shared `azahar_log.txt` had 704,798 bytes of another agent's run** at the moment
  this track's smoke launched. `ci-3ds.sh` greps that whole file for `[fatal]`, so a
  neighbour's crash fails your run. `emu run` slices from the byte offset taken at launch.
- `azahar_log.txt` is **truncated when Azahar starts**, so the offset can land past EOF;
  the slicer detects `size < offset` and restarts at 0 with a warning.
- `[profop]` windows can have an **empty** top list (`[profop] n=65 top:`).
- Older logs lack `[watchdog]` columns `hwm=`/`base=` — parse key=value, never positionally.
- The **first `[watchdog]` beat's `(+N)` covers process start**, not 5 seconds of frames.
  Including it drags every fps percentile down; `prof --fps` drops it.
- `git status --porcelain` is always non-empty here (the 48-patch stack keeps the
  submodules modified), so the manifest's `dirty` uses `--ignore-submodules=all`.

## What other tracks must change to plug in

- **oracle**: land `tools/dev/framediff.py` and `tools/dev/fhdiff.py` at those exact
  paths and `gdx-dev diff` starts working with no change here. For `.gdxrec`, `gdx-dev`
  already stages `rec/<name>.gdxrec` and sets **`[debug] input_replay=<name>`** — if the
  key or the directory differ, tell this track rather than changing the caller.
- **hwlink**: land `tools/dev/hw.py` with the subcommands from `02-hwlink-plan.md §4`;
  `gdx-dev hw <args…>` forwards argv and the exit code verbatim.
- **every track**: put replays in `tools/dev/replays/` rather than `/tmp`, and cite
  `gdx-dev report <run_id> --markdown` output in progress files.

## Next step

1. Step 9 adoption: `ci-3ds.sh --emu` → `gdx-dev emu run`, delete `ci-3ds.sh:401-481`.
   Held back only because three other tracks are running smokes through that script right
   now; the verdict semantics already agree (same heartbeat rule, same fatal rule, scoped
   to this run's log slice instead of the shared file).
2. `prof --ab` has no two-real-run A/B yet. It is unit-tested and both refusal paths work
   (differing `gputrace`, differing `build.id`), but it has not been run over two
   6-minute lever runs — that needs the Qt lock for ~15 minutes, or two `retro` instances.
3. `emu click` is unexercised — it needs Accessibility permission for the terminal, which
   only the user can grant.
4. `layout_option` is 1 on this machine, so `emu shot` captures one screen and warns.
   Set it to 0 in Azahar's qt-config.ini before any two-screen visual comparison.
