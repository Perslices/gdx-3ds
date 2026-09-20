/* port/gdx_linkstubs_desktop.c — desktop-side definitions for symbols the default desktop
 * configuration gets from a TU that a non-default configuration does not compile.
 *
 * Sibling of port/3ds/game/gdx3ds_us_linkstubs.c, which does the same job for the 3DS build.
 * Nothing here changes behaviour in the shipped configuration: the EK block below does not
 * compile there at all, and gdx_rival_detail_level's 0 is the NATIVE level, i.e. exactly what
 * Racer_Draw did before the hook existed.
 */

/* ── Always ─────────────────────────────────────────────────────────────────────────────────
 *
 * port/3ds/patches/decomp-port-rival-detail.patch adds a `gdx_rival_detail_level()` call to
 * Racer_Draw under `#ifdef PORT` — which is every port build, not just the 3DS one — but only
 * the 3DS supplies a definition (port/3ds/gdx3ds_menu.c, driven by the [perf] rival_detail INI
 * key). A desktop build of a patched tree therefore failed to link. It has no INI and no menu
 * entry for the option, so it reports NATIVE: 0 leaves gdxRivalDetail at 0, which is the
 * branch Racer_Draw took before the patch. See port/3ds/patches/README.md
 * (0 NATIVE / 1 REDUCED / 2 MINIMAL). */
int gdx_rival_detail_level(void) {
    return 0;
}

/* ── GDX_EXPANSION_KIT=OFF only ─────────────────────────────────────────────────────────────
 *
 * The cartridge-only configuration (what the 3DS port builds, and what the reference renderer
 * in tools/refbuild builds so it compiles the same game) drops port/n64_leo.c,
 * port/gen/EkLinkStubs.c and port/disk_buffer.cpp, but not their callers. EXPANSION_KIT is
 * defined on the G-Diffuser target by port/CMakeLists.txt whenever GDX_EXPANSION_KIT is ON,
 * which is the default and every release, so none of this exists in a shipped binary. */
#ifndef EXPANSION_KIT

/* Deliberately NOT "PR/leo.h": this TU is part of the G-Diffuser exe, which compiles in the host
 * CRT environment, not the decomp one, so the libultra headers' u8/u32 typedefs are not in scope
 * (the same reason port/shims.c hand-declares every libultra symbol it defines). The linker
 * resolves by name and size, so the storage below is byte-equivalent to the real object. */
#include <stddef.h>

/* port/n64_leo.c: populated from the disk image on EK builds. Zero means "no disk", which is
 * what shims.c's US-only LeoTestUnitReady already reports. sizeof(LEODiskID) is 32 bytes
 * (PR/leo.h: gameName[4] + gameVersion + diskNumber + ramUse + diskUsage + LEOSerialNum(16) +
 * company[2] + freeArea[6]); it is only ever read as "all zero = absent" in this build. */
/* u64 elements, not bytes: the real LEODiskID contains a u64 (LEOSerialNum.lineNumber), so its
 * alignment is 8 and a char array would under-align the object its readers cast it to. */
unsigned long long leoBootID[4];

/* port/gen/EkLinkStubs.c: ucode text markers that n64_gfx_bridge.cpp's matchesUcodeText
 * compares by ADDRESS and never dereferences as code. One u64 apiece, the EK stub's shape. */
unsigned long long gspF3DEX2_Rej_fifoTextStart[1];
unsigned long long gspL3DEX2_fifoTextStart[1];

/* port/disk_buffer.cpp: gdx_menu.cpp's storage panel asks whether a verified fzerox-disk.o2r
 * exists before it offers to delete the original .ndd. With no EK there is no disk archive and
 * no original to delete, so the honest answer is 0 — the panel simply never suggests deletion. */
int gdx_disk_archive_verified(void) {
    return 0;
}

#endif /* !EXPANSION_KIT */
