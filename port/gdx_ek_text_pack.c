/* Expansion Kit English text pack -- the runtime half. See gdx_ek_text_pack.h
 * for why it exists; this file is only the apply loop.
 *
 * Deliberately the same shape as gdx_ek_strings.c's loop, and for the same
 * reasons: both bounds are generated so neither should ever trip, and both are
 * checked anyway because the failure mode -- writing past a .data array -- is
 * silent corruption that surfaces somewhere else entirely.
 */
#include "gdx_ek_text_pack.h"
#include "gen/EkTranslatedStrings.h"
#include "port_log.h"

int gdx_ek_text_pack_apply(void) {
    unsigned int i;
    unsigned int applied = 0;
    unsigned int skipped = 0;

    if (gEkTextPackRowCount == 0u) {
        return 0;
    }

    for (i = 0; i < gEkTextPackRowCount; i++) {
        const GdxEkTextPackRow* row = &gEkTextPackRows[i];
        unsigned char* dest = (unsigned char*) row->dest;
        const unsigned char* src = gEkTextPack + row->packOffset;
        unsigned int b;

        if (row->size > row->capacity) {
            gdx_port_logf("[ek-english] SKIP %s: %u bytes exceeds capacity %u\n", row->name,
                          row->size, row->capacity);
            skipped++;
            continue;
        }
        if (row->packOffset > gEkTextPackSize || row->size > gEkTextPackSize - row->packOffset) {
            /* A row that cannot be served is skipped whole rather than half-copied: a
               half-written texture is a screen of noise that looks like a decode bug. */
            gdx_port_logf("[ek-english] SKIP %s: pack offset %u+%u past the %u-byte pack\n",
                          row->name, row->packOffset, row->size, gEkTextPackSize);
            skipped++;
            continue;
        }

        for (b = 0; b < row->size; b++) {
            dest[b] = src[b];
        }
        if (row->zeroTail) {
            /* Clear the tail rather than leave the Japanese initializer's bytes after the
               new NUL -- gdx_ek_strings.c's reason, unchanged: a half-Japanese buffer
               resurfaces later as a stray glyph in a screenshot. */
            for (b = row->size; b < row->capacity; b++) {
                dest[b] = 0;
            }
        }
        applied++;
    }

    /* The three pointer tables carry no disk bytes at all: their English is already
       committed as string literals in port/gen/EkTranslatedStrings.c, because the text
       lives in anonymous literals with no named storage to copy into. Repointing them is
       the same loop gdx_ek_strings_apply() runs, shared rather than duplicated. */
    gdx_ek_strings_repoint_tables();

    gdx_port_logf("[ek-english] %u row(s) applied, %u skipped, %u bytes"
                  " (source CRC64=0x%016llX)\n",
                  applied, skipped, gEkTextPackSize, gEkTextPackSourceCrc64);
    return (int) applied;
}
