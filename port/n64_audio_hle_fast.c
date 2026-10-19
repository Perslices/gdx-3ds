/* n64_audio_hle_fast — see the header for the contract and the bit-exactness argument.
 * Each kernel below names the reference function or dispatch case it mirrors; keep the two in
 * step, and re-run port/tests/hle_fast_tests.c after touching either. */
#include "n64_audio_hle_fast.h"

#include <string.h>

#if defined(__ARM_FEATURE_SAT) || defined(__ARM_FEATURE_SIMD32)
#include <arm_acle.h>
#endif

/* DMEM is a byte array read as s16/u32 lanes; may_alias makes that well-defined without leaning
 * on -fno-strict-aliasing (which every build of this file also passes). */
#if defined(__GNUC__)
typedef int16_t HleS16 __attribute__((may_alias));
typedef uint32_t HleU32 __attribute__((may_alias));
#else
typedef int16_t HleS16;
typedef uint32_t HleU32;
#endif

/* SSAT #16 on the ARM11: exactly the reference's ClampS16. */
static inline int16_t Sat16(int32_t v) {
#if defined(__ARM_FEATURE_SAT)
    return (int16_t)__ssat(v, 16);
#else
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (int16_t)v;
#endif
}

/* ---- ARMv6 SIMD/DSP instructions, and their exact C emulations off ARM ----------------------
 * Packed words are little-endian s16 pairs: bits 0..15 = the lower-addressed sample. */
static inline uint32_t Pack16(int32_t lo, int32_t hi) {
    return ((uint32_t)lo & 0xFFFFu) | ((uint32_t)hi << 16); /* PKHBT */
}

static inline int32_t Lo16(uint32_t w) {
    return (int16_t)(uint16_t)(w & 0xFFFFu);
}

static inline int32_t Hi16(uint32_t w) {
    return (int16_t)(uint16_t)(w >> 16);
}

#if defined(__ARM_FEATURE_SIMD32) && defined(__ARM_FEATURE_DSP)
/* SMUAD / SMLAD: lo*lo + hi*hi (+ acc), 32-bit wrapping sum. */
static inline int32_t Smuad(uint32_t a, uint32_t b) {
    return __smuad((int16x2_t)a, (int16x2_t)b);
}
static inline int32_t Smlad(uint32_t a, uint32_t b, int32_t acc) {
    return __smlad((int16x2_t)a, (int16x2_t)b, acc);
}
/* QADD16: two independent saturating s16 adds. */
static inline uint32_t Qadd16(uint32_t a, uint32_t b) {
    return (uint32_t)__qadd16((int16x2_t)a, (int16x2_t)b);
}
/* SMLAWB/T with a zero accumulator: (a * s16 lane) >> 16 on the full 48-bit product. */
static inline int32_t MulwLo(int32_t a, uint32_t b) {
    return __smlawb(a, (int32_t)b, 0);
}
static inline int32_t MulwHi(int32_t a, uint32_t b) {
    return __smlawt(a, (int32_t)b, 0);
}
#else
static inline int32_t Smlad(uint32_t a, uint32_t b, int32_t acc) {
    const uint32_t lo = (uint32_t)(Lo16(a) * Lo16(b));
    const uint32_t hi = (uint32_t)(Hi16(a) * Hi16(b));
    return (int32_t)((uint32_t)acc + lo + hi);
}
static inline int32_t Smuad(uint32_t a, uint32_t b) {
    return Smlad(a, b, 0);
}
static inline uint32_t Qadd16(uint32_t a, uint32_t b) {
    return Pack16(Sat16(Lo16(a) + Lo16(b)), Sat16(Hi16(a) + Hi16(b)));
}
static inline int32_t MulwLo(int32_t a, uint32_t b) {
    return (int32_t)(((int64_t)a * Lo16(b)) >> 16);
}
static inline int32_t MulwHi(int32_t a, uint32_t b) {
    return (int32_t)(((int64_t)a * Hi16(b)) >> 16);
}
#endif

/* The whole op's byte range lies inside DMEM with no wrap. The reference masks EVERY access, so
 * a range that wraps is legal there and different here: those ops go to the reference. */
static inline int RangeOk(uint32_t off, uint32_t bytes) {
    return off < GDX_HLEFAST_DMEM_SIZE && bytes <= GDX_HLEFAST_DMEM_SIZE - off;
}

/* The reference masks s16 offsets with ~1; an odd base would silently realign there. */
static inline int EvenRangeOk(uint32_t off, uint32_t bytes) {
    return (off & 1u) == 0u && RangeOk(off, bytes);
}

static inline int Overlaps(uint32_t a, uint32_t aBytes, uint32_t b, uint32_t bBytes) {
    return aBytes != 0u && bBytes != 0u && a < b + bBytes && b < a + aBytes;
}

/* The last-writer map: DmemSetU8/DmemSetS16 record the op for every 16-byte block they touch. */
static void MarkWritten(const GdxHleFastCtx* c, uint32_t off, uint32_t bytes) {
    uint32_t first;
    uint32_t last;
    if (bytes == 0u) {
        return;
    }
    first = off >> 4;
    last = (off + bytes - 1u) >> 4;
    memset(c->lastOp + first, c->op, last - first + 1u);
}

