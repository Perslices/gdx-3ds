from framework import *


def link(url, text):
    return f'<a href="{esc(url)}">{text}</a>'


def build(D):
    b = []
    b.append('<p class="eyebrow">07 · Pushing the boundaries</p><h1>Pushing the boundaries</h1>')
    b.append('<p class="lede">Two comparisons. First against this port\'s own New 3DS build: what the Old 3DS forced that the New 3DS '
             'never needed, and what comes back the other way. Then against other N64 projects on the 3DS, from their own READMEs and '
             'press, each claim linked. Where we could not check something, the page says so.</p>')

    b.append('<h2>Against our own New 3DS build</h2>')
    b.append('<p>The New 3DS build keeps the render-thread approach the first phase (2026-08-11 to 09-03) shipped: '
             'libultraship\'s display-list interpreter, moved to its own CPU core so it runs a frame ahead of the game. '
             'The Old 3DS has no second core to give that interpreter, so the second phase (09-07 to 10-19) took a '
             'different route: a native renderer that skips the per-frame display-list walk for the geometry a race '
             'spends its time on. Both reach a comparable result; the table is how they get there.</p>')
    b.append(table(["", "New 3DS build", "Old 3DS build"], [
        ["Race renderer", "libultraship's display-list interpreter (Fast3D) on its own CPU core, one frame ahead of the game", "The native renderer: one small loop per race category over a game-state snapshot, writing PICA200 command words directly; the interpreter remains for menus and the long tail of other screens"],
        ["What limits the frame", "CPU instruction count in the interpreter and the bridge that feeds it (the GPU itself was not the bottleneck: about 0.4 ms per frame in the emulator)", "Instruction-cache misses, not instruction count: at 268 MHz with no L2 cache, the render path ran at 4.34 cycles per instruction with 43.6% of cycles stalled on instruction fetch, so the interpreter's own code did not reliably fit the 16 KB L1 instruction cache"],
        ["Cores", "A render thread on the second core, one frame ahead of the game", "No second core. One core does everything but audio, which gets a fixed 30% share of the other"],
        ["Memory", "124 MB app-mode budget, 16 MiB RDRAM arena", "64 MB app-mode budget (half the console's 128 MB FCRAM is reserved for the system); RDRAM arena halved to 8 MiB plus a 16 MiB linear-heap pin, 38.9 MB of heap left for the game"],
        ["Audio", "Headroom to spare in the synthesis budget", "Synthesis needed about 36% of its CPU share against a 30% grant: narrowing the audio lock (−20 ms of race-wall waiting per frame) and hand-tuned ARMv6 SIMD decode kernels (2.2× cheaper) closed the gap"],
        ["Stereo 3D", "Per draw: the packed vertex buffers replayed with a second matrix for the right eye", "Per frame: the recorded command stream resubmitted for the right eye with a per-eye uniform, interleaved in chunks, with its own swap-pacing mode"],
        ["Hardware result", "Full Grand Prix, end of the first phase: median " + esc(D["final_median"]) + " fps, p10 " + esc(D["final_p10"]) + ", " + esc(D["final_cap"]) + " of frames at the 60 cap", "16.5 ms median frame time on a full 2D Grand Prix race, 0 hitches; 59–60 fps in 3D on most Grand Prix courses, with the heaviest courses (Silence, Sand Ocean) able to trip the frame-skip safety net to a steady 30 fps mid-race — see <a href=\"old-3ds.html\">Old 3DS</a> for the full campaign. Measured with a different harness (frame time and a vblank-hold share, not the fps/p10/cap-% stats above), so not directly stacked against the New 3DS row"],
        ["Correctness gate", "Exact parity with the interpreter checked for every performance lever, on and off", "The game's own per-tick state hash must match exactly; pixels are checked within a tolerance; an on-console verify mode re-checks every GPU command word the native path writes"],
    ]))
    b.append('<h3>What the Old 3DS forced that the New 3DS never needed</h3>')
    b.append('<ul>'
             '<li><b>Leaving the display list in races.</b> On the New 3DS, a faster interpreter on its own core was enough. On a CPU a third '
             'the clock speed with no L2 cache, it was not: the interpreter\'s own code did not reliably fit in cache, so races stopped walking '
             'the display list at all.</li>'
             '<li><b>A hardware profiler.</b> Emulator timings stopped predicting hardware results on the Old 3DS, because the emulator used '
             'does not model the instruction cache, the console\'s memory permissions, or how long some operations actually take to complete. '
             'Several Old 3DS regressions (a performance round that showed no gain on hardware after winning in the emulator, an early build '
             'that froze on boot, a boot crash traced to a VRAM framebuffer the CIA maps read-only) only came to light once a real hardware '
             'performance counter, or the console itself, was read.</li>'
             '<li><b>A memory diet.</b> A smaller RAM arena, streamed rather than fully-resident audio data, capped texture uploads, a pinned '
             'heap layout, and a high-water-mark measurement in place of a free-list number that had been misread as headroom. Even after the '
             'diet, the Old 3DS build has a memory ceiling: a single large asset load can still exhaust the budget on heavy content, which is '
             'listed as a known issue for this release.</li>'
             '<li><b>Frame pacing as a design problem.</b> With only a millisecond or two of margin per frame, when the screen swap is '
             'requested matters as much as the work done before it: a dedicated swap gate, an end-of-frame mode, and a race-scoped frame-skip '
             'guard that knows not to trip during an intro.</li>'
             '</ul>')
    b.append('<h3>What carries back to the New 3DS</h3>')
    b.append(table(["Change", "Status on the New 3DS"], [
        ["A fix for a freeze when backlight/HOME handling toggled the screen off", "Applies to both models. The New 3DS build never showed the freeze in practice, because no New 3DS HOME test happened to run with the bottom screen off"],
        ["A save-persistence fix for a rare corrupted save on power loss", "The underlying cause is in how the SD card's filesystem handles a file rename, not anything model-specific, so the New 3DS build is believed to carry the same fix; not separately confirmed on a New 3DS log"],
        ["A never-blocking file log and hitch-detection receipts", "Model-independent; applies to both"],
        ["The native renderer, the per-eye stereo resubmit, and the Old 3DS's end-of-frame pacing mode", "Not yet enabled on the New 3DS: the native renderer currently steps aside when the New 3DS's own render thread is active, pending its own New 3DS verification pass"],
        ["The two audio tuning levers added for the Old 3DS's tighter CPU-share budget", "Default on for Old 3DS only; the New 3DS default is unchanged, since it was never short on audio headroom"],
        ["The Old 3DS's reduced-memory build profile", "Off for the New 3DS build, which is otherwise unchanged by the Old 3DS work"],
    ]))

    b.append('<h2>Against the field</h2>')
    b.append('<p>Searched on 2026-10-08, with results still current at release. Every row is what the project or the press says about '
             'itself; none of these figures were measured by us, and the games differ in load, so this is not a benchmark. Our own figures '
             'come from our hardware logs.</p>')
    R = [
        ["Super Mario 64 3DS port (mkst)", link("https://github.com/mkst/sm64-port/tree/3ds-port", "mkst/sm64-port, branch 3ds-port"),
         "Decomp PC port; renderer details not in the README",
         "README: \"Naïve frame-skip if frame takes longer than 33.3ms\"; a 60 FPS enhancement patch is listed. Press (2020): \"a solid 30fps\", model not stated (" + link("https://www.videogameschronicle.com/news/mario-64-3ds-port/", "VGC") + ")",
         "Yes (README)"],
        ["Ocarina of Time 64 3DS port", link("https://github.com/Andresac90/OOT-64-3DS-Port", "Andresac90/OOT-64-3DS-Port"),
         "zeldaret/oot decomp; renderer started from the SM64 PC port's Fast3D renderer",
         "README: New 3DS \"60 in 2D and 3D\" by interpolation (measured on hardware). Old 3DS: \"about 15–17 in the biggest scene (Kokiri Forest), around 45 in interiors\" (measured at simulated Old 3DS speed); \"60 everywhere is still being worked on\"",
         "Yes"],
        ["Majora's Mask 64 3DS port", link("https://github.com/Andresac90/MM-64-3DS-Port", "Andresac90/MM-64-3DS-Port"), "decomp", "\"Nothing runs on the 3DS yet\"", "–"],
        ["Smash 64 for New 3DS", link("https://github.com/2gifts/smash64-3ds", "2gifts/smash64-3ds"),
         "decomp; credits the SM64 3DS graphics backend, citro3d; renders on the New 3DS's third core",
         "\"Original 3DS/2DS models are not supported\"; \"a locked 60 FPS is not guaranteed in every situation\"", "Yes"],
        ["Mario Kart 64 3DS", link("https://github.com/EstebanPdN/mario-kart-64-3ds", "EstebanPdN/mario-kart-64-3ds"),
         "SpaghettiKart (libultraship) based; Fast3D vertex/texture ranges, combiner plans compiled once per material",
         "\"retains its original 30 Hz game simulation\"; an adaptive interpolated midpoint frame on Old and New 3DS, \"not a promise of a fixed or sustained 60 FPS mode\"",
         "Not mentioned"],
        ["Ship of Harkinian 3DS", link("https://github.com/999sian/soh-3ds", "999sian/soh-3ds"),
         "libultraship (SoH); PICA200 renderer on citro3d, built for F3DEX2",
         "New 3DS is the main target; \"Old 3DS uses native 20 FPS presentation\"; \"Physical Old 3DS gameplay is not yet verified\"", "Not mentioned"],
        ["rt64-3ds", link("https://github.com/oneup03/rt64-3ds", "oneup03/rt64-3ds"),
         "Renderer for N64ModernRuntime static recompilations: F3DEX2 interpreter, CPU vertex transform and lighting, per-eye replay",
         "\"Under construction. First target is Donkey Kong 64 (New 3DS only)\"; no frame rates", "Yes"],
        ["sm64coopdx New 3DS", link("https://github.com/antoinebou12/sm64coopdx-switch/pull/26", "PR #26"),
         "citro3d", "Not yet called playable; \"30 FPS stability\" listed as a next gate", "–"],
        ["DaedalusX64-3DS (emulator)", link("https://www.gamebrew.org/wiki/DaedalusX64_3DS", "GameBrew") + ", " + link("https://wololo.net/2020/05/01/n64-emulation-masterfeizz-releases-armv6-dynarec-version-of-daedalusx64-for-the-3ds-ports-it-to-the-vita-version-mario-kart-64-super-mario-64-are-now-effectively-playable-on-both-the-vi/", "Wololo 2020"),
         "HLE emulator, ARMv6 dynarec",
         "\"Tested on N3DS, runs about at 20-30FPS on average\" (v1.0, 2022-10-16)", "Not verified"],
        ["Ocarina of Time 3D (Nintendo/Grezzo, 2011)", link("https://www.nintendolife.com/news/2011/05/ocarina_of_time_3d_is_a_smoother_experience_than_before", "Nintendo Life"),
         "Remake, native 3DS engine", "Miyamoto: \"It ran at 20fps on the Nintendo64, but on the 3DS, it runs at 30fps\"", "Yes"],
        ["Star Fox 64 3D (2011)", link("https://www.nintendolife.com/reviews/2011/09/star_fox_64_3d_3ds", "Nintendo Life review"),
         "Remake", "Numeric frame rate: not found", "Yes"],
    ]
    b.append(table(["Project", "Source", "Approach", "What it says about Old / New 3DS performance", "Stereo 3D"], R))
    b.append('<h3>What we can and cannot say</h3>')
    b.append('<ul>'
             '<li>We found no other N64 decompilation, recompilation or emulator project on the 3DS that claims a sustained 60 fps on an '
             '<b>Old</b> 3DS. The nearest are the OoT port (15–45 fps by scene, measured at simulated Old 3DS speed), the MK64 port '
             '(adaptive interpolation over 30 Hz logic), SoH-3DS (20 fps presentation, unverified on hardware) and the SM64 port.</li>'
             '<li>Every renderer we found interprets N64 display lists (Fast3D / F3DEX2 translated to citro3d). We found none that, like this '
             'project\'s native renderer, skips display-list construction for whole categories of geometry and writes precomputed PICA200 '
             'command words instead.</li>'
             '<li>Our 60 is <b>race-only</b>: menus still run on the interpreter with frame skipping, and the post-race results table is '
             'still heavy. Several of the projects above target the whole game and interpolate instead; that is a different trade, not a lesser one.</li>'
             '<li>No other native F-Zero X port on the 3DS was found. Wololo (2020), citing a Reddit compatibility list, names F-Zero X among the games with "playable performance" on the New 3DS under DaedalusX64.</li>'
             '<li>Not verified: Old 3DS frame rates for the SM64 port and DaedalusX64 (the sources are forum threads that returned HTTP 403), '
             'stereo support in DaedalusX64, and a numeric frame rate for Star Fox 64 3D.</li>'
             '<li>Also searched and not found: a Star Fox 64 (Starship), Perfect Dark, Banjo or 2Ship 3DS port; an upstream libultraship '
             '3DS backend; a Mupen64Plus or ParaLLEl-N64 core for 3DS.</li>'
             '<li>Correction to this post-mortem\'s own <a href="frontier.html">Frontier</a> page (written 09-03): it describes the SM64 '
             '3DS port as 30 Hz logic interpolated to 60. The README read for this page describes naive frame skipping below 30 fps and '
             'lists a separate 60 FPS patch; we could not confirm the interpolation description, and Frontier now links here.</li>'
             '</ul>')
    return page("boundaries.html", "Pushing the Boundaries", "".join(b))
