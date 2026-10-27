/* port/3ds/gdx3ds_menu.c — MENU: v1 bottom-screen touch menu. Contract in the header.
 *
 * Console mechanics: same window-swap trick as gdx3ds_fps_hud.c (consoleSetWindow is
 * real; CSI H is window-relative). Every paint batch swaps the window to the full
 * console, addresses rows absolutely ("\x1b[<row>;1H\x1b[K" — position + clear line, so
 * stale characters never survive without pad math), then parks the stdout scroll
 * window on row 30 and restores the cursor, so stray printf traffic (debug.console)
 * stays confined to the last row.
 *
 * Allocation discipline: integer-only snprintf into fixed buffers, fwrite(stdout)
 * (newlib float formatting can malloc via dtoa; console stdout is unbuffered).
 *
 * Touch: hit zones are console cells (bottom screen 320x240 px / 8px font = 40x30
 * cells). Actions fire on the touch-DOWN edge only; the hid state is whatever
 * gdx3ds_os_poll_input latched this frame (this TU never calls hidScanInput — a
 * second scan per frame would eat the game's key-down edges).
 */
#include "gfx/gdx3ds_native_cfg.h"
#include "gdx3ds_menu.h"
#include "gdx3ds_dynlod.h"
#include "gdx3ds_perf_preset.h"
#include "gdx3ds_fps_hud.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_frameskip.h"
#include "gdx3ds_racemap.h"
#include "gdx3ds_bottomhud.h"
#include "gdx3ds_menu_tabbar_layout.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>  /* uint16_t -- MenuWriteShot's own SHOT writer, see gdx3ds_racemap.c/gdx3ds_bottomhud.c */
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strcasecmp, for the [menu] tab key */
#include <sys/stat.h> /* mkdir, MenuWriteShot's own SHOT writer */

#include <3ds.h>

#include "gdx3ds_config.h"     /* gdx3ds_os PUBLIC include dir */
#include "gdx3ds_input_map.h"
#include "gdx3ds_motion.h"     /* MOTION: INPUT tab rows 23..28 */
#include "gdx3ds_vram_guard.h" /* n60boot */
#include "gfx/gdx3ds_gpu_prof.h"

/* Cross-stream live hooks (diagnostic-export idiom: prototypes declared here, see
 * gdx3ds_audio_ndsp.c / gdx3ds_stereo.cpp / gfx_citro3d.cpp). */
extern int gdx3ds_audio_output_active(void);
extern void gdx3ds_audio_set_master_volume(int pct);
extern int gdx3ds_audio_get_master_volume(void);
extern void gdx3ds_audio_set_diag(int on);
extern int gdx3ds_audio_get_diag(void);
extern int gdx3ds_stereo_runtime_enabled(void);
extern void gdx3ds_heapwatch_stats(unsigned long* usedKb, unsigned long* hwmKb, long* perMinKb, int* triggers);
extern int gdx3ds_stereo_get_iod_px(void);
extern void gdx3ds_stereo_set_iod_px(int px);
extern int gdx3ds_stereo_get_conv_x100(void);
extern void gdx3ds_stereo_set_conv_x100(int centi);
extern void gdx3ds_disp_set_mode(int mode);

/* ---- geometry ------------------------------------------------------------------ */
#define MENU_COLS 40
#define MENU_ROWS 30
#define MENU_ROW_FPS 1
#define MENU_ROW_TABS 2
#define MENU_ROW_SCROLL 30 /* stdout scroll window parks here */

enum {
    TAB_STATUS = 0,
    TAB_DISP,
    TAB_3D,
    TAB_AUD,
    TAB_INPUT,
    TAB_LOG,
    TAB_DBG,
    TAB_MAP,   /* BOTTOMHUD: gated on [debug] bottom_map; hidden (and unreachable) when off */
    TAB_RACE,  /* BOTTOMHUD: gated on the DASH/idle view being dismissed; see MENU_TAB_HIDDEN */
    TAB_ABOUT,
    TAB_COUNT
};
/* nibottom Phase 2: the DASH race HUD is NOT a tab — see gdx3ds_bottomhud.h's "FULL-SCREEN
 * TAKEOVER, NOT A MENU TAB" note. gdx3ds_menu_tick_body hands the whole tick to it whenever
 * gdx3ds_bottomhud_race_live() is 1 (and the DASH hasn't been dismissed -- see TAB_RACE) and
 * returns before touching the tab bar at all. The idle view (gdx3ds_bottomhud_idle_active(),
 * outside a race) is the same shape -- also not a tab, also a full-screen hand-off.
 *
 * TAB_RACE (second visual-feedback round, item 4; idle-view round: repurposed to also cover the
 * idle view, not just the race) is not a real content tab: it has no PaintXxx/TouchXxx case below
 * (TouchTabBar special-cases it instead, see that function). It exists only as a tab-bar LABEL,
 * visible whenever EITHER full-screen takeover has been dismissed back to the console: mid-race
 * (painted "RACE", tapping it calls gdx3ds_bottomhud_resume()) or outside a race with the idle
 * view enabled (painted "DASH", tapping it calls gdx3ds_bottomhud_idle_resume()) -- the two cases
 * are mutually exclusive (one requires a live race, the other requires none), so this one tab
 * slot covers both with no new tab-bar column -- see MenuTabRaceLabel/TouchTabBar. */

/* Tab bar: "STAT DISP 3D AUD INP LOG DBG [MAP] ABT" + the screen-off button, right-aligned.
 *
 * BOTTOMHUD: the columns used to be a hand-maintained table, which cannot survive a tab
 * whose PRESENCE depends on an ini key. They are now walked every tick in MenuLayoutTabs()
 * (hw bug n60-3d8, item 1 -- see that function's own comment for why "once" was the bug) from
 * the labels themselves -- label, one space, next label, starting at column 1 -- which
 * reproduces the old table exactly when MAP is hidden (STAT 1-4, DISP 6-9, 3D 11-12,
 * AUD 14-16, INP 18-20, LOG 22-24, DBG 26-28, ABT 30-32, off button 36-40) and shifts ABT
 * to 34-36 and the off button to 38-40 when it is shown. With [debug] bottom_map=0 the
 * bottom screen is therefore the one it has always been, to the column. */
/* COORDINATOR FIX (hw bug n60-3d7, item 5): "the RACE tab is truncated off the tab bar and
 * impossible to tap... shorten labels". With MAP and RACE both shown (the exact moment the user
 * needs RACE: mid-race, dismissed, MAP tab also unlocked) the prior full labels needed 41
 * columns of a 40-column bar before the OFF button even got a look-in -- RACE's own last column
 * landed on top of OFF's hit box, and OFF (checked first in TouchTabBar) silently stole the tap.
 * STAT/DISP/INP/LOG/ABT shortened by one column each (5 total) so all ten tabs plus a same-row
 * OFF button fit with a column to spare even in the worst case; RACE and MAP stay full-length
 * since they're the two tabs this fix is actually about. MenuLayoutTabs' sOffC0 below no longer
 * assumes a fixed worst case either -- see its own comment. */
static const char* const kTabLabels[TAB_COUNT] = {
    "STA", "DSP", "3D", "AUD", "IN", "LG", "DBG", "MAP", "RACE", "AB",
};
static Gdx3dsMenuTabCols sTabCols[TAB_COUNT];
static int sMapTabShown = 0;   /* == gdx3ds_racemap_enabled(), latched at init */
static int sRaceHudEnabled = 0; /* == gdx3ds_bottomhud_enabled(), latched at init: [debug] bottom_race */
static int sOffC0 = 36;
static int sOffC1 = 40;

/* TAB_RACE is visible whenever EITHER full-screen takeover has been dismissed back to the
 * console -- the race-DASH case (unchanged) or the idle-view case (new). Mutually exclusive by
 * construction: the race case requires gdx3ds_bottomhud_race_live(), the idle case requires its
 * negation, so at most one of these two can ever be true at once. */
#define MENU_TAB_RACE_DASH_VISIBLE \
    (sRaceHudEnabled && gdx3ds_bottomhud_dismissed() && gdx3ds_bottomhud_race_live())
#define MENU_TAB_RACE_IDLE_VISIBLE \
    (gdx3ds_bottomhud_idle_dismissed() && !gdx3ds_bottomhud_race_live())
#define MENU_TAB_HIDDEN(t) \
    (((t) == TAB_MAP && !sMapTabShown) || \
     ((t) == TAB_RACE && !(MENU_TAB_RACE_DASH_VISIBLE || MENU_TAB_RACE_IDLE_VISIBLE)))

/* COORDINATOR FIX (hw bug n60-3d8, item 1: "the RACE tab was on debug but unclickable because
 * OFF was covering it"). This used to run exactly ONCE, from gdx3ds_menu_init, and cache
 * sTabCols[]/sOffC0/sOffC1 for the rest of the process's life. TAB_RACE's own visibility is
 * NOT fixed at init time, though -- MENU_TAB_HIDDEN(TAB_RACE) reads
 * gdx3ds_bottomhud_dismissed()/race_live() live, and RACE only ever becomes reachable mid-race
 * (almost always well after init, when it was hidden). PaintTabBar() re-checks
 * MENU_TAB_HIDDEN() itself on every paint and draws whatever is visible RIGHT NOW, so once
 * RACE turned visible its label WAS drawn in the live tab bar -- but TouchTabBar() (and OFF's
 * own drawn position in PaintTabBar) were still using the stale, init-time sTabCols/sOffC0,
 * computed back when RACE was hidden. OFF's touch box -- checked FIRST in TouchTabBar -- ended
 * up starting inside the columns RACE's label was now actually drawn in, so OFF silently ate
 * the tap meant for RACE. The fix: call this every tick (gdx3ds_menu_tick_body, cheap --
 * TAB_COUNT strlen() calls on string literals), so whatever PaintTabBar draws and
 * TouchTabBar hit-tests THIS tick is always laid out from THIS tick's own live visibility,
 * never a stale one. The actual column arithmetic moved to gdx3ds_menu_tabbar_layout.h so the
 * host test (gdx3ds_menu_tabbar_host_tests.c) exercises the exact same code this calls. */
static void MenuLayoutTabs(void) {
    int hidden[TAB_COUNT];
    for (int t = 0; t < TAB_COUNT; t++) {
        hidden[t] = MENU_TAB_HIDDEN(t) ? 1 : 0;
    }
    gdx3ds_menu_layout_tabs(kTabLabels, hidden, TAB_COUNT, MENU_COLS, sTabCols, &sOffC0, &sOffC1);
}

