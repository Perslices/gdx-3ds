# 09 — Effects, HUD, sky, and the integration milestone (Tier 2 items 3-6)

Track `fx`. Planning only — nothing here is implemented. Basis: read-only study of
`~/code/gdx-3ds/m1` at mainline `feat/3ds-hwaudio`, plus the master plan
(`00-master-plan.md` §2, §5 Tier 2 3-6, §6, §8) and the shipped lever receipts.

## 1. Goal

Take the last three per-frame emitters off the interpreter — the effects sprite stream,
the HUD/marker texrect stream, and the background — then merge the whole Tier 2 stack
(vendor → oracle → interp → machines → track → fx) and prove **locked 60 with stereo on
and FULL rival detail, on hardware**. This track owns the final gate; the other tracks
own their own parity.

## 2. Code facts

### 2.1 Effects (`decomp/src/game/racer.c`)

`Racer_Draw` (5553) is one 1,640-line function that emits the entire race scene.
211 `gEffectsVtxPtr` sites, all inside it. `GfxPool.effectsVtxBuffer[2048]`
(`decomp/include/sys.h:98,123`), re-based per frame at `ovl_i2/race.c:191-192`
(also `records.c:880`, `course_edit/19DD60.c:402`, `ending/ending.c:687`).

| class | site | verts | batching today | per-frame ceiling |
|---|---|---|---|---|
| shadows | 5939-6003 | 4 **static ROM** verts (`D_800CDBA4[shadowType]`) | 1 `gSPMatrix` + 1 `gDPLoadTextureBlock_4b` (I4 32x64) + 1 draw **per racer** | 30 + 3 ghosts |
| attack highlight | 6006-6060 | machine DL (no effects verts) | matrix + `gDPSetBlendColor` + machine DL per racer | 30 |
| boost flames | 6219-6465 | 4 verts (`unk_2B3==1`) or 5 (`==2`, adds a speed-stretched tip at 6296-6301) per booster, `boosterInfo->count` 1-4 | **already batched per racer**: one `gSPVertex(count*4or5)` + 3-6 `gSP2Triangles` (6309-6348) | 30 racers x 4 = 120 sprites, 30 draws |
| explosions (32-ring) | 6467-6562 | 4 | `gSPVertex(4)`+2 tris **per sprite**, plus a per-sprite `gDPLoadTextureBlock` of the animation frame (6509, 6556) and prim/env colors (6502-6506) | 2x32 |
| machine debris (128-ring) | 6564-6586 | 3 | `gSPVertex(3)`+1 tri per sprite | 128 |
| flying sparks (32) | 6587-6613 | 3 | same | 32 |
| collision sparks (32) | 6614-6660 | 4 | + `gSPTexture` LOD-tile change per sprite (6653) | 32 |
| smoke (32) | 6662-6700 | 4 | + `gSPTexture` per sprite (6693) | 32 |
| pit force field | 6701-6746 | 4 | per racer | 30 |
| fall explosion (32) | 6747-6855 | 4 (two passes: 6795, 6834) | + `gSPTexture` per sprite | 2x32 |
| countdown / lap sign | 6856-6962 | static verts | `gDPLoadTextureBlock` 32x32 RGBA16 + DL | few |
| position markers | 7010-7100 | ortho texrects | `gDPSetPrimDepth` (7018, 7070) + `gDPLoadTextureBlock` + texrect **per marker** | 3 + rivals |
| check markers | 7117-7160 | ortho texrects | `G_RM_XLU_SURF`, `G_CC_MODULATEIA_PRIM`, I4 4b tex per marker | few |

Realistic load sits well below the ceilings (the rings are only as full as the carnage),
but the crowd frames the profile calls out are the full ones. Two shapes matter:
**shadows and boost flames are already one draw per racer** (7 distinct shadow textures
across 30 draws + 30 texture imports); **the ring classes are one draw per sprite** with
a state change between most of them. Every class is preceded by a static segment-4 setup
DL (`D_4007FB8`, `D_4007FD8`, `D_4008000`, `D_4008028`, `D_4008130`,
`aSetupPitForceFieldDL`, `aSetupFallExplosionDL`) fixing combiner + render mode for the
whole class — **the game already sorts the classes by material**.

