/* gdx_ndsp_hash — implementation.  See gdx_ndsp_hash.h for what this hashes and why that
 * particular thing is the stable one.
 *
 * Plain C99, no decomp headers, no platform headers: the host suite
 * (port/tests/ndsphash_tests.c, CI as `gdx3ds_ndsphash_host_tests`) compiles this file
 * UNMODIFIED and drives it through the public API, which is the same discipline
 * port/tests/pcm_capture_tests.c uses for gdx_audio_capture.c.
 */

#include "gdx_ndsp_hash.h"

#include <stdlib.h>
#include <string.h>

/* Every receipt goes through the port's own log sink (port/n64_sched.c defines it for both
 * the 3DS and the desktop build).  Declared plainly rather than weak on purpose: the
 * `__attribute__((weak))` idiom the 3DS-only TUs use is a Mach-O link error for an UNDEFINED
 * symbol in a static link, and this file is built on the host too, as part of two standalone
 * test binaries.  Each of those supplies its own gdx_dbg_logf, which is the right shape
 * anyway -- a test that wants to assert on what the instrument SAID needs to own the sink. */
extern void gdx_dbg_logf(const char* fmt, ...);

/* The INI accessor exists only in the 3DS build.  Referencing it at all off-3DS would make
 * the host suite's link depend on a symbol it has no business providing, so the whole
 * INI-reading path is compiled out instead: off-3DS, gdx_ndsphash_configure() is the only way
 * in, which is exactly what port/tests/ndsphash_tests.c uses. */
#ifdef GDX_PLATFORM_3DS
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback);
#endif

/* ------------------------------------------------------------------------- hashing -- */

/* FNV-1a 64.  Chosen over a CRC because it is four lines, has no table to get wrong, and is
 * trivially re-implementable in the Python comparator -- tools/dev/ndspdiff.py does NOT need
 * to re-hash anything, but the host suite's golden vectors are checkable by hand.
 * 64 bits for the key: at ~16,000 tasks a session the 32-bit birthday probability of a false
 * `selfdiv` is ~3%, which would be an instrument that cries wolf once every few runs.  At 64
 * bits it is ~1e-11. */
#define GDX_FNV64_OFFSET 1469598103934665603ULL
#define GDX_FNV64_PRIME  1099511628211ULL

static uint64_t GdxFnv64(uint64_t h, const void* data, unsigned int len) {
    const unsigned char* p = (const unsigned char*)data;
    unsigned int i;
    for (i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= GDX_FNV64_PRIME;
    }
    return h;
}

static uint64_t GdxFnv64U32(uint64_t h, uint32_t v) {
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xFFu);
    b[1] = (unsigned char)((v >> 8) & 0xFFu);
    b[2] = (unsigned char)((v >> 16) & 0xFFu);
    b[3] = (unsigned char)((v >> 24) & 0xFFu);
    return GdxFnv64(h, b, 4u);
}

/* --------------------------------------------------------------------------- state -- */

/* Within-run contradiction table: key -> value, open-addressed, linear probe, never resized.
   Sized so a canonical-replay session (~16,000 audio tasks, audioring's `ticks=15596`) keeps
   the load factor under 0.5, where linear probing is still cheap.  Allocated ONLY when the
   lever is armed, so the shipped build carries the pointer and nothing else. */
#define GDX_NDSPHASH_SLOTS 32768u

typedef struct {
    uint64_t key;
    uint32_t val;
    uint32_t used;
} GdxNdspSlot;

static struct {
    int mode;           /* 0 off, 1 counters, 2 counters + streamed pairs */
    int every;          /* record one task in `every` */
    int faultTask;      /* 1-based task index for the render fault, 0 = none */
    int faultSource;    /* 1-based task index for the source fault, 0 = none */
    int aiPin;          /* [debug] ndsphash_pin: constant AI buffer length (see the header) */
    int lockstep;       /* [debug] ndsphash_lockstep: one audio tick per game frame, inline */
    int initialised;
    int reported;

    GdxNdspSlot* slots;
    unsigned int slotsUsed;
    unsigned int slotsFull;   /* insertions refused because the table filled */

    unsigned int tasks;       /* audio tasks seen (before sampling) */
    unsigned int pairs;       /* (key,value) pairs sealed */
    unsigned int selfDiv;     /* same key, different value, within THIS run */
    unsigned int loadFolds;   /* address-bearing loads whose CONTENT entered a key */
    unsigned int saveFolds;   /* saves whose CONTENT entered a value */
    unsigned int emptyVal;    /* tasks that wrote nothing back (silence-shaped lists) */

