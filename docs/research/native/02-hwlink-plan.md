# 02 — Hardware link (track `hwlink`): design + work breakdown

Date: 2026-09-07. Planning only, no implementation. Basis: `00-master-plan.md` §3, §4.1, §8;
the mainline tree at `native/main`; libctru 2.7.0 headers in `/opt/devkitpro/libctru/include`;
3ds-hbmenu `master` (`source/ui/netloader.c`, `source/loaders/rosalina.c`, `source/launch.c`,
`source/menu.h`); Luma3DS `master` (`sysmodules/rosalina/source/input_redirection.c`).
Every external fact below was re-verified against those sources today; citations inline.

## 1. Goal

Turn the hardware loop from "user carries an SD card" into: `tools/dev/hw.py push` builds,
pushes, and runs a build on the console; logs stream back live; the next build reloads with
no human touch; inputs can be driven from the host; a crash yields a backtrace. One human
action per power-on (hbmenu Y), two Rosalina toggles, then hands off.

## 2. Current state (verified, file:line)

- No networking anywhere: `soc:U` sits unused in the CIA ACL (`port/3ds/packaging/gdiffuser.rsf:219`
  service list). No `socInit`, no `3dslink.h` include in the tree.
- Logging: `logStep`/`aptLogReceipt`/watchdog write through `svcOutputDebugString` plus
  `gdx3ds_filelog_write` (`port/3ds/main_3ds.cpp:236-246`, `:269-275`, `:205-215`). The sink is
  a static 64 KiB ring behind a `RecursiveLock`, batched to `sdmc:/3ds/gdiffuser/log.txt`
  (`port/3ds/gdx3ds_filelog.c:34-45`, `:68-95`). One extra sink hook, no heap, no blocking.
- Threads/priorities: main+game 0x30 core 0; audio drain and HLE producer 0x18 with a core
  ladder 2→1→0 (`port/3ds/audio/gdx3ds_audio_ndsp.c:94-95`, `:660-676`, `:999-1000`,
  `:1033-1040`); render thread 0x24 core 2, 192 KiB stack (`port/3ds/gdx3ds_renderthread.cpp:59-61`,
  `:312`); watchdog 0x18 detached, core -2 = exheader default (`port/3ds/main_3ds.cpp:982`).
  libctru range is [0x18;0x3F], core 1 needs `APT_SetAppCpuTimeLimit` and holds one thread only.
- APT: `aptMainLoop()` is pumped as the first statement of the frame loop after a render-thread
  join (`port/3ds/main_3ds.cpp:1028-1050`); `aptLifecycleHook` handles SUSPEND/RESTORE/SLEEP/
  WAKEUP/EXIT with flushed receipts (`:348-380`).
- Exit chain: loop break → `[exit] cause=…` → save flush → audio stop → audio shutdown →
  render join → window shutdown → fs shutdown → filelog flush (`port/3ds/main_3ds.cpp:1255-1290`).
  A clean programmatic exit already exists: `gdx_request_quit()` sets the flag the loop
  condition reads (`port/3ds/lus_glue/gdx3ds_input_glue.c:65-72`, `main_3ds.cpp:1028`).
- Input: `gdx3ds_os_poll_input` = `hidScanInput`/`hidKeysHeld`/`hidCircleRead`
  (`port/3ds/os/gdx3ds_os_ctru.c:382-424`); the per-frame override hook is
  `gdx_input_script_override(GdxInputPad*)` (`port/gdx_input_script.h:29`), applied once per
  `gdx_controller_poll()` after the pad read (`port/input_bridge.c:707-719`); tick-scheduled
  autoinput at `port/input_bridge.c:208-380`.
- Config: 128 entries max, 24/32/64-char section/key/value caps
  (`port/3ds/os/gdx3ds_config.c:14-18`); `gdx3ds_config_set_string/_set_int/_save` already
  exported (`port/3ds/os/gdx3ds_config.h:73-75`).
- Paths: every data path is an absolute literal under `sdmc:/3ds/gdiffuser/`
  (`port/3ds/assets/gdx3ds_fs_sd.c:57-70`, `gdx3ds_config.h:50`, `gdx3ds_filelog.c:34`);
  nothing resolves relative to `argv[0]` (its only uses are desktop, `port/main.cpp:640`, `:705`).

