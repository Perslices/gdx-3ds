# track — progress (native course geometry path)

Branch `native/track`, worktree `~/code/gdx-3ds/track`. Plan: `08-track-plan.md`. Protocol:
`<scratch>/native/PROTOCOL.md`. Killswitch `[native] track` (0 off / 1 native / 2 verify),
census `[native] trkcensus=1`, receipts `[trk]`.

## State

- M0 census: measured — 4P demo race and 1P 30-machine GP (below).
- M1 shim + decomp patch: DONE (0fdf062; roundtrip clean; pass-through default).
- M2 sink / templates / host test: DONE (025a22d, 0f888b5; templates decode from the seg-8 carve).
- M3 replay + Gdx_NativeBatch + LUS marker patch: DONE (0f888b5, 20bcc2a), renders correctly in 4P split screen.
- M4 parity: DONE in emulator — 13,878 (4P demo) + 380 (1P GP) runs verified bit-identical, 0 mismatches.
- M5: emulator profile DONE (≈ −0.9 ms/frame 1P GP, −1.3…−1.7 4P demo, gputrace on); hardware card staged at ~/Desktop/G-Diffuser-3DS-track (needs a human run).

Commits: 025a22d (templates + host test), 0fdf062 (shim/recorder/decomp patch),
0f888b5 (replay/native batch/LUS patch).

## Design as built (deviations from 08-track-plan.md, with reasons)

1. **Record at game time, replay at the display-list position.** `Course_Draw` runs on the game
   thread while the interpreter walks the list later (render thread when `renderthread=1`), so
   the sink cannot issue PICA draws from `course.c`. The shim records `{VTX ptr,n,slot | MOD
   slot,val | DL template}` ops into a per-GfxPool record and writes ONE `G_NOOP` marker
   (`p=0x5A`, w1 = record) into the game's own Gfx stream wherever a material-state command is
   about to follow (and at pass end). The interpreter's G_NOOP handler (LUS patch) hands the
   marker to `Interpreter::GdxTrackMarker`, which replays the run's ops at exactly the position
   the original VTX/DL commands occupied — painter's order (Z_CMP off) is preserved by
   construction, and the ≤24 texture loads, `gSPTexture`, prim/fog and the two setup DLs stay
   real GBI, so the interpreter's tile/TMEM state for the gadget pass is untouched (M3 hygiene
   for free).
2. **Vertex cache = the interpreter's own `loaded_vertices`.** Plan §3.2 proposed a 16-slot
   mirror; the tunnel-join templates reference slots 16-20 (`course.c:3575` loads 5 verts at
   slot 16), and `GfxSpVertex`/`GfxSpModifyVertex` ARE the RSP cache mirror. Replay calls them
   directly with the game's `courseVtxBuffer` pointers, so the transform, `AdjXForAspectRatio`,
   ST scaling, clip flags and the fog factor are the interpreter's own code — plan §3.3's
   "convert the matrix the way the interpreter does" parity trap disappears (it IS the
   interpreter's `MP_matrix`).
3. **Per-triangle: exact first tri, fast rest.** The first tri of every run (and any tri needing
   the clipper) goes through `GfxSpTri1` so the draw-state resolve (shader, texture import,
   depth/alpha, pack params, aux constants) is the interpreter's; the remaining tris use
   `GdxTrackTri` (patch member, verbatim copies of the trifast expressions: reject box, cull
   cross product, packed 12-float layout, `GdxFminExact` UV clamps). Tris are appended to a
   `Gdx_NativeBatch` (backend, same linear VBO pool) and flushed as ONE draw per material run
   (and per mid-template SETCOMBINE segment). VBO bytes are bit-identical to the stock path
   by construction; `[native] track=2` proves it per run, in-process (below).
4. **Parity oracle without determinism pins.** In-race runs are not reproducible yet (oracle
   O6/O7 pending), so a two-run SHOT compare cannot be expected identical even stock-vs-stock.
   `track=2` (verify) emits the stock GBI AND the record; a start marker snapshots the vertex
   cache, the end marker restores it, replays the run into the native batch, hashes the batch
   and DISCARDS it, and compares against the hash of the packed draws the stock path submitted
   for the same run. Receipt: `[trk] verify frame=N runs=K ok=K`. SHOT byte-compare is kept as
   the visual sanity check; `[fh]` will slot in unchanged once the oracle merges.
5. **18 distinct templates, not 20.** `func_800A4BAC` wires 20 slots but `D_80140F0`/`D_8014138`
   are each used twice. Decoded at first use from the asset-bound arrays (`port/gen/
   AssetBindings.c`: `u8 D_80140F0[0x48]` …) with the same decoder the host test runs over
   the committed raw blobs; a mismatch against the committed golden table refuses to arm.
6. `Course_DrawModel` is not wrapped with push/pop_macro: the shim macros forward to the real
   GBI whenever `gdxtrk_active == 0`, and DrawModel only runs on the course-select screen.

## Results 2026-09-12 (RetroArch + Azahar libretro core lane, `gdx-dev retro run`, build 20bcc2a)

The `retro run` subcommand does not stage `--replay` (gdxdev/retro.py:363 `replay=None`), so
these three sessions ran the game's attract-mode **4-player split-screen demo race** (Mute
City, 4 viewports: `Course_Draw` runs 4x per frame, cameras 0-3). That scene exercises the
viewport-fold / split-screen risk (plan §6.4) harder than a 1P race; the 30-machine GP crowd
lap is a second set of runs with the replay staged by hand (below, when complete).

