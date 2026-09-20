# hwlink — progress

Track: `hwlink` (branch `native/hwlink`, worktree `~/code/gdx-3ds/hwlink`).
Plan: `docs/research/native/02-hwlink-plan.md`. Protocol: scratch `native/PROTOCOL.md`.
User-facing doc: `docs/HW-DEV-LOOP.md`.

## State

App side and host side are implemented, build clean, and are verified for everything
Azahar can adjudicate. Plan steps 1-3 and 6 (host half) and 10 are **done**; steps 4-8 are
the hardware items, staged for the user at `~/Desktop/G-Diffuser-3DS-hwlink/`. Step 9
(framed CAPTURE blob transport) is joint with the oracle track and gated on its arm hook.

## What landed

| File | What |
|---|---|
| `port/3ds/net/gdx3ds_net.h` | Contract + the full `[net]` ini key list |
| `port/3ds/net/gdx3ds_net.c` | SOC bring-up, SPSC log ring, net thread, control listener, RELOAD/PUT, hb:ldr relaunch, APT suspend/resume |
| `port/3ds/net/CMakeLists.txt` | `gdx3ds_net` static lib (3DS-only; INTERFACE stub on the host configure) |
| `port/3ds/CMakeLists.txt` | build id hoisted above `add_subdirectory`, `net/` registered and linked |
| `port/3ds/main_3ds.cpp` | **+27 lines only**: include, watchdog `net=` field, 4 APT hook cases, init, per-frame drain, exit chain, relaunch |
| `port/3ds/gdx3ds_filelog.c` | **+6 lines**: the one sink hook that mirrors every tracer line into the ring |
| `tools/dev/hw.py` | Host driver, stdlib only |
| `docs/HW-DEV-LOOP.md` | The one-page user card |

Commits: `4e29bbb` scaffold, `b8d0910` the app-side TU, `861550f` `hw.py`,
`58e0772` two Azahar-found fixes + the user doc.

## Protocol spec (TCP 17492, the console listens)

ASCII command line terminated by `\n`; the reply is one `OK …` / `ERR <reason>` line. One
command in flight. A framing error closes the connection and the host reconnects. Commands
are **parsed** on the net thread and **executed** on the main thread.

| Command | Payload | Reply |
|---|---|---|
| `PING` | — | `OK PONG <build-id> <frame> <uptime_ms>` |
| `RELOAD <size> <crc32hex>` | `<size>` raw bytes | `OK RELOAD ready <n>` → `OK RELOAD` → link closes |
| `PUT <sdmc-path> <size> <crc32hex>` | `<size>` raw bytes | `OK PUT ready <n>` → `OK PUT sdmc:<path>` |
| `CAPTURE <tag>` | — | `OK CAPTURE armed <tag>` / `ERR CAPTURE oracle-hook-absent` |
| `REPLAY <sdmc-path>` | — | `OK REPLAY sdmc:<path>` / `ERR REPLAY hook-absent` |
| `INI <section> <key> <value>` | — | `OK INI` |
| `INI SAVE` | — | `OK INI saved` |
| `QUIT` | — | `OK QUIT` |

`ERR … busy` = another command is in flight. `ERR … main-thread-stalled` = the frame loop
did not drain the request within 3 s — the link stays usable, which is the point.

Paths are rejected unless they are sdmc-relative absolute and contain no `..`: the control
port is LAN-reachable.

Log link: TCP **17491**, the console dials **out** to `__3dslink_host` (or `[net] host=`).
Raw newline-separated text, **no handshake** — verified by disassembling libctru's
`link3dsConnectToHost` (plain `socket()` + `connect()` to port `0x5344` = 17491 network
order, then two `dup2`s). That is why our own fd is drop-in compatible with `3dslink -s`
and why `hw.py log` is just a server on that port.

## ini keys (all under `[net]`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | **0** | master killswitch; 0 = init returns before allocating anything |
| `port` | 17492 | control listener |
| `log_port` | 17491 | log link (`LINK3DS_COMM_PORT`) |
| `host` | *(empty)* | explicit log-link peer; empty = use `__3dslink_host` |
| `stdio` | 0 | also run `link3dsStdio()` (opt-in: it would put a game-thread `printf` on a socket) |
| `target` | `/3ds/G-Diffuser-3DS.3dsx` | RELOAD write target, sdmc-relative; also the `hb:ldr` path |
| `soc_kb` | 1024 | SOC context buffer, KiB |

