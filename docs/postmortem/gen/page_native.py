from framework import *


def build(D):
    b = []
    # The leading "05 ·" below is cosmetic placeholder text: framework.page()/number_eyebrow()
    # overwrites it at build time to match this page's real position in PAGES.
    b.append('<p class="eyebrow">05 · The native fork</p><h1>The native fork: six weeks, a second console</h1>')
    b.append('<p class="lede">The New 3DS chapter ended on 09-03 with the game running through libultraship\'s Fast3D interpreter: every '
             'triangle translated and lit on the CPU, one display-list command at a time. Starting 09-07, that path was replaced, piece by '
             'piece, with code that talks to the PICA200 GPU directly — not because the interpreter was slow in the abstract, but because it '
             'was the only way to also reach the Old 3DS. This page is that fork: why it started, what it replaced, and the four features it '
             'shipped with by 10-19: Old 3DS support, the 64DD Expansion Kit on console, a bottom-screen dashboard, and a fixed New 3DS freeze.</p>')

    b.append(tiles([
        ("~1,600", "commits reachable from the native integration branch, 09-07 → the morning of 10-19", True),
        ("187", "feature branches cut for this chapter, almost all merged back", False),
        ("199", "internal progress and plan documents dated 09-07 → 10-19, this chapter's window", False),
        ("2", "consoles supported from one codebase: Old 3DS and New 3DS", True),
        ("0<small> / 10,466</small>", "lines of the libultraship F3DEX2 interpreter (09-07 snapshot) that still run during a race with the native race renderer on — the default on both consoles at release (see the table below)", True),
        ("43 <small>days</small>", "09-07 to 10-19, across five rate-limit or session pauses", False),
    ]))

    b.append('<h2>Why fork the renderer</h2>')
    b.append('<p>A feasibility pass on 09-07 inventoried what the 3DS build actually compiles from libultraship: 36 files, 15,043 lines. '
             'Of those, one thing dominates: the Fast3D interpreter itself (<code>interpreter.cpp</code>, 10,466 lines in that pre-fork '
             'snapshot — F3DEX2 and RDP '
             'opcodes, RSP transform-and-light on the CPU, texture cache, the combiner). Outside that 36-file total, but just as central '
             'to a race frame, sits a separate 12,353-line bridge whose only job is to widen the game\'s 32-bit display-list words into '
             'the 64-bit words the interpreter wants. Everything else libultraship provides on 3DS — window, audio, input, config, GUI — '
             'was already replaced by this port\'s own glue code; graphics was the one piece still routed through someone else\'s '
             'CPU-side renderer.</p>')
    b.append('<p>A census — a short pass that counts how existing code already does something, before deciding whether a lever is worth '
             'building — found machine meshes, the track, and effects/HUD each drawn a different way, which pointed to a hybrid '
             'opportunity rather than a clean rewrite: the table below has the detail. The long tail — menus, the course editor, the '
             'ending, one-off screens — stays on an interpreter forever, because translating all of it would cost more than it returns. '
             'The fork is therefore a <b>hybrid renderer</b>: an owned interpreter for everything, plus three race-only paths that bypass '
             'it for the things a race frame actually spends its time on.</p>')
    b.append(table(["Tier", "What it does", "Status at release"], [
        ["Tier 1 — own the interpreter", "A purpose-built F3DEX2 → citro3d walker reads the game's display list directly, replacing libultraship's interpreter and its bridge. Same pixels, no 32→64-bit conversion pass.", "Shipped and on by default on both consoles (see the preset note below); an explicit <code>[perf] preset = off</code> switches back to the exact prior libultraship path."],
        ["Tier 2 — bypass it for races", "<code>track</code> replays the course tessellator's vertex stream straight into a GPU buffer; <code>machines</code> draws the racers from baked, retained lists; the HUD, effects and shadows batch into sprite passes. Together this is the native race renderer (NRR).", "Built and on by default on both consoles. On Old 3DS hardware an internal development card held 59–60 fps in Grand Prix races with the 3D slider up (see <a href=\"old-3ds.html\">Old 3DS</a>) — that card, not the shipped v1.0.0 binary, so it isn't a hardware-confirmed figure for this release's own 3D mode (see the release note below). On New 3DS the same default also turns the core-2 render thread off in favour of the NRR's sequential host loop — the two are mutually exclusive by design (see <a href=\"boundaries.html\">Pushing the Boundaries</a>) — and that stack is verified in the emulator and exercised on real hardware during the title-screen freeze fix below, but still awaits its own dedicated New 3DS hardware verification pass for full races."],
    ]))
    b.append('<p>Every individual lever is still an ini killswitch that compiles to off, with a verify mode that draws both paths and '
             'compares the packed vertex bytes bit for bit before trusting the new one — that discipline is why a two-console fork stayed '
             'tractable, since a regression on one model shows up as a byte mismatch, not a maybe. What changed for release: a no-ini player '
             'no longer sees that off default. <code>[perf] preset</code> defaults to <code>auto</code>, which resolves per console at '
             'startup — <code>n60</code> on New 3DS, the matching <code>o3ds-auto</code> table on Old 3DS — and switches the whole race-frame '
             'lever set on out of the box. An explicit ini key always wins, so <code>preset = off</code> is the one-line escape hatch back to '
             'every lever\'s compiled-off default.</p>')

    b.append('<h2>Fixing the dev loop before forking</h2>')
    b.append('<p>The 09-07 plan explicitly delayed the renderer work by about two weeks to fix the development loop first: every iteration '
             'to that point meant a human carrying an SD card between a Mac and a console, and a renderer rewrite is a differential-testing '
             'problem — the new path has to match the old one pixel for pixel before it is allowed to be faster. The fix added a network '
             'path to the console: a control listener (TCP 17492) that accepts a new build, writes it to the SD card, and relaunches the app '
             'through the Homebrew Launcher\'s own reload hook, plus a log link (TCP 17491, the same protocol <code>3dslink -s</code> already '
             'speaks) that streams the console\'s log back live. One Y-button press on the Homebrew Launcher per power-on was enough; after '
             'that the console reloaded builds on its own. By release, a 3dslink launch turns this network link on by default, alongside the '
             'bottom-screen dashboard described below.</p>')

    b.append('<h2>Native track, machines, sky, HUD and effects</h2>')
    b.append('<p>Each race-frame category got its own small, specialised loop instead of a general-purpose interpreter pass:</p>')
    b.append(table(["Category", "What changed"], [
        ["Track", "The spline tessellator's per-frame vertex stream is written straight into a GPU-resident buffer instead of being walked and translated command by command."],
        ["Machines", "The 30 racer meshes are retained on the GPU and redrawn from baked lists, instead of being re-submitted as fresh display-list commands every frame."],
        ["Sky", "A small per-frame vertex path, kept resident where the scene allows it; the one category that needed explicit stereo and fallback handling of its own (see the New 3DS section below)."],
        ["HUD and effects", "Texture-rect sprites (HUD gauges, particle effects) batch into atlas pages instead of one draw call per rectangle."],
        ["Shadows", "Folded into the same batched pass as the effects sprites."],
    ]))
    b.append('<p>Each phase of a race frame — the shared core, one category, and the vendored GPU-command path underneath — is also sized to '
             'fit inside the CPU\'s 16 KB instruction cache, the single biggest lesson of the Old 3DS phase below: code that does not fit in '
             'L1-I pays for it on every fetch, regardless of how few instructions it runs.</p>')

    b.append('<h2>A real 60 on the Old 3DS</h2>')
    b.append('<p>The Old 3DS was ruled out on day one of the whole project (08-11): its 64 MB application region was judged too small. '
             'The native fork revisited that from 09-24, and by 10-08 it held 59–60 fps (245–256 of 256 frames on the first vblank) in '
             'Grand Prix races with the 3D slider up, on a console with no L2 cache and a third of the New 3DS\'s clock. That campaign — '
             'the memory fit, the instruction-cache cliff that '
             'explained why emulator wins did not transfer, and the race-by-race hardware numbers — is its own page: '
             '<a href="old-3ds.html">Old 3DS: the road to a real 60</a>.</p>')

    b.append('<h2>The Expansion Kit, shipped on both consoles</h2>')
    b.append('<p>The 64DD Expansion Kit — the course editor, the machine creator, and the extra 64DD disk content — runs as its own build, '
             '"G-Diffuser EK", with its own HOME-menu title and its own save file (<code>saves/fzerox-ek.sav</code>) and logs '
             '(<code>log-EK.txt</code>), so it installs side by side with the plain build on the same SD card without either one touching the '
             'other\'s save. It needs the user\'s own 64DD disk image (Japanese retail or the fan English translation); without one, the '
             'game boots and plays normally and the EK-only menu entries simply don\'t open.</p>')
    b.append('<p>Two things changed late for this release, both in the disk path: the disk image streams from the SD card instead of being '
             'copied into RAM (a 62 MB saving), and <code>disk_stream</code> now defaults to its full-streaming mode (2) for every 3DS build '
             'rather than being an opt-in flag. Japanese-region inputs are also accepted by default, since the only retail 64DD disk is '
             'Japan-only. A first boot with a disk image does a roughly one-minute whole-image integrity pass once and remembers the result '
             'for next time.</p>')
    b.append('<p>The Expansion Kit was verified against the plain game under the native renderer and found to fit the Old 3DS\'s tighter '
             'memory budget with a comparable margin; see <a href="old-3ds.html">Old 3DS: the road to a real 60</a> for the tick-exact '
             'verification numbers and the two drawing defects found and fixed along the way.</p>')

    b.append('<h2>The bottom screen: DASH</h2>')
    b.append('<p>The 3DS\'s second screen started this project as a debug console. Through the native-fork chapter it became a proper race '
             'dashboard (DASH): the same information the top-screen HUD shows — lap, position, energy, split and best times, the minimap — '
             'composited directly into the console\'s own bottom-screen framebuffer rather than drawn through the GPU, so it costs under a '
             'millisecond of CPU time per frame and does not touch the render thread\'s budget at all. Three placement modes are available, '
             'cycled with a touch-screen chip or a SELECT long-press: HUD elements all on the top screen (closest to the original game), '
             'split between both screens, or all moved to the bottom screen. Outside a race the bottom screen shows an idle view instead; the '
             'DEBUG button opens the same settings and log console the screen always had. In 2–4 player races the HUD is forced to the top '
             'screen, since the bottom screen is one shared surface.</p>')
    b.append('<p>A fourth mode — a fully custom, on-device drag-and-resize layout editor for the dashboard — was prototyped but shelved '
             'before release; the three fixed layouts above are what ships. One cosmetic defect is known and left alone rather than chased '
             'further: the speed readout can briefly show an implausible number — as high as ~826 KPH — in the single frame right at a '
             'race\'s finish line.</p>')

    b.append('<h2>The New 3DS freeze</h2>')
    b.append('<p>Late in this chapter, a real New 3DS froze — not crashed, froze, with no crash dump and no log line — every single time '
             'the title screen\'s attract-mode demo cycle reached its one full-30-machine race demo. The emulator never reproduced it in '
             'repeated multi-minute runs, which argued for a genuine hardware timing issue rather than a logic bug.</p>')
    b.append('<p>Bisection on the console (not the emulator) found the cause: the GPU command buffer that the native renderer and its '
             'vendored graphics path both write into was sized the same on both consoles, and a full 30-machine race frame sits right at '
             'that buffer\'s edge on New 3DS. With one specific lever disabled, the same frame instead hit a clean, bounds-checked panic in '
             'the buffer writer at the identical point — the strongest evidence available that the freeze and the panic are two code paths '
             'tripping over the same undersized buffer, though it doesn\'t by itself prove they\'re the identical bug.</p>')
    b.append('<p>The fix doubles the buffer on New 3DS, which has the RAM margin to spare (the size multiplier goes from 2x to 4x the citro3d '
             'default), and adds a durable log line the moment any future overflow is about to happen, so a recurrence would be diagnosable '
             'from the log alone. Four separate real-hardware boot sessions through the exact frame that used to freeze every time came back '
             'clean afterward. The same review re-measured the Old 3DS\'s own buffer and found it already running tight (70% of capacity on '
             'the emulator, by this project\'s own margin for "raise it"); it got the same doubling, 2x to 4x, on the logic that the New 3DS '
             'case had just shown an emulator reading can understate a real console\'s margin.</p>')

    b.append('<h2>Release: v1.0.0</h2>')
    b.append('<p>The native fork\'s work ships as four console builds from one source tree: a plain build and an Expansion Kit build, each '
             'for Old 3DS and New 3DS. Which performance profile runs is decided at startup by asking the console what it is, not by which '
             'binary was installed — an Old 3DS build on a New 3DS still gets the New 3DS settings, but the reverse does not fit the memory '
             'budget and is not supported. All four share one SD-card folder for settings, save data and media. The native race renderer '
             'ships on by default on both consoles — a no-ini <code>[perf] preset = auto</code> resolves to each console\'s own tuned lever '
             'table, so a player does not need to write any ini lines for performance. Verification depth still differs by console and by '
             'feature: the native draw paths themselves are extensively hardware-tested on Old 3DS; on New 3DS they are verified in the '
             'emulator and exercised on real hardware during the freeze fix below, but still await a dedicated New 3DS hardware frame-rate '
             'pass for full races (see the Tier 2 status above). Stereoscopic 3D is a separate, narrower claim: it is on by default on both '
             'consoles, but the 59–60 fps figure above is from an internal development card, not the shipped binary, so it isn\'t yet a '
             'hardware-confirmed locked 60 on either console (see <a href="../RELEASE-NOTES.md#known-issues">RELEASE-NOTES.md</a> and '
             '<a href="../STATUS.md">STATUS.md</a>).</p>')
    b.append(table(["Known at release", "What it means"], [
        ["Stereoscopic 3D not yet a locked 60 fps on either console", "3D plays on by default on both consoles, but the 59–60 fps figure in the Tier 2 row above is from an internal development card, not the shipped binary — no hardware-confirmed 3D frame rate for v1.0.0 yet (see RELEASE-NOTES.md / STATUS.md)"],
        ["A rare speed-readout spike (as high as ~826 KPH) at the finish line", "Cosmetic; the bottom-screen DASH only, noted above"],
        ["Brief audio gaps during loads", "Short, occasional; not the crackle hypothesis this chapter investigated and refuted"],
        ["The Old 3DS has a memory ceiling", "Reproduced on the Azahar emulator under the real 64 MB budget simulation, not yet on real Old 3DS hardware: a single large asset load during a race can exhaust the budget under worst-case conditions. A separate real-hardware capture (heapUsed≈24.2 MB peak) suggests the margin is thin on real hardware too"],
        ["Not every mode is hardware-tested on every model", "The game has been played on real hardware throughout this chapter, but coverage favors the modes exercised most during development"],
    ]))
    b.append('<p>The day-by-day account of this chapter — including the parts that did not work on the first try — continues on '
             '<a href="timeline.html">Every Step</a>; the hard problems it fought through are catalogued on '
             '<a href="difficulties.html">Difficulties</a>.</p>')
    return page("native.html", "The Native Fork", "".join(b))
