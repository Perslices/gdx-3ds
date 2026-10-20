import json, os
from framework import *


def build(D):
    O = json.load(open(os.path.join(os.path.dirname(__file__), "o3ds-data.json")))
    cards = O["cards"]; cats = O["cats"]; A = O["agents"]
    b = []
    # The leading "05 ·" below is cosmetic placeholder text: framework.page()/number_eyebrow()
    # overwrites it at build time to match this page's real position in PAGES.
    b.append('<p class="eyebrow">05 · Old 3DS</p><h1>Old 3DS: the road to a real 60</h1>')
    b.append('<p class="lede">On 08-11 the first feasibility study said this port was New 3DS only: the Old 3DS memory '
             'region could not hold it. Six weeks later the same game boots on an Old 3DS (268 MHz, no L2 cache), races at about '
             '10 fps, and eleven days after that an internal development card holds 59–60 fps (245–256 of 256 frames on the '
             'first vblank) in Grand Prix races with the 3D slider up — on an internal development card, not the shipped v1.0.0 build. '
             'v1.0.0 now ships that same lever set as its default, but a separate real-hardware confirmation pass for the shipped '
             'binary\'s own 3D frame rate has not been done; see the note below and RELEASE-NOTES.md / STATUS.md for the current, '
             'authoritative status. This page is that road, card by card, with the number each hardware run actually logged.</p>')
    b.append(tiles([
        ('~10<small>fps</small>', "first Old 3DS race, 09-29 (wall p50 96 ms; the user said \"like 9fps\")", False),
        ('16.5<small>ms</small>', "race median frame time on card 3, 10-02, 2D, 0 race hitches", True),
        ('59–60<small>fps</small>', "3D Grand Prix race windows on card 4, 10-08, an internal development card (245–256 of 256 frames on the first vblank); v1.0.0 ships that same renderer family on by default — see the note below", True),
        ('0', "bad draws in the hardware verify mode: 199,472 checked on card 2, and again (count not logged) on card 4", True),
        (esc(str(len(cards))), "hardware cards and runs logged between 09-28 and 10-08", False),
        (esc(str(A["hw_logsets"])), "physical SD-card round trips logged for the Old 3DS campaign — no network loader on this console, so every test was a hands-on trip", False),
    ]))
    b.append('<div class="note">The 59–60 fps 3D figure above is from card 4 (10-08), an internal development build with the '
             'native race renderer\'s stereo path turned on by hand — it is <b>not</b> a hardware-confirmed figure for the shipped '
             'v1.0.0 binary. What <i>is</i> true for v1.0.0: a no-ini <code>[perf] preset = auto</code> resolves to the Old 3DS\'s own '
             'tuned table (native renderer, NRR and stereo all on, plus the memory-fit levers card 4 still set by hand), so the same '
             'lever family card 4 ran by hand is now the default — see <a href="native.html">The native fork</a> for how the '
             'per-console default resolves. What is <b>not</b> yet true: a separate real-hardware pass confirming the shipped '
             'v1.0.0 build\'s own 3D frame rate has not been run. The current, authoritative status is in '
             '<a href="../RELEASE-NOTES.md#known-issues">RELEASE-NOTES.md, Known issues</a> ("Stereoscopic 3D plays but isn\'t yet a '
             'locked 60 fps on either console") and <a href="../STATUS.md">STATUS.md</a>\'s stereoscopic-3D row ("plays, not yet a '
             'locked 60 fps, exact hardware figure not confirmed for this release"). An explicit <code>[perf] preset = off</code> '
             'reverts to the legacy interpreter.</div>')

    b.append('<h2>The machine</h2>')
    b.append(table(["", "Old 3DS", "New 3DS", "Source"], [
        ["ARM11 cores, clock", "2 × MPCore at 268 MHz", "4 × MPCore, up to 804 MHz", '<a href="https://www.3dbrew.org/wiki/Hardware">3dbrew: Hardware</a>'],
        ["L2 cache", "none", "2 MB (\"optional\" per 3dbrew)", '<a href="https://www.3dbrew.org/wiki/Hardware">3dbrew: Hardware</a>'],
        ["L1 instruction cache", "16 KB per core; a miss costs ~37 cycles per 32-byte line (measured)", "not measured here; misses can be served from L2", "hardware profile, below"],
        ["Memory for the app", "64 MB application region (<code>memtype=0</code>); default heap grant 24 MiB", "124 MB app mode", "boot log <code>[mem-budget]</code>; see <a href=\"difficulties.html\">Difficulties</a>"],
        ["Cores the port can use", "core 0; core 1 is a 30% time-limited share (audio lives there)", "core 0 + core 2 (render thread)", "hardware profile, below"],
    ]))
    b.append('<div class="note">3dbrew lists 128 MB of FCRAM on the Old 3DS in total; 64 MB is the application region the console grants this title, '
             'read from the console\'s own <code>[mem-budget]</code> line: <code>model=old3ds memtype=0 app=67108864 heapGrant=25165824</code>.</div>')

    b.append('<h2>Every card, every number</h2>')
    b.append('<p>A \"card\" is one SD-card round trip: a build, an ini, a written test plan, the user\'s play session, and the log back. '
             'The Old 3DS runs Luma 6.6 on arm9loaderhax, so the homebrew launcher does not start and every build went over as a CIA '
             'installed through FBI. There was no network loader for this phase; each row below is a physical trip.</p>')
    short = {"First boot (RDRAM 8)": "boot", "C4 run 1: O3DS profile": "run 1", "C4 run 2: narrow audio lock": "run 2",
             "card 4 (VRAM fix)": "card 4"}
    cats_x = [f"{c['date']}|{short.get(c['name'], c['name'].replace('n60-', ''))}" for c in cards if c["fps2d"] or c["fps3d"]]
    s2 = [c["fps2d"] for c in cards if c["fps2d"] or c["fps3d"]]
    s3 = [c["fps3d"] for c in cards if c["fps2d"] or c["fps3d"]]
    svg = grouped_bars(cats_x, [("2D race", "--c1", s2), ("3D race", "--c3", s3)], ymax=66, ylabel="race frames per second (Grand Prix)",
                       dec=1, cap=60, ymarks=[0, 20, 40, 60], h=330)
    b.append(figure(svg, "<b>Race frame rate on the Old 3DS, per card.</b> Values are the frame rates named in the run's log analysis: "
                    "1000 / median frame time where a median exists (first boot 96.0 ms → 10.4; card 2 14.2 ms → 57.3), the steady-mode rate "
                    "(an early frame-skip preset → 15, a later one → 20 rendered) and the hold-1 share for the 3D cards (card 4: 245–256/256 → 59.5). "
                    "Two bars are <b>estimates</b> from documented ranges: card 2's 3D (48–57 → 52) and card 3's 3D (Mute City only, ~60). "
                    "The later cards skip drawing to keep logic at 60 ticks/s. Before the native race renderer the game ran in slow motion, "
                    "so the first three bars are also the game's speed, not just its frame rate.",
                    legend([("2D", "--c1"), ("3D (slider up)", "--c3")])))
    b.append(table(["Date", "Card", "What it carried", "What the hardware said", "Log / doc"],
                   [[esc(c["date"]), "<b>" + esc(c["name"]) + "</b>", c["what"], c["hw"], "<span class=\"mono\">" + esc(c["src"]) + "</span>"] for c in cards]))

    b.append('<h2>Phase 1 · Make it fit (09-24 → 09-29)</h2>')
    b.append('<p>The Old 3DS milestone had sat unstarted on a stale note that said the user did not own one; corrected on 09-24. Four parallel '
             'subagents, then a fifth, worked under a simulated <code>memtype=0</code> grant. The N64 RDRAM arena turned out to be 18% used and '
             'was halved to 8 MiB; <code>audio_table</code> moved off the heap behind a stream; texture uploads were capped. By 09-25 the canonical '
             'replay passed 49/49 under the simulated grant with 2.65 MiB of heap to spare. On 09-28 the real console answered '
             '<code>memtype=0</code>, byte-identical to the simulation, and the shipped build died in its boot preload exactly as predicted. '
             'One build flag, <code>-DGDX_3DS_O3DS=ON</code> (RDRAM 8 MiB plus a 16 MiB linear-heap pin, a 38.9 MB heap), became the Old 3DS profile; '
             'the New 3DS binary stays <code>cmp</code>-identical.</p>')

    b.append('<h2>Phase 2 · Find the costs (09-29 → 09-30)</h2>')
    b.append('<p>The first race spent 96 ms per frame with the GPU busy for 3.6 ms of it. The parallel wave that followed split the '
             'unattributed \"other\" bucket (it was the game thread waiting on the audio mutex while the HLE synthesiser held it: '
             '25.8 ms per frame, cut to 0.2 ms by narrowing the lock), made the synthesiser 2.2× cheaper with bit-exact ARMv6 SIMD kernels '
             '(verified on the console, 0 mismatches), and found that the game was running in slow motion: one logic tick per drawn frame, '
             'Grand Prix at roughly 22–25% speed. Frame skipping (an automatic mode, then an even-cadence \"steady\" mode) put the '
             'game at full speed and made it \"playable but nausea inducing\".</p>')
    b.append('<p>A staged retained-mode renderer followed (per-category timers, GPU-resident machines, batched '
             'HUD/shadows/fx, a resident track with float24-safe rebasing, overlap of CPU and GPU), each lever exact against the interpreter. '
             'Every stage won in the emulator. On hardware, the third stage moved almost nothing: a steady race window stayed at 24–36 ms '
             'against the second stage\'s 26–35.</p>')

    b.append('<h2>Phase 3 · The probe that changed the plan (09-30 → 10-01)</h2>')
    cp = O["cpi_probe"]
    svg = grouped_bars([c[0] for c in cp], [("CPI", "--c5", [c[1] for c in cp])], ymax=6.5, ylabel="cycles per instruction, probe loop", dec=2,
                       h=230, w=560, ymarks=[0, 2, 4, 6])
    b.append(figure(svg, "<b>The I-cache cliff, measured on the console.</b> A hardware probe ran the same loop body at three hot-code sizes. "
                    "At 16 KB (the L1 instruction cache) it retires about one instruction per cycle; at 24 KB it takes four. The game's own render "
                    "path ran at CPI 4.34 with 43.6% of its cycles stalled on instruction fetch."))
    b.append('<p>A performance-counter instrumentation pass put the ARM11 hardware counters in the build. The answer explained why the emulator '
             'wins did not transfer: the emulator does not model the instruction cache, and the renderer\'s cost on the console was mostly '
             'fetching code. The track, the one category whose levers had paid, was the one with the fewest I-misses per instruction: a few '
             'tight loops. The HUD ran 284 k instructions per frame at one I-miss every 17 instructions.</p>')
    b.append('<p>Two answers were on the table. A research pass (five lenses and an adversarial review) had concluded on 09-30 that a native '
             '60 was \"marginal for the 1P GP race\" and proposed pose replay: re-submit the last frame with the skipped tick\'s poses '
             '(\"60 Hz motion\"). It was built, worked on hardware at 40 images per second, and showed sky jitter, tearing edges and broken '
             'particle trails; it was set aside. On 10-01 the user wrote \"we should go for a real 60! i still believe it has to be possible.\"</p>')

    b.append('<h2>Phase 4 · The native race renderer (10-01 → 10-02)</h2>')
    b.append('<p>The renderer was redesigned around the cache, not around instruction counts. In races, each category (HUD, track, machines, '
             'sky, dressing, effects, shadows, decorations) runs one small specialised loop over a compact snapshot of game state and writes '
             'precomputed PICA200 command words directly into the GPU command buffer, drawing from GPU-resident meshes. The game\'s display-list '
             'sections for those categories are not built on rendered ticks at all. Each phase of the frame (core, one category, and the '
             'vendored citro3d path) is sized to fit the 16 KB L1-I: 9.1–11.0 KB per phase on card 2; the HUD phase reached 16,164 of 16,384 '
             'bytes on card 4. Game logic is untouched and checked by a game-state hash identical on every tick; pixels are held to a tolerance '
             '(2D: Δ>8 on ≤0.05% of pixels).</p>')
    names = cats["names"]
    svg = grouped_bars(names, [("interpreter + early levers", "--c8", cats["rel4"]), ("card 3 (native renderer)", "--c1", cats["card3"])],
                       ylabel="ms per rendered frame, hardware", dec=2, h=300)
    s4 = sum(cats["rel4"]); s3 = sum(cats["card3"])
    b.append(figure(svg, f"<b>Per-category CPU cost per rendered frame, before and after the native renderer.</b> Hardware medians by category, "
                    f"before (10-01) and after (card 3, 10-02) the switch. Sum of these categories: "
                    f"{s4:.1f} → {s3:.1f} ms. Frame begin is left out because it measures different things in the two runs (GPU wait before, "
                    f"vblank slack after). Per tick, logic {cats['tick_rel4']['logic']} → {cats['tick_card3']['logic']} ms is unchanged by design.",
                    legend([("before, 10-01", "--c8"), ("card 3, 10-02", "--c1")])))

    b.append('<h2>Phase 5 · 3D at 60 (10-03 → 10-08)</h2>')
    b.append('<p>The first native-renderer builds refused stereo. The legacy path draws each eye per draw (in the emulator '
             '+0.77 ms per frame), which would have put back per-draw CPU work the native renderer had just removed. The fix builds the frame '
             'body once and resubmits the recorded command stream for the right eye, with the shader skewing clip x by a per-eye uniform and a '
             'per-draw stereo class (scene, anchored, deep sky, or none for UI). The emulator cost was +0.047 ms per frame against +0.77 for '
             'legacy per-draw stereo. On hardware the CPU fit, but the frames missed vblank: citro3d requests the swap only when the whole GX '
             'queue drains, and a whole right eye queued after frame end is a 5–7 ms tail. Interleaving the right eye per submitted chunk and '
             'swapping in end mode closed it: Mute City at ~60 in 3D on 10-07, Grand Prix race windows at 59–60 on 10-08.</p>')

    b.append('<h2>What was still open at card 4, and what happened after</h2>')
    b.append('<ul>'
             '<li><b>Race end.</b> The post-race results table cost ~40 ms per frame (147 HUD runs and 166 texture binds); still in race scope, '
             'so it tripped the cost guard at the time. Addressed in later work on the results screen.</li>'
             '<li><b>Heavier venues.</b> Silence and Sand Ocean reached 16–18 ms of work on card 3.</li>'
             '<li><b>Menus</b> stay on the interpreter with frame skipping; this is unchanged in the shipped release.</li>'
             '<li><b>Audio crackle</b> on the Old 3DS: the leading hypothesis (a 3DS-vblank-vs-audio-rate mismatch) was measured and refuted '
             '— the N64 audio code is already a self-correcting feedback loop and the port\'s producer thread paces to ring depth, not vblank. '
             'A separate fix (the resample window alignment) shipped; the DSP-stage crackle candidate stayed open, and brief audio gaps during '
             'loads remain a known issue at release (see <a href="index.html">Overview</a>).</li>'
             '<li><b>The bottom-screen framebuffer in VRAM</b> crashed three cards\' worth of boots (<code>lcd_vram</code> mapped read-only '
             'for the CPU under the CIA\'s exheader, which the emulator does not enforce). Fixed and merged after card 4 (a crash-proof boot '
             'trail, a VRAM guard, and the fix itself); shipped off by default.</li>'
             '</ul>')

    b.append('<h2>The Expansion Kit on the Old 3DS</h2>')
    b.append('<p>The 64DD Expansion Kit build (course and machine editors, streamed disk image) was brought up on the New 3DS first. Asked '
             'whether it could reach the Old 3DS, measurement in the emulator under the Old 3DS profile found it fits with about 6 MB of heap '
             'to spare, and that gameplay under the native renderer is identical tick for tick on a ROM course (6,002/6,002) and a 64DD course '
             '(8,601/8,601). Two EK-only rendering defects (sky during the race intro, gadget and dirt-patch dressing) were found and fixed, '
             'with the shipped binary byte-identical before and after.</p>')
    b.append('<p>Fitting is not the same as playing. On release day the Old 3DS Expansion Kit build booted on hardware with the retail '
             'disk image streaming from the SD card, but the 64DD courses stuttered, the disk-streamed music broke up into static, and the '
             'course editor and menus, which still run through the general-purpose drawing path rather than the native race renderer, were '
             'very slow. The owner held it back: v1.0.0 ships the Expansion Kit for the New 3DS only, and the Old 3DS build stays a '
             'from-source experiment — see <a href="native.html">The native fork</a> for the shipped release shape.</p>')
    return page("old-3ds.html", "Old 3DS: The Road to a Real 60", "".join(b))