**Packaging question, answered:** hbmenu's netloader writes to `"sdmc:/3ds/" + <basename>`
(3ds-hbmenu `source/ui/netloader.c:334-338`) and launches from there. Since every data path is
an absolute `sdmc:/3ds/gdiffuser/…`, a 3dsx at `sdmc:/3ds/G-Diffuser-3DS.3dsx` finds its
archives, ini, saves and log exactly as the SD install does — the netloader route and the
documented layout (`docs/3DS-HARDWARE.md` §3) coincide, no change needed. Keep the pushed
basename stable so reload targets one file, not a litter.

## 3. Design — app side, `port/3ds/net/gdx3ds_net.c`

New TU, one killswitch: `[net] enabled = 0` (default OFF; dev builds set it in the ini).
Receipts on every path, prefix `[net]`, through the existing filelog sink.

### 3.1 Bring-up

```
SOC buffer: memalign(0x1000, 0x100000)  → socInit(buf, 0x100000)   (soc.h:105)
host      : __3dslink_host (3dslink.h:13), set by libctru's argv parser from the
            "<hex-ip>_3DSLINK_" argument the netloader appends (netloader.c:378-383;
            "_3DSLINK_" confirmed present in libctru.a)
log link  : our own TCP connect to __3dslink_host:17491 (LINK3DS_COMM_PORT). NOT
            link3dsStdio(): that redirects process stdout/stderr, so any printf on the
            game thread would block on a stalled socket. We own the fd and write only
            from the net thread. [net] stdio=1 opts into link3dsStdio() if ever needed.
control   : TCP listen on 0.0.0.0:17492, one client, backlog 1
input     : host-side only — it drives Rosalina's UDP 4950; the app does not listen.
```

Bring-up goes after `gdx3ds_filelog_init()` and before `gdx3ds_os_window_init()` in
`main()` so boot lines reach the host; failures degrade to "no link", never abort.

### 3.2 Control protocol (TCP 17492, line header + binary payload)

Text command line, `\n`-terminated, ASCII; replies are `OK …` / `ERR <reason>` lines.

| Command | Payload | Reply | Effect |
|---|---|---|---|
| `PING` | — | `OK PONG <build-id> <frame> <uptime_ms>` | liveness + build identity (`GDX3DS_BUILD_ID`, `port/3ds/CMakeLists.txt:77`) |
| `RELOAD <size> <crc32>` | `<size>` raw bytes | `OK RELOAD` then link closes | see §3.3 |
| `CAPTURE <label>` | — | `OK CAPTURE armed` | calls the oracle track's arm hook (`gdx_request_frame_dump`-shaped); the image/hash returns over the log link as `[fh] …` / a `CAP <label> <len>` framed blob |
| `INI <section> <key> <value>` | — | `OK INI` | `gdx3ds_config_set_string` + optional `SAVE` |
| `INI SAVE` | — | `OK INI saved` | `gdx3ds_config_save(GDX3DS_CONFIG_DEFAULT_PATH)` |
| `QUIT` | — | `OK QUIT` | `gdx_request_quit()` → normal `[exit]` chain |

Rules: one command in flight; 4 s recv timeout per fragment; any framing error closes the
connection (the host reconnects). Commands are parsed on the net thread but **executed on
the main thread**: the net thread sets a request word, the frame loop drains it next
iteration (same pattern as `sHeapTriggerPending`, `main_3ds.cpp:159`, `:1230-1240`). This
keeps `gdx3ds_config_set_*`, the capture arm and the quit flag single-threaded.

### 3.3 RELOAD (the highest-leverage item)

1. Net thread streams the payload to `sdmc:/3ds/G-Diffuser-3DS.3dsx.tmp` in 64 KiB static
   chunks (never buffer the whole ~3.0 MB image; measured size of the current 3dsx).
2. CRC32 check, then `rename()` over `sdmc:/3ds/G-Diffuser-3DS.3dsx` — a truncated transfer
   can never leave an unbootable target.
