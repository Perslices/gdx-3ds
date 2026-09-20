# Track `devcli` — one CLI for the fork's dev loop (`tools/dev/gdx-dev`)

Plan only. Nothing implemented. Master plan §4.5 names this deliverable; §3 is the
indictment it answers. Companion tracks: `oracle` (framediff/fhdiff), `hwlink` (hw.py).

## 1. Goal

Every agent on this fork re-derives the same procedures from prose in `docs/research/`,
writes them into `/tmp/*.sh|*.swift|*.py`, and the scripts evaporate with the session.
`RESUME.md` §3 records the cost: *"All `/tmp` helper scripts are gone"*, *"agents have
killed each other's runs and deleted each other's artifacts"*, *"no committed diff tool
(each agent re-did it by hand with `cmp`)"*.

`tools/dev/gdx-dev` is a single stdlib-only Python 3 CLI (no pip, no venv) that owns those
procedures as committed, tested code. Success is measurable: a subagent brief stops containing
shell recipes and contains `gdx-dev` invocations instead; every run leaves a machine-readable
manifest a replacement agent can read after the first dies at a token limit.

Non-goals: it does not replace `tools/ci-3ds.sh` (CI keeps its entry point and calls into
gdx-dev later), does not judge performance (hardware is truth), and does not build the frame
oracle or the hardware link — it delegates to those tracks.

## 2. Inventory: the ad-hoc scripts being replaced

