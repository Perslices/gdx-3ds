/* port/3ds/gdx3ds_membudget.cpp — O3DS BUDGET receipts ([debug] membudget, DEFAULT 0).
 * Contract + why: port/3ds/include/gdx3ds_membudget.h.
 *
 * NOTHING here runs unless the lever is on: gdx3ds_membudget_enabled() is one file-static
 * load, and both emitters return on it before touching a service. That is what keeps this
 * file out of every parity and profile claim the fork has already made.
 */

#include "gdx3ds_membudget.h"

#include <3ds.h>
#include <malloc.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_heapattr.h"
#include "gdx3ds_heapcfg.h"
#include "gdx_segment_source.h"

/* The two single largest malloc'd blocks in the process, neither of which any existing
 * census names. The RDRAM arena is ONE 16 MB calloc (decomp_port.c gdx_rdram_init, sized
 * by GDX_RDRAM_SIZE for the Expansion Pak overlay offsets) -- over a third of the shipped
 * heap in one row. gdx_rom_buffer is the raw-ROM fallback source and is normally NULL in
 * an archive build; if it is ever not, a budget argument that missed it would be wrong by
 * the size of the ROM. */
#include "n64_rdram.h" /* gdx_rdram, GDX_RDRAM_SIZE -- never restate the size here */
extern "C" {
extern unsigned char* gdx_rom_buffer;
extern size_t gdx_rom_size;
}

/* devkitARM's 3dsx linker script. __start__ .. __bss_start__ is text+rodata+data (the
 * mapped image), __bss_start__ .. __bss_end__ is BSS. Both are committed out of the same
 * APPLICATION region the malloc heap is carved from, which is exactly why they belong in
 * a budget receipt: on Old 3DS that region is the binding constraint, not the frame time. */
extern "C" {
extern char __start__[];
extern char __bss_start__[];
extern char __bss_end__[];
}

namespace {

int sEnabled = 0;
int sTopRows = 24;
int sHeapAttrTop = 24; /* [debug] heapattr_top -- rows of the C4c allocation census */

void Emit(const char* line, size_t len) {
    svcOutputDebugString(line, len);
    gdx3ds_filelog_write(line, len);
}

void EmitF(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void EmitF(const char* fmt, ...) {
    char msg[224];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n > 0) {
        Emit(msg, (size_t)(n < (int)sizeof(msg) ? n : (int)sizeof(msg) - 1));
    }
}

/* One census row. `kind` says which pool the bytes came out of, because mixing them is how
 * a budget argument goes wrong: `heap` rows sum against mallinfo().uordblks, `static` rows
 * are BSS/image (never on the heap -- ekbuild Experiment 2 proved that the hard way), and
 * `linear` rows come from MEMOP_ALLOC_LINEAR, a different physical region again. */
struct Row {
    unsigned long bytes;
    const char* kind;
    char owner[48];
};

constexpr size_t kMaxRows = 160;

struct Census {
    Row rows[kMaxRows];
    size_t count = 0;

    void Add(unsigned long bytes, const char* kind, const char* owner) {
        if (bytes == 0u || count >= kMaxRows) {
            return;
        }
        Row& r = rows[count++];
        r.bytes = bytes;
        r.kind = kind;
        std::snprintf(r.owner, sizeof(r.owner), "%s", owner);
    }

