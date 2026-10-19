/* port/3ds/gdx3ds_launch.c -- N60LAUNCH: the console half of the launch-variant selector (HID
 * window, bottom-screen prompt, SD cycle counter, per-variant log name). Contract:
 * gdx3ds_launch.h; the pure rules: gdx3ds_launch_core.c (host-tested).
 *
 * Why a window AFTER the screen is up (the O3DSHOME3 lesson): that card read the buttons at
 * main() entry, before gfxInit, for up to 2 s, and only THEN drew "THIS RUN: ..."; every
 * hardware run logged held=0x00000000. The user cannot see when main() starts, and the read
 * never overlapped anything the user could react to. Here the prompt is on the bottom screen
 * first and HID is scanned every vblank while it is up, so a hold that starts at launch AND a
 * press made after reading the prompt both land. The main()-entry sample is still taken and
 * logged (early_held=) as hardware evidence of what that point sees. */
#include "gdx3ds_launch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <3ds.h>

#include "gdx3ds_boot_trail.h" /* n60boot */
#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_launch_core.h"
#include "gdx3ds_vram_guard.h" /* n60boot */

_Static_assert(GDX_LAUNCH_KEY_L == KEY_L && GDX_LAUNCH_KEY_R == KEY_R && GDX_LAUNCH_KEY_X == KEY_X &&
                   GDX_LAUNCH_KEY_Y == KEY_Y && GDX_LAUNCH_KEY_DUP == KEY_DUP &&
                   GDX_LAUNCH_KEY_DDOWN == KEY_DDOWN && GDX_LAUNCH_KEY_START == KEY_START,
               "gdx3ds_launch_core.h key bits must match libctru's KEY_*");

extern const char* gdx3ds_fps_hud_build_id(void) __attribute__((weak));

#define LAUNCH_COUNTER_PATH "sdmc:/3ds/gdiffuser/launch_cycle.txt"
#define LAUNCH_WINDOW_MS_DEFAULT 3000
#define LAUNCH_WINDOW_MS_MAX 10000
#define LAUNCH_SHOW_CHOICE_MS 1200

static GdxLaunchLogFn sLog = NULL;
static int sOn = 0;
static int sHoldPending = 0;
static char sName[GDX_LAUNCH_NAME_LEN] = "main";
static char sVia[24] = "none";
static char sTag[GDX_LAUNCH_NAME_LEN + 3] = "";
static char sLogTag[GDX_LAUNCH_NAME_LEN] = "";
static int sWindowMs = 0;
static int sPickFrame = -1;
static int sWindowFrames = 0;
static u32 sEarlyHeld = 0;
static u32 sFirstHeld = 0;
static u32 sSeenHeld = 0;
static unsigned sBoot = 0;
static int sCycleCount = 0;
static int sCounterOk = 0;
static int sApplied = 0;
static int sFailed = 0;
static u32 sSimMask = 0;
static int sSimFrom = 0;
static PrintConsole sConsole;

static void LaunchLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void LaunchLog(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (sLog != NULL) {
        sLog(line);
    } else {
        svcOutputDebugString(line, strlen(line));
        gdx3ds_filelog_write(line, strlen(line));
    }
}

/* ------------------------------------------------------------------ selection ---- */

static void OverrideReceipt(const char* section, const char* key, const char* value, int rc, void* ctx) {
    (void)ctx;
    if (rc == 0) {
        LaunchLog("[launch]   forced [%s] %s = %s", section, key, value);
    } else {
        LaunchLog("[launch]   FAILED [%s] %s = %s (rc=%d%s)", section, key, value, rc,
                  rc == 2 ? ": override table full" : ": bad key");
        sFailed++;
    }
}

static void SetChoice(const char* name, const char* via) {
    snprintf(sName, sizeof(sName), "%s", name);
    snprintf(sVia, sizeof(sVia), "%s", via);
    snprintf(sTag, sizeof(sTag), "[%s]", sName);
    gdx_launch_log_tag(sName, sLogTag, sizeof(sLogTag));
}

static const char* LogFileName(char* out, size_t cap) {
    if (sLogTag[0] == '\0') {
        snprintf(out, cap, "log.txt");
    } else {
        snprintf(out, cap, "log-%s.txt", sLogTag);
    }
    return out;
}