| # | Procedure | Where it lives today | Replaced by |
|---|---|---|---|
| 1 | Azahar singleton lock: `mkdir /tmp/azahar.lock`, poll, `touch` ≤25 s, `rm -rf`, steal if stale | `tools/ci-3ds.sh:402-425` (as bash), re-described in `locked60-campaign.md` "Common rules", PROTOCOL.md | `emu lock/unlock/status`, held by every `emu run` |
| 2 | Launch + bounded boot + kill: `pkill -9 -x azahar`, pgrep-empty check, launch, sleep N, SIGTERM then -9 | `tools/ci-3ds.sh:447-464` | `emu run --seconds` |
| 3 | Verdict parsing: `grep OutputDebugString \| grep -Eo 'frame [0-9]+'`, distinct-frame count, `[fatal]` count | `tools/ci-3ds.sh:466-478` | `emu run` verdict + `prof --fail-on-error` |
| 4 | Preflight: `log_filter=*:Debug` in qt-config.ini, `fzerox.o2r` present, Azahar binary present | `tools/ci-3ds.sh:427-440` | `emu run` preflight (exit 3) |
| 5 | ini **merge** (never overwrite other agents' keys) into `sdmc/3ds/gdiffuser/gdiffuser.ini`; measurement set `console=1 filelog=1 filelog_max_kb=4096 diag_audio=1 verbose=1 gputrace=1` | prose only, `locked60-campaign.md` "Common rules" | `emu run --ini k=v`, `sd stage --ini` |
| 6 | Autoinput staging: `/tmp/gpt-autoinput.txt` → `sdmc/gdx-autoinput.txt`; grammar `ticks` header, `<tick> TOKEN [hold]`, `<tick> SHOT <label>`, ≤64 events, tick-ascending | grammar in `bridgecache-progress.md` M5; scripts (`gp-autoinput.txt`, `storm2.txt`) were scratch-only and are gone | `emu run --replay <id>`, replays committed under `tools/dev/replays/` |
| 7 | Artifact snapshot every ~25 s (shared SD is hostile; other agents delete SD artifacts) | prose, `locked60-campaign.md`; PROTOCOL.md line 20 | `emu run` snapshot loop → run dir + `/tmp/<track>-art/` |
| 8 | Window capture: `swift /tmp/wid.swift` → `screencapture -x -l <id>`, `layout_option=0` for both screens | `RESUME-2026-09-05.md:25`, `locked60-round3.md:6`, `locked60-round2.md:35` — the swift file itself is gone | `emu shot` (vendored `wid.swift` under `tools/dev/mac/`) |
| 9 | Click injection via a hand-written CGEvent swift snippet | scratch only, undocumented, rewritten per agent | `emu click x,y` (same vendored dir) |
| 10 | `[prof]`/`[profop]`/`[rt]`/`[gpu]`/`[fill]` window tables, hand-assembled with grep/awk per report | every `*-progress.md` A/B table | `prof` |
| 11 | fps from `[watchdog] frame=N(+D)` 5-second deltas (`+130 ≈ 26 fps`) | `docs/3DS-HARDWARE.md:177-180`, computed by hand every time | `prof --fps` (p10/p50/p90) |
| 12 | SD card loop: `/Volumes/NO NAME`, copy `.3dsx` to `3ds/`, write ini, `rm log.txt`, `sync`, `diskutil eject` | `RESUME-2026-08-22.md:11-12`, `RESUME-2026-09-02.md:60-61` | `sd stage` / `sd pull` |
| 13 | Frame compare by hand with `cmp` | `RESUME.md` §3 ("no committed diff tool") | `diff` → oracle's `framediff.py` |

Receipt formats gdx-dev must parse are already fixed in the source and must not be
re-guessed (`port/3ds/gfx/gdx3ds_gpu_prof.c`):
`[prof%s] frame=%lu n=%u br= dsp= vtx= tri= imp= drw= mtx= oth= | vtxN= nV= nT= nI= nD= nM= nRun=`,
`[profop] n=%u top:`, `[gpu] frame= n= wall= build= proc= gpu= wVbl= wP3D=`,
`[fill] … passes= copies= rdbk= estMpix=`, `[rt] mode= frame= n= tasks= waitMain= waitRender= fence=`,
`[watchdog] beat= frame=N(+D) stage= fiber=`, `[race-dl] lists= f3d= cmds= dl= vtx= mtx=`.
Error lines to count: `[fatal]`, `[fatal-trace]`, `[gfxfail]`, `[gdl-bad]`, `[gdl-miss]`,
`[datafail]`, `[texdiag]`, `bad_alloc`.

## 3. RetroArch / libretro viability (2026-09-07)

The singleton is the worst structural problem in the loop: one Azahar, four agents, a
`/tmp` lock, and a *shared* virtual SD that agents overwrite. Items 1-3 and 6 were run
directly on this Mac today; 4-5 are from the shipped binary plus upstream source (links at
the end of the section). Findings:

1. **Azahar Qt CLI, ground truth from the installed build** (Azahar 2126.0,
   `/Applications/Azahar.app/Contents/MacOS/azahar --help`, run 2026-09-07):
   `-p/--movie-play <path>`, `-r/--movie-record <path>`, `-a/--movie-record-author`,
   `-g/--gdbport <port>`, `-d/--dump-video <path>`, `-f/--fullscreen`, `-w/--windowed`,
   `-i/--install`, `-c`/`-x` (Z3DS compress/decompress), `--room`, `-v`, `-h`.
   **Confirmed absent**: any `--screenshot`, `--headless`, `--no-gui`, `--config`, or
   instance/profile flag. Screen capture must therefore stay host-side (§2 item 8), and
   `.ctm` play/record is the lane's only deterministic-input primitive. Worth correcting a
   folk belief: Azahar has **no** process-level single-instance lock (no SingleApplication in
   `src/citra_qt/CMakeLists.txt`, no `LSMultipleInstancesProhibited` in `Info.plist`), so
   `open -n` does start a second one. The singleton is enforced by the *shared user-data
   directory* — one `~/Library/Application Support/Azahar`, one virtual SD, no flag to move
   it. That is what our `/tmp` lock actually protects, and why it cannot just be dropped.
2. **A macOS arm64 Azahar libretro core exists prebuilt.**
   `https://buildbot.libretro.com/nightly/apple/osx/arm64/latest/azahar_libretro.dylib.zip`
   — HTTP `last-modified: Mon, 07 Sep 2026 01:01:30 GMT`, 8,614,060 bytes; unpacked
   `azahar_libretro.dylib` is a 31.5 MB **Mach-O 64-bit arm64** dylib (checked today). No
   source build required. Azahar's release 2126.0 (2026-08-10) also ships
   `azahar-libretro-macos-arm64-2126.0.zip` — **prefer that**, since it pins the core to the
   exact frontend version installed here and keeps the lanes comparable; the nightly is the
   fallback. `.github/workflows/libretro.yml` builds macOS arm64 in CI, so the lane is
   maintained, not incidental.
3. **The core takes `.3dsx`.** `~/Library/Application Support/RetroArch/info/azahar_libretro.info`
   declares `supported_extensions = "3ds|3dsx|elf|axf|cci|cxi|app"`; the dylib's own
   extension string is `3ds|3dsx|z3dsx|elf|axf|cci|zcci|cxi|zcxi|app`. `savestate = "true"`.
   `retro_get_system_info` sets `need_fullpath = true`: stage real files, never archives.
4. **The core options include the ones that matter**: `citra_is_new_3ds` (mandatory —
   the render thread is New3DS-only), `citra_graphics_api` (`Auto | Vulkan | OpenGL |
   Software` — the core ships its own `libretro_vk.cpp` Vulkan context, and `Software` is
   the insurance policy), `citra_use_hw_shader`, `citra_resolution_factor`,
   `citra_layout_option`, `citra_use_virtual_sd`, `citra_use_libretro_save_path`
   (strings dump of the dylib, defined in `src/citra_libretro/core_settings.cpp`).
5. **SD mapping is frontend-driven, therefore per-instance — the real prize.** With
   `citra_use_libretro_save_path = "LibRetro Default"`, `core_settings.cpp`'s
   `ParseStorageOptions()` takes `GetSaveDir()` (falling back to `GetSystemDir()`), appends
   `"Azahar/"`, and calls `FileUtil::SetUserPath()` — those wrap
   `RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY`/`GET_SYSTEM_DIRECTORY`, i.e. the ordinary
   `savefile_directory` / `system_directory` config keys. A per-instance `--config` therefore
   gives every agent a **private virtual SD**, which the Qt lane cannot do at any price.
   (Exact `sdmc/` subdir name: assumed standard Citra layout, confirm in the spike.)
6. **RetroArch 1.22.2 is already installed** (Git 69a4f0ea, Nov 20 2025); its `--help` here
   confirms `-c/--config`, `--appendconfig`, `-L/--libretro`, `-v`, `--log-file`,
   `-P/--play-replay`, `-R/--record-replay`, `--eof-exit`, `--max-frames=N`,
   `--max-frames-ss`, `--max-frames-ss-path`, `-e/--entryslot`, and `retroarch.cfg` already
   carries `network_cmd_enable` / `network_cmd_port = "55355"`. Three consequences:
   per-instance `--config` ⇒ per-instance port ⇒ **N concurrent instances, no lock**;
   `-P … --eof-exit` is a frontend-level deterministic replay that beats `.ctm`; and
   `--max-frames N --max-frames-ss --max-frames-ss-path` gives a bounded run with an
   end-of-run screenshot, **retiring the wid.swift/screencapture hack**. The UDP verbs live
   in `command.h` (not `command.c`) — `SCREENSHOT`, `FRAMEADVANCE`, `PAUSE_TOGGLE`, `RESET`,
   `SAVE_STATE`/`LOAD_STATE`, `GET_STATUS`, `LOAD_CONTENT`/`CLOSE_CONTENT`, `SEEK_REPLAY`,
   and **`READ_CORE_MEMORY <addr> <len>`**, which lets a test assert on emulated 3DS memory
   rather than pixels — no lane we have can do that. `retroarch --command "CMD;HOST;PORT"`
   sends them, so gdx-dev needs no socket code.
7. **Not yet verified (the go/no-go for this lane)**: (a) does the core actually render our
   `.3dsx` on macOS arm64 — no report of this core under RetroArch's MoltenVK/`glcore`
   drivers was found anywhere, the dylib mentions "SPIR-V 1.0 (under OpenGL 4.3 semantics)"
   while Apple GL stops at 4.1, so the working `citra_graphics_api` value here is unknown
   (try `Vulkan`, then `OpenGL`, then `Software`); (b) does the guest's
   `svcOutputDebugString` reach `--log-file` — the core
   implements the syscall but exports no libretro log-interface string, so the fallback is
   our own `sdmc:/3ds/gdiffuser/log.txt` (`filelog=1`), which is lane-independent and
   probably what we should parse anyway; (c) whether two instances contend on anything
   below the config layer (audio device, shader cache).

**Verdict**: the RetroArch lane is *plausible and cheap to falsify* — one hour, no build.
It is scheduled as a spike (§5 step 8), gated behind `gdx-dev retro`, and Azahar Qt stays
the default lane until the spike passes. Emulator numbers remain relative-only either way.

Sources (all seen 2026-09-07): `azahar --help` and `RetroArch --help` on this machine;
`buildbot.libretro.com/nightly/apple/osx/arm64/latest/`; `strings` over the downloaded
`azahar_libretro.dylib`; github.com/azahar-emu/azahar — `src/citra_libretro/{environment,core_settings}.cpp`,
`src/citra_qt/{citra_qt.cpp,CMakeLists.txt}`, `.github/workflows/libretro.yml`, release 2126.0
(2026-08-10); github.com/libretro/RetroArch — `command.h`, `docs/retroarch.6`;
docs.libretro.com network-control-interface (2026-03-08) and install-macos (2026-02-02).

## 4. CLI specification

`tools/dev/gdx-dev` — Python 3, stdlib only, one module per subcommand under
`tools/dev/gdxdev/`. Global flags: `--track NAME` (default: worktree basename),
`--run-root DIR` (default `/tmp/gdx-runs`), `--json`, `--quiet`, `--dry-run`.

**Run directory** `$run_root/<track>/<run_id>/`, `run_id = YYYYmmdd-HHMMSS-<slug>`:
`manifest.json`, `log.txt` (sdmc filelog snapshots), `emu.log` (the Azahar-log slice for
this run only, cut by byte offset taken at launch), `shots/`, `capture/`, `prof/`,
`console.txt`, `VERDICT`. A symlink `/tmp/<track>-art` always points at the newest run
directory, satisfying PROTOCOL.md's snapshot rule with no per-agent bookkeeping.

**`manifest.json` (schema 1)** — every field required, `null` where inapplicable:
```json
{ "schema": 1, "run_id": "...", "track": "atlas", "lane": "azahar|retro|hw",
  "started": "ISO8601", "ended": "ISO8601", "wall_s": 412.3,
  "build": { "id": "native/atlas@a1b2c3d", "dirty": true, "artifact": "…/G-Diffuser-3DS.3dsx",
             "sha256": "…", "generator": "Ninja", "configure_s": 4.1, "build_s": 61.7 },
  "ini": { "gputrace": 1, "verbose": 1, "filelog": 1, "atlas3d": 1 },
  "replay": { "id": "storm2", "sha256": "…", "kind": "gdxrec|autoinput|ctm|bsv", "events": 41 },
  "gputrace": true, "seconds": 360,
  "platform": { "host": "darwin-arm64", "azahar": "2126.0", "retroarch": null, "model": "emu-new3ds" },
  "result": { "verdict": "PASS", "fatals": 0, "max_frame": 21044, "distinct_frames": 812,
              "errors": { "gfxfail": 0, "gdl-bad": 0, "datafail": 0, "texdiag": 0 } },
  "artifacts": { "log": "log.txt", "emu_log": "emu.log", "shots": ["drive1.bmp"] } }
```
`gputrace` is promoted out of `ini` deliberately: PROTOCOL.md requires every perf claim to
state it, so no report can omit it.

**Subcommands**

- `build [--target T] [--jobs N] [--clean] [--reconfigure] [--touch-patched]` — Ninja +
  ccache configure/build wrapper. Prints build id (`GDX3DS_BUILD_ID` = `branch@hash`,
  a *configure-time* value: `port/3ds/CMakeLists.txt:59-78`), configure/build seconds,
  artifact path + sha256. Auto-reconfigures when `HEAD` ≠ the configured hash (the
  documented post-merge stale-build-id trap) and warns. `--touch-patched` touches patched
  sources and asserts each `.o` mtime advanced (the stale-object trap, PROTOCOL.md line 11).
- `emu run <3dsx> [--replay ID|FILE] [--ini k=v ...] [--seconds N] [--capture] [--shots] [--name S] [--lane azahar|retro]`
  — lock → preflight → ini merge → replay stage → log rotate + Azahar-log offset mark →
  launch → snapshot loop (20 s) → graceful stop → collect → parse → `VERDICT`.
  `--replay` resolves an id under `tools/dev/replays/` and stages it by kind: a `.gdxrec`
  (the oracle track's tick-exact recorder) to `sdmc:/3ds/gdiffuser/rec/<name>.gdxrec` plus
  `[debug] input_replay=<name>` in the merged ini; a `.txt` autoinput script to
  `sdmc/gdx-autoinput.txt`. gdxrec is preferred where it exists; the two are mutually
  exclusive (`input_replay` wins over autoinput) and `emu run` refuses to stage both.
- `emu lock [--wait S] [--hold]` / `emu unlock` / `emu status` — `--hold` daemonizes the
  ≤25 s toucher for interactive work; `status` prints owning track, age, live pid.
- `emu shot [--out F]` / `emu click X,Y` — vendored `tools/dev/mac/` swift helpers.
- `prof <log|run_id> [--sections] [--ops] [--fps] [--by-vtxn] [--ab RUN_A RUN_B] [--window N] [--json] [--fail-on-error]`
  — `--fps` from `[watchdog]` deltas ÷ 5 with p10/p50/p90; `--by-vtxn` buckets `[prof]`
  windows by `nD` (crowd `nD≥100` vs menu), the campaign's A/B convention; `--ab` prints a
  frame-aligned per-section delta table plus both runs' error counts, refusing to compare
  runs whose manifests disagree on `gputrace` or `build.id`.
- `diff <a> <b> [--mask NAME] [--sheet]` and `diff hashes <logA> <logB>` — delegates to the
  oracle track's `tools/dev/framediff.py` / `tools/dev/fhdiff.py`; exit 7 if absent.
- `sd stage <dir> [--ini k=v] [--wipe-log] [--no-eject]` / `sd pull <dest>` — `/Volumes/NO NAME`
  copy, ini merge, `rm log.txt`, `sync`, `diskutil eject`; refuses to write outside `3ds/`.
- `hw <args…>` — passthrough to the hwlink track's `tools/dev/hw.py`.
- `retro run <3dsx> …` — RetroArch lane, gated on the §5 step 8 spike.
- `runs list|show <id>|path <id>` and `report <id> [--markdown]` — the A/B table an agent
  pastes into its progress file.
- `retro`/`hw` reuse `emu run`'s flags verbatim so briefs stay lane-agnostic.

**Exit codes** (uniform across subcommands): `0` OK/PASS · `1` FAIL (assertion: `[fatal]`,
no heartbeat, frames differ) · `2` usage · `3` preflight/environment · `4` lock timeout ·
`5` staging failure (SD, ini, replay) · `6` no data parsed · `7` delegate tool missing.

## 5. Work breakdown (each step ends green and committed)

1. **Skeleton + run dirs + manifest** (`gdx-dev runs`, `--dry-run`). *Verify*: `runs list`
   on a hand-made dir; manifest round-trips through a schema check in `tools/dev/tests/`.
2. **`prof`** — parse-only, no emulator needed. *Verify*: run it over the committed
   hardware logs behind `locked60-campaign.md`'s profile table and reproduce that table's
   numbers (`br=3.2-3.8 dsp=6.5-7.3 tri=3.4-3.6`) exactly; golden-file test.
3. **`build`** — *Verify*: clean configure + incremental build both green; build id matches
   `git rev-parse --short HEAD`; deliberately merge-then-build and confirm the auto-reconfigure fires.
4. **`emu lock/unlock/status`** — *Verify*: two shells; second blocks, `status` names the
   first; kill the first ungracefully and confirm the stale steal after the threshold.
5. **`emu run`** (ini merge, replay staging, snapshot loop, verdict) — *Verify*: A/B the
   verdict against `tools/ci-3ds.sh --emu` on the same build (must agree on PASS/FAIL,
   max frame, fatal count); confirm a pre-existing foreign ini key survives the merge.
6. **`emu shot` / `emu click`** — *Verify*: a two-screen capture during a run, non-black,
   both screens present (`layout_option=0`).
7. **`sd stage`/`sd pull`, `diff`, `hw` delegation** — *Verify*: stage to a scratch dir with
   `--no-eject` and byte-compare the tree; delegation prints exit 7 when the peer tool is absent.
8. **RetroArch spike** (timeboxed 1 day, may end in "no") — install the 2126.0 release core, write
   a per-instance config (`system_directory`, `network_cmd_port`), boot the `.3dsx` with
   `-P … --eof-exit --max-frames N --max-frames-ss`. *Verify*: two instances at once, each
   producing its own `log.txt` and end screenshot, and the guest log reaching one of the two
   sinks. Ship `retro run` only if all three hold; otherwise commit the findings and stop.
9. **Adoption** — `ci-3ds.sh --emu` calls `gdx-dev emu run`; PROTOCOL.md's shell recipes
   become `gdx-dev` lines. *Verify*: CI green with `ci-3ds.sh:401-481` deleted.

## 6. Risks

- **The CLI becomes another thing to learn.** Mitigation: `--dry-run` prints the exact legacy
  command each subcommand replaces; the §2 doc refs stay in `--help`.
- **Snapshot loop races the emulator's SD writes** (torn `log.txt`). Mitigation: copy to a
  temp name then rename; keep the last N, never overwrite in place.
- **`[fatal]` grep on the shared `azahar_log.txt` catches another agent's crash.** Today's
  ci-3ds.sh has this bug latently. Mitigation: byte-offset slicing at launch (`emu.log`).
- **Lock stealing** (ci-3ds.sh steals at 900 s) can kill a legitimate long run. Mitigation:
  `emu run` heartbeats every 20 s and writes its pid+track into the lock dir; steal only if
  the pid is dead *and* stale.
- **RetroArch lane fails to render on MoltenVK/glcore** — the one genuinely unproven link
  (§3 item 7a). Contained by the timebox, with `citra_graphics_api=Software` as the fallback
  and Azahar Qt remaining the default lane regardless.
- **Scope creep into the oracle/hwlink tracks**, and **stdlib-only drift** (someone reaches
  for numpy/Pillow). Mitigation: `diff`/`hw` stay thin delegations that exit 7 and never
  implement image comparison or the 3dslink path; CI asserts `python3 -S -c "import gdxdev"`
  succeeds with no site-packages.

## 7. Estimate

Steps 1-4 ~1.5 d; step 5 (the bulk, plus lock semantics) ~1.5 d; steps 6-7 ~1 d; step 8 spike
1 d timeboxed, may end in a "no"; step 9 adoption ~0.5 d. **≈5.5 days for one agent**, or ~3 days
split (prof+build vs emu+lock are independent).

## 8. How a subagent brief references this

Briefs stop carrying shell. The stanza becomes:

> Dev loop: use `tools/dev/gdx-dev` (spec: `docs/research/native/05-devcli-plan.md`). Build
> with `gdx-dev build`. Emulator runs with `gdx-dev emu run build-3ds/port/3ds/G-Diffuser-3DS.3dsx
> --replay storm2 --ini <yourkey>=1 --seconds 360 --shots` — it takes the lock, merges the
> ini, snapshots artifacts, and prints PASS/FAIL; never launch Azahar yourself. A/B with
> `gdx-dev prof --ab <runA> <runB> --by-vtxn`. Paste `gdx-dev report <run_id> --markdown`
> into your progress file. Hardware: `gdx-dev sd stage` (or `gdx-dev hw`). If you need
> something gdx-dev cannot do, say so in your report — do not write a `/tmp` script.

## 9. First 10 commands

```sh
gdx-dev build --reconfigure                       # 1. build id + times + sha256
gdx-dev emu status                                # 2. is anyone holding Azahar?
gdx-dev emu run build-3ds/port/3ds/G-Diffuser-3DS.3dsx --seconds 45 --name boot
                                                  # 3. boot smoke, PASS/FAIL
gdx-dev runs list                                 # 4. what this track has run
gdx-dev prof <run_id> --fps --sections            # 5. fps percentiles + section table
gdx-dev emu run …/G-Diffuser-3DS.3dsx --replay storm2 --ini mylever=1 \
    --seconds 360 --shots --name on               # 6. lever ON
gdx-dev emu run …/G-Diffuser-3DS.3dsx --replay storm2 --ini mylever=0 \
    --seconds 360 --shots --name off              # 7. lever OFF (same build)
gdx-dev prof --ab <run_on> <run_off> --by-vtxn    # 8. frame-aligned A/B delta
gdx-dev diff runs/on/shots/drive1.bmp runs/off/shots/drive1.bmp --sheet
                                                  # 9. visual parity (oracle track)
gdx-dev report <run_on> --markdown                # 10. paste into the progress file
```