    /* Selection sort, descending. kMaxRows is 160 and this runs on the census cadence
     * (~every 10 s, only under the lever), so the simplest correct sort is the right one. */
    void SortDescending() {
        for (size_t i = 0; i + 1 < count; i++) {
            size_t best = i;
            for (size_t j = i + 1; j < count; j++) {
                if (rows[j].bytes > rows[best].bytes) {
                    best = j;
                }
            }
            if (best != i) {
                const Row tmp = rows[i];
                rows[i] = rows[best];
                rows[best] = tmp;
            }
        }
    }
};

/* Every resident segment-blob family, by name, at its whole-file buffer size. These are
 * process-lifetime caches (gdx_segment_source.c: "the whole file buffer stays alive"), so
 * a family that ever loads is resident for the rest of the run -- which makes them the
 * first place a memory diet should look. */
void AddSegmentFamilies(Census& census) {
    for (unsigned int i = 0;; i++) {
        const char* key = nullptr;
        unsigned int payloadBytes = 0;
        unsigned int allocBytes = 0;
        if (!GdxSegmentSourceFamilyResidency(i, &key, &payloadBytes, &allocBytes)) {
            break;
        }
        if (allocBytes == 0u || key == nullptr) {
            continue; /* discovered but not resident: it costs nothing yet */
        }
        census.Add((unsigned long)allocBytes, "heap", key);
    }
}

/* The COMMIT resource limit, read exactly the way __system_allocateHeaps reads it
 * (svcGetResourceLimit(CUR_PROCESS_HANDLE) -> RESLIMIT_COMMIT). `avail` at crt0 time was
 * limit - current BEFORE the two heaps were committed, so reconstructing it here as
 * (limit - current) + heapGrant + linGrant turns the memtype-0 projection into something
 * the FIRST hardware run measures instead of infers: on the console, `availBoot` is the
 * one number the whole budget is derived from, and nothing else in the port prints it.
 * Returns false and leaves the outputs alone if either svc fails. */
bool ReadCommitLimit(unsigned long* limitOut, unsigned long* currentOut) {
    Handle rl = 0;
    if (R_FAILED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE))) {
        return false;
    }
    ResourceLimitType name = RESLIMIT_COMMIT;
    s64 limit = 0;
    s64 current = 0;
    const bool ok = R_SUCCEEDED(svcGetResourceLimitLimitValues(&limit, rl, &name, 1)) &&
                    R_SUCCEEDED(svcGetResourceLimitCurrentValues(&current, rl, &name, 1));
    svcCloseHandle(rl);
    if (!ok) {
        return false;
    }
    *limitOut = (unsigned long)limit;
    *currentOut = (unsigned long)current;
    return true;
}

} // namespace

void gdx3ds_membudget_configure(void) {
    sEnabled = gdx3ds_config_get_bool("debug", "membudget", 0) ? 1 : 0;
    const int top = gdx3ds_config_get_int("debug", "membudget_top", 24);
    sTopRows = (top > 0 && top <= (int)kMaxRows) ? top : 24;
    const int attrTop = gdx3ds_config_get_int("debug", "heapattr_top", 24);
    sHeapAttrTop = (attrTop > 0 && attrTop <= 64) ? attrTop : 24;
}

int gdx3ds_membudget_enabled(void) {
    return sEnabled;
}

