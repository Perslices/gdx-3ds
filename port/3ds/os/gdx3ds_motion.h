/* port/3ds/os/gdx3ds_motion.h -- accelerometer tilt steering (MOTION) for the 3DS port.
 *
 * WHAT IT IS
 * The circle pad stays the primary input; this module ADDS a second, physical one: the player
 * tilts the console and the vehicle follows. It is additive on purpose -- the pad keeps working,
 * so a player can use either or both, and the feature can be switched off (the default) without
 * changing how the pad behaves.
 *
 * BOTH AXES
 * One three-axis accelerometer gives the full gravity direction, so the tilt decomposes into the
 * two things the game reads:
 *   roll  (turn like a steering wheel)  -> stickX, the racing steering input
 *   pitch (lean the top edge fore/aft)  -> stickY, which the decomp feeds to racer->tiltUpInput
 *                                          (decomp/src/game/racer.c:3451) -- the machine's
 *                                          nose-up/nose-down tilt
 * Both come from the same captured neutral pose, so a player never has to hold the console at
 * one particular angle.
 *
 * WHY IT LIVES HERE
 * gdx3ds_os_poll_input (gdx3ds_os_ctru.c) is the one place per frame that runs hidScanInput()
 * and produces the N64-mapped pad. The accelerometer is HID too, so it is sampled there, once per
 * frame, in gdx3ds_motion_poll(). The contribution is added in lus_glue/gdx3ds_input_glue.c --
 * the seam between that pad and port/input_bridge.c -- which is also where it is gated to live
 * gameplay: the same stick moves the MENU and the pause menu cursors, and tilting must never
 * move those.
 *
 * COST WHEN OFF
 * The mode is OFF by default and OFF means OFF: the accelerometer is not enabled, the HID
 * sampling loop it would keep running stays down, and gdx3ds_motion_poll returns before it reads
 * anything. Enabling and disabling happens on the mode change, so a player who never turns the
 * feature on pays nothing for it.
 *
 * WHY NO GYROSCOPE MODE
 * Rate integration (rotate to steer, ease back to centre) was tried and dropped: the 3DS gyro is
 * noisy enough that the only stable thing it produced was a binary "turning left / turning
 * right", which the accelerometer's absolute roll already gives more accurately and with no
 * drift. The gyroscope is therefore never enabled.
 *
 * All values are -80..80, the N64 stick range the rest of the port already speaks.
 *
 * Config ([input] section, all persisted by the MENU's INPUT tab):
 *   motion             0 off (default) / 1 tilt
 *   motion_angle       degrees of tilt for full deflection, 10..90, default 45
 *   motion_deadzone    degrees ignored around centre, 0..20, default 3
 *   motion_invert      1 flips the ROLL (stickX) sign
 *   motion_invert_pitch 1 flips the PITCH (stickY) sign -- independent of motion_invert, because
 *                      how a player holds the console can make one right and the other wrong
 *
 * (The old `motion_plane` key is gone: the full gravity vector is used now, so the Y/Z swap it
 * selected no longer exists. `motion = 2`, the removed gyro mode, is clamped to tilt on load.)
 */
#ifndef GDX3DS_MOTION_H
#define GDX3DS_MOTION_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX3DS_MOTION_OFF = 0,   /* accelerometer not enabled; zero cost */
    GDX3DS_MOTION_TILT = 1,  /* accelerometer: absolute roll + pitch vs a captured neutral pose */
    GDX3DS_MOTION_MODE_COUNT = 2
};

/* INPUT tab readout. Every field is what the module last published, so the menu can show the
 * sensor state without reaching into the module. */
typedef struct {
    int accelX;     /* low-passed accelerometer X, sensor counts (scale is unit-specific) */
    int accelY;     /* low-passed accelerometer Y */
    int steer;      /* last published stickX contribution, -80..80 */
    int pitch;      /* last published stickY contribution, -80..80 */
    int calibrated; /* 1 once the neutral pose is captured */
    int sensorOk;   /* 1 when the accelerometer was actually enabled */
} Gdx3dsMotionReadout;

/* Read the [input] motion* keys once, from gdx3ds_os_window_init right after the config load
 * (before the first poll), and enable the accelerometer if a mode is selected. Idempotent; the
 * MENU's setters below are the live path. */
void gdx3ds_motion_init(void);

/* Sample the accelerometer, low-pass it, and advance the neutral-pose capture. Called once per
 * frame from gdx3ds_os_poll_input, immediately after hidScanInput(). Returns before touching the
 * sensor at all when the mode is OFF. The capture runs here rather than in apply() so it also
 * completes while the player is in the menu instead of only during a race. */
void gdx3ds_motion_poll(void);

/* Tell the module whether the game is currently driving. Steering only happens while active; the
 * 1->0 edge drops the accumulated state and re-arms the capture, so a new race starts from the
 * player's current pose instead of a stale one. Call every frame from the pad seam. */
void gdx3ds_motion_set_active(int active);

/* Add the current tilt to *stickX (roll) and *stickY (pitch), clamped to -80..80. No-op unless
 * the module is active with a non-OFF mode; also no-op while the neutral pose is still being
 * captured. Either pointer may be NULL. */
void gdx3ds_motion_apply(int* stickX, int* stickY);

/* Re-capture the neutral pose over the next ~30 frames ("hold the console still"). The MENU
 * exposes it as the RECENT button. */
void gdx3ds_motion_recenter(void);

/* Fill `out` with the last published state. No-op when out is NULL. */
void gdx3ds_motion_readout(Gdx3dsMotionReadout* out);

int gdx3ds_motion_get_mode(void);
void gdx3ds_motion_set_mode(int mode);
int gdx3ds_motion_get_angle(void);
void gdx3ds_motion_set_angle(int degrees);
int gdx3ds_motion_get_deadzone(void);
void gdx3ds_motion_set_deadzone(int degrees);
int gdx3ds_motion_get_invert(void);
void gdx3ds_motion_set_invert(int invert);
int gdx3ds_motion_get_invert_pitch(void);
void gdx3ds_motion_set_invert_pitch(int invert);

/* Compile-time defaults, exported so the MENU's RESET DEFAULTS restores exactly these. */
#define GDX3DS_MOTION_DEFAULT_MODE         GDX3DS_MOTION_OFF
#define GDX3DS_MOTION_DEFAULT_ANGLE        45
#define GDX3DS_MOTION_DEFAULT_DEADZONE     3
#define GDX3DS_MOTION_DEFAULT_INVERT       0
#define GDX3DS_MOTION_DEFAULT_INVERT_PITCH 0

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MOTION_H */