### 2.2 HUD (`ovl_i3/hud.c`, `ovl_i3/minimap.c`)

24 `gSPTextureRectangle`, 32 `gDPLoadTextureBlock`. The ROM textures are already
**strips** indexed by texrect UV offset — `aSpeedDigitsTex` 12x160 loaded once (745) then
3 rects at `s = digit*12<<5` (754); `aTimerSymbolsTex` 8x224 (1090/1203/1258/1326);
`aLapCounterSymbolsTex` 8x72; `aTotalRacerDigitsTex` 8x132 (802-820). Only one paletted
path: `gDPLoadTLUT_pal256(aReversePalette)` + CI8 112x2 slices (1789-1799, the REVERSE
marquee) — there is **no CI4 speedometer**, the digits are RGBA16. Portraits (1497-1525)
are 32x32 RGBA16 per racer per frame. Minimap: two 4b loads + 2 rects
(`minimap.c:261-265`) then per-player blips (281+). State via `D_8014940` / `D_80149A0` /
`D_80149D0`.

Measured (`docs/research/texrect2-progress.md` M1/M4, Azahar, gputrace):

| window | rects/frame | E4+E5 ms/f before atlas | after atlas |
|---|---|---|---|
| race steady | 39-42 | 2.14-2.24 | 1.66-1.68 |
| race start crowd (fr 3137) | 119 | 10.44 | 7.85 |
| results / rankings | 145-163 | 12.6 | 9.4 |
| 3D-model menu | 222-256 | 9.62 | 7.34 |

The atlas (`lus-trectbatch-atlas.patch` + `gfx_citro3d.cpp:1657-1840`) already removes
the per-rect batch break: 512x256 RGBA8 pages, cells ≤ 320x64, 1-texel replicated gutter,
≤ 8 pages, LRU eviction with view copy-out (`AtlasTryPlace`, 1681); receipts
`[trect2] tb=/merged/switched/bypassed/armed vt=` and `[c3d] atlasEv=`. Residual per-rect
cost: **import lookup 0.40 ms steady / 2.9 ms crowd** (content hash per rect) **+ ~25
us/rect in the 2x `GfxSpTri1` path**. That residue is what a native 2D batcher removes.

### 2.3 Sky / background (`ovl_i3/background.c`)

Per frame, per camera: 28 verts (`Background_UpdateSkyboxVtx`, 713) and **6 quads
total** — skybox 2 tris (1106), venue floor 4 tris (1127-1128), clouds 4 tris
(1150-1151), 3 texture loads. This is nothing.

Background sprites (`Background_DrawBackgroundSprites`, 1412): `sBackgroundSprites[6]`,
each costing a **TLUT load** (`gDPLoadTLUT_pal16/pal256`, 1439-1447, palette swaps built
at 1280-1310 / 1369-1382) + a CI4 64x64 `gDPLoadTextureBlock_4b` (1450) + 4 verts + 2
tris. Stars (`Background_DrawStars`, 1558): `sStars[100]`, `sStarCount = 100/gNumPlayers`
(1468), one texture load then **per star** a `gDPPipeSync` + `gDPSetPrimColor` + texrect
(1576-1584). Stars are therefore up to 100 rects/frame of the 119-145 crowd counts — the
single biggest rect population in the race, and *not* HUD.

Known geometry defect: the skybox quad is built at 4:3 (`aspectRatio = 0.75`) and
under-covers the 400x240 top screen, exposing the black clear colour — the sky wedge
(`docs/research/sky-backdrop-wedge-analysis.md` §2, `Background_Update:626`,
`UpdateSkyboxVtx:713`). Fix is vertical overscan in the decomp geometry, not a renderer
change; the DISP full-bleed/hybrid modes make it worse (`gfx_citro3d.cpp:1043-1047`).

### 2.4 Transition (`ovl_i2/transition.c`)