static uint32_t sPathCounts[GDX_HLEFAST_PATH_COUNT];
static int sInjectFault = 0;

void gdx_hlefast_inject_fault(int on) {
    sInjectFault = on;
}

void gdx_hlefast_path_counts(uint32_t out[GDX_HLEFAST_PATH_COUNT]) {
    memcpy(out, sPathCounts, sizeof(sPathCounts));
}

static inline HleS16* DmemS16(const GdxHleFastCtx* c, uint32_t off) {
    return (HleS16*)(void*)(c->dmem + off);
}

/* Coefficient i is valid while its two bytes fit: 2i + 1 < bookLen, i.e. i < bookLen / 2. */
void gdx_hlefast_build_book(GdxHleFastBook* out, const uint8_t* book, uint32_t bookLen) {
    uint32_t valid = bookLen / 2u;
    if (valid > GDX_HLEFAST_BOOK_COEFS) {
        valid = GDX_HLEFAST_BOOK_COEFS;
    }
    memcpy(out->coef, book, valid * sizeof(int16_t));
    memset(out->coef + valid, 0, (GDX_HLEFAST_BOOK_COEFS - valid) * sizeof(int16_t));
    out->builtMask = 0; /* predictor matrices are rebuilt on first use */
}

/* ---- DMA / copy ops: GDX_A_CLEARBUFF, A_LOADBUFF, A_SAVEBUFF, A_DMEMMOVE ------------------ */

int gdx_hlefast_clear(const GdxHleFastCtx* c, uint32_t dmem, uint32_t size) {
    if (!RangeOk(dmem, size)) {
        return 0;
    }
    memset(c->dmem + dmem, 0, size);
    MarkWritten(c, dmem, size);
    return 1;
}

int gdx_hlefast_load(const GdxHleFastCtx* c, uint32_t dmemDest, const uint8_t* src, uint32_t size) {
    if (!RangeOk(dmemDest, size)) {
        return 0;
    }
    memcpy(c->dmem + dmemDest, src, size);
    MarkWritten(c, dmemDest, size);
    return 1;
}

static int AnyNonzero(const uint8_t* p, uint32_t size) {
    uint8_t acc = 0;
    uint32_t k;
    for (k = 0; k < size; k++) {
        acc |= p[k];
    }
    return acc != 0u;
}

int gdx_hlefast_save(const GdxHleFastCtx* c, uint32_t dmemSrc, uint8_t* dst, uint32_t size,
                     int* nonzero) {
    if (!RangeOk(dmemSrc, size)) {
        return 0;
    }
    memcpy(dst, c->dmem + dmemSrc, size);
    *nonzero = AnyNonzero(dst, size);
    return 1;
}

/* The reference copies forward when out <= in and backward otherwise: memmove's contract. */
int gdx_hlefast_move(const GdxHleFastCtx* c, uint32_t dmemIn, uint32_t dmemOut, uint32_t count) {
    if (!RangeOk(dmemIn, count) || !RangeOk(dmemOut, count)) {
        return 0;
    }
    memmove(c->dmem + dmemOut, c->dmem + dmemIn, count);
    MarkWritten(c, dmemOut, count);
    return 1;
}

/* ---- A_ADPCM: RunAdpcm's 4-bit block-convolution path ------------------------------------- */

static void AdpcmSeedPreamble(HleS16* out, int16_t last[16], uint32_t flags, const int16_t* state,
                              const int16_t* loopState) {
    const int isInit = (flags & 1u) != 0u;
    const int isLoop = (flags & 2u) != 0u;
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        if (isInit) {
            last[i] = 0;
        } else if (isLoop && loopState != NULL) {
            last[i] = loopState[i];
        } else {
            last[i] = state[i];
        }
    }
    for (i = 0; i < 16u; i++) {
        out[i] = last[i];
    }
}

/* Four data bytes -> eight residuals, high nibble first, sign-extended, scaled by `shift`. */
static inline void AdpcmUnpackSubBlock(const uint8_t* bytes, uint32_t shift, int32_t e[8]) {
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        int32_t hi = (bytes[i] >> 4) & 0xF;
        int32_t lo = bytes[i] & 0xF;
        if (hi & 0x8) {
            hi -= 16;
        }
        if (lo & 0x8) {
            lo -= 16;
        }
        e[2u * i] = hi << shift;
        e[2u * i + 1u] = lo << shift;
    }
}

/* One 8-sample sub-block from its entry history, truncating once per output (see RunAdpcm). The
 * accumulator is int64 exactly as in the reference: with shift up to 15 a residual reaches 2^18
 * and one product 2^33. */
static inline void AdpcmDecodeSubBlock(HleS16* out, const int16_t* c0, const int16_t* c1,
                                       const int32_t e[8], int32_t* eH2, int32_t* eH1) {
    int16_t sblk[8];
    uint32_t i;
    uint32_t k;
    for (i = 0; i < 8u; i++) {
        int64_t acc = (int64_t)c0[i] * (int64_t)*eH2 + (int64_t)c1[i] * (int64_t)*eH1;
        for (k = 0; k < i; k++) {
            acc += (int64_t)c1[i - 1u - k] * (int64_t)e[k];
        }
        acc += (int64_t)e[i] << 11;
        sblk[i] = Sat16((int32_t)(acc >> 11));
        out[i] = sblk[i];
    }
    *eH2 = sblk[6];
    *eH1 = sblk[7];
}

