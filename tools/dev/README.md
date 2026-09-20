# `tools/dev/gdx-dev` — one CLI for the fork's dev loop

Python 3, standard library only. No pip, no venv, no `PYTHONPATH`: from a fresh worktree,
`tools/dev/gdx-dev <subcommand>` just works.

**Why it exists.** Every agent on this fork used to re-derive the same procedures from
prose, write them into `/tmp/*.sh`, and lose them when the session ended — `RESUME.md` §3
records the cost ("all `/tmp` helper scripts are gone", "agents have killed each other's
runs and deleted each other's artifacts", "no committed diff tool"). Those procedures now
live here as committed code. Spec: `docs/research/native/05-devcli-plan.md`.

**If a brief tells you to run shell against Azahar, run `gdx-dev` instead.** If gdx-dev
cannot do what you need, say so in your report — do not write a `/tmp` script.

## Exit codes (uniform across every subcommand)

| code | meaning |
|---|---|
| 0 | OK / PASS |
| 1 | FAIL — an assertion failed (`[fatal]`, no heartbeat, frames differ) |
| 2 | usage error |
| 3 | preflight / environment problem (no Azahar, no `log_filter=*:Debug`, no `fzerox.o2r`) |
| 4 | lock timeout — another agent holds Azahar |
| 5 | staging failure (SD, ini, replay) |
| 6 | nothing parsed (empty or receipt-free log) |
| 7 | delegate tool missing (the `oracle` or `hwlink` track has not merged) |

## Global flags

`--track NAME` (default: worktree basename) · `--run-root DIR` (default `/tmp/gdx-runs`) ·
`--json` (machine-readable result on stdout) · `--quiet` · `--dry-run` (prints the legacy
shell the subcommand replaces and changes nothing).

`GDX_TRACK` and `GDX_RUN_ROOT` work as environment equivalents.

## Run directories

```
/tmp/gdx-runs/<track>/<YYYYmmdd-HHMMSS-slug>/
    manifest.json   schema-1, every field required (see below)
    log.txt         the guest's sdmc filelog, snapshotted every 20 s
    emu.log         THIS RUN'S slice of the shared azahar_log.txt (byte offset at launch)
    console.txt     the emulator process's own stdout/stderr
    shots/          BMPs from `SHOT <label>` events
    capture/  prof/
    VERDICT         "PASS <detail>" / "FAIL <detail>"
```

`/tmp/<track>-art` is a symlink to the newest run directory, which is what satisfies
PROTOCOL.md's "snapshot your artifacts every 25 s" with no per-agent bookkeeping.

`manifest.json` promotes **`gputrace` to a top-level field**, out of `ini`, on purpose:
PROTOCOL.md requires every perf claim to state it, so no report can quietly omit it.

---

## `gdx-dev build`

cmake + Ninja + ccache wrapper. Prints the build id, configure/build seconds, artifact
path and sha256.

```sh
tools/dev/gdx-dev build
tools/dev/gdx-dev build --reconfigure --jobs 8
tools/dev/gdx-dev build --touch-patched      # stale-object trap check
```

Two traps it owns for you:

- `GDX3DS_BUILD_ID` is a **configure-time** value (`port/3ds/CMakeLists.txt:59-78`). After
  a merge, a plain rebuild keeps the *old* id and every report you write cites the wrong
  build. `build` compares HEAD against the id baked into the generated build system and
  reconfigures automatically, with a warning.
- `--touch-patched` touches every source named by `port/3ds/patches/*.patch` and asserts
  the `.o` files actually rebuilt (PROTOCOL.md line 11).

`dirty` in the manifest ignores submodules — the 48-patch stack means they are always
modified, so counting them would mark every build dirty and the flag would say nothing.

## `gdx-dev emu run`

The whole emulator lane in one command: take the lock → preflight → merge the ini → stage
the replay → mark the log offset → launch → snapshot every 20 s → stop gracefully →
parse → write `VERDICT`. Exits 0 on PASS, 1 on FAIL.

```sh
tools/dev/gdx-dev emu run build-3ds/port/3ds/G-Diffuser-3DS.3dsx \
    --replay storm2 --ini mylever=1 --seconds 360 --shots --name lever-on
```

| flag | effect |
|---|---|
| `--replay ID\|FILE` | stage a replay from `tools/dev/replays/` (see below) |
| `--ini K=V` | merge one ini key; `[debug]` by default, `section.k=v` to target a section |
| `--seconds N` | run length (default 60) |
| `--gputrace` | set `[debug] gputrace=1` — inflates profiles, recorded in the manifest |
| `--name S` | slug for the run id |
| `--shots` | collect `autotest/*.bmp` written by `SHOT` events |
| `--lock-wait S` | how long to wait for another agent to finish (default 900) |
| `--dry-run` | print the legacy shell instead of running it |

Always sets the measurement ini set (`console=1 filelog=1 filelog_max_kb=4096
verbose=1`). `gputrace` is deliberately **not** in that set.

**The `[fatal]` scoping fix.** `tools/ci-3ds.sh` greps the whole shared `azahar_log.txt`,
so another agent's crash — or your own previous run's — fails your run. `emu run` records
the log's byte offset immediately before launch and slices `emu.log` from there, so the
verdict only ever sees this run's bytes. (If Azahar truncated the log meanwhile, the slice
restarts at 0 and says so.)