## Receipts (`[net]` prefix, through the existing filelog sink)

`init`, `soc rc=`, `thread up prio= core=`, `ctl listening`, `link up build= host=`,
`log link down rc= errno=`, `relink attempt= errno=`, `ctl accept from`, `ctl closed`,
`cmd <VERB> …`, `recv ok path= size= crc=`, `recv crc mismatch want= got=`, `recv short`,
`dropped= total=`, `suspend ack=`, `resume — relinking`, `hbldr SetTarget … rc=`,
`hbldr SetArgv argc= rc=`, `hbldr unavailable rc= — plain exit`, `links closed`,
`shutdown done dropped_total=`.

Plus one field on the existing watchdog beat: `net=0|1|2` (down / log link / log+control),
**omitted entirely when `[net] enabled = 0`** so a killswitched run keeps a byte-identical
`[watchdog]` line.

## Verified in Azahar (evidence)

Azahar emulates `soc:U` over real host sockets, so the whole link — outbound log
connection, inbound control listener, file transfer — is exercisable there. What Azahar
cannot adjudicate is `hb:ldr` (no Rosalina), the netloader, InputRedirection, the GDB stub,
sleep/WiFi-drop behaviour, and anything about performance.

Artifacts: `/tmp/hwlink-art/` (`log-baseline-netoff.txt`, `log-neton-*.txt`,
`hostlog-neton.txt`, `gdiffuser.ini.ORIGINAL`).

**1. Killswitch off** — 1,569-line run with no `[net]` section in the ini at all:

```
$ grep -c "\[net\]" log-baseline-netoff.txt            -> 0
[watchdog] beat=1 frame=231(+231) … heapUsed=40607544 heapFree=7585992 hwm=… base=0
```

Zero `[net]` lines, and no `net=` field on the watchdog beat.

**2. Killswitch on** — bring-up, in order, streamed live to the Mac's `hw.py log`:

```
[net] init port=17492 log_port=17491 host=127.0.0.1 soc=1024KiB target=/3ds/G-Diffuser-3DS.3dsx build=native/hwlink@4e29bbbd
[net] soc rc=0x00000000 size=1024KiB
[net] thread up prio=0x3E core=0
[net] ctl listening 0.0.0.0:17492
[net] link up build=native/hwlink@4e29bbbd host=127.0.0.1:17491
[watchdog] beat=1 … heapUsed=41696104 … base=0 net=1
```

The host received the whole boot trace from `gdx3ds_config_load` onward (675 lines),
i.e. the pre-init lines buffered in the ring were flushed once the link came up.

**3. Control protocol against the real firmware** (`hw.py`, Azahar guest):

```
PING     -> OK PONG native/hwlink@4e29bbbd 995 19370      (2 s later: … 1121 21476)
CAPTURE  -> ERR CAPTURE oracle-hook-absent                (weak hook unresolved: correct)
PUT      -> OK PUT sdmc:/3ds/gdiffuser/rec/t.gdxrec       (rec/ auto-created; byte-identical)
REPLAY   -> ERR REPLAY hook-absent
INI      -> OK INI            ([net] cmd INI [debug] cadence=1 rc=0)
INI SAVE -> OK INI saved      (key present in the rewritten gdiffuser.ini)
BOGUS x  -> ERR unknown command BOGUS
INI      -> ERR INI usage: INI <section> <key> <value> | INI SAVE
```

**4. RELOAD end to end.** 3,024,676 B streamed into the guest, CRC checked, renamed over
the target; the staged file `cmp`s byte-identical with the source `.3dsx`:

```
[net] recv ok path=sdmc:/3ds/G-Diffuser-3DS.3dsx size=3024676 crc=710E7270
[net] cmd RELOAD staged=/3ds/G-Diffuser-3DS.3dsx -> quit + hb:ldr
exiting
[exit] cause=app quit suspended=0 … disk_save_flush … audio_thread_stop … audio_shutdown …
[exit] render_thread_join done / window_shutdown done / fs_shutdown done
[net] links closed (thread exit) / [net] links closed (shutdown)
[net] shutdown done dropped_total=0
[exit] net_shutdown done
[net] hbldr unavailable rc=0xD88007FA — plain exit, relaunch by hand
[fatal-trace] atexit: process exiting via exit()
```

`0xD88007FA` is "port not found" — the **graceful degrade is confirmed**. `hw.py reload`
correctly reported no relaunch and printed the loop guard.

**5. Plain QUIT** produces the same `[exit]` chain minus any hb:ldr attempt, proving
`sRelaunchPending` gates it correctly.

**6. Memory cost, measured** — same build, same frames, `[net]` on vs off:

| Frame | `heapUsed` net=off | `heapUsed` net=on | delta |
|---|---|---|---|
| 258 | 40,610,304 | 41,696,104 | **+1,085,800 B** |
| 514 | 41,347,864 | 42,432,408 | **+1,084,544 B** |
| 770 | 41,347,864 | 42,432,408 | **+1,084,544 B** |

`arena` grows by the same 1,085,440 B. Heap = the 1 MiB SOC buffer (1,048,576) + the
32 KiB thread stack + allocator bookkeeping. **Static (`.bss`) cost is 101,852 B**
(`arm-none-eabi-size -A gdx3ds_net.o`): `sRxBuf` 64 KiB + `sRing` 32 KiB + the hb:ldr
`argbuf` 1 KiB + the CRC table 1 KiB + line/path buffers. Total ≈ **1.13 MiB**, matching
the plan's §3.6 budget line for line.

**7. Thread footprint.** One thread, `prio=0x3E core=0`, confirmed by the receipt. The map
is now 0x18 audio drain + producer + watchdog, 0x24 render (core 2), 0x30 main/game
(core 0), 0x3E net (core 0). Nothing preempts an existing thread. **Azahar cannot judge
whether this costs frame time** — that is hardware TEST-PLAN item F.

**8. Ring drop accounting works.** During the first 3 MB RELOAD, with `[debug] verbose=1`
and `diag_audio=1` filling the log, the net thread was inside `netRecvFileTo` and not
draining: `[net] dropped=11 total=11`. Bounded, counted, and reported — exactly the
filelog's own policy. A later RELOAD on a quieter link dropped 0.

**9. Host-side, no console needed.** `hw.py`'s control framing against an **independent
mock console** sharing no code with it: PING / INI / INI SAVE / CAPTURE / PUT+REPLAY /
RELOAD / QUIT all frame correctly and the 3,024,676-byte `.3dsx` arrived byte-identical
(CRC `710E7270`). `ir_packet()` byte-exact against the documented Rosalina layout: neutral
`(0x00000FFF, 0x02000000, 0x007FF7FF, 0x80800081, 0)`, A/Y/L+R active-low masks, circle-pad
L/R/U extremes, HOME/POWER/POWER-long = 1/2/4, touch word including the touching flag.

## Two bugs Azahar caught that reading the code did not

1. `gdx3ds_net_relaunch_if_requested()` was gated on `sNetEnabled`, which
   `gdx3ds_net_shutdown()` clears **by design** two lines earlier in the exit chain. The
   `hb:ldr` call was silently skipped and not even the "unavailable" receipt was written —
   on hardware the self-relaunch would have failed leaving no evidence. Fixed in `58e0772`.
2. `hw.py reload` hard-failed with `EADDRINUSE` whenever a `hw.py log` already served
   17491 — the *normal* two-terminal setup. It now falls back to polling `PING`.

## Two facts checked against libctru 2.7.0 rather than assumed

1. **`__system_initArgv` CONSUMES the netloader's host argument.** Disassembling
   `initArgv.o` out of `libctru.a`: the LAST argv entry is tested for `strlen == 17` and
   `strncmp(arg + 8, "_3DSLINK_", 8)`; on a match it does `strtoul(arg, NULL, 16)` into
   `__3dslink_host`, decrements `argc`, and NULLs `argv[argc]`. So the trailer can **not**
   be copied out of `main()`'s argv for the RELOAD hand-off — `gdx3ds_net.c` re-synthesises
   it as `"%08lX_3DSLINK_"` of `__3dslink_host.s_addr`. **This is the one guessed byte
   format left in the track and only hardware TEST-PLAN B can confirm it.** If the
   relaunched build comes back but its log link never connects, this is the suspect.
