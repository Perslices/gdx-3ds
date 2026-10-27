/* port/3ds/os/gdx3ds_motion.c -- accelerometer tilt steering, see the header for the design.
 *
 * Sensor notes that are not obvious from the call sites:
 *
 *  - The accelerometer is a three-axis part on every 3DS model, so a single capture of the
 *    gravity vector carries both inputs this module needs: roll (steering) and pitch (the
 *    machine's nose-up/nose-down tilt). No irrst / Circle Pad Pro path is involved.
 *
 *  - The sensor is OFF until HIDUSER_EnableAccelerometer() is called, and until then
 *    hidAccelRead hands back zeros -- indistinguishable from "this console has no sensor", with
 *    the symptom that tilt steering simply never moves the car. Enabling is therefore tied to
 *    the mode (see SetSensorEnabled): OFF means disabled, which is also what keeps the feature
 *    free when a player does not use it. The Result is logged rather than dropped, and
 *    sensorOk carries it to the MENU.
 *
 *  - The math never uses the accelerometer's absolute scale. Every angle is an atan2 of a cross
 *    product against its dot product, which is invariant under a unit or gain difference, so a
 *    libctru whose accelVector is in g, in 1/4096 g, or in raw HID counts all produce the same
 *    angles. That is deliberate: no per-console calibration constant lives in this file. (A
 *    per-axis gain mismatch WOULD show up as a slight ellipse; it is invisible at the +-45 deg a
 *    steering input actually uses.)
 *
 *  - The neutral pose is stored as a VECTOR, not as an angle. atan2f of two absolute axes has a
 *    seam at +-180 deg, and a console held the normal way (screen facing the player, top edge up)
 *    puts gravity on exactly that seam: AY is -1g and AX is ~0, so a tremor flips the angle by
 *    360 deg instead of a degree. Taking the angle BETWEEN the captured vector and the current
 *    one has no seam anywhere on the sphere.
 *
 *  - Noise: the sensor is noisier than a stick and the bottom HUD/menu samples this every frame,
 *    so every axis goes through a one-pole low-pass (1/4 per poll, ~65 ms to settle at 60 Hz)
 *    before any math. That is slow enough to kill sensor jitter and fast enough to be
 *    imperceptible against a player's own reaction time.
 */
#include "gdx3ds_motion.h"
#include "gdx3ds_config.h"

#include <math.h>

#include <3ds.h>

/* Same diagnostic sink the rest of the OS layer uses (svcOutputDebugString + filelog when the
 * link provides it; NULL on a build that does not). */
extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));

#define GDX_MOTION_RAD_TO_DEG 57.29577951f

/* One-pole low-pass coefficient: new = old + (sample - old) / K. K=4 is ~65 ms at 60 Hz. */
#define GDX_MOTION_SMOOTH_DIV 4.0f

/* Neutral-pose capture: 30 polls (~0.5 s at 60 Hz). Long enough to average out a hand tremor on
 * the first frames of a race, short enough that the countdown hides it. */
#define GDX_MOTION_CAL_POLLS 30

/* Below this much total gravity the capture is rejected: a console held edge-on to gravity, or a
 * sensor that was never enabled and still reports zeros. */
#define GDX_MOTION_MIN_GRAVITY 0.001f

static int sMode = GDX3DS_MOTION_DEFAULT_MODE;
static int sAngle = GDX3DS_MOTION_DEFAULT_ANGLE;
static int sDeadzone = GDX3DS_MOTION_DEFAULT_DEADZONE;
static int sInvert = GDX3DS_MOTION_DEFAULT_INVERT;
static int sInvertPitch = GDX3DS_MOTION_DEFAULT_INVERT_PITCH;

/* Low-passed sensor state. */
static float sAx = 0.0f;
static float sAy = 0.0f;
static float sAz = 0.0f;

/* Captured neutral gravity vector -- see the seam note in the file header. */
static float sNeutralX = 0.0f;
static float sNeutralY = 0.0f;
static float sNeutralZ = 0.0f;
static int sCalibrated = 0;
static int sCalPolls = 0;
static float sCalSumX = 0.0f;
static float sCalSumY = 0.0f;
static float sCalSumZ = 0.0f;

/* Last published contributions, also what the INPUT tab readout shows. */
static int sSteer = 0;
static int sPitch = 0;

static int sActive = 0;
static int sSensorOn = 0;
static int sSensorOk = 0;

/* ---- small helpers ------------------------------------------------------------- */