static void LogChoiceReceipt(void) {
    char logName[40];
    const char* build = gdx3ds_fps_hud_build_id != NULL ? gdx3ds_fps_hud_build_id() : "?";
    LaunchLog("[launch] variant=%s via %s at frame %d (window %d ms, %d frames) early_held=0x%08lX "
              "first_held=0x%08lX seen_held=0x%08lX log=%s cfg=%d/%d build=%s",
              sName, sVia, sPickFrame, sWindowMs, sWindowFrames, (unsigned long)sEarlyHeld,
              (unsigned long)sFirstHeld, (unsigned long)sSeenHeld, LogFileName(logName, sizeof(logName)),
              gdx3ds_config_entry_count(), gdx3ds_config_entry_capacity(), build);
    if (sCycleCount > 0) {
        LaunchLog("[launch] cycle boot=%u of %d (%s) counter=%s", sBoot, sCycleCount,
                  gdx3ds_config_get_string("debug", "launch_cycle", ""), sCounterOk ? "advanced" : "FAILED");
    }
}

/* Apply the choice: the overrides, then the log name (before the file log opens). */
static void ApplyChoice(const char* name, const char* via) {
    char summary[96];
    if (!gdx_launch_variant_exists(name)) {
        LaunchLog("[launch] WARNING: variant '%s' (via %s) has no [variant.%s] section: running main", name, via, name);
        name = "main";
    }
    SetChoice(name, via);
    gdx3ds_filelog_set_tag(sLogTag);
    LogChoiceReceipt();
    gdx_launch_variant_summary(sName, summary, sizeof(summary));
    LaunchLog("[launch] %s: %s", sName, summary);
    sApplied = gdx_launch_apply_variant(sName, OverrideReceipt, NULL);
    if (sApplied > 0 || sFailed > 0) {
        LaunchLog("[launch] %d override(s) applied, %d failed", sApplied, sFailed);
    }
}

/* ------------------------------------------------------------------ cycle ---- */

/* The number of the boot about to run (missing/unreadable = 0); k+1 is on the card before the
 * pick is applied, so a freeze in this boot still advances the cycle. */
static int ReadAndAdvanceCounter(unsigned* bootOut) {
    unsigned boot = 0;
    FILE* f = fopen(LAUNCH_COUNTER_PATH, "r");
    if (f != NULL) {
        if (fscanf(f, "%u", &boot) != 1) {
            boot = 0;
        }
        fclose(f);
    }
    *bootOut = boot;
    f = fopen(LAUNCH_COUNTER_PATH, "w");
    if (f == NULL) {
        return 0;
    }
    const int ok = fprintf(f, "%u\n", boot + 1u) > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    return ok;
}

static void SelectByCycle(const char* list) {
    char name[GDX_LAUNCH_NAME_LEN];
    sCounterOk = ReadAndAdvanceCounter(&sBoot);
    sCycleCount = gdx_launch_cycle_pick(list, sBoot, name, sizeof(name));
    ApplyChoice(name, "cycle");
}

/* ------------------------------------------------------------------ hold window ---- */

static u32 ScanHeld(int frame) {
    hidScanInput();
    u32 held = hidKeysHeld();
    if (sSimMask != 0 && frame >= sSimFrom) {
        held |= sSimMask; /* TEST ONLY: [debug] launch_sim_hold */
    }
    return held;
}

static void PrintBindings(void) {
    int count = 0;
    const GdxLaunchButton* buttons = gdx_launch_buttons(&count);
    for (int i = 0; i < count; i++) {
        const char* name = gdx3ds_config_get_string("launch", buttons[i].bindKey, NULL);
        if (name == NULL || name[0] == '\0') {
            continue;
        }
        char summary[26];
        gdx_launch_variant_summary(name, summary, sizeof(summary));
        printf("  %-4s = %-5.5s %s\n", buttons[i].label, name, summary);
    }
    printf("  nothing = main (the ini as is)\n");
}

