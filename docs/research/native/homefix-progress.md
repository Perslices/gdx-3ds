# homefix — HOME suspend death with the native levers on (progress)

Branch `native/homefix`, worktree `~/code/gdx-3ds/homefix`, off `native/main@1c9e558`
(= b367f42 + the profile doc). Brief: hw run E (New 3DS, `native/main@b367f42`, ini
`[native] all=1 interp=0`, `[debug] renderthread=2` = ahead mode, `[stereo] enabled=1`,
`[menu] screen_off=0` at the time — the user had turned the screen back ON at log line 148):
after ~22 min of play and two clean lid sleep/wake cycles the user pressed HOME and "the app
died". `~/Desktop/G-Diffuser-3DS-native1/hw-runE-0920/log.txt` ends:

```
[transition] capture #12 320x240 source=lcd-scanout mode=7 chg=0 mirrorSeq=75854 mirrorMode=7 firstNonZeroPx=0
[apt] suspend (HOME/POWER -> HOME menu)
[apt] render thread parked (idle=1)
[apt] audio parked (drain=1 hle=1)
[watchdog] beat=269 frame=75969(+186) stage=9 ...
[watchdog] beat=270 frame=75969(+0) stage=9 ...
[watchdog] beat=271 frame=75969(+0) stage=9 ...
```

No `[apt] restore`, no `[apt] shutdown requested`, no `[exit]` chain, no Luma dump.

## State

| item | status | commit |
|---|---|---|
| root-cause audit (ranked hypotheses below) | DONE — no lever-specific fault found in the suspend window; one real hook-order bug found and fixed | |
| fix: our APT hook re-armed ahead of citro3d's (render-thread join before gx-queue clear), `[debug] apt_hook_first` killswitch | DONE | `8a4d591` |
| receipts: `[apt] <phase> step=N ...` flushed before every hook step, `hook done -> libctru` line, restore/wakeup chain | DONE | `8a4d591` |
| watchdog suspended-mode poll (100 ms, flushed; `WAKE observed`, gpu right, close order), `[debug] apt_watch` killswitch | DONE | `8a4d591` |
| emulator: canonical replay fhdiff vs b367f42 (RetroArch lane, software renderer) | see "Verified" | |
| `tools/ci-3ds.sh --jobs 8` | see "Verified" | |
| Desktop card `~/Desktop/G-Diffuser-3DS-homefix/` | see "Verified" | |

## Reading the log first: what the evidence does and does not say

1. The watchdog beats after the suspend (`+0`, stage=9) are EXPECTED: an app parked in the
   HOME menu keeps its threads; the watchdog (svc + filelog only) is the only thread of ours
   that still runs, and it wrote two beats = the process was alive and parked for >= 10 s
   after the hook finished. stage=9 is the loop-top `sWatchStage = 9; gdx3ds_rt_join_idle()`
   that precedes `aptMainLoop()` when `aptShouldJumpToHome()` is already visible
   (`main_3ds.cpp:1146-1152` at b367f42): the render thread was joined BEFORE the jump, hence
   `idle=1`.
2. `idle=1` here vs `idle=0` on lid sleep: the loop-top join only runs (in ahead mode) when a
   HOME/close request is visible; `aptShouldJumpToHome()` is false for a lid close, so the
   sleep hook itself has to join a render thread that may be mid-frame (`idle=0`, 3 of 6
   sleeps in run E and the release log). Both values are benign by themselves.
3. Between `[apt] audio parked` and the end of our hook only `gdx3ds_net_suspend()` (immediate
   return: `[net] enabled` unset, no `[net]` line anywhere in the log) and
   `aptBacklightForHome()` (no-op: the bottom screen was ON — `[menu] screen ON` at line 148,
   no later `screen OFF`; the same no-op is visible on all three lid sleeps of run E, which
   print no backlight line either) run. The hook finished. The "missing backlight line" is
   not a symptom.
4. After the hook, main is inside libctru for the rest of the log: `GSPGPU_SaveVramSysArea`
   -> `aptScreenTransfer` (APT IPC + GSP capture info, no GX) -> `aptDspSleep` (ndspFinalize;
   both audio threads parked) -> `GSPGPU_ReleaseRight` -> `APT_JumpToHomeMenu` ->
   `aptWaitForWakeUp` (libctru 2.7.0 apt.c:813-832, source read). Nothing of ours runs in
   that window except the watchdog.