static int ClampInt(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void ClearMotionState(void) {
    sSteer = 0;
    sPitch = 0;
    sCalPolls = 0;
    sCalSumX = 0.0f;
    sCalSumY = 0.0f;
    sCalSumZ = 0.0f;
    sCalibrated = 0;
}

/* Enable or disable the accelerometer to match the mode. Disabling matters as much as enabling:
 * a 3DS with the sensor on keeps HID sampling at its own rate for as long as the app runs, and
 * that is measurable on an Old 3DS even when nothing reads the result. */
static void SetSensorEnabled(int on) {
    if (on == sSensorOn) {
        return;
    }
    sSensorOn = on;
    if (on) {
        const Result rc = HIDUSER_EnableAccelerometer();
        sSensorOk = R_SUCCEEDED(rc) ? 1 : 0;
        if (&gdx_dbg_logf != NULL) {
            gdx_dbg_logf("[motion] accelerometer on rc=0x%08lx\n", (unsigned long) rc);
        }
    } else {
        HIDUSER_DisableAccelerometer();
        sSensorOk = 0;
        if (&gdx_dbg_logf != NULL) {
            gdx_dbg_logf("[motion] accelerometer off\n");
        }
    }
}

/* Roll and pitch since the captured neutral pose, in degrees. Both are the angle BETWEEN the
 * neutral gravity vector and the current one, resolved on the axis each rotation actually turns
 * about, so neither has a seam:
 *
 *   the rotation taking n to c has axis (n x c) and angle atan2(|n x c|, n.c);
 *   its +X component (console right) is the fore/aft lean, i.e. pitch;
 *   its +Z component (out of the screen) is the steering-wheel roll.
 *
 * Positive pitch means the console was leaned back (top edge toward the player), which maps to
 * positive stickY -- the same sense as pushing the N64 stick up, which racer.c takes as
 * nose-up. motion_invert_pitch flips it for a player who disagrees. */
static void TiltAnglesDeg(float* rollDeg, float* pitchDeg) {
    const float nx = sNeutralX, ny = sNeutralY, nz = sNeutralZ;
    const float cx = sAx, cy = sAy, cz = sAz;
    /* n x c */
    const float crossX = ny * cz - nz * cy;
    const float crossZ = nx * cy - ny * cx;
    const float dot = nx * cx + ny * cy + nz * cz;
    *rollDeg = atan2f(crossZ, dot) * GDX_MOTION_RAD_TO_DEG;
    *pitchDeg = atan2f(crossX, dot) * GDX_MOTION_RAD_TO_DEG;
}

/* One tilt angle to an N64 stick contribution, with the centre deadzone and the
 * motion_angle-to-full-deflection span. */
static int AngleToStick(float deltaDeg) {
    const float mag = fabsf(deltaDeg);
    const int span = sAngle - sDeadzone;
    int out;
    if (mag <= (float) sDeadzone || span <= 0) {
        return 0;
    }
    out = (int) ((mag - (float) sDeadzone) * (80.0f / (float) span));
    if (deltaDeg < 0.0f) {
        out = -out;
    }
    return ClampInt(out, -80, 80);
}

/* ---- config -------------------------------------------------------------------- */

void gdx3ds_motion_init(void) {
    sMode = ClampInt(gdx3ds_config_get_int("input", "motion", GDX3DS_MOTION_DEFAULT_MODE),
                      0, GDX3DS_MOTION_MODE_COUNT - 1);
    sAngle = ClampInt(gdx3ds_config_get_int("input", "motion_angle", GDX3DS_MOTION_DEFAULT_ANGLE), 10, 90);
    sDeadzone = ClampInt(gdx3ds_config_get_int("input", "motion_deadzone", GDX3DS_MOTION_DEFAULT_DEADZONE), 0, 20);
    sInvert = gdx3ds_config_get_int("input", "motion_invert", GDX3DS_MOTION_DEFAULT_INVERT) ? 1 : 0;
    sInvertPitch =
        gdx3ds_config_get_int("input", "motion_invert_pitch", GDX3DS_MOTION_DEFAULT_INVERT_PITCH) ? 1 : 0;
    ClearMotionState();

    /* A config that still says motion = 2 (the removed gyro mode) read 2 into sMode above; the
     * clamp lands it on TILT, which is the closest thing that mode did without the drift. */
    SetSensorEnabled(sMode != GDX3DS_MOTION_OFF);

    if (&gdx_dbg_logf != NULL) {
        static const char* const kNames[] = { "OFF", "TILT" };
        gdx_dbg_logf("[motion] mode=%s angle=%d dz=%d invert=%d/%d\n", kNames[sMode], sAngle,
                     sDeadzone, sInvert, sInvertPitch);
    }
}

int gdx3ds_motion_get_mode(void) {
    return sMode;
}

void gdx3ds_motion_set_mode(int mode) {
    const int next = ClampInt(mode, 0, GDX3DS_MOTION_MODE_COUNT - 1);
    if (next == sMode) {
        return;
    }
    sMode = next;
    /* The new mode has no neutral pose yet, and OFF must actually stop the sensor. */
    ClearMotionState();
    SetSensorEnabled(next != GDX3DS_MOTION_OFF);
}

int gdx3ds_motion_get_angle(void) {
    return sAngle;
}

void gdx3ds_motion_set_angle(int degrees) {
    sAngle = ClampInt(degrees, 10, 90);
}

int gdx3ds_motion_get_deadzone(void) {
    return sDeadzone;
}

void gdx3ds_motion_set_deadzone(int degrees) {
    sDeadzone = ClampInt(degrees, 0, 20);
}

int gdx3ds_motion_get_invert(void) {
    return sInvert;
}

void gdx3ds_motion_set_invert(int invert) {
    sInvert = invert ? 1 : 0;
}

int gdx3ds_motion_get_invert_pitch(void) {
    return sInvertPitch;
}

void gdx3ds_motion_set_invert_pitch(int invert) {
    sInvertPitch = invert ? 1 : 0;
}

/* ---- sampling ------------------------------------------------------------------ */

void gdx3ds_motion_poll(void) {
    accelVector accel;
    const float k = 1.0f / GDX_MOTION_SMOOTH_DIV;

    /* The whole feature costs nothing while it is off: no sensor read, no floating point, no
     * service traffic (the sensor is disabled too -- see SetSensorEnabled). */
    if (sMode == GDX3DS_MOTION_OFF || !sSensorOn) {
        return;
    }

    hidAccelRead(&accel);

    sAx += ((float) accel.x - sAx) * k;
    sAy += ((float) accel.y - sAy) * k;
    sAz += ((float) accel.z - sAz) * k;

    /* Neutral-pose capture. It lives here, in the once-per-frame sample, rather than in
     * gdx3ds_motion_apply(): apply() is only reached during live gameplay, so calibrating there
     * left the INPUT tab stuck on "hold the console still" for as long as the player was in the
     * menu -- the exact place they go to turn the mode on. */
    if (!sCalibrated) {
        sCalSumX += sAx;
        sCalSumY += sAy;
        sCalSumZ += sAz;
        if (++sCalPolls >= GDX_MOTION_CAL_POLLS) {
            const float n = (float) sCalPolls;
            const float nx = sCalSumX / n;
            const float ny = sCalSumY / n;
            const float nz = sCalSumZ / n;
            sCalPolls = 0;
            sCalSumX = 0.0f;
            sCalSumY = 0.0f;
            sCalSumZ = 0.0f;
            /* Reject a degenerate reference (no gravity at all) instead of locking to it: a zero
             * vector would read as "tilt does nothing", which is the failure this check exists to
             * keep visible in the MENU. */
            if (fabsf(nx) + fabsf(ny) + fabsf(nz) > GDX_MOTION_MIN_GRAVITY) {
                sNeutralX = nx;
                sNeutralY = ny;
                sNeutralZ = nz;
                sCalibrated = 1;
            }
        }
    }
}

void gdx3ds_motion_set_active(int active) {
    const int next = active ? 1 : 0;
    if (next == sActive) {
        return;
    }
    sActive = next;
    /* Both directions drop the state: entering gameplay captures a fresh neutral pose (the
     * console is held the way the player is about to play, not however it sat during the menu),
     * and leaving it must not leave a latched contribution behind. */
    ClearMotionState();
}

void gdx3ds_motion_recenter(void) {
    /* Drop the reference and let gdx3ds_motion_poll re-capture it over the next ~30 frames,
     * which is why RECENT works from the menu without a race running. */
    sCalibrated = 0;
    sCalPolls = 0;
    sCalSumX = 0.0f;
    sCalSumY = 0.0f;
    sCalSumZ = 0.0f;
    sSteer = 0;
    sPitch = 0;
}

/* ---- steering ------------------------------------------------------------------ */

void gdx3ds_motion_apply(int* stickX, int* stickY) {
    float rollDeg;
    float pitchDeg;

    /* Early out before any work -- including before the neutral-pose test -- so the OFF and
     * inactive cases are a couple of loads and a branch. */
    if (sMode == GDX3DS_MOTION_OFF || !sActive || !sCalibrated) {
        sSteer = 0;
        sPitch = 0;
        return;
    }

    TiltAnglesDeg(&rollDeg, &pitchDeg);

    sSteer = AngleToStick(rollDeg);
    sPitch = AngleToStick(pitchDeg);
    if (sInvert) {
        sSteer = -sSteer;
    }
    if (sInvertPitch) {
        sPitch = -sPitch;
    }

    if (stickX != NULL && sSteer != 0) {
        *stickX = ClampInt(*stickX + sSteer, -80, 80);
    }
    if (stickY != NULL && sPitch != 0) {
        *stickY = ClampInt(*stickY + sPitch, -80, 80);
    }
}

void gdx3ds_motion_readout(Gdx3dsMotionReadout* out) {
    if (out == NULL) {
        return;
    }
    out->accelX = (int) sAx;
    out->accelY = (int) sAy;
    out->steer = sActive ? sSteer : 0;
    out->pitch = sActive ? sPitch : 0;
    out->calibrated = sCalibrated;
    out->sensorOk = sSensorOk;
}
