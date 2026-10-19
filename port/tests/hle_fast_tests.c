// Bit-exactness gate for [debug] audio_simd (port/n64_audio_hle_fast.c, NEXT.md C4k).
//
// THE CLAIM. With the lever on, gdx_audio_hle_run produces the same bytes as with it off: all
// 4 KB of DMEM, every host state buffer the ops write back (ADPCM/resample/filter state), every
// SAVEBUFF target, and the [spike] last-writer map. Not "close", not "inaudible": equal.
//
// THE METHOD. Both legs run the REAL interpreter (n64_audio_hle.c, compiled unmodified, the same
// way dsp_tests.c drives it) over the same command list from the same starting state; only
// gdx_audio_hle_set_fast() differs. Every list starts with LOADBUFFs of a full random DMEM image
// (which also resets the last-writer map to one value), a LOADADPCM and a SETLOOP, so no state
// leaks between legs or trials, and ends with SAVEBUFFs of all of DMEM, so the whole DMEM is
// observable. The host buffers are restored from a snapshot before each leg.
//
// THE INPUTS. Three generators, all deterministic (xorshift, fixed seeds):
//   voice    -- lists shaped like decomp synthesis.c's: per voice LOADBUFF + SETBUFF + ADPCM +
//               SETBUFF + RESAMPLE + ENVSETUP1/2 + ENVMIXER, then reverb MIXERs, the low-pass
//               FILTER pair, INTERLEAVE and SAVEBUFF. Offsets are the 16-byte-aligned kind the
//               game emits, so these exercise the FAST path (the report says how often).
//   random   -- any op, any field: odd offsets, ranges that wrap past 4 KB, overlapping
//               in/out windows, 2-bit ADPCM, extreme gains, volumes and ramps, pitch 0..0xFFFF,
//               short and odd-length codebooks. These exercise the PROOFS: every refusal must
//               fall back to the reference and still match.
//   aliased  -- in-place and self-aliased windows on purpose (resample out over its own input,
//               envmixer buses on the source and on each other, DMEMMOVE both directions,
//               interleave into its own input), the cases the ordering argument is about.
//
// The ndsphash in-game gate (FINAL fold=, two emulator runs, lever 0 vs 1) is the RECORDED
// input counterpart: the game's own command lists and sample data, which cannot ship here.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdx_audio_mix_census.h"
#include "n64_audio_hle_fast.h"

extern void gdx_audio_hle_run(const void* dataPtr, unsigned int dataSizeBytes);
extern void gdx_audio_hle_set_fast(int on);
extern void gdx_audio_hle_fast_counts(uint32_t* fastOps, uint32_t* fallbacks);
extern void gdx_audio_hle_debug_lastop(uint8_t out[256]);
extern void gdx_audio_hle_set_verify(int everyN);
extern void gdx_audio_hle_verify_counts(uint32_t* checked, uint32_t* skipped, uint32_t* mismatches);

// ---- harness stubs (the same four dsp_tests.c supplies) -----------------------------------
int gGdxRaceActive = 0;
void gdx_dbg_logf(const char* fmt, ...) { (void)fmt; }
int gdx_diag_audio_enabled(void) { return 0; }
int CVarGetInteger(const char* name, int defaultValue) {
    (void)name;
    return defaultValue;
}

// Fake-address regions: n64_audio_hle.c resolves every w1 address through these.
#define MAX_REGIONS 16
typedef struct {
    uint32_t base;
    uint8_t* host;
    uint32_t size;
} Region;
static Region sRegions[MAX_REGIONS];
static int sRegionCount = 0;
static uint32_t sNextAddr = 0x10000u;

static uint32_t RegisterRegion(void* host, uint32_t size) {
    const uint32_t base = sNextAddr;
    sRegions[sRegionCount].base = base;
    sRegions[sRegionCount].host = (uint8_t*)host;
    sRegions[sRegionCount].size = size;
    sRegionCount++;
    sNextAddr += size + 0x10000u;
    return base;
}

void* gdx_resolve_registered_host_address(unsigned int addr) {
    int i;
    for (i = 0; i < sRegionCount; i++) {
        const uint32_t off = addr - sRegions[i].base;
        if (off < sRegions[i].size) {
            return sRegions[i].host + off;
        }
    }
    return NULL;
}

void* gdx_resolve_module_host_address(unsigned int addr) {
    (void)addr;
    return NULL;
}