## `gdx-dev emu lock` / `unlock` / `status`

```sh
tools/dev/gdx-dev emu status              # who holds Azahar, and is one running?
tools/dev/gdx-dev emu lock --hold         # daemonised toucher for interactive work
tools/dev/gdx-dev emu unlock
```

`emu run` takes the lock itself — you only need these for interactive driving.

The lock is `/tmp/azahar.lock` (PROTOCOL.md), and it protects the **shared user-data
directory**: Azahar has no single-instance lock, but there is exactly one
`~/Library/Application Support/Azahar` and no flag to move it. gdx-dev writes
`owner.json` (track, pid, start time) into the lock and heartbeats it every 20 s, so it
steals a lock only when the owning process is **dead** *and* stale — unlike
`ci-3ds.sh`, which steals on age alone and can kill a legitimate long run. A lock with no
`owner.json` is a legacy bash holder and gets ci-3ds.sh's conservative 900 s threshold.

## `gdx-dev emu shot` / `emu click`

```sh
tools/dev/gdx-dev emu shot --out /tmp/devcli-art/title.png
tools/dev/gdx-dev emu click 640,520
```

Azahar has no `--screenshot` flag (confirmed absent from `azahar --help` on 2126.0), so
capture is host-side: `tools/dev/mac/wid.swift` finds the `CGWindowID`, `screencapture -l`
grabs it. Both screens land in one image only with `layout_option=0` — `shot` checks and
warns. `click` drives the host-side bottom-screen touch menu, which guest autoinput
cannot reach. Both helpers are vendored under `tools/dev/mac/` because the `/tmp` copies
have been lost at least three times.

## `gdx-dev prof`

Every table agents used to hand-assemble with grep and awk.

```sh
tools/dev/gdx-dev prof <run_id>                         # fps + sections + ops + errors
tools/dev/gdx-dev prof /path/to/log.txt --fps
tools/dev/gdx-dev prof <run_id> --by-vtxn               # crowd vs menu buckets
tools/dev/gdx-dev prof --ab <run_on> <run_off> --by-vtxn
tools/dev/gdx-dev prof <run_id> --fail-on-error         # exit 1 on any error receipt
```

- `--fps` — p10/p50/p90 from `[watchdog] frame=N(+D)` deltas ÷ 5 s
  (`docs/3DS-HARDWARE.md:177-180`). The **first beat is dropped**: its `(+N)` covers boot
  and asset load, not a frame rate, and including it drags every percentile down.
  Percentiles are nearest-rank, so a printed p50 is a value that actually occurred.
- `--sections` — mean ms/frame per `[prof]` section, with `[prof!]` storm windows on
  their own row.
