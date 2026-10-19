/* port/3ds/include/gdx3ds_vram_guard.h -- n60boot: no CPU store may target VRAM.
 *
 * The CIA maps VRAM read-only (packaging/gdiffuser.rsf `1f000000-1f5fffff:r`); a CPU store
 * there is a permission data abort on hardware, and Azahar does not enforce the mapping, so
 * no emulator gate could see card n60-3d4 die (lcd_vram put the bottom screen in VRAM, the
 * launch prompt's consoleInit cleared it). Every bottom-screen CPU writer (launch prompt,
 * debug console, fps HUD, menu) calls the guard right before its consoleInit(GFX_BOTTOM).
 *
 * `[debug] vram_guard` (DEFAULT 1):
 *   0  log the violation, let the write happen (reproduces n60-3d4 on hardware; never ship)
 *   1  log the violation, move the bottom framebuffers to the linear heap, continue
 *   2  log the violation, then svcBreak (+ svcExitProcess when the emulator returns from it):
 *      the run dies with the receipt (emulator gates)
 * A clean check costs one pointer test and one boot-trail crumb; the receipt
 * `[vramguard] <who>: bottom framebuffer <addr> linear heap (ok)` goes to the file log.
 */
#ifndef GDX3DS_VRAM_GUARD_H
#define GDX3DS_VRAM_GUARD_H

#ifdef __cplusplus
extern "C" {
#endif

void gdx3ds_vram_guard_bottom_cpu_write(const char* who);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_VRAM_GUARD_H */