5. The log ending 10-15 s after the suspend with no receipt is consistent with THREE endings:
   (a) the wake path died before our restore hook (`aptWaitForWakeUp`: `GSPGPU_AcquireRight`,
   `GSPGPU_RestoreVramSysArea`, then hooks — citro3d's ONRESTORE runs before ours);
   (b) the process died or was killed while parked; (c) the user closed / powered off from
   the HOME menu. Evidence for (c) being what "log ends after suspend" looks like on this
   card: the Sep 19 release-build log (`sd-backup-0919/log-release-build.txt` = run E's
   `log-prev.txt`) ALSO ends at `[apt] suspend / render thread parked / audio parked /
   bottom backlight on` with no `[apt] shutdown requested`, no `[exit]` chain — although the
   close-from-HOME path is documented (home-crash-audit.md) to print both. So on the release
   build a HOME-menu close left exactly the trace run E left, minus the two parked beats.
   **Ask the user what the screen showed** (Luma crash screen / "An error has occurred" /
   black + reboot / HOME menu tile gone) — that decides (a)/(b)/(c).

## Ranked hypotheses (file:line at native/main@1c9e558 unless noted)

1. **Lever-independent ending outside our code (wake path or HOME-menu close) — most likely
   on the evidence, medium confidence.** Path (a): libctru `aptWaitForWakeUp` apt.c:647-671 —
   `__ctru_speedup_config`, `GSPGPU_AcquireRight(0)`, `GSPGPU_RestoreVramSysArea()`, then
   `aptCallHook(ONRESTORE)` where citro3d's `C3Di_AptEventHook` (base.c:48-70:
   `C3Di_RenderQueueEnableVBlank`, dirty-all flags) runs BEFORE ours — our `[apt] restore`
   is the first receipt and never came. Path (c): the close order wakes us with
   `APTCMD_WAKEUP_CANCEL` (no ONRESTORE), `aptMainLoop()` returns false and main logs
   `[apt] shutdown requested` + flush (main_3ds.cpp:1154-1157) — absent in BOTH the release
   log and run E, so either the SD write after a HOME-menu close never lands, or the console
   does not deliver a close order to a .3dsx under hbldr and just kills it. New receipts
   discriminate all of these (see "What the next hardware run prints").
2. **aptHook order: citro3d's gx-queue wait/stop/clear runs before our render-thread join
   (real bug, fixed here; NOT run E's trigger because run E's loop-top join ran).**
   `aptHook` is LIFO (apt.c:858-867: the newest cookie becomes `aptFirstHook`);
   `C3D_Init` (gfx_citro3d.cpp:1409, inside `ctx->InitWindow`, main_3ds.cpp:913) registers
   citro3d's hook (base.c:131) AFTER ours (main_3ds.cpp:858). On ONSUSPEND/ONSLEEP citro3d
   runs `C3Di_RenderQueueWaitDone` = `gxCmdQueueWait` + `gxCmdQueueStop` + `gxCmdQueueClear`
   (renderqueue.c:92-100) and `gxCmdQueueClear` `svcBreak(USERBREAK_PANIC)`s if the queue is
   running (gxqueue.c:53-56); only THEN our `aptRenderThreadPark` joins the render thread.
   In ahead mode with the render thread mid-frame (`idle=0`: every lid sleep; a HOME flag
   landing between the loop-top check main_3ds.cpp:1146 and `aptMainLoop()` at :1154) the
   render thread's `C3D_FrameEnd -> gxCmdQueueRun` (renderqueue.c C3D_FrameEnd) submits GPU
   work AFTER libctru's "GPU idle" point, i.e. while `GSPGPU_SaveVramSysArea` /
   `aptScreenTransfer` / `GSPGPU_ReleaseRight` run — the GPU-busy-at-release state the
   power-off fix removed for the main thread. Native levers change only the odds (frame
   length on core 2). Fix: `8a4d591` re-arms our hook at the head after `InitWindow`
   (`[debug] apt_hook_first=1`, receipt `[apt] hooks: ours ahead of citro3d's ...`).
3. **Filelog/SD wedge on the first receipt after resume (lever-independent, low).**
   `aptLogReceipt` = svc + `gdx3ds_filelog_write` + `gdx3ds_filelog_flush` (fwrite + fflush
   to sdmc under the recursive lock, gdx3ds_filelog.c:75-100). If the FS session is
   unusable right after the wake, main blocks holding the lock, the watchdog blocks on the
   same lock at its next beat, and the log ends silently with the app "frozen". The new
   watchdog receipts would wedge the same way, so a log ending exactly at `WAKE observed`
   (written by the watchdog) with the app visibly alive on screen points here.
