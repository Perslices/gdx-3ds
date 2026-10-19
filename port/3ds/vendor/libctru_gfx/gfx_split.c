/* port/3ds/vendor/libctru_gfx/gfx_split.c -- libctru v2.7.0 source/gfx.c, ALTERED (n60boot).
 *
 * Origin: https://github.com/devkitPro/libctru, tag v2.7.0 (36fe1ad), libctru/source/gfx.c,
 * zlib license (LICENSE next to this file). This is an altered version, not the original:
 *
 *   - Framebuffer memory is chosen PER SCREEN (top / bottom) instead of once for both.
 *     Upstream gfxInit(..., vrambuffers) sets one allocator for both screens, so
 *     [native] lcd_vram=1 put the BOTTOM framebuffers in VRAM too; the CIA maps VRAM
 *     read-only (packaging/gdiffuser.rsf), and the first CPU store into the bottom screen
 *     (consoleInit's clear, the launch prompt, the fps HUD, the menu) was a permission data
 *     abort: card n60-3d4 died at boot on the Old 3DS. gdx3ds_gfx_init_split() puts each
 *     screen where the caller says; gfxInit() keeps its upstream meaning (both screens alike).
 *   - The present "DMA size" bits follow the screen's own memory (upstream: one flag).
 *   - A screen whose VRAM allocation fails falls back to the linear heap (upstream: NULL).
 *   - gdx3ds_gfx_move_screen_to_linear(): re-home one screen's framebuffers in the linear
 *     heap at run time (the VRAM guard's repair, gdx3ds_vram_guard.c).
 *   - Brace style reformatted to the project's (K&R).
 *
 * Linked into G-Diffuser-3DS as an object, so every gfx* symbol resolves here and libctru.a's
 * gfx.o is never pulled (a missing symbol would pull it and fail the link on duplicates). */
#include <stdbool.h>
#include <string.h>

#include <3ds/types.h>
#include <3ds/allocator/linear.h>
#include <3ds/allocator/vram.h>
#include <3ds/services/gspgpu.h>
#include <3ds/gfx.h>
#include <3ds/os.h>

#include "gdx3ds_gfx_split.h"

static u8* gfxTopFramebuffers[2];
static u8* gfxBottomFramebuffers[2];
static u32 gfxTopFramebufferMaxSize;
static u32 gfxBottomFramebufferMaxSize;
static GSPGPU_FramebufferFormat gfxFramebufferFormats[2];

static enum {
    MODE_2D = 0,
    MODE_3D = 1,
    MODE_WIDE = 2,
} gfxTopMode;
static bool gfxIsVram[2]; /* n60boot: per screen (upstream: one bool) */
static bool gfxInited;    /* upstream used screenFree != NULL */
static u8 gfxCurBuf[2];
static u8 gfxIsDoubleBuf[2];

static void FreeFramebuffer(void* mem);

/* Both buffers of a screen in one memory: a VRAM screen whose second buffer does not fit moves
 * whole to the linear heap (the present mode bits are per screen, not per buffer). */
static void AllocFramebufferPair(gfxScreen_t screen, size_t size, u8** out) {
    if (gfxIsVram[screen]) {
        out[0] = (u8*)vramAlloc(size);
        out[1] = (u8*)vramAlloc(size);
        if (out[0] != NULL && out[1] != NULL) {
            return;
        }
        FreeFramebuffer(out[0]);
        FreeFramebuffer(out[1]);
        gfxIsVram[screen] = false; /* VRAM full: this screen lives in the linear heap */
    }
    out[0] = (u8*)linearAlloc(size);
    out[1] = (u8*)linearAlloc(size);
}

static void FreeFramebuffer(void* mem) {
    if (mem == NULL) {
        return;
    }
    if (gdx3ds_gfx_addr_in_vram(mem)) {
        vramFree(mem);
    } else {
        linearFree(mem);
    }
}

static u8** ScreenFramebuffers(gfxScreen_t screen) {
    return screen == GFX_TOP ? gfxTopFramebuffers : gfxBottomFramebuffers;
}

static u32* ScreenMaxSize(gfxScreen_t screen) {
    return screen == GFX_TOP ? &gfxTopFramebufferMaxSize : &gfxBottomFramebufferMaxSize;
}

void gfxSet3D(bool enable) {
    gfxTopMode = enable ? MODE_3D : MODE_2D;
}

bool gfxIs3D(void) {
    return gfxTopMode == MODE_3D;
}

void gfxSetWide(bool enable) {
    gfxTopMode = enable ? MODE_WIDE : MODE_2D;
}

bool gfxIsWide(void) {
    return gfxTopMode == MODE_WIDE;
}

