/* track_template_tests.c — host unit test for the [native] track index-template decoder.
 *
 * Runs gdxtrk_decode_template() over the 18 committed raw blobs (gdx3ds_track_templates_raw.inc,
 * the bytes the device's asset-bound D_8014* arrays hold) and asserts the exact decoded index
 * arrays and SETCOMBINE events against the committed golden table — then again over a
 * byte-swapped copy (the N64 big-endian word order) to prove the word-order sniff. Also pins
 * the structural facts the replay relies on: every template is 10-16 tris, slots <= 20, and
 * every SETCOMBINE-carrying template restores the track combiner (FC147E28 44FE793C) at its
 * end. Compiled by tools/ci-3ds.sh like the other host tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../3ds/gfx/gdx3ds_track_templates.h"
#include "../3ds/gfx/gdx3ds_track_templates_raw.inc"

static int sFailures = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            sFailures++;                                    \
            printf("FAIL: " __VA_ARGS__);                   \
            printf("\n");                                   \
        }                                                   \
    } while (0)

static void ByteSwapCopy(const unsigned char* src, unsigned len, unsigned char* dst) {
    unsigned i;
    for (i = 0; i < len; i += 4) {
        dst[i + 0] = src[i + 3];
        dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1];
        dst[i + 3] = src[i + 0];
    }
}

int main(void) {
    int i;
    CHECK(sizeof(kGdxTrkRawTable) / sizeof(kGdxTrkRawTable[0]) == GDXTRK_TEMPLATE_COUNT, "raw table count");
    for (i = 0; i < GDXTRK_TEMPLATE_COUNT; i++) {
        GdxTrkTemplate t;
        GdxTrkTemplate tBig;
        unsigned char swapped[GDXTRK_RAW_MAX_BYTES];
        const char* name = kGdxTrkRawTable[i].name;
        int rc = gdxtrk_decode_template(kGdxTrkRawTable[i].raw, kGdxTrkRawTable[i].len, &t);
        CHECK(rc == GDXTRK_DECODE_OK, "%s: decode rc=%d", name, rc);
        CHECK(strcmp(name, kGdxTrkTemplateNames[i]) == 0, "%s: name table order (%s)", name, kGdxTrkTemplateNames[i]);
        CHECK(gdxtrk_template_equal(&t, &kGdxTrkGolden[i]), "%s: decoded != golden (ntris %d/%d events %d/%d)",
              name, t.ntris, kGdxTrkGolden[i].ntris, t.nevents, kGdxTrkGolden[i].nevents);
        CHECK(t.ntris >= 10 && t.ntris <= GDXTRK_MAX_TRIS, "%s: ntris=%d", name, t.ntris);
        CHECK(t.maxSlot <= GDXTRK_MAX_SLOT, "%s: maxSlot=%d", name, t.maxSlot);
        if (t.nevents != 0) {
            const GdxTrkCombineEvent* last = &t.events[t.nevents - 1];
            CHECK(t.nevents == 2, "%s: nevents=%d", name, t.nevents);
            CHECK(last->w0 == 0xFC147E28u && last->w1 == 0x44FE793Cu,
                  "%s: trailing SETCOMBINE %08X %08X is not the track combiner", name, last->w0, last->w1);
            CHECK(t.events[0].w0 == 0xFC357E6Au && t.events[0].w1 == 0x44FE793Cu,
                  "%s: leading SETCOMBINE %08X %08X unexpected", name, t.events[0].w0, t.events[0].w1);
            CHECK(t.events[0].beforeTri == 0, "%s: first event beforeTri=%d", name, t.events[0].beforeTri);
        }
        ByteSwapCopy(kGdxTrkRawTable[i].raw, kGdxTrkRawTable[i].len, swapped);
        rc = gdxtrk_decode_template(swapped, kGdxTrkRawTable[i].len, &tBig);
        CHECK(rc == GDXTRK_DECODE_OK, "%s: big-endian decode rc=%d", name, rc);
        CHECK(gdxtrk_template_equal(&tBig, &t), "%s: big-endian decode differs", name);
    }
    /* Decoder rejections. */
    {
        GdxTrkTemplate t;
        /* Little-endian words: the opcode is byte 3 of each w0 (G_VTX, then G_ENDDL). */
        unsigned char junk[16] = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0xDF, 0x00, 0x00, 0x00, 0x00 };
        CHECK(gdxtrk_decode_template(junk, sizeof(junk), &t) == GDXTRK_DECODE_UNSUPPORTED_OP, "junk opcode accepted");
        junk[11] = 0x00;
        CHECK(gdxtrk_decode_template(junk, sizeof(junk), &t) == GDXTRK_DECODE_NO_ENDDL, "missing ENDDL accepted");
        CHECK(gdxtrk_decode_template(junk, 7, &t) == GDXTRK_DECODE_BAD_ARGS, "odd length accepted");
    }
    if (sFailures == 0) {
        printf("=== ALL PASS (%d templates) ===\n", GDXTRK_TEMPLATE_COUNT);
        return 0;
    }
    printf("=== %d FAILURES ===\n", sFailures);
    return 1;
}