- `--by-vtxn` — buckets `[prof]` windows by **`nD`** (draws/frame), the campaign's A/B
  convention: `crowd` is `nD>=100`, `menu` is `nD<16`. (The flag keeps the `--by-vtxn`
  name from the spec; mean `vtxN` is printed per bucket alongside.)
- `--ops` — `[profop]` top opcode handlers, mean ms/frame and calls/frame.
- `--ab A B` — frame-aligned delta table plus both runs' error counts. **Refuses** to
  compare runs whose manifests disagree on `gputrace` (it inflates profiles) or on
  `build.id` (a lever A/B must use one build); `--force` overrides the build check.

Parsed receipts: `[prof]` `[prof!]` `[profop]` `[gpu]` `[fill]` `[rt]` `[watchdog]`, and
the error census `[fatal] [fatal-trace] [gfxfail] [gdl-bad] [gdl-miss] [datafail]
[texdiag] bad_alloc`. Parsing is a tolerant `key=value` scan, not a positional regex, so
an added column does not break it.

## `gdx-dev runs` / `gdx-dev report`

```sh
tools/dev/gdx-dev runs list
tools/dev/gdx-dev runs show <run_id>
cd "$(tools/dev/gdx-dev runs path <run_id>)"
tools/dev/gdx-dev report <run_id> --markdown >> docs/research/native/<track>-progress.md
```

`report --markdown` prints the block to paste into a progress file: build id, gputrace
state, replay, ini, verdict, fps percentiles and the section table — everything a claim
needs to cite under PROTOCOL.md. With no run id it uses the newest run.

## `gdx-dev sd stage` / `sd pull`

The physical SD-card loop. **Dry-run by default** — it prints the plan and writes nothing
until `--apply`, because this is the user's real card.

```sh
tools/dev/gdx-dev sd stage build-3ds/port/3ds/G-Diffuser-3DS.3dsx           # dry run
tools/dev/gdx-dev sd stage ~/Desktop/stage-dir --ini mylever=1 --wipe-log --apply
tools/dev/gdx-dev sd pull /tmp/devcli-art/hw-run
```

Guards: every destination is forced under `3ds/` on the card, and the card's
`gdiffuser.ini` is **merged**, never rewritten, so the user's own keys (`border_mode`,
`deadzone`, stereo) survive. Ejects with `diskutil` unless `--no-eject`.

## `gdx-dev diff` / `gdx-dev hw` — delegations

These are thin on purpose and must stay that way: they never implement image comparison
or the 3dslink path.

```sh
tools/dev/gdx-dev diff a.bmp b.bmp --sheet      # -> tools/dev/framediff.py   (oracle)
tools/dev/gdx-dev diff hashes runA.log runB.log # -> tools/dev/fhdiff.py      (oracle)
tools/dev/gdx-dev hw push --ip 192.168.1.50     # -> tools/dev/hw.py          (hwlink)
```

If the peer track has not merged into your branch, they exit **7** and name the track and
the plan section to ask for. That is the signal to merge or to ask — not to reimplement.

## `gdx-dev retro` — the RetroArch lane

Opt-in second lane whose point is that it has **no singleton**: each instance gets its own
config, its own private virtual SD, its own log and its own UDP port, so N agents can run
at once. See `docs/research/native/devcli-progress.md` for the spike verdict and the exact
working configuration; Azahar Qt remains the default lane.

```sh
tools/dev/gdx-dev retro run build-3ds/port/3ds/G-Diffuser-3DS.3dsx --seconds 300 --replay storm2
tools/dev/gdx-dev retro status            # who owns which instance; flags orphans
tools/dev/gdx-dev retro reap              # kill RetroArch whose driver is gone
tools/dev/gdx-dev retro reap --prune      # also delete idle instance dirs >2 days old
tools/dev/gdx-dev retro reap --prune --days 7 --dry-run
tools/dev/gdx-dev retro setup --instance a
tools/dev/gdx-dev retro cmd SCREENSHOT --instance a
```

### Renderer (`--gfx`)

`--gfx vulkan|opengl|software`, case-insensitive. With no flag the lane tries Vulkan, then
OpenGL, then Software.

