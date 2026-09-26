/* port/gdx_disk_dcrc.cpp -- see gdx_disk_dcrc.h for why this is its own translation unit. */
#include "gdx_disk_dcrc.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <direct.h>
#define gdx_dcrc_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define gdx_dcrc_mkdir(path) mkdir((path), 0755)
#endif

static const char* gdx_dcrc_leaf(const char* diskName) {
    const char* leaf = diskName;
    const char* slash = strrchr(diskName, '/');
    if (slash != nullptr) {
        leaf = slash + 1;
    }
    const char* back = strrchr(leaf, '\\');
    if (back != nullptr) {
        leaf = back + 1;
    }
    return leaf;
}

/* "<baseDir>saves" with NO trailing separator. The trailing separator matters: a 3DS FS
 * devoptab is not a host filesystem, and "mkdir(.../saves/)" is exactly the shape whose
 * behaviour differs between the two. Nothing in this file passes a directory path with one. */
static int gdx_dcrc_build_dir(const char* baseDir, char* out, unsigned int outSize) {
    if (out == nullptr || outSize == 0) {
        return 0;
    }
    const char* base = (baseDir != nullptr) ? baseDir : "";
    const int n = snprintf(out, outSize, "%ssaves", base);
    return (n > 0 && (unsigned int) n < outSize) ? 1 : 0;
}

int gdx_dcrc_build_path(const char* baseDir, const char* diskName, char* out, unsigned int outSize) {
    if (out == nullptr || outSize == 0 || diskName == nullptr) {
        return 0;
    }
    out[0] = '\0';
    const char* base = (baseDir != nullptr) ? baseDir : "";
    const int n = snprintf(out, outSize, "%ssaves/%s.dcrc", base, gdx_dcrc_leaf(diskName));
    if (n <= 0 || (unsigned int) n >= outSize) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

GdxDcrcLoadResult gdx_dcrc_load(const char* baseDir, const char* diskName, unsigned int size,
                                unsigned long long sample, unsigned long long* outCrc) {
    /* A zero sample is the "this boot could not sample the image" sentinel. Accepting a
     * cached fingerprint without validating it is the one thing this cache must never do:
     * the region gate is deliberately keyed on identity rather than on a filename, and a
     * blindly trusted sidecar would hand a renamed image a way past it. */
    if (sample == 0ull) {
        return GDX_DCRC_MISS_NOSAMPLE;
    }
    char path[1200];
    if (!gdx_dcrc_build_path(baseDir, diskName, path, sizeof(path))) {
        return GDX_DCRC_MISS_NOPATH;
    }
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return (errno == ENOENT) ? GDX_DCRC_MISS_ABSENT : GDX_DCRC_MISS_UNREADABLE;
    }
    char magic[GDX_DCRC_MAGIC_BYTES + 1] = {};
    unsigned int storedSize = 0;
    unsigned long long storedSample = 0, storedCrc = 0;
    const bool read = fread(magic, 1, GDX_DCRC_MAGIC_BYTES, f) == GDX_DCRC_MAGIC_BYTES &&
                      fread(&storedSize, sizeof(storedSize), 1, f) == 1 &&
                      fread(&storedSample, sizeof(storedSample), 1, f) == 1 &&
                      fread(&storedCrc, sizeof(storedCrc), 1, f) == 1;
    const bool err = ferror(f) != 0;
    fclose(f);
    if (!read) {
        /* Short reads and I/O errors are different diagnoses: a short file is a torn write
         * (recoverable, and this boot rewrites it), an error is a card problem. */
        return err ? GDX_DCRC_MISS_UNREADABLE : GDX_DCRC_MISS_SHORT;
    }
    if (memcmp(magic, GDX_DCRC_MAGIC, GDX_DCRC_MAGIC_BYTES) != 0) {
        return GDX_DCRC_MISS_MAGIC;
    }
    if (storedSize != size) {
        return GDX_DCRC_MISS_SIZE;
    }
    if (storedSample != sample) {
        return GDX_DCRC_MISS_SAMPLE;
    }
    if (outCrc != nullptr) {
        *outCrc = storedCrc;
    }
    return GDX_DCRC_HIT;
}

