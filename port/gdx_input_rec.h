// G-Diffuser — .gdxrec input recorder / replayer (ORACLE track, plan §2).
//
// One line per input tick, streamed (fgets per poll; O(1) memory, no event cap). Both directions
// run through gdx_input_inject() in input_bridge.c — the single injector autoinput, GDX_INPUT_SCRIPT
// and (via gdx_replay_start) the hwlink net path share — so a replayed pad flows through the
// identical accumulate-then-finalize path physical input does.
//
// FORMAT (ASCII; `#` starts a comment anywhere a line can start; blank lines ignored):
//
//   gdxrec 1
//   build <build id>
//   seed1 0x<hex> | none
//   seed2 0x<hex> | none
//   pins renderthread=<n> rival_detail_auto=<n> stereo=<n> fh_every=<n>
//   # tick btn stickx sticky [touch tx ty] [SHOT label]
//   0 0000 0 0
//   1 0000 0 0
//   ...
//   4600 8000 -34 12 SHOT race00
//
// Header lines are `key value`, in any order, until the first data line. Data line K (0-based,
// comments excluded) is tick K: <tick> <btn: 4 hex digits, N64 OSContPad mask> <stickx> <sticky>
// (decimal -80..80). Optional `touch tx ty` (3DS bottom screen; parsed and ignored by this
// build, never emitted). `SHOT <label>` is a TRAILING token on the frame's own line, so a
// trailing SHOT can never be dropped at parse. The recorder inserts `# mode <m> tick <t>`
// comment lines at game-mode changes (diagnostic only).
//
// RECORD: [debug] input_record=1 -> rec/<input_record_name|rec0>.gdxrec, written from the same
// poll AFTER every injector stage, so recording a replay reproduces the file byte for byte.
// REPLAY: [debug] input_replay=<name> (rec/<name>.gdxrec, wins over autoinput; logs
// "[rec] replay <name> frames=N base=<tick>" and "[rec] end tick=N" at EOF, after which
// physical input returns). Deterministic replays start at boot (base tick 0); a mid-run
// gdx_replay_start() replays relative to its start tick and is NOT a parity oracle.
#pragma once

#include "gdx_input_script.h" // GdxInputPad

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxInputRecHeader {
    const char* build;   // "<branch>@<sha8>" or "unknown"
    int seed1Have;       // [debug] rand_seed1 pinned?
    unsigned int seed1;
    int seed2Have;
    unsigned int seed2;
    const char* pins;    // "renderthread=1 rival_detail_auto=0 stereo=0 fh_every=0"
} GdxInputRecHeader;

// Stores a copy of the run's header (record writes it; replay compares against it). Optional.
void gdx_input_rec_init(const GdxInputRecHeader* hdr);

// Opens `path` for writing and emits the header. Returns 0 on success. Call BEFORE the first poll
// so line 0 == tick 0.
int gdx_input_rec_start_record(const char* path);

// Closes the recording (flushes). Safe to call when not recording.
void gdx_input_rec_stop(void);

// Cross-track contract (hwlink): start replaying `sdmc_path` (any path fopen accepts) at the
// NEXT input tick. Returns 0 on success, nonzero when the file is missing or its header is not
// "gdxrec 1". Replaces any replay in progress.
int gdx_replay_start(const char* sdmc_path);

// Replay armed (pending or active): the injector skips autoinput while this is true.
int gdx_input_rec_replay_armed(void);
int gdx_input_rec_replay_active(void);

// Invoked (main thread, inside the poll) when a replay reaches EOF. NULL = none.
void gdx_input_rec_set_end_callback(void (*cb)(void));

// The injector stage: replay override (when active), then record (when recording).
void gdx_input_rec_poll(unsigned long tick, GdxInputPad* pad, int gameMode);

// A SHOT was armed for this tick by another injector stage (autoinput / script): the recorder
// appends "SHOT <label>" to the current tick's line.
void gdx_input_rec_note_shot(const char* label);
/* C5TICK: 1 if the next replay line (the next tick's input) carries a SHOT (or the replay is armed
 * but not started); 0 when no replay runs. Read-ahead + seek back. */
int gdx_input_rec_peek_shot(void);

#ifdef __cplusplus
}
#endif
