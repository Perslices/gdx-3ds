/* port/3ds/lus_glue/gdx3ds_audiobridge.c — hook R5 (port/3ds/audio/STATUS.md).
 *
 * On desktop, libultraship's os.cpp/audiobridge satisfies the decomp's osAiSetNextBuffer
 * (the capture-tap site in decomp src/audio/<rom|disk>/lib/thread.c) and osAiGetLength.
 * The carved 3DS build has neither, so the produced PCM routes to stream C's ndsp
 * backend here: 4 bytes per interleaved stereo s16 frame, return value ignored by the
 * caller (overrun policy is drop-oldest inside the backend).
 *
 * osAiGetLength mirrors it for the pacing reads (aiSamplesLeft in thread.c and the
 * legacy 2048-frame cushion): frames buffered * 4 = bytes, exactly the AI-DMA length
 * semantics the decomp expects.
 */

#include "gdx3ds_audio.h"
#include "gdx_ndsp_hash.h"

#include <stddef.h>
#include <stdint.h>

int32_t osAiSetNextBuffer(void* buf, size_t sizeBytes) {
    if (buf == NULL || sizeBytes == 0) {
        return 0;
    }
    (void)gdx3ds_audio_push((const int16_t*)buf, sizeBytes / 4u);
    return 0;
}

uint32_t osAiGetLength(void) {
    /* [debug] ndsphash_pin (default 0, one int compare when off): report a CONSTANT instead
     * of the live ring occupancy.
     *
     * decomp/src/audio/rom/lib/thread.c:46-48 is this function's only caller on 3DS, and it
     * uses the answer for exactly one thing -- sizing the tick's AI buffer:
     *     aiSamplesLeft   = osAiGetLength() / 4;
     *     gAiBufLengths[i] = ((samplesPerFrameTarget - aiSamplesLeft + 0x80) & 0xFFF0) + 0x10;
     * The ring is drained by a real-time thread on another core, so that length differs
     * between two otherwise identical runs, every later sample position and envelope step
     * differs with it, and the two audio command streams never realign. Measured: two
     * identical runs of the canonical replay agree on their first 205 audio tasks and then
     * share only 17.2% of their command lists. It is the same root cause as audiodiet's
     * 3452-vs-3439 reads and audioasync's 37,056-vs-37,648 sil_ppk.
     *
     * Reporting 0 means "the AI is empty", so every tick produces the same full buffer. That
     * is a PARITY MODE and not the shipped configuration -- it changes where the output is
     * cut, though not what is in it -- and any claim made under it has to say so, exactly as
     * a claim made under [debug] det=1 does. The producer's own pacing loop reads
     * gdx3ds_audio_buffered() directly (port/gdx_audio_thread.cpp) and is untouched by this.
     */
    if (gdx_ndsphash_ai_pin()) {
        return 0u;
    }
    return (uint32_t)(gdx3ds_audio_buffered() * 4u);
}
