/* gdx3ds_ships.c -- N60SHIPS: [ships] census and [lsplit] logic split (contract: gdx3ds_ships.h). */
#include "gfx/gdx3ds_ships.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"

int gdx3ds_ships_census_on = 0;
int gdx3ds_lsplit_on = 0;
int gdx3ds_nrr_ab_mask = 0;
int gdx3ds_nrr_ab_phase = 1;
static unsigned sAbPeriod = 0;
static unsigned long sIter = 0;

static int sLatched = 0;
static unsigned sOwnFrames = 0;

/* census window */
static uint32_t sCensusSum[GDX3DS_SHIPS_COUNT];
static uint32_t sCensusCalls = 0;
static int sMode = -1;
static int sPlayers = 0;
static int sTotal = 0;

/* logic split: the open bucket and when it opened (flat timeline inside Race_Update) */
static int sCur = GDX3DS_LSPLIT_NONE;
static uint64_t sT0 = 0;
static uint64_t sTicks[GDX3DS_LSPLIT_COUNT];
static uint32_t sRaceTicks = 0;
static uint32_t sReads = 0;

static void ShipsLog(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 2) {
        n = (int)sizeof(buf) - 2;
    }
    buf[n++] = '\n';
    buf[n] = '\0';
    svcOutputDebugString(buf, (size_t)n);
    gdx3ds_filelog_write(buf, (size_t)n);
}

void gdx3ds_ships_census_add(const uint8_t* counts, int gameMode, int numPlayers, int totalRacers) {
    int i;
    for (i = 0; i < GDX3DS_SHIPS_COUNT; i++) {
        sCensusSum[i] += counts[i];
    }
    sCensusCalls++;
    sMode = gameMode;
    sPlayers = numPlayers;
    sTotal = totalRacers;
}

static void LsplitClose(uint64_t now) {
    if (sCur >= 0) {
        sTicks[sCur] += now - sT0;
    }
}

void gdx3ds_lsplit_begin(void) {
    if (!gdx3ds_lsplit_on) {
        return;
    }
    sT0 = svcGetSystemTick();
    sReads++;
    sCur = GDX3DS_LSPLIT_MENUS;
    sRaceTicks++;
}

void gdx3ds_lsplit_mark_slow(int bucket) {
    uint64_t now;
    if (sCur < 0) {
        return; /* not inside Race_Update */
    }
    now = svcGetSystemTick();
    sReads++;
    LsplitClose(now);
    sCur = bucket;
    sT0 = now;
}

void gdx3ds_lsplit_end(void) {
    if (!gdx3ds_lsplit_on || sCur < 0) {
        return;
    }
    LsplitClose(svcGetSystemTick());
    sReads++;
    sCur = GDX3DS_LSPLIT_NONE;
}

static void Latch(void) {
    if (sLatched || gdx3ds_config_loaded() == 0) {
        return;
    }
    sLatched = 1;
    gdx3ds_ships_census_on = gdx3ds_config_get_int("debug", "ships_census", 0) != 0;
    gdx3ds_lsplit_on = gdx3ds_config_get_int("debug", "logic_split", 0) != 0;
    sAbPeriod = (unsigned)gdx3ds_config_get_int("debug", "nrr_ab_period", 0);
    gdx3ds_nrr_ab_mask = sAbPeriod != 0 ? gdx3ds_config_get_int("debug", "nrr_ab", 0) & 3 : 0;
    if (gdx3ds_nrr_ab_mask != 0) {
        gdx3ds_nrr_ab_phase = 0;
        ShipsLog("[ab] init mask=%d period=%u (bit0 nrr_mach, bit1 nrr_speck_batch: off in even blocks, on in odd)",
                 gdx3ds_nrr_ab_mask, sAbPeriod);
    }
    if (gdx3ds_ships_census_on || gdx3ds_lsplit_on) {
        ShipsLog("[ships] init census=%d logic_split=%d (read-only receipts; window = the [c5cat] window when its "
                 "clock is armed, else 64 iterations)",
                 gdx3ds_ships_census_on, gdx3ds_lsplit_on);
    }
}

static float Avg(uint32_t sum) {
    return sCensusCalls != 0 ? (float)sum / (float)sCensusCalls : 0.0f;
}

static float TickMs(uint64_t t) {
    return sRaceTicks != 0 ? (float)((double)t / CPU_TICKS_PER_MSEC / (double)sRaceTicks) : 0.0f;
}