The one framebuffer readback: `Transition_SetBackgroundBuffer:713-714` calls
`gdx_read_current_framebuffer(gFrameBuffers[var_v0], 320, 240)`, then a scalar u16 copy
loop (725-731), optional greyscale/palette conversion, and
`gdx_set_native_rgba16_texture_range` on the capture and `sTransitionPalette` (749-761).
Port side `n64_gfx_bridge.cpp:12217`: on 3DS it captures from `GdxReadTopScanoutRgba16`
(top-LCD scanout, CPU-readable linear memory) because both the GPU mirror and a direct
fb0 display-transfer read returned scrambled VRAM at the race→menu teardown. Wipes draw
as ortho quads (`SmallTilesDraw:920`, `LargeTilesDraw:1060`, `TiledDraw:1288`).

### 2.5 Stereo, borders, dynlod (carried, not rewritten)

- Anchored markers: `gdx3ds_stereo.{h,cpp}` class `GDX3DS_STEREO_ANCHORED`; draw test
  `stereo_anchor && !texBacked && prg.cc.opt_prim_depth && bufVbo[3] == 1.0f`, depth from
  `mCurrentPrimDepth` (`prim_depth/0x7FFF`), receipt `anchor=<draws>/dmin/dmax`. **The one
  place a native path can silently regress stereo**: it needs ortho draws to carry prim
  depth and prim-depth changes to split batches.
- Border modes live only in the projection fixup: `sDispProjScene` / `sDispProjUi`,
  classified per draw by `w == 1` (`gfx_citro3d.cpp:1022-1112`). A native 2D path must use
  `sDispProjUi` and honour `DispModeFactors`.
- Dynlod (`gdx3ds_dynlod.c`, hooks `gdx_rival_detail_level()`) is unchanged here, but the
  final gate is defined at **FULL** detail, auto pinned at the floor.

## 3. Design

### 3.1 Native sprite batch (Tier 2 item 3)