3. Post a `RELOAD` request to the main thread. It calls `gdx_request_quit()`; the loop
   unwinds through the existing teardown (`main_3ds.cpp:1255-1290`), and **after**
   `gdx3ds_fs_shutdown()` (last flush done) we do:
   `svcConnectToPort(&h, "hb:ldr")`; `HBLDR_SetTarget("/3ds/G-Diffuser-3DS.3dsx")` —
   IPC header `0x20002`, static buffer 0, path with the `sdmc:` prefix stripped;
   `HBLDR_SetArgv(buf, 0x400)` — header `0x30002`, static buffer 1, where `buf[0]` = argc
   and NUL-separated strings follow from `buf+4` (hbmenu `loaders/rosalina.c:10-49`,
   `launch.c:38-46`, `menu.h:22-26`, `ENTRY_ARGBUFSIZE 0x400`). Re-emit our own
   `"<hex-ip>_3DSLINK_"` argument so the new process inherits the host address.
   Then return from `main()`; Luma relaunches the target on process exit.
4. If `hb:ldr` is unavailable (non-Luma loader), log `[net] hbldr unavailable rc=…` and exit
   normally — the user re-presses A in hbmenu. Graceful, never fatal.

### 3.4 Threading and priority

One thread: `threadCreate(netThreadMain, …, 32 KiB, 0x3E, 0, /*detached=*/false)`.

Justification: core 0 (appcore) is always available, needs no `APT_SetAppCpuTimeLimit`
grant (core 1's single slot is already contested by the audio ladder,
`gdx3ds_audio_ndsp.c:660-676`), and priority 0x3E is below the 0x30 main/game thread, so
the net thread only runs when main is blocked (it blocks every frame on the vblank pace).
Core 2 is rejected: the render thread (0x24) and often the audio drain (0x18) live there,
and a socket thread there would compete for exactly the core the fork/join model measures.
The thread does `poll()` with a 50 ms timeout, so its steady-state cost is ~20 wakeups/s of
a few microseconds at the lowest app priority.

Lock discipline (the real hazard): **no game-thread code may ever call `send()`**. Log
mirroring goes through a static 32 KiB SPSC ring: `gdx3ds_net_log_write()` is called from
inside `gdx3ds_filelog_write` (already under its recursive lock, already no-alloc), copies
bytes into the ring, and returns; the net thread drains and sends. Ring full ⇒ drop and
count, reported as `[net] dropped=N` on the next drain — exactly the filelog's own
policy (`gdx3ds_filelog.c:82-92`). Nothing on the frame path can block on the network.

### 3.5 Sleep / HOME / APT interaction

- SOC sockets do **not** survive a suspend cleanly. On `APTHOOK_ONSLEEP` / `ONSUSPEND` the
  hook sets `sNetSuspended = 1` (after the existing render-park and audio-park steps); the
  net thread stops sending and drops both connections. On `ONWAKEUP` / `ONRESTORE` it clears
  the flag and re-runs the connect path (log link reconnect + listener rebind), logging
  `[net] relink rc=…`. Rosalina's own redirection thread does the same dance
  (`input_redirection.c:126-131`: it waits for `Wifi__IsConnected()` after `Sleep__Status()`).
- Practical consequence for the user card: **lid open, auto-sleep off, on the charger.**
- `ONEXIT` closes sockets before `socExit()`; `socExit()` runs from an `atexit` handler
  registered after `socInit`, mirroring the libctru example
  (`/opt/devkitpro/examples/3ds/network/3dslink-demo/source/3dslink-demo.c:54-67`).

### 3.6 Memory and thread budget

| Item | Cost | Note |
|---|---|---|
| SOC shared buffer | **1 MiB** committed, `memalign(0x1000, 0x100000)` | mapped away to the soc module for its lifetime; the standard size in every libctru example |
| log ring + rx staging + line buffers | 32 KiB + 64 KiB + ~2 KiB (static) | no heap after init |
| net thread stack | 32 KiB | |
| **Total** | **~1.13 MiB** | against ~40-50 MB of New3DS headroom at the ~80 MB static center (`docs/research/3ds-memory-budget.md` §2-3) |

Thread map after this track: 0x18 audio drain + producer + watchdog, 0x24 render (core 2),
0x30 main/game (core 0), **0x3E net (core 0)**. Nothing preempts an existing thread.

