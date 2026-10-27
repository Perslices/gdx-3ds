/* port/3ds/lus_glue/gdx3ds_input_glue.c — pad-read seam for port/input_bridge.c.
 *
 * Desktop defines gdx_lus_read_pads in port/main.cpp against the LUS ControlDeck (SDL
 * gamepads, hotplug, multi-port routing). On 3DS the built-in HID is the only device:
 * stream B's gdx3ds_os_poll_input already produces N64-mapped buttons + a scaled
 * -80..80 stick, so this is a straight demultiplex; pads 1..3 report disconnected.
 *
 * gdx_request_quit (the GDX_INPUT_SCRIPT QUIT hook) closes the window the same way the
 * desktop path does — through the window's running flag, which the frame loop polls.
 */

#include "gdx3ds_os.h"
#include "gdx3ds_motion.h"

#include <string.h>

#define GDX3DS_MAXCONTROLLERS 4

/* MOTION gating. Both live in the decomp side of the link; declared weak so a build that
 * stages this glue without the game (and the host stub) still links, and treats "absent" as
 * "no gameplay" -- i.e. motion never fires rather than firing everywhere. */
extern int gdx_input_in_gameplay(void) __attribute__((weak)); /* port/input_bridge.c */
extern signed char gGamePaused __attribute__((weak));         /* decomp: game state latch */

/* 1 only while the player is actually driving: a live race and not the pause overlay. Tilt or
 * gyro steering on a menu tick would move the same stick the MENU and the pause menu read to
 * walk their cursors, so both are excluded here rather than deeper in the pipeline. */
static int MotionAllowed(void) {
    if (&gdx_input_in_gameplay == NULL || !gdx_input_in_gameplay()) {
        return 0;
    }
    if (&gGamePaused != NULL && gGamePaused) {
        return 0;
    }
    return 1;
}

int gdx_lus_read_pads(int capacity, unsigned short* outButtons, signed char* outStickX, signed char* outStickY,
                      unsigned char* outConnected) {
    Gdx3dsPadState pads[GDX3DS_MAXCONTROLLERS];
    int ports = (capacity < GDX3DS_MAXCONTROLLERS) ? capacity : GDX3DS_MAXCONTROLLERS;
    int i;

    for (i = 0; i < capacity; i++) {
        if (outButtons != NULL) {
            outButtons[i] = 0;
        }
        if (outStickX != NULL) {
            outStickX[i] = 0;
        }
        if (outStickY != NULL) {
            outStickY[i] = 0;
        }
        if (outConnected != NULL) {
            outConnected[i] = 0;
        }
    }
    if (ports <= 0) {
        return 0;
    }

    memset(pads, 0, sizeof(pads));
    gdx3ds_os_poll_input(pads, ports);

    /* MOTION: tilt is an ADDITIVE second input on port 0 only -- the circle pad keeps working
     * exactly as before, which is why this runs on the finished -80..80 stick instead of
     * replacing it. Roll adds to stickX (steering); pitch adds to stickY, which the decomp feeds
     * to racer->tiltUpInput (decomp/src/game/racer.c:3451). set_active() is called every frame
     * (not just when allowed) so the module sees the edge that ends a race and drops its
     * latched contribution and stale neutral pose. */
    {
        const int motion = MotionAllowed();
        gdx3ds_motion_set_active(motion);
        if (motion) {
            int sx = pads[0].stickX;
            int sy = pads[0].stickY;
            gdx3ds_motion_apply(&sx, &sy);
            pads[0].stickX = (signed char) sx;
            pads[0].stickY = (signed char) sy;
        }
    }

    for (i = 0; i < ports; i++) {
        if (outButtons != NULL) {
            outButtons[i] = pads[i].buttons;
        }
        if (outStickX != NULL) {
            outStickX[i] = pads[i].stickX;
        }
        if (outStickY != NULL) {
            outStickY[i] = pads[i].stickY;
        }
        if (outConnected != NULL) {
            outConnected[i] = pads[i].connected;
        }
    }
    return 1;
}

/* Set by main_3ds.cpp once the Fast3dWindow exists; polled nowhere before that, so a
 * script QUIT during early boot degrades to a no-op instead of a crash. */
static volatile int sGdxQuitRequested = 0;

void gdx_request_quit(void) {
    sGdxQuitRequested = 1;
}

int gdx3ds_quit_requested(void) {
    return sGdxQuitRequested;
}