**Recommendation: leave it alone — the default (Vulkan first) is the stable one here.**
Measured 2026-09-13, five consecutive 60 s boots per renderer on clean instances:

| renderer | runs PASS | boot attempts | early crashes |
|---|---|---|---|
| Vulkan | 4/5 | 5 | **0 (0%)** |
| OpenGL | 5/5 | 7 | 2 (29%) |

Vulkan did not crash once; OpenGL crashed on 2 of 7 boots and only reached 5/5 because the
retry rescued it. Both reach the same guest frame (~4600) in 60 s, so there is no speed
argument either way. Vulkan's single FAIL was a boot that produced no guest log at all —
now retried like any other early crash. Small sample, on a machine running other tracks'
emulators, so treat it as "no reason to pin OpenGL", not as a precise crash rate.

The azahar-libretro core intermittently dies during boot (SIGBUS/SIGSEGV, independent of
the build under test). `retro run`
therefore treats "the guest heartbeat never advanced" as an **early crash** and retries the
same renderer up to 3 attempts total before falling back. Every attempt is recorded in the
manifest's `attempts` array, so a flaky boot is visible afterwards rather than only in the
console. A single `[watchdog]` beat does not count as booted — the crash lands after the
first beat.

**A fresh instance does not inherit another agent's experiment.** The instance ini is
seeded from the shared Azahar Qt SD so real preferences carry over (`[display]`,
`[stereo]`, `[input]`, `[net]`, `[menu]`, `[audio]`), but **every key in `[debug]` and
`[native]` is stripped**. Those two sections are levers and run-control knobs belonging to
whoever used the Qt SD last, and gdx-dev can set any of them itself with `--ini`, so
inheriting them means running a configuration nobody asked for. The rule is by section
rather than by a key list, because a key list has to be chased every time another track
invents a lever — `trectbatch` was missed exactly that way. Measured on this machine, the
shared ini carried `quit_at_tick = 3400` (every seeded run would have stopped at tick
3400), plus `input_replay`, `trectbatch`, `tmemfast`, `trifast` and a whole `[native]`
block: 33 inherited keys. Both lanes then set the same measurement ini explicitly.

`retro run` also sets `ApplePersistenceIgnoreState` on the RetroArch bundle before every
launch. After an abnormal exit macOS otherwise offers to restore the previous session with
an NSAlert that has no window to attach to, so it is invisible and the process hangs
forever — which is what happened to several runs after the orphan-kill testing.

**Omit `--instance` and you get a unique one** (named after the run id), with its own
config, private SD and UDP port. Pass `--instance NAME` only when you deliberately want
to reuse one; `retro run` then refuses to start if a live driver already owns that name,
and tells you which pid, track and run dir hold it (exit 4).

The instance is claimed with an O_EXCL pid file at `<instance>/driver.json`. The driver
kills its RetroArch on every exit path it can observe — `atexit` plus SIGTERM/SIGINT/
SIGHUP — and the emulator is started with `start_new_session=True` so one `killpg` takes
down it and anything it spawned. SIGKILL cannot be caught, which is what `reap` is for:
it kills RetroArch processes that were reparented to pid 1, or whose pid file names a
dead driver, and clears their locks.

`--prune` deletes instance directories that are idle and older than `--days` (default 2).
Unique-per-run instances accumulate one directory each — 74 of them, 852 MB, within a few
days here — so this is how the private-SD win stops eating the disk. It never touches a
directory with a running RetroArch or a live driver lock. Pruning a *named* instance is
safe: the private SD is re-seeded on the next run.

Age comes from `retroarch.cfg`, which gdx-dev rewrites at every launch — **not** from the
newest file in the tree. `seed_sd` copies the user's `dspfirm.cdc`, which is dated 2035 on
this machine, so a newest-file heuristic put every instance nine years in the future and
pruned nothing at any threshold.

`reap` requires **positive** evidence that a driver is gone. A process with a live parent
and no pid file is left alone — several tracks share this machine, and "no lock" means
"started before this feature", not "orphan".

## Replays