// ---- host memory the command lists address ------------------------------------------------
#define SAMPLE_BYTES 0x8000u
#define STATE_SLOTS 8u
#define SAVE_BYTES 0x2000u

typedef struct {
    uint8_t dmemImage[0x1000];
    uint8_t samples[SAMPLE_BYTES];
    uint8_t book[0x1000];
    int16_t loopState[16];
    int16_t adpcmState[STATE_SLOTS][16];
    int16_t resampleState[STATE_SLOTS][16];
    int16_t filterState[STATE_SLOTS][32];
    int16_t filterCoef[8];
    uint8_t save[SAVE_BYTES];
    uint8_t dmemOut[0x1000];
} HostMem;

static HostMem sMem;
static uint32_t aImage, aSamples, aBook, aLoop, aAdpcm, aResample, aFilterState, aCoef, aSave,
    aDmemOut;

static void RegisterHostMem(void) {
    aImage = RegisterRegion(sMem.dmemImage, sizeof(sMem.dmemImage));
    aSamples = RegisterRegion(sMem.samples, sizeof(sMem.samples));
    aBook = RegisterRegion(sMem.book, sizeof(sMem.book));
    aLoop = RegisterRegion(sMem.loopState, sizeof(sMem.loopState));
    aAdpcm = RegisterRegion(sMem.adpcmState, sizeof(sMem.adpcmState));
    aResample = RegisterRegion(sMem.resampleState, sizeof(sMem.resampleState));
    aFilterState = RegisterRegion(sMem.filterState, sizeof(sMem.filterState));
    aCoef = RegisterRegion(sMem.filterCoef, sizeof(sMem.filterCoef));
    aSave = RegisterRegion(sMem.save, sizeof(sMem.save));
    aDmemOut = RegisterRegion(sMem.dmemOut, sizeof(sMem.dmemOut));
}

// ---- deterministic randomness ---------------------------------------------------------------
static uint64_t sRng = 0x9E3779B97F4A7C15ull;

static uint32_t Rand32(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 7;
    sRng ^= sRng << 17;
    return (uint32_t)(sRng >> 11);
}

static uint32_t RandBelow(uint32_t n) {
    return (n == 0u) ? 0u : Rand32() % n;
}

static int Chance(uint32_t percent) {
    return RandBelow(100u) < percent;
}

static void FillRandom(void* p, size_t n) {
    uint8_t* b = (uint8_t*)p;
    size_t i;
    for (i = 0; i < n; i++) {
        b[i] = (uint8_t)Rand32();
    }
}

// ---- command encoders (bit layouts read field by field from gdx_audio_hle_run) --------------
typedef struct {
    uint32_t w0;
    uint32_t w1;
} Cmd;

enum {
    OP_ADPCM = 1, OP_CLEARBUFF = 2, OP_ADDMIXER = 4, OP_RESAMPLE = 5, OP_FILTER = 7,
    OP_SETBUFF = 8, OP_DMEMMOVE = 10, OP_LOADADPCM = 11, OP_MIXER = 12, OP_INTERLEAVE = 13,
    OP_HILOGAIN = 14, OP_SETLOOP = 15, OP_INTERL = 17, OP_ENVSETUP1 = 18, OP_ENVMIXER = 19,
    OP_LOADBUFF = 20, OP_SAVEBUFF = 21, OP_ENVSETUP2 = 22, OP_S8DEC = 23
};

#define MAX_CMDS 512
static Cmd sList[MAX_CMDS];
static int sCount = 0;

static void Emit(uint32_t w0, uint32_t w1) {
    if (sCount < MAX_CMDS) {
        sList[sCount].w0 = w0;
        sList[sCount].w1 = w1;
        sCount++;
    }
}

static uint32_t Op(uint32_t op) {
    return op << 24;
}

static void EmitLoad(uint32_t dmem, uint32_t bytes, uint32_t addr) {
    Emit(Op(OP_LOADBUFF) | (((bytes >> 4) & 0xFFu) << 16) | (dmem & 0xFFFFu), addr);
}

static void EmitSave(uint32_t dmem, uint32_t bytes, uint32_t addr) {
    Emit(Op(OP_SAVEBUFF) | (((bytes >> 4) & 0xFFu) << 16) | (dmem & 0xFFFFu), addr);
}