static int32_t AbsI32(int32_t v) {
    return (v < 0) ? -v : v;
}

/* Row i of the block-convolution matrix over v = (hist2, hist1, e0..e7):
 *   out_i = c0[i]*hist2 + c1[i]*hist1 + sum_{k<i} c1[i-1-k]*e_k + 2048*e_i,  then >> 11
 * which is AdpcmDecodeSubBlock's accumulator term for term, e_i << 11 written as 2048 * e_i. */
static void BuildPredictorRows(GdxHleFastBook* b, uint32_t pred) {
    const int16_t* c0 = &b->coef[pred * 16u];
    const int16_t* c1 = c0 + 8;
    int64_t histWeight = 0; /* max over rows of |c0_i| + |c1_i| */
    int64_t residWeight = 2048; /* max over rows of sum |c1| on residuals + 2048 */
    uint32_t i;
    uint32_t j;
    int shift;
    for (i = 0; i < 8u; i++) {
        int32_t m[10] = { 0 };
        int64_t rowResid = 2048;
        m[0] = c0[i];
        m[1] = c1[i];
        for (j = 0; j < i; j++) {
            m[2u + j] = c1[i - 1u - j];
            rowResid += AbsI32(c1[i - 1u - j]);
        }
        m[2u + i] = 2048;
        for (j = 0; j < 5u; j++) {
            b->rows[pred][i][j] = Pack16(m[2u * j], m[2u * j + 1u]);
        }
        if (AbsI32(c0[i]) + AbsI32(c1[i]) > histWeight) {
            histWeight = AbsI32(c0[i]) + AbsI32(c1[i]);
        }
        if (rowResid > residWeight) {
            residWeight = rowResid;
        }
    }
    /* |hist| <= 32768 (s16), |e| <= 8 << shift. Every partial sum of a row is bounded by the
     * sum of its terms' magnitudes; below 2^31 no SMLAD can wrap and the int32 sum IS the
     * reference's int64 sum. shift <= 12 also keeps every residual inside s16 for packing. */
    b->maxShift[pred] = -1;
    for (shift = 0; shift <= 12; shift++) {
        const int64_t bound = histWeight * 32768 + residWeight * ((int64_t)8 << shift);
        if (bound < ((int64_t)1 << 31)) {
            b->maxShift[pred] = (int8_t)shift;
        }
    }
    b->builtMask |= 1u << pred;
}

/* Four data bytes -> four packed residual pairs (e0|e1, e2|e3, ...), high nibble first. */
static inline void AdpcmUnpackPacked(const uint8_t* bytes, uint32_t shift, uint32_t w[4]) {
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        const int32_t hi = ((int32_t)(int8_t)bytes[i]) >> 4;
        const int32_t lo = ((int32_t)((uint32_t)bytes[i] << 28)) >> 28;
        w[i] = Pack16((int32_t)((uint32_t)hi << shift), (int32_t)((uint32_t)lo << shift));
    }
}

/* Output i of a packed sub-block: the first `pairs` = (i + 4) / 2 words of its row, since row
 * i is zero past e_i. */
static inline int16_t AdpcmRow2(const uint32_t r[5], uint32_t v0, uint32_t v1) {
    return Sat16(Smlad(v1, r[1], Smuad(v0, r[0])) >> 11);
}
static inline int16_t AdpcmRow3(const uint32_t r[5], uint32_t v0, uint32_t v1, uint32_t v2) {
    return Sat16(Smlad(v2, r[2], Smlad(v1, r[1], Smuad(v0, r[0]))) >> 11);
}
static inline int16_t AdpcmRow4(const uint32_t r[5], uint32_t v0, uint32_t v1, uint32_t v2,
                                uint32_t v3) {
    return Sat16(Smlad(v3, r[3], Smlad(v2, r[2], Smlad(v1, r[1], Smuad(v0, r[0])))) >> 11);
}
static inline int16_t AdpcmRow5(const uint32_t r[5], uint32_t v0, uint32_t v1, uint32_t v2,
                                uint32_t v3, uint32_t v4) {
    return Sat16(Smlad(v4, r[4], Smlad(v3, r[3], Smlad(v2, r[2], Smlad(v1, r[1], Smuad(v0, r[0]))))) >> 11);
}

/* One sub-block on the packed path, fully unrolled so the five packed input words stay in
 * registers. */
