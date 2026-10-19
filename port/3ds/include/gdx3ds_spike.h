/* port/3ds/include/gdx3ds_spike.h — n60st3: the frame-spike attribution receipt
 * (docs/research/native/n60stereo-progress.md §8).
 *
 * WHY. In 3D the race frames hug 16.7 ms and about one in five doubles (hw-n60-3d2: [hitch] lines of
 * 34-41 ms every 20-50 ticks). The [hitch] line names the categories of the long iteration, but its
 * cause sits in the frame BEFORE it: a 17 ms `fb` is a begin that waited because the previous frame's
 * top swap was requested after a vblank (its GPU tail, or its CPU, ran late), and a 7-8 ms `tl`
 * (tail + input + apt) is CPU after the frame end that delays the NEXT begin. This receipt keeps the
 * previous frame's numbers and splits the tail.
 *
 * `[debug] spike_ms = N` (DEFAULT 0 = off: one int test per hook). An iteration whose wall (begin of
 * the loop body to its end, every wait included) exceeds N ms is a spike. Its line is buffered (4
 * lines) and written on a later non-spike iteration, at most one per 8 iterations and 8 per 256
 * (the rest counted: supp=). Per iteration while armed: 8 tick reads (~0.6 µs each on the console)
 * and a few copies. Line (ms):
 *
 *   [spike] f=N ms= | beg q= v= ph= | prev ms= rend= eg= gtail= gap= dr= tl= | tl= apt= inp= save=
 *           rpl= tele= hud= end= | st_us= ch= dr= | why=gate+queue+tail+cpu
 *
 *   beg  this iteration's frame begin: q = GX queue wait (the previous frame's GPU still running),
 *        v = vblank wait, ph = the pace rule (0 stock, 1 hybrid, 2 end) — needs the pace module
 *        armed ([debug] pace_receipt=1 or a swap gate), else 0;
 *   prev the previous iteration: ms = its wall, rend = its begin return -> frame end, eg = its end
 *        guard wait, gtail = its frame end -> its top swap request (the GPU tail), gap = vblanks
 *        between the two latest swap requests (2 = that frame was shown for two vblanks), dr = its
 *        [overlap] drain wait, tl = its tail+input+apt;
 *   tl   this iteration after its frame end: apt / inp ([c5cat] buckets, when the mprof clock runs),
 *        save = disk save tick, rpl = replay + core-1 probe, tele = periodic telemetry + net,
 *        hud = fps HUD + bottom menu, end = gstate + pacer;
 *   st   this frame's right-eye CPU (enter + kick chunks + end, µs) and kick chunks; dr its drain;
 *   why  gate: v >= 8 (a late swap cost a vblank at the begin); queue: q >= 2; tail: tl >= 3;
 *        cpu: the rest of the iteration >= 16.7.
 *
 * `[spike] sum f= n= spikes= gate= queue= tail= cpu= none= gtail_max= supp=` every 1024 iterations. */
#ifndef GDX3DS_SPIKE_H
#define GDX3DS_SPIKE_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX_SPIKE_MARK_SAVE0 = 0, /* before gdx_disk_save_tick */
    GDX_SPIKE_MARK_SAVE1,     /* after it */
    GDX_SPIKE_MARK_POST0,     /* the tail after the frame end starts */
    GDX_SPIKE_MARK_POST1,     /* after the replay iteration end + core-1 probe */
    GDX_SPIKE_MARK_TELE1,     /* after the periodic telemetry + net tick */
    GDX_SPIKE_MARK_HUD1,      /* after the fps HUD + menu tick */
    GDX_SPIKE_MARK_COUNT
};

/* Top of the loop body (next to gdx3ds_hitch_frame_begin). */
void gdx3ds_spike_frame_begin(void);
/* A tail sub-section boundary (main loop). */
void gdx3ds_spike_mark(int mark);
/* End of the loop body (next to gdx3ds_hitch_frame_end): classify, buffer the line. */
void gdx3ds_spike_frame_end(unsigned long frame);
/* Between iterations: one buffered line out, within the rate budget. */
void gdx3ds_spike_after_frame(unsigned long frame);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_SPIKE_H */