    uint64_t sessionFold;     /* order-dependent fold of every pair: a one-number receipt */
    uint64_t pairXor;         /* order-INDEPENDENT fold: survives a reordered tick stream */

    /* live task */
    int recording;
    uint64_t key;
    uint32_t val;
    int sawSave;
    int faultRenderSpent;
    int faultSourceSpent;
} sNh;

/* ------------------------------------------------------------------- configuration -- */

void gdx_ndsphash_configure(int mode, int every, int faultTask, int faultSource) {
    if (mode < 0) {
        mode = 0;
    }
    if (every < 1) {
        every = 1;
    }
    sNh.mode = mode;
    sNh.every = every;
    sNh.faultTask = (faultTask > 0) ? faultTask : 0;
    sNh.faultSource = (faultSource > 0) ? faultSource : 0;
    sNh.initialised = 1;

    if (sNh.mode > 0 && sNh.slots == NULL) {
        sNh.slots = (GdxNdspSlot*)calloc((size_t)GDX_NDSPHASH_SLOTS, sizeof(GdxNdspSlot));
        /* A failed allocation disarms the within-run check but keeps the pair stream, which
           is the half the cross-run gate actually needs.  Reported, never silent. */
    }
}

void gdx_ndsphash_init(void) {
#ifndef GDX_PLATFORM_3DS
    if (!sNh.initialised) {
        gdx_ndsphash_configure(0, 1, 0, 0);
    }
#else
    int mode, every, fr, fs;

    if (sNh.initialised) {
        return;
    }
    mode = gdx3ds_config_get_int("debug", "ndsphash", 0);
    every = gdx3ds_config_get_int("debug", "ndsphash_every", 1);
    fr = gdx3ds_config_get_int("debug", "ndsphash_fault", 0);
    fs = gdx3ds_config_get_int("debug", "ndsphash_fault_source", 0);
    gdx_ndsphash_configure(mode, every, fr, fs);
    /* The AI pin is deliberately INDEPENDENT of `mode`: it changes guest behaviour, so it
       must be nameable on its own line in a run manifest and must never come along for
       the ride with a diagnostic. */
    gdx_ndsphash_set_ai_pin(gdx3ds_config_get_int("debug", "ndsphash_pin", 0));
    gdx_ndsphash_set_lockstep(gdx3ds_config_get_int("debug", "ndsphash_lockstep", 0));

    if (sNh.mode > 0 || sNh.aiPin || sNh.lockstep) {
        gdx_dbg_logf("[ndsphash] init mode=%d every=%d slots=%u table=%s fault=%d faultsrc=%d "
                     "aipin=%d lockstep=%d\n",
                     sNh.mode, sNh.every, (unsigned)GDX_NDSPHASH_SLOTS,
                     (sNh.slots != NULL) ? "on" : "ALLOC-FAILED",
                     sNh.faultTask, sNh.faultSource, sNh.aiPin, sNh.lockstep);
    }
#endif
}

int gdx_ndsphash_enabled(void) {
    return sNh.mode > 0;
}

int gdx_ndsphash_ai_pin(void) {
    return sNh.aiPin;
}

void gdx_ndsphash_set_ai_pin(int on) {
    sNh.aiPin = (on != 0);
}

int gdx_ndsphash_lockstep(void) {
    return sNh.lockstep;
}

void gdx_ndsphash_set_lockstep(int on) {
    sNh.lockstep = (on != 0);
}

/* -------------------------------------------------------------------- the task tap -- */

/* Opcodes whose w1 is an RDRAM address.  Their w1 must NEVER enter the key: it is a
   sample-cache slot, an audio-heap note-state address or a soundfont book pointer, and all of
   those move between runs and move when `[debug] audio_stream` swaps the byte source --
   `audiodiet` took a 10.25 MB blob off the heap, which shifts every allocation after it.  The
   CONTENT arrives via gdx_ndsphash_fold_load() instead, which is the whole content-addressing
   idea: fold what the bytes ARE, never where they live.
   Opcode numbers mirror port/n64_audio_hle.c's enum, which mirrors decomp/include/PR/abi.h
   under EXPANSION_KIT=1 (the reason A_HILOGAIN is 14 there and not 24). */
