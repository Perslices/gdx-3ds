/* port/3ds/gdx3ds_launch_core.h -- N60LAUNCH: the pure half of the launch-variant selector
 * (docs/research/native/n60launch-progress.md). Plain C over gdx3ds_config only, so the
 * selection rules are host-tested (port/3ds/test/gdx3ds_launch_host_tests.c); the HID poll, the
 * screen, the SD counter and the log file live in gdx3ds_launch.c.
 *
 * INI:
 *   [debug]
 *   launch_variants = 1        ; master lever (default 0: nothing below is read)
 *   launch_force = N           ; this variant, no button, no window (emulator / ini-driven run)
 *   launch_cycle = main,N,P    ; boot-cycling: boot k runs entry k % n (counter on the SD);
 *                              ; separators , / : or space. Set = no hold window.
 *   launch_window_ms = 3000    ; the hold window after the screen is up (0 = no window)
 *   launch_sim_hold = l@30     ; TEST ONLY: pretend L is held from window frame 30
 *   [launch]
 *   hold_l = N                 ; hold_l / hold_r / hold_x / hold_y / hold_up / hold_down
 *   hold_r = P
 *   [variant.N]                ; one section per variant: RAM-only overrides (never saved)
 *   ndsp_core1 = 1             ; a bare key is [debug]; write section.key for another section
 *
 * The name "main" (any case) is the ini as is. */
#ifndef GDX3DS_LAUNCH_CORE_H
#define GDX3DS_LAUNCH_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_LAUNCH_NAME_LEN 16
#define GDX_LAUNCH_MAX_CYCLE 8

/* libctru's KEY_* bit values (gdx3ds_launch.c static-asserts them). */
#define GDX_LAUNCH_KEY_START (1u << 3)
#define GDX_LAUNCH_KEY_DUP (1u << 6)
#define GDX_LAUNCH_KEY_DDOWN (1u << 7)
#define GDX_LAUNCH_KEY_R (1u << 8)
#define GDX_LAUNCH_KEY_L (1u << 9)
#define GDX_LAUNCH_KEY_X (1u << 10)
#define GDX_LAUNCH_KEY_Y (1u << 11)

typedef struct GdxLaunchButton {
    const char* bindKey; /* [launch] key, e.g. "hold_l" */
    const char* label;   /* on-screen name, e.g. "L" */
    uint32_t mask;
} GdxLaunchButton;

const GdxLaunchButton* gdx_launch_buttons(int* count);

int gdx_launch_is_main(const char* name);
/* 1 for "main" or a name with at least one [variant.<name>] entry. */
int gdx_launch_variant_exists(const char* name);

/* The buttons that [launch] binds to a variant name. */
uint32_t gdx_launch_bound_mask(void);
/* The variant bound to the first bound button in `held` (table order L, R, X, Y, UP, DOWN), or
 * NULL. *button (optional) gets that button's table entry. */
const char* gdx_launch_pick_held(uint32_t held, const GdxLaunchButton** button);

/* Split a cycle list into names; returns how many (0..max). */
int gdx_launch_cycle_parse(const char* list, char names[][GDX_LAUNCH_NAME_LEN], int max);
/* Entry boot % n of the list into out; returns n (0 = empty list, out untouched). */
int gdx_launch_cycle_pick(const char* list, unsigned boot, char* out, size_t cap);

/* A variant key: "section.key" -> (section, key); a bare key -> ("debug", key). 0 on bad input. */
int gdx_launch_split_key(const char* raw, char* section, size_t sectionCap, char* key, size_t keyCap);

/* Force every [variant.<name>] entry through gdx3ds_config_override. `sink` (optional) sees each
 * one with the override's return code. Returns entries applied (0 for main), -1 when the variant
 * does not exist. */
typedef void (*GdxLaunchOverrideSink)(const char* section, const char* key, const char* value, int rc, void* ctx);
int gdx_launch_apply_variant(const char* name, GdxLaunchOverrideSink sink, void* ctx);

/* "k=v k=v ..." of the variant's entries, truncated to cap. */
void gdx_launch_variant_summary(const char* name, char* out, size_t cap);

/* The log tag: the name with anything outside [A-Za-z0-9_-] dropped; "" for main. */
void gdx_launch_log_tag(const char* name, char* out, size_t cap);

/* "l@30" / "R" (from frame 0) -> mask + first frame. 0 when the spec names no known button. */
int gdx_launch_parse_sim_hold(const char* spec, uint32_t* mask, int* fromFrame);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_LAUNCH_CORE_H */