/* ---- state --------------------------------------------------------------------- */
static PrintConsole* sCon = NULL;
static int sEnabled = 0;
static int sTab = TAB_STATUS;
static int sDirty = 0;          /* full page repaint requested */
static int sFpsDirty = 0;
static char sFpsLine[44] = "drawn --.- game --.-";
static int sScreenOff = 0;
static int sBacklightOff = 0;   /* WE powered the bottom backlight off (gsp::Lcd) */
static volatile int sLcdSessionOpen = 0; /* 1 only inside LcdSetBottomBacklight */
static int sPrevTouching = 0;
static int sWakeSwallow = 0;    /* swallow the touch that woke the screen */
static u64 sLastPeriodicTick = 0;
/* n60st3 [menu] race_quiet (default 0): during an on-track race the ~1 Hz refresh repaints only the
 * fps row, not the live page (STATUS walks the heap with mallinfo and repaints ~20 console rows on
 * the main thread: hw-n60-3d2 showed a 6-10 ms `tl` every ~50 iterations, one doubled frame a
 * second). The page repaints on the first refresh after the race. Bottom screen only. */
static int sRaceQuiet = 0;
extern int gdx_input_in_gameplay(void) __attribute__((weak)); /* port/input_bridge.c */
static u32 sPrevHeld = 0;
/* nidle verification gap: the plain tab-bar console UI (this file's own default screen, owner
 * whenever neither the race DASH nor the idle view's own takeover above applies) had no SHOT
 * hook at all -- gdx_request_frame_dump only knew about gpuprof/racemap/bottomhud. Same
 * independent-instance shape as gdx3ds_racemap.c's / gdx3ds_bottomhud.c's own sPendingShot/
 * sShotArmed pair; see MenuWriteShot/MenuServiceShot below. */
static char sPendingShot[40];
static int sShotArmed = 0;
static int sCaptureAction = -1; /* INPUT tab: action awaiting a key, -1 = none */
static u32 sReadoutPainted = 0;  /* INPUT tab: raw pad values last painted (live readout) */
static int sMotionPainted = 0;   /* INPUT tab: last motion steer painted, quantised (see below) */
static int sMotionCalPainted = 0; /* INPUT tab: TILT calibration state last painted (row 27) */
static u64 sReadoutTick = 0;
static int sLogSkip = 0;        /* LOG tab scroll offset (lines back from live) */
static int sDispMode = 0;       /* mirrors gdx3ds_disp mode for the radio UI */
static int sDbgVerbose = 0;     /* live [debug] verbose latch (main loop reads) */
static int sRivalDetail = 0;    /* [perf] rival_detail latch (Racer_Draw reads) */

/* RIVAL-DETAIL port hook: read once per Racer_Draw by the decomp patch
 * (decomp-port-rival-detail.patch). 0=NATIVE 1=REDUCED 2=MINIMAL. Latched from
 * [perf] rival_detail in gdx3ds_menu_init (even with the menu UI disabled) and
 * live-updated by the DISP tab row. DYNLOD (gdx3ds_dynlod.c): the manual setting is
 * the FLOOR; the controller may raise the effective level up to MINIMAL. */
int gdx_rival_detail_floor(void) {
    return sRivalDetail;
}

int gdx_rival_detail_level(void) {
    return gdx3ds_dynlod_effective_level(sRivalDetail);
}

#define MENU_LOG_LINES 25   /* rows 3..27 */
#define MENU_LOG_ROW0 3
#define MENU_LOG_SKIP_MAX 200
#define MENU_LOG_STEP 20

/* ---- receipts ------------------------------------------------------------------ */
static void MenuLog(const char* fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(buf) - 1) {
            n = (int)sizeof(buf) - 1;
        }
        svcOutputDebugString(buf, (size_t)n);
        gdx3ds_filelog_write(buf, (size_t)n);
    }
}

static void MenuSaveCfg(void) {
    if (gdx3ds_config_save(GDX3DS_CONFIG_DEFAULT_PATH) != 0) {
        MenuLog("[menu] WARNING: ini save failed (%s)", GDX3DS_CONFIG_DEFAULT_PATH);
    }
}

/* ---- console primitives -------------------------------------------------------- */
static int sPaintDepth = 0;
static int sSavedCursorX = 0;
static int sSavedCursorY = 0;

static void PaintBegin(void) {
    if (sPaintDepth++ == 0) {
        sSavedCursorX = sCon->cursorX;
        sSavedCursorY = sCon->cursorY;
        consoleSetWindow(sCon, 1, 1, MENU_COLS, MENU_ROWS);
    }
}

static void PaintEnd(void) {
    if (--sPaintDepth == 0) {
        consoleSetWindow(sCon, 1, MENU_ROW_SCROLL, MENU_COLS, 1);
        sCon->cursorX = sSavedCursorX;
        sCon->cursorY = sSavedCursorY;
        fflush(stdout);
    }
}

/* Position at (row, col 1), clear the line, print (formatted, integer-only). */
static void gdx3ds_menu_tick_body(void);

static void PaintRow(int row, const char* fmt, ...) {
    char text[96];
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    int n = snprintf(line, sizeof(line), "\x1b[%d;1H\x1b[K%s", row, text);
    if (n > 0) {
        fwrite(line, 1, (size_t)n, stdout);
    }
}

static void PaintClearPage(void) {
    for (int r = 3; r <= MENU_ROWS - 1; r++) {
        PaintRow(r, "");
    }
}

/* TAB_RACE's own label text, chosen per the SAME two mutually-exclusive cases
 * MENU_TAB_RACE_DASH_VISIBLE/MENU_TAB_RACE_IDLE_VISIBLE already distinguish -- "RACE" paints
 * exactly when the race case applies (race_live() is 1 whenever this tab is visible at all AND
 * it's not the idle case), "DASH" otherwise. Both are 4 characters, matching kTabLabels[TAB_RACE]
 * ("RACE") exactly, so the column widths MenuLayoutTabs/gdx3ds_menu_layout_tabs already computed
 * from that literal stay correct no matter which string actually gets painted here. */
static const char* MenuTabRaceLabel(void) {
    return gdx3ds_bottomhud_race_live() ? "RACE" : "DASH";
}

/* ---- tab bar ------------------------------------------------------------------- */
static void PaintTabBar(void) {
    char line[160];
    int n = snprintf(line, sizeof(line), "\x1b[%d;1H\x1b[K", MENU_ROW_TABS);
    int first = 1;
    for (int t = 0; t < TAB_COUNT; t++) {
        if (MENU_TAB_HIDDEN(t)) {
            continue;
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%s%s%s", first ? "" : " ",
                      t == sTab ? "\x1b[7m" : "", t == TAB_RACE ? MenuTabRaceLabel() : kTabLabels[t],
                      t == sTab ? "\x1b[0m" : "");
        first = 0;
        if (n >= (int)sizeof(line) - 12) {
            break;
        }
    }
    /* COORDINATOR FIX (hw bug n60-3d7, item 5): pick the 5-column "[OFF]" only when it actually
     * fits before the 40-column edge; this used to key off sMapTabShown alone (the only tab that
     * could ever be conditionally visible when this was written), which stopped being accurate
     * once RACE became a second one -- see MenuLayoutTabs' sOffC0 comment. */
    n += snprintf(line + n, sizeof(line) - (size_t)n, "\x1b[%d;%dH%s", MENU_ROW_TABS, sOffC0,
                  (sOffC0 + 5 - 1 <= MENU_COLS) ? "[OFF]" : "\x1b[7mOFF\x1b[0m");
    fwrite(line, 1, (size_t)n, stdout);
}

/* ---- pages --------------------------------------------------------------------- */
static void PaintStatus(void) {
    struct mallinfo mi = mallinfo();
    PaintRow(4, "heap used %6lu KB  free %6lu KB", (unsigned long)mi.uordblks / 1024u,
             (unsigned long)mi.fordblks / 1024u);
    {
        unsigned long hwmKb = 0; long perMin = 0; int trig = 0;
        gdx3ds_heapwatch_stats(NULL, &hwmKb, &perMin, &trig);
        PaintRow(5, "linear %5lu KB hwm %6lu KB %+ld/min%s", (unsigned long)linearSpaceFree() / 1024u,
                 hwmKb, perMin, trig > 0 ? "!" : "");
    }
    unsigned buildMsX10 = 0, topOpMsX10 = 0;
    int topOp = -1;
    if (gdx3ds_gpuprof_hud_sample(&buildMsX10, &topOp, &topOpMsX10)) {
        if (topOp >= 0) {
            PaintRow(7, "cpu build %u.%u ms  top %02X %u.%u ms", buildMsX10 / 10,
                     buildMsX10 % 10, (unsigned)topOp, topOpMsX10 / 10, topOpMsX10 % 10);
        } else {
            PaintRow(7, "cpu build %u.%u ms", buildMsX10 / 10, buildMsX10 % 10);
        }
    } else {
        PaintRow(7, "cpu build --.- (DBG: gputrace)");
    }
    PaintRow(9, "build %.34s", gdx3ds_fps_hud_build_id());
    PaintRow(11, "audio: %s   3d: %s",
             gdx3ds_audio_output_active() ? "ndsp" : "NULL SINK",
             gdx3ds_stereo_runtime_enabled() ? "on" : "off");
    PaintRow(13, "tap the tabs above; [OFF] sleeps");
    PaintRow(14, "the bottom screen (touch wakes).");
}

static void PaintDispAutoRow(void);

static void PaintDisp(void) {
    static const char* kModes[4] = {
        "AUTHENTIC   stock borders",
        "FULL-BLEED  scene fills 400x240",
        "ZOOM        uniform, thin bands",
        "HYBRID      fill + mild stretch",
    };
    static const char* kRivalNames[4] = { "NATIVE", "REDUCED", "MINIMAL", "LOW" };
    PaintRow(4, "BORDER MODE (live + saved)");
    for (int m = 0; m < 4; m++) {
        PaintRow(6 + m * 2, "(%c) %s", sDispMode == m ? 'x' : ' ', kModes[m]);
    }
    PaintRow(15, "full-bleed/hybrid can expose sky-");
    PaintRow(16, "edge gaps (4:3 backdrops fall");
    PaintRow(17, "short of the opened edges).");
    PaintRow(19, "RIVAL DETAIL: %s  (tap cycles)",
             kRivalNames[(sRivalDetail >= 0 && sRivalDetail <= 3) ? sRivalDetail : 0]);
    PaintRow(21, "reduced/minimal simplify DISTANT");
    PaintRow(22, "rival machines for extra fps.");
    PaintDispAutoRow();
    PaintRow(26, "auto raises the cut in crowds;");
    PaintRow(27, "your setting above is the floor.");
}

