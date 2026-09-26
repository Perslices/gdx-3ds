/* port/gdx_o2r_stream.cpp -- implementation. See gdx_o2r_stream.h for the why.
 *
 * The zip locator is deliberately the smallest thing that can be correct for this one job:
 * find the End Of Central Directory record, walk the central directory, match one name,
 * check method == 0 (STORED) and the two sizes, then read that entry's LOCAL header to learn
 * the true data offset (the local header's extra field is allowed to differ in length from
 * the central one -- assuming otherwise is the classic zip-parser bug, and it would land the
 * window a few bytes off and serve every sample shifted, which is exactly the silent audio
 * corruption this track exists to avoid).
 *
 * Zip64 is REFUSED rather than handled: an .o2r that needs it is >4 GiB or has >65535
 * entries, neither of which this port produces, and a half-implemented zip64 path is worse
 * than none.
 */
#include "gdx_o2r_stream.h"

#include "gdx_block_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(GDX_PLATFORM_3DS)
#include <3ds.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace {

GdxBlockCache s_cache = {};
const char* s_refusal = "not-attempted";
char s_source[512] = {};
unsigned int s_dataOffset = 0;
unsigned int s_payloadBytes = 0;

#if defined(GDX_PLATFORM_3DS)
LightLock s_lock;
bool s_lockReady = false;
void lockInit() {
    if (!s_lockReady) {
        LightLock_Init(&s_lock);
        s_lockReady = true;
    }
}
void lockTake() { lockInit(); LightLock_Lock(&s_lock); }
void lockDrop() { LightLock_Unlock(&s_lock); }
#elif defined(_WIN32)
SRWLOCK s_lock = SRWLOCK_INIT;
void lockTake() { AcquireSRWLockExclusive(&s_lock); }
void lockDrop() { ReleaseSRWLockExclusive(&s_lock); }
#else
pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
void lockTake() { pthread_mutex_lock(&s_lock); }
void lockDrop() { pthread_mutex_unlock(&s_lock); }
#endif

unsigned int rd16(const unsigned char* p) {
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8);
}
unsigned int rd32(const unsigned char* p) {
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8) | ((unsigned int) p[2] << 16) |
           ((unsigned int) p[3] << 24);
}

/* Locates `entryKey` in the zip at `path`. On success writes the absolute file offset of the
 * entry's DATA and its stored size, and returns nullptr. On failure returns the refusal
 * reason. `outFileSize` always gets the file size when the file could be opened. */
const char* locateStoredEntry(const char* path, const char* entryKey, unsigned int* outDataOff,
                              unsigned int* outSize, unsigned int* outFileSize) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return "open-failed";
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return "open-failed";
    }
    const long fsz = ftell(f);
    if (fsz <= 22) {
        fclose(f);
        return "open-failed";
    }
    *outFileSize = (unsigned int) fsz;

    /* EOCD: scan backwards over at most 64 KiB + 22 for the signature. */
    unsigned int tailWant = 66u * 1024u;
    if ((unsigned long) tailWant > (unsigned long) fsz) {
        tailWant = (unsigned int) fsz;
    }
    /* malloc'd, not a file static: this is 66 KiB and the lever is off by default, so the
     * shipped build must not carry it in .bss (census row 3 is .bss, 7,937,496 B). Freed
     * before this function returns on every path. */
    unsigned char* tail = (unsigned char*) malloc(tailWant);
    if (tail == nullptr) {
        fclose(f);
        return "open-failed";
    }
    if (fseek(f, fsz - (long) tailWant, SEEK_SET) != 0 ||
        fread(tail, 1, tailWant, f) != tailWant) {
        free(tail);
        fclose(f);
        return "open-failed";
    }
    long eocd = -1;
    for (long i = (long) tailWant - 22; i >= 0; i--) {
        if (rd32(tail + i) == 0x06054B50u) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        free(tail);
        fclose(f);
        return "no-entry";
    }
    const unsigned int entries = rd16(tail + eocd + 10);
    const unsigned int cdSize = rd32(tail + eocd + 12);
    const unsigned int cdOff = rd32(tail + eocd + 16);
    free(tail);
    if (entries == 0xFFFFu || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) {
        fclose(f);
        return "not-stored"; /* zip64: refused, see the file header */
    }
    if ((unsigned long long) cdOff + cdSize > (unsigned long long) fsz || cdSize == 0) {
        fclose(f);
        return "no-entry";
    }

    /* Walk the central directory in bounded chunks rather than holding it all: an .o2r has
     * thousands of entries and this runs before the heap has anything to spare. */
    const unsigned int keyLen = (unsigned int) strlen(entryKey);
    unsigned char hdr[46];
    char name[256];
    unsigned int pos = cdOff;
    const unsigned int cdEnd = cdOff + cdSize;
    const char* result = "no-entry";
    unsigned int localOff = 0, storedSize = 0;
    for (unsigned int i = 0; i < entries && pos + 46u <= cdEnd; i++) {
        if (fseek(f, (long) pos, SEEK_SET) != 0 || fread(hdr, 1, 46, f) != 46) {
            result = "no-entry";
            break;
        }
        if (rd32(hdr) != 0x02014B50u) {
            result = "no-entry";
            break;
        }
        const unsigned int method = rd16(hdr + 10);
        const unsigned int compSize = rd32(hdr + 20);
        const unsigned int uncompSize = rd32(hdr + 24);
        const unsigned int nameLen = rd16(hdr + 28);
        const unsigned int extraLen = rd16(hdr + 30);
        const unsigned int commentLen = rd16(hdr + 32);
        const unsigned int lho = rd32(hdr + 42);
        pos += 46u + nameLen + extraLen + commentLen;
        if (nameLen != keyLen || nameLen >= sizeof(name)) {
            continue;
        }
        if (fread(name, 1, nameLen, f) != nameLen) {
            result = "no-entry";
            break;
        }
        if (memcmp(name, entryKey, nameLen) != 0) {
            continue;
        }
        /* Found the name. From here every exit is a decision about THIS entry. */
        if (method != 0u) {
            result = "not-stored";
            break;
        }
        if (compSize != uncompSize || compSize == 0u) {
            result = "not-stored";
            break;
        }
        localOff = lho;
        storedSize = compSize;
        result = nullptr;
        break;
    }
    if (result != nullptr) {
        fclose(f);
        return result;
    }

    /* The LOCAL header's extra field may differ in length from the central one. Read it. */
    unsigned char lhdr[30];
    if (fseek(f, (long) localOff, SEEK_SET) != 0 || fread(lhdr, 1, 30, f) != 30 ||
        rd32(lhdr) != 0x04034B50u) {
        fclose(f);
        return "no-entry";
    }
    const unsigned int lNameLen = rd16(lhdr + 26);
    const unsigned int lExtraLen = rd16(lhdr + 28);
    const unsigned long long dataOff = (unsigned long long) localOff + 30u + lNameLen + lExtraLen;
    if (dataOff + storedSize > (unsigned long long) fsz) {
        fclose(f);
        return "size-mismatch";
    }
    fclose(f);
    *outDataOff = (unsigned int) dataOff;
    *outSize = storedSize;
    return nullptr;
}

} // namespace