static void ShowPrompt(void) {
    gdx3ds_vram_guard_bottom_cpu_write("launch prompt");
    gdx3ds_boot_trail("[boot] launch prompt: consoleInit(GFX_BOTTOM)");
    consoleInit(GFX_BOTTOM, &sConsole);
    consoleSelect(&sConsole);
    printf("\x1b[2J\x1b[1;1H G-Diffuser launch variants\n\n HOLD a button NOW:\n\n");
    PrintBindings();
    printf("\n START = skip (main)\n");
    gdx3ds_boot_trail("[boot] launch prompt drawn; hold window open");
}

static void ShowCountdown(int remainingMs) {
    printf("\x1b[14;1H waiting %d.%d s ...   ", remainingMs / 1000, (remainingMs % 1000) / 100);
}

static void ShowChoice(void) {
    printf("\x1b[14;1H                          \x1b[16;1H THIS RUN: %s (%s)\n", sName, sVia);
    if (strcmp(sVia, "none") != 0 && strcmp(sVia, "start") != 0) {
        printf(" you can let go now\n");
    }
    svcSleepThread((s64)LAUNCH_SHOW_CHOICE_MS * 1000000LL);
    printf("\x1b[2J");
}

static int ElapsedMs(u64 startTick) {
    return (int)((svcGetSystemTick() - startTick) / CPU_TICKS_PER_MSEC);
}

/* Poll every vblank until a bound button is held, START is pressed, or the window ends. */
static const char* RunHoldWindow(const char** via) {
    const u64 start = svcGetSystemTick();
    for (int frame = 0;; frame++) {
        const u32 held = ScanHeld(frame);
        const GdxLaunchButton* button = NULL;
        const char* name = gdx_launch_pick_held(held, &button);
        sFirstHeld = frame == 0 ? held : sFirstHeld;
        sSeenHeld |= held;
        sWindowFrames = frame + 1;
        if (name != NULL) {
            sPickFrame = frame;
            *via = button->bindKey;
            return name;
        }
        if ((held & KEY_START) != 0) {
            sPickFrame = frame;
            *via = "start";
            return "main";
        }
        const int elapsed = ElapsedMs(start);
        if (elapsed >= sWindowMs) {
            *via = "none";
            return "main";
        }
        if ((frame % 6) == 0) {
            ShowCountdown(sWindowMs - elapsed);
        }
        gspWaitForVBlank();
    }
}

/* ------------------------------------------------------------------ entry points ---- */

static int ClampWindowMs(int ms) {
    if (ms < 0) {
        return 0;
    }
    return ms > LAUNCH_WINDOW_MS_MAX ? LAUNCH_WINDOW_MS_MAX : ms;
}

static void SampleEarlyHeld(void) {
    hidScanInput();
    sEarlyHeld = hidKeysHeld();
}

/* n60lead: [native] lcd_vram is read at gfxInit, before the hold window, so it cannot be an
 * ordinary [variant.*] lever: by the time a held L/R/X/Y is recognised (the window, at frame
 * ~30), gfxInit already ran with whatever the ini said. Two early-sample (main()-entry, before
 * gfxInit) escapes read the SAME sEarlyHeld this card already logs, so a risky lcd_vram=1 can
 * still be opt-in rather than baked into MAIN:
 *   - SELECT held, ini lcd_vram=1 -> forces it OFF for this run (safety net for a risky default).
 *   - [debug] lcd_vram_hold = l/r/x/y, that key held, ini lcd_vram=0 -> forces it ON for this run
 *     (opt-in on top of a safe default; pair it with the SAME key's [launch] hold_* binding so one
 *     held button both flips lcd_vram early and selects the matching [variant.*] at the window).
 * Best effort either way: the old O3DSHOME3 selector read this same point and saw nothing on
 * hardware, so the receipt (early_held=, lcd_vram_escape=/lcd_vram_hold=) is the evidence. */
static int sLcdVramEscaped = 0;
static int sLcdVramHoldOn = 0;

static void ApplyEarlyLcdVramEscape(void) {
    if ((sEarlyHeld & KEY_SELECT) == 0) {
        return;
    }
    if (gdx3ds_config_get_int("native", "lcd_vram", 0) == 0) {
        return;
    }
    if (gdx3ds_config_override("native", "lcd_vram", "0") == 0) {
        sLcdVramEscaped = 1;
        LaunchLog("[launch] lcd_vram_escape=1: SELECT held at the early sample (early_held=0x%08lX), "
                  "[native] lcd_vram forced 0 for this run", (unsigned long)sEarlyHeld);
    }
}

