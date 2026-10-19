/* gdx_save_ek_layout_tests.c -- host test of port/gdx_save_ek_layout.c (ekship, o3dsek-plan S1).
 *
 * Images are built the way the game's own writers build them (decomp ovl_i2/save.c): the settings
 * section with its byte-sum checksum, and the ProfileSave union written to BOTH profiles with one
 * checksum -- by the shipped writer (Save_SaveEditCup: unk fields zero, any track names) or by the
 * EK writer (Save_UpdateCupCompletion / staff-ghost: cupCompletion bits 1 << (difficulty * 2),
 * staffGhostCompletion bits per DD course). Asserts:
 *   - nothing the shipped writer can produce is ever PROVEN (fuzzed over arbitrary names);
 *   - every EK image with a mark in bytes 2..9 is PROVEN (fuzzed), and one without is not;
 *   - each decline reason fires on its own defect;
 *   - the file seed reads the shared save read-only: its bytes, size and mtime are unchanged after
 *     every verdict, and the output image is written only on PROVEN. */
#include "gdx_save_ek_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        sChecks++;                                          \
        if (!(cond)) {                                      \
            sFailures++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

static uint8_t sImage[GDX_SAVE_EK_IMAGE_SIZE];
static unsigned sRng = 12345u;

static unsigned NextRandom(void) {
    sRng = sRng * 1103515245u + 12345u;
    return (sRng >> 16) & 0x7FFFu;
}

static unsigned Sum16(const uint8_t* bytes, size_t count) {
    unsigned sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum = (sum + bytes[i]) & 0xFFFFu;
    }
    return sum;
}

static void PutU16Le(uint8_t* at, unsigned value) {
    at[0] = (uint8_t) (value & 0xFFu);
    at[1] = (uint8_t) ((value >> 8) & 0xFFu);
}

static uint8_t* Section(unsigned profile) {
    return sImage + profile * GDX_SAVE_EK_PROFILE_SIZE + GDX_SAVE_EK_SECTION_OFFSET;
}

/* Save_SaveSettings on both profiles. */
static void WriteSettings(const char* fileName) {
    for (unsigned p = 0; p < 2; p++) {
        uint8_t* settings = sImage + p * GDX_SAVE_EK_PROFILE_SIZE;
        memset(settings, 0, GDX_SAVE_EK_SETTINGS_SIZE);
        memcpy(settings, fileName, strlen(fileName) < 8 ? strlen(fileName) : 8);
        settings[8] = 0x03;
        PutU16Le(settings + 0xE, Sum16(settings, 0xE));
    }
}

/* Writes SECTION0's 0x40 bytes (checksum recomputed) to both profiles, as the game does. */
static void WriteMirroredSection(uint8_t* bytes) {
    PutU16Le(bytes, Sum16(bytes + 2, GDX_SAVE_EK_SECTION_SIZE - 2));
    memcpy(Section(0), bytes, GDX_SAVE_EK_SECTION_SIZE);
    memcpy(Section(1), bytes, GDX_SAVE_EK_SECTION_SIZE);
}

static void FreshImage(void) {
    memset(sImage, 0, sizeof(sImage));
    WriteSettings("FZERO");
}

/* The shipped writer: unk_02/unk_04/unk_08 zero, six 9-byte track names at +0xA. */
static void WriteShippedEditCup(const uint8_t names[54]) {
    uint8_t bytes[GDX_SAVE_EK_SECTION_SIZE];
    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes + 0xA, names, 54);
    WriteMirroredSection(bytes);
}

/* The EK writer: staffGhostCompletion and the 60 cupCompletion bytes. */
static void WriteEkDdCups(unsigned staff, const uint8_t cups[60]) {
    uint8_t bytes[GDX_SAVE_EK_SECTION_SIZE];
    memset(bytes, 0, sizeof(bytes));
    PutU16Le(bytes + 2, staff);
    memcpy(bytes + 4, cups, 60);
    WriteMirroredSection(bytes);
}

static GdxSaveEkVerdict Classify(GdxSaveEkReport* report) {
    return gdx_save_ek_classify(sImage, sizeof(sImage), report);
}