Keep the game's vertex math **exactly** — the billboard basis is `camera->basis.y/z`
computed in C (6212-6218) and every quad is world-space integer `Math_Round` output. Do
what the track plan does: **sink the writes**, pointing `gEffectsVtxPtr` at a PICA-layout
`linearAlloc` ring instead of `GfxPool.effectsVtxBuffer`, so the stores land in GPU format
with no copy. Two constraints: the game reads the buffer back (`gEffectsVtxPtr - var_s7`
at 6308/6355; `course.c:4545` bounds-checks `effectsVtxBuffer[0x7E5]`), so the ring must
stay `Vtx`-addressable from the game side — otherwise the sink is a shadow write with
`Vtx` authoritative (**decide by measurement in M1**; the shadow write is the safe default
and still removes the interpreter's vertex walk). The bridge's byte-offset effects-batch
pairing is desktop-only (`GDX_INTERP_P1`, not built on 3DS), so no dependency there.

Batching: one draw per *class*, not per sprite —
`Gdx_BeginSpriteClass(setupDlId)` … append quads … `Flush()`. Within a class the only
per-sprite state is prim/env colour and, for explosions and the `gSPTexture` users, the
animation frame or LOD tile. Fold prim/env into **per-vertex colour** (the class
combiners are `MODULATEIA_PRIM`-shaped; verify each setup DL against the combiner census
and sub-batch-split where it is not expressible); put the 8-16 `D_800CDAB8[]` animation
frames (≤ 32x32 IA8) in one atlas page at load and select by UV, so all 32 explosion
sprites become one draw.

Expected shape: **~8 draws/frame** for the effects stream (one per class) plus ≤ 7 for
shadows batched by `shadowType`, replacing up to ~380 draws with a state change between
most of them. Shadows go native as *instanced quads*: 7 static 4-vert meshes,
per-instance matrix and prim colour, transformed on the CPU (≤ 132 verts/frame), keyed by
`shadowType` — killing 30 imports of the same 7 I4 32x64 images.

### 3.2 HUD → 2D batcher (Tier 2 item 4)

Do **not** rewrite the atlas; reuse `AtlasTryPlace`/views wholesale. The native batcher
replaces only what sits above it: instead of the interpreter turning each texrect into two
`GfxSpTri1` calls plus a content-hash import lookup, it resolves each ROM texture pointer
**once per page** to a view id and appends 4 packed verts. Targets: the 0.40/2.9 ms import
lookup and the ~25 us/rect triangle path.

- Rects keep `w == 1`, so UI-vs-scene classification, border matrices and
  `GDX3DS_STEREO_UI_ZERO_PARALLAX` keep working unchanged.
- **Prim-depth markers keep their own draw.** The batcher must break on a prim-depth
  change exactly as the interpreter does and publish `mCurrentPrimDepth` the same way.
  Non-negotiable; verified by the `anchor=` receipt.
- TLUT: only the REVERSE marquee (CI8 + `pal256`) and the background sprites (CI4/CI8 +
  `pal16/pal256`). Expand to RGBA8 at import into an atlas cell keyed by (texture pointer,
  palette hash) — background palettes are swapped at runtime (`background.c:1369-1382`).
- Stars: 100 rects, one shared 8x8 IA8 texture, per-star prim colour → one draw with
  per-vertex colour and zero state changes. The best rect win in the race.
- Transition: leave the readback exactly as-is (scanout capture is the only path proven
  correct on hardware). Wipe quads become native 2D draws; the captured 320x240 RGBA16
  becomes a directly-uploaded texture instead of a per-frame import.

### 3.3 Sky / background (Tier 2 item 5a) — **keep on the interpreter**

6 quads + 3 texture loads per camera per frame — well under 0.1 ms even at the
interpreter's worst measured per-draw cost. Porting it buys nothing and risks the sky
wedge, the fog blend (`gfx_citro3d.cpp:1367` `wantFogBlend`, explicitly "do not touch")
and the venue-floor/cloud UV scroll. **Decision: not native.** The background *sprites*
(6, TLUT each) and the *stars* (≤100 rects) move to the 2D batcher because they are 2D
work, not because they are background.

### 3.4 Shadows and boost tint (Tier 2 item 5b)

- Shadows: **native**, instanced quads (§3.1); z-tested decals (`G_RM_ZB_XLU_DECAL`,
  5942), order-safe against each other given append order.
- Attack highlight ("boost tint") is a *machine* draw — matrix + `gDPSetBlendColor` + the
  same `D_800CDDB0` LOD DL (6023-6030) — not an effect. **Owned by the machines track**,
  which bakes those DLs; fx only asserts the blend-colour slot is a bakeable material
  parameter. Same for the Time Attack ghost pass (6189-6212, env alpha 160) and the
  GP-end-cutscene lookat/scroll pass (6131-6163).

### 3.5 The long tail — explicitly on the interpreter, forever

`menus.c` (357 sites), course editor (~500), EK screens, ending, records, machine
creator. Are they at 60? **We do not know on hardware, and this track must find out.**
What is known: the 3D-model menu is the heaviest rect frame measured anywhere (222-256
rects, E4 9.62 → 7.34 ms post-atlas) and rankings is 145-163 rects at 12.6 → 9.4 ms —
Azahar ms with gputrace on, not fps. M6 measures menu/results/records/EK fps on hardware
with the trace off and states a verdict. If a long-tail screen misses 60 the fix is the
generic 2D batcher, which already covers those screens because it sits under the
interpreter's texrect opcode — not a native menu path.

## 4. Draw-order analysis

Order today is command order, and the render modes say which classes are order-sensitive.

- **Opaque, z-tested** (machines 6114-6130, track): order-independent among themselves.
- **Z-tested decals** (shadows, `G_RM_ZB_XLU_DECAL`/`DECAL2`, 5942): after the track,
  before translucent effects. Among themselves they overlap only when ships stack, and
  the alpha is a constant 200, so A-over-B ≠ B-over-A: keep the game's `sLastRacer`→
  `gRacers` order inside the batch (append order gives this for free).
- **Translucent, z-tested** (every effect class; the setup DLs are XLU with `Z_CMP`):
  **not** order-independent. The inter-class order (explosions → debris → sparks →
  collision sparks → smoke → force field → fall explosions → markers) is fixed, and
  within a class the ring is walked newest-first. Per-class batching reorders nothing —
  it only removes state changes *inside* a class, which is why that is the right grain.
- **Interleaving with machines**: effects draw strictly after all machines in
  `Racer_Draw`, so native effects and native machines never interleave; the only
  obligation is "flush machines before the first effect draw".
- **Screen-space** (markers, check markers, HUD, stars): last. Markers use
  `gDPSetPrimDepth` + `G_ZS_PRIM` and *are* z-tested at a chosen depth; check markers
  clear `G_ZBUFFER` (7117). HUD order matters (portraits under highlight borders,
  `hud.c:1497-1525`) and is preserved by append order.

Rule for implementation: **one flush boundary per class, batches append in command
order, never sort.** Any reordering claim needs a frame-hash receipt, not an argument.

## 5. Milestones

Agent-days at the observed cadence. Every milestone lands behind an ini key with a
receipt.

**M0 — census (1 d).** Counts the docs lack: effects sprites and draws per frame by
class, stars/frame, marker draws/frame. Receipt
`[fx] cls=<name>:<sprites>/<draws> … stars= mark= tlut=`, gated like `[trect2]`.
*Verify:* steady / race-start crowd / rankings / menu windows on Azahar plus one hardware
run — the baseline every later ms claim is measured against.

**M1 — sprite sink + ring (3 d).** PICA-layout effects ring, `gEffectsVtxPtr` retarget,
shadow-write-vs-true-sink decision recorded. `[native] fx_sink`.
*Verify:* frame-hash parity on the replays with the batcher still off (the sink alone must
be byte-transparent) + host differential harness on `Racer_Draw`'s emitted vertex set.

**M2 — per-class sprite batcher (4 d).** 8 class batches, shadow instancing, explosion
frame atlas. `[native] fx_sprites`, receipt `[fx] draws=<before>/<after> merged=
split=<reason>`. *Verify:* frame hash on replays; hardware `[prof]` A/B with gputrace on
**and** an fps window with it off; a counter proving no class reordered.

**M3 — 2D batcher (4 d).** Native texrect path over the existing atlas: per-page view
resolution, prim-depth splits, TLUT-keyed CI expansion, stars, background sprites,
transition wipes. `[native] fx_hud`, receipt extends `[trect2]`. *Verify:* byte-identical
menu SHOTs (`m1`-`m5`, the deterministic ones from the texrect work);
`anchor=<draws>/dmin/dmax` stable run-to-run in stereo; `[profop]` E4+E5 import-lookup ms
→ ~0.

**M4 — sky/shadow/tint decisions closed (1 d).** Sky stays interpreted with a measured ms
justifying it; shadows land in M2; the attack-highlight hand-off is written down and
accepted by the machines track.

**M5 — integration (4 d).** `[native]` section, merge order, DBG rows, receipts,
TEST-PLAN cards (§6).

**M6 — long-tail measurement + final gate (3 d).** Hardware fps for menus, results,
records, course editor, EK; then the gate run (§6.3).

Total ≈ 20 agent-days, matching the master plan's "effects+HUD 1-2 weeks + integration
1-2".

## 6. Integration

### 6.1 The `[native]` ini section

```ini
[native]
all       = 0   ; master: 1 = every native path on (overrides the four below)
interp    = 0   ; Tier 1 own interpreter (track: interp)
machines  = 0   ; baked machine meshes (track: machines)
track     = 0   ; direct-VBO tessellation (track: track)
fx_sink   = 0   ; effects vertex ring in PICA layout
fx_sprites= 0   ; per-class sprite batches + shadow instancing
fx_hud    = 0   ; native 2D batcher (HUD, stars, markers, wipes)
```

Read once at boot (like `trectbatch`/`renderthread`); default 0 until each track's gate
passes, then flipped to 1 one track at a time. `all=1` is the release configuration and
what the final gate runs. Every key is a killswitch: `0` restores the exact prior path,
and each track must keep that true through its own gate.

### 6.2 Merge order, receipts, DBG

Merge order is the dependency order: **vendor → oracle → interp → machines → track →
fx**. `fx` is last because the 2D batcher sits on the interpreter's texrect opcode, the
sprite sink shares the track plan's ring-VBO machinery, and the shadow/attack-highlight
split needs the machine bake to exist. Each merge fast-forwards onto `native/main` only
after that track's own gate; `fx` re-runs the full replay set after every upstream merge
and owns the bisect if a hash moves.

Receipts (all on the existing gputrace/verbose gate, drained at the `[race-dl]` cadence):
`[fx]` per §5, `[trect2]` extended, `[c3d] atlasEv=`, `[native] all= interp= machines=
track= fx=<sink|sprites|hud bitmask>` printed once at boot so every log says what was on.

DBG tab (`port/3ds/gdx3ds_menu.c` `PaintDbg`, currently rows 4-18 with 20-21 as the
footer): add row 22 `(x) NATIVE     interp/mach/trk/fx (reboot)` showing a 4-glyph state,
and row 24 `(x) NATIVE FX  sink/sprites/hud (reboot)`. Same `x`/`-`/` ` tri-state idiom
the LEVERS row already uses. Footer moves to 26-27.

TEST-PLAN cards: one `TEST-PLAN.txt` per staged SD card at
`~/Desktop/G-Diffuser-3DS-<name>/` with build id + SHA256, the ini, the autoinput script,
the four windows to capture (menu, race steady, race-start crowd, results), the receipt
lines to look for, and the pass/fail line. Cards are additive — the fx card also runs the
machines and track cards.

### 6.3 Final gate

Configuration: `[native] all=1`, `[stereo] enabled=1`, `[perf] rival_detail=0`
(FULL) with `rival_detail_auto=0` so the tier cannot rescue the frame, `[debug]
gputrace=0`, New 3DS hardware, 30-machine GP.

| metric | how measured | pass |
|---|---|---|
| fps | `fps` log lines over the run, percentiles computed offline | median ≥ 59.0, p10 ≥ 58.0, no window below 55 |
| render-thread cost | `[prof]` buckets (BR/RUN/VTX/TRI/IMP/DRW/MTX) in a separate gputrace-**on** run, same replay | crowd-frame total ≤ 9 ms, drw+imp ≤ 2 ms |
| rects | `[trect2]` / `[profop]` E4+E5 | crowd E4+E5 ≤ 2 ms |
| effects | `[fx] draws=` | ≤ 20 draws/frame in the race-start crowd window |
| stereo | `anchor=<draws>/dmin/dmax` + a two-eye capture | markers sit on their ship; HUD at zero parallax |
| soak | 30 min continuous GP | zero error lines (gfxfail/gdl-bad/gdl-miss/datafail/fatal/bad_alloc/texdiag), heap plateau flat, no `[heap-watch]` trigger |
| lifecycle | HOME → sleep → resume → close, three times | clean, no VRAM/linear leak in `[c3d]` |

Two runs are always required because gputrace inflates the profile: an fps run with it
off and a bucket run with it on. Every perf claim in the report cites the window and the
trace state, per PROTOCOL.

### 6.4 Docs

`docs/3DS-HARDWARE.md`: §5 gains the `[native]` block with the same commented-defaults
treatment `[debug]` has; §7 loses the crowd-fps caveat and gains "report any effect that
draws in front of something it should be behind, with the `[fx]`/`[trect2]` lines"; §8
gains one troubleshooting row — "visual glitch after the native renderer → set
`[native] all=0`, relaunch, report whether it persists". Release notes: a "Native
renderer" section naming the keys, the measured before/after hardware fps, and an
explicit statement that Old 3DS is still not supported.

## 7. Old 3DS — what is still missing after Tier 2

Tier 2 is necessary, not sufficient.

- **CPU.** 268 MHz single-issue vs New 3DS 804 MHz + L2, and no spare core: the render
  thread (`[debug] renderthread=1/2`, worth median 49→56 fps on New 3DS) must fold back
  into the main loop, giving up the `max(logic, render)` prize. Nothing here measures
  that; it needs a real Old 3DS.
- **Memory, 64 MB APPLICATION region** (`docs/research/3ds-memory-budget.md` §1). Static
  budget today is ~71-87 MB → **does not fit**. Required diet: `audio_table` is 10.7 MB
  and is now stored *uncompressed* in `fzerox.o2r` precisely to fix boot stalls
  (`bootaudio2-progress.md:83`) — that trade reverses on Old 3DS and must be re-solved
  (stream from SD, or accept the stall); the LUS resource cache is unbounded and needs a
  byte-accounted cap (~8 MB vs the 24 MB budgeted for New); the texture set becomes
  per-race instead of resident; fiber stacks are 7 MB at 1 MB defaults and should be
  64-128 KB (`n64_sched.c:223`); the atlas drops from 8 x 512 KiB linear to 2-3 pages.
- **Unknown, needs hardware we may not own:** fps at 268 MHz with all native paths on;
  whether 6 MB VRAM holds stereo targets *and* atlas pages; whether the o2r read path's
  transients fit; whether `.cia` extended memory (80/96 MB) is acceptable to ship. The
  memory report's own recommendation was to **formally drop Old 3DS**, and nothing since
  has changed that. Treat it as a post-Tier-2 milestone whose first week is measurement,
  and do not promise it in release notes.

## 8. Risks

1. **Prim-depth stereo regression.** A batcher that merges across a prim-depth change
   un-anchors the markers and still looks fine in mono. Mitigate: the split is a hard
   invariant with its own counter, plus a two-eye capture in the gate.
2. **Effect draw-order regression.** Translucent classes are not order-independent; a
   batcher that hoists or sorts yields a plausible-looking wrong frame. Mitigate:
   append-order-only batching, per-class flush boundaries.
3. **Per-sprite colour not expressible in a class combiner.** Mitigate: sub-batch splits,
   at a cost M2's `split=<reason>` receipt makes visible.
4. **Ring aliasing.** `course.c:4545` bounds-checks the course pointer against the
   *effects* buffer; a retargeted ring can silently change overflow behaviour. Mitigate:
   keep the game-visible `Vtx` array authoritative until M1 proves otherwise.
5. **Atlas pressure.** Stars, background sprites and explosion frames land in a store that
   already hit `full=22/pages=8` in one session before eviction existed. Mitigate: watch
   `atlasEv=`, raise `kAtlasMaxPages` only with a linear-heap receipt.
6. **Scope creep into the long tail.** Menus stay interpreted; M6 measures, it does not
   port.
7. **Sky wedge blamed on the fork.** It predates the fork and is decomp geometry — say so
   in the release notes.

## 9. Dependencies

- **Blocking:** vendor (Phase 0), oracle (Phase 1 — frame hash, replay, capture: without
  it none of the parity claims here are checkable), interp (Tier 1, for the texrect
  opcode hook), machines (for the attack-highlight/ghost passes), track (for the ring-VBO
  machinery M1 reuses).
- **Consumed unchanged:** `gfx_citro3d.cpp` atlas (1657-1840), stereo classes and
  `sDispProjUi`/`DispModeFactors` (1022-1112), `gdx3ds_dynlod.c`, `gdx3ds_gpu_prof.h`
  buckets, `GdxReadTopScanoutRgba16`.
- **Hands off to others:** attack highlight, Time Attack ghost pass, GP-end cutscene
  scroll pass → machines track. Sky wedge fix → whoever owns decomp geometry; not fx.
- **Human-in-the-loop:** every hardware verdict (SD card staging per PROTOCOL), one
  reference lap per course for the replay files, and the subjective stereo-comfort call.

## 10. First 10 commands

```
1  <scratch>/native/bootstrap.sh fx native/fx
2  cd ~/code/gdx-3ds/fx && git log --oneline -3 && ls docs/research/native/
3  grep -n 'gEffectsVtxPtr' decomp/src/game/racer.c | awk -F: '{print $1}' | head -40
4  sed -n '6219,6470p' decomp/src/game/racer.c        # booster batch: the one already-batched class
5  sed -n '1657,1860p' port/3ds/gfx/gfx_citro3d.cpp   # AtlasTryPlace + view/eviction contract
6  grep -n 'GfxSpTri1\|texrect\|TextureRectangle' libultraship/src/fast/interpreter.cpp | head -30
7  grep -n 'opt_prim_depth\|mCurrentPrimDepth\|ANCHORED' port/3ds/gfx/gfx_citro3d.cpp port/3ds/gfx/gdx3ds_stereo.cpp
8  cat port/3ds/patches/README.md | sed -n '1,60p'    # patch order before adding an fx patch
9  DEVKITPRO=/opt/devkitpro cmake -S . -B build-3ds -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake -DGDX_PLATFORM_3DS=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build-3ds --target G-Diffuser-3DS_3dsx -j8
10 # M0: add the [fx] census counters behind the gputrace gate, build, run the Azahar
   #     30-machine GP replay under the /tmp/azahar.lock protocol, snapshot to /tmp/fx-art/
```