/* DYNLOD row (live: the controller's current tier, ~1 Hz refresh). */
static void PaintDispAutoRow(void) {
    static const char* kTierNames[4] = { "NATIVE", "REDUCED", "MINIMAL", "LOW" };
    const int on = gdx3ds_dynlod_auto_enabled();
    int tier = gdx3ds_dynlod_tier();
    if (tier < sRivalDetail) {
        tier = sRivalDetail;
    }
    if (tier < 0 || tier > 3) {
        tier = 0;
    }
    if (on) {
        PaintRow(24, "AUTO LOD: ON   now %s", kTierNames[tier]);
    } else {
        PaintRow(24, "AUTO LOD: OFF  (tap toggles)");
    }
}

static void Paint3D(void) {
    int on = gdx3ds_stereo_runtime_enabled();
    int conv = gdx3ds_stereo_get_conv_x100();
    PaintRow(4, "STEREO 3D  (%s)", on ? "enabled" : "disabled");
    PaintRow(6, "IOD     [-]  %2d px [+]  (0-40)", gdx3ds_stereo_get_iod_px());
    PaintRow(9, "CONV    [-]  0.%02d  [+]  (0-.90)", conv);
    PaintRow(12, "live while the 3D slider is up;");
    PaintRow(13, "saved to [stereo] iod/convergence");
    if (!on) {
        PaintRow(15, "enable: [stereo] enabled=1 + boot");
    }
}

static void PaintAud(void) {
    PaintRow(4, "OUTPUT: %s",
             gdx3ds_audio_output_active() ? "ndsp (DSP up)" : "NULL SINK (silent)");
    PaintRow(6, "MASTER  [-]  %3d   [+]  (0-100)", gdx3ds_audio_get_master_volume());
    PaintRow(9, "step 5; saved to [audio]");
    PaintRow(10, "master_volume");
    PaintRow(12, "music/sfx split: not in v1 (the");
    PaintRow(13, "mix lives inside the ROM audio");
    PaintRow(14, "lib, no cheap ndsp-side hook).");
}

static const char* InputDisplayName(int i) {
    /* config-stable names -> display labels */
    static const char* kNames[] = { "ACCEL", "BOOST", "BRAKE", "SLIDE-L",
                                    "SLIDE-R", "CAMERA", "PAUSE" };
    if (i >= 0 && i < (int)(sizeof(kNames) / sizeof(kNames[0]))) {
        return kNames[i];
    }
    return gdx3ds_input_action_name(i);
}

/* MOTION block (gdx3ds_motion.h): tilt input, rows 23..27, with the RECENT/RESET pair on row 28.
 * The mode/angle/deadzone/invert rows are live -- they persist to [input] on the tap, exactly
 * like the pad-tuning rows above -- but nothing is applied unless the player enables the mode AND
 * a race is running (gdx3ds_input_glue.c does that gating). Row 26 is the one two-target row: the
 * left half flips the roll (stickX) sign, the right half the pitch (stickY) sign, because a
 * handset can make one right and the other wrong. Row 27 is the verification surface: it is how a
 * player checks the sensor exists and which way it points before turning tilt on. */
static void PaintMotionRows(void) {
    static const char* kMode[] = { "OFF", "TILT" };
    Gdx3dsMotionReadout ro;
    const int mode = gdx3ds_motion_get_mode();

    gdx3ds_motion_readout(&ro);

    PaintRow(23, "MOTION   [-]  %-5s [+]  off/tilt", kMode[mode]);
    PaintRow(24, "ANGLE    [-]  %3d   [+]  deg to full lock", gdx3ds_motion_get_angle());
    PaintRow(25, "DEADZON  [-]  %2d    [+]  deg ignore", gdx3ds_motion_get_deadzone());
    PaintRow(26, "INVERT  X %-3s        Y %-3s", gdx3ds_motion_get_invert() ? "YES" : "NO",
             gdx3ds_motion_get_invert_pitch() ? "YES" : "NO");
    if (mode == GDX3DS_MOTION_OFF) {
        PaintRow(27, "tilt off: pad only, sensor disabled");
    } else if (!ro.sensorOk) {
        PaintRow(27, "tilt on but sensor refused: see log");
    } else if (!ro.calibrated) {
        PaintRow(27, "hold the console still: calibrating");
    } else {
        PaintRow(27, "accel %5d %5d  X%+3d Y%+3d", ro.accelX, ro.accelY, ro.steer, ro.pitch);
    }
    /* Quantised: the raw contribution moves every frame by a unit or two from sensor noise, and
     * comparing it unquantised would repaint the whole page at the 10 Hz readout rate forever.
     * 1/8 of the 160-unit range is below what a player can read off a text row anyway. The
     * calibration flag is tracked separately: row 27 swaps between "hold still" and the readout
     * without the contributions necessarily moving at all (they are pinned to 0 while capturing),
     * so leaving it out of the repaint test kept the "hold still" text on screen long after the
     * capture had finished. */
    sMotionPainted = (ro.steer >> 3) ^ ((ro.pitch >> 3) << 8);
    sMotionCalPainted = ro.calibrated | (ro.sensorOk << 1);
}

static void PaintInput(void) {
    char keys[24];
    PaintRow(3, "tap an action, then press its key");
    int count = gdx3ds_input_action_count();
    for (int i = 0; i < count && i < 7; i++) {
        if (i == sCaptureAction) {
            PaintRow(4 + i * 2, "\x1b[7m%-8s press a key (tap=cancel)\x1b[0m",
                     InputDisplayName(i));
        } else {
            PaintRow(4 + i * 2, "%-8s %s", InputDisplayName(i),
                     gdx3ds_input_key_label(gdx3ds_input_action_mask(i), keys,
                                            (int)sizeof(keys)));
        }
    }
    {
        static const char* kCurve[] = { "LINEAR", "SOFT", "SOFTER" };
        static const char* kDpad[] = { "OFF", "FULL", "RAMP" };
        int rx, ry, sx, sy;
        gdx3ds_input_stick_readout(&rx, &ry, &sx, &sy);
        PaintRow(18, "DEADZONE [-]  %3d   [+]  raw 0-80", gdx3ds_input_get_deadzone());
        PaintRow(19, "RANGE    [-]  %3d   [+]  full at raw", gdx3ds_input_get_range());
        PaintRow(20, "CURVE    [-] %-6s [+]  centre feel", kCurve[gdx3ds_input_get_curve()]);
        PaintRow(21, "DPAD     [-] %-6s [+]  d-pad steers", kDpad[gdx3ds_input_get_dpad_steer()]);
        PaintRow(22, "pad %4d %4d  -> stick %4d %4d", rx, ry, sx, sy);
        sReadoutPainted = (u32) ((rx & 0xffff) | ((ry & 0xffff) << 16));
    }
    PaintMotionRows();
    PaintRow(28, "[RECENT]      [RESET]");
    PaintRow(29, "capture: ABXY LR ZL ZR START SEL DPAD CS");
}

static void PaintLog(void) {
    static char lines[MENU_LOG_LINES][MENU_COLS + 1];
    int n = gdx3ds_filelog_tail_lines((unsigned)sLogSkip, &lines[0][0], MENU_LOG_LINES,
                                      MENU_COLS + 1);
    for (int i = 0; i < MENU_LOG_LINES; i++) {
        if (i < n) {
            PaintRow(MENU_LOG_ROW0 + i, "%.40s", lines[i]);
        } else {
            PaintRow(MENU_LOG_ROW0 + i, "");
        }
    }
    if (sLogSkip == 0) {
        PaintRow(29, "[ UP ]  [ DN ]  [LIVE]  tail");
    } else {
        PaintRow(29, "[ UP ]  [ DN ]  [LIVE]  -%d", sLogSkip);
    }
}

/* FRAMESKIP (gdx3ds_frameskip.h): live, rtMode 0 only -- the New 3DS runs the render thread,
 * where the lever is refused, so the entry reads unavailable there. */
static void PaintDbgFrameskipRow(void) {
    char mode[16];
    if (!gdx3ds_frameskip_available()) {
        PaintRow(20, "(-) FRAMESKIP  n/a: render thread on");
        return;
    }
    gdx3ds_frameskip_mode_label(mode, sizeof(mode));
    PaintRow(20, "(%c) FRAMESKIP  %s (tap: cycle)",
             gdx3ds_frameskip_mode() != GDX_FRAMESKIP_OFF ? 'x' : ' ', mode);
}