static void EmitSetBuff(uint32_t in, uint32_t out, uint32_t count) {
    Emit(Op(OP_SETBUFF) | (in & 0xFFFFu), ((out & 0xFFFFu) << 16) | (count & 0xFFFFu));
}

static void EmitEnvMixer(uint32_t src, uint32_t n, uint32_t swap, const uint32_t d[4]) {
    Emit(Op(OP_ENVMIXER) | (((src >> 4) & 0xFFu) << 16) | ((n & 0xFFu) << 8) | ((swap & 1u) << 4),
         (((d[0] >> 4) & 0xFFu) << 24) | (((d[1] >> 4) & 0xFFu) << 16) | (((d[2] >> 4) & 0xFFu) << 8) |
             ((d[3] >> 4) & 0xFFu));
}

// Prefix: full DMEM image (4 x 0x400), codebook, loop state. Suffix: DMEM out (4 x 0x400).
static void EmitPrologue(uint32_t bookLen) {
    uint32_t k;
    for (k = 0; k < 4u; k++) {
        EmitLoad(k * 0x400u, 0x400u, aImage + k * 0x400u);
    }
    Emit(Op(OP_LOADADPCM) | (bookLen & 0xFFFFFFu), aBook);
    Emit(Op(OP_SETLOOP), aLoop);
}

static void EmitEpilogue(void) {
    uint32_t k;
    for (k = 0; k < 4u; k++) {
        EmitSave(k * 0x400u, 0x400u, aDmemOut + k * 0x400u);
    }
}

// ---- field generators --------------------------------------------------------------------
// Mostly the game's shape (16-byte aligned, in range), sometimes anything.
static uint32_t RandDmem(int adversarial) {
    if (!adversarial || Chance(60)) {
        return RandBelow(0xF0u) << 4;
    }
    if (Chance(50)) {
        return RandBelow(0x800u) * 2u; /* even, may run past 4 KB */
    }
    return RandBelow(0x2000u); /* odd or beyond DMEM */
}

static uint32_t RandStateAddr(uint32_t base, uint32_t slotBytes) {
    return base + RandBelow(STATE_SLOTS) * slotBytes;
}

static int32_t RandS16(void) {
    return (int16_t)(uint16_t)Rand32();
}

static uint32_t RandPitch(void) {
    if (Chance(70)) {
        return 0x2000u + RandBelow(0xC000u);
    }
    return Rand32() & 0xFFFFu;
}

// ---- generators ---------------------------------------------------------------------------
/* DMEM layout from decomp/src/audio/disk/lib/audio.h + synthesis.c: compressed ADPCM in a
   window ending at DMEM_COMPRESSED_ADPCM_DATA (0x940), decoded to DMEM_UNCOMPRESSED_NOTE
   (0x580), resampled to DMEM_TEMP (0x3C0), enveloped into LEFT/RIGHT (0x940/0xAE0) and
   WET_LEFT/WET_RIGHT (0xC80/0xE20), interleaved back into DMEM_TEMP and saved. */