GdxDcrcStoreResult gdx_dcrc_store(const char* baseDir, const char* diskName, unsigned int size,
                                  unsigned long long sample, unsigned long long crc, int* outErrno) {
    if (outErrno != nullptr) {
        *outErrno = 0;
    }
    if (sample == 0ull) {
        return GDX_DCRC_WRITE_SKIPPED;
    }
    char path[1200];
    char dir[1200];
    if (!gdx_dcrc_build_path(baseDir, diskName, path, sizeof(path)) ||
        !gdx_dcrc_build_dir(baseDir, dir, sizeof(dir))) {
        return GDX_DCRC_WRITE_NOPATH;
    }

    /* Create the directory rather than assuming somebody else did. The original code did
     * assume it -- "disk_savefile.cpp creates it on its own first flush" -- and that flush
     * only happens after a 64DD WRITE, so on a card where saves/ did not yet exist the
     * sidecar could not be written at the mount, was never retried, and every boot paid the
     * full 61.9 MB pass with no receipt saying so. EEXIST is the expected answer. */
    errno = 0;
    const int madeDir = gdx_dcrc_mkdir(dir);
    const int mkdirErrno = (madeDir == 0) ? 0 : errno;

    errno = 0;
    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        const int openErrno = errno;
        /* Separate "the directory is not there and could not be made" from "the file could
         * not be opened for some other reason" -- they are different things to go and check
         * on the card, and the receipt is the only place anyone will read either of them. */
        if (openErrno == ENOENT && madeDir != 0 && mkdirErrno != EEXIST) {
            if (outErrno != nullptr) {
                *outErrno = mkdirErrno;
            }
            return GDX_DCRC_WRITE_NODIR;
        }
        if (outErrno != nullptr) {
            *outErrno = openErrno;
        }
        return GDX_DCRC_WRITE_OPEN;
    }
    bool ok = fwrite(GDX_DCRC_MAGIC, 1, GDX_DCRC_MAGIC_BYTES, f) == GDX_DCRC_MAGIC_BYTES;
    ok = (fwrite(&size, sizeof(size), 1, f) == 1) && ok;
    ok = (fwrite(&sample, sizeof(sample), 1, f) == 1) && ok;
    ok = (fwrite(&crc, sizeof(crc), 1, f) == 1) && ok;
    /* fflush and fclose both report, and on a 3DS SD card the write that fails is far more
     * likely to fail at the flush than at the fwrite -- newlib buffers a 25-byte record
     * entirely, so fwrite alone proves nothing. */
    if (fflush(f) != 0) {
        ok = false;
    }
    const int savedErrno = errno;
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        if (outErrno != nullptr) {
            *outErrno = savedErrno;
        }
        /* A partial record would be re-read next boot as SHORT and recomputed, which is
         * correct but leaves a permanent lie on the card. Remove it. */
        remove(path);
        return GDX_DCRC_WRITE_IO;
    }
    return GDX_DCRC_WRITE_OK;
}

const char* gdx_dcrc_load_text(GdxDcrcLoadResult result) {
    switch (result) {
        case GDX_DCRC_HIT:
            return "hit";
        case GDX_DCRC_MISS_ABSENT:
            return "absent";
        case GDX_DCRC_MISS_UNREADABLE:
            return "unreadable";
        case GDX_DCRC_MISS_SHORT:
            return "truncated";
        case GDX_DCRC_MISS_MAGIC:
            return "not-a-dcrc";
        case GDX_DCRC_MISS_SIZE:
            return "size-mismatch";
        case GDX_DCRC_MISS_SAMPLE:
            return "sample-mismatch";
        case GDX_DCRC_MISS_NOSAMPLE:
            return "no-sample";
        case GDX_DCRC_MISS_NOPATH:
            return "no-path";
    }
    return "unknown";
}

const char* gdx_dcrc_store_text(GdxDcrcStoreResult result) {
    switch (result) {
        case GDX_DCRC_WRITE_OK:
            return "ok";
        case GDX_DCRC_WRITE_SKIPPED:
            return "skipped";
        case GDX_DCRC_WRITE_NOPATH:
            return "FAILED-no-path";
        case GDX_DCRC_WRITE_NODIR:
            return "FAILED-no-dir";
        case GDX_DCRC_WRITE_OPEN:
            return "FAILED-open";
        case GDX_DCRC_WRITE_IO:
            return "FAILED-io";
    }
    return "FAILED-unknown";
}