static inline void AdpcmDecodeSubBlockPacked(HleS16* out, const uint32_t rows[8][5],
                                             const uint8_t* bytes, uint32_t shift, int32_t* eH2,
                                             int32_t* eH1) {
    uint32_t e[4];
    AdpcmUnpackPacked(bytes, shift, e);
    {
        const uint32_t v0 = Pack16(*eH2, *eH1);
        const uint32_t v1 = e[0];
        const uint32_t v2 = e[1];
        const uint32_t v3 = e[2];
        const uint32_t v4 = e[3];
        const int16_t s6 = AdpcmRow5(rows[6], v0, v1, v2, v3, v4);
        const int16_t s7 = AdpcmRow5(rows[7], v0, v1, v2, v3, v4);
        out[0] = AdpcmRow2(rows[0], v0, v1);
        out[1] = AdpcmRow2(rows[1], v0, v1);
        out[2] = AdpcmRow3(rows[2], v0, v1, v2);
        out[3] = AdpcmRow3(rows[3], v0, v1, v2);
        out[4] = AdpcmRow4(rows[4], v0, v1, v2, v3);
        out[5] = AdpcmRow4(rows[5], v0, v1, v2, v3);
        out[6] = s6;
        out[7] = s7;
        *eH2 = s6;
        *eH1 = s7;
    }
}

int gdx_hlefast_adpcm(const GdxHleFastCtx* c, uint32_t dmemIn, uint32_t dmemOut, uint32_t count,
                      uint32_t flags, int16_t* state, const int16_t* loopState,
                      GdxHleFastBook* book, int scaleClampOn) {
    const uint32_t numOut = count / 2u;
    const uint32_t numFrames = (numOut + 15u) / 16u;
    const uint32_t outBytes = 32u + numFrames * 32u;
    int16_t last[16];
    HleS16* out;
    const uint8_t* in;
    int32_t eH2;
    int32_t eH1;
    uint32_t f;
    uint32_t i;

    if (state == NULL || (flags & 4u) != 0u) {
        return 0;
    }
    if (!EvenRangeOk(dmemOut, outBytes) || !RangeOk(dmemIn, numFrames * 9u)) {
        return 0;
    }
    if (scaleClampOn) {
        /* See this function's own header comment: a pre-scan, before any write, so a decline
           here never leaves a partially-written output for the reference to redo on top of. */
        const uint8_t* scan = c->dmem + dmemIn;
        for (f = 0; f < numFrames; f++, scan += 9) {
            if (((scan[0] >> 4) & 0xFu) > 12u) {
                return 0;
            }
        }
    }
    out = DmemS16(c, dmemOut);
    in = c->dmem + dmemIn;
    AdpcmSeedPreamble(out, last, flags, state, loopState);
    out += 16;
    eH2 = last[14];
    eH1 = last[15];
    /* Read order matches the reference byte for byte (header, then each sub-block's four bytes
     * just before its eight writes), so an input window that overlaps the output decodes the
     * same bytes the reference would. */
    for (f = 0; f < numFrames; f++) {
        const uint8_t header = in[0];
        const uint32_t shift = (header >> 4) & 0xFu;
        const uint32_t pred = header & 0xFu;
        if ((book->builtMask & (1u << pred)) == 0u) {
            BuildPredictorRows(book, pred);
        }
        if ((int32_t)shift <= book->maxShift[pred]) {
            sPathCounts[GDX_HLEFAST_PATH_ADPCM_PACKED]++;
            AdpcmDecodeSubBlockPacked(out, book->rows[pred], in + 1, shift, &eH2, &eH1);
            AdpcmDecodeSubBlockPacked(out + 8, book->rows[pred], in + 5, shift, &eH2, &eH1);
        } else {
            const int16_t* c0 = &book->coef[pred * 16u];
            const int16_t* c1 = c0 + 8;
            int32_t e[8];
            sPathCounts[GDX_HLEFAST_PATH_ADPCM_WIDE]++;
            AdpcmUnpackSubBlock(in + 1, shift, e);
            AdpcmDecodeSubBlock(out, c0, c1, e, &eH2, &eH1);
            AdpcmUnpackSubBlock(in + 5, shift, e);
            AdpcmDecodeSubBlock(out + 8, c0, c1, e, &eH2, &eH1);
        }
        in += 9;
        out += 16;
    }
    if (sInjectFault) {
        DmemS16(c, dmemOut)[16] ^= 0x1000; /* loud enough to survive any downstream volume */
    }
    MarkWritten(c, dmemOut, outBytes);
    {
        const HleS16* tail = DmemS16(c, dmemOut) + numOut;
        for (i = 0; i < 16u; i++) {
            state[i] = tail[i];
        }
    }
    return 1;
}

/* ---- A_RESAMPLE: RunResample --------------------------------------------------------------- */

/* The reference's GdxResampleSampleAt: virtual 0..3 is the carried pre-roll, 4+ is DMEM. */
static inline int16_t ResampleTap(const int16_t hist[4], const HleS16* x, uint32_t virtIdx) {
    return (virtIdx < 4u) ? hist[virtIdx] : x[virtIdx - 4u];
}

static inline int16_t ResampleOne(int16_t xm1, int16_t x0, int16_t x1, int16_t x2,
                                  const int16_t* co) {
    const int32_t acc = ((int32_t)xm1 * co[0] + (int32_t)x0 * co[1] + (int32_t)x1 * co[2] +
                         (int32_t)x2 * co[3]) >> 15;
    return Sat16(acc);
}

/* The packed path's scratch: pairs[m] = (V[m], V[m+1]) over the virtual input V (4 pre-roll
 * samples, then DMEM), so taps (-1,0) and (+1,+2) of an output are two word loads. At most
 * 2048 DMEM samples + the pre-roll. */