**M4 parity (verify, `track=2`, run `20260912-231637-verify1`, 660 s, frames 630-69773):**
1387 sampled frames, `vok=13878 vbad=0 fb=0`, zero `[trk] verify MISMATCH`, zero
`[fatal]/[gfxfail]/[gdl-bad]`. 851,554 template triangles walked, 344,428 appended (205,064
fast path + 139,364 exact), every run's native packed VBO bytes == the stock draws' bytes.
Mechanism: in-process per-run FNV-1a over the packed float range (cache snapshot/restore at
the START/END markers), see Design §4. This is the bit-identical claim; SHOT byte-compare
between separate runs is not meaningful until the oracle's determinism pins land.

**M0 census (same runs; 1388 `Course_Draw` samples, every 64th call, 4 viewports):**
chunk loads 4-52 (mean 15.1), verts mean 136 (max 456), template tris 64-832 (mean 244),
material runs 1-24 (mean 3.9), ModifyVertex mean 16 (max 56), gSPTexture mean 4.3, LoadBlock
mean 3.4 (max 16) — per viewport. Per frame (replay receipts): 10.0 runs, 37.9 chunk loads,
342 verts, 614 template tris walked, 248 appended (148 fast / 100 exact — the exact share is
the interpreter clipper: track tris crossing the screen edges), 0.46 SETCOMBINE segments.

**M5 emulator profile (`gputrace=1`, runs `20260912-231658-base1` track=0 vs
`20260912-231700-native1` track=1, run concurrently on identical private instances, ~1105
in-race windows each (nT>=150), `tools/dev/prof_ab.py`):**

| bucket (ms/frame) | track=0 | track=1 | delta |
|---|---|---|---|
| br  | 4.51 | 4.15 | -0.36 |
| dsp | 12.29 | 11.67 | -0.63 |
| vtx | 1.36 | 1.34 | -0.02 |
| tri | 6.45 | 5.79 | -0.65 |
| imp | 1.64 | 1.64 | 0.00 |
| drw | 0.96 | 0.94 | -0.02 |
| [profop] 06 TRI2 | 11.02 | 5.37 | -5.65 |
| [profop] 01 VTX | 2.53 | 2.08 | -0.45 |
| [profop] 00 (marker replay) | - | 5.03 | +5.03 |

Net ~1.3-1.7 ms/frame on the render thread in the emulator with gputrace on (absolute numbers
are emulator+JIT numbers; only the delta means anything). Native receipts: 9.3 draws/frame
for 10.2 runs (one draw per run + SETCOMBINE segments), fallback=0, zero error lines.
Where the replay's 5.0 ms goes: GfxSpVertex for ~38 loads/frame (still carries the
interpreter's per-vertex diagnostics), ~100 exact tris/frame through GfxSpTri1 (clipper), and
the fast path. The plan's 2.5-4.0 ms estimate assumed 50-70 chunk draws in 4-15 runs on a
crowd lap; hardware is the truth (card staged, TEST-PLAN.txt).

**30-machine GP, 1P (replay `race.txt` staged into the private SDs by hand; runs
`20260912-233108-verify2`, `-233111-base2` (track=0), `-233113-native2` (track=1); race band
= game frames ~8000-16183, `[gpu] md=18`):**

