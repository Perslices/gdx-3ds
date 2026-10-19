/* EKSHIP: see gdx_save_ek_layout.h for the proof this implements. */
#include "gdx_save_ek_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GDX_SAVE_EK_STAFF_MAX 0xFFFu   /* 12 DD courses, save.c: sDDStaffGhostCompletion &= 0xFFF */
#define GDX_SAVE_EK_CUP_BITS 0x55u     /* 1 << (difficulty * 2), difficulty 0..3 */
#define GDX_SAVE_EK_CUP_COUNT 60u      /* SaveDDCups.cupCompletion[30 * 2] */
#define GDX_SAVE_EK_MARK_FIRST 2u      /* staffGhostCompletion */
#define GDX_SAVE_EK_MARK_END 10u       /* ... through cupCompletion[5]: shipped writes zeros here */

static unsigned ReadU16Le(const uint8_t* bytes) {
    return (unsigned) bytes[0] | ((unsigned) bytes[1] << 8);
}

/* save.c Save_CalculateChecksum: a u16 byte sum. */
static unsigned ByteSum16(const uint8_t* bytes, size_t count) {
    unsigned sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum = (sum + bytes[i]) & 0xFFFFu;
    }
    return sum;
}

static const uint8_t* ProfileSection(const uint8_t* image, unsigned profile) {
    return image + profile * GDX_SAVE_EK_PROFILE_SIZE + GDX_SAVE_EK_SECTION_OFFSET;
}

static int SettingsChecksumValid(const uint8_t* image) {
    const unsigned stored = ReadU16Le(image + GDX_SAVE_EK_SETTINGS_SIZE - 2u);
    return stored == ByteSum16(image, GDX_SAVE_EK_SETTINGS_SIZE - 2u);
}

static int SectionChecksumValid(const uint8_t* section) {
    return ReadU16Le(section) == ByteSum16(section + 2, GDX_SAVE_EK_SECTION_SIZE - 2u);
}

static int DdFieldsInRange(const uint8_t* section) {
    if (ReadU16Le(section + 2) > GDX_SAVE_EK_STAFF_MAX) {
        return 0;
    }
    for (unsigned i = 0; i < GDX_SAVE_EK_CUP_COUNT; i++) {
        if ((section[4 + i] & ~GDX_SAVE_EK_CUP_BITS) != 0) {
            return 0;
        }
    }
    return 1;
}

static int HasEkMark(const uint8_t* section) {
    for (unsigned i = GDX_SAVE_EK_MARK_FIRST; i < GDX_SAVE_EK_MARK_END; i++) {
        if (section[i] != 0) {
            return 1;
        }
    }
    return 0;
}

static unsigned CountDdMarks(const uint8_t* section) {
    unsigned marks = 0;
    for (unsigned i = 0; i < GDX_SAVE_EK_CUP_COUNT; i++) {
        if (section[4 + i] != 0) {
            marks++;
        }
    }
    return marks;
}

GdxSaveEkVerdict gdx_save_ek_classify(const uint8_t* image, size_t size, GdxSaveEkReport* report) {
    if (report != NULL) {
        report->staffGhostCompletion = 0;
        report->ddMarks = 0;
    }
    if (image == NULL || size != GDX_SAVE_EK_IMAGE_SIZE) {
        return GDX_SAVE_EK_BAD_SIZE;
    }
    const uint8_t* section0 = ProfileSection(image, 0);
    const uint8_t* section1 = ProfileSection(image, 1);
    if (report != NULL) {
        report->staffGhostCompletion = ReadU16Le(section0 + 2);
        report->ddMarks = CountDdMarks(section0);
    }
    if (!SettingsChecksumValid(image)) {
        return GDX_SAVE_EK_SETTINGS_CHECKSUM;
    }
    if (!SectionChecksumValid(section0) || !SectionChecksumValid(section1)) {
        return GDX_SAVE_EK_SECTION_CHECKSUM;
    }
    if (memcmp(section0, section1, GDX_SAVE_EK_SECTION_SIZE) != 0) {
        return GDX_SAVE_EK_SECTION_MIRROR;
    }
    if (!DdFieldsInRange(section0)) {
        return GDX_SAVE_EK_FIELD_RANGE;
    }
    if (!HasEkMark(section0)) {
        return GDX_SAVE_EK_NO_EK_MARK;
    }
    return GDX_SAVE_EK_PROVEN;
}

const char* gdx_save_ek_verdict_name(GdxSaveEkVerdict verdict) {
    switch (verdict) {
        case GDX_SAVE_EK_PROVEN:
            return "proven";
        case GDX_SAVE_EK_BAD_SIZE:
            return "bad-size";
        case GDX_SAVE_EK_SETTINGS_CHECKSUM:
            return "settings-checksum";
        case GDX_SAVE_EK_SECTION_CHECKSUM:
            return "section-checksum";
        case GDX_SAVE_EK_SECTION_MIRROR:
            return "section-mirror";
        case GDX_SAVE_EK_FIELD_RANGE:
            return "field-range";
        case GDX_SAVE_EK_NO_EK_MARK:
            return "no-ek-mark";
        case GDX_SAVE_EK_NO_FILE:
            return "no-file";
        case GDX_SAVE_EK_READ_FAILED:
            return "read-failed";
    }
    return "unknown";
}

static long FileSize(FILE* f) {
    if (fseek(f, 0, SEEK_END) != 0) {
        return -1;
    }
    const long size = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) {
        return -1;
    }
    return size;
}

static GdxSaveEkVerdict ClassifyOpenFile(FILE* f, uint8_t* buffer, GdxSaveEkSeed* seed) {
    seed->legacyBytes = FileSize(f);
    if (seed->legacyBytes != (long) GDX_SAVE_EK_IMAGE_SIZE) {
        return seed->legacyBytes < 0 ? GDX_SAVE_EK_READ_FAILED : GDX_SAVE_EK_BAD_SIZE;
    }
    if (fread(buffer, 1, GDX_SAVE_EK_IMAGE_SIZE, f) != GDX_SAVE_EK_IMAGE_SIZE) {
        return GDX_SAVE_EK_READ_FAILED;
    }
    return gdx_save_ek_classify(buffer, GDX_SAVE_EK_IMAGE_SIZE, &seed->report);
}

int gdx_save_ek_seed_from_file(const char* legacyPath, uint8_t* image, GdxSaveEkSeed* out) {
    GdxSaveEkSeed seed;
    memset(&seed, 0, sizeof(seed));
    seed.legacyBytes = -1;
    seed.verdict = GDX_SAVE_EK_NO_FILE;
    FILE* f = (legacyPath != NULL) ? fopen(legacyPath, "rb") : NULL;
    if (f != NULL) {
        uint8_t* buffer = (uint8_t*) malloc(GDX_SAVE_EK_IMAGE_SIZE);
        if (buffer == NULL) {
            seed.verdict = GDX_SAVE_EK_READ_FAILED;
        } else {
            seed.verdict = ClassifyOpenFile(f, buffer, &seed);
            if (seed.verdict == GDX_SAVE_EK_PROVEN && image != NULL) {
                memcpy(image, buffer, GDX_SAVE_EK_IMAGE_SIZE);
            }
            free(buffer);
        }
        fclose(f);
    }
    if (out != NULL) {
        *out = seed;
    }
    return (seed.verdict == GDX_SAVE_EK_PROVEN && image != NULL) ? 1 : 0;
}