#define RESAMPLE_PAIRS_MAX (GDX_HLEFAST_DMEM_SIZE / 2u + 8u)
static uint32_t sResamplePairs[RESAMPLE_PAIRS_MAX];

/* The ROM table's rows as packed coefficient pairs, (c0|c1, c2|c3). Built once. */
static uint32_t sResampleCoefPairs[64][2];
static const int16_t (*sResampleCoefSource)[4] = NULL;

static void PackResampleTable(const int16_t table[64][4]) {
    uint32_t p;
    if (sResampleCoefSource == table) {
        return;
    }
    for (p = 0; p < 64u; p++) {
        sResampleCoefPairs[p][0] = Pack16(table[p][0], table[p][1]);
        sResampleCoefPairs[p][1] = Pack16(table[p][2], table[p][3]);
    }
    sResampleCoefSource = table;
}

/* Packed resample, only when the output cannot touch the input window (then reading every
 * tap up front is the same as reading it just in time). The 4-term dot product is summed as
 * (x-1*c0 + x0*c1) + (x1*c2 + x2*c3) instead of left to right; no ROM row has sum |c| above
 * 2^16 (port/tests/hle_fast_tests.c asserts it), so no partial sum of s16 x c can reach 2^31
 * in either order and both orders give the same int32. */
static void ResamplePacked(const int16_t hist[4], const HleS16* x, HleS16* y, uint32_t numOut,
                           uint32_t lastPair, uint32_t* frac, uint32_t* virtIdx,
                           uint32_t pitchStep, uint32_t tapBack) {
    uint32_t m;
    uint32_t n;
    uint32_t f = *frac;
    uint32_t v = *virtIdx;
    for (m = 0; m <= lastPair; m++) {
        const int32_t a = (m < 4u) ? hist[m] : x[m - 4u];
        const int32_t b = (m + 1u < 4u) ? hist[m + 1u] : x[m - 3u];
        sResamplePairs[m] = Pack16(a, b);
    }
    for (n = 0; n < numOut; n++) {
        const uint32_t* co = sResampleCoefPairs[(f >> 10) & 63u];
        const uint32_t t0 = v - tapBack; /* virtual index of tap 0 */
        const int32_t acc = Smlad(sResamplePairs[t0 + 2u], co[1], Smuad(sResamplePairs[t0], co[0]));
        y[n] = Sat16(acc >> 15);
        f += pitchStep;
        v += f >> 16;
        f &= 0xFFFFu;
    }
    *frac = f;
    *virtIdx = v;
}

int gdx_hlefast_resample(const GdxHleFastCtx* c, uint32_t dmemIn, uint32_t dmemOut, uint32_t count,
                         uint32_t flags, int16_t* state, const int16_t table[64][4],
                         uint32_t tapBack) {
    const uint32_t numOut = ((count + 0xFu) & ~0xFu) / 2u;
    uint32_t pitchStep;
    uint32_t frac;
    uint32_t virtIdx = 4u;
    int16_t hist[4];
    const HleS16* x;
    HleS16* y;
    uint32_t n = 0;

    if (state == NULL || (tapBack != 1u && tapBack != 4u) || !EvenRangeOk(dmemIn, 0u) ||
        !EvenRangeOk(dmemOut, numOut * 2u)) {
        return 0;
    }
    pitchStep = ((uint32_t)(uint16_t)state[15]) << 1; /* Q15 -> Q16 */
    if ((flags & 1u) != 0u) {
        hist[0] = hist[1] = hist[2] = hist[3] = 0;
        frac = 0;
    } else {
        hist[0] = state[0];
        hist[1] = state[1];
        hist[2] = state[2];
        hist[3] = state[3];
        frac = (uint32_t)(uint16_t)state[4];
    }
    x = DmemS16(c, dmemIn);
    y = DmemS16(c, dmemOut);
    if (numOut > 0u) {
        /* The furthest DMEM sample read: tap 3 of the last output (DMEM index
         * advLast + 3 - tapBack), or the closing state's newest sample (advEnd - 1), which
         * sits past the last tap in the RSP window. Signed: with tapBack 4 and a slow pitch
         * nothing past the carried pre-roll may be read at all. */
        const uint32_t advLast = (frac + (numOut - 1u) * pitchStep) >> 16;
        const uint32_t advEnd = (frac + numOut * pitchStep) >> 16;
        int32_t furthest = (int32_t)advLast + 3 - (int32_t)tapBack;
        uint32_t readBytes;
        if ((int32_t)advEnd - 1 > furthest) {
            furthest = (int32_t)advEnd - 1;
        }
        readBytes = (furthest >= 0) ? ((uint32_t)furthest + 1u) * 2u : 0u;
        if (!RangeOk(dmemIn, readBytes)) {
            return 0;
        }
        /* pairs up to virtual tap0(last) + 2, i.e. (tap 2, tap 3) of the last output */
        {
            const uint32_t lastPair = 4u + advLast - tapBack + 2u;
            if (!Overlaps(dmemIn, readBytes, dmemOut, numOut * 2u) &&
                lastPair + 1u < RESAMPLE_PAIRS_MAX) {
                sPathCounts[GDX_HLEFAST_PATH_RESAMPLE_PACKED]++;
                PackResampleTable(table);
                ResamplePacked(hist, x, y, numOut, lastPair, &frac, &virtIdx, pitchStep, tapBack);
                n = numOut;
            }
        }
    }
    if (n < numOut) {
        sPathCounts[GDX_HLEFAST_PATH_RESAMPLE_SCALAR]++;
    }
    /* Taps are read before the output is written, sample by sample, as in the reference: an
     * output window that overlaps the input sees the same (partly rewritten) samples. While
     * tap 0 still indexes the carried pre-roll (virtual < 4) every tap goes through
     * ResampleTap; after that all four are DMEM. */
    for (; n < numOut && virtIdx < 4u + tapBack; n++) {
        const int16_t* co = table[(frac >> 10) & 63u];
        const uint32_t t0 = virtIdx - tapBack;
        const int16_t xm1 = ResampleTap(hist, x, t0);
        const int16_t x0 = ResampleTap(hist, x, t0 + 1u);
        const int16_t x1 = ResampleTap(hist, x, t0 + 2u);
        const int16_t x2 = ResampleTap(hist, x, t0 + 3u);
        y[n] = ResampleOne(xm1, x0, x1, x2, co);
        frac += pitchStep;
        virtIdx += frac >> 16; /* == the reference's while (frac >= 0x10000) loop: frac < 3<<16 */
        frac &= 0xFFFFu;
    }
    for (; n < numOut; n++) {
        const HleS16* p = x + (virtIdx - tapBack - 4u);
        const int16_t* co = table[(frac >> 10) & 63u];
        const int16_t xm1 = p[0];
        const int16_t x0 = p[1];
        const int16_t x1 = p[2];
        const int16_t x2 = p[3];
        y[n] = ResampleOne(xm1, x0, x1, x2, co);
        frac += pitchStep;
        virtIdx += frac >> 16;
        frac &= 0xFFFFu;
    }
    MarkWritten(c, dmemOut, numOut * 2u);
    state[0] = ResampleTap(hist, x, virtIdx - 4u);
    state[1] = ResampleTap(hist, x, virtIdx - 3u);
    state[2] = ResampleTap(hist, x, virtIdx - 2u);
    state[3] = ResampleTap(hist, x, virtIdx - 1u);
    state[4] = (int16_t)(uint16_t)frac;
    return 1;
}

