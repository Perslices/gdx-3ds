/* port/3ds/gdx3ds_oracle.h — ORACLE: true frame capture + [fh] frame-hash receipts.
 *
 * Plan: docs/research/native/03-oracle-plan.md §1; progress: docs/research/native/oracle-progress.md.
 *
 * What "true" means: the capture reads the citro3d MAIN TARGET (240x400 portrait = 400x240
 * landscape) after C3D_FrameEnd, read through the in-order gx queue, i.e. frame N
 * exactly — not the libctru scanout back buffer (one display transfer downstream, frame N-1/N-2),
 * and not the 320x240 interpreter frame mirror (black in-game). Output per capture:
 *
 *   [fh] frame=<host frame> tick=<input tick> eye=L w=400 h=240 hash=<crc32> tag=<tag> mode=<m>
 *
 * `frame` is the host loop's frameNum (one VI tick per iteration), `tick` the input tick the
 * same iteration polled (what .gdxrec lines are numbered by), `hash` CRC-32 over the packed
 * 192,000-byte RGBA5551 buffer, `mode` GET_MODE(gGameMode). SHOT captures also write
 * autotest/<tag>_<eye>.bmp (24-bit, 400x240 native extent = 288,054 bytes).
 *
 * Triggers: SHOT <label> (autoinput / .gdxrec / GDX_INPUT_SCRIPT, via gdx_request_frame_dump),
 * [debug] fh_every=N (hash only, tag=auto), gdx_oracle_capture_request() (hwlink net CAPTURE).
 *
 * Idle cost is zero: the after-endframe hook returns after two loads unless armed; the
 * readback stall happens ONLY on captured frames (state it in any perf claim).
 *
 * ini ([debug]): fh=1 (killswitch), fh_every=0, fh_eye=L|R|both, fh_scale=0 (BMP-only
 * downscale width, hashes always native), fh_every_bmp=0, shot_legacy=0 (also run the old
 * mirror/scanout dumps), platform=<free text, carried in the init receipt>, quit_at_tick=0
 * (N>0: leave the frame loop once the input tick reaches N — self-terminating scripted runs).
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* After gdx3ds_config_load(): latches every ini key above once, emits the "[fh] init" receipt. */
void gdx3ds_oracle_init(void);

/* [debug] fh != 0. */
int gdx3ds_oracle_enabled(void);

/* True when SHOT requests should be routed here instead of the legacy mirror/scanout dumps. */
int gdx3ds_oracle_owns_shot(void);

/* Main thread, once per host iteration BEFORE the frame's EndFrame is issued: publishes the
 * (frame, tick, mode) triple the after-endframe hook stamps on that frame's receipt. Ring-
 * buffered by frame so the render thread (pipe/ahead modes) can lag main by several frames. */
void gdx3ds_oracle_frame_mark(unsigned long frame, unsigned long tick, int gameMode);

/* Immediately after EndFrame() on whichever thread issued it (render thread in rt modes, main
 * in rtMode 0). Counts frames; when armed: capture (queued behind frame N on the in-order gx
 * queue, sentinel-polled), hash, receipt, BMP. */
void gdx3ds_oracle_after_endframe(void);

/* GDX3DS_BUILD_ID ("<branch>@<sha8>") — the .gdxrec `build` header uses it. */
const char* gdx3ds_oracle_build_id(void);

/* Cross-track contract (hwlink): schedule a true capture + [fh] receipt (+ BMP) of the next
 * presented frame. Callable from ANY thread; `tag` may be NULL (-> "req"). A second request
 * before the frame ends replaces the tag (one capture per frame). */
void gdx_oracle_capture_request(const char* tag);

/* Replay-driven clean exit ([debug] replay_quit=1 -> [rec] end -> leave the frame loop). */
int gdx3ds_oracle_quit_requested(void);
void gdx3ds_oracle_request_quit(const char* why);

#ifdef __cplusplus
}
#endif