2. **SOC implements neither `SO_RCVTIMEO` nor `SO_SNDTIMEO`** — they are not in libctru's
   `sys/socket.h` at all, and the first build failed on exactly that. Every timeout in the
   TU is therefore a `poll()` budget over a non-blocking fd, which is the stronger property
   anyway: a WiFi drop mid-transfer cannot wedge the net thread inside `recv`/`send`.

## Known limitations / accepted

- The last three teardown receipts (`[net] shutdown done`, `[exit] net_shutdown done`,
  `[net] hbldr …`) cannot reach the host, because they are written *after* the link they
  describe was closed. They land in the SD `log.txt` (`[debug] filelog = 1`), which is why
  the user card asks for that key. Unavoidable: the relaunch must be strictly last.
- `RELOAD` writes the ~3 MB image from the net thread, so the SD write is not on the frame
  path but the net thread stops draining the log ring for its duration (see evidence 8).
- The control port has no authentication. It is a LAN dev tool behind a default-off
  killswitch; path traversal is rejected, but do not enable `[net]` on an untrusted network.

## Gotchas for whoever picks this up

- **`git add -A` in a fresh bootstrap worktree deleted the ten `docs/research/native/`
  plan docs** (they are tracked on `native/main` but were absent from the new worktree).
  Caught and restored in `d0f…`; `git diff --stat native/main..HEAD -- docs/` is now this
  track's two files and nothing else. **Check this on every worktree before your first
  `git add -A`.**
- `open -a Azahar.app --args …` on macOS **silently reuses an already-running instance and
  ignores the args**. Two runs in a row looked like one very long run with the old ini
  still loaded, which wasted a boot cycle. `pkill -9 -f Azahar` and confirm with `pgrep`
  before every launch.
- Azahar's `soc:U` is real host sockets: the guest binding `0.0.0.0:17492` shows up in
  `lsof` on the Mac, and the guest connecting to `127.0.0.1` reaches the Mac's loopback.
  That is what makes the whole link testable without hardware.
- The shared Azahar SD ini was restored to its pre-test state (backup at
  `/tmp/hwlink-art/gdiffuser.ini.ORIGINAL`), except for a documented `[net] enabled = 0`
  section left behind on purpose so the next agent sees the key exists.

## Cross-track notes

- **`gdx_oracle_capture_request` signature is UNRESOLVED — read before merging
  `native/oracle`.** `03-oracle-plan.md` §1 spells it `(const char* tag, int flags)`; this
  branch was directed to call the **one-argument** form and does. C has no name mangling,
  so a mismatch **links silently** and hands the callee garbage in `r1` under AAPCS.
  Whoever merges must reconcile it in both places and rebuild. The same warning is a
  comment at the declaration in `gdx3ds_net.c`.
- `gdx_replay_start(const char* sdmcPath)` is the other weak hook. `hw.py replay` already
  does `PUT` then `REPLAY`, so the oracle track only has to define the injector entry
  point — transport and arming are done and tested.
- The `.gdxrec` format is **the oracle track's**. This track defines no competing record
  format. UDP 4950 is exploration only and says so in `hw.py`'s docstring, at runtime, in
  `docs/HW-DEV-LOOP.md` §4 and in `gdx3ds_net.h`.
- `main_3ds.cpp` edits are 27 lines in six places, deliberately away from `EndFrame`, to
  keep the oracle track's merge cheap.

## Next step

Hand the staged card to the user and run hardware TEST-PLAN A-F (below, and in
`~/Desktop/G-Diffuser-3DS-hwlink/TEST-PLAN.txt`). B (`RELOAD` + `hb:ldr`) is the day-one
gate and the only unverified byte format. Then plan step 9, the framed CAPTURE blob
transport, jointly with the oracle track.