/* ---- A_ENVMIXER: the dispatch case's per-sample loop --------------------------------------- */

/* |v| <= 0xFFFF keeps s * v inside int32 for any s16 s, so the reference's (s * v) >> 16 is the
 * 48-bit SMLAWB result, and the quotient lies in [-32768, 32767]: it packs into an s16 lane. */
static inline int VolumePackable(int32_t v) {
    return v >= -0xFFFF && v <= 0xFFFF;
}

/* One 8-sample envmixer block, two samples per step. Every buffer here is a multiple of 16
 * bytes (the ABI packs them >> 4), so two of them can only share a location lane-for-lane:
 * the pairwise order touches each location in the same sequence as the per-sample one. */
static void EnvmixBlockPacked(const HleS16* src, HleS16* dryL, HleS16* dryR, HleS16* wetL,
                              HleS16* wetR, int32_t volL, int32_t volR, int32_t reverb) {
    const HleU32* s = (const HleU32*)(const void*)src;
    HleU32* dl = (HleU32*)(void*)dryL;
    HleU32* dr = (HleU32*)(void*)dryR;
    HleU32* wl = (HleU32*)(void*)wetL;
    HleU32* wr = (HleU32*)(void*)wetR;
    uint32_t k;
    for (k = 0; k < 4u; k++) {
        const uint32_t in = s[k];
        const uint32_t l = Pack16(MulwLo(volL, in), MulwHi(volL, in));
        const uint32_t r = Pack16(MulwLo(volR, in), MulwHi(volR, in));
        dl[k] = Qadd16(dl[k], l);
        dr[k] = Qadd16(dr[k], r);
        wl[k] = Qadd16(wl[k], Pack16(MulwLo(reverb, l), MulwHi(reverb, l)));
        wr[k] = Qadd16(wr[k], Pack16(MulwLo(reverb, r), MulwHi(reverb, r)));
    }
}