## 4. Design — host side, `tools/dev/hw.py`

Single Python file, stdlib only, subcommands:

- `hw.py push [--build] [--ip A.B.C.D]` — optional cmake build, then `3dslink -a <ip> -s
  <3dsx>` (netloader: TCP 17491, deflate, `netloader.c:168-243`), then attach the log tail.
  `--ip` defaults to `[hw] ip` in a gitignored `tools/dev/hw.ini` (no personal data committed).
- `hw.py log` — TCP server on 17491; timestamps, tees to `artifacts/hw-<ts>.log`, summarises
  `[fh] `, `[prof]`, `[watchdog]`, `[fatal` lines.
- `hw.py reload <3dsx>` — connect 17492, `RELOAD <size> <crc32>` + bytes, wait for the link
  to drop, then for the new process's `[net] link up build=…` with a changed build id.
  ~3.0 MB over 3DS WiFi is a 2-5 s round trip; deflate is a later optimisation.
- `hw.py input <replay.txt>` — UDP driver for Rosalina InputRedirection, port 4950. Packet is
  exactly 20 bytes, five little-endian u32 (`input_redirection.c:117`, `:136-160`):
  `[0] hid` (12 button bits, **active-low**: neutral `0x00000FFF`, clear a bit to press —
  A0 B1 SELECT2 START3 RIGHT4 LEFT5 UP6 DOWN7 R8 L9 X10 Y11), `[1] touch` (neutral
  `0x02000000`), `[2] circle pad` = `(y<<12)|x`, 12 bits per axis, neutral `0x7FF` each
  (packet neutral `0x007FF7FF`), `[3] C-stick/IR` (neutral `0x80800081`), `[4] special`
  (bit0 HOME, bit1 POWER, bit2 POWER-long). Rosalina copies `buf[0..11]` into HID shared
  memory unconditionally and `buf[12..19]` only when `n >= 20`, so always send 20.
- `hw.py gdb [--core]` — writes a batch script and runs
  `arm-none-eabi-gdb -batch -x <script> <elf>` against `target extended-remote <ip>:4000`
  (Rosalina GDB stub), printing `bt`, `info threads`, `thread apply all bt`.

**Replay format — coordination note for the oracle track.** InputRedirection is a free-running
UDP patch of HID shared memory: it has no frame clock, so it **cannot** produce a deterministic
replay. Divide the two jobs explicitly:

- *Exploration / manual driving / menu navigation*: `hw.py input` over UDP 4950. Send at
  ~60 Hz, repeat each state 3× on a transition (no ACK, packets are lost).
- *Deterministic parity replays*: app side, through `gdx_input_script_override()`
  (`port/gdx_input_script.h:29`) — one pad state consumed per `gdx_controller_poll()`.
  The recorder's on-device line and the replay file share one record:
  `<tick> <buttons-hex> <stickX> <stickY>` (tick = the poll counter, buttons = the N64
  OSContPad mask the port already speaks, sticks = -80..80, exactly `GdxInputPad`).
  `hw.py` ships a replay file with `push`/`reload` (write it to `sdmc:/3ds/gdiffuser/`
  via a later `PUT` command, or stage it on the card); the oracle track owns the
  recorder, the file name and the `[rec]` line format. **Both tracks must agree on this
  record before either lands code** — flagged as the one cross-track dependency.

## 5. User setup card (one page, ship as `docs/HW-DEV-LOOP.md`)

1. Console: New 3DS/2DS with Luma3DS ≥ 10.1.1 (13.x recommended — the Rosalina menu items
   below are 10.x+). Same requirement as `docs/3DS-HARDWARE.md` §1.
2. Put it on the charger. **System Settings → Power Saving / auto-sleep OFF, lid open.**
   Sleep drops sockets (§3.5). Screen dim is fine.
3. WiFi connected to the same LAN/subnet as the Mac. Find the IP: hbmenu shows it on the
   netloader screen, or System Settings → Internet Settings → Connection → status.
