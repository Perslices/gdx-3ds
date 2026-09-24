/* port/3ds/game/gdx3ds_ek_stubs.cpp -- 3DS stand-ins for the desktop-only EK glue that
 * port/disk_buffer.cpp calls. Compiled ONLY when GDX_3DS_EXPANSION_KIT=ON (B11).
 *
 * disk_buffer.cpp's archive path takes a SHA-256 of the reconstructed 64DD image and compares
 * it with the sha recorded by the first-boot Torch extractor, to decide whether the desktop
 * "Data & Files" panel may offer to DELETE the user's original .ndd. The three helpers live in
 * port/gdx_extract_launch.cpp, which the 3DS build deliberately drops (first-boot extraction
 * moved to tools/prebake on PC; see the keep/drop table in port/3ds/game/CMakeLists.txt).
 *
 * The 3DS has no such panel and must never delete user content, so these return empty strings.
 * disk_buffer.cpp reads that as "not verified" and leaves the deletion gate closed -- the
 * fail-safe verdict, not a bypass. If a Data & Files equivalent ever ships on 3DS, these must
 * be replaced by a real SHA-256 before it may offer deletion.
 */

#include <string>



#include "port_log.h"   /* gdx_port_logf is a static inline in this header */

namespace gdx {

static void warn_once() {
    static bool warned = false;
    if (!warned) {
        warned = true;
        gdx_port_logf("[leo] 3DS build: no SHA-256 extractor linked, disk-archive deletion gate "
                      "stays permanently closed\n");
    }
}

std::string GdxExtractSha256Bytes(const void* data, unsigned long long len) {
    (void)data;
    (void)len;
    warn_once();
    return std::string();
}

std::string GdxExtractRecordedDiskSha256(const char* dataDir) {
    (void)dataDir;
    warn_once();
    return std::string();
}

std::string GdxExtractFileSha256(const char* path) {
    (void)path;
    warn_once();
    return std::string();
}

} // namespace gdx

/* ---------------------------------------------------------------------------------
 * NEGATIVE RESULT, kept as a note so nobody repeats it (B11).
 *
 * The EK build stalls at boot with 262 KB left of a 48.84 MB malloc arena, so the obvious
 * move is to take some of libctru's 32 MB linear reservation (of which the shipped build
 * leaves ~28.8 MB unused -- [mem-census] lin=) and give it to malloc, by defining the weak
 * __ctru_linear_heap_size. Measured: it changes NOTHING. Linear memory on 3DS comes from a
 * separate physical region (MEMOP_ALLOC_LINEAR), not from the application heap, and under
 * the 3dsx homebrew environment the malloc heap is a FIXED grant from the loader. With
 * __ctru_linear_heap_size = 16 MB the build reached the identical guest frame with
 * heapUsed=48546168 heapFree=261768 -- within 2 KB of the 32 MB-linear run. All it would
 * buy is less linear headroom for texture uploads.
 * --------------------------------------------------------------------------------- */