int gdx_hlefast_envmix(const GdxHleFastCtx* c, uint32_t dmemSrc, uint32_t sampleCount,
                       const uint32_t dests[4], int32_t volL, int32_t volR, int32_t reverb,
                       int32_t rampL, int32_t rampR, int32_t rampReverb, uint32_t xorBits) {
    const uint32_t numBlocks = sampleCount >> 3;
    const uint32_t bytes = numBlocks * 16u;
    const HleS16* src;
    HleS16* dryL;
    HleS16* dryR;
    HleS16* wetL;
    HleS16* wetR;
    uint32_t blk;
    uint32_t d;

    if (xorBits != 0u) {
        return 0; /* [debug] envmix_xbits: the mupen64plus xor-mask semantics aren't packed here */
    }
    if (!EvenRangeOk(dmemSrc, bytes)) {
        return 0;
    }
    for (d = 0; d < 4u; d++) {
        if (!EvenRangeOk(dests[d], bytes)) {
            return 0;
        }
    }
    src = DmemS16(c, dmemSrc);
    dryL = DmemS16(c, dests[0]);
    dryR = DmemS16(c, dests[1]);
    wetL = DmemS16(c, dests[2]);
    wetR = DmemS16(c, dests[3]);
    /* Per sample: one source read, then the four bus updates in the reference's order, so a
     * bus that aliases the source or another bus accumulates identically. */
    for (blk = 0; blk < numBlocks; blk++) {
        uint32_t i;
        const uint32_t end = blk * 8u + 8u;
        if (VolumePackable(volL) && VolumePackable(volR) && VolumePackable(reverb)) {
            const uint32_t o = blk * 8u;
            sPathCounts[GDX_HLEFAST_PATH_ENVMIX_PACKED]++;
            EnvmixBlockPacked(src + o, dryL + o, dryR + o, wetL + o, wetR + o, volL, volR, reverb);
            volL += rampL;
            volR += rampR;
            reverb += rampReverb;
            continue;
        }
        sPathCounts[GDX_HLEFAST_PATH_ENVMIX_SCALAR]++;
        for (i = blk * 8u; i < end; i++) {
            const int32_t s = src[i];
            const int32_t dl = (s * volL) >> 16;
            const int32_t dr = (s * volR) >> 16;
            const int32_t wl = (dl * reverb) >> 16;
            const int32_t wr = (dr * reverb) >> 16;
            dryL[i] = Sat16(dryL[i] + dl);
            dryR[i] = Sat16(dryR[i] + dr);
            wetL[i] = Sat16(wetL[i] + wl);
            wetR[i] = Sat16(wetR[i] + wr);
        }
        volL += rampL;
        volR += rampR;
        reverb += rampReverb;
    }
    for (d = 0; d < 4u; d++) {
        MarkWritten(c, dests[d], bytes);
    }
    return 1;
}

/* ---- A_MIXER / A_ADDMIXER / A_HILOGAIN ----------------------------------------------------- */

/* (s * gain) >> 15 == SMLAWB(gain << 1, s) >> 0 exactly (the 48-bit product halves the same way),
 * and with gain != -32768 the result lies in [-32767, 32767]: it packs, and QADD16 is the
 * reference's ClampS16(out + that). Needs 4-byte alignment and disjoint or identical windows. */
static void MixerPairs(HleS16* out, const HleS16* in, uint32_t numSamples, int32_t gain) {
    HleU32* o = (HleU32*)(void*)out;
    const HleU32* s = (const HleU32*)(const void*)in;
    const int32_t gain2 = gain * 2;
    uint32_t k;
    for (k = 0; k < numSamples / 2u; k++) {
        const uint32_t w = s[k];
        o[k] = Qadd16(o[k], Pack16(MulwLo(gain2, w), MulwHi(gain2, w)));
    }
    for (k = numSamples & ~1u; k < numSamples; k++) {
        const int32_t v = in[k];
        out[k] = Sat16((int32_t)out[k] + ((v * gain) >> 15));
    }
}

int gdx_hlefast_mixer(const GdxHleFastCtx* c, uint32_t dmemIn, uint32_t dmemOut,
                      uint32_t numSamples, int32_t gain) {
    const uint32_t bytes = numSamples * 2u;
    const HleS16* in;
    HleS16* out;
    uint32_t k;
    if (!EvenRangeOk(dmemIn, bytes) || !EvenRangeOk(dmemOut, bytes)) {
        return 0;
    }
    in = DmemS16(c, dmemIn);
    out = DmemS16(c, dmemOut);
    if (((dmemIn | dmemOut) & 3u) == 0u && gain != -32768 &&
        (dmemIn == dmemOut || !Overlaps(dmemIn, bytes, dmemOut, bytes))) {
        sPathCounts[GDX_HLEFAST_PATH_MIXER_PACKED]++;
        MixerPairs(out, in, numSamples, gain);
    } else {
        sPathCounts[GDX_HLEFAST_PATH_MIXER_SCALAR]++;
        for (k = 0; k < numSamples; k++) {
            const int32_t s = in[k];
            out[k] = Sat16((int32_t)out[k] + ((s * gain) >> 15));
        }
    }
    MarkWritten(c, dmemOut, bytes);
    return 1;
}

/* QADD16 is two lane-wise ClampS16(a + b). Pairwise needs 4-byte alignment and either no
 * overlap or exact aliasing (then each lane still reads its own sample before writing it). */
static void AddMixPairs(HleS16* out, const HleS16* in, uint32_t numSamples) {
    HleU32* o = (HleU32*)(void*)out;
    const HleU32* s = (const HleU32*)(const void*)in;
    uint32_t k;
    for (k = 0; k < numSamples / 2u; k++) {
        o[k] = Qadd16(o[k], s[k]);
    }
    for (k = numSamples & ~1u; k < numSamples; k++) {
        out[k] = Sat16((int32_t)out[k] + in[k]);
    }
}

