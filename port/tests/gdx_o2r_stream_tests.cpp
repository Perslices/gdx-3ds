/* port/tests/gdx_o2r_stream_tests.cpp -- host gate for C4d's byte source.
 *
 * WHAT THIS GATES, and it is the narrow but load-bearing part of the audio diet: that the
 * bytes the streamed path serves for `audio_blob/audio_table` are the SAME bytes the resident
 * path served, for every offset and length the audio driver can ask for, and that every way
 * the archive can be wrong is a REFUSAL rather than a plausible-looking wrong window.
 *
 * Why that matters more here than for a texture: a window landed a few bytes off does not
 * crash and does not change one pixel. It changes the sound. `covfinish` did not add an audio
 * census family precisely because "a SHOT is a framebuffer hash; an audio feature could be
 * recorded but never SAMPLED" -- so nothing downstream of this file can catch a constant
 * offset error. The zip locator therefore reads the LOCAL header for the data offset instead
 * of assuming the central directory's extra field length, and case 6 is built to fail if it
 * ever stops doing that.
 *
 * No archive, no ROM, no dump: every zip here is written by the test, byte by byte.
 */
#include "../gdx_block_cache.h"
#include "../gdx_o2r_stream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int gChecks = 0;
static int gFailures = 0;

static void check(bool ok, const char* what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what);
    }
}

/* ---------------------------------------------------------------- synthetic archive ---- */

static void put16(std::vector<unsigned char>& v, unsigned int x) {
    v.push_back((unsigned char) (x & 0xFF));
    v.push_back((unsigned char) ((x >> 8) & 0xFF));
}
static void put32(std::vector<unsigned char>& v, unsigned int x) {
    for (int i = 0; i < 4; i++) {
        v.push_back((unsigned char) ((x >> (8 * i)) & 0xFF));
    }
}

struct Entry {
    std::string name;
    std::vector<unsigned char> data;
    unsigned int method;      /* 0 stored, 8 "deflated" (contents are not really deflated;
                               * the locator must refuse on the METHOD, without decoding) */
    unsigned int localExtra;  /* extra-field bytes in the LOCAL header only */
    unsigned int centralExtra;/* extra-field bytes in the CENTRAL header only */
    unsigned int declaredUncomp; /* 0 => same as data.size() */
};

/* Writes a zip whose local and central extra fields deliberately DIFFER in length, so a
 * locator that computes the data offset from the central header lands in the wrong place. */