static u32 LcdVramHoldMask(void) {
    const char* key = gdx3ds_config_get_string("debug", "lcd_vram_hold", "");
    if (strcasecmp(key, "l") == 0) {
        return KEY_L;
    } else if (strcasecmp(key, "r") == 0) {
        return KEY_R;
    } else if (strcasecmp(key, "x") == 0) {
        return KEY_X;
    } else if (strcasecmp(key, "y") == 0) {
        return KEY_Y;
    } else {
        return 0;
    }
}

static void ApplyEarlyLcdVramHold(void) {
    const u32 mask = LcdVramHoldMask();
    if (mask == 0 || (sEarlyHeld & mask) == 0) {
        return;
    }
    if (gdx3ds_config_get_int("native", "lcd_vram", 0) != 0) {
        return; /* ini already asks for it: nothing to flip */
    }
    if (gdx3ds_config_override("native", "lcd_vram", "1") == 0) {
        sLcdVramHoldOn = 1;
        LaunchLog("[launch] lcd_vram_hold=1: [debug] lcd_vram_hold key held at the early sample "
                  "(early_held=0x%08lX), [native] lcd_vram forced 1 for this run", (unsigned long)sEarlyHeld);
    }
}

void gdx3ds_launch_boot_early(GdxLaunchLogFn log) {
    sLog = log;
    sOn = gdx3ds_config_get_bool("debug", "launch_variants", 0);
    if (!sOn) {
        return;
    }
    SampleEarlyHeld();
    gdx3ds_boot_trail_f("[boot] launch: early_held=0x%08lX", (unsigned long)sEarlyHeld);
    ApplyEarlyLcdVramEscape();
    ApplyEarlyLcdVramHold();
    gdx3ds_filelog_set_mirror_history(1); /* the file starts at boot, whenever it opens */
    const char* forced = gdx3ds_config_get_string("debug", "launch_force", "");
    const char* cycle = gdx3ds_config_get_string("debug", "launch_cycle", "");
    if (forced[0] != '\0') {
        ApplyChoice(forced, "force");
        return;
    }
    if (cycle[0] != '\0') {
        SelectByCycle(cycle);
        return;
    }
    sWindowMs = ClampWindowMs(gdx3ds_config_get_int("debug", "launch_window_ms", LAUNCH_WINDOW_MS_DEFAULT));
    gdx_launch_parse_sim_hold(gdx3ds_config_get_string("debug", "launch_sim_hold", ""), &sSimMask, &sSimFrom);
    if (sWindowMs == 0 || gdx_launch_bound_mask() == 0) {
        ApplyChoice("main", sWindowMs == 0 ? "no-window" : "no-bindings");
        return;
    }
    sHoldPending = 1;
}

int gdx3ds_launch_defers_filelog(void) {
    return sHoldPending;
}

void gdx3ds_launch_boot_window(void) {
    const char* via = "none";
    if (!sHoldPending) {
        return;
    }
    sHoldPending = 0;
    ShowPrompt();
    const char* name = RunHoldWindow(&via);
    SetChoice(gdx_launch_variant_exists(name) ? name : "main", via);
    gdx3ds_boot_trail_f("[boot] launch window closed: variant=%s via %s frames=%d", sName, sVia, sWindowFrames);
    ShowChoice();
    ApplyChoice(name, via);
    gdx3ds_filelog_init(); /* deferred: now named for the variant, history mirrored from boot */
    gdx3ds_boot_trail("[boot] file log open (after the launch window)");
}

const char* gdx3ds_launch_tag(void) {
    return sTag;
}

/* AUDIOFIX: the bare variant name ("main" when no launch variant fired, else "q"/"c"/"g"/...),
 * for sdmc:/3ds/gdiffuser/pcm-<variant>.wav -- gdx3ds_launch_tag()'s "[S]" is for the on-screen
 * fps row, not a filename. */
const char* gdx3ds_launch_variant_name(void) {
    return sName;
}
