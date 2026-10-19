/* port/3ds/include/gdx3ds_filelog.h — optional SD-card sink for the boot/watchdog/fatal
 * tracers (H-HARDWARE).
 *
 * On real hardware the svcOutputDebugString channel the tracers rely on goes nowhere
 * useful (Azahar logs it; a retail New3DS does not, unless a Luma debugger is attached).
 * This sink mirrors those lines to sdmc:/3ds/gdiffuser/log.txt so a hardware crash
 * leaves evidence on the SD card.
 *
 * INI (sdmc:/3ds/gdiffuser/gdiffuser.ini):
 *   [debug]
 *   filelog = 1          ; default 0 (off — zero SD traffic when disabled)
 *   filelog_max_kb = 256 ; size cap; logging stops with a truncation marker when hit
 *   filelog_async = 0    ; 1 = SD writes on a writer thread, not on the logging thread (HITCH)
 *   filelog_async_ms = 1000 ; the writer's cadence (a 32 KiB backlog wakes it early)
 *   filelog_drop_full = 1  ; async: a full ring DROPS the line (counted) instead of the caller
 *                          ; waiting for the writer and flushing (N60LOG; fatal lines still flush)
 *   receipt_every = 64     ; window-receipt cadence in iterations (N60LOG; 512 on the O3DS card)
 *
 * Contract:
 *   - init() after gdx3ds_config_load(); every call is a no-op when disabled.
 *   - write() is thread-safe (main + watchdog + audio threads) via a recursive lock and
 *     appends one '\n'-terminated line to a FIXED 64 KiB batch ring — never the heap, so
 *     it is safe from the operator-new failure path and can never grow (a line that
 *     does not fit is dropped and the next flush appends "[filelog] dropped=N").
 *   - Batches reach the SD card when the ring is 3/4 full, every 250 ms, on any
 *     "[fatal"-prefixed line, or on an explicit flush() — so per-line SD fflush stalls
 *     are gone while fatal lines still survive svcBreak/abort where no teardown runs.
 *   - The file is truncated on every boot: one boot, one log.
 */
#ifndef GDX3DS_FILELOG_H
#define GDX3DS_FILELOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void gdx3ds_filelog_init(void);
void gdx3ds_filelog_write(const char* msg, size_t len);
/* Extra durability point for the fatal tracers; harmless when disabled/closed. */
void gdx3ds_filelog_flush(void);
/* n60hwdiag: BEFORE gdx3ds_filelog_init, log to `path` instead of log.txt, opened for APPEND with
 * no rotation (a boot-cycling bisect names one file per boot). */
void gdx3ds_filelog_set_session_path(const char* path);
/* N60LAUNCH: BEFORE gdx3ds_filelog_init, log to sdmc:/3ds/gdiffuser/log-<tag>.txt (truncated per
 * boot; the previous one kept as log-<tag>-prev.txt) so a launch variant never touches log.txt's
 * rotation. Empty/NULL = the historic names. A session path, when set, wins. */
void gdx3ds_filelog_set_tag(const char* tag);
/* N60LAUNCH: BEFORE a (deferred) gdx3ds_filelog_init: the file starts with the retained ring's
 * whole history instead of "from now", so the boot lines logged before the init are not lost. */
void gdx3ds_filelog_set_mirror_history(int on);
/* n60hwdiag: while on, every line is flushed synchronously AND fsync'd (FSFILE_Flush), so a hard
 * freeze keeps every line written before it. Off restores the configured batch/async policy. */
void gdx3ds_filelog_set_durable(int on);

/* MENU LOG tab: copy up to maxLines retained lines (oldest first) into `out`, an
 * array of maxLines x lineCap chars, skipping the skipNewest most recent lines.
 * Returns lines written. Retention is always on (independent of debug.filelog). */
int gdx3ds_filelog_tail_lines(unsigned skipNewest, char* out, int maxLines, int lineCap);

/* C4I logging-cost ledger, read by the [othx] receipt (gdx3ds_gpu_prof.c). Cumulative
 * since arming; the reader takes window deltas. *Ticks are svcGetSystemTick ticks spent
 * inside write() (lock wait included), split by caller: main = the logical main thread
 * (host loop + game fibers), other = every other thread. flushTicks is the SD batch
 * flush (fwrite + fflush) share of those ticks, whichever thread paid it. Arming costs
 * two tick reads per logged line; unarmed, write() pays one int test. */
typedef struct GdxFilelogCost {
    uint64_t mainTicks;
    uint64_t otherTicks;
    unsigned long mainLines;
    unsigned long otherLines;
    unsigned long flushes;
    uint64_t flushTicks;
} GdxFilelogCost;
void gdx3ds_filelog_cost_arm(int on);
void gdx3ds_filelog_cost_totals(GdxFilelogCost* out);

/* HITCH: cumulative sink statistics (always counted; zero heap). sync* = batch flushes
 * written by the logging thread itself (the hardware hitch: 26-270 ms each on the Old 3DS
 * card, hw-rel2); async* = writes by the [debug] filelog_async=1 writer thread (none of it
 * on the logging thread). syncWaits = synchronous flushes that had to wait for an in-flight
 * async write; ringFullFlushes = appends that found 64 KiB unflushed and flushed first.
 * pending = ring bytes not yet on the card; writerBusy = an async write is in flight now. */
typedef struct GdxFilelogStats {
    unsigned long lines;
    uint64_t bytes;
    unsigned long syncFlushes;
    uint64_t syncBytes;
    uint64_t syncTicks;
    unsigned long syncWaits;
    unsigned long ringFullFlushes;
    unsigned long asyncWrites;
    uint64_t asyncBytes;
    uint64_t asyncTicks;
    uint64_t asyncMaxTicks;
    int async;
    int writerBusy;
    unsigned pending;
    unsigned long asyncDeferred; /* QUICKFIX: writer passes deferred by [debug] filelog_race_quiet */
    unsigned long ringFullDrops;   /* N60LOG: lines dropped because the ring was full (no caller wait) */
    unsigned long receiptsThinned; /* N60LOG: window receipts skipped by [debug] receipt_every */
    unsigned long audioYields;     /* N60LOG: writer chunks that waited for an audio-stream SD read */
} GdxFilelogStats;
/* N60LOG [debug] receipt_every (default 64; 64..4096, a multiple of 64): the [c5cat] window family's
 * window length in iterations (gdx3ds_mainprof.c, gdx3ds_ships.c); other window receipts are
 * thinned in the sink to one line per key per receipt_every host iterations. */
int gdx3ds_filelog_receipt_every(void);
/* N60LOG: the host loop's iteration clock for that thinning (main_3ds.cpp, once per iteration). */
void gdx3ds_filelog_frame_end(unsigned long frame);
void gdx3ds_filelog_stats(GdxFilelogStats* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_FILELOG_H */