static int GdxNdspOpHasAddress(uint32_t op) {
    switch (op) {
        case 1u:  /* A_ADPCM        w1 = NoteSynthesisBuffers.adpcmdecState */
        case 5u:  /* A_RESAMPLE     w1 = finalResampleState */
        case 6u:  /* A_RESAMPLE_ZOH w1 = finalResampleState */
        case 7u:  /* A_FILTER       w1 = coefficient table (prime) or filter state (apply) */
        case 11u: /* A_LOADADPCM    w1 = soundfont codebook */
        case 15u: /* A_SETLOOP      w1 = AdpcmLoop.predictorState */
        case 20u: /* A_LOADBUFF     w1 = sample-cache slot holding VADPCM bytes */
        case 21u: /* A_SAVEBUFF     w1 = AI buffer or reverb ring -- the OUTPUT side */
        case 23u: /* A_S8DEC        w1 = decoder state */
            return 1;
        default:
            /* Everything else is DMEM offsets, counts, pitches and envelope volumes, all of
               which are stable across runs and are exactly what the key must be sensitive
               to.  They go in verbatim. */
            return 0;
    }
}

int gdx_ndsphash_task_begin(const void* cmds, unsigned int byteCount) {
    const unsigned char* p = (const unsigned char*)cmds;
    unsigned int count;
    unsigned int i;

    sNh.recording = 0;
    sNh.sawSave = 0;
    if (sNh.mode <= 0) {
        return 0;
    }

    sNh.tasks++;
    if (cmds == NULL || byteCount < 8u) {
        return 0;
    }
    if (sNh.every > 1 && (sNh.tasks % (unsigned int)sNh.every) != 0u) {
        return 0;
    }

    sNh.recording = 1;
    sNh.key = GDX_FNV64_OFFSET;
    sNh.val = 0x811C9DC5u; /* FNV-1a 32 offset basis, folded 32-bit for a compact pair line */

    /* The command list is a flat array of {u32 w0, u32 w1} in host order -- the same layout
       gdx_audio_hle_run reads.  Read it the same way rather than casting to a struct, so this
       file stays free of that TU's private types. */
    count = byteCount / 8u;
    for (i = 0; i < count; i++) {
        uint32_t w0;
        uint32_t w1;
        uint32_t op;
        memcpy(&w0, p + (size_t)i * 8u, 4u);
        memcpy(&w1, p + (size_t)i * 8u + 4u, 4u);
        op = (w0 >> 24) & 0xFFu;
        sNh.key = GdxFnv64U32(sNh.key, w0);
        if (!GdxNdspOpHasAddress(op)) {
            sNh.key = GdxFnv64U32(sNh.key, w1);
        } else {
            /* Fold a marker so an address-bearing command is not indistinguishable from a
               one-word command; the address itself stays out. */
            sNh.key = GdxFnv64U32(sNh.key, 0xADD8E551u);
        }
    }
    return 1;
}

void gdx_ndsphash_fold_load(const void* src, unsigned int byteCount) {
    if (!sNh.recording || src == NULL || byteCount == 0u) {
        return;
    }
    sNh.loadFolds++;
    sNh.key = GdxFnv64(sNh.key, src, byteCount);
}

void gdx_ndsphash_fold_save(const void* dst, unsigned int byteCount) {
    uint64_t h;
    if (!sNh.recording || dst == NULL || byteCount == 0u) {
        return;
    }
    sNh.saveFolds++;
    sNh.sawSave = 1;
    /* The value is a 32-bit fold of a 64-bit hash: one pair line stays short enough that a
       16,000-tick session's stream is ~700 KB of log rather than 1.1 MB, and 32 bits over the
       ~16,000 values a run produces is a 3e-5 chance of masking one changed tick.  The gate
       is "divergent=0", so a masked tick costs sensitivity, never a false alarm. */
    h = GdxFnv64((uint64_t)sNh.val | GDX_FNV64_OFFSET, dst, byteCount);
    sNh.val = (uint32_t)(h ^ (h >> 32));
}

