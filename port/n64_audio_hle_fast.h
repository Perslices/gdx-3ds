/* n64_audio_hle_fast — bit-exact fast paths for the HLE audio kernels in n64_audio_hle.c.
 * NEXT.md C4k, agent o3dsaudio. Lever: [debug] audio_simd (default 0).
 *
 * WHAT THIS IS. The reference interpreter (n64_audio_hle.c) reaches every DMEM byte through
 * DmemGetS16/DmemSetS16/DmemGetU8/DmemSetU8: a mask, a memcpy and a last-writer store per
 * sample, so a malformed command can never escape the 4 KB DMEM. That safety is per ACCESS.
 * These kernels move it to per OP: each one first proves the whole op's read and write ranges
 * sit inside DMEM without wrapping and at the alignment the reference would use, and only then
 * runs over plain pointers. Anything that fails the proof returns 0 and the caller runs the
 * reference kernel unchanged, so the fast path can only ever be taken where it computes the
 * same thing.
 *
 * WHY IT IS BIT-EXACT, by construction rather than by luck:
 *   - The arithmetic is the reference's arithmetic, expression for expression, in the same
 *     integer widths (the ADPCM predictor stays int64; the envmixer/mixer products stay int32).
 *   - Every read and write happens in the reference's ORDER, so aliasing (in-place resample,
 *     an envmixer bus that doubles as its own source, overlapping DMEMMOVE) gives the same
 *     bytes. Where an op cannot keep that order cheaply (INTERLEAVE into its own input) the
 *     proof refuses it.
 *   - The ARMv6 instructions used are exact equivalents, not approximations: SSAT #16 is
 *     ClampS16, QADD16 is two independent ClampS16(a + b) on 16-bit lanes, SMLAWB/T is the
 *     48-bit (a * b16) >> 16, and SMUAD/SMLAD sum 16x16 products in 32 bits -- each used only
 *     where a per-op (or per-frame, per-block) bound proves no partial sum can leave the
 *     range the reference's own arithmetic stays in. Off ARM the same algorithm runs on C
 *     emulations of those instructions, so the host test exercises the packed code paths too.
 *   - The last-writer map ([spike] attribution) is marked for exactly the 16-byte blocks the
 *     reference would have marked.
 * port/tests/hle_fast_tests.c runs the reference and this file over the same randomized and
 * adversarial command lists and compares all of DMEM, every state buffer, every saved byte
 * and the last-writer map.
 *
 * Every function returns 1 when it handled the op, 0 when the caller must run the reference.
 * `dmem` is the interpreter's 4 KB DMEM (4-byte aligned); `lastOp` its 256-entry map.
 */
#ifndef N64_AUDIO_HLE_FAST_H
#define N64_AUDIO_HLE_FAST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_HLEFAST_DMEM_SIZE 0x1000u
/* Coefficients the ADPCM predictor can index: 16 predictors x 2 taps x 8 columns. */
#define GDX_HLEFAST_BOOK_COEFS 256u

typedef struct GdxHleFastCtx {
    uint8_t* dmem;
    uint8_t* lastOp;
    uint8_t op; /* the opcode to record in lastOp for every block written */
} GdxHleFastCtx;

/* The codebook as the fast ADPCM kernel wants it, rebuilt at every A_LOADADPCM.
 *   coef      the reference's BookCoef() as a table: coef[i] = book s16 i when its two bytes
 *             lie inside the loaded length, else 0 (BookCoef's own bounds rule).
 *   rows      per predictor, the 8x10 block-convolution matrix over (hist2, hist1, e0..e7) as
 *             packed s16 pairs, so one output is 2-5 SMLADs. Built lazily, the first time a
 *             frame names that predictor (builtMask).
 *   maxShift  per predictor, the largest frame scale for which every partial sum of every row
 *             provably fits in int32 (and every residual in s16): the packed path's proof. A
 *             frame above it decodes on the int64 path, exactly as the reference does. */
typedef struct GdxHleFastBook {
    int16_t coef[GDX_HLEFAST_BOOK_COEFS];
    uint32_t builtMask;
    int8_t maxShift[16];
    uint32_t rows[16][8][5];
} GdxHleFastBook;

void gdx_hlefast_build_book(GdxHleFastBook* out, const uint8_t* book, uint32_t bookLen);

/* Which inner path each kernel took, cumulative: the parity test asserts every packed path AND
 * every in-kernel fallback ran, so a green run cannot mean "the proof always refused". */