void gdx3ds_membudget_boot(void) {
    if (!sEnabled) {
        return;
    }
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);

    const unsigned long appSize = (unsigned long)osGetMemRegionSize(MEMREGION_APPLICATION);
    const unsigned long appUsed = (unsigned long)osGetMemRegionUsed(MEMREGION_APPLICATION);
    const unsigned long sysSize = (unsigned long)osGetMemRegionSize(MEMREGION_SYSTEM);
    const unsigned long baseSize = (unsigned long)osGetMemRegionSize(MEMREGION_BASE);

    /* The two libctru grants. Both are weak symbols left at 0 by this port, so
     * __system_allocateHeaps computed them: linear = (available/2) page-aligned down,
     * heap = available - linear, heap capped at 24 MB unless that would push linear past
     * 32 MB. Disassembled from the libctru we link (allocateHeaps.o); see the progress
     * doc. Printing the RESULT means the formula never has to be trusted second-hand. */
    const unsigned long heapGrant = (unsigned long)envGetHeapSize();
    const unsigned long linGrant = (unsigned long)envGetLinearHeapSize();

    const unsigned long imageBytes = (unsigned long)(__bss_start__ - __start__);
    const unsigned long bssBytes = (unsigned long)(__bss_end__ - __bss_start__);

    struct mallinfo mi = mallinfo();

    EmitF("[mem-budget] model=%s memtype=%lu app=%lu/%lu sys=%lu base=%lu "
          "heapGrant=%lu linGrant=%lu linFree=%lu image=%lu bss=%lu "
          "arena=%lu used=%lu free=%lu",
          isNew3ds ? "new3ds" : "old3ds", (unsigned long)osGetApplicationMemType(), appUsed,
          appSize, sysSize, baseSize, heapGrant, linGrant, (unsigned long)linearSpaceFree(),
          imageBytes, bssBytes, (unsigned long)mi.arena, (unsigned long)mi.uordblks,
          (unsigned long)mi.fordblks);

    /* Second line, and the one that says whether a build-time knob actually took.
     *
     * `heapCfg`/`pinHeap`/`pinLin` are what THIS BUILD asked for (port/3ds/gdx3ds_heapcfg.c,
     * a link-time override of libctru's weak __ctru_heap_size / __ctru_linear_heap_size);
     * `heapGrant`/`linGrant` on the line above are what libctru actually committed. They are
     * printed separately on purpose. o3dsbudget lost a run to a lane setting that wrote
     * exactly the right file and still did not apply, and the rule it paid for -- a setting
     * is pinned only when the guest says so -- applies at least as much to a symbol override,
     * which fails silently and looks like nothing happened.
     *
     * `availBoot` reconstructs the commit budget __system_allocateHeaps divided up. On an
     * `auto` build it must equal heapGrant+linGrant; on a pinned build the difference is the
     * memory the simulation is deliberately refusing to use. */
    unsigned long commitLimit = 0;
    unsigned long commitUsed = 0;
    if (ReadCommitLimit(&commitLimit, &commitUsed)) {
        const unsigned long availNow = commitLimit > commitUsed ? commitLimit - commitUsed : 0ul;
        EmitF("[mem-budget] heapCfg=%s pinHeap=%lu pinLin=%lu simAvail=%lu "
              "commitLimit=%lu commitUsed=%lu availNow=%lu availBoot=%lu",
              gdx3ds_heapcfg_mode(), gdx3ds_heapcfg_pin_heap(), gdx3ds_heapcfg_pin_linear(),
              gdx3ds_heapcfg_sim_avail(), commitLimit, commitUsed, availNow,
              availNow + heapGrant + linGrant);
    } else {
        EmitF("[mem-budget] heapCfg=%s pinHeap=%lu pinLin=%lu simAvail=%lu commit=unreadable",
              gdx3ds_heapcfg_mode(), gdx3ds_heapcfg_pin_heap(), gdx3ds_heapcfg_pin_linear(),
              gdx3ds_heapcfg_sim_avail());
    }
}

/* ---- [rdram-res]: inside the single largest heap row -------------------------------------
 * The census above prints the RDRAM arena as ONE 16 MB row, because that is what it costs.
 * It says nothing about what fraction of it is ever used, and 16 MB is not an N64 size --
 * the cartridge has 4 MB and the Expansion Pak makes it 8. This line opens the row.
 *
 * Reading it: the arena is two cursors growing toward each other inside one calloc.
 *
 *   0          gfxRsv        staging      arenaStart        bumpHwm     persistLow      size
 *   |--unused--|--GfxPool----|--1 MiB-----|--mode arena-----|---HOLE----|--persist------|
 *
 * `bumpHwm` is the high-water of the upward cursor across the WHOLE run (gdx_rdram_mode_reset
 * rewinds the live cursor at every mode transition, so `bumpNow` systematically under-reports).
 * `persistLow` is the downward cursor, which never rewinds. `hole` is what a smaller arena
 * would reclaim -- and `touchedHi` is the check that reclaiming it is safe: it is the highest
 * NON-ZERO byte below persistLow, found by scanning, so it sees writes that never went through
 * either allocator. `touchedHi > bumpHwm` means something writes into the hole by raw address
 * and the hole is not free. `physHi` is the highest offset the gfx bridge ever RESOLVED, which
 * is the other constraint: GDX_RDRAM_SIZE is also the top of the bridge's bare-offset
 * classification window (n64_gfx_bridge.cpp), so a proposed size must clear physHi too.
 *
 * `minSafe` is the three of them maxed and rounded UP to a MiB: the smallest -DGDX_3DS_RDRAM_MB
 * this run is evidence for. It is evidence from ONE run over ONE replay; the corpus-wide
 * number is the max over every replay, which is the whole point of running more than one. */