static void GenVoiceList(void) {
    const uint32_t voices = 1u + RandBelow(16u);
    const uint32_t aiBytes = 0x160u + RandBelow(3u) * 0x10u; /* aiBufLen * SAMPLE_SIZE */
    uint32_t v;
    uint32_t k;
    Emit(Op(OP_CLEARBUFF) | 0x940u, 0x340u);
    Emit(Op(OP_CLEARBUFF) | 0xC80u, 0x340u);
    for (v = 0; v < voices; v++) {
        const uint32_t adpcmFlags = Chance(10) ? 1u : (Chance(10) ? 2u : 0u);
        const uint32_t d[4] = { 0x940u, 0xAE0u, 0xC80u, 0xE20u };
        const uint32_t frames = 8u + RandBelow(20u);
        const uint32_t loadBytes = ((frames * 9u) + 15u) & ~15u;
        const uint32_t skip = RandBelow(16u) * 2u;
        EmitLoad(0x940u - loadBytes, loadBytes, aSamples + RandBelow(SAMPLE_BYTES - 0x200u));
        EmitSetBuff(0x940u - loadBytes, 0x580u, frames * 32u);
        Emit(Op(OP_ADPCM) | (adpcmFlags << 16), RandStateAddr(aAdpcm, 32u));
        EmitSetBuff(0x580u + skip, 0x3C0u, aiBytes);
        Emit(Op(OP_RESAMPLE) | ((Chance(10) ? 1u : 0u) << 16) | RandPitch(), RandStateAddr(aResample, 32u));
        if (Chance(20)) {
            Emit(Op(OP_HILOGAIN) | ((0x10u + RandBelow(0x40u)) << 16) | (aiBytes + 32u), 0x3C0u << 16);
        }
        if (Chance(15)) {
            Emit(Op(OP_FILTER) | (2u << 16) | aiBytes, aCoef);
            Emit(Op(OP_FILTER) | (adpcmFlags & 1u) << 16 | 0x3C0u, RandStateAddr(aFilterState, 64u));
        }
        Emit(Op(OP_ENVSETUP1) | (RandBelow(0x80u) << 16) | ((uint32_t)RandS16() & 0x3FFu),
             (((uint32_t)RandS16() & 0x7FFu) << 16) | ((uint32_t)RandS16() & 0x7FFu));
        Emit(Op(OP_ENVSETUP2), ((Rand32() & 0xFFF0u) << 16) | (Rand32() & 0xFFF0u));
        EmitEnvMixer(0x3C0u, aiBytes / 2u, Chance(20) ? 1u : 0u, d);
    }
    // reverb: decay mix in place, low-pass pair, wet->dry return
    Emit(Op(OP_MIXER) | ((aiBytes >> 4) << 16) | (Rand32() & 0x7FFFu), (0xC80u << 16) | 0xC80u);
    Emit(Op(OP_FILTER) | (2u << 16) | aiBytes, aCoef);
    Emit(Op(OP_FILTER) | ((Chance(10) ? 1u : 0u) << 16) | 0xC80u, RandStateAddr(aFilterState, 64u));
    Emit(Op(OP_MIXER) | ((aiBytes >> 4) << 16) | (Rand32() & 0x7FFFu), (0xC80u << 16) | 0x940u);
    Emit(Op(OP_ADDMIXER) | ((aiBytes >> 4) << 16), (0xE20u << 16) | 0xAE0u);
    Emit(Op(OP_INTERLEAVE) | ((aiBytes >> 4) << 16) | 0x3C0u, (0x940u << 16) | 0xAE0u);
    for (k = 0; k < 3u; k++) {
        EmitSave(0x3C0u + k * 0xF0u, 0xF0u, aSave + k * 0x100u);
    }
}

static void GenRandomOp(int adversarial) {
    switch (RandBelow(19u)) {
        case 0:
            Emit(Op(OP_CLEARBUFF) | RandDmem(adversarial), RandBelow(0x600u));
            break;
        case 1:
            EmitSetBuff(RandDmem(adversarial), RandDmem(adversarial), RandBelow(0x400u));
            break;
        case 2:
            Emit(Op(OP_DMEMMOVE) | RandDmem(adversarial), (RandDmem(adversarial) << 16) | RandBelow(0x600u));
            break;
        case 3:
            EmitLoad(RandDmem(adversarial), RandBelow(0x40u) << 4, aSamples + RandBelow(SAMPLE_BYTES - 0x1000u));
            break;
        case 4:
            EmitSave(RandDmem(adversarial), RandBelow(0x40u) << 4, aSave + RandBelow(SAVE_BYTES - 0x1000u));
            break;
        case 5:
            Emit(Op(OP_ADPCM) | (RandBelow(8u) << 16), RandStateAddr(aAdpcm, 32u));
            break;
        case 6:
            Emit(Op(OP_RESAMPLE) | (RandBelow(4u) << 16) | RandPitch(), RandStateAddr(aResample, 32u));
            break;
        case 7:
            Emit(Op(OP_MIXER) | (RandBelow(0x60u) << 16) | (Rand32() & 0xFFFFu),
                 (RandDmem(adversarial) << 16) | RandDmem(adversarial));
            break;
        case 8:
            Emit(Op(OP_ADDMIXER) | (RandBelow(0x60u) << 16), (RandDmem(adversarial) << 16) | RandDmem(adversarial));
            break;
        case 9:
            Emit(Op(OP_INTERLEAVE) | (RandBelow(0x30u) << 16) | RandDmem(adversarial),
                 (RandDmem(adversarial) << 16) | RandDmem(adversarial));
            break;
        case 10:
            Emit(Op(OP_HILOGAIN) | (RandBelow(0x100u) << 16) | RandBelow(0x400u), RandDmem(adversarial) << 16);
            break;
        case 11:
            Emit(Op(OP_FILTER) | (2u << 16) | RandBelow(0x400u), aCoef);
            break;
        case 12:
            Emit(Op(OP_FILTER) | (RandBelow(2u) << 16) | RandDmem(adversarial), RandStateAddr(aFilterState, 64u));
            break;
        case 13:
            Emit(Op(OP_ENVSETUP1) | (RandBelow(0x100u) << 16) | (Rand32() & 0xFFFFu), Rand32());
            break;
        case 14:
            Emit(Op(OP_ENVSETUP2), Rand32());
            break;
        case 15: {
            const uint32_t d[4] = { RandDmem(adversarial), RandDmem(adversarial), RandDmem(adversarial),
                                    RandDmem(adversarial) };
            EmitEnvMixer(RandDmem(adversarial), RandBelow(0x100u), RandBelow(2u), d);
            break;
        }
        case 16:
            Emit(Op(OP_SETLOOP), Chance(50) ? aLoop : RandStateAddr(aAdpcm, 32u));
            break;
        case 17:
            Emit(Op(OP_S8DEC) | (RandBelow(2u) << 16), RandStateAddr(aAdpcm, 32u));
            break;
        default:
            Emit(Op(OP_INTERL) | RandBelow(0x200u), (RandDmem(adversarial) << 16) | RandDmem(adversarial));
            break;
    }
}