typedef enum GdxHleFastPath {
    GDX_HLEFAST_PATH_ADPCM_PACKED = 0, /* frames on the SMLAD matrix */
    GDX_HLEFAST_PATH_ADPCM_WIDE,       /* frames whose scale failed the int32 bound: int64 */
    GDX_HLEFAST_PATH_RESAMPLE_PACKED,  /* ops on the pair window */
    GDX_HLEFAST_PATH_RESAMPLE_SCALAR,  /* ops whose output overlaps the input window */
    GDX_HLEFAST_PATH_ENVMIX_PACKED,    /* 8-sample blocks on SMLAWB + QADD16 */
    GDX_HLEFAST_PATH_ENVMIX_SCALAR,    /* blocks with a volume outside +-0xFFFF */
    GDX_HLEFAST_PATH_MIXER_PACKED,
    GDX_HLEFAST_PATH_MIXER_SCALAR,
    GDX_HLEFAST_PATH_COUNT
} GdxHleFastPath;
void gdx_hlefast_path_counts(uint32_t out[GDX_HLEFAST_PATH_COUNT]);

/* NEGATIVE CONTROL, tests only: 1 flips bit 12 of the first freshly decoded sample of
 * every fast ADPCM op, so the parity gate and the verify mode can be shown to FIRE. */
void gdx_hlefast_inject_fault(int on);

int gdx_hlefast_clear(const GdxHleFastCtx* c, uint32_t dmem, uint32_t size);
int gdx_hlefast_load(const GdxHleFastCtx* c, uint32_t dmemDest, const uint8_t* src, uint32_t size);
/* *nonzero = 1 when any saved byte was nonzero (the [audio-hle] nzSaves counter). */
int gdx_hlefast_save(const GdxHleFastCtx* c, uint32_t dmemSrc, uint8_t* dst, uint32_t size,
                     int* nonzero);
int gdx_hlefast_move(const GdxHleFastCtx* c, uint32_t dmemIn, uint32_t dmemOut, uint32_t count);

/* A_ADPCM, 4-bit block-convolution form only (the caller keeps the small-ADPCM and
 * sequential-debug paths on the reference).
 * scaleClampOn: [debug] adpcm_scale_clamp (n64_audio_hle.c's RunAdpcm). This kernel's packed/wide
 * decode both use the header's raw scale nibble unclamped, same "prove or decline" contract as
 * envmix_xbits's xorBits below -- when the lever is on, this kernel pre-scans every frame header
 * in the call and declines (returns 0, before writing anything) if ANY frame's scale >= 12, so
 * the reference's clamp runs instead. Scale < 12 is identical either way (mupen's rshift clamp
 * is a no-op there too), so a lever-on call with no such frame still takes the fast path. */
int gdx_hlefast_adpcm(const GdxHleFastCtx* c, uint32_t dmemIn, uint32_t dmemOut, uint32_t count,
                      uint32_t flags, int16_t* state, const int16_t* loopState,
                      GdxHleFastBook* book, int scaleClampOn);
/* A_RESAMPLE; pitch is read from state[15] exactly as RunResample does. `tapBack` places the
 * 4-tap window at virtual cursor - tapBack: 1 = the historical window x[c-1..c+2], 4 = the
 * RSP's x[c-4..c-1] ([debug] audio_rs_align=1, n64_audio_hle.c). Anything else declines. */
int gdx_hlefast_resample(const GdxHleFastCtx* c, uint32_t dmemIn, uint32_t dmemOut, uint32_t count,
                         uint32_t flags, int16_t* state, const int16_t table[64][4],
                         uint32_t tapBack);
/* xorBits: w0's x0..x3 nibble when [debug] envmix_xbits is on and any of the four bits is set,
 * else 0. The packed kernels below never carry the mupen64plus-rsp-hle xor-mask semantics (see
 * n64_audio_hle.c's GDX_A_ENVMIXER case comment) -- nonzero always declines (returns 0) so the
 * caller runs the reference, same "prove or decline" contract as everything else here. */
int gdx_hlefast_envmix(const GdxHleFastCtx* c, uint32_t dmemSrc, uint32_t sampleCount,
                       const uint32_t dests[4], int32_t volL, int32_t volR, int32_t reverb,
                       int32_t rampL, int32_t rampR, int32_t rampReverb, uint32_t xorBits);
int gdx_hlefast_mixer(const GdxHleFastCtx* c, uint32_t dmemIn, uint32_t dmemOut,
                      uint32_t numSamples, int32_t gain);
int gdx_hlefast_addmix(const GdxHleFastCtx* c, uint32_t dmemIn, uint32_t dmemOut,
                       uint32_t numSamples);
int gdx_hlefast_interleave(const GdxHleFastCtx* c, uint32_t dmemOut, uint32_t dmemL,
                           uint32_t dmemR, uint32_t numSamples);
int gdx_hlefast_hilogain(const GdxHleFastCtx* c, uint32_t dmem, uint32_t numSamples, int32_t gain);
int gdx_hlefast_filter(const GdxHleFastCtx* c, uint32_t dmemBuf, uint32_t sizeBytes,
                       uint32_t flags, int16_t* state, const int16_t coef[8]);

#ifdef __cplusplus
}
#endif

#endif /* N64_AUDIO_HLE_FAST_H */