static void TestFreshAndShipped(void) {
    FreshImage();
    CHECK(Classify(NULL) == GDX_SAVE_EK_NO_EK_MARK, "a zero image is not EK: %s",
          gdx_save_ek_verdict_name(Classify(NULL)));

    uint8_t names[54];
    memset(names, 0, sizeof(names));
    memcpy(names, "MYCOURSE", 8);
    FreshImage();
    WriteShippedEditCup(names);
    CHECK(Classify(NULL) != GDX_SAVE_EK_PROVEN, "a named shipped edit cup is never EK");

    /* Fuzz the shipped writer over arbitrary name bytes: never PROVEN. */
    unsigned proven = 0;
    for (int iter = 0; iter < 20000; iter++) {
        for (int i = 0; i < 54; i++) {
            names[i] = (uint8_t) (NextRandom() & 0xFFu);
        }
        FreshImage();
        WriteShippedEditCup(names);
        if (Classify(NULL) == GDX_SAVE_EK_PROVEN) {
            proven++;
        }
    }
    CHECK(proven == 0, "shipped edit cups classified EK: %u of 20000", proven);
}

static void TestEkProven(void) {
    uint8_t cups[60];
    GdxSaveEkReport report;

    memset(cups, 0, sizeof(cups));
    FreshImage();
    WriteEkDdCups(1u << 3, cups); /* staff ghost beaten on DD course 3 */
    CHECK(Classify(&report) == GDX_SAVE_EK_PROVEN, "staff bit alone proves EK: %s",
          gdx_save_ek_verdict_name(Classify(NULL)));
    CHECK(report.staffGhostCompletion == 8u && report.ddMarks == 0, "report staff=%x marks=%u",
          report.staffGhostCompletion, report.ddMarks);

    memset(cups, 0, sizeof(cups));
    cups[0] = 1u << (2 * 2); /* character 0, DD-1 cup, difficulty 2 */
    FreshImage();
    WriteEkDdCups(0, cups);
    CHECK(Classify(&report) == GDX_SAVE_EK_PROVEN, "cupCompletion[0] proves EK");
    CHECK(report.ddMarks == 1, "one mark, got %u", report.ddMarks);

    /* Characters 3+ only: bytes the shipped layout uses for names -> not proven, start fresh. */
    memset(cups, 0, sizeof(cups));
    cups[10] = 0x01;
    FreshImage();
    WriteEkDdCups(0, cups);
    CHECK(Classify(NULL) == GDX_SAVE_EK_NO_EK_MARK, "a mark only past byte 9 is not proof");

    /* Fuzz the EK writer: every image with a mark in 2..9 is PROVEN. */
    unsigned missed = 0;
    for (int iter = 0; iter < 20000; iter++) {
        for (int i = 0; i < 60; i++) {
            cups[i] = (uint8_t) (NextRandom() & 0x55u);
        }
        unsigned staff = NextRandom() & 0xFFFu;
        const int hasMark = staff != 0 || cups[0] | cups[1] | cups[2] | cups[3] | cups[4] | cups[5];
        FreshImage();
        WriteEkDdCups(staff, cups);
        const GdxSaveEkVerdict verdict = Classify(NULL);
        if (hasMark && verdict != GDX_SAVE_EK_PROVEN) {
            missed++;
        }
        if (!hasMark && verdict == GDX_SAVE_EK_PROVEN) {
            missed++;
        }
    }
    CHECK(missed == 0, "EK fuzz misclassified %u of 20000", missed);
}

static void TestDeclineReasons(void) {
    uint8_t cups[60];
    memset(cups, 0, sizeof(cups));
    cups[1] = 0x04;

    CHECK(gdx_save_ek_classify(sImage, sizeof(sImage) - 1, NULL) == GDX_SAVE_EK_BAD_SIZE, "short image");
    CHECK(gdx_save_ek_classify(NULL, sizeof(sImage), NULL) == GDX_SAVE_EK_BAD_SIZE, "NULL image");

    FreshImage();
    WriteEkDdCups(0, cups);
    sImage[3] ^= 0x01; /* profile 0 settings byte without its checksum */
    CHECK(Classify(NULL) == GDX_SAVE_EK_SETTINGS_CHECKSUM, "settings checksum");

    FreshImage();
    WriteEkDdCups(0, cups);
    Section(1)[20] ^= 0x10;
    CHECK(Classify(NULL) == GDX_SAVE_EK_SECTION_CHECKSUM, "profile 1 section checksum");

    FreshImage();
    WriteEkDdCups(0, cups);
    Section(0)[0] ^= 0x01;
    CHECK(Classify(NULL) == GDX_SAVE_EK_SECTION_CHECKSUM, "profile 0 section checksum");

    /* Both copies valid but different. */
    FreshImage();
    WriteEkDdCups(0, cups);
    uint8_t other[GDX_SAVE_EK_SECTION_SIZE];
    memcpy(other, Section(0), sizeof(other));
    other[30] = 0x01;
    PutU16Le(other, Sum16(other + 2, GDX_SAVE_EK_SECTION_SIZE - 2));
    memcpy(Section(1), other, sizeof(other));
    CHECK(Classify(NULL) == GDX_SAVE_EK_SECTION_MIRROR, "mirror");

    FreshImage();
    WriteEkDdCups(0x1000u, cups);
    CHECK(Classify(NULL) == GDX_SAVE_EK_FIELD_RANGE, "staff > 0xFFF");

    cups[7] = 0x02; /* not a difficulty bit */
    FreshImage();
    WriteEkDdCups(0, cups);
    CHECK(Classify(NULL) == GDX_SAVE_EK_FIELD_RANGE, "cup bit outside 0x55");
}