static void PaintDbg(void) {
    PaintRow(4, "(%c) GPUTRACE   [gpu]/[prof] telem",
             gdx3ds_gpuprof_get_enabled() ? 'x' : ' ');
    PaintRow(6, "(%c) DIAG-AUDIO [audio-out] receipts",
             gdx3ds_audio_get_diag() ? 'x' : ' ');
    PaintRow(8, "(%c) VERBOSE    frame/present/census", sDbgVerbose ? 'x' : ' ');
    PaintRow(10, "(%c) FPS ROW    top-row fps counter",
             gdx3ds_fps_hud_get_enabled() ? 'x' : ' ');
    PaintRow(12, "(%c) BRFAST     bridge per-list memo",
             gdx3ds_config_get_int("debug", "brfast", 1) ? 'x' : ' ');
    {
        /* LOCKED-60 lever set: trectbatch latches at boot, so this one needs a reboot. */
        int n = gdx3ds_config_get_int("debug", "trectbatch", 1) + gdx3ds_config_get_int("debug", "trifast", 1) +
                gdx3ds_config_get_int("debug", "tmemfast", 1);
        PaintRow(14, "(%c) LEVERS     atlas+tri+tmem (reboot)", n == 3 ? 'x' : (n == 0 ? ' ' : '-'));
    }
    /* LOCKED-60 Task H: core-2 render thread; the thread is created at boot, so a flip
     * needs a relaunch (port/3ds/gdx3ds_renderthread.cpp reads the key once). */
    {
        int rt = gdx3ds_config_get_int("debug", "renderthread", 1);
        PaintRow(16, "(%c) RENDER THR %s (reboot)", rt ? 'x' : ' ',
                 rt >= 2 ? "core-2 ahead (2)" : (rt == 1 ? "core-2 pipe (1)" : "off"));
    }
    /* LOCKED-60 Task I: the bridge stage on the submitting core (needs the render thread). */
    PaintRow(18, "(%c) BRIDGE MAIN core-0 pre-pass (reboot)",
             gdx3ds_config_get_int("debug", "bridgemain", 1) ? 'x' : ' ');
    PaintDbgFrameskipRow();
    /* [native] levers (09-fx-integration-plan.md §6.2): read once at boot by each track. The
     * glyphs are the raw ini keys with the `all` master applied; x = on, - = verify (2). */
    {
        char st[5];
        const int all = gdx3ds_native_raw("all", 0);
        const int v[4] = { gdx3ds_native_lever("interp", 0), gdx3ds_native_lever("machines", 0),
                           gdx3ds_native_lever("track", 0), gdx3ds_native_fx_mask() };
        int i;
        for (i = 0; i < 4; i++) {
            st[i] = v[i] == 0 ? ' ' : (v[i] == 2 && i < 3 ? '-' : 'x');
        }
        st[4] = 0;
        PaintRow(22, "(%c) NATIVE     %s interp/mach/trk/fx (reboot)", all ? 'x' : ' ', st);
    }
    {
        char st[4];
        const int v[3] = { gdx3ds_native_lever("fx_sink", 0), gdx3ds_native_lever("fx_sprites", 0),
                           gdx3ds_native_lever("fx_hud", 0) };
        int i;
        for (i = 0; i < 3; i++) {
            st[i] = v[i] == 0 ? ' ' : (v[i] == 2 ? '-' : 'x');
        }
        st[3] = 0;
        PaintRow(24, "(%c) NATIVE FX  %s sink/sprites/hud (reboot)", gdx3ds_native_fx_mask() ? 'x' : ' ', st);
    }
    PaintRow(26, "toggles apply live and persist");
    PaintRow(27, "to gdiffuser.ini [debug].");
    /* CUSTOM HUD mode: a button (not a toggle -- tapping it hands the whole screen to the DASH's
     * CUSTOM-mode editor, same takeover shape a live race uses; see gdx3ds_menu_tick_body's own
     * takeover-gate comment). Gated on sRaceHudEnabled, same as the MAP tab: pointless without
     * [debug] bottom_race=1 at all, so it says so rather than silently doing nothing on a tap.
     * SHELVED (nibottom-progress.md §12): CUSTOM is off by default and this row is hidden
     * entirely unless [debug] hud_custom=1 (gdx3ds_bottomhud_custom_enabled) -- not merely
     * disabled-looking, since the feature itself is no longer reachable without the lever. */
    if (gdx3ds_bottomhud_custom_enabled()) {
        if (sRaceHudEnabled) {
            PaintRow(28, "(>) HUD EDIT   custom bottom-HUD layout (tap)");
        } else {
            PaintRow(28, "(-) HUD EDIT   n/a: [debug] bottom_race=0");
        }
    }
}

static void PaintAbout(void) {
    PaintRow(4, "G-DIFFUSER - F-ZERO X on 3DS");
    PaintRow(6, "build %.34s", gdx3ds_fps_hud_build_id());
    PaintRow(9, "F-ZERO X (C) 1998 NINTENDO");
    PaintRow(10, "decomp: the F-Zero X decomp team");
    PaintRow(11, "base: G-Diffuser + libultraship");
    PaintRow(12, "3DS port: the gdx-3ds effort");
    PaintRow(14, "thanks for playing.");
}

/* BOTTOMHUD: the MAP page is two text rows and a hole. Rows 4..27 (y 24..215) are left
 * blank here and filled every frame by gdx3ds_racemap_paint() writing RGB565 pixels
 * straight into this same console framebuffer -- see gdx3ds_racemap.h for the mapping
 * from a minimap source pixel to a bottom-screen pixel. */
static void PaintMap(void) {
    char status[44];
    gdx3ds_racemap_status_line(status, (int)sizeof(status));
    PaintRow(3, "RACE MAP  %s", status);
    PaintRow(29, "minimap 3x of sCourseMinimapTex 64x64");
}

static void PaintPage(void) {
    PaintClearPage();
    switch (sTab) {
        case TAB_STATUS: PaintStatus(); break;
        case TAB_DISP:   PaintDisp(); break;
        case TAB_3D:     Paint3D(); break;
        case TAB_AUD:    PaintAud(); break;
        case TAB_INPUT:  PaintInput(); break;
        case TAB_LOG:    PaintLog(); break;
        case TAB_DBG:    PaintDbg(); break;
        case TAB_MAP:    PaintMap(); break;
        default:         PaintAbout(); break;
    }
}

static void PaintAll(void) {
    PaintBegin();
    PaintRow(MENU_ROW_FPS, "%s", gdx3ds_fps_hud_get_enabled() ? sFpsLine : "");
    PaintTabBar();
    PaintPage();
    PaintEnd();
    sFpsDirty = 0;
    sDirty = 0;
}

/* ---- screen off ---------------------------------------------------------------- */
/* O3DSHOMEFIX (NEXT.md C4j, docs/research/native/o3dshomefix-progress.md): the Old 3DS HOME
 * freeze. Every freezing run had turned the bottom backlight off through gsp::Lcd at some point
 * (`[menu] screen OFF (touch wakes)`), and the HOME menu then never drew -- EVEN WHEN the
 * backlight was back ON at the press (hw-rdram8-0929, hw-c4run1-0929). What those runs still had
 * in common at the press was the gsp::Lcd SESSION: this file used to open it once and keep it for
 * the process lifetime. Every run that never opened it resumed from HOME (probes, the wave-2
 * card's 7 suspends, the G1 ini with screen_off=0). The HOME menu talks to gsp::Lcd itself on the
 * way in; a service port's session count is finite and libctru's srvGetServiceHandle BLOCKS on a
 * full port, which matches a HOME menu that loads (~6 MB, normal) and then waits forever.
 *
 * The fix removes both halves of the hazard for every model: the session is open only for the
 * duration of one backlight call (never across an APT transition), and the APT suspend hook
 * hands the backlight back ON before libctru jumps (gdx3ds_menu_force_backlight). Short-lived
 * sessions cost one srv round trip per touch-menu toggle -- nothing on a frame path. */
static Result LcdSetBottomBacklight(int on) {
    Result rc = gspLcdInit();
    if (R_FAILED(rc)) {
        return rc;
    }
    sLcdSessionOpen = 1;
    rc = on ? GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM) : GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
    gspLcdExit();
    sLcdSessionOpen = 0;
    return rc;
}

static void MenuSetBacklight(int on) {
    /* O3DSHOME3: [menu] lcd (default 1). 0 = never open a gsp::Lcd session or touch the
     * backlight (screen-off blanks only). */
    if (!gdx3ds_config_get_bool("menu", "lcd", 1)) {
        return;
    }
    if (R_FAILED(LcdSetBottomBacklight(on))) {
        MenuLog("[menu] gspLcd unavailable; screen-off blanks only");
        return;
    }
    sBacklightOff = on ? 0 : 1;
}

/* APT suspend/restore accessors (main_3ds.cpp aptLifecycleHook): the HOME menu must not
 * open onto a bottom screen we powered down, and the saved state must come back after.
 * No menu state change, no log (hook context: svc + filelog only, never the console). */
int gdx3ds_menu_backlight_off(void) {
    return sScreenOff && sBacklightOff;
}

/* 1 while a gsp::Lcd session of ours is open -- only ever inside one backlight call, so the
 * APT receipt reads 0; anything else is the C4j hazard back. */
int gdx3ds_menu_lcd_session_open(void) {
    return sLcdSessionOpen;
}

/* Returns the GSPLCD result (0 when nothing was needed). sBacklightOff is left alone: it is
 * the menu's saved intent, which the restore hook re-applies. */
int gdx3ds_menu_force_backlight(int on) {
    if (!sBacklightOff) {
        return 0;
    }
    return (int)LcdSetBottomBacklight(on);
}

static void MenuEnterOff(int wasUserToggle) {
    /* nibottom Phase 2 (advisor check-in): a mode cycle made mid-race defers its ini write; the
     * screen going off is a "menu close" moment, never a race frame, so it is a safe place to
     * flush one if it is still pending (e.g. the player backed out via HOME before the race
     * ended). A no-op when nothing is pending. */
    gdx3ds_bottomhud_flush_pending_save();
    sScreenOff = 1;
    PaintBegin();
    for (int r = 1; r <= MENU_ROWS; r++) {
        PaintRow(r, "");
    }
    PaintEnd();
    MenuSetBacklight(0);
    /* COORDINATOR FIX (hw bug n60-3d8, item 2): this used to persist [menu] screen_off=1 to
     * gdiffuser.ini (via MenuSaveCfg -> gdx3ds_config_save, the WHOLE table's writer) on every
     * tap of the OFF button -- "the game must never rewrite gdiffuser.ini just from ...
     * turning the screen off" is now a hard rule, not a judgement call, so this no longer
     * writes anything. wasUserToggle only distinguishes the log line below from the quiet
     * init-time restore (gdx3ds_menu_init, below) -- neither path touches the ini. A
     * gdiffuser.ini written by an OLDER build may still carry [menu] screen_off from before
     * this fix; the init-time read of it (gdx3ds_config_get_int("menu","screen_off",0)) is left
     * as a one-time compat fallback, same spirit as gdx3ds_bottomhud.c's hud.ini migration. */
    if (wasUserToggle) {
        MenuLog("[menu] screen OFF (touch wakes)");
    } else {
        MenuLog("[menu] screen OFF (restored, no ini write)");
    }
}

static void MenuLeaveOff(void) {
    sScreenOff = 0;
    MenuSetBacklight(1);
    sDirty = 1;
    /* COORDINATOR FIX (hw bug n60-3d8, item 2): same rule as MenuEnterOff above -- waking the
     * screen (a touch, or the DASH forcing the backlight back on for a live race) is not a user
     * SETTING change and must never rewrite gdiffuser.ini. */
    MenuLog("[menu] screen ON");
}

/* ---- touch handling ------------------------------------------------------------ */
static void TouchTabBar(int col) {
    if (col >= sOffC0 && col <= sOffC1) {
        MenuEnterOff(1);
        return;
    }
    for (int t = 0; t < TAB_COUNT; t++) {
        if (MENU_TAB_HIDDEN(t)) {
            continue;
        }
        if (col >= sTabCols[t].c0 && col <= sTabCols[t].c1) {
            if (t == TAB_RACE) {
                /* Not a real tab -- see its enum comment. Hands the screen straight back to
                 * whichever full-screen takeover is actually dismissed right now (mutually
                 * exclusive -- see MENU_TAB_RACE_DASH_VISIBLE/MENU_TAB_RACE_IDLE_VISIBLE); sTab
                 * is left wherever it was so the tab bar looks the same if dismissed again. */
                if (gdx3ds_bottomhud_race_live()) {
                    gdx3ds_bottomhud_resume();
                    MenuLog("[menu] tab=RACE -> DASH resumed");
                } else {
                    gdx3ds_bottomhud_idle_resume();
                    MenuLog("[menu] tab=DASH -> idle view resumed");
                }
                return;
            }
            if (sTab != t) {
                sTab = t;
                sCaptureAction = -1;
                sLogSkip = 0;
                sDirty = 1;
                MenuLog("[menu] tab=%s", kTabLabels[t]);
            }
            return;
        }
    }
}