`tools/dev/replays/` — see its own README for the grammar and the three rules that cost
real time (interleave `SHOT` lines among the button lines; drive SHOTs belong at tick
4600+; keep every line under 62 bytes, comments included). `emu run --replay <id>`
validates the script before staging, so a typo — or a long comment — fails immediately
instead of producing a run that silently did nothing.

`.gdxrec` files (the `oracle` track's tick-exact recorder) are staged to
**`sdmc:/rec/<name>.gdxrec`** — the SD **root**, not under `3ds/gdiffuser/` — with
`[debug] input_replay=<name>`, which wins over autoinput. The two are mutually exclusive.
The guest builds the relative path `rec/<name>.gdxrec` and a `.3dsx` runs with the SD root
as its cwd (`port/3ds/main_3ds.cpp:794`), which is why the root is the only place that
works; `tools/dev/oracle-run.sh:129` stages the same way.

Replay ids resolve from **both** `tools/dev/replays/` (autoinput scripts) and
`<repo>/replays/` (the oracle track's `.gdxrec` recordings).

**A replay that does not RUN TO THE END fails the run.** A guest core that dies mid-replay
still leaves an advancing heartbeat and zero `[fatal]` lines, so the heartbeat alone is not
evidence of a good run. When a replay is staged, both lanes additionally require:

| kind | completion evidence |
|---|---|
| `.gdxrec` | the `[rec] end` receipt **and** every scheduled `SHOT` captured |
| autoinput | the last scheduled tick reached **and** every scheduled `SHOT` captured |
| either | the emulator still alive at the end of `--seconds`, unless it exited via `replay_quit` / `quit_at_tick` |

A run stopped deliberately by `[debug] quit_at_tick` counts as complete once the tick is
reached (the `[oracle] quit requested: [debug] quit_at_tick reached` receipt only exists
because it was), and SHOTs scheduled *after* that tick are dropped from the expectation
rather than failing a run that did exactly what it was told.

**Both lanes return as soon as the guest is finished**, rather than idling out the rest of
`--seconds`. The emulator routinely outlives the guest — a `.3dsx` that exits leaves
Azahar up with a dead window — so the loop watches the guest log for
`[fatal-trace] atexit:` or `[oracle] quit requested` instead of only the process. Measured:
a `quit_at_tick` run set to `--seconds 240` now returns in 24 s on the retro lane and 168 s
on the Qt lane.

SHOTs are counted by label from `[fh] … tag=<label>` and `[autotest] SHOT captured ->`,
ignoring the periodic `tag=auto` captures. Staging a `.gdxrec` that carries SHOTs also
arms `[debug] fh=1`, because the capture path is gated on it and would otherwise log
"capture request ignored" and silently capture nothing.

Every run writes a one-line `RESULT` file (also printed):

```
RESULT FAIL frame=4969 fatals=0 replay=race-mutecity-start(gdxrec) complete=False shots=5/8 wall_s=57.3 sha256=2be07093d4b47553
```

It quotes the **artifact sha256**, not just the build id: `GDX3DS_BUILD_ID` is a
configure-time value, so after a merge-then-rebuild it names the wrong commit and can lie
about what actually ran. `replay_complete`, `shots_expected`, `shots_seen`,
`missing_shots` and `wall_s` are all in `manifest.json` too.

**A replay that does not load at all fails the run.** Both lanes check the guest log for the
receipt — `[rec] replay armed` for a `.gdxrec`, `autoinput script loaded:` for a script —
and fail on `[rec] replay open failed`, on a missing receipt, or on an autoinput script
that fell back to the seconds timebase. Before this check, a mis-staged replay booted
normally, never replayed, and still reported PASS.

## Tests

```sh
python3 tools/dev/tests/test_gdxdev.py
```

Stdlib `unittest`. Covers the manifest schema, the ini merge preserving a foreign agent's
key, autoinput grammar rejection, the log parser against a golden fixture, the `[fatal]`
scoping fix, delegate exit 7, and an import under `python3 -S` (no site-packages) to
catch stdlib-only drift.
