/* port/3ds/gdx3ds_racemap.c — BOTTOMHUD: the race map on the bottom screen.
 * Contract, mechanism and cost model: gdx3ds_racemap.h. Evidence:
 * docs/research/native/bottomhud-progress.md.
 *
 * Drawing discipline, and why the loop is shaped the way it is: a 3DS framebuffer is
 * PORTRAIT — index = x * height + (height - 1 - y). Scanning a source row (x varying,
 * y fixed) therefore writes with a 480-byte stride and touches a fresh cache line on
 * every pixel. So the outer loop walks SCREEN COLUMNS and the inner loop walks y
 * downward, which makes the writes contiguous and forward. Better still: an integer 3x
 * upscale means three adjacent screen columns are identical, so one 192-entry column
 * strip is built per source column (64 of them, 4 KB of source that sits in L1) and
 * memcpy'd three times. Per paint that is 12,288 palette lookups and 192 memcpys of
 * 384 bytes, instead of 36,864 scattered pixel stores.
 *
 * Allocation discipline (the MENU's): integer-only snprintf into fixed buffers, no
 * float formatting, no malloc on the paint path.
 */
#include "gdx3ds_racemap.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx_racemap.h"

/* ---- geometry (gdx3ds_racemap.h documents the derivation) ------------------------ */
#define BMAP_SRC_DIM 64
#define BMAP_SCALE 3
#define BMAP_PX (BMAP_SRC_DIM * BMAP_SCALE) /* 192 */
#define BMAP_X0 64                          /* (320 - 192) / 2 */
#define BMAP_Y0 24                          /* console row 4 */
#define BMAP_FB_H 240
#define BMAP_FB_W 320

/* Background for MINIMAP_PALETTE_CLEAR. The N64 draws the CLEAR index with alpha 0 over
 * the racing scene; there is no scene behind the bottom screen, so index 0 becomes the
 * panel colour. Very dark blue-grey rather than black so the 192x192 box reads as a
 * deliberate frame and the pixel compare has an unambiguous "outside the track" value. */
#define BMAP_BG565 0x0841u /* r=1 g=2 b=1 in RGB565 */

/* ---- state ----------------------------------------------------------------------- */
static int sEnabled = 0;
static uint16_t sPal565[4];
static char sPendingShot[40];
static int sShotArmed = 0;
static unsigned long sPaints = 0;
static unsigned long sPaintTicksTotal = 0;
static unsigned long sPaintTicksMax = 0;

static void BmapLog(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int) sizeof(buf) - 1) {
            n = (int) sizeof(buf) - 1;
        }
        svcOutputDebugString(buf, (size_t) n);
        gdx3ds_filelog_write(buf, (size_t) n);
    }
}

/* RGBA5551 (r5 g5 b5 a1, MSB-first) -> RGB565. Green gains a bit by replication, which is
 * the same expansion the ORACLE's BMP writer uses in the other direction. */