static void TouchDisp(int row, int col) {
    (void)col;
    for (int m = 0; m < 4; m++) {
        if (row == 6 + m * 2 || row == 7 + m * 2) {
            if (sDispMode != m) {
                sDispMode = m;
                gdx3ds_disp_set_mode(m);
                gdx3ds_config_set_int("display", "border_mode", m);
                MenuSaveCfg();
                sDirty = 1;
                MenuLog("[menu] disp border_mode=%d", m);
            }
            return;
        }
    }
    if (row == 19 || row == 20) { /* RIVAL DETAIL row (+1 touch slop) */
        sRivalDetail = (sRivalDetail + 1) % (gdx3ds_dynlod_tier_max() + 1);
        gdx3ds_config_set_int("perf", "rival_detail", sRivalDetail);
        MenuSaveCfg();
        sDirty = 1;
        MenuLog("[menu] perf rival_detail=%d", sRivalDetail);
    } else if (row == 24 || row == 25) { /* AUTO LOD row (+1 touch slop): live, no relaunch */
        const int on = gdx3ds_dynlod_auto_enabled() ? 0 : 1;
        gdx3ds_dynlod_set_auto(on);
        gdx3ds_config_set_int("perf", "rival_detail_auto", on);
        MenuSaveCfg();
        sDirty = 1;
        MenuLog("[menu] perf rival_detail_auto=%d", on);
    }
}

/* Shared stepper zones for the 3D/AUD pages: the [-] glyph sits at cols 9-11 and the
 * [+] glyph at cols 20-22 on every stepper row; zones widened one cell each side. */
static int StepperDelta(int col) {
    if (col >= 8 && col <= 13) {
        return -1;
    }
    if (col >= 19 && col <= 25) {
        return 1;
    }
    return 0;
}

static void Touch3D(int row, int col) {
    int d = StepperDelta(col);
    if (d == 0) {
        return;
    }
    if (row >= 5 && row <= 7) { /* IOD row 6 ± 1 */
        int v = gdx3ds_stereo_get_iod_px() + d;
        if (v < 0) v = 0;
        if (v > 40) v = 40;
        gdx3ds_stereo_set_iod_px(v);
        gdx3ds_config_set_int("stereo", "iod", v);
        MenuSaveCfg();
        sDirty = 1;
        MenuLog("[menu] stereo iod=%d", v);
    } else if (row >= 8 && row <= 10) { /* CONV row 9 ± 1 */
        int v = gdx3ds_stereo_get_conv_x100() + d * 5;
        if (v < 0) v = 0;
        if (v > 90) v = 90;
        gdx3ds_stereo_set_conv_x100(v);
        char buf[8];
        snprintf(buf, sizeof(buf), "0.%02d", v);
        gdx3ds_config_set_string("stereo", "convergence", buf);
        MenuSaveCfg();
        sDirty = 1;
        MenuLog("[menu] stereo convergence=%s", buf);
    }
}

static void TouchAud(int row, int col) {
    int d = StepperDelta(col);
    if (d == 0 || row < 5 || row > 7) { /* MASTER row 6 ± 1 */
        return;
    }
    int v = gdx3ds_audio_get_master_volume() + d * 5;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    gdx3ds_audio_set_master_volume(v);
    gdx3ds_config_set_int("audio", "master_volume", v);
    MenuSaveCfg();
    sDirty = 1;
    MenuLog("[menu] audio master_volume=%d", v);
}

static void MenuPersistBind(int action) {
    char key[48];
    char hex[16];
    snprintf(key, sizeof(key), "bind_%s", gdx3ds_input_action_name(action));
    snprintf(hex, sizeof(hex), "0x%X", gdx3ds_input_action_mask(action));
    gdx3ds_config_set_string("input", key, hex);
}

static void TouchInput(int row, int col) {
    int count = gdx3ds_input_action_count();
    for (int i = 0; i < count && i < 7; i++) {
        if (row == 4 + i * 2 || row == 5 + i * 2) {
            if (sCaptureAction == i) {
                sCaptureAction = -1; /* tap again = cancel */
                MenuLog("[menu] input capture cancel (%s)", gdx3ds_input_action_name(i));
            } else {
                sCaptureAction = i;
                sPrevHeld = hidKeysHeld(); /* edges start from the current state */
                MenuLog("[menu] input capture %s", gdx3ds_input_action_name(i));
            }
            sDirty = 1;
            return;
        }
    }
    if (row >= 18 && row <= 21) { /* INPUT TUNE steppers */
        int d = StepperDelta(col);
        if (d == 0) {
            return;
        }
        if (row == 18) {
            gdx3ds_input_set_deadzone(gdx3ds_input_get_deadzone() + d * 2);
            gdx3ds_config_set_int("input", "deadzone", gdx3ds_input_get_deadzone());
        } else if (row == 19) {
            gdx3ds_input_set_range(gdx3ds_input_get_range() + d * 5);
            gdx3ds_config_set_int("input", "range", gdx3ds_input_get_range());
        } else if (row == 20) {
            gdx3ds_input_set_curve((gdx3ds_input_get_curve() + d + 3) % 3);
            gdx3ds_config_set_int("input", "curve", gdx3ds_input_get_curve());
        } else {
            gdx3ds_input_set_dpad_steer((gdx3ds_input_get_dpad_steer() + d + 3) % 3);
            gdx3ds_config_set_int("input", "dpad_steer", gdx3ds_input_get_dpad_steer());
        }
        MenuSaveCfg();
        sDirty = 1;
        MenuLog("[menu] input tune dz=%d range=%d curve=%d dpad=%d", gdx3ds_input_get_deadzone(),
                gdx3ds_input_get_range(), gdx3ds_input_get_curve(), gdx3ds_input_get_dpad_steer());
        return;
    }
    /* MOTION steppers (rows 23..25) and the INVERT toggle (row 26). Same "tap = adjust and
     * persist" contract as the pad-tuning rows above; the values are latched by
     * gdx3ds_motion.c, so nothing here needs a reboot. */
    if (row >= 23 && row <= 25) {
        const int d = StepperDelta(col);
        if (d == 0) {
            return;
        }
        if (row == 23) {
            gdx3ds_motion_set_mode((gdx3ds_motion_get_mode() + d + GDX3DS_MOTION_MODE_COUNT) %
                                   GDX3DS_MOTION_MODE_COUNT);
            gdx3ds_config_set_int("input", "motion", gdx3ds_motion_get_mode());
        } else if (row == 24) {
            gdx3ds_motion_set_angle(gdx3ds_motion_get_angle() + d * 5);
            gdx3ds_config_set_int("input", "motion_angle", gdx3ds_motion_get_angle());
        } else {
            gdx3ds_motion_set_deadzone(gdx3ds_motion_get_deadzone() + d);
            gdx3ds_config_set_int("input", "motion_deadzone", gdx3ds_motion_get_deadzone());
        }
        MenuSaveCfg();
        sDirty = 1;
        MenuLog("[menu] input motion=%d angle=%d dz=%d", gdx3ds_motion_get_mode(),
                gdx3ds_motion_get_angle(), gdx3ds_motion_get_deadzone());
        return;
    }
    if (row == 26) {
        /* Two targets on one row, split down the middle: the left half is the roll (stickX)
         * sign, the right half the pitch (stickY) sign. Same "tap = toggle and persist"
         * contract as the rows above; no stepper, so both fit on one 40-column line. */
        if (col < 20) {
            gdx3ds_motion_set_invert(!gdx3ds_motion_get_invert());
            gdx3ds_config_set_int("input", "motion_invert", gdx3ds_motion_get_invert());
            MenuLog("[menu] input motion_invert=%d", gdx3ds_motion_get_invert());
        } else {
            gdx3ds_motion_set_invert_pitch(!gdx3ds_motion_get_invert_pitch());
            gdx3ds_config_set_int("input", "motion_invert_pitch", gdx3ds_motion_get_invert_pitch());
            MenuLog("[menu] input motion_invert_pitch=%d", gdx3ds_motion_get_invert_pitch());
        }
        MenuSaveCfg();
        sDirty = 1;
        return;
    }
    if (row == 27) {
        return; /* MOTION live readout: not a control */
    }
    if (row == 28) {
        if (col >= 15) { /* [RESET] */
            gdx3ds_input_reset_defaults();
            for (int i = 0; i < count; i++) {
                MenuPersistBind(i);
            }
            gdx3ds_input_set_deadzone(16);
            gdx3ds_input_set_range(145);
            gdx3ds_input_set_curve(0);
            gdx3ds_input_set_dpad_steer(0);
            gdx3ds_config_set_int("input", "deadzone", 16);
            gdx3ds_config_set_int("input", "range", 145);
            gdx3ds_config_set_int("input", "curve", 0);
            gdx3ds_config_set_int("input", "dpad_steer", 0);
            gdx3ds_motion_set_mode(GDX3DS_MOTION_DEFAULT_MODE);
            gdx3ds_motion_set_angle(GDX3DS_MOTION_DEFAULT_ANGLE);
            gdx3ds_motion_set_deadzone(GDX3DS_MOTION_DEFAULT_DEADZONE);
            gdx3ds_motion_set_invert(GDX3DS_MOTION_DEFAULT_INVERT);
            gdx3ds_motion_set_invert_pitch(GDX3DS_MOTION_DEFAULT_INVERT_PITCH);
            gdx3ds_config_set_int("input", "motion_invert_pitch", GDX3DS_MOTION_DEFAULT_INVERT_PITCH);
            gdx3ds_config_set_int("input", "motion", GDX3DS_MOTION_DEFAULT_MODE);
            gdx3ds_config_set_int("input", "motion_angle", GDX3DS_MOTION_DEFAULT_ANGLE);
            gdx3ds_config_set_int("input", "motion_deadzone", GDX3DS_MOTION_DEFAULT_DEADZONE);
            gdx3ds_config_set_int("input", "motion_invert", GDX3DS_MOTION_DEFAULT_INVERT);
            MenuSaveCfg();
            sCaptureAction = -1;
            sDirty = 1;
            MenuLog("[menu] input reset defaults");
        } else if (col <= 9) { /* [RECENT]: forget the TILT neutral pose, capture a new one */
            gdx3ds_motion_recenter();
            sDirty = 1;
            MenuLog("[menu] input motion recenter");
        }
    }
}