4. Boot the Homebrew Launcher. Press **Y** once per power-on → "waiting for 3dslink".
5. Rosalina menu (**L + Down + Select**) → *Miscellaneous options* → **Enable input
   redirection** (needed for `hw.py input`); → *Debugger options* → **Enable debugger**
   (needed for `hw.py gdb`; leave it off if you are not debugging, and turn it off before
   powering down).
6. Tell the agent the IP. From then on: `hw.py push` for the first build, `hw.py reload`
   for every build after — no console interaction unless it powers off or sleeps.
7. If the console reboots or you power-cycle it: repeat 4 and 5.

## 6. Failure handling

- **WiFi drop** looks like: log link goes silent mid-line; the control socket's next `send`
  fails with `EPIPE`/`ETIMEDOUT` after the SOC stack's own timeout (seconds); Rosalina's UDP
  input simply stops updating, so the pad **freezes in its last state** — the driver must send
  an explicit all-neutral packet on shutdown and on every reconnect, or the game keeps
  accelerating. App side: net thread closes both fds and retries `connect()` every 2 s,
  logging `[net] relink attempt=N rc=…`; the filelog ring keeps the missed lines up to 32 KiB
  and they are re-sent on reconnect (the ring holds bytes, not a promise).
- **Watchdog**: the existing 0x18 beat (`main_3ds.cpp:183-236`) gains one field,
  `net=<state>` (0 down, 1 log-only, 2 full), so a silent host is distinguishable from a
  wedged game. `sWatchdogQuiet` already covers teardown; RELOAD uses the same path.
- **Host retries**: `hw.py reload` retries twice, then tells the user to press A in hbmenu;
  `hw.py log` is a server and reconnects forever.
- **RELOAD loop guard**: if the new build fails to announce `[net] link up` within 40 s,
  `hw.py` reports the previous build's log tail and refuses to push again until told to,
  so a boot-crashing build cannot be reloaded in a loop.
