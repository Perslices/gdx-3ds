/* port/gdx_disk_dcrc.h -- the `.dcrc` sidecar: one cached 64DD identity fingerprint.
 *
 * WHY THIS FILE EXISTS. Mounting a 64DD image costs a whole-image CRC-64/XZ pass, and the
 * answer is an IDENTITY -- "which disk is this?" -- shared by three consumers (the
 * Japanese-region gate, the disk-layout variant decision, and the `.gdd` save sidecar's
 * source key). `docs/research/native/diskstream-progress.md` section 3 decided to compute it
 * once and cache it across boots in a `<leaf>.dcrc` sidecar validated by a positional
 * 64 x 4 KiB sample, so that later boots read 256 KiB instead of 61.9 MB. On a New 3DS
 * streaming off an SD card that pass is the whole of a ~1 minute boot.
 *
 * WHY IT IS ITS OWN TRANSLATION UNIT. It used to be four static functions in
 * port/disk_buffer.cpp, which needs decomp headers and cannot be compiled for the host -- so
 * the one feature whose failure mode is "silently does nothing, forever" had no test. This
 * unit has no decomp headers, no 3DS headers and no libultraship, exactly like
 * gdx_disk_stream.cpp, which is what lets port/tests/gdx_disk_stream_tests.cpp gate it.
 *
 * EVERY FAILURE IS NAMED, and that is the point of the enums below. The first hardware boot
 * with a real disk reported `crc64=0x...(computed)` and nothing else, and `(computed)` alone
 * cannot distinguish "no sidecar existed" (correct, on a first boot) from "the sidecar was
 * rejected by the sample check" (a bug) from "the write FAILED" (a bug that makes every
 * future boot pay the full pass). The caller prints these names in the `[diskstream]`
 * receipt, so the next boot's log answers the question by itself.
 *
 * THE RECORD IS UNCHANGED from the original implementation -- magic "GDXC1", u32 size,
 * u64 sample, u64 crc, native byte order, 25 bytes -- so a sidecar written by an older build
 * still validates.
 */
#ifndef GDX_DISK_DCRC_H
#define GDX_DISK_DCRC_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_DCRC_MAGIC "GDXC1"
#define GDX_DCRC_MAGIC_BYTES 5u
#define GDX_DCRC_RECORD_BYTES 25u

/* Why a lookup did not produce a usable fingerprint. GDX_DCRC_HIT is the only value that
 * skips the whole-image pass; every other value is printed in the receipt so the reason a
 * boot paid 61.9 MB is never a guess. */
typedef enum {
    GDX_DCRC_HIT = 0,          /* size AND sample matched: the full pass is skipped */
    GDX_DCRC_MISS_ABSENT,      /* no sidecar at the path -- the normal first-boot answer */
    GDX_DCRC_MISS_UNREADABLE,  /* present, but could not be opened or read */
    GDX_DCRC_MISS_SHORT,       /* truncated: fewer than GDX_DCRC_RECORD_BYTES bytes */
    GDX_DCRC_MISS_MAGIC,       /* not a .dcrc record at all */
    GDX_DCRC_MISS_SIZE,        /* recorded against an image of a different size */
    GDX_DCRC_MISS_SAMPLE,      /* same size, different bytes -- the validation that matters */
    GDX_DCRC_MISS_NOSAMPLE,    /* this boot could not sample the image; the cache is unusable */
    GDX_DCRC_MISS_NOPATH       /* the sidecar path could not be built (buffer too small) */
} GdxDcrcLoadResult;

typedef enum {
    GDX_DCRC_WRITE_OK = 0,
    GDX_DCRC_WRITE_SKIPPED,    /* no sample to validate against, so a record would be a lie */
    GDX_DCRC_WRITE_NOPATH,
    GDX_DCRC_WRITE_NODIR,      /* the saves directory is absent and could not be created */
    GDX_DCRC_WRITE_OPEN,       /* fopen failed -- errno says why */
    GDX_DCRC_WRITE_IO          /* fwrite/fflush/fclose failed; the partial file is removed */
} GdxDcrcStoreResult;

/* "<baseDir>saves/<leaf of diskName>.dcrc". baseDir carries its own trailing separator (the
 * port's gdx_exe_dir contract) or is empty. Returns 1 on success, 0 if it would not fit. */
int gdx_dcrc_build_path(const char* baseDir, const char* diskName, char* out, unsigned int outSize);

/* Reads the cached fingerprint. *outCrc is written ONLY on GDX_DCRC_HIT. */
GdxDcrcLoadResult gdx_dcrc_load(const char* baseDir, const char* diskName, unsigned int size,
                               unsigned long long sample, unsigned long long* outCrc);

/* Writes the record, creating "<baseDir>saves" if it is missing. Nothing here is required for
 * correctness -- a failure costs a re-computation, never a wrong answer -- but a failure that
 * is not reported costs it on EVERY boot, which is what this returns a reason for.
 * *outErrno, when non-null, receives errno for the OPEN and IO results. */
GdxDcrcStoreResult gdx_dcrc_store(const char* baseDir, const char* diskName, unsigned int size,
                                 unsigned long long sample, unsigned long long crc, int* outErrno);

/* Short, stable, grep-able names for the receipt. Never null. */
const char* gdx_dcrc_load_text(GdxDcrcLoadResult result);
const char* gdx_dcrc_store_text(GdxDcrcStoreResult result);

#ifdef __cplusplus
}
#endif

#endif /* GDX_DISK_DCRC_H */
