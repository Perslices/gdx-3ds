/* Expansion Kit ENGLISH TEXT PACK (B11m) -- the build-time source of translated
 * words, for a player whose own 64DD dump is the retail Japanese one.
 *
 * The port already binds English art and text when the MOUNTED image is the
 * fan-translated release (port/gen/EkTranslatedOverrides.c,
 * port/gen/EkTranslatedStrings.c, applied by gdx_disk_finalize). That seam is
 * dormant for everyone else, for two measured reasons: its gate is an
 * EXACT-IMAGE CRC-64, which nddconv (B11l) showed two dumps of two physical
 * discs can never share; and it reads the English out of the mounted disk, which
 * a retail-JP owner does not have.
 *
 * This pack supplies the bytes instead, so the two halves separate cleanly:
 *
 *   the user's own disk  -> drive presence, the MFS volume and its saves, the DD
 *                           cups, courses, audio. Authoritative, never overridden.
 *   the pack             -> the words. 104 texture rows plus the overlay .data
 *                           text, i.e. exactly the rows tools/gen_ek_text_pack.py
 *                           MEASURED to differ between the two disks and to be
 *                           text-bearing.
 *
 * The data half (port/gen/EkTranslatedTextPack.c) is generated and .gitignore'd,
 * because it is disk bytes. It is EMPTY unless the generator was run with a
 * translated image, in which case gEkTextPackRowCount is 0 and
 * gdx_ek_text_pack_apply() returns 0 without touching anything -- byte-for-byte
 * the behaviour this build had before the pack existed.
 */
#ifndef GDX_EK_TEXT_PACK_H
#define GDX_EK_TEXT_PACK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* name;         /* symbol name, for the diagnostic receipt only */
    void* dest;               /* the array the bytes are copied into */
    unsigned int packOffset;  /* byte offset into gEkTextPack */
    unsigned int size;        /* bytes to copy */
    unsigned int capacity;    /* dest's real declared byte budget (sizeof) */
    unsigned char zeroTail;   /* 1 = clear dest[size..capacity), for NUL-terminated text */
} GdxEkTextPackRow;

extern const unsigned char gEkTextPack[];
extern const unsigned int gEkTextPackSize;
extern const unsigned long long gEkTextPackSourceCrc64;
extern const GdxEkTextPackRow gEkTextPackRows[];
extern const unsigned int gEkTextPackRowCount;

/* Copies every pack row over what the disk fill just wrote, and repoints the
 * three pointer tables in port/gen/EkTranslatedStrings.c. Returns the number of
 * rows applied, 0 when the pack is empty. Idempotent: re-applying writes the
 * same bytes.
 *
 * MUST be called after gdx_ek_assets_fill() -- it overwrites the fill -- and
 * BEFORE gdx_disk_finalize's D_80769DF0 byte-swap, which the pack's own copy of
 * that texture would otherwise be written over the top of in the wrong order.
 */
int gdx_ek_text_pack_apply(void);

/* Repoints the three char* tables in port/gen/EkTranslatedStrings.c at their
 * committed English literals. Defined in port/gdx_ek_strings.c, which owns the
 * one cast the three differently-declared arrays force, and declared here
 * because this header is the committed one both appliers share. */
void gdx_ek_strings_repoint_tables(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_EK_TEXT_PACK_H */