// Windows that alias on purpose: the ordering argument's test cases.
static void GenAliasedOp(void) {
    const uint32_t base = RandBelow(0xC0u) << 4;
    const uint32_t delta = (RandBelow(9u) << 4) * (Chance(50) ? 1u : 0u) + (Chance(30) ? 2u : 0u);
    switch (RandBelow(6u)) {
        case 0: /* resample output over its own input, ahead or behind */
            EmitSetBuff(base, base + delta, 0x40u + RandBelow(0x100u));
            Emit(Op(OP_RESAMPLE) | RandPitch(), RandStateAddr(aResample, 32u));
            break;
        case 1: { /* envmixer buses on the source and on each other */
            const uint32_t d[4] = { base, base + (RandBelow(3u) << 4), base + 0x10u, base };
            EmitEnvMixer(base + (RandBelow(2u) << 4), 8u + RandBelow(0x80u), RandBelow(2u), d);
            break;
        }
        case 2:
            Emit(Op(OP_DMEMMOVE) | base, ((base + delta) << 16) | RandBelow(0x200u));
            break;
        case 3:
            Emit(Op(OP_DMEMMOVE) | (base + delta), (base << 16) | RandBelow(0x200u));
            break;
        case 4:
            Emit(Op(OP_INTERLEAVE) | (RandBelow(0x20u) << 16) | base, (base << 16) | (base + delta));
            break;
        default: /* decode window overlapping the compressed input */
            EmitSetBuff(base, base + delta, RandBelow(0x200u));
            Emit(Op(OP_ADPCM) | (RandBelow(3u) << 16), RandStateAddr(aAdpcm, 32u));
            break;
    }
}

// ---- one trial: same list, both legs, compare everything ------------------------------------
typedef struct {
    HostMem mem;
    uint8_t lastOp[256];
} Outcome;

static HostMem sSnapshot;
static Outcome sLeg[2];

static void RandomizeHostMem(void) {
    uint32_t i;
    FillRandom(&sMem, sizeof(sMem));
    // Most frames get realistic shifts (<= 12), some anything, so both the common case and the
    // int64-only overflow case run.
    for (i = 0; i + 9u <= SAMPLE_BYTES; i += 9u) {
        if (Chance(80)) {
            sMem.samples[i] = (uint8_t)((RandBelow(13u) << 4) | RandBelow(16u));
        }
    }
    memset(sMem.dmemOut, 0, sizeof(sMem.dmemOut));
}

static uint32_t RandBookLen(void) {
    static const uint32_t kLens[] = { 0u, 1u, 7u, 32u, 33u, 64u, 256u, 512u, 4096u };
    if (Chance(80)) {
        return kLens[RandBelow(sizeof(kLens) / sizeof(kLens[0]))];
    }
    return RandBelow(0x1000u);
}