- M4 verify: 138 sampled frames, `vok=380 vbad=0 fb=0`, zero MISMATCH, zero error lines.
- M0 census (1P, camera 0): chunk loads mean 16.3 (max 26), template tris mean 261 (max 416),
  material runs mean 2.8 (max 20). Per frame: 2.8 runs, ~259 tris walked, 99 appended
  (57 fast / 42 exact), 2.3 native draws.
- M5 profile, race band only (218 windows each, `prof_ab.py --min-nt 100` over the `[prof]`
  lines between the first and last census line): br 4.69→4.48 (−0.21), dsp 12.72→12.42
  (−0.29), vtx −0.02, tri 6.87→6.44 (−0.43), imp 0.00, drw +0.04 → **≈ −0.9 ms/frame**
  exclusive-bucket total; `[profop]` 06 TRI2 6.93→4.72, 01 VTX 1.38→1.21, 00 marker replay
  2.83 (inclusive of its GfxSpVertex/GfxSpTri1/flush children, so not additive with the
  buckets). nT drops by ~90 because native tris bypass the interpreter's tri counter.
- The 4P split-screen demo (4 track passes/frame) gave ≈ −1.3…−1.7 ms/frame; the win scales
  with track passes and chunk count, and the fixed per-marker cost (GfxSpVertex per load,
  exact tris through the clipper) is what to attack next.
- SHOT byte-compare on the RetroArch lane: NOT usable — the guest `_scan.bmp` is a stale
  scanout there (all four SHOTs show the machine-settings screen while `md=18` and the census
  prove the race ran); base-vs-native differed by 17-18 kB on three SHOTs and 0 on one, which
  is just "which stale frame". Use `[trk] verify` (exact) or the oracle's `[fh]` when merged.

## M0 numbers (first attempt, Azahar Qt lane)

Mechanism: `[native] trkcensus=1` with `track=0` (pass-through; the shim counts what the
native path would record, per `Course_Draw` call, every 64th call). Azahar, build 0fdf062,
30-machine GP Jack Cup / Mute City, run `/tmp/track-art/m0census` (2026-09-12 22:54).

| window | chunk loads (VTX) | verts | template DLs | tris | ModifyVertex | material runs | gSPTexture | LoadBlock |
|---|---|---|---|---|---|---|---|---|
| intro-card track preview, game frames 4017-5105 (18 samples) | 15-22 | 128-184 | 15-22 | 240-352 | 0-8 | 1-2 | 3-5 | 1-2 |