- **`hb:ldr` missing** (Luma too old, or the entry point is not Rosalina's loader): §3.3
  step 4, graceful.

## 7. Work breakdown (with verification per step)

Steps 1-3 are emulator-verifiable (Azahar has no useful network stack for netloader, but
compile + boot + killswitch-off no-op is checkable); **4-9 can only be verified on hardware.**

| # | Step | Verification |
|---|---|---|
| 1 | `gdx3ds_net.c` skeleton: `[net] enabled`, SOC init/exit, `atexit`, `[net]` receipts, no sockets used | Build; Azahar boot with `enabled=0` → zero new log lines, byte-identical behaviour; `enabled=1` → `[net] soc rc=0x0` then idle. Heap census delta ≈ 1 MiB (`[mem-census]`, `main_3ds.cpp:697-711`) |
| 2 | Log link: SPSC ring + net thread + connect to `__3dslink_host:17491` | Host `nc -l 17491` against Azahar cannot be reached → verify by unit-testing the ring on host (fill/drain/drop counter) and by the `[net] link down` receipt |
| 3 | Control listener + `PING`/`INI`/`QUIT`, main-thread request drain | Azahar: no listener reachable, so verify `QUIT` by wiring the same request word to a debug key; confirm the `[exit]` chain receipts are unchanged |
| 4 | **HW**: `3dslink -a … -s` push, live log | TEST-PLAN A (below) |
| 5 | **HW**: `RELOAD` + `hb:ldr` self-relaunch | TEST-PLAN B — the master plan's day-one risk (§8) |
| 6 | **HW**: `hw.py input` UDP driver, neutral-on-exit | TEST-PLAN C |
| 7 | **HW**: sleep/HOME relink, WiFi-drop retry | TEST-PLAN D |
| 8 | **HW**: `hw.py gdb` batch backtrace | TEST-PLAN E |
| 9 | `CAPTURE` handoff to the oracle track's arm hook + framed blob transport | Joint with oracle track: one `[fh]` line per armed frame matches the same replay on Azahar |
| 10 | `docs/HW-DEV-LOOP.md` (§5) + `native/docs/research/native/hwlink-progress.md` | Doc review |

### TEST-PLAN card (hand to the user with the staged build)

```
Setup: card §5 steps 1-5 done, console IP written down.
A. push      Mac: tools/dev/hw.py push --ip <IP>
             PASS: hbmenu leaves the netloader screen, game boots, the Mac terminal
             prints the boot logSteps ("gdx3ds_config_load" … "entering frame loop")
             and a [watchdog] beat every 5 s.  Report: first 40 lines + any [net] line.
B. reload    Mac: tools/dev/hw.py reload <new .3dsx>  (game running from A)
             PASS: game exits, console relaunches on its own within ~10 s, the new
             build id appears in "[net] link up build=<branch>@<hash>".
             FAIL modes to report verbatim: "[net] hbldr unavailable rc=…", a hang after
             "[exit] fs_shutdown done", or a return to hbmenu instead of a relaunch.
C. input     Mac: tools/dev/hw.py input --demo   (presses A, then holds circle-pad right 1 s)
             PASS: the console reacts; on Ctrl-C the pad returns to neutral (no stuck input).
             Report whether L/R/X/Y and the circle pad all respond.
D. sleep     Close the lid for 10 s, reopen.  PASS: log resumes within ~5 s with
             "[apt] wakeup" then "[net] relink … rc=0x0"; game still runs; a following
             `hw.py reload` still works.  Then: turn WiFi off at the router for 20 s and back
             on — report what the console log shows and whether the game kept 60 fps.
E. gdb       Mac: tools/dev/hw.py gdb    (with Rosalina debugger enabled)
             PASS: a backtrace prints. Report the first 20 lines.
Also report: any frame-rate change during A-E vs a normal SD-card run (this is the
"does the net thread cost anything" question).
```

## 8. Risks

- `hb:ldr` self-relaunch from a running 3dsx is untested by us (hbmenu does exactly this, so
  the mechanism is sound; the unknown is doing it from a process that held GSP/DSP/SOC).
  Mitigation: relaunch strictly after the full teardown chain, `socExit` included — step 5 is
  the day-one gate.
- Sockets across suspend may hang a `send` rather than error. Mitigate with `SO_SNDTIMEO` on
  the log fd and the suspend flag set from the APT hook before the OS transition runs.
- Input redirection patches HID shared memory globally (it drives the HOME menu too) and a
  stuck stream outlives our process: neutral-on-exit is mandatory.
- Priority 0x3E on core 0 could starve if the game thread never blocks; the vblank pace makes
  that unlikely, and if logs lag the fix is a bigger ring, not a higher priority.
- Netloader uses whatever basename `3dslink` sends; a differently-named push leaves a second
  file and reload then targets the wrong path. `hw.py` pins the basename.

## 9. Estimate

App skeleton + log link + control: ~1.5 agent days. `hw.py`: ~1 day. Hardware bring-up
(steps 4-8, incl. user round trips): ~1-1.5 days wall clock. Capture handoff: ~0.5 day,
gated on the oracle track. Docs: 0.25 day. **~4-5 agent days**, inside the master plan's
2-week Phase 1 run alongside the oracle and reference-build tracks.

## 10. First 10 commands

```
1  <scratch>/native/bootstrap.sh hwlink native/hwlink        # worktree ~/code/gdx-3ds/hwlink
2  cd ~/code/gdx-3ds/hwlink && git log --oneline -3          # confirm base = native/main
3  sed -n '715,760p;1025,1050p;1255,1292p' port/3ds/main_3ds.cpp   # boot / loop / exit anchors
4  sed -n '55,65p;300,325p' port/3ds/gdx3ds_renderthread.cpp       # core/prio facts for the doc
5  grep -rn "gdx_request_quit\|gdx3ds_quit_requested" port/        # the exit lever
6  mkdir -p port/3ds/net tools/dev && git add -A && git commit --allow-empty -m "chore(hwlink): scaffold"
7  $EDITOR port/3ds/net/gdx3ds_net.h                         # contract first: init/tick/log_write/shutdown
8  cp /opt/devkitpro/examples/3ds/network/3dslink-demo/source/3dslink-demo.c /tmp/  # reference socInit
9  DEVKITPRO=/opt/devkitpro cmake -S . -B build-3ds -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake -DGDX_PLATFORM_3DS=ON -DCMAKE_BUILD_TYPE=Release
10 cmake --build build-3ds --target G-Diffuser-3DS_3dsx -j8  # baseline build before touching code
```