void gfxSetScreenFormat(gfxScreen_t screen, GSPGPU_FramebufferFormat format) {
    u32 reqSize = GSP_SCREEN_WIDTH * gspGetBytesPerPixel(format);
    u8** framebuffers = ScreenFramebuffers(screen);
    u32* maxSize = ScreenMaxSize(screen);
    reqSize *= screen == GFX_TOP ? GSP_SCREEN_HEIGHT_TOP_2X : GSP_SCREEN_HEIGHT_BOTTOM;

    if (*maxSize < reqSize) {
        FreeFramebuffer(framebuffers[0]);
        FreeFramebuffer(framebuffers[1]);
        AllocFramebufferPair(screen, reqSize, framebuffers);
        *maxSize = reqSize;
    }

    gfxFramebufferFormats[screen] = format;
}

GSPGPU_FramebufferFormat gfxGetScreenFormat(gfxScreen_t screen) {
    return gfxFramebufferFormats[screen];
}

void gfxSetDoubleBuffering(gfxScreen_t screen, bool enable) {
    gfxIsDoubleBuf[screen] = enable ? 1 : 0; // make sure they're the integer values '1' and '0'
}

static void gfxPresentFramebuffer(gfxScreen_t screen, u8 id, bool hasStereo) {
    u32 stride = GSP_SCREEN_WIDTH * gspGetBytesPerPixel(gfxFramebufferFormats[screen]);
    u32 mode = gfxFramebufferFormats[screen];

    const u8* fb_a;
    const u8* fb_b;
    if (screen == GFX_TOP) {
        fb_a = gfxTopFramebuffers[id];
        switch (gfxTopMode) {
            default:
            case MODE_2D:
                mode |= BIT(6);
                fb_b = fb_a;
                break;
            case MODE_3D:
                mode |= BIT(5);
                fb_b = hasStereo ? (fb_a + gfxTopFramebufferMaxSize / 2) : fb_a;
                break;
            case MODE_WIDE:
                fb_b = fb_a;
                break;
        }
    } else {
        fb_a = gfxBottomFramebuffers[id];
        fb_b = fb_a;
    }

    if (!gfxIsVram[screen]) {
        mode |= 1 << 8;
    } else {
        mode |= 3 << 8;
    }

    gspPresentBuffer(screen, id, fb_a, fb_b, stride, mode);
}

void gdx3ds_gfx_init_split(GSPGPU_FramebufferFormat topFormat, GSPGPU_FramebufferFormat bottomFormat, bool topInVram,
                           bool bottomInVram) {
    gfxIsVram[GFX_TOP] = topInVram;
    gfxIsVram[GFX_BOTTOM] = bottomInVram;
    gfxInited = true;

    // Initialize GSP
    gspInit();

    // Initialize configuration
    gfxSet3D(false);
    gfxSetScreenFormat(GFX_TOP, topFormat);
    gfxSetScreenFormat(GFX_BOTTOM, bottomFormat);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);

    // Present the framebuffers
    gfxCurBuf[0] = gfxCurBuf[1] = 0;
    gfxPresentFramebuffer(GFX_TOP, 0, false);
    gfxPresentFramebuffer(GFX_BOTTOM, 0, false);

    // Wait for VBlank and turn the LCD on
    gspWaitForVBlank();
    GSPGPU_SetLcdForceBlack(0x0);
}

void gfxInit(GSPGPU_FramebufferFormat topFormat, GSPGPU_FramebufferFormat bottomFormat, bool vrambuffers) {
    gdx3ds_gfx_init_split(topFormat, bottomFormat, vrambuffers, vrambuffers);
}

void gfxInitDefault(void) {
    gfxInit(GSP_BGR8_OES, GSP_BGR8_OES, false);
}

void gfxExit(void) {
    if (!gfxInited) {
        return;
    }

    if (gspHasGpuRight()) {
        // Wait for VBlank and turn the LCD off
        gspWaitForVBlank();
        GSPGPU_SetLcdForceBlack(0x1);
    }

    // Free framebuffers
    FreeFramebuffer(gfxTopFramebuffers[0]);
    FreeFramebuffer(gfxTopFramebuffers[1]);
    FreeFramebuffer(gfxBottomFramebuffers[0]);
    FreeFramebuffer(gfxBottomFramebuffers[1]);
    gfxTopFramebuffers[0] = gfxTopFramebuffers[1] = NULL;
    gfxBottomFramebuffers[0] = gfxBottomFramebuffers[1] = NULL;
    gfxTopFramebufferMaxSize = gfxBottomFramebufferMaxSize = 0;

    // Deinitialize GSP
    gspExit();

    gfxInited = false;
}