These are the course-intro **preview** frames (the run's SHOTs show the "1: MUTE CITY" card —
the script's 2980-tick A holds never registered as new presses, so driving never started).
They already show the shape that matters: ONE template call per chunk load (dl == vtx, always),
16 tris per template, and only 1-2 material runs on this course. In-race crowd-lap numbers:
pending the press-based script (`autoinput-race.txt` v2). The plan's "50-70 chunk draws,
4-15 runs" estimate is high for Mute City; the per-frame cost is dominated by the ~2×N
interpreter commands (VTX + DL per chunk) plus 16 TRI2 executions per chunk, exactly what the
marker replay removes.

## ini keys, receipts, artifacts

- `[native] track = 0|1|2` — 0 pass-through (default), 1 native replay, 2 verify (stock drawn,
  native replayed into a discarded batch and hash-compared per run).
- `[native] trkcensus = 0|1` — `[trk] cam=.. mode=.. pool=.. frame=.. vtx= verts= dl= tris= mod=
  runs= state=tex/loadsync/settimg/loadblock realdl= ovr=replace/noop fb= ops=nOps/nRuns` every
  64th Course_Draw, plus `[trk] replay(prev) markers= runs= vtx= verts= tris= fast= exact= draws=
  comb= fb= vok= vbad=` (the previous frame of that pool, written by the interpreter thread).
- `[trk] init mode= census= templates(carve)=18/18 templates(arrays)=0 seg8=..` once; a carve
  decode mismatch forces track=0 (`[trk] templates failed verification`).
- `[trk] verify MISMATCH frame= run= stock=(tris,hash) native=(tris,hash) fast= exact= fb=`
  (first 12) when `track=2` finds a run whose native VBO bytes differ from the stock draws.
- Run tooling (scratch, not committed): `run_azahar.sh <name> <3dsx> <max s> <done SHOT>
  <sec.key=val ...>` merges ini keys, installs `$AUTOINPUT`, holds the lock, snapshots to
  `/tmp/track-art/<name>/`.

## API (shared with the machines track) — port/3ds/gfx/gdx3ds_nativebatch.h

```c
int      Gdx_NativeBatchBegin(int stereoClass);                  /* adopt current backend state */
int      Gdx_NativeBatchBeginWithAux(const Gdx3dsVboPackAux*, int stereoClass);
int      Gdx_NativeBatchMaterial(const Gdx3dsNativeMaterial*, int stereoClass); /* explicit state */
float*   Gdx_NativeBatchCursor(void);           /* next packed slot, stride GDX3DS_VBOPACK_STRIDE */
float*   Gdx_NativeBatchVerts(uint32_t nVerts); /* reserve; NULL = pool exhausted */
void     Gdx_NativeBatchCommit(uint32_t nVerts);/* another writer filled the cursor */
uint16_t* Gdx_NativeBatchIndices(uint32_t n);   /* NULL: indexed draws not built (plan M4) */
uint32_t Gdx_NativeBatchFlush(void);            /* ONE draw for [start, cursor) */
uint32_t Gdx_NativeBatchDiscard(uint32_t* hash);/* verify: hash + drop */
void     Gdx_NativeBatchEnd(void);
void     Gdx_NativeBatchStockHashArm(int on);   /* verify: hash every packed backend draw */
uint32_t Gdx_NativeBatchStockHash(uint32_t* tris, int reset);
```
Delta vs plan §3.7: `Material()` takes the interpreter's shader ids + backend texture ids +
depth/decal/alpha flags + an aux record instead of a bespoke material struct, and `Cursor/
Commit` exist because the track adopts triangles the interpreter appended at the pool cursor.
The pool is the per-frame `mVboPool` (reset each StartFrame); the machines track's persistent
VBO is the one extension not built here. `Material()` is exercised by nobody yet — the track
inherits state from the interpreter — and its caller must keep `Interpreter::mRenderingState`
consistent (see the header comment).

## Gotchas

- `gSPVertex(sCourseDisp - 1, …, 5, 16)` (`Course_SetupChunkPipeTunnelJoinVtx`) OVERWRITES the
  previous slot: either the 8-vertex load just recorded (→ replace the last op) or a
  `gSPTexture ON` (→ the stock game drops that command; the shim NOOPs the slot). Detected by
  whether the packet expression advanced `sCourseDisp`.
- Autoinput: a long `A <hold>` never re-presses. The course-intro card needs a fresh press, so
  the race script issues 500-tick holds with 40-tick gaps for the whole run (max 64 events).
- `gdx_dbg_logf` is not line-budgeted (it is `gdx_port_vlogf`); the census "stopping" was the
  game leaving `Course_Draw` (intro card), not the logger.
- The bound `D_8014*` arrays (port/gen/AssetBindings.c) are identity placeholders — never
  filled. The template bytes are in the segment-8 carve: `gdx_rdram + gSegment16C8A0VramStart +
  0x140F0…` in N64 word order (the decoder sniffs word order from G_ENDDL).
- Static-archive order: `interpreter.o` (libgdx3ds_lus_carve.a) references the port's marker
  entry, which lives in libgdx3ds_gfx.a. The hook is a weak C symbol and gfx_citro3d.cpp
  carries a strong reference (`gdx3ds_track_replay_anchor`) so the object is always pulled.
- `gdx-dev retro run` ignores `--replay` (gdxdev/retro.py:363); copy the script to
  `~/Library/Application Support/gdx-retro/<instance>/saves/Azahar/Azahar/sdmc/gdx-autoinput.txt`
  yourself. Without it the game runs the attract demo (a 4P split-screen race), which is a
  fine track-heavy scene but not the GP. Its `shots/` re-collects the instance's old
  screenshots; the guest `_scan.bmp` is stale on that lane.

## Next steps

1. Hardware: run the card (`~/Desktop/G-Diffuser-3DS-track`, TEST-PLAN.txt) — runs A/B/C/D.
2. Cut the replay's fixed cost: a lean vertex load (no diag hash / geodiag branches) and a
   port of the clipper into the fast path so the ~40 % "exact" tris stop paying GfxSpTri1.
3. Indexed draws (plan M4): `C3D_DrawElements` + 16 verts/48 indices per chunk.
4. Switch the parity gate to the oracle's `[fh]` replays once `native/oracle` merges.