static void TouchLog(int row, int col) {
    if (row < 28) {
        return;
    }
    if (col >= 1 && col <= 7) { /* [ UP ] — older */
        if (sLogSkip < MENU_LOG_SKIP_MAX) {
            sLogSkip += MENU_LOG_STEP;
            sDirty = 1;
        }
    } else if (col >= 9 && col <= 15) { /* [ DN ] — newer */
        sLogSkip -= MENU_LOG_STEP;
        if (sLogSkip < 0) {
            sLogSkip = 0;
        }
        sDirty = 1;
    } else if (col >= 17 && col <= 22) { /* [LIVE] */
        sLogSkip = 0;
        sDirty = 1;
    }
}

/* off -> auto -> steady -> off. FIXED N is ini-only: a tap from it leaves to off. */
static int NextFrameskipMode(int mode) {
    if (mode == GDX_FRAMESKIP_OFF) {
        return GDX_FRAMESKIP_AUTO;
    } else if (mode == GDX_FRAMESKIP_AUTO) {
        return GDX_FRAMESKIP_STEADY;
    } else {
        return GDX_FRAMESKIP_OFF;
    }
}

static const char* FrameskipIniValue(int mode) {
    if (mode == GDX_FRAMESKIP_AUTO) {
        return "auto";
    } else if (mode == GDX_FRAMESKIP_STEADY) {
        return "steady";
    } else {
        return "0";
    }
}

/* Returns 0 when refused (render thread on): nothing changes, nothing persists. */
static int TouchDbgFrameskip(void) {
    const int next = NextFrameskipMode(gdx3ds_frameskip_mode());
    if (!gdx3ds_frameskip_request_mode(next)) {
        MenuLog("[menu] dbg frameskip refused: render thread on");
        return 0;
    }
    gdx3ds_config_set_string("debug", "frameskip", FrameskipIniValue(next));
    MenuLog("[menu] dbg frameskip=%s (applies next frame)", FrameskipIniValue(next));
    return 1;
}

static void TouchDbg(int row, int col) {
    (void)col;
    if (row == 4 || row == 5) {
        int on = !gdx3ds_gpuprof_get_enabled();
        gdx3ds_gpuprof_set_enabled(on);
        gdx3ds_config_set_int("debug", "gputrace", on);
        MenuLog("[menu] dbg gputrace=%d", on);
    } else if (row == 6 || row == 7) {
        int on = !gdx3ds_audio_get_diag();
        gdx3ds_audio_set_diag(on);
        gdx3ds_config_set_int("debug", "diag_audio", on);
        MenuLog("[menu] dbg diag_audio=%d", on);
    } else if (row == 8 || row == 9) {
        sDbgVerbose = !sDbgVerbose;
        gdx3ds_config_set_int("debug", "verbose", sDbgVerbose);
        MenuLog("[menu] dbg verbose=%d", sDbgVerbose);
    } else if (row == 10 || row == 11) {
        int on = !gdx3ds_fps_hud_get_enabled();
        gdx3ds_fps_hud_set_enabled(on);
        gdx3ds_config_set_int("debug", "fps", on);
        MenuLog("[menu] dbg fps=%d", on);
    } else if (row == 12 || row == 13) {
        int on = !gdx3ds_config_get_int("debug", "brfast", 1);
        gdx3ds_config_set_int("debug", "brfast", on);
        MenuLog("[menu] dbg brfast=%d", on);
    } else if (row == 14 || row == 15) {
        int on = !(gdx3ds_config_get_int("debug", "trectbatch", 1) && gdx3ds_config_get_int("debug", "trifast", 1) &&
                   gdx3ds_config_get_int("debug", "tmemfast", 1));
        gdx3ds_config_set_int("debug", "trectbatch", on);
        gdx3ds_config_set_int("debug", "trifast", on);
        gdx3ds_config_set_int("debug", "tmemfast", on);
        MenuLog("[menu] dbg levers=%d (reboot for atlas)", on);
    } else if (row == 16 || row == 17) {
        /* cycles 1 (pipe) -> 2 (ahead) -> 0 (off) -> 1 */
        int cur = gdx3ds_config_get_int("debug", "renderthread", 1);
        int next = cur == 1 ? 2 : (cur >= 2 ? 0 : 1);
        gdx3ds_config_set_int("debug", "renderthread", next);
        MenuLog("[menu] dbg renderthread=%d (relaunch to apply)", next);
    } else if (row == 18 || row == 19) {
        int on = !gdx3ds_config_get_int("debug", "bridgemain", 1);
        gdx3ds_config_set_int("debug", "bridgemain", on);
        MenuLog("[menu] dbg bridgemain=%d (relaunch to apply)", on);
    } else if (row == 20 || row == 21) {
        if (!TouchDbgFrameskip()) {
            return;
        }
    } else if (row == 28 || row == 29) {
        if (!gdx3ds_bottomhud_custom_enabled()) {
            /* SHELVED: the row is hidden in PaintDbg when hud_custom=0, so a tap here can only
             * happen against a stale touch (e.g. a queued touch-up from the row before this
             * tick's paint hid it) -- refuse silently rather than opening the shelved editor. */
            return;
        }
        if (!sRaceHudEnabled) {
            MenuLog("[menu] dbg hud edit refused: [debug] bottom_race=0");
            return;
        }
        gdx3ds_bottomhud_enter_custom_edit();
        MenuLog("[menu] dbg hud edit -> opening custom layout editor");
    } else {
        return;
    }
    MenuSaveCfg();
    sDirty = 1;
}

static void MenuHandleTouch(int row, int col) {
    if (row <= MENU_ROW_TABS) {
        TouchTabBar(col);
        return;
    }
    switch (sTab) {
        case TAB_DISP:  TouchDisp(row, col); break;
        case TAB_3D:    Touch3D(row, col); break;
        case TAB_AUD:   TouchAud(row, col); break;
        case TAB_INPUT: TouchInput(row, col); break;
        case TAB_LOG:   TouchLog(row, col); break;
        case TAB_DBG:   TouchDbg(row, col); break;
        default: break; /* STATUS/ABOUT: nothing tappable below the tab bar */
    }
}

/* ---- public API ---------------------------------------------------------------- */
/* nibottom Phase 2: lets gdx3ds_bottomhud.c position its live-number console-text overlay on
 * the SAME PrintConsole this file owns (the MENU's bottom-screen console), without that TU
 * declaring its own gdx3ds_menu-internal state. NULL before gdx3ds_menu_init runs. */
PrintConsole* gdx3ds_menu_console(void) {
    return sCon;
}

int gdx3ds_menu_enabled(void) {
    return sEnabled;
}

/* nidle verification gap (see sPendingShot/sShotArmed's own comment): wires this file into
 * gdx_request_frame_dump's weak-hook dispatch (port/n64_gfx_bridge.cpp), same shape as
 * gdx3ds_racemap_note_shot/gdx3ds_bottomhud_note_shot. Armed unconditionally -- the SHOT only
 * actually writes a file on whatever tick the plain console UI is the one painting the bottom
 * screen (MenuServiceShot, called once at the end of gdx3ds_menu_tick_body); on a tick the race
 * DASH or idle view owns the screen instead, gdx3ds_bottomhud_note_shot's own arm covers it,
 * and this one simply finds sTab's screen isn't current when its turn comes and no-ops. */
void gdx3ds_menu_note_shot(const char* label) {
    if (!sEnabled || label == NULL || label[0] == '\0') {
        return;
    }
    snprintf(sPendingShot, sizeof(sPendingShot), "%s", label);
    __atomic_store_n(&sShotArmed, 1, __ATOMIC_RELEASE);
}

void gdx3ds_menu_set_fps_line(const char* line) {
    if (line == NULL) {
        return;
    }
    snprintf(sFpsLine, sizeof(sFpsLine), "%s", line);
    sFpsDirty = 1;
}

int gdx3ds_dbg_verbose_active(void) {
    return sDbgVerbose || gdx3ds_gpuprof_get_enabled();
}

/* Console echo gate read by gdx_port_write_log (port_log.h, weak extern): while the touch
 * menu owns the bottom console, diagnostic stderr echo would scroll the menu page away
 * between repaints — svc + filelog sinks stay live; the LOG tab reads the ring. Legacy
 * console=1-without-menu keeps the echo. */
int gdx3ds_console_echo_enabled = 1;

/* CONSOLE FILTER: while the menu owns the bottom screen, every stdout/stderr write that is not
 * the menu's or the FPS row's own painting is dropped at the device layer. Stray printers
 * (LUS's raw fprintf(stderr, "Unsupported ccmux"), backend one-shots, anything on the render
 * thread) otherwise put a newline on the bottom row and scroll the tab bar away. The svc and
 * filelog sinks are untouched (they never go through stdio). */
#include <sys/iosupport.h>
extern int gdx3ds_rt_on_render_thread(void) __attribute__((weak));
static const devoptab_t* sConInner = NULL;
static devoptab_t sConFilter;
static volatile int sConPassthrough = 0;

static ssize_t ConFilterWrite(struct _reent* r, void* fd, const char* ptr, size_t len) {
    if (&gdx3ds_rt_on_render_thread != NULL && gdx3ds_rt_on_render_thread()) {
        return (ssize_t)len; /* never from the render thread: the console is not thread-safe */
    }
    if (!sConPassthrough && !gdx3ds_console_echo_enabled) {
        return (ssize_t)len; /* menu owns the console: drop */
    }
    return sConInner->write_r(r, fd, ptr, len);
}

static void ConFilterInstall(void) {
    if (sConInner != NULL || devoptab_list[STD_OUT] == NULL) {
        return;
    }
    sConInner = devoptab_list[STD_OUT];
    sConFilter = *sConInner;
    sConFilter.name = "gdxcon";
    sConFilter.write_r = ConFilterWrite;
    devoptab_list[STD_OUT] = &sConFilter;
    devoptab_list[STD_ERR] = &sConFilter;
}

void gdx3ds_console_passthrough(int on) {
    sConPassthrough = on ? 1 : 0;
}