4. **Native paths touching GPU/linear memory while parked — ruled out by reading.**
   `gdx3ds_mesh.cpp`: bakes are lazy at bridge lookup (:965-990) inside the walk, i.e. on the
   submitting thread (bridgemain=1: main, inside `osSpTaskStartGo`) or the render thread —
   never while main sits in APT; buffers are `malloc`/`calloc` (:285-297), no `linearAlloc`
   anywhere in the native files (grep). The "per-frame linear pool" is the backend's
   `mVboPool`, `linearAlloc`ed once at Init (gfx_citro3d.cpp:1463) and re-based per frame in
   StartFrame on the render thread; index buffers are carved from it
   (`Gdx_NativeBatchIndices`, :4399). Recorders (`gdx3ds_track_sink.c`, `gdx3ds_fx_sink.c`)
   are plain rings written from the game fibers, which are real threads parked on
   LightEvents and only ever run from main's `gdx_fiber_switch`; no thread, lock, event or
   svc in any sink/replay file. `[native]` stats lines come from the render thread once per
   second and stop with it. Nothing native runs between `[apt] suspend` and the wake.
5. **Transition capture #12 racing the suspend — ruled out.** `source=lcd-scanout` is the
   CPU read of the top LCD back buffer (`n64_gfx_bridge.cpp:8790-8820`
   `GdxReadTopScanoutRgba16`: "no GX queue, no PPF"); the GPU readback path
   (`ReadColorBufRgba16`, gfx_citro3d.cpp:4044-4095, sync display transfer + sentinel poll)
   is not the source used. The frame mirror's async `GX_TextureCopy` (:4006) goes through
   citro3d's bound queue and is covered by citro3d's `gxCmdQueueWait` in its ONSUSPEND hook
   — after fix 2 that wait runs with the render thread joined, so nothing is left in flight.
6. **Post-resume GPU state with native draws — no fault.** citro3d's ONRESTORE dirties
   every tracked block (attr/buf info, program, textures, texenv, framebuf, fog LUT);
   the native batch and mesh paths issue only `C3D_*` calls (no `GPUCMD_AddWrite` in
   port/3ds/gfx), so the first frame after resume re-uploads everything. Visual at worst.
7. **DSP ordering on restore (doc error, lever-independent, low).** The audit said our
   ONRESTORE runs "AFTER aptDspWakeup -> ndspInitialize"; libctru 2.7.0 apt.c:671-687 calls
   the hooks BEFORE `aptDspWakeup()`. `gdx3ds_audio_resume` un-parks the drain thread, which
   may then issue `DSP_FlushDataCache`/`ndspChnWaveBufAdd` while ndsp re-initialises; those
   degrade to error Results and the release build resumed cleanly many times. Left as is,
   documented in the hook comment.
8. Memory: not it (release build heapUsed 52.6 MB vs run E 47.5 MB, linfree 24 MB).

## What the next hardware run prints (build `native/homefix`, `[debug] filelog=1`)

Every line below is flushed before the next call. HOME press:

```
[apt] suspend (HOME/POWER -> HOME menu)
[apt] suspend step=1 render thread park (t=+0ms active=1 gpu=1 rtIdle=1 frameOpen=0)
[apt] render thread parked (idle=1 join=0ms frameOpen=0)
[apt] suspend step=2 audio park (...)
[apt] audio parked (drain=1 hle=1)
[apt] suspend step=3 net suspend (...)
[apt] suspend step=4 backlight for HOME menu (...)
[apt] bottom backlight on for HOME menu ...          <- only when the menu's [OFF] is active
[apt] suspend hook done -> libctru (SaveVramSysArea, screen transfer, DSP sleep, ReleaseRight, JumpToHomeMenu, wait) t=+Nms
[apt] watch entered state=1 t=+Nms active=1 gpu=1 close=0 rtIdle=1 frameOpen=0     <- watchdog, 100 ms poll
[apt] watch gpu right released state=1 ...           <- GSPGPU_ReleaseRight ran
[apt] watch apt inactive (jump handed to the HOME menu) ...
[apt] watch parked state=1 t=+1000ms ...             <- every 1 s while in the HOME menu
[watchdog] beat=... apt=1 active=0 gpu=0             <- the 5 s beat carries apt= while parked
[apt] watch WAKE observed (main past the APT wait; AcquireRight/RestoreVramSysArea/restore hook next) ...
[apt] watch gpu right acquired ...
[apt] restore (resumed from HOME menu)
[apt] restore step=1 cpu state (...)
[apt] cpu state re-applied (...)
[apt] restore step=2 audio resume (...)
[apt] audio resumed
[apt] restore step=3 net resume (...)
[apt] restore step=4 backlight restore (...)
[apt] restore hook done -> libctru (DSP wakeup), frame loop resumes t=+Nms
[apt] watch: transition complete t=+Nms active=1 gpu=1 close=0
```