static uint16_t Rgba5551To565(unsigned short v) {
    const unsigned r = (v >> 11) & 31u;
    const unsigned g = (v >> 6) & 31u;
    const unsigned b = (v >> 1) & 31u;
    return (uint16_t) ((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
}

/* MINIMAP_*_COLOR from decomp/src/overlays/ovl_i3/minimap.h, as RGBA5551 literals, in
 * GdxRaceMapMarkerKind order. Reproduced rather than read through a decomp header so this
 * TU stays free of game types; the derivation is one GPACK_RGBA5551 each. */
static const unsigned short kMarkerRgba5551[6] = {
    (unsigned short) ((0u << 11) | (31u << 6) | (31u << 1) | 1u),  /* PLAYER1 cyan    */
    (unsigned short) ((31u << 11) | (31u << 6) | (0u << 1) | 1u),  /* PLAYER2 yellow  */
    (unsigned short) ((0u << 11) | (31u << 6) | (0u << 1) | 1u),   /* PLAYER3 green   */
    (unsigned short) ((31u << 11) | (15u << 6) | (31u << 1) | 1u), /* PLAYER4 magenta */
    (unsigned short) ((0u << 11) | (0u << 6) | (31u << 1) | 1u),   /* LEADER blue     */
    (unsigned short) ((0u << 11) | (0u << 6) | (0u << 1) | 1u),    /* GHOST black     */
};

void gdx3ds_racemap_init(void) {
    sEnabled = gdx3ds_config_get_bool("debug", "bottom_map", 0) ? 1 : 0;
    BmapLog("[bmap] init enabled=%d scale=%d box=%d,%d-%d,%d src=%dx%d", sEnabled, BMAP_SCALE, BMAP_X0, BMAP_Y0,
            BMAP_X0 + BMAP_PX - 1, BMAP_Y0 + BMAP_PX - 1, BMAP_SRC_DIM, BMAP_SRC_DIM);
}

int gdx3ds_racemap_enabled(void) {
    return sEnabled;
}

void gdx3ds_racemap_note_shot(const char* label) {
    if (!sEnabled || label == NULL || label[0] == '\0') {
        return;
    }
    snprintf(sPendingShot, sizeof(sPendingShot), "%s", label);
    __atomic_store_n(&sShotArmed, 1, __ATOMIC_RELEASE);
}

/* ---- the blit -------------------------------------------------------------------- */

/* Fill the whole 192x192 box with one colour (the "no race" placeholder, and the clear
 * that precedes a paint is not needed because every pixel is written). */
static void BmapFillBox(uint16_t* fb, uint16_t colour) {
    int x;
    for (x = 0; x < BMAP_PX; x++) {
        uint16_t* col = fb + (size_t) (BMAP_X0 + x) * BMAP_FB_H + (BMAP_FB_H - 1 - (BMAP_Y0 + BMAP_PX - 1));
        int k;
        for (k = 0; k < BMAP_PX; k++) {
            col[k] = colour;
        }
    }
}

/* One 3x3-source-pixel marker == 9x9 screen pixels, clipped to the box. The game stamps
 * gDPFillRectangle(x-1, y-1, x+1, y+1), i.e. exactly 3x3 source pixels. */
static void BmapMarker(uint16_t* fb, int srcCol, int srcRow, uint16_t colour) {
    int dc;
    for (dc = -1; dc <= 1; dc++) {
        const int c = srcCol + dc;
        int sx;
        if (c < 0 || c >= BMAP_SRC_DIM) {
            continue;
        }
        for (sx = 0; sx < BMAP_SCALE; sx++) {
            const int x = BMAP_X0 + c * BMAP_SCALE + sx;
            uint16_t* col = fb + (size_t) x * BMAP_FB_H;
            int dr;
            for (dr = -1; dr <= 1; dr++) {
                const int r = srcRow + dr;
                int sy;
                if (r < 0 || r >= BMAP_SRC_DIM) {
                    continue;
                }
                for (sy = 0; sy < BMAP_SCALE; sy++) {
                    const int y = BMAP_Y0 + r * BMAP_SCALE + sy;
                    col[BMAP_FB_H - 1 - y] = colour;
                }
            }
        }
    }
}

/* 24-bit bottom-up BMP of the whole 320x240 bottom screen, read back out of the same
 * framebuffer the LCD scans out. This is the bottom-screen half of the ORACLE's SHOT:
 * gdx3ds_oracle.c cannot take it, because its capture reads the citro3d MAIN TARGET and
 * the bottom screen is not a citro3d target at all. */
static long BmapWriteShot(const char* path, const uint16_t* fb) {
    const unsigned rowBytes = (BMAP_FB_W * 3u + 3u) & ~3u;
    const uint32_t imageBytes = rowBytes * BMAP_FB_H;
    const uint32_t fileBytes = 54u + imageBytes;
    /* 8 KiB, not the ORACLE's 64: this is always-resident BSS in a build whose feature is
       off by default, and 8 KiB still coalesces the 240 row writes into ~29 FS transactions. */
    static unsigned char sWriteBuf[8u * 1024u];
    unsigned char header[54];
    unsigned char row[BMAP_FB_W * 3u + 3u];
    FILE* f;
    unsigned y;

    mkdir("autotest", 0777); /* EEXIST is fine; CWD-relative == sdmc root on 3DS */
    f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    setvbuf(f, (char*) sWriteBuf, _IOFBF, sizeof(sWriteBuf));
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    header[2] = (unsigned char) (fileBytes);
    header[3] = (unsigned char) (fileBytes >> 8);
    header[4] = (unsigned char) (fileBytes >> 16);
    header[5] = (unsigned char) (fileBytes >> 24);
    header[10] = 54;
    header[14] = 40;
    header[18] = (unsigned char) (BMAP_FB_W & 0xFF);
    header[19] = (unsigned char) (BMAP_FB_W >> 8);
    header[22] = (unsigned char) (BMAP_FB_H & 0xFF);
    header[23] = (unsigned char) (BMAP_FB_H >> 8);
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, sizeof(header), f);
    for (y = 0; y < BMAP_FB_H; y++) {
        const unsigned sy = BMAP_FB_H - 1u - y; /* BMP rows are bottom-up */
        unsigned x;
        memset(row, 0, rowBytes);
        for (x = 0; x < BMAP_FB_W; x++) {
            const uint16_t p = fb[(size_t) x * BMAP_FB_H + (BMAP_FB_H - 1u - sy)];
            const unsigned r = (p >> 11) & 31u;
            const unsigned g = (p >> 5) & 63u;
            const unsigned b = p & 31u;
            row[x * 3u + 0] = (unsigned char) ((b << 3) | (b >> 2));
            row[x * 3u + 1] = (unsigned char) ((g << 2) | (g >> 4));
            row[x * 3u + 2] = (unsigned char) ((r << 3) | (r >> 2));
        }
        fwrite(row, 1, rowBytes, f);
    }
    fclose(f);
    return (long) fileBytes;
}

static void BmapServiceShot(const uint16_t* fb, const GdxRaceMapSnapshot* snap) {
    char path[80];
    char tag[40];
    long bytes;
    u64 t0;
    int ms;
    if (!__atomic_exchange_n(&sShotArmed, 0, __ATOMIC_ACQ_REL)) {
        return;
    }
    snprintf(tag, sizeof(tag), "%s", sPendingShot);
    snprintf(path, sizeof(path), "autotest/%s_B.bmp", tag);
    t0 = svcGetSystemTick();
    bytes = BmapWriteShot(path, fb);
    ms = (int) ((svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC);
    if (bytes < 0) {
        BmapLog("[bmap] SHOT write failed: %s ms=%d", path, ms);
    } else {
        BmapLog("[bmap] SHOT captured -> %s bytes=%ld ms=%d valid=%d mode=%d markers=%d", path, bytes, ms,
                snap->valid, snap->mode, snap->markerCount);
    }
}

int gdx3ds_racemap_paint(unsigned short* fbIn) {
    uint16_t* fb = (uint16_t*) fbIn;
    GdxRaceMapSnapshot snap;
    const unsigned char* tex = NULL;
    unsigned short pal5551[4];
    const u64 t0 = svcGetSystemTick();
    int painted = 0;
    int srcCol;
    int i;

    if (!sEnabled || fb == NULL) {
        return 0;
    }
    gdx_racemap_snapshot(&snap);

    if (!snap.valid || !gdx_racemap_bitmap(&tex, pal5551)) {
        BmapFillBox(fb, BMAP_BG565);
        BmapServiceShot(fb, &snap);
        return 0;
    }

    /* The TLUT arrives as host-order RGBA5551 (port/gdx_racemap.h undoes minimap.c's
       PORT-only pre-swap). CLEAR is alpha-0 and becomes the panel colour. */
    for (i = 0; i < 4; i++) {
        sPal565[i] = (i == 0) ? BMAP_BG565 : Rgba5551To565(pal5551[i]);
    }

    /* Column strips: one per source column, memcpy'd across the 3 identical screen
       columns. `strip[k]` is screen y = BMAP_Y0 + BMAP_PX - 1 - k, i.e. the framebuffer's
       own ascending-index order for this column. */
    for (srcCol = 0; srcCol < BMAP_SRC_DIM; srcCol++) {
        uint16_t strip[BMAP_PX];
        int k;
        int dx;
        for (k = 0; k < BMAP_PX; k++) {
            const int srcRow = (BMAP_PX - 1 - k) / BMAP_SCALE;
            strip[k] = sPal565[tex[(size_t) srcRow * BMAP_SRC_DIM + srcCol] & 3u];
        }
        for (dx = 0; dx < BMAP_SCALE; dx++) {
            const int x = BMAP_X0 + srcCol * BMAP_SCALE + dx;
            memcpy(fb + (size_t) x * BMAP_FB_H + (BMAP_FB_H - 1 - (BMAP_Y0 + BMAP_PX - 1)), strip, sizeof(strip));
        }
    }

    for (i = 0; i < snap.markerCount; i++) {
        const GdxRaceMapMarker* m = &snap.markers[i];
        if (m->blink && !snap.blinkOn) {
            continue; /* the game's own (gGameFrameCount % 16) < 8 blink */
        }
        if ((unsigned) m->kind < 6u) {
            BmapMarker(fb, m->col, m->row, Rgba5551To565(kMarkerRgba5551[m->kind]));
        }
    }
    painted = 1;

    {
        /* The self-timing window CLOSES HERE, before the SHOT write. A bottom-screen BMP is
           ~230 KB written row by row on this same thread and costs ~50 ms (the `[bmap] SHOT`
           receipts say so), so leaving it inside made `max_us` a screenshot timer and inflated
           `avg_us` by the shot rate -- exactly lodburst's finding about `[dynlod] max=`. The
           first ON run measured avg_us=1361-1956 / max_us=51616-54338 with shots on; with
           `[debug] shots=0` the same build measures avg_us=963-977 / max_us=1160-1276, and
           those are the honest numbers. Quote a `[bmap]` figure only from a shots=0 run. */
        const unsigned long dt = (unsigned long) (svcGetSystemTick() - t0);
        sPaints++;
        sPaintTicksTotal += dt;
        if (dt > sPaintTicksMax) {
            sPaintTicksMax = dt;
        }
        /* Self-measurement, ~every 4 s at 60 Hz: the perf claim in the progress doc is this
           line, not a guess. us = ticks * 1e6 / CPU_TICKS_PER_SEC, integer-only. */
        if ((sPaints % 256u) == 0u) {
            const unsigned long ticksPerMs = (unsigned long) CPU_TICKS_PER_MSEC;
            const unsigned long avgUs =
                (unsigned long) (((u64) (sPaintTicksTotal / 256u) * 1000u) / ticksPerMs);
            const unsigned long maxUs = (unsigned long) (((u64) sPaintTicksMax * 1000u) / ticksPerMs);
            BmapLog("[bmap] paints=%lu avg_us=%lu max_us=%lu markers=%d mode=%d", sPaints, avgUs, maxUs,
                    snap.markerCount, snap.mode);
            sPaintTicksTotal = 0;
            sPaintTicksMax = 0;
        }
    }
    BmapServiceShot(fb, &snap);
    return painted;
}

void gdx3ds_racemap_status_line(char* buf, int cap) {
    GdxRaceMapSnapshot snap;
    if (buf == NULL || cap <= 0) {
        return;
    }
    if (!sEnabled) {
        snprintf(buf, (size_t) cap, "race map off ([debug] bottom_map=1)");
        return;
    }
    gdx_racemap_snapshot(&snap);
    if (!snap.valid) {
        snprintf(buf, (size_t) cap, "no race (mode %d)", snap.mode);
        return;
    }
    snprintf(buf, (size_t) cap, "LAP %d/%d   POS %d/%d   %dP", snap.lap, snap.totalLaps, snap.position,
             snap.totalRacers, snap.numPlayers);
}