void gdx_ndsphash_task_end(void) {
    unsigned int idx;
    unsigned int probes;

    if (!sNh.recording) {
        return;
    }
    sNh.recording = 0;
    sNh.pairs++;
    if (!sNh.sawSave) {
        sNh.emptyVal++;
    }

    sNh.sessionFold = GdxFnv64U32(GdxFnv64(sNh.sessionFold, &sNh.key, 8u), sNh.val);
    sNh.pairXor ^= (sNh.key ^ ((uint64_t)sNh.val << 32) ^ (uint64_t)sNh.val);

    if (sNh.mode >= 2) {
        /* One pair per line.  A line is self-contained, so a truncated or interleaved log
           still yields every pair that made it out -- which matters, because the 3DS link
           drops lines under load and a batched format would lose eight pairs per drop. */
        gdx_dbg_logf("[ndsphash] e %u %08X%08X %08X\n",
                     sNh.pairs,
                     (unsigned)(uint32_t)(sNh.key >> 32), (unsigned)(uint32_t)sNh.key,
                     (unsigned)sNh.val);
    }

    if (sNh.slots == NULL) {
        return;
    }
    idx = (unsigned int)(sNh.key % (uint64_t)GDX_NDSPHASH_SLOTS);
    for (probes = 0; probes < GDX_NDSPHASH_SLOTS; probes++) {
        GdxNdspSlot* s = &sNh.slots[idx];
        if (!s->used) {
            if (sNh.slotsUsed * 2u >= GDX_NDSPHASH_SLOTS) {
                sNh.slotsFull++;   /* keep the load factor under 0.5; stop inserting */
                return;
            }
            s->used = 1u;
            s->key = sNh.key;
            s->val = sNh.val;
            sNh.slotsUsed++;
            return;
        }
        if (s->key == sNh.key) {
            if (s->val != sNh.val) {
                sNh.selfDiv++;
                if (sNh.selfDiv <= 8u) {
                    gdx_dbg_logf("[ndsphash] SELFDIV task=%u key=%08X%08X a=%08X b=%08X\n",
                                 sNh.pairs,
                                 (unsigned)(uint32_t)(sNh.key >> 32), (unsigned)(uint32_t)sNh.key,
                                 (unsigned)s->val, (unsigned)sNh.val);
                }
            }
            return;
        }
        idx++;
        if (idx >= GDX_NDSPHASH_SLOTS) {
            idx = 0u;
        }
    }
    sNh.slotsFull++;
}

/* ------------------------------------------------------------------ fault injection -- */

int gdx_ndsphash_fault_render(void) {
    if (sNh.mode <= 0 || sNh.faultTask <= 0 || sNh.faultRenderSpent) {
        return 0;
    }
    if (sNh.tasks != (unsigned int)sNh.faultTask) {
        return 0;
    }
    sNh.faultRenderSpent = 1;
    {
        gdx_dbg_logf("[ndsphash] FAULT render task=%u (one bit of one rendered sample)\n",
                     sNh.tasks);
    }
    return 1;
}

int gdx_ndsphash_fault_source(void) {
    if (sNh.mode <= 0 || sNh.faultSource <= 0 || sNh.faultSourceSpent) {
        return 0;
    }
    if (sNh.tasks != (unsigned int)sNh.faultSource) {
        return 0;
    }
    sNh.faultSourceSpent = 1;
    {
        gdx_dbg_logf("[ndsphash] FAULT source task=%u (one bit of one delivered sample byte)\n",
                     sNh.tasks);
    }
    return 1;
}

/* ----------------------------------------------------------------------- receipts -- */

void gdx_ndsphash_report_final(void) {
    if (sNh.mode <= 0 || sNh.reported) {
        return;
    }
    sNh.reported = 1;
    gdx_dbg_logf("[ndsphash] FINAL tasks=%u pairs=%u selfdiv=%u distinct=%u full=%u\n",
                 sNh.tasks, sNh.pairs, sNh.selfDiv, sNh.slotsUsed, sNh.slotsFull);
    gdx_dbg_logf("[ndsphash] FINAL loads=%u saves=%u emptyval=%u fold=%08X%08X xor=%08X%08X\n",
                 sNh.loadFolds, sNh.saveFolds, sNh.emptyVal,
                 (unsigned)(uint32_t)(sNh.sessionFold >> 32), (unsigned)(uint32_t)sNh.sessionFold,
                 (unsigned)(uint32_t)(sNh.pairXor >> 32), (unsigned)(uint32_t)sNh.pairXor);
}

void gdx_ndsphash_reset_for_test(void) {
    GdxNdspSlot* keep = sNh.slots;
    if (keep != NULL) {
        memset(keep, 0, (size_t)GDX_NDSPHASH_SLOTS * sizeof(GdxNdspSlot));
    }
    memset(&sNh, 0, sizeof(sNh));
    sNh.slots = keep;
    sNh.every = 1;
}

void gdx_ndsphash_stats_for_test(unsigned int* outTasks, unsigned int* outPairs,
                                 unsigned int* outSelfDiv, uint64_t* outFold) {
    if (outTasks != NULL) {
        *outTasks = sNh.tasks;
    }
    if (outPairs != NULL) {
        *outPairs = sNh.pairs;
    }
    if (outSelfDiv != NULL) {
        *outSelfDiv = sNh.selfDiv;
    }
    if (outFold != NULL) {
        *outFold = sNh.sessionFold;
    }
}