Close from the HOME menu instead: `[apt] watch CLOSE order pending ([exit] chain expected)`
then `[apt] shutdown requested — leaving frame loop`, `[exit] cause=apt close order
suspended=1` and the `[exit]` chain. Lid sleep prints the same shape under `sleep` /
`wakeup` (no backlight step, `hook done -> libctru (ReplySleepNotificationComplete, wait)`).

Where the log ends names the step:

| last line | meaning |
|---|---|
| a `step=N` line with no follow-up | that hook step wedged/faulted (the receipt says which thread state it saw) |
| `hook done -> libctru`, no `watch` line | died inside libctru's suspend path before the 100 ms poll fired (SaveVramSysArea / screen transfer / DSP sleep) — or `[debug] apt_watch=0` |
| `watch ... gpu right released`, no `apt inactive` | died in `APT_JumpToHomeMenu` |
| `watch parked` beats only | the process died or was killed while parked in the HOME menu; nothing of ours ran — a HOME-menu close/power-off that delivers no close order looks like this |
| `WAKE observed`, no `[apt] restore` | `GSPGPU_AcquireRight` / `GSPGPU_RestoreVramSysArea` / citro3d's ONRESTORE hook (or hypothesis 3: SD wedge on the first write) |
| `restore step=N`, nothing after | that restore step |
| `restore hook done`, no further beats | libctru's `aptDspWakeup` or the first frame after resume |

## Verified / not verified

Staged card: `~/Desktop/G-Diffuser-3DS-homefix/` — `.3dsx` sha256
`8478df6162e3ebfbf07876e67ed5c5fec37cdd5c0a94ac2cebc07a109e78d982`, `.elf`
`1c9793760570a80f173bf9e86facb9c2090df863eb73d536faf433e1f7425e42`, build id
`native/homefix@f276355d` (the code is final at `f276355`; this doc commit follows it).

| check | result |
|---|---|
| `tools/ci-3ds.sh --jobs 8` at `f276355` | **PASS** (vendor manifest 123 files, 15 patches already applied, 7 host test suites, both .3dsx built) |
| canonical replay, RetroArch lane, Vulkan, `det=1 fh=1 fh_every=1 renderthread=0 replay_quit=1 platform=azahar stereo.enabled=1 net.enabled=0 native.all=1 native.interp=0`: b367f42 (`bced3f34…`, run `20260920-135714-base-1`) vs `8a4d591` (`d81954ea…`, `20260920-140022-homefix-1`) | `fhdiff: compared=6002 divergent=0 only_in_a=0 only_in_b=0 last_common=6002` |
| same, b367f42 vs the staged `f276355` build (`8478df61…`, `20260920-140405-final-1`) | `fhdiff: compared=6002 divergent=0 ... last_common=6002`; 8/8 SHOT BMPs byte-identical; `[rec] end` reached; boot receipt `[apt] hooks: ours ahead of citro3d's (debug.apt_hook_first=1), suspended-mode watchdog poll=1 (debug.apt_watch), wake event=1` |
| HOME / lid-sleep / close-from-HOME on hardware | **NOT verified** — emulator lanes cannot press HOME or close the lid, and the replay lane runs `renderthread=0` so neither the hook re-arm nor the park path is exercised there. Hardware run per the card's TEST-PLAN.txt is the next step. |
| hook re-arm ordering | verified by reading libctru 2.7.0 `aptHook` (LIFO) + the boot receipt; not observable in the emulator |

Next step: user runs the card (runs 1-5 in TEST-PLAN.txt); paste the last `[apt]` line of
each run into the table above and decode it with "Where the log ends names the step". If
run E's ending was a HOME-menu close (hypothesis 1c), the new log will show `CLOSE order
pending` — or still nothing, which then proves the console kills the .3dsx without a close
order and the `[exit]` chain can never land from a HOME-menu close.

## Gotchas

- `gdx-dev retro run` host-crashes RetroArch at guest frame ~39 under machine load (load
  average 23 while this ran, lever-independent, known in interphooks-progress.md); the loop
  script retries the whole run. Software renderer only.
- Azahar/RetroArch cannot press HOME or close the lid; the replay lanes run with
  `renderthread=0` (determinism), so the park/unpark and hook paths are hardware-only.
- Never move `aptHook` registration before `InitWindow` again: LIFO order is the fix.