void gdx3ds_menu_init(void) {
    /* Live latches init from config even when the UI is disabled (main_3ds.cpp's
     * telemetry gate reads gdx3ds_dbg_verbose_active unconditionally, Racer_Draw
     * reads gdx_rival_detail_level every frame). */
    sDbgVerbose = gdx3ds_config_get_int("debug", "verbose", 0) != 0;
    /* [perf] preset (gdx3ds_perf_preset.c) may re-default the floor; an explicit key wins. */
    sRivalDetail = gdx3ds_perf_get_int("rival_detail", 0);
    gdx3ds_dynlod_init(); /* DYNLOD latches [perf] rival_detail_auto after the floor above */
    if (sRivalDetail < 0 || sRivalDetail > gdx3ds_dynlod_tier_max()) {
        sRivalDetail = 0;
    }
    /* BOTTOMHUD: latch [debug] bottom_map and lay the tab bar out for it. Deliberately
     * BEFORE the [menu] enabled bail-out and unconditional, so the "[bmap] init" receipt
     * is on the record in every run, including the ones with the menu UI switched off. */
    gdx3ds_racemap_init();
    sMapTabShown = gdx3ds_racemap_enabled();
    /* nibottom Phase 2: same reasoning, same placement — [debug] bottom_race's receipt must
     * be on the record whether or not the MENU UI itself is enabled. */
    gdx3ds_bottomhud_init();
    sRaceHudEnabled = gdx3ds_bottomhud_enabled();
    MenuLayoutTabs();
    sEnabled = gdx3ds_config_get_bool("menu", "enabled", 1);
    if (!sEnabled) {
        return;
    }
    gdx3ds_console_echo_enabled = 0; /* menu owns the console from here on */
    sCon = (PrintConsole*)gdx3ds_fps_hud_console();
    if (sCon == NULL) {
        gdx3ds_vram_guard_bottom_cpu_write("menu"); /* n60boot */
        sCon = consoleInit(GFX_BOTTOM, NULL);
    }
    ConFilterInstall();
    sConPassthrough = 1; /* init paints below; cleared at the end of init */
    sDispMode = gdx3ds_config_get_int("display", "border_mode", 1); /* default FULL-BLEED (user pick, HW-verified) */
    if (sDispMode < 0 || sDispMode > 3) {
        sDispMode = 0;
    }
    /* BOTTOMHUD: `[menu] tab` — the tab to open on. Default STAT, i.e. the behaviour this
     * menu has always had. It exists because the MENU is driven by REAL hid touch
     * (gdx3ds_os_poll_input's latch), not by the replay's input stream, so a headless
     * replay can otherwise never reach a tab: without this key the MAP page could only be
     * verified by hand. Matched against the tab labels, case-insensitively. */
    {
        const char* want = gdx3ds_config_get_string("menu", "tab", "");
        if (want != NULL && want[0] != '\0') {
            for (int t = 0; t < TAB_COUNT; t++) {
                if (MENU_TAB_HIDDEN(t)) {
                    continue;
                }
                if (strcasecmp(want, kTabLabels[t]) == 0) {
                    sTab = t;
                    break;
                }
            }
            MenuLog("[menu] tab_ini=%s -> %s", want, kTabLabels[sTab]);
        }
    }
    sLastPeriodicTick = svcGetSystemTick();
    sRaceQuiet = gdx3ds_config_get_bool("menu", "race_quiet", 0);
    if (gdx3ds_config_get_int("menu", "screen_off", 0)) {
        MenuEnterOff(0); /* restore last state; no redundant save */
    } else {
        PaintAll();
    }
    MenuLog("[menu] init tab=%s disp=%d off=%d map_tab=%d race_tab=%d race_quiet=%d", kTabLabels[sTab], sDispMode,
            sScreenOff, sMapTabShown, sRaceHudEnabled, sRaceQuiet);
    sConPassthrough = 0;
}

void gdx3ds_menu_tick(void) {
    if (!sEnabled || sCon == NULL) {
        return;
    }
    sConPassthrough = 1; /* all menu painting happens inside this call (main thread) */
    gdx3ds_menu_tick_body();
    sConPassthrough = 0;
}

static int RaceQuietNow(void) {
    return sRaceQuiet && &gdx_input_in_gameplay != NULL && gdx_input_in_gameplay() != 0;
}

/* race_quiet's refresh: the fps row alone (the live page waits for the race to end). */
static void PaintFpsRowOnly(void) {
    PaintBegin();
    PaintRow(MENU_ROW_FPS, "%s", gdx3ds_fps_hud_get_enabled() ? sFpsLine : "");
    PaintEnd();
    sFpsDirty = 0;
}

/* nibottom Phase 2: 1 while the DASH owns the whole bottom screen (gdx3ds_bottomhud_race_live()
 * was 1 last tick) — used only to detect the 1->0 edge below, so the ordinary MENU UI gets one
 * full repaint right after a race ends instead of showing whatever stale page it had before the
 * takeover started. */
static int sRaceHudWasLive = 0;
/* 1 while the race DASH (not the idle view) owned the screen last tick. The idle-view branch
 * runs before the sRaceHudWasLive cleanup, so a race that ends straight into the idle view must
 * run the end-of-race reset itself, or the idle view keeps its stale "already painted" latch and
 * the last DASH frame stays on screen. */
static int sRaceDashWasLive = 0;

/* ---- SHOT (same format/writer shape as gdx3ds_racemap.c's / gdx3ds_bottomhud.c's own,
 * independent instance -- see sPendingShot/sShotArmed's own comment above). --- */

#define MENU_SHOT_FB_W 320
#define MENU_SHOT_FB_H 240

