/* Expansion Kit text binding: put the loaded 64DD disk's own words on screen.
 *
 * The EK's UI text is not a yaml asset -- it is compiled into the decompiled
 * overlay sources as C initializers, so port/gen/EkAssetBindings.c's slice table
 * has no entry for it and gdx_ek_assets_fill() never touches it. That is why the
 * port rendered Japanese even with the fan-translated disk loaded: the disk does
 * carry the English, in the same overlay .data sections, at addresses recovered
 * in tools/gen_ek_translated_strings.py. This file is the runtime half.
 *
 * Translating the decomp sources instead would make the port assert a translation
 * rather than show the disk's. Bound this way the disk is the single source of
 * truth for the text, so a retail-JP disk still renders Japanese through the same
 * code path with nothing to keep in sync.
 *
 * Two shapes, handled differently:
 *
 *   gEkTranslatedStrings[]      Named arrays in overlay .data. Real storage
 *                               exists, so the disk's bytes are copied into it,
 *                               the way gEkTranslatedOverrides[] re-copies the
 *                               reshaped label textures.
 *
 *   gEkTranslatedStringTables[] Arrays of char*. No named storage to copy into --
 *                               the text lives in anonymous string literals -- so
 *                               the generator bakes the disk's text into
 *                               port/gen/EkTranslatedStrings.c (each entry carries
 *                               the disk offset it came from) and this file
 *                               repoints the array. The caller's CRC64 gate is
 *                               what makes that equivalent: the table is only ever
 *                               applied to the exact disk image those bytes were
 *                               read from.
 *
 * Called once per disk load from gdx_disk_finalize (port/disk_buffer.cpp), and
 * only on the translated variant. Idempotent: re-applying writes the same bytes.
 */
#include "gen/EkTranslatedStrings.h"
#include "gdx_ek_text_pack.h"
#include "port_log.h"

/* B11m -- the English text pack puts the SAME three tables on screen with no
 * translated disk mounted, so the repoint loop is shared rather than copied.
 * Declared in gdx_ek_text_pack.h; defined here because the cast it needs is this
 * file's, and duplicating that cast is how the two copies drift. */
void gdx_ek_strings_repoint_tables(void) {
    unsigned int i;

    for (i = 0; i < gEkTranslatedStringTableCount; i++) {
        const GdxEkTranslatedStringTable* tbl = &gEkTranslatedStringTables[i];
        /* The three arrays are not declared alike in the decomp -- two char*, one
         * const char* -- so the table carries dest as void* and the single cast
         * happens here. Entries are only ever read (drawn), so handing a char* slot
         * a pointer to a string literal is safe; an extern that misstated one
         * array's type to make the assignment look tidy would not be. */
        const char** dest = (const char**) tbl->dest;
        unsigned int e;

        for (e = 0; e < tbl->count; e++) {
            dest[e] = tbl->text[e];
        }
    }
}

/* B11d -- the residency seam. `disk == 0` no longer means "no image": in [debug]
 * disk_stream=2 the image is mounted as an SD stream with no resident buffer. */
extern int gdx_disk_image_read(unsigned int offset, unsigned int length, void* dst);
extern int gdx_disk_image_available(void);
extern unsigned int gdx_disk_image_size(void);

void gdx_ek_strings_apply(const unsigned char* disk, unsigned long long diskSize) {
    unsigned int i;
    unsigned int copied = 0;
    unsigned int repointed = 0;

    if (disk == 0) {
        if (!gdx_disk_image_available()) {
            return;
        }
        diskSize = (unsigned long long) gdx_disk_image_size();
    }

    for (i = 0; i < gEkTranslatedStringCount; i++) {
        const GdxEkTranslatedString* row = &gEkTranslatedStrings[i];
        unsigned char* dest = (unsigned char*) row->dest;
        const unsigned char* src = (disk != 0) ? disk + row->diskOffset : 0;
        unsigned int b;

        /* Both bounds are generated, so neither should ever trip. Checked anyway
         * because the failure mode -- writing past a .data array, or reading past
         * the disk image -- is silent corruption that surfaces elsewhere. */
        if (row->size > row->capacity) {
            gdx_port_logf("[ek-strings] SKIP %s: %u bytes exceeds capacity %u\n", row->name,
                          row->size, row->capacity);
            continue;
        }
        if ((unsigned long long) row->diskOffset + row->size > diskSize) {
            gdx_port_logf("[ek-strings] SKIP %s: disk offset 0x%08X+%u past end of image\n",
                          row->name, row->diskOffset, row->size);
            continue;
        }

        if (src != 0) {
            for (b = 0; b < row->size; b++) {
                dest[b] = src[b];
            }
        } else if (!gdx_disk_image_read(row->diskOffset, row->size, dest)) {
            /* Refused reads leave `dest` as the compiled-in Japanese initializer rather than
               half-English, which is the readable failure. */
            gdx_port_logf("[ek-strings] SKIP %s: streamed read refused at 0x%08X+%u\n",
                          row->name, row->diskOffset, row->size);
            continue;
        }
        /* Clear the tail rather than leave the Japanese initializer's bytes after
         * the new NUL. Nothing reads past a terminator today, but a half-Japanese
         * buffer resurfaces later as a stray glyph in a screenshot. */
        for (b = row->size; b < row->capacity; b++) {
            dest[b] = 0;
        }
        copied++;
    }

    gdx_ek_strings_repoint_tables();
    for (i = 0; i < gEkTranslatedStringTableCount; i++) {
        repointed += gEkTranslatedStringTables[i].count;
    }

    gdx_port_logf("[ek-strings] bound %u array string(s) and repointed %u table entry(s)"
                  " from the translated disk\n",
                  copied, repointed);
}