static void RunLeg(int fast, Outcome* out) {
    memcpy(&sMem, &sSnapshot, sizeof(sMem));
    gdx_audio_hle_set_fast(fast);
    gdx_audio_hle_run(sList, (unsigned int)(sCount * (int)sizeof(Cmd)));
    memcpy(&out->mem, &sMem, sizeof(sMem));
    gdx_audio_hle_debug_lastop(out->lastOp);
}

static int FirstDiff(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    size_t i;
    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)i;
        }
    }
    return -1;
}

static int CompareLegs(const char* gen, unsigned trial) {
    const int memDiff = FirstDiff(&sLeg[0].mem, &sLeg[1].mem, sizeof(HostMem));
    const int opDiff = FirstDiff(sLeg[0].lastOp, sLeg[1].lastOp, sizeof(sLeg[0].lastOp));
    if (memDiff < 0 && opDiff < 0) {
        return 0;
    }
    printf("FAIL %s trial=%u cmds=%d host-byte=%d lastop-block=%d\n", gen, trial, sCount, memDiff, opDiff);
    return 1;
}

typedef enum { GEN_VOICE, GEN_RANDOM, GEN_ALIASED } GenKind;

static void BuildList(GenKind kind) {
    int ops;
    int k;
    sCount = 0;
    EmitPrologue(RandBookLen());
    if (kind == GEN_VOICE) {
        GenVoiceList();
    } else {
        ops = 8 + (int)RandBelow(40u);
        for (k = 0; k < ops; k++) {
            if (kind == GEN_ALIASED && Chance(60)) {
                GenAliasedOp();
            } else {
                GenRandomOp(kind != GEN_VOICE);
            }
        }
    }
    EmitEpilogue();
}

static int RunGenerator(const char* name, GenKind kind, unsigned trials) {
    uint32_t fast0, fb0, fast1, fb1;
    unsigned t;
    int failures = 0;
    gdx_audio_hle_fast_counts(&fast0, &fb0);
    for (t = 0; t < trials; t++) {
        RandomizeHostMem();
        memcpy(&sSnapshot, &sMem, sizeof(sMem));
        BuildList(kind);
        RunLeg(0, &sLeg[0]);
        RunLeg(1, &sLeg[1]);
        failures += CompareLegs(name, t);
        if (failures > 10) {
            break;
        }
    }
    gdx_audio_hle_fast_counts(&fast1, &fb1);
    printf("%-8s trials=%u fastOps=%u fallbacks=%u failures=%d\n", name, trials,
           (unsigned)(fast1 - fast0), (unsigned)(fb1 - fb0), failures);
    // A gate that never takes the fast path proves nothing: demand real coverage.
    if (kind == GEN_VOICE && (fast1 - fast0) < trials * 20u) {
        printf("FAIL %s: fast path barely exercised\n", name);
        failures++;
    }
    return failures;
}

/* NEGATIVE CONTROL: the gate must FIRE on a real one-bit fault in a fast kernel (one sample of
   every fast ADPCM op), both in the leg-vs-leg comparison and in the on-device verify mode. */
/* A decode whose output nothing downstream overwrites: prologue, one ADPCM op, epilogue. */
static void BuildDecodeOnlyList(void) {
    sCount = 0;
    EmitPrologue(512u);
    EmitLoad(0x940u - 0x100u, 0x100u, aSamples + RandBelow(SAMPLE_BYTES - 0x200u));
    EmitSetBuff(0x940u - 0x100u, 0x580u, 0x100u);
    Emit(Op(OP_ADPCM), RandStateAddr(aAdpcm, 32u));
    EmitEpilogue();
}

