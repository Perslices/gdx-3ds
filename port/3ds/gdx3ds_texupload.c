/* port/3ds/gdx3ds_texupload.c — C4c cut #1: the Fast3D decoded-texture staging buffer.
 *
 * WHAT IT IS. `Fast::Interpreter::Init` does, once, for the life of the process:
 *
 *     int max_tex_size = std::min(8192, mRapi->GetMaxTextureSize());
 *     mTexUploadBuffer = (uint8_t*)malloc(max_tex_size * max_tex_size * 4);
 *
 * `GfxRenderingAPIC3D::GetMaxTextureSize()` returns 1024 (the PICA200 maximum), so the
 * reservation is 1024 * 1024 * 4 = **4,194,304 B**. C4c's allocation census found it at
 * rank 3 of the whole heap and 16.7% of an Old 3DS memtype-0 grant -- the largest single
 * block in the process that no [mem-res] row has ever named, because every census in this
 * fork enumerates OWNERS and nobody owned this one.
 *
 * It is a worst-case reservation, not a working set: it is sized for the largest texture
 * the GPU could theoretically accept, and what the game actually decodes into it is
 * 4 * width * height for whatever tile is being imported. `[tex-upload] cap= hwm=` on the
 * [mem-census] cadence prints the two side by side.
 *
 * WHAT THIS FILE ADDS.
 *
 *   [debug] tex_upload_kb   (DEFAULT 0 = exactly today's max_tex_size^2 * 4)
 *       The reservation, in KiB. An ini lever rather than a CMake knob because Fast3D's
 *       Init runs long after gdiffuser.ini is read -- unlike the heap split (C4b/C4e),
 *       which crt0 has already made, or the RDRAM arena (C4g), which is also a
 *       classification bound the compiler folds in. Nothing here is a compile-time
 *       constant, so nothing here needs to be a build knob.
 *
 *   The clamps. Shrinking a buffer that ~11 decoders write into by raw index is only safe
 *       if the extent is bounded at each of them, so gdx3ds_tex_upload_clamp_rows() /
 *       _clamp_bytes() give every decoder the largest extent that still fits, and SAY SO
 *       when they cut one. A clamp that fires is a lever set too small: bounded output
 *       plus a receipt, which is this port's established answer (ImportTextureI4's own
 *       CLAMP path, added after a mid-race heap access violation).
 *
 *       At the default reservation the clamps are arithmetic that never fires -- a
 *       1024x1024 extent is the most the backend can accept in the first place.
 *
 * docs/research/native/heapattr-progress.md.
 */

#include <stddef.h>
#include <stdint.h>

#include "gdx3ds_config.h"

extern void gdx_dbg_logf(const char* fmt, ...);

/* Read by gdx3ds_membudget.cpp for the [tex-upload] receipt. 0 until Init has run, which
   is honest: before that there is no reservation to report. */
unsigned long gdx3ds_tex_upload_cap_bytes = 0;

static size_t sCapBytes = 0;
static int sClampLogs = 0;

/* Called once from Fast::Interpreter::Init with what the stock code would have allocated.
   Returns what to allocate instead. */
size_t gdx3ds_tex_upload_pick_size(size_t defaultBytes) {
    const int kb = gdx3ds_config_get_int("debug", "tex_upload_kb", 0);
    size_t bytes = defaultBytes;
    if (kb > 0) {
        bytes = (size_t)kb * 1024u;
    }
    /* A reservation below one 32x32 RGBA32 tile cannot decode anything at all; refuse to
       honour a typo rather than clamp every texture in the game to nothing. */
    if (bytes < 4096u) {
        bytes = 4096u;
    }
    sCapBytes = bytes;
    gdx3ds_tex_upload_cap_bytes = (unsigned long)bytes;
    gdx_dbg_logf("[tex-upload] init cap=%lu default=%lu lever_kb=%d\n", (unsigned long)bytes,
                 (unsigned long)defaultBytes, kb);
    return bytes;
}

/* The largest number of rows of `width` RGBA32 texels that fits the reservation. */
uint32_t gdx3ds_tex_upload_clamp_rows(uint32_t width, uint32_t height) {
    if (sCapBytes == 0u || width == 0u || height == 0u) {
        return height;
    }
    const size_t rowBytes = (size_t)width * 4u;
    const size_t maxRows = sCapBytes / rowBytes;
    if ((size_t)height <= maxRows) {
        return height;
    }
    if (sClampLogs < 16) {
        sClampLogs++;
        gdx_dbg_logf("[tex-upload] CLAMP rows %ux%u -> %ux%lu (cap=%lu) -- "
                     "[debug] tex_upload_kb is too small for this content\n",
                     width, height, width, (unsigned long)maxRows, (unsigned long)sCapBytes);
    }
    return (uint32_t)(maxRows != 0u ? maxRows : 1u);
}

/* The byte-count form, for the one decoder that fills by memcpy rather than by texel. */
size_t gdx3ds_tex_upload_clamp_bytes(size_t want) {
    if (sCapBytes == 0u || want <= sCapBytes) {
        return want;
    }
    if (sClampLogs < 16) {
        sClampLogs++;
        gdx_dbg_logf("[tex-upload] CLAMP bytes %lu -> %lu -- "
                     "[debug] tex_upload_kb is too small for this content\n",
                     (unsigned long)want, (unsigned long)sCapBytes);
    }
    return sCapBytes;
}
