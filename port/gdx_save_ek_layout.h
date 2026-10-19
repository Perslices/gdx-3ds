#pragma once
/* EKSHIP (docs/research/native/o3dsek-plan.md §7, S1): is a cart-SRAM image provably written by an
 * Expansion Kit build?
 *
 * The 3DS EK title keeps its own save (saves/fzerox-ek.sav). Before that, an EK .3dsx and the shipped
 * game shared saves/fzerox.sav, and the two builds read ONE 0x40-byte section of each ProfileSave
 * differently: SaveEditCup in the shipped build, SaveDDCups in the EK (decomp/include/fzx_save.h, a
 * union at ProfileSave+0x19A0). Both checksums cover the same bytes with the same byte sum, so a
 * checksum cannot tell the layouts apart. What can:
 *   - the shipped build writes SaveEditCup.unk_02/unk_04/unk_08 as ZERO on every path (Save_SaveEditCup,
 *     Save_InitEditCup's clear, the port's zero-filled fresh image), i.e. section bytes 2..9;
 *   - the EK writes staffGhostCompletion (bytes 2..3, one bit per DD course, <= 0xFFF) and
 *     cupCompletion[0..5] (bytes 4..9, one bit per difficulty at 1 << (difficulty * 2), so every byte
 *     is a subset of 0x55) there.
 * So a section with BOTH profile copies checksum-valid and identical, every DD field in range, and a
 * NONZERO byte in 2..9 can only have been written by an EK build. Anything else -- including an EK
 * save whose only DD progress is for characters 3+ (bytes 10+, which the shipped layout uses for
 * edit-cup names) -- is NOT proven, and the caller starts fresh.
 *
 * Pure function over bytes: no file I/O, host-testable (port/tests/gdx_save_ek_layout_tests.c). */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_SAVE_EK_IMAGE_SIZE 0x8000u      /* SaveContext */
#define GDX_SAVE_EK_PROFILE_SIZE 0x19E0u    /* ProfileSave */
#define GDX_SAVE_EK_SECTION_OFFSET 0x19A0u  /* ProfileSave.editCup / .ddCups */
#define GDX_SAVE_EK_SECTION_SIZE 0x40u
#define GDX_SAVE_EK_SETTINGS_SIZE 0x10u     /* SaveSettings, checksum at +0xE */

typedef enum GdxSaveEkVerdict {
    GDX_SAVE_EK_PROVEN = 0,          /* EK-layout, safe to copy */
    GDX_SAVE_EK_BAD_SIZE,            /* not a 32 KB image */
    GDX_SAVE_EK_SETTINGS_CHECKSUM,   /* profile 0's settings checksum fails: not a usable save */
    GDX_SAVE_EK_SECTION_CHECKSUM,    /* a profile's DD/edit-cup section checksum fails */
    GDX_SAVE_EK_SECTION_MIRROR,      /* the two profile copies of the section differ */
    GDX_SAVE_EK_FIELD_RANGE,         /* staffGhostCompletion > 0xFFF or a cupCompletion bit outside 0x55 */
    GDX_SAVE_EK_NO_EK_MARK,          /* bytes 2..9 all zero: indistinguishable from the shipped layout */
    GDX_SAVE_EK_NO_FILE,             /* gdx_save_ek_seed_from_file: the shared save does not exist */
    GDX_SAVE_EK_READ_FAILED,         /* gdx_save_ek_seed_from_file: open/read error or out of memory */
} GdxSaveEkVerdict;

typedef struct GdxSaveEkReport {
    unsigned staffGhostCompletion; /* section bytes 2..3 (little-endian, the 3DS in-memory order) */
    unsigned ddMarks;              /* nonzero cupCompletion bytes over all 60 */
} GdxSaveEkReport;

/* REPORT may be NULL. */
GdxSaveEkVerdict gdx_save_ek_classify(const uint8_t* image, size_t size, GdxSaveEkReport* report);

const char* gdx_save_ek_verdict_name(GdxSaveEkVerdict verdict);

typedef struct GdxSaveEkSeed {
    GdxSaveEkVerdict verdict;
    long legacyBytes; /* size of the shared save, -1 when absent */
    GdxSaveEkReport report;
} GdxSaveEkSeed;

/* The one-time copy. Opens LEGACYPATH READ-ONLY ("rb"; never written, renamed or removed), classifies
 * it, and only on GDX_SAVE_EK_PROVEN copies its GDX_SAVE_EK_IMAGE_SIZE bytes into IMAGE and returns 1.
 * IMAGE is untouched on every other verdict. OUT may be NULL. */
int gdx_save_ek_seed_from_file(const char* legacyPath, uint8_t* image, GdxSaveEkSeed* out);

#ifdef __cplusplus
}
#endif