static int NegativeControl(void) {
    uint32_t checked0, skipped0, mism0, checked1, skipped1, mism1;
    int detected = 0;
    unsigned t;
    gdx_hlefast_inject_fault(1);
    for (t = 0; t < 20u; t++) {
        RandomizeHostMem();
        memcpy(&sSnapshot, &sMem, sizeof(sMem));
        BuildDecodeOnlyList();
        RunLeg(0, &sLeg[0]);
        RunLeg(1, &sLeg[1]);
        if (FirstDiff(&sLeg[0].mem, &sLeg[1].mem, sizeof(HostMem)) >= 0 ||
            FirstDiff(sLeg[0].lastOp, sLeg[1].lastOp, sizeof(sLeg[0].lastOp)) >= 0) {
            detected++;
        }
    }
    gdx_audio_hle_verify_counts(&checked0, &skipped0, &mism0);
    gdx_audio_hle_set_verify(1);
    for (t = 0; t < 20u; t++) {
        RandomizeHostMem();
        memcpy(&sSnapshot, &sMem, sizeof(sMem));
        BuildDecodeOnlyList();
        RunLeg(1, &sLeg[1]);
    }
    gdx_audio_hle_set_verify(0);
    gdx_hlefast_inject_fault(0);
    gdx_audio_hle_verify_counts(&checked1, &skipped1, &mism1);
    printf("negative control: parity gate fired on %d/20 faulted lists, verify mode on %u/%u\n",
           detected, (unsigned)(mism1 - mism0), (unsigned)(checked1 - checked0));
    if (detected < 20 || mism1 - mism0 < 20u) {
        printf("FAIL negative control: an injected one-bit kernel fault was not detected\n");
        return 1;
    }
    return 0;
}

/* --bench: host wall time of the voice lists, reference vs fast. A host CPU is not an ARM11;
   the number is a sanity check that the fast path is faster, never a hardware claim. */
#include <time.h>
static double BenchLeg(int fast, unsigned lists, unsigned repeats) {
    double total = 0.0;
    unsigned t;
    unsigned r;
    sRng = 0x1234567u;
    for (t = 0; t < lists; t++) {
        clock_t c0;
        RandomizeHostMem();
        BuildList(GEN_VOICE);
        gdx_audio_hle_set_fast(fast);
        c0 = clock();
        for (r = 0; r < repeats; r++) {
            gdx_audio_hle_run(sList, (unsigned int)(sCount * (int)sizeof(Cmd)));
        }
        total += (double)(clock() - c0);
    }
    return total * 1.0e6 / CLOCKS_PER_SEC / (double)(lists * repeats);
}

static void Bench(void) {
    const double ref = BenchLeg(0, 200u, 100u);
    const double fast = BenchLeg(1, 200u, 100u);
    printf("bench (host): reference %.2f us/list, fast %.2f us/list, x%.2f\n", ref, fast, ref / fast);
}

/* The packed resample path sums (x-1*c0 + x0*c1) + (x1*c2 + x2*c3) where the reference sums
   left to right. That is only the same int32 if no partial sum can wrap: assert the ROM table's
   worst row keeps 32768 * sum|c| below 2^31. The table is the interpreter's own (via the same
   A_RESAMPLE path) -- read here from the copy dsp_tests.c keeps for exactly this purpose would
   duplicate it, so the bound is checked on the interpreter's rows through a probe command:
   a unit impulse of -32768 at each tap position, all four at once, is the worst case. */
static int CheckResampleBound(void) {
    extern const int16_t* gdx_audio_hle_debug_resample_table(void);
    const int16_t* t = gdx_audio_hle_debug_resample_table();
    int64_t worst = 0;
    int p;
    for (p = 0; p < 64; p++) {
        int64_t sum = 0;
        int k;
        for (k = 0; k < 4; k++) {
            const int32_t c = t[p * 4 + k];
            sum += (c < 0) ? -c : c;
        }
        if (sum > worst) {
            worst = sum;
        }
    }
    if (worst * 32768 >= ((int64_t)1 << 31)) {
        printf("FAIL resample bound: worst row sum|c|=%lld reaches 2^31\n", (long long)worst);
        return 1;
    }
    printf("resample bound: worst row sum|c|=%lld, 32768*sum=%lld < 2^31\n", (long long)worst,
           (long long)(worst * 32768));
    return 0;
}

/* The on-device verify mode ([debug] audio_simd_verify) run here on every tick: it must check
   every list, find nothing, and leave exactly the bytes a plain fast run leaves. */
