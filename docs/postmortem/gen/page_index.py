from framework import *


def build(D):
    b = []
    b.append('<p class="eyebrow">01 · Overview</p><h1>F-Zero X on 3DS — Old and New, native, with the 64DD Expansion Kit</h1>')
    b.append('<p class="lede">A post-mortem of porting G-Diffuser, the F-Zero X decompilation, to the Nintendo 3DS — '
             'v1.0.0, covering both the original 24-day sprint that put it on a New 3DS at 60 fps in 3D (2026-08-11 to '
             '09-03) and the 43-day second phase that added Old 3DS support, a native renderer, and the Expansion '
             'Kit (2026-09-07 to 10-19). One person directed throughout; one long-lived Claude session orchestrated; '
             + esc(D["subagents"]) + ' subagents built the first phase and many more carried the second.</p>')
    b.append('<p class="lede">The console went from a black screen to a native-60 stereoscopic racer, then from a '
             'New-3DS-only build to a four-way release covering both consoles with and without the 64DD expansion.</p>')
    b.append(tiles([
        (esc(D["final_median"]) + '<small>fps</small>', "median on hardware, full Grand Prix, New 3DS, end of the first phase (from 12–20 fps in the first emulator race)", True),
        (esc(D["final_p10"]) + '<small>fps</small>', "tenth-percentile floor in 30-machine crowds, first phase (from 25–35)", True),
        (esc(D["final_cap"]), "of frames at the 60 cap, first phase (from 14% one week earlier)", True),
        (esc(D["active_days"]), "active days across the first phase's 24 calendar days, three pauses when the weekly plan allowance ran out", False),
        (esc(D["tokens_total_h"]), "tokens across all sessions and agents, first phase only; see Cost", False),
        (esc(D["cost_b_h"]), "at published API list prices (Fable $10/$50 per MTok, 1-hour cache writes), first phase only; see Cost", False),
        (esc(D["commits_3ds"]), "commits touching the 3DS port in the first phase, frozen 2026-10-08 from the v0.9.0 docs — not re-verified against final history", False),
        (esc(D["loc_port3ds"]), "lines under port/3ds plus " + D["patches"] + " pure-delta patches, first-phase count", False),
    ]))
    b.append('<h2>What v1.0.0 added</h2>')
    b.append(tiles([
        ("4", "console builds from one source tree: Old 3DS, New 3DS, and each with the Expansion Kit", True),
        ("199", "internal progress notes written during the second phase (docs/research/native/*.md)", False),
        ("43<small>days</small>", "calendar span of the second phase, 2026-09-07 to 10-19 inclusive", False),
    ]))
    b.append('<div class="note">The tiles above are dated. The first phase\'s hardware numbers and token/cost '
             'figures were measured through 2026-09-03 and have not been re-run for v1.0.0; the <a href="agentic.html">'
             'Agentic workflow</a> page describes how the second phase was run, and the <a href="cost.html">Cost</a> '
             'page is explicit about what it does and does not price.</div>')

    b.append('<h2>On hardware</h2>')
    b.append('<div class="media">'
             '<figure class="vid"><video autoplay muted loop playsinline preload="metadata" poster="../media/gameplay-race-poster.jpg" src="../media/gameplay-race.mp4"></video><a class="open" href="../media/gameplay-race.mp4" target="_blank" rel="noopener">open clip</a>'
             '<figcaption>Grand Prix on a New 3DS, final build. The bottom screen is the port\'s debug menu: 59.8 fps, heap, build id.</figcaption></figure>'
             '<figure class="vid"><video autoplay muted loop playsinline preload="metadata" poster="../media/gameplay-handheld-poster.jpg" src="../media/gameplay-handheld.mp4"></video><a class="open" href="../media/gameplay-handheld.mp4" target="_blank" rel="noopener">open clip</a>'
             '<figcaption>Handheld, mid-race. Native 60 Hz pacing; no interpolation.</figcaption></figure>'
             '</div>')
    b.append('<div class="media four">'
             '<figure><a href="../media/title.jpg" target="_blank" rel="noopener"><img src="../media/title.jpg" alt="F-Zero X title screen on the New 3DS" loading="lazy"></a><figcaption>Title screen, launched from the Homebrew Launcher (.3dsx).</figcaption></figure>'
             '<figure><a href="../media/menu.jpg" target="_blank" rel="noopener"><img src="../media/menu.jpg" alt="Select Mode screen" loading="lazy"></a><figcaption>Select Mode.</figcaption></figure>'
             '<figure><a href="../media/character1.jpg" target="_blank" rel="noopener"><img src="../media/character1.jpg" alt="Select Machine screen" loading="lazy"></a><figcaption>Select Machine.</figcaption></figure>'
             '<figure><a href="../media/character2.jpg" target="_blank" rel="noopener"><img src="../media/character2.jpg" alt="Blue Falcon stats" loading="lazy"></a><figcaption>Blue Falcon.</figcaption></figure>'
             '</div>')
    b.append('<h2>What was built</h2>')
    b.append('<p>Four console builds from one source tree, all booting from the HOME menu (.cia) or the homebrew '
             'launcher (.3dsx): Old 3DS / 2DS and New 3DS / New 2DS XL, each either the base game or with the 64DD '
             'Expansion Kit (course editor and machine creator, playable with a user-supplied disk image). Every build '
             'reads prebaked asset archives from the SD card — no ROM or disk image ever ships with the game.</p>')
    b.append('<p>They play the whole game: Grand Prix, Time Trial, Practice, Death Race, and the machine editor screens, '
             'with music and effects on the real DSP, stereoscopic 3D on the slider, a bottom-screen dashboard with three '
             'HUD layouts and an idle view outside races, and clean HOME suspend, resume, close, lid sleep and power-off. '
             'The New 3DS build keeps the native-60 frame pacing the first phase reached; the Old 3DS build gets to a '
             'comparable result by a different route — see <a href="frontier.html">Frontier</a> and <a href="boundaries.html">'
             'Pushing the boundaries</a> for the two builds compared side by side.</p>')
    b.append('<div class="cols2">')
    b.append('<div><h3>The stack</h3><ul>'
             '<li><b>decomp</b>: the F-Zero X decompilation (inspectredc/fzerox), untouched upstream, plus a patch stack.</li>'
             '<li><b>libultraship</b>: the N64 runtime (Kenix3), untouched upstream, including a 13-line newlib fix that lets it build on devkitARM; it still draws every menu and non-race screen.</li>'
             '<li><b>port/3ds</b>: ' + esc(D["loc_port3ds"]) + ' lines as of the first phase, since grown: a citro3d rendering backend, stereo, audio, input, menu, lifecycle, render thread, profilers, packaging.</li>'
             '<li><b>the native renderer</b> (added in the second phase): an owned interpreter that skips the per-frame '
             'display-list walk for race geometry — see <a href="frontier.html">Frontier</a> for how, and why it is '
             'what lets the Old 3DS keep up.</li>'
             '</ul></div>')
    b.append('<div><h3>The headline numbers on hardware, first phase (New 3DS)</h3>' + table(["When", "Median", "p10", "At cap"], D["headline_rows"], num_cols=(1, 2, 3)) + '</div>')
    b.append('</div>')

    b.append('<h2>The first phase in five lines</h2>')
    b.append('<ol>')
    b.append('<li><b>08-11 → 08-14: from research to a race.</b> Feasibility dossier, plan for a team of agents, five parallel foundation streams, six boot bugs, first race at 12–20 fps in the emulator.</li>')
    b.append('<li><b>08-20 → 08-22: from pink to playable, then to a real console.</b> Fog, HUD, livery, shadow, sky fixed; the loadblock discovery took menus from 27 to 60 fps; first hardware run 08-21 (\"wow it runs amazing\"); audio the same evening.</li>')
    b.append('<li><b>08-27 → 08-28: product grade.</b> Five-agent fleet drop, stereo armed, touch menu, rival detail, GPU vertex-transform moonshot built and shelved, README with legal notice, release package.</li>')
    b.append('<li><b>09-01 → 09-02: lifecycle and the last big lever.</b> HOME crash and close-from-HOME hang fixed; a bridge output cache proven a NO-GO and replaced by memoizing each sub-display-list\'s classification instead of re-scanning it every frame; LOCKED-60 campaign launched with four parallel levers.</li>')
    b.append('<li><b>09-02 → 09-03: multi-core.</b> Render thread on core 2, one-frame-ahead mode, bridge on main, auto LOD; median 59.6 with 53% of frames at cap; crash fixed; viewport placement fixed; stereo markers anchored.</li>')
    b.append('</ol>')
    b.append('<h2>The second phase in six lines</h2>')
    b.append('<p>Not originally planned; it ran 09-07 to 10-19, 43 calendar days.</p>')
    b.append('<ol>')
    b.append('<li><b>09-07 → 09-29: scoping, then making it fit.</b> The Old 3DS milestone, previously written off, was restarted; '
             'parallel agents fit the build into its 64 MB app region (the RDRAM arena halved, audio streamed instead of resident, '
             'texture uploads capped); on 2026-09-29, in the first hardware session on a real Old 3DS, the shipped (New-3DS-era, 16 MiB '
             'RDRAM) build died at boot while the <code>-DGDX_3DS_O3DS=ON</code> profile, built to fit the console\'s memory grant, booted '
             'and raced the same session at about 10 fps.</li>')
    b.append('<li><b>09-29 → 09-30: finding the real costs.</b> A narrowed audio lock cut the race-wall wait on the Old 3DS by about 20 ms '
             'a frame; frame skipping ended the game\'s accidental slow motion; a first retained-mode renderer won big in the emulator and '
             'barely moved the number on hardware.</li>')
    b.append('<li><b>09-30 → 10-01: the probe that changed the plan.</b> Hardware performance counters showed the real limit was '
             'instruction-cache misses, not instruction count. A pose-replay fallback was built and worked, then was set aside on the '
             'user\'s call: "we should go for a real 60! i still believe it has to be possible."</li>')
    b.append('<li><b>10-01 → 10-02: the native renderer.</b> Rebuilt around the cache instead of around instruction counts, one small '
             'loop per race category, sized to fit the 16 KB instruction cache. Median frame time on a 2D Grand Prix race reached 16.5 ms.</li>')
    b.append('<li><b>10-03 → 10-08: 3D at 60, on the Old 3DS too.</b> A per-eye resubmit with its own swap-pacing mode reached 59–60 fps '
             'in 3D on most Grand Prix courses; the heaviest courses (Silence, Sand Ocean) can still trip the frame-skip safety net to a '
             'steady 30 fps mid-race.</li>')
    b.append('<li><b>10-08 → 10-19: the Expansion Kit, a dashboard, and release.</b> The 64DD Expansion Kit verified tick-for-tick '
             'identical under the native renderer and was brought to the Old 3DS; the bottom-screen DASH shipped; a standing '
             'adversarial review audited the merged result before the four-way v1.0.0 release.</li>')
    b.append('</ol>')
    b.append('<p>See <a href="agentic.html">Agentic workflow</a> for how the second phase was run and '
             '<a href="old-3ds.html">Old 3DS</a> for the full campaign, card by card.</p>')

    b.append('<h2>Read on</h2>')
    b.append('<ul>')
    for f, t, s in PAGES[1:]:
        b.append(f'<li><a href="{f}"><b>{esc(t)}</b></a>: {esc(s)}.</li>')
    b.append('</ul>')
    b.append('<div class="note">Every number on these pages has a source: git history, the research documents under '
             '<code>docs/research/</code>, the hardware logs, or the session transcripts the <a href="cost.html">Cost</a> page counts. '
             'Where a value is an estimate (the dollar figures, which apply published API list prices to exact token counts), the '
             'assumption is stated next to it.</div>')
    return page("index.html", "G-Diffuser on 3DS", "".join(b))