static void EmitRdramOccupancy() {
    size_t st[GDX_RDRAM_PROBE_SLOTS];
    gdx_rdram_probe_stats(st);
    if (st[2] == 0) { /* arenaStart 0 => gdx_rdram_init has not run */
        return;
    }
    const unsigned long size       = (unsigned long)st[0];
    const unsigned long bumpHwm    = (unsigned long)st[4];
    const unsigned long persistLow = (unsigned long)st[5];
    const unsigned long touchedHi  = (unsigned long)st[6];
    const unsigned long physHi     = (unsigned long)st[8];
    unsigned long need = bumpHwm > touchedHi ? bumpHwm : touchedHi;
    if (physHi > need) {
        need = physHi;
    }
    need += (unsigned long)(size - persistLow); /* the persist block rides the TOP: it moves down with the size */
    const unsigned long minSafeMb = (need + (1024ul * 1024ul) - 1ul) / (1024ul * 1024ul);
    EmitF("[rdram-res] size=%lu stagingBase=%lu arenaStart=%lu bumpNow=%lu bumpHwm=%lu",
          size, (unsigned long)st[1], (unsigned long)st[2], (unsigned long)st[3], bumpHwm);
    EmitF("[rdram-res] persistLow=%lu persistBytes=%lu hole=%lu touchedHi=%lu preArenaHi=%lu "
          "physHi=%lu allocFail=%lu minSafeMB=%lu",
          persistLow, size - persistLow,
          persistLow > bumpHwm ? persistLow - bumpHwm : 0ul, touchedHi,
          (unsigned long)st[7], physHi, (unsigned long)st[9], minSafeMb);
}

/* ---- [tex-upload]: inside the largest heap block no [mem-res] row names ------------------
 * Fast3D's Interpreter::Init holds ONE malloc(maxTexSize^2 * 4) for the process lifetime --
 * 4,194,304 B on this backend, because GetMaxTextureSize() is the PICA200's 1024. C4c's
 * allocation census put it at rank 3 of the whole heap. `hwm` is the largest decoded extent
 * anything actually uploaded on this run; `cap` is what Init reserved. The pair is the cut. */
extern "C" {
extern unsigned long gdx3ds_tex_upload_hwm_bytes;
extern unsigned long gdx3ds_tex_upload_hwm_w;
extern unsigned long gdx3ds_tex_upload_hwm_h;
extern unsigned long gdx3ds_tex_upload_count;
extern unsigned long gdx3ds_tex_upload_cap_bytes; /* gdx3ds_texupload.c; 0 before Init runs */
}

static void EmitTexUploadHighWater() {
    EmitF("[tex-upload] cap=%lu hwm=%lu w=%lu h=%lu uploads=%lu spare=%lu",
          gdx3ds_tex_upload_cap_bytes, gdx3ds_tex_upload_hwm_bytes, gdx3ds_tex_upload_hwm_w,
          gdx3ds_tex_upload_hwm_h, gdx3ds_tex_upload_count,
          gdx3ds_tex_upload_cap_bytes > gdx3ds_tex_upload_hwm_bytes
              ? gdx3ds_tex_upload_cap_bytes - gdx3ds_tex_upload_hwm_bytes
              : 0ul);
}