static int CheckVerifyMode(void) {
    uint32_t checked0, skipped0, mism0, checked1, skipped1, mism1;
    unsigned t;
    int failures = 0;
    gdx_audio_hle_verify_counts(&checked0, &skipped0, &mism0);
    for (t = 0; t < 500u; t++) {
        RandomizeHostMem();
        memcpy(&sSnapshot, &sMem, sizeof(sMem));
        BuildList((t & 1u) ? GEN_VOICE : GEN_ALIASED);
        RunLeg(1, &sLeg[0]);
        gdx_audio_hle_set_verify(1);
        RunLeg(1, &sLeg[1]);
        gdx_audio_hle_set_verify(0);
        failures += CompareLegs("verify", t);
    }
    gdx_audio_hle_verify_counts(&checked1, &skipped1, &mism1);
    printf("verify mode: checked=%u skipped=%u mismatches=%u\n", (unsigned)(checked1 - checked0),
           (unsigned)(skipped1 - skipped0), (unsigned)(mism1 - mism0));
    if (mism1 != mism0 || checked1 - checked0 < 400u) {
        printf("FAIL verify mode\n");
        failures++;
    }
    return failures;
}

/* Every packed path and every in-kernel fallback must have run, or the gate is vacuous. */
/* o3dscrackle [debug] audio_rs_align=1: the RSP tap window x[c-4..c-1]. Both legs run it (the
   lever is the interpreter's, not the fast path's), so this is the same reference-vs-fast proof
   on the other window, with its own floor on both inner resample paths. */
extern void gdx_audio_hle_set_rs_align(int on);

static int RunRsAlignPass(void) {
    uint32_t before[GDX_HLEFAST_PATH_COUNT];
    uint32_t after[GDX_HLEFAST_PATH_COUNT];
    int failures = 0;
    gdx_hlefast_path_counts(before);
    gdx_audio_hle_set_rs_align(1);
    failures += RunGenerator("voice/rs", GEN_VOICE, 3000u);
    failures += RunGenerator("random/rs", GEN_RANDOM, 3000u);
    failures += RunGenerator("alias/rs", GEN_ALIASED, 3000u);
    gdx_audio_hle_set_rs_align(0);
    gdx_hlefast_path_counts(after);
    printf("rs_align=1 paths: resample.packed=%u resample.scalar=%u\n",
           (unsigned)(after[GDX_HLEFAST_PATH_RESAMPLE_PACKED] - before[GDX_HLEFAST_PATH_RESAMPLE_PACKED]),
           (unsigned)(after[GDX_HLEFAST_PATH_RESAMPLE_SCALAR] - before[GDX_HLEFAST_PATH_RESAMPLE_SCALAR]));
    if (after[GDX_HLEFAST_PATH_RESAMPLE_PACKED] - before[GDX_HLEFAST_PATH_RESAMPLE_PACKED] < 100u ||
        after[GDX_HLEFAST_PATH_RESAMPLE_SCALAR] - before[GDX_HLEFAST_PATH_RESAMPLE_SCALAR] < 100u) {
        printf("FAIL rs_align=1: a resample inner path ran fewer than 100 times\n");
        failures++;
    }
    return failures;
}

static int CheckPathCoverage(void) {
    static const char* const kNames[GDX_HLEFAST_PATH_COUNT] = {
        "adpcm.packed", "adpcm.wide", "resample.packed", "resample.scalar",
        "envmix.packed", "envmix.scalar", "mixer.packed", "mixer.scalar"
    };
    uint32_t counts[GDX_HLEFAST_PATH_COUNT];
    int failures = 0;
    int p;
    gdx_hlefast_path_counts(counts);
    printf("paths:");
    for (p = 0; p < GDX_HLEFAST_PATH_COUNT; p++) {
        printf(" %s=%u", kNames[p], (unsigned)counts[p]);
        if (counts[p] < 100u) {
            failures++;
        }
    }
    printf("\n");
    if (failures != 0) {
        printf("FAIL path coverage: %d inner path(s) ran fewer than 100 times\n", failures);
    }
    return failures;
}

int main(int argc, char** argv) {
    int failures = 0;
    RegisterHostMem();
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        Bench();
        return 0;
    }
    failures += RunGenerator("voice", GEN_VOICE, 3000u);
    failures += RunGenerator("random", GEN_RANDOM, 6000u);
    failures += RunGenerator("aliased", GEN_ALIASED, 6000u);
    failures += RunRsAlignPass();
    failures += NegativeControl();
    failures += CheckResampleBound();
    failures += CheckPathCoverage();
    failures += CheckVerifyMode();
    if (failures != 0) {
        printf("hle_fast_tests: %d FAILURES\n", failures);
        return 1;
    }
    printf("hle_fast_tests: 24000 lists bit-exact (DMEM, state, saves, last-writer map; 9000 on the "
           "audio_rs_align=1 window), 0 failures\n");
    return 0;
}