static void EmitCensus(unsigned long frame) {
    const uint32_t* s = sCensusSum;
    if (sCensusCalls == 0) {
        return;
    }
    ShipsLog("[ships] frame=%lu calls=%u md=%d np=%d tot=%d | act=%.1f vis=%.2f l1=%.2f l2=%.2f l3=%.2f l4=%.2f "
             "l5=%.2f l6=%.2f | shd=%.2f boost=%.2f near=%.2f hl=%.2f crash=%.2f ghost=%.2f",
             frame, sCensusCalls, sMode, sPlayers, sTotal, (double)Avg(s[GDX3DS_SHIPS_ACT]),
             (double)Avg(s[GDX3DS_SHIPS_VIS]), (double)Avg(s[GDX3DS_SHIPS_L1]), (double)Avg(s[GDX3DS_SHIPS_L1 + 1]),
             (double)Avg(s[GDX3DS_SHIPS_L1 + 2]), (double)Avg(s[GDX3DS_SHIPS_L1 + 3]),
             (double)Avg(s[GDX3DS_SHIPS_L1 + 4]), (double)Avg(s[GDX3DS_SHIPS_L6]), (double)Avg(s[GDX3DS_SHIPS_SHD]),
             (double)Avg(s[GDX3DS_SHIPS_BOOST]), (double)Avg(s[GDX3DS_SHIPS_NEAR]), (double)Avg(s[GDX3DS_SHIPS_HL]),
             (double)Avg(s[GDX3DS_SHIPS_CRASH]), (double)Avg(s[GDX3DS_SHIPS_GHOST]));
}

static void EmitSplit(unsigned long frame) {
    float sum = 0.0f;
    int i;
    if (sRaceTicks == 0) {
        return;
    }
    for (i = 0; i < GDX3DS_LSPLIT_COUNT; i++) {
        sum += TickMs(sTicks[i]);
    }
    ShipsLog("[lsplit] frame=%lu ticks=%u | menus=%.3f efx=%.3f rpre=%.3f coll=%.3f ai=%.3f phys=%.3f rpost=%.3f "
             "cam=%.3f bg=%.3f crs=%.3f oth=%.3f | sum=%.3f reads=%.1f",
             frame, sRaceTicks, (double)TickMs(sTicks[GDX3DS_LSPLIT_MENUS]), (double)TickMs(sTicks[GDX3DS_LSPLIT_EFX]),
             (double)TickMs(sTicks[GDX3DS_LSPLIT_RPRE]), (double)TickMs(sTicks[GDX3DS_LSPLIT_COLL]),
             (double)TickMs(sTicks[GDX3DS_LSPLIT_AI]), (double)TickMs(sTicks[GDX3DS_LSPLIT_PHYS]),
             (double)TickMs(sTicks[GDX3DS_LSPLIT_RPOST]), (double)TickMs(sTicks[GDX3DS_LSPLIT_CAM]),
             (double)TickMs(sTicks[GDX3DS_LSPLIT_BG]), (double)TickMs(sTicks[GDX3DS_LSPLIT_CRS]),
             (double)TickMs(sTicks[GDX3DS_LSPLIT_OTH]), (double)sum, (double)sReads / (double)sRaceTicks);
}

void gdx3ds_ships_emit(unsigned long frame) {
    if (gdx3ds_nrr_ab_mask != 0) {
        ShipsLog("[ab] frame=%lu phase=%d mask=%d period=%u", frame, gdx3ds_nrr_ab_phase, gdx3ds_nrr_ab_mask,
                 sAbPeriod);
    }
    if (gdx3ds_ships_census_on) {
        EmitCensus(frame);
    }
    if (gdx3ds_lsplit_on) {
        EmitSplit(frame);
    }
    memset(sCensusSum, 0, sizeof(sCensusSum));
    sCensusCalls = 0;
    memset(sTicks, 0, sizeof(sTicks));
    sRaceTicks = 0;
    sReads = 0;
    sOwnFrames = 0;
}

void gdx3ds_ships_frame_end(unsigned long frame, int c5catArmed) {
    Latch();
    if (gdx3ds_nrr_ab_mask != 0) {
        sIter++;
        gdx3ds_nrr_ab_phase = (int)((sIter / sAbPeriod) & 1u);
    }
    if (!gdx3ds_ships_census_on && !gdx3ds_lsplit_on && gdx3ds_nrr_ab_mask == 0) {
        return;
    }
    if (c5catArmed) {
        return; /* gdx3ds_ships_emit rides the [c5cat] window */
    }
    if (++sOwnFrames >= (unsigned)gdx3ds_filelog_receipt_every()) { /* N60LOG: the [c5cat] window */
        gdx3ds_ships_emit(frame);
    }
}