/* ---- the file seed: read-only on the shared save ------------------------------------------- */

static const char* kLegacy = "ekseed_fzerox.sav";

static void WriteFile(const char* path, const uint8_t* bytes, size_t size) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        CHECK(0, "cannot create %s", path);
        return;
    }
    fwrite(bytes, 1, size, f);
    fclose(f);
}

static int ReadBack(const char* path, uint8_t* bytes, size_t cap, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    *size = fread(bytes, 1, cap, f);
    fclose(f);
    return 1;
}

/* Seeds from KLEGACY holding SIZE bytes of sImage; asserts the verdict, the image contract, and that
 * the file is bit-for-bit, size and mtime unchanged. */
static void SeedAndCheck(size_t size, GdxSaveEkVerdict expected, const char* what) {
    static uint8_t out[GDX_SAVE_EK_IMAGE_SIZE];
    static uint8_t back[GDX_SAVE_EK_IMAGE_SIZE + 16];
    WriteFile(kLegacy, sImage, size);
    struct stat before;
    stat(kLegacy, &before);
    memset(out, 0xA5, sizeof(out));

    GdxSaveEkSeed seed;
    const int copied = gdx_save_ek_seed_from_file(kLegacy, out, &seed);
    CHECK(seed.verdict == expected, "%s: verdict %s, expected %s", what, gdx_save_ek_verdict_name(seed.verdict),
          gdx_save_ek_verdict_name(expected));
    CHECK(copied == (expected == GDX_SAVE_EK_PROVEN), "%s: copied=%d", what, copied);
    CHECK(seed.legacyBytes == (long) size, "%s: legacyBytes=%ld", what, seed.legacyBytes);
    if (copied) {
        CHECK(memcmp(out, sImage, GDX_SAVE_EK_IMAGE_SIZE) == 0, "%s: copy differs from the source", what);
    } else {
        int untouched = 1;
        for (size_t i = 0; i < sizeof(out); i++) {
            if (out[i] != 0xA5) {
                untouched = 0;
                break;
            }
        }
        CHECK(untouched, "%s: the image was written on a decline", what);
    }

    struct stat after;
    stat(kLegacy, &after);
    size_t backSize = 0;
    CHECK(ReadBack(kLegacy, back, sizeof(back), &backSize), "%s: the shared save disappeared", what);
    CHECK(backSize == size && memcmp(back, sImage, size) == 0, "%s: the shared save changed", what);
    CHECK(after.st_size == before.st_size && after.st_mtime == before.st_mtime, "%s: the shared save was touched",
          what);
    remove(kLegacy);
}

static void TestFileSeed(void) {
    uint8_t cups[60];
    memset(cups, 0, sizeof(cups));
    cups[2] = 0x11;

    FreshImage();
    WriteEkDdCups(0x0F0u, cups);
    SeedAndCheck(sizeof(sImage), GDX_SAVE_EK_PROVEN, "EK save");

    FreshImage();
    SeedAndCheck(sizeof(sImage), GDX_SAVE_EK_NO_EK_MARK, "shipped/fresh save");

    FreshImage();
    WriteEkDdCups(0x0F0u, cups);
    SeedAndCheck(sizeof(sImage) - 100, GDX_SAVE_EK_BAD_SIZE, "torn save");

    remove(kLegacy);
    GdxSaveEkSeed seed;
    uint8_t probe = 0x5A;
    CHECK(gdx_save_ek_seed_from_file(kLegacy, &probe, &seed) == 0 && seed.verdict == GDX_SAVE_EK_NO_FILE &&
              seed.legacyBytes == -1 && probe == 0x5A,
          "missing shared save: verdict %s", gdx_save_ek_verdict_name(seed.verdict));
    CHECK(gdx_save_ek_seed_from_file(NULL, &probe, NULL) == 0, "NULL path");
}

int main(void) {
    TestFreshAndShipped();
    TestEkProven();
    TestDeclineReasons();
    TestFileSeed();
    if (sFailures != 0) {
        printf("gdx_save_ek_layout_tests: %d of %d checks FAILED\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_save_ek_layout_tests: %d checks passed (shipped fuzz 20000, EK fuzz 20000, read-only seed)\n",
           sChecks);
    return 0;
}