static bool writeZip(const std::string& path, const std::vector<Entry>& entries) {
    std::vector<unsigned char> out;
    std::vector<unsigned int> localOffsets;
    for (const Entry& e : entries) {
        localOffsets.push_back((unsigned int) out.size());
        put32(out, 0x04034B50u);
        put16(out, 20);             /* version needed */
        put16(out, 0);              /* flags */
        put16(out, e.method);
        put16(out, 0);              /* mod time */
        put16(out, 0);              /* mod date */
        put32(out, 0);              /* crc32 (not checked by the locator) */
        put32(out, (unsigned int) e.data.size());
        put32(out, e.declaredUncomp ? e.declaredUncomp : (unsigned int) e.data.size());
        put16(out, (unsigned int) e.name.size());
        put16(out, e.localExtra);
        out.insert(out.end(), e.name.begin(), e.name.end());
        for (unsigned int i = 0; i < e.localExtra; i++) {
            out.push_back((unsigned char) 0xAA);
        }
        out.insert(out.end(), e.data.begin(), e.data.end());
    }
    const unsigned int cdOff = (unsigned int) out.size();
    for (size_t i = 0; i < entries.size(); i++) {
        const Entry& e = entries[i];
        put32(out, 0x02014B50u);
        put16(out, 20);
        put16(out, 20);
        put16(out, 0);
        put16(out, e.method);
        put16(out, 0);
        put16(out, 0);
        put32(out, 0);
        put32(out, (unsigned int) e.data.size());
        put32(out, e.declaredUncomp ? e.declaredUncomp : (unsigned int) e.data.size());
        put16(out, (unsigned int) e.name.size());
        put16(out, e.centralExtra);
        put16(out, 0); /* comment len */
        put16(out, 0); /* disk start */
        put16(out, 0); /* internal attrs */
        put32(out, 0); /* external attrs */
        put32(out, localOffsets[i]);
        out.insert(out.end(), e.name.begin(), e.name.end());
        for (unsigned int k = 0; k < e.centralExtra; k++) {
            out.push_back((unsigned char) 0xBB);
        }
    }
    const unsigned int cdSize = (unsigned int) out.size() - cdOff;
    put32(out, 0x06054B50u);
    put16(out, 0);
    put16(out, 0);
    put16(out, (unsigned int) entries.size());
    put16(out, (unsigned int) entries.size());
    put32(out, cdSize);
    put32(out, cdOff);
    put16(out, 0);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

/* The payload the "audio_table" entry carries: 0x40 header + u32 size + deterministic body,
 * exactly the framing gdx_segment_source.c expects (GDX_ARCHIVE_PAYLOAD_OFFSET = 0x44). */
static unsigned char bodyByte(unsigned int i) {
    unsigned int x = i * 2654435761u + 0x9E3779B9u;
    x ^= x >> 13;
    return (unsigned char) (x & 0xFF);
}

static std::vector<unsigned char> makeEntry(unsigned int payloadBytes) {
    std::vector<unsigned char> v;
    v.resize(0x44u + payloadBytes, 0);
    v[0] = 'O'; v[1] = '2'; v[2] = 'R'; v[3] = 0;
    for (int i = 0; i < 4; i++) {
        v[0x40 + i] = (unsigned char) ((payloadBytes >> (8 * i)) & 0xFF);
    }
    for (unsigned int i = 0; i < payloadBytes; i++) {
        v[0x44u + i] = bodyByte(i);
    }
    return v;
}

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : ".";
    const std::string good = dir + "/o2rtest-good.zip";
    const std::string deflated = dir + "/o2rtest-deflated.zip";
    const std::string other = dir + "/o2rtest-other.zip";
    const char* kKey = "audio_blob/audio_table";
    /* Not a round number and not a multiple of any block size under test, so a tail block is
     * always partial -- the case a reader gets wrong quietly. */
    const unsigned int kPayload = 300000u + 777u;

    std::vector<unsigned char> entry = makeEntry(kPayload);
    const unsigned int kEntryBytes = (unsigned int) entry.size();

    {
        std::vector<Entry> es;
        /* A decoy whose name SHARES A PREFIX with the key: a locator that compares with
         * strncmp or stops at the first prefix match takes this one. */
        Entry decoy; decoy.name = "audio_blob/audio_table_extra"; decoy.method = 0;
        decoy.localExtra = 0; decoy.centralExtra = 0; decoy.declaredUncomp = 0;
        decoy.data.assign(64, 0x5A);
        es.push_back(decoy);
        Entry pad; pad.name = "segment_blob/hud_gfx"; pad.method = 8;
        pad.localExtra = 4; pad.centralExtra = 0; pad.declaredUncomp = 999;
        pad.data.assign(128, 0x11);
        es.push_back(pad);
        Entry tab; tab.name = kKey; tab.method = 0;
        /* The whole point of case 6: 11 bytes of extra in the LOCAL header, 0 in the central
         * one. Computing the data offset from the central header lands 11 bytes early. */
        tab.localExtra = 11; tab.centralExtra = 0; tab.declaredUncomp = 0;
        tab.data = entry;
        es.push_back(tab);
        check(writeZip(good, es), "case 0: wrote the good archive");
    }
    {
        std::vector<Entry> es;
        Entry tab; tab.name = kKey; tab.method = 8;
        tab.localExtra = 0; tab.centralExtra = 0; tab.declaredUncomp = kEntryBytes;
        tab.data.assign(kEntryBytes / 2, 0x77);
        es.push_back(tab);
        check(writeZip(deflated, es), "case 0: wrote the deflated archive");
    }
    {
        std::vector<Entry> es;
        Entry a; a.name = "segment_blob/course_data"; a.method = 0;
        a.localExtra = 0; a.centralExtra = 0; a.declaredUncomp = 0;
        a.data.assign(512, 0x33);
        es.push_back(a);
        check(writeZip(other, es), "case 0: wrote the keyless archive");
    }

    /* -- case 1: open, and the receipt fields ------------------------------------------- */
    {
        const char* paths[1] = { good.c_str() };
        const int ok = gdx_o2r_stream_open(paths, 1, kKey, kEntryBytes, 0x44u, 14u, 8u);
        check(ok == 1, "case 1: open succeeds on a STORED entry");
        check(std::strcmp(gdx_o2r_stream_refusal(), "ok") == 0, "case 1: refusal reads 'ok'");
        check(gdx_o2r_stream_active() == 1, "case 1: the stream is active");
        check(gdx_o2r_stream_payload_bytes() == kPayload,
              "case 1: payload bytes == entry bytes - 0x44");
        check(std::strcmp(gdx_o2r_stream_source(), good.c_str()) == 0,
              "case 1: the source path is the archive that won");
    }

    /* -- case 2: every byte, at every alignment ----------------------------------------- */
    {
        std::vector<unsigned char> buf(70000);
        const unsigned int offs[] = { 0u, 1u, 15u, 16u, 4095u, 4096u, 16383u, 16384u,
                                      16385u, 65535u, 131072u, kPayload - 1u, kPayload - 9u };
        const unsigned int lens[] = { 1u, 9u, 16u, 144u, 1024u, 16384u, 16385u, 65536u };
        int mismatches = 0, refusedInRange = 0;
        for (unsigned int o : offs) {
            for (unsigned int l : lens) {
                if ((unsigned long long) o + l > kPayload) {
                    continue;
                }
                std::memset(buf.data(), 0xCC, buf.size());
                if (!gdx_o2r_stream_read(o, l, buf.data())) {
                    refusedInRange++;
                    continue;
                }
                for (unsigned int i = 0; i < l; i++) {
                    if (buf[i] != bodyByte(o + i)) {
                        mismatches++;
                        break;
                    }
                }
            }
        }
        check(refusedInRange == 0, "case 2: no in-range read is refused");
        check(mismatches == 0, "case 2: every in-range read is byte-exact");
    }

    /* -- case 3: a full sequential sweep, which is what a boot-time sample load looks like */
    {
        const unsigned int chunk = 1777u; /* not block-aligned on purpose */
        std::vector<unsigned char> buf(chunk);
        unsigned int pos = 0;
        int bad = 0;
        while (pos < kPayload) {
            unsigned int want = chunk;
            if (kPayload - pos < want) {
                want = kPayload - pos;
            }
            if (!gdx_o2r_stream_read(pos, want, buf.data())) {
                bad++;
                break;
            }
            for (unsigned int i = 0; i < want; i++) {
                if (buf[i] != bodyByte(pos + i)) {
                    bad++;
                    break;
                }
            }
            pos += want;
        }
        check(bad == 0, "case 3: a whole-payload sweep through an 8-block cache is exact");
    }

    /* -- case 4: out of range refuses AND leaves dst untouched -------------------------- */
    {
        unsigned char buf[64];
        std::memset(buf, 0xEE, sizeof(buf));
        check(gdx_o2r_stream_read(kPayload, 1, buf) == 0, "case 4: read at the end refuses");
        check(gdx_o2r_stream_read(kPayload - 4u, 64u, buf) == 0,
              "case 4: a read running off the end refuses");
        check(gdx_o2r_stream_read(0xFFFFFFF0u, 64u, buf) == 0,
              "case 4: an offset that would wrap refuses");
        bool untouched = true;
        for (unsigned char b : buf) {
            if (b != 0xEE) {
                untouched = false;
            }
        }
        check(untouched, "case 4: dst is untouched on every refusal");
        check(gdx_o2r_stream_read(0, 0, buf) == 1, "case 4: a zero-length read succeeds");
    }

    /* -- case 5: refusals, each one named --------------------------------------------- */
    {
        const char* p[1] = { deflated.c_str() };
        check(gdx_o2r_stream_open(p, 1, kKey, kEntryBytes, 0x44u, 14u, 8u) == 0,
              "case 5: a DEFLATED entry is refused");
        check(std::strcmp(gdx_o2r_stream_refusal(), "not-stored") == 0,
              "case 5: the refusal is named 'not-stored'");
        check(gdx_o2r_stream_active() == 0, "case 5: nothing is left open after a refusal");
    }
    {
        const char* p[1] = { other.c_str() };
        check(gdx_o2r_stream_open(p, 1, kKey, kEntryBytes, 0x44u, 14u, 8u) == 0,
              "case 5: an archive without the key is refused");
        check(std::strcmp(gdx_o2r_stream_refusal(), "no-entry") == 0,
              "case 5: the refusal is named 'no-entry'");
    }
    {
        const std::string absent = dir + "/o2rtest-absent.zip";
        const char* p[1] = { absent.c_str() };
        check(gdx_o2r_stream_open(p, 1, kKey, kEntryBytes, 0x44u, 14u, 8u) == 0,
              "case 5: a missing file is refused");
        check(std::strcmp(gdx_o2r_stream_refusal(), "open-failed") == 0,
              "case 5: the refusal is named 'open-failed'");
    }
    {
        const char* p[1] = { good.c_str() };
        check(gdx_o2r_stream_open(p, 1, kKey, kEntryBytes + 16u, 0x44u, 14u, 8u) == 0,
              "case 5: an entry whose size disagrees with the table is refused");
        check(std::strcmp(gdx_o2r_stream_refusal(), "size-mismatch") == 0,
              "case 5: the refusal is named 'size-mismatch'");
    }
    {
        check(gdx_o2r_stream_open(nullptr, 0, kKey, kEntryBytes, 0x44u, 14u, 8u) == 0,
              "case 5: no paths at all is refused");
        check(std::strcmp(gdx_o2r_stream_refusal(), "no-path") == 0,
              "case 5: the refusal is named 'no-path'");
        unsigned char b = 0x5A;
        check(gdx_o2r_stream_read(0, 1, &b) == 0, "case 5: reading an inactive stream refuses");
        check(b == 0x5A, "case 5: an inactive read leaves dst untouched");
    }

    /* -- case 6: the LOCAL header's extra field is what sets the data offset ------------ */
    {
        const char* paths[1] = { good.c_str() };
        check(gdx_o2r_stream_open(paths, 1, kKey, kEntryBytes, 0x44u, 14u, 8u) == 1,
              "case 6: reopened the good archive");
        unsigned char first[16] = {};
        check(gdx_o2r_stream_read(0, sizeof(first), first) == 1, "case 6: read the first bytes");
        bool exact = true;
        for (unsigned int i = 0; i < sizeof(first); i++) {
            if (first[i] != bodyByte(i)) {
                exact = false;
            }
        }
        /* If the locator used the CENTRAL extra length (0) instead of the LOCAL one (11), the
         * window starts 11 bytes early and this is wrong by a constant -- inaudible as a
         * crash, very audible as noise. */
        check(exact, "case 6: the window is placed from the LOCAL header, not the central one");
    }

    /* -- case 7: archive scan order (last-mounted-wins is passed in reverse) ------------ */
    {
        const char* paths[3] = { other.c_str(), deflated.c_str(), good.c_str() };
        /* `other` lacks the key ("no-entry", keep looking); `deflated` has it but not STORED.
         * A scan that stops at the first archive carrying the NAME never reaches `good`. */
        const int ok = gdx_o2r_stream_open(paths, 3, kKey, kEntryBytes, 0x44u, 14u, 8u);
        check(ok == 1, "case 7: the scan walks past a keyless and a deflated archive");
        check(std::strcmp(gdx_o2r_stream_source(), good.c_str()) == 0,
              "case 7: the archive that actually serves it is the one that won");
    }
    {
        const char* paths[2] = { good.c_str(), other.c_str() };
        check(gdx_o2r_stream_open(paths, 2, kKey, kEntryBytes, 0x44u, 14u, 8u) == 1,
              "case 7: the FIRST path that can serve it wins");
        check(std::strcmp(gdx_o2r_stream_source(), good.c_str()) == 0,
              "case 7: and it is the first, not the last");
    }

    /* -- case 8: the cache is a cache, and a tiny one is still correct ------------------ */
    {
        const char* paths[1] = { good.c_str() };
        /* 4 blocks of 4 KiB = one set: every miss evicts. Correctness must not depend on
         * capacity, and a read LARGER than the whole cache must still be served. */
        check(gdx_o2r_stream_open(paths, 1, kKey, kEntryBytes, 0x44u, 12u, 4u) == 1,
              "case 8: opens with a one-set cache");
        std::vector<unsigned char> buf(40000);
        check(gdx_o2r_stream_read(1234u, (unsigned int) buf.size(), buf.data()) == 1,
              "case 8: a read larger than the entire cache is served");
        int bad = 0;
        for (unsigned int i = 0; i < buf.size(); i++) {
            if (buf[i] != bodyByte(1234u + i)) {
                bad++;
                break;
            }
        }
        check(bad == 0, "case 8: and it is byte-exact");
        /* Re-reading the same span twice must be cheaper the second time. */
        char line[320];
        gdx_o2r_stream_format_stats(line, sizeof(line));
        check(std::strstr(line, "refused=0") != nullptr, "case 8: no refusals were recorded");
    }

    /* -- case 9: the block cache directly: window, invalidate, uncached ----------------- */
    {
        GdxBlockCache c = {};
        /* A window that starts inside a block and ends inside another: the audio entry's
         * payload does exactly this (0x44 past whatever offset the local header lands on). */
        const unsigned int base = 0x44u + 7u;
        check(gdx_block_cache_open(&c, good.c_str(), base, 4096u, 0u, 12u, 8u) == 1,
              "case 9: a cache opens over an unaligned window");
        unsigned char buf[256];
        std::memset(buf, 0x22, sizeof(buf));
        check(gdx_block_cache_read(&c, base - 1u, 4u, buf) == 0,
              "case 9: a read starting before the window refuses");
        check(gdx_block_cache_read(&c, base + 4000u, 200u, buf) == 0,
              "case 9: a read running past the window refuses");
        bool untouched = true;
        for (unsigned char b : buf) {
            if (b != 0x22) {
                untouched = false;
            }
        }
        check(untouched, "case 9: dst untouched on a window refusal");
        check(gdx_block_cache_read(&c, base, 256u, buf) == 1, "case 9: an in-window read serves");
        GdxBlockCacheStats st = {};
        gdx_block_cache_get_stats(&c, &st);
        check(st.reads == 1ull && st.readBytes == 256ull, "case 9: stats count the served read");
        const unsigned long long missesBefore = st.blockMisses;
        check(gdx_block_cache_read(&c, base, 256u, buf) == 1, "case 9: the same read again");
        gdx_block_cache_get_stats(&c, &st);
        check(st.blockMisses == missesBefore, "case 9: the second read is a cache hit");
        gdx_block_cache_invalidate(&c, base, 256u);
        check(gdx_block_cache_read(&c, base, 256u, buf) == 1, "case 9: read after invalidate");
        gdx_block_cache_get_stats(&c, &st);
        check(st.blockMisses > missesBefore, "case 9: invalidate forced a refetch");
        unsigned char raw[16];
        check(gdx_block_cache_read_uncached(&c, 0u, 4u, raw) == 1, "case 9: uncached read serves");
        check(raw[0] == 0x50 && raw[1] == 0x4B, "case 9: and it read the zip's local signature");
        gdx_block_cache_close(&c);
        check(gdx_block_cache_active(&c) == 0, "case 9: closed");
        check(gdx_block_cache_read(&c, base, 4u, buf) == 0, "case 9: a closed cache refuses");
    }

    /* -- case 10: the cache never hands back more than it was asked for ----------------- */
    {
        GdxBlockCache c = {};
        /* 6 blocks is not a multiple of 4 ways; the SET count rounds DOWN, so this is one set
         * of four, 4 blocks, not six. A cache that quietly grew would break the budget claim
         * this whole track is about. */
        check(gdx_block_cache_open(&c, good.c_str(), 0u, 0u, 0u, 12u, 6u) == 1,
              "case 10: opens with a non-multiple block count");
        check(gdx_block_cache_bytes(&c) == 4u * 4096u,
              "case 10: the cache is no larger than asked (sets round DOWN)");
        check(gdx_block_cache_block_bytes(&c) == 4096u, "case 10: block size honoured");
        gdx_block_cache_close(&c);
        check(gdx_block_cache_open(&c, good.c_str(), 0u, 0u, 0u, 99u, 8u) == 1,
              "case 10: an out-of-range blockLog2 falls back to the default");
        check(gdx_block_cache_block_bytes(&c) == 16384u, "case 10: the default is 16 KiB");
        gdx_block_cache_close(&c);
        check(gdx_block_cache_open(&c, good.c_str(), 0u, 0u, 12345678u, 14u, 8u) == 0,
              "case 10: a wrong expected file size is refused");
    }

    gdx_o2r_stream_close();
    std::remove(good.c_str());
    std::remove(deflated.c_str());
    std::remove(other.c_str());

    std::printf("gdx_o2r_stream_tests: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