void gdx3ds_membudget_census(unsigned long frame, const char* why, const unsigned long gfx[8],
                             unsigned long lusBytes, unsigned long bigBytes) {
    if (!sEnabled) {
        return;
    }
    static Census census; /* 160 rows x 60 B: off the stack, this runs on the game thread */
    census.count = 0;

    if (gdx_rdram != nullptr) {
        census.Add((unsigned long)GDX_RDRAM_SIZE, "heap", "decomp/RDRAM arena");
    }
    if (gdx_rom_buffer != nullptr) {
        census.Add((unsigned long)gdx_rom_size, "heap", "port/raw ROM buffer");
    }
    AddSegmentFamilies(census);
    census.Add(lusBytes, "heap", "lus/ResourceManager cache");
    if (gfx != nullptr) {
        census.Add(gfx[0], "heap", "bridge/loaded asset segments");
        census.Add(gfx[2], "heap", "bridge/persistent raw textures");
        census.Add(gfx[4], "heap", "bridge/wide-conversion cache");
        census.Add(gfx[6], "heap", "bridge/setup_gfx segment");
    }

    const unsigned long imageBytes = (unsigned long)(__bss_start__ - __start__);
    const unsigned long bssBytes = (unsigned long)(__bss_end__ - __bss_start__);
    census.Add(bssBytes, "static", "image/.bss");
    census.Add(imageBytes, "static", "image/.text+.rodata+.data");

    const unsigned long linGrant = (unsigned long)envGetLinearHeapSize();
    const unsigned long linFree = (unsigned long)linearSpaceFree();
    census.Add(linGrant > linFree ? linGrant - linFree : 0ul, "linear", "citro3d/linear heap");

    census.SortDescending();

    struct mallinfo mi = mallinfo();
    unsigned long heapAttributed = 0;
    for (size_t i = 0; i < census.count; i++) {
        if (std::strcmp(census.rows[i].kind, "heap") == 0) {
            heapAttributed += census.rows[i].bytes;
        }
    }

    EmitF("[mem-res] frame=%lu why=%s rows=%u top=%d", frame, why != nullptr ? why : "?",
          (unsigned)census.count, sTopRows);
    const size_t shown = census.count < (size_t)sTopRows ? census.count : (size_t)sTopRows;
    for (size_t i = 0; i < shown; i++) {
        const Row& r = census.rows[i];
        EmitF("[mem-res] rank=%u bytes=%lu kb=%lu kind=%s owner=%s", (unsigned)(i + 1), r.bytes,
              r.bytes / 1024ul, r.kind, r.owner);
    }
    /* The reconciliation line is the honest half of the census: `unattributed` is every
     * heap byte no row above claims (C++ containers, std::string, the interpreter's own
     * batches, libctru service buffers). A diet plan built on the ranked rows alone would
     * be a plan for whatever fraction this line says is covered. */
    const unsigned long heapUsed = (unsigned long)mi.uordblks;
    EmitF("[mem-res] totals heapUsed=%lu heapFree=%lu arena=%lu attributed=%lu "
          "unattributed=%lu pct=%lu big=%lu static=%lu linUsed=%lu",
          heapUsed, (unsigned long)mi.fordblks, (unsigned long)mi.arena, heapAttributed,
          heapUsed > heapAttributed ? heapUsed - heapAttributed : 0ul,
          heapUsed != 0ul ? (heapAttributed * 100ul) / heapUsed : 0ul, bigBytes,
          imageBytes + bssBytes, linGrant > linFree ? linGrant - linFree : 0ul);

    EmitTexUploadHighWater();

    EmitRdramOccupancy();

    /* C4c: the OTHER half of the reconciliation line above. [mem-res] attributes the heap by
     * enumerating owners somebody thought to name; [heap-attr] attributes it by return
     * address, from inside the allocator, so the `unattributed=` bytes get named too. It
     * prints nothing at all unless the build carries the map (-DGDX_3DS_HEAPATTR=ON) --
     * the same policy [rdram-res] uses for GDX_3DS_MEMPROBE, and for the same reason: a row
     * of zeros reads like a measurement. */
    if (gdx3ds_heapattr_available()) {
        gdx3ds_heapattr_census(frame, sHeapAttrTop);
    }
}