extern "C" {

int gdx_o2r_stream_open(const char* const* paths, unsigned int pathCount, const char* entryKey,
                        unsigned int expectedEntryBytes, unsigned int payloadOffset,
                        unsigned int blockLog2, unsigned int cacheBlocks) {
    gdx_o2r_stream_close();
    s_refusal = "no-path";
    if (paths == nullptr || pathCount == 0u || entryKey == nullptr || entryKey[0] == '\0') {
        return 0;
    }
    for (unsigned int i = 0; i < pathCount; i++) {
        if (paths[i] == nullptr || paths[i][0] == '\0') {
            continue;
        }
        unsigned int dataOff = 0, size = 0, fileSize = 0;
        const char* why = locateStoredEntry(paths[i], entryKey, &dataOff, &size, &fileSize);
        if (why != nullptr) {
            /* "no-entry" on an archive that simply does not carry this key is not a failure
             * of the run; keep looking. Anything else is about THIS entry and is reported. */
            if (strcmp(why, "no-entry") != 0) {
                s_refusal = why;
            } else if (strcmp(s_refusal, "no-path") == 0) {
                s_refusal = "no-entry";
            }
            continue;
        }
        if (expectedEntryBytes != 0u && size != expectedEntryBytes) {
            /* The generated table and the archive disagree about this entry. Refuse: serving
             * a differently-sized blob through a window sized from the table is how every
             * sample ends up shifted by a constant, which is inaudible as a crash and very
             * audible as noise. */
            s_refusal = "size-mismatch";
            continue;
        }
        if (payloadOffset >= size) {
            s_refusal = "size-mismatch";
            continue;
        }
        const unsigned int payloadBytes = size - payloadOffset;
        if (!gdx_block_cache_open(&s_cache, paths[i], dataOff + payloadOffset, payloadBytes,
                                  fileSize, blockLog2, cacheBlocks)) {
            s_refusal = "cache-failed";
            continue;
        }
        snprintf(s_source, sizeof(s_source), "%s", paths[i]);
        s_dataOffset = dataOff + payloadOffset;
        s_payloadBytes = payloadBytes;
        s_refusal = "ok";
        return 1;
    }
    return 0;
}

void gdx_o2r_stream_close(void) {
    gdx_block_cache_close(&s_cache);
    s_source[0] = '\0';
    s_dataOffset = 0;
    s_payloadBytes = 0;
    s_refusal = "not-attempted";
}

int gdx_o2r_stream_active(void) {
    return gdx_block_cache_active(&s_cache);
}

const char* gdx_o2r_stream_refusal(void) {
    return s_refusal;
}

const char* gdx_o2r_stream_source(void) {
    return s_source;
}

unsigned int gdx_o2r_stream_data_offset(void) {
    return s_dataOffset;
}

unsigned int gdx_o2r_stream_payload_bytes(void) {
    return s_payloadBytes;
}

unsigned int gdx_o2r_stream_cache_bytes(void) {
    return gdx_block_cache_bytes(&s_cache);
}

int gdx_o2r_stream_read(unsigned int offset, unsigned int length, void* dst) {
    if (!gdx_block_cache_active(&s_cache)) {
        return 0;
    }
    /* Payload-relative in, absolute-file out. An offset that would wrap is refused rather
     * than wrapped: the cache would then read a legal-looking window of the wrong file. */
    if ((unsigned long long) s_dataOffset + offset + length > 0xFFFFFFFFull) {
        gdx_block_cache_note_refusal(&s_cache);
        return 0;
    }
    lockTake();
    const int ok = gdx_block_cache_read(&s_cache, s_dataOffset + offset, length, dst);
    lockDrop();
    return ok;
}

const char* gdx_o2r_stream_format_stats(char* buf, unsigned int bufSize) {
    if (buf == nullptr || bufSize == 0u) {
        return "";
    }
    char body[256];
    lockTake();
    gdx_block_cache_format_stats(&s_cache, body, sizeof(body));
    const unsigned int off = s_dataOffset;
    const unsigned int bytes = s_payloadBytes;
    lockDrop();
    snprintf(buf, bufSize, "%s dataOff=%u payload=%u src=%s", body, off, bytes, s_source);
    return buf;
}

} // extern "C"