static long MenuWriteShot(const char* path, const uint16_t* fb) {
    const unsigned rowBytes = (MENU_SHOT_FB_W * 3u + 3u) & ~3u;
    const uint32_t imageBytes = rowBytes * MENU_SHOT_FB_H;
    const uint32_t fileBytes = 54u + imageBytes;
    static unsigned char sWriteBuf[8u * 1024u];
    unsigned char header[54];
    unsigned char row[MENU_SHOT_FB_W * 3u + 3u];
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
    header[18] = (unsigned char) (MENU_SHOT_FB_W & 0xFF);
    header[19] = (unsigned char) (MENU_SHOT_FB_W >> 8);
    header[22] = (unsigned char) (MENU_SHOT_FB_H & 0xFF);
    header[23] = (unsigned char) (MENU_SHOT_FB_H >> 8);
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, sizeof(header), f);
    for (y = 0; y < MENU_SHOT_FB_H; y++) {
        const unsigned sy = MENU_SHOT_FB_H - 1u - y; /* BMP rows are bottom-up */
        unsigned x;
        memset(row, 0, rowBytes);
        for (x = 0; x < MENU_SHOT_FB_W; x++) {
            const uint16_t p = fb[(size_t) x * MENU_SHOT_FB_H + (MENU_SHOT_FB_H - 1u - sy)];
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

/* Called once at the end of gdx3ds_menu_tick_body, after whatever this tick's own repaint (or
 * no-op) already happened -- consumes the arm regardless of whether anything was actually
 * repainted this tick, same "whatever's on screen right now" contract DashServiceShot/
 * BmapServiceShot already have for their own screens. */
static void MenuServiceShot(const uint16_t* fb) {
    char path[80];
    long bytes;
    if (!__atomic_exchange_n(&sShotArmed, 0, __ATOMIC_ACQ_REL)) {
        return;
    }
    snprintf(path, sizeof(path), "autotest/%s_R.bmp", sPendingShot);
    bytes = MenuWriteShot(path, fb);
    if (bytes < 0) {
        MenuLog("[menu] SHOT write failed: %s", path);
    } else {
        MenuLog("[menu] SHOT captured -> %s bytes=%ld", path, bytes);
    }
}

/* Runs the race DASH's end-of-race reset on the first tick its race is over, whichever branch
 * of gdx3ds_menu_tick_body takes the screen next (see sRaceDashWasLive). */
static void MenuEndRaceDashIfOver(void) {
    if (!sRaceDashWasLive || gdx3ds_bottomhud_race_live()) {
        return;
    }
    gdx3ds_bottomhud_on_race_ended();
    sRaceDashWasLive = 0;
}

static void gdx3ds_menu_tick_body(void) {
    const u32 held = hidKeysHeld();
    const int touching = (held & KEY_TOUCH) != 0;

    /* nibottom Phase 2: the DASH takeover. See gdx3ds_bottomhud.h's "FULL-SCREEN TAKEOVER, NOT A
     * MENU TAB" note for why this is a hand-off, not a tab: the real layout uses the whole 320x240
     * screen, including the rows the tab bar and scroll-park otherwise own. Checked BEFORE the
     * screen-off state (a live race forces the backlight back on — there is nothing for a dev
     * console screen-saver to protect once the dashboard is the thing on screen) and before
     * anything else in this tick. !gdx3ds_bottomhud_dismissed(): the DASH's own "DEBUG" button (nee
     * "MENU"; second visual-feedback round, item 4) sets this, mid-race, to hand the screen back
     * to the ordinary tab UI below -- see TAB_RACE's own comment for how it's picked back up.
     *
     * CUSTOM HUD mode: gdx3ds_bottomhud_edit_active() is OR'd in so the DBG tab's "HUD EDIT"
     * button (TouchDbg, row 28) can hand the WHOLE screen to the DASH's CUSTOM-mode editor even
     * with NO live race (gdx3ds_bottomhud_tick substitutes sample values in that case -- see its
     * own comment) -- the exact same takeover shape a live race uses, just entered a different
     * way. sRaceHudWasLive below still tracks this correctly with no further change: it is set
     * to 1 on every tick this whole if-branch runs (race or edit-without-a-race alike), and the
     * "race ended" cleanup path, once this condition goes false again, simply also fires on
     * DONE-without-a-race -- which is exactly the per-race-latch reset the editor needs before
     * the debug tabs repaint themselves fully again (see that block's own comment). */
    if ((sRaceHudEnabled && gdx3ds_bottomhud_race_live() && !gdx3ds_bottomhud_dismissed()) ||
        gdx3ds_bottomhud_edit_active()) {
        touchPosition pos = {0, 0};
        if (sScreenOff) {
            MenuLeaveOff();
        }
        if (touching) {
            hidTouchRead(&pos);
        }
        gdx3ds_bottomhud_tick(sCon->frameBuffer, held, sPrevHeld, touching, sPrevTouching, (int) pos.px,
                               (int) pos.py);
        sPrevHeld = held;
        sPrevTouching = touching;
        sRaceHudWasLive = 1;
        sRaceDashWasLive = 1;
        return;
    }
    MenuEndRaceDashIfOver();

    /* IDLE VIEW (coordinator ask: "we shouldn't just have the bottom screen only show a polished
     * experience during race"): the SAME shape as the race-DASH takeover just above, checked only
     * once that one does not apply (gdx3ds_bottomhud_idle_active() is itself false whenever a race
     * is live, CUSTOM edit mode is active, the lever is off, or the idle view has been dismissed
     * to the console -- see gdx3ds_bottomhud.h's own "IDLE VIEW" doc comment). sRaceHudWasLive is
     * reused, not duplicated: it only ever means "some full-screen takeover owned the screen last
     * tick, give the console UI one full repaint now that it doesn't" -- true for either takeover
     * alike, exactly like the edit-mode-without-a-race case already shared it above.
     *
     * UNLIKE the race-DASH branch above, this one does NOT force the backlight back on while
     * `sScreenOff` -- "[debug] bottom_race=1 keeps OFF/sleep behaviour unchanged" is explicit
     * that a live race (where there is nothing for a screen-saver to protect) is the one case
     * that overrides sleep, not every full-screen takeover. With the screen off, `!sScreenOff`
     * below is false, so control falls straight through to the ordinary touch-wakes-it handling
     * further down, completely unmodified; the very next tick (now awake) picks the idle view
     * back up here, same as it always would have. */
    if (!sScreenOff && gdx3ds_bottomhud_idle_active()) {
        touchPosition pos = {0, 0};
        if (touching) {
            hidTouchRead(&pos);
        }
        gdx3ds_bottomhud_idle_tick(sCon->frameBuffer, held, sPrevHeld, touching, sPrevTouching, (int) pos.px,
                                    (int) pos.py);
        sPrevHeld = held;
        sPrevTouching = touching;
        sRaceHudWasLive = 1;
        return;
    }
    /* COORDINATOR FIX (hw bug n60-3d8, item 1): re-lay the tab bar out every tick the ordinary
     * MENU UI is about to run, BEFORE this tick's own touch handling and painting below -- see
     * MenuLayoutTabs' own comment for why a once-at-init layout went stale the moment TAB_RACE
     * became reachable. */
    MenuLayoutTabs();
    if (sRaceHudWasLive) {
        /* Some full-screen takeover stopped owning the screen this tick, for one of THREE
         * different reasons that all need the ordinary MENU UI's full repaint below but only ONE
         * of which is a real race end: gdx3ds_bottomhud_race_live() still true here means the
         * DEBUG button just dismissed the DASH takeover mid-race (TAB_RACE is now live in the tab
         * bar, painted "RACE", ready to hand it back) -- the DASH's own once-per-race latches must
         * NOT be reset in that case, or resuming would force a pointless full backdrop repaint
         * next tick. The other two cases (CUSTOM edit DONE without a live race; the idle view's
         * own DEBUG button dismissing IT) both reach gdx3ds_bottomhud_on_race_ended() below
         * (race_live() is false in both), which is harmless for them too -- it only resets
         * latches each one either doesn't use (the race-only ones) or already resets itself on
         * its own resume path (sIdleBaked, via gdx3ds_bottomhud_idle_resume()). Only a real 1->0
         * race-live edge is what actually MATTERS here; the other two are along for the ride. */
        if (!gdx3ds_bottomhud_race_live()) {
            gdx3ds_bottomhud_on_race_ended();
            sRaceDashWasLive = 0;
        }
        sRaceHudWasLive = 0;
        sDirty = 1;
    }

    if (sScreenOff) {
        if (touching && !sPrevTouching) {
            MenuLeaveOff();
            sWakeSwallow = 1; /* this press must not also hit a widget */
        }
        sPrevTouching = touching;
        if (!sScreenOff && sDirty) {
            PaintAll();
        }
        return;
    }

    /* COORDINATOR FIX (hw bug n60-3d7, item 5): "let the MENU/RACE toggle work from a key
     * (SELECT long-press or similar)" -- the key-based twin of tapping the RACE tab
     * (TouchTabBar's TAB_RACE branch). Same threshold/ticks-not-wall-time/re-arm-on-release
     * reasoning as gdx3ds_bottomhud.c's own sSelectHoldTicks (the DASH-side half of this same
     * toggle); only counts while RACE (or, idle-view round: the idle view) would actually be
     * reachable (MENU_TAB_RACE_DASH_VISIBLE/MENU_TAB_RACE_IDLE_VISIBLE, the same two mutually-
     * exclusive conditions TAB_RACE's own visibility uses), so holding SELECT elsewhere in the
     * menu (e.g. no race live and the idle view off/already shown) does nothing. */
    {
        static int sMenuSelectHoldTicks = 0;
        const int raceReachable = MENU_TAB_RACE_DASH_VISIBLE;
        const int idleReachable = MENU_TAB_RACE_IDLE_VISIBLE;
        if ((raceReachable || idleReachable) && (held & KEY_SELECT)) {
            if (sMenuSelectHoldTicks <= 90) {
                sMenuSelectHoldTicks++;
            }
            if (sMenuSelectHoldTicks == 90) {
                if (raceReachable) {
                    gdx3ds_bottomhud_resume();
                    MenuLog("[menu] SELECT long-press -> DASH resumed");
                } else {
                    gdx3ds_bottomhud_idle_resume();
                    MenuLog("[menu] SELECT long-press -> idle view resumed");
                }
            }
        } else {
            sMenuSelectHoldTicks = 0;
        }
    }

    if (touching && !sPrevTouching && !sWakeSwallow) {
        touchPosition pos;
        hidTouchRead(&pos);
        if (pos.px < 320 && pos.py < 240) {
            const int col = pos.px / 8 + 1;
            const int row = pos.py / 8 + 1;
            MenuHandleTouch(row, col);
        }
    }
    if (!touching) {
        sWakeSwallow = 0;
    }
    sPrevTouching = touching;

    /* INPUT capture: bind the first fresh key press from the bindable set. */
    if (sCaptureAction >= 0) {
        const u32 fresh = held & ~sPrevHeld & gdx3ds_input_bindable_mask();
        if (fresh != 0) {
            const u32 key = fresh & (~fresh + 1u); /* lowest set bit */
            char label[24];
            gdx3ds_input_action_set_mask(sCaptureAction, key);
            MenuPersistBind(sCaptureAction);
            MenuSaveCfg();
            MenuLog("[menu] input bind %s=%s (0x%lX)",
                    gdx3ds_input_action_name(sCaptureAction),
                    gdx3ds_input_key_label(key, label, (int)sizeof(label)),
                    (unsigned long)key);
            sCaptureAction = -1;
            sDirty = 1;
        }
    }

    /* nibottom Phase 2: the KEY_SELECT mode-cycle hotkey only matters WHILE the DASH takeover is
     * active (there is nothing for the mode to change the placement of otherwise) — handled
     * entirely inside gdx3ds_bottomhud_tick above, which already has `held`/`sPrevHeld`. */
    sPrevHeld = held;

    const u64 now = svcGetSystemTick();
    /* INPUT tab live readouts: repaint at <= 10 Hz while the pad or the motion contribution
     * moves, so tuning the deadzone/range (or the motion ANGLE) can be done by eye without
     * paying a per-frame console repaint. The motion value is compared already quantised by
     * PaintMotionRows, so sensor noise alone does not keep the 10 Hz repaint running. */
    if (sTab == TAB_INPUT && !sDirty &&
        (now - sReadoutTick) >= (u64)(100.0 * CPU_TICKS_PER_MSEC)) {
        int rx, ry;
        Gdx3dsMotionReadout mro;
        gdx3ds_input_stick_readout(&rx, &ry, NULL, NULL);
        gdx3ds_motion_readout(&mro);
        if ((u32) ((rx & 0xffff) | ((ry & 0xffff) << 16)) != sReadoutPainted ||
            ((mro.steer >> 3) ^ ((mro.pitch >> 3) << 8)) != sMotionPainted ||
            (mro.calibrated | (mro.sensorOk << 1)) != sMotionCalPainted) {
            sDirty = 1;
        }
        sReadoutTick = now;
    }
    /* Repaint policy (MENU-PERF): full repaint on change; ~1 Hz refresh for the live
     * pages (STATUS numbers, LOG tail in live mode) and the row-1 fps line. */
    const int periodic = (now - sLastPeriodicTick) >= (u64)(1000.0 * CPU_TICKS_PER_MSEC);
    if (sDirty) {
        PaintAll();
    } else if (periodic && RaceQuietNow()) {
        PaintFpsRowOnly();
    } else if (periodic &&
               (sTab == TAB_STATUS || sTab == TAB_DISP || (sTab == TAB_LOG && sLogSkip == 0) ||
                sTab == TAB_MAP || sFpsDirty)) {
        PaintBegin();
        PaintRow(MENU_ROW_FPS, "%s", gdx3ds_fps_hud_get_enabled() ? sFpsLine : "");
        if (sTab == TAB_STATUS) {
            PaintStatus();
        } else if (sTab == TAB_DISP) {
            PaintDispAutoRow(); /* DYNLOD live tier only; the rest of the page is static */
        } else if (sTab == TAB_LOG) {
            PaintLog();
        } else if (sTab == TAB_MAP) {
            /* BOTTOMHUD: the box is pixels and repaints itself every frame, but the header row
             * is console text -- without this it kept whatever lap/position it was painted with
             * when the tab opened, which on a boot-time [menu] tab=MAP is "no race (mode 0)". */
            PaintMap();
        }
        PaintEnd();
        sFpsDirty = 0;
    }
    if (periodic) {
        sLastPeriodicTick = now;
    }

    /* BOTTOMHUD: the map is the one page that is NOT repaint-on-change -- the racer markers
     * move every frame. It writes pixels, not console cells, into rows 4..27 of the same
     * framebuffer, so it runs after the text paint above and never fights it. One int test
     * per frame on every other tab, and on every frame of every run with the lever off. */
    if (sTab == TAB_MAP && !sScreenOff && sMapTabShown) {
        gdx3ds_racemap_paint(sCon->frameBuffer);
    }

    /* nidle verification gap: this is the plain console UI's own tick -- the one screen owner
     * gdx_request_frame_dump previously had no SHOT hook for (see sPendingShot/sShotArmed's own
     * comment). Whatever is currently in sCon->frameBuffer at this point is exactly what the
     * bottom LCD is about to scan out this tick (same "read back the real framebuffer" shape
     * the race DASH and racemap's own SHOT already use). */
    MenuServiceShot(sCon->frameBuffer);
}
