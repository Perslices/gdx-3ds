/* port/3ds/include/gdx3ds_boot_trail.h -- n60boot: the crash-proof boot trail.
 *
 * WHY. Card n60-3d4 died at boot on the Old 3DS with no log at all: the file log opens after
 * the launch window, Luma 6.6 leaves no dump, and svcOutputDebugString goes nowhere on
 * hardware. The trail is the record that survives such a death.
 *
 * WHAT. From main() entry (before config, gfxInit and the file log) until the first game
 * frames, every boot milestone (each logStep, plus crumbs around gfxInit / consoleInit / the
 * launch prompt / the file log) is appended to sdmc:/3ds/gdiffuser/boot<tag>.txt as ONE raw
 * FSFILE_Write(..., FS_WRITE_FLUSH) (no stdio, no heap, no lock: offsets are reserved
 * atomically), and to svcOutputDebugString. The file is rewritten every boot; the previous
 * boot's trail is kept as boot<tag>-prev.txt. The main thread's CPU exception handler is
 * armed for the same window (gdx3ds_crash_arm_boot): an early data abort lands its report in
 * the trail. Nothing here touches VRAM or GSP.
 *
 * ON BY DEFAULT (compile-time GDX3DS_BOOT_TRAIL=1): the key cannot be read before the config
 * is, so the trail opens unconditionally; `[debug] boot_trail = 0` (read right after config
 * load) closes it there. `[debug] boot_trail_frames = N` (default 2): crumbs for host frames
 * 0..N-1, closed at frame N. The closing line `[boottrail] closed ...` carries the cost
 * (writes, avg/max us per crumb) and is also copied into the file log.
 */
#ifndef GDX3DS_BOOT_TRAIL_H
#define GDX3DS_BOOT_TRAIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* main() entry: rotate boot.txt -> boot-prev.txt, open a fresh boot.txt, arm the handler. */
void gdx3ds_boot_trail_open(void);

/* After gdx3ds_config_load: latch [debug] boot_trail / boot_trail_frames. */
void gdx3ds_boot_trail_latch_config(void);

/* One crumb: the trail file + svcOutputDebugString. No-op once closed. Any thread. */
void gdx3ds_boot_trail(const char* line);
void gdx3ds_boot_trail_f(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* One crumb to the trail file only (logStep already sent the line to svc). */
void gdx3ds_boot_trail_file(const char* line);

/* Host frame hook (before the frame counter advances): crumbs, then the close. */
void gdx3ds_boot_trail_frame(unsigned long frame);

/* The closing receipt (cost), close the file, disarm the boot handler. Idempotent. */
void gdx3ds_boot_trail_close(const char* why);

int gdx3ds_boot_trail_active(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_BOOT_TRAIL_H */