int gdx_hlefast_addmix(const GdxHleFastCtx* c, uint32_t dmemIn, uint32_t dmemOut,
                       uint32_t numSamples) {
    const uint32_t bytes = numSamples * 2u;
    const HleS16* in;
    HleS16* out;
    uint32_t k;
    if (!EvenRangeOk(dmemIn, bytes) || !EvenRangeOk(dmemOut, bytes)) {
        return 0;
    }
    in = DmemS16(c, dmemIn);
    out = DmemS16(c, dmemOut);
    if (((dmemIn | dmemOut) & 3u) == 0u && (dmemIn == dmemOut || !Overlaps(dmemIn, bytes, dmemOut, bytes))) {
        AddMixPairs(out, in, numSamples);
    } else {
        for (k = 0; k < numSamples; k++) {
            out[k] = Sat16((int32_t)out[k] + in[k]);
        }
    }
    MarkWritten(c, dmemOut, bytes);
    return 1;
}

int gdx_hlefast_hilogain(const GdxHleFastCtx* c, uint32_t dmem, uint32_t numSamples, int32_t gain) {
    const uint32_t bytes = numSamples * 2u;
    HleS16* p;
    uint32_t k;
    if (!EvenRangeOk(dmem, bytes)) {
        return 0;
    }
    p = DmemS16(c, dmem);
    for (k = 0; k < numSamples; k++) {
        const int32_t s = p[k];
        p[k] = Sat16((s * gain) >> 4);
    }
    MarkWritten(c, dmem, bytes);
    return 1;
}

/* ---- A_INTERLEAVE -------------------------------------------------------------------------- */

int gdx_hlefast_interleave(const GdxHleFastCtx* c, uint32_t dmemOut, uint32_t dmemL,
                           uint32_t dmemR, uint32_t numSamples) {
    const uint32_t inBytes = numSamples * 2u;
    const uint32_t outBytes = numSamples * 4u;
    const HleS16* l;
    const HleS16* r;
    uint32_t k;
    if (!EvenRangeOk(dmemOut, outBytes) || !EvenRangeOk(dmemL, inBytes) ||
        !EvenRangeOk(dmemR, inBytes)) {
        return 0;
    }
    /* The reference interleaves in place sample by sample; writing its own input changes what
     * later iterations read. Keep that case on the reference rather than reproduce it. */
    if (Overlaps(dmemOut, outBytes, dmemL, inBytes) || Overlaps(dmemOut, outBytes, dmemR, inBytes)) {
        return 0;
    }
    l = DmemS16(c, dmemL);
    r = DmemS16(c, dmemR);
    if ((dmemOut & 3u) == 0u) {
        HleU32* o = (HleU32*)(void*)(c->dmem + dmemOut);
        for (k = 0; k < numSamples; k++) {
            /* little-endian word = L in the low half, R in the high half: PKHBT */
            o[k] = (uint32_t)(uint16_t)l[k] | ((uint32_t)(uint16_t)r[k] << 16);
        }
    } else {
        HleS16* o = DmemS16(c, dmemOut);
        for (k = 0; k < numSamples; k++) {
            o[2u * k] = l[k];
            o[2u * k + 1u] = r[k];
        }
    }
    MarkWritten(c, dmemOut, outBytes);
    return 1;
}

/* ---- A_FILTER apply: RunFilter ------------------------------------------------------------- */

/* The reference sums eight s16 x s16 products in an int32 that can wrap (8 x 2^30); the 3DS
 * build is -fwrapv. Summing in uint32 gives the same bits without relying on it. */
static inline int16_t FilterTap8(const int16_t coef[8], const int16_t* win) {
    uint32_t acc = 0;
    int t;
    for (t = 0; t < 8; t++) {
        acc += (uint32_t)((int32_t)coef[t] * (int32_t)win[t]);
    }
    return Sat16((int32_t)(acc + 0x4000u) >> 15);
}

int gdx_hlefast_filter(const GdxHleFastCtx* c, uint32_t dmemBuf, uint32_t sizeBytes,
                       uint32_t flags, int16_t* state, const int16_t coef[8]) {
    const uint32_t numSamples = sizeBytes / 2u;
    const uint32_t numBlocks = numSamples / 8u;
    int16_t win[16];
    HleS16* buf;
    uint32_t blk;
    int t;

    if (state == NULL || coef == NULL || numSamples == 0u ||
        !EvenRangeOk(dmemBuf, numBlocks * 16u)) {
        return 0;
    }
    for (t = 0; t < 8; t++) {
        win[t] = ((flags & 1u) != 0u) ? 0 : state[t];
    }
    buf = DmemS16(c, dmemBuf);
    for (blk = 0; blk < numBlocks; blk++) {
        HleS16* b = buf + blk * 8u;
        int16_t outBlk[8];
        int k;
        for (t = 0; t < 8; t++) {
            win[8 + t] = b[t];
        }
        for (k = 0; k < 8; k++) {
            outBlk[k] = FilterTap8(coef, &win[k]);
        }
        for (k = 0; k < 8; k++) {
            b[k] = outBlk[k];
        }
        for (t = 0; t < 8; t++) {
            win[t] = win[8 + t]; /* this block's INPUT is the next block's tail */
        }
    }
    MarkWritten(c, dmemBuf, numBlocks * 16u);
    for (t = 0; t < 8; t++) {
        state[t] = win[t];
    }
    return 1;
}
