/* port/3ds/gdx3ds_racemap.h — BOTTOMHUD: the race map on the bottom screen.
 *
 * gdiffuser.ini `[debug] bottom_map` — DEFAULT 0. With it 0 the MENU tab bar is the one
 * it has always been, nothing in this TU runs, and the only cost anywhere is one int
 * test per frame in gdx3ds_menu_tick. With it 1 the MENU grows a "MAP" tab which draws
 * the race minimap at 3x into the bottom-screen console framebuffer.
 *
 * MECHANISM, and why it costs the top screen nothing:
 *   - The picture is SOURCED, not re-rendered. minimap.c keeps the track as a 64x64 CI8
 *     bitmap in RDRAM and the racers as plain positions; port/gdx_racemap.h reads both.
 *     No display list is intercepted, re-issued or doubled.
 *   - The picture is DRAWN BY THE CPU straight into the libctru console's framebuffer —
 *     the same u16* the MENU's text goes through. consoleInit() put the bottom screen in
 *     GSP_RGB565_OES with double-buffering OFF, so that pointer is the scanout buffer and
 *     a store to it is the whole present path. No citro3d target, no C3D_FrameDrawOn, no
 *     display transfer, no VRAM.
 *   Consequence: zero GPU work, zero extra draws, and the stereo right-eye pass (which
 *   re-issues every draw, stereocost-progress.md: ~1 ms) is untouched in both directions.
 *
 * COST MODEL: this runs on MAIN, at the tail of the host frame loop. mainprof-progress.md
 * M0 measured main as the SLACK thread — it works ~4.6 ms and blocks ~24 ms per crowd
 * frame while the render thread (core 2) runs ~93% busy. CPU time spent here therefore
 * eats slack, not frame time, until it exceeds that slack. On an Old 3DS, where the render
 * thread is unavailable ([rt] mode=seq) and main IS the critical path, the cost is fully
 * additive; the lever default is OFF for that reason too.
 *
 * GEOMETRY (the pixel-compare anchor, derived in docs/research/native/bottomhud-progress.md):
 *   source pixel (col,row) in 0..63  ->  bottom-screen pixel
 *       x = 64 + 3*col      in 64..255   (192 wide, centred on the 320-px panel)
 *       y = 24 + 3*row      in 24..215   (192 tall, exactly console rows 4..27)
 *   framebuffer index = x * 240 + (239 - y)    (3DS framebuffers are portrait/column-major)
 *
 * KNOWN, MEASURED, NOT FIXED: the validity gate is the game MODE, and the top screen stops
 * DRAWING the minimap before the mode leaves the race (the finish / results camera). Measured
 * on `race-mutecity-start`: at SHOT `m01_4342` and `m01_4642` the top screen's minimap crop
 * holds zero white pixels while this page still shows the last race's map with `valid=1
 * mode=1`. Harmless -- a map on the results screen is not wrong -- but it means the two
 * screens are only guaranteed to agree WHILE the race HUD is up, which is the window the
 * pixel compare in bottomhud-progress.md uses.
 *
 * Every entry point is MAIN-THREAD ONLY except gdx3ds_racemap_note_shot, which only arms
 * a flag.
 */
#ifndef GDX3DS_RACEMAP_H
#define GDX3DS_RACEMAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* After gdx3ds_config_load(): latches [debug] bottom_map and emits the always-on receipt
 *   [bmap] init enabled=<0|1> scale=3 box=64,24-255,215 src=64x64
 * so a headless run can prove which arm it is. */
void gdx3ds_racemap_init(void);

/* [debug] bottom_map != 0. Stable after init, and safe (0) before it. */
int gdx3ds_racemap_enabled(void);

/* Paint the map into the console framebuffer `fb` (PrintConsole::frameBuffer, RGB565,
 * 320x240 portrait). Returns 1 if it painted a live race, 0 if it painted the
 * "no race" placeholder. Main thread, inside the MENU's paint window. */
int gdx3ds_racemap_paint(unsigned short* fb);

/* One line of text for the MENU's header row: "LAP 2/3  POS 4/30" or "no race". */
void gdx3ds_racemap_status_line(char* buf, int cap);

/* SHOT plumbing. gdx_request_frame_dump routes the top screen to the ORACLE
 * (autotest/<tag>_L.bmp); this arms the matching BOTTOM capture, written by the next
 * gdx3ds_racemap_paint as autotest/<tag>_B.bmp (24-bit, 320x240, from the very bytes the
 * LCD is scanning out). Callable from any thread — it only stores a tag. No-op unless the
 * lever is on. */
void gdx3ds_racemap_note_shot(const char* label);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_RACEMAP_H */