u8* gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16* width, u16* height) {
    unsigned id = gfxCurBuf[screen] ^ gfxIsDoubleBuf[screen];
    unsigned scr_width = GSP_SCREEN_WIDTH;
    unsigned scr_height;
    u8* fb;

    if (screen == GFX_TOP) {
        fb = gfxTopFramebuffers[id];
        scr_height = GSP_SCREEN_HEIGHT_TOP;
        switch (gfxTopMode) {
            default:
            case MODE_2D:
                break;
            case MODE_3D:
                if (side != GFX_LEFT) {
                    fb += gfxTopFramebufferMaxSize / 2;
                }
                break;
            case MODE_WIDE:
                scr_height = GSP_SCREEN_HEIGHT_TOP_2X;
                break;
        }
    } else { // GFX_BOTTOM
        fb = gfxBottomFramebuffers[id];
        scr_height = GSP_SCREEN_HEIGHT_BOTTOM;
    }

    if (width) {
        *width = scr_width;
    }
    if (height) {
        *height = scr_height;
    }

    return fb;
}

void gfxFlushBuffers(void) {
    const u32 baseSize = GSP_SCREEN_WIDTH * gspGetBytesPerPixel(gfxGetScreenFormat(GFX_TOP));
    const u32 topSize = GSP_SCREEN_HEIGHT_TOP * baseSize;
    const u32 topSize2x = GSP_SCREEN_HEIGHT_TOP_2X * baseSize;
    const u32 bottomSize = GSP_SCREEN_HEIGHT_BOTTOM * baseSize;

    GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), gfxTopMode == MODE_WIDE ? topSize2x : topSize);
    if (gfxTopMode == MODE_3D) {
        GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL), topSize);
    }
    GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL), bottomSize);
}

void gfxScreenSwapBuffers(gfxScreen_t scr, bool hasStereo) {
    gfxCurBuf[scr] ^= gfxIsDoubleBuf[scr];
    gfxPresentFramebuffer(scr, gfxCurBuf[scr], hasStereo);
}

void gfxConfigScreen(gfxScreen_t scr, bool immediate) {
    (void)immediate;
    gfxScreenSwapBuffers(scr, true);
}

void gfxSwapBuffers(void) {
    gfxScreenSwapBuffers(GFX_TOP, true);
    gfxScreenSwapBuffers(GFX_BOTTOM, true);
}

void gfxSwapBuffersGpu(void) {
    gfxScreenSwapBuffers(GFX_TOP, true);
    gfxScreenSwapBuffers(GFX_BOTTOM, true);
}

/* ---------------------------------------------------------------- n60boot additions ---- */

bool gdx3ds_gfx_addr_in_vram(const void* addr) {
    const u32 a = (u32)addr;
    return a >= OS_VRAM_VADDR && a < OS_VRAM_VADDR + OS_VRAM_SIZE;
}

bool gdx3ds_gfx_screen_in_vram(gfxScreen_t screen) {
    const u8* const* framebuffers = (const u8* const*)ScreenFramebuffers(screen);
    return gdx3ds_gfx_addr_in_vram(framebuffers[0]) || gdx3ds_gfx_addr_in_vram(framebuffers[1]);
}

void gdx3ds_gfx_framebuffer_pair(gfxScreen_t screen, const void** out0, const void** out1) {
    const u8* const* framebuffers = (const u8* const*)ScreenFramebuffers(screen);
    *out0 = framebuffers[0];
    *out1 = framebuffers[1];
}

/* Both new buffers are allocated before anything changes, so a failure leaves the screen as it
 * was. The old ones are freed only after a vblank has latched the new scanout address. */
bool gdx3ds_gfx_move_screen_to_linear(gfxScreen_t screen) {
    u8** framebuffers = ScreenFramebuffers(screen);
    const u32 size = *ScreenMaxSize(screen);
    if (!gfxInited || !gdx3ds_gfx_screen_in_vram(screen)) {
        return true;
    }
    u8* fresh0 = (u8*)linearAlloc(size);
    u8* fresh1 = (u8*)linearAlloc(size);
    if (fresh0 == NULL || fresh1 == NULL) {
        linearFree(fresh0);
        linearFree(fresh1);
        return false;
    }
    memset(fresh0, 0, size);
    memset(fresh1, 0, size);
    GSPGPU_FlushDataCache(fresh0, size);
    GSPGPU_FlushDataCache(fresh1, size);
    u8* old0 = framebuffers[0];
    u8* old1 = framebuffers[1];
    framebuffers[0] = fresh0;
    framebuffers[1] = fresh1;
    gfxIsVram[screen] = false;
    gfxPresentFramebuffer(screen, gfxCurBuf[screen], false);
    gspWaitForVBlank();
    FreeFramebuffer(old0);
    FreeFramebuffer(old1);
    return true;
}
