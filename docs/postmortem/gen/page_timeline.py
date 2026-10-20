from framework import *


def build(D, commits_per_day, prompts_per_day):
    b = []
    b.append('<p class="eyebrow">03 · Every step</p><h1>Step by step: twenty-four days, then forty-three more</h1>')
    b.append('<p class="lede">The project ran in five bursts separated by three credit pauses. Dates are from git and the session '
             'transcripts (local time); quotes are the user\'s own words at that moment.</p>')

    days = sorted(set(list(commits_per_day) + list(prompts_per_day)))
    svg = grouped_bars([d[5:] for d in days], [("commits to the 3DS port", "--c1", [commits_per_day.get(d, 0) for d in days]),
                                               ("user messages", "--c2", [prompts_per_day.get(d, 0) for d in days])],
                       ylabel="per day", dec=0, h=260, vals_on=False)
    b.append(figure(svg, "<b>The rhythm of the project.</b> Commits touching the port and messages typed by the user, per day. "
                    "Gaps are three pauses when the weekly usage allowance for the project's regular model (the \"Fable\" tier) ran out "
                    "(08-15 → 08-19, 08-22 → 08-26, 08-29 → 08-31).",
                    legend([("commits", "--c1"), ("user messages", "--c2")])))

    b.append('<h2>Burst 1 · Research to first race (08-11 → 08-14)</h2>')
    b.append('<div class="timeline">')
    b.append(event("08-11 03:16", "Deep research: is a 3DS port feasible?", "First message of the project invoked the deep-research workflow on libultraship and 3DS targeting. Verdict: New 3DS only; the Old 3DS 64 MB region cannot hold it. \"new 3ds as target makes total sense\"."))
    b.append(event("08-11 03:44", "Plan for a team of agents", "\"lets create a plan for team of agents to work. can we segment plan so we can have multiple work trees and multiple contributing agents ?\" A dossier, a memory budget, a five-stream plan (A gfx, B os, C audio, D assets, E 32-bit sweep, F census) and a reviewer agent for the plan.", "big"))
    b.append(event("08-11 → 08-12", "Phase 0 and the carve spike", "Contract headers, stream directories, a stub build. The libultraship carve spike proved the runtime compiles on devkitARM with a 13-line newlib patch. Phase 1 streams ran in parallel worktrees and merged in order E→B→A→D→C on 08-12 21:45."))
    b.append(event("08-13 01:07", "\"start the crew\"", "M1 integration: the full game compiled and linked on 08-13 01:33. Then the black screens: bzero, 64DD, sdmc: path mangling, double swap, the SETTIMG low-address guard, the DMA misroute. User at 05:37, 05:42, 12:44, 13:00, 13:16: \"still black screen\"."))
    b.append(event("08-13 13:17", "First pixels", "\"oh! i saw some random shapes briefly for the menu or something. just a white screen with a rainbow rectangle at the bottom\"."))
    b.append(event("08-13 → 08-14", "Race entry", "Race freeze (DMA misroute) at 19:26, then a race-time death exposed as an uncaught bad_alloc (EnqueueList's worst-case reserve), fixed by capping the resource cache. \"crashed again when race tried to load\", \"says fatal error encountered\"."))
    b.append(event("08-14 01:01", "First race", "\"omg it works ! i can race ! it looks messed up and runs at 12 to 20fps but it works!\" Nine minutes later: \"i didnt see any hard crashes right now, so lets see about the visual fixes. any work that can be done in parallel ?\"", "gold big"))
    b.append(event("08-14 04:26", "Hardware prep, CI, docs", "\"lets do 1, 2, and 3. for CI though, no github actions. this will all be local for now.\" SMDH metadata, makerom CIA packaging, the real-hardware runbook, a local ci-3ds.sh, the user guide. Two deep-research runs: 60 fps on New 3DS, and stereoscopic 3D. The stereo foundation and GPU profiling telemetry landed the same night."))
    b.append(event("08-14 14:06", "Pause 1", "\"we ran out of fable credits until next week, lets capture all the inflight tasks and store state so we can pick up the work later.\" A resume note was written for the next session; the fog blocker (pink road) open; three agents in flight.", "bad"))
    b.append(event("08-15", "Opus interlude", "A deep-research pass on Fast3D vs RT64 confirmed the renderer choice; an asset-cost shift (ETC1, decimation) was added to the plan and marked \"done with opus, reconsider with fable\"."))
    b.append('</div>')

    b.append('<h2>Burst 2 · Playable, then hardware (08-20 → 08-22)</h2>')
    b.append('<div class="timeline">')
    b.append(event("08-20 02:55", "Resume on Opus 4.8", "\"can you resume from where we last left off ? we should have tokens\". Fog solved at 01:57 local (per-vertex blend, not the depth LUT): the road appeared. Consolidation merge of fog + gpuprof + stereo + perf + cull.", "big"))
    b.append(event("08-20 05:38", "\"definitely run ! run as many subagents on plausible tasks\"", "Fleet on HUD garbage, shadow, boost, sky wedge, tunnel roof, km/h, building pop-in, CI. The measurement pass returned the CPU-bound verdict. Night session 1 while the user slept."))
    b.append(event("08-20 12:45", "Night branch promoted", "\"sure, promote it. i dont see any large regressions. probably better\"."))
    b.append(event("08-20 22:45", "The HUD bug", "Root cause found after eight wrong fixes: the bridge's ILP32 high-bits backstop dropping 13 SETTIMG per frame. \"omg finally!!\"", "gold"))
    b.append(event("08-21 00:23", "\"insanity is doing the same things and expecting different results\"", "User demanded new methods for the remaining visual bugs; deep-research run on the fixing methods. Night session 2: S7 memos, async frame mirror, vblank skip, the env-colour flush that fixed rival-coloured machine bodies."))
    b.append(event("08-21 13:03", "\"menus … still crazy slow at around 10fps\"", "Sectioned profiler → per-opcode profiler → G_LOADBLOCK at 22.34 ms per frame. Fixed within the hour. 14:09: \"oh dude the menu was way faster ! … blazing fast on the UI\". 14:14: \"oh this is awesome !!\"", "gold big"))
    b.append(event("08-21 14:41", "\"i think id like to try it on a real device\"", "The .cia and .3dsx were packaged. 15:54: \"wow it runs amazing ! like you were suggesting, maybe even better than the emu. sound doesn't work btw. sound has never worked on the emu either.\"", "gold big"))
    b.append(event("08-21 15:54 → 21:22", "The audio afternoon", "Eleven SD round trips. Test tone heard, then not; music on PC, then not; the wrong build blamed. Root cause: the ROM-side audio driver needed host porting and linear interpolation for the real DSP. 21:22: \"working! giant win.\"", "gold"))
    b.append(event("08-21 22:49", "\"would love to see the framerate at a crispy 60. then... maybe 3D?\"", "Race-start hitch fixed (blob preload off the audio thread), gputrace made to reach hardware, the yellow-decal ship fixed by clamping shader-clamped tiles. Hardware: median 48–50 fps, p95 58."))
    b.append(event("08-22 01:31", "Pause 2", "\"agh, ran out of tokens again. lets wrap up the work for now to resume when i have fable 5 access again\". Resume checkpoint 854b9d6.", "bad"))
    b.append(event("08-23 20:50", "The friend question", "\"im showing this to my friend, and he is unimpressed by this port to 3DS. why is it impressive in laymans terms?\" A short separate session answered it; the <a href=\"frontier.html\">Frontier</a> page is the long answer."))
    b.append('</div>')

    b.append('<h2>Burst 3 · Product grade (08-27 → 08-28)</h2>')
    b.append('<div class="timeline">')
    b.append(event("08-27 06:21", "Fleet drop", "\"hi, i believe we are good to resume … feel free to spin up any necessary subagents to do parallel work.\" Five agents landed and batch-merged by 03:22 local: traffic grind (br 19.7 → 10.4 ms), sky wedge (padding sampling, proven with magenta), CCMUX 11, exact off-axis stereo, shadow audit.", "big"))
    b.append(event("08-27 16:00", "Hardware round with stereo armed", "\"performance is much better! 3D works great, doesnt seem to affect performance much at all. last race did have 30fps AND had a hard crash (first time ever!)\". The crash was the filelog growing without bound; fixed with the retained ring, plus the upstream combiner-key leak found the same afternoon."))
    b.append(event("08-27 16:53", "\"the bottom screen should have a menu for these features\"", "Touch menu v1 with STAT/DISP/3D/AUD/INP/LOG/DBG/ABT tabs, config write-back, live setters. 19:21: \"playing perfectly, full bleed should be default display mode i think\". Full-bleed made the default at 15:21 local."))
    b.append(event("08-27 19:30", "\"i did the first GP! worked great.\"", "Then Time Trial froze on its first menu: course-select state never reset in a static non-EK build (retail relied on overlay re-DMA). Fixed 16:18 local."))
    b.append(event("08-28 00:18", "\"lets do it, but lets do this definitely on a branch. all of these seem relatively high risk but worth a try\"", "Rival detail option, TRILOOP packed VBO, APT power/HOME exit fix. 04:22: \"minimal appears to be a 8 to 9 fps gain ! sometimes a little more. def worth it.\""))
    b.append(event("08-28 04:27", "\"moonshot! lets go for it\"", "GPU vertex transform built in one agent session (0e1068f): works, imperceptible on hardware, breaks the venue floor. Shelved with a report. 13:08: \"visual oddity: gpuxform on, the floor outside the track loses texture mapping.\"", "gold"))
    b.append(event("08-28 13:17", "\"lets try pushing the last lever. lets also write up the README for release\"", "README-3DS with the legal notice (educational, no Nintendo affiliation, no ROM or assets). Bridge translation cache brief written. 13:39: \"lets gather up everything to pause/checkpoint for that final push\". Pause 3.", "bad"))
    b.append('</div>')

    b.append('<h2>Burst 4 · Lifecycle and the last big lever (09-01 → 09-02)</h2>')
    b.append('<div class="timeline">')
    b.append(event("09-01 21:27", "\"lets proceed with the things we were working on. the goal to 60fps\"", "Back on the next usage tier (\"Fable 5.1\"), the bridge cache agent found: census said 87% of commands are host-built per frame, output cache NO-GO; pivot to brfast memos, br 11.5 → 4.9 ms. Deep research on render architectures: nobody has shipped beyond the per-frame walk.", "big"))
    b.append(event("09-01 23:53", "\"oh! also major bug. the home button crashes the game!\"", "Suspend-path audit, audio park gate. Fixed 20:56 local. Combined test build staged with A/B inis and the first TEST-PLAN.txt."))
    b.append(event("09-02 14:02", "Hardware round 2", "\"i think brfast helped with framerate … home menu came up! i was also able to return to game. but when i tried to 'close' the software it hung.\" Close-from-HOME root cause (sticky park event, hot spinner) fixed 10:18 local. Median 57.2 vs 56.0, p10 47.2 vs 42.0 with brfast on vs off."))
    b.append(event("09-02 14:41", "\"lets start on those projects. divide the work into appropriate subtasks for multiple agents\"", "LOCKED-60 campaign: Task A HUD texrect atlas, B TMEM bookkeeping, C per-triangle cost, D boot audio. All four landed by 13:40 local; staged as hwtest2.", "big"))
    b.append(event("09-02 18:35", "\"working but VERY slow. massive step back. UI works but tons of graphical errors\"", "The combined-lever HUD break (trifast without the atlas UV offset). Fixed at 14:30 local; the rule \"combined-lever screenshot before staging\" written. 18:59: \"ok seems fine right now\"."))
    b.append(event("09-02 21:56", "Hardware round 3 and 4", "All four levers on with stereo on: median 56, p10 48, 32% at cap. Same-course A/B with trace on: steady 16.4 → 14.9 ms, crowd 20.2 → 18.7 ms. Merged to mainline."))
    b.append(event("09-02 21:59", "\"lets do /deep-research on whats left, come up with plan, and execute subagents for all parallel work\"", "Research 2: core 2 usable, core 3 not, no precedent for cross-core render pipelining. Agents F (dispatch census), G (boot audio 2), H (render thread on core 2). 22:11: \"so curious if the multi core will be profitable!\""))
    b.append('</div>')

    b.append('<h2>Burst 5 · Multi-core (09-02 night → 09-03)</h2>')
    b.append('<div class="timeline">')
    b.append(event("09-02 18:44 → 21:47 local", "Render thread M2 → M6", "Sync mode, then pipe mode forked at osSpTaskStartGo, then ahead mode (DP-done acked when the game parks). The console filter fixed the bottom-screen tab bar the user reported at 00:05 (\"debug doesnt have tabs anymore\")."))
    b.append(event("09-03 01:39", "\"so median is up +7fps?\"", "Hardware round 3 (pipe mode): median 49 → 56, p10 42 → 48 with the trace on; stable, HOME clean, but not yet overlapping. 01:46: \"got it. can you work on 2, 3, and 4 right now in parallel?\" → Tasks I (bridge on main), J (auto rival detail), E (machine atlas)."))
    b.append(event("09-03 04:49", "\"ok SD in. can you set the render thread?\"", "Round 4 card: ahead mode + bridge on main + auto LOD, full Grand Prix, trace off."))
    b.append(event("09-03 13:20", "Round 5 verdict: median 59.6, p10 51.4, 53% at cap. And a hard crash.", "\"i did a full GP and then i poked around the menus and there was a hard crash. also, is there any way you could create a webpage with some histograms of the performance\". Crash = texture cache LRU splice on core 2; fixed by render-thread ownership. The performance report page was written as a local HTML file.", "gold big"))
    b.append(event("09-03 13:46 → 15:23", "The menu placement bug", "User captured side-by-side screenshots against the real game: machine-select ships 1.6× too large. Viewport fold fix, verified against the references. \"sorry didnt mean to interrupt but its looking good\"."))
    b.append(event("09-03 16:32", "Stereo markers", "\"one thing thats really jarring is the 1P/2P/3P markers during a race. they sit on the most foreground in 3D but the ships are somewhere in the distance\". Anchored at the labeled ship's depth via the game's own prim-depth register; verified in side-by-side stereo; staged as round 5 with the crash and viewport fixes."))
    b.append(event("09-03 18:08", "This post-mortem", "\"lets write up a whole post mortem on this project.\"", "big"))
    b.append('</div>')

    b.append('<h2>State at the time of writing</h2>')
    b.append('<p>Everything staged at this point shipped or was closed out in the chapters below: the pending '
             'build\'s ahead mode, bridge-on-main and automatic rival detail all merged, along with the texture-cache '
             'crash fix, the viewport fix and the stereo marker anchor. The GPU vertex transform stayed shelved (see '
             '<a href="road-to-60.html">Road to Sixty</a>); the tunnel-roof bug (#27) is still open at release.</p>')
    b.append(native_fork_begins())
    b.append(old3ds_section())
    b.append(shipping_it())
    return page("timeline.html", "Every Step", "".join(b))


def native_fork_begins():
    b = []
    b.append('<h2>Chapter 2 · The native fork begins (09-07 → 09-23)</h2>')
    b.append('<p>A feasibility pass on 09-07 inventoried what the New 3DS build actually used of libultraship\'s renderer and concluded the '
             'fix should start with the development loop, not the renderer: no network path to the console, no reliable frame oracle, and '
             'no desktop reference build on the machine doing the work. Over the next two weeks that loop was rebuilt (a network control '
             'and log link to the console, input-recorder determinism, a Linux reference render container) and the first renderer tier — an '
             'owned F3DEX2 interpreter replacing libultraship\'s — landed, along with a native track, machine, sky, HUD and effects path for '
             'races, and the first pass of the 64DD Expansion Kit on console. That work, and the chapters it enabled below, is told in full '
             'on <a href="native.html">The Native Fork</a>; this page returns to a day-by-day account once the Old 3DS work opens on 09-24.</p>')
    # One chart for the whole native-fork window (09-07 → 10-19, chapters 2-4): this period has no
    # archived session transcripts (source-timeline.md §7), so commits are the only per-day signal
    # available across its full range, unlike the message-rich New 3DS chapter charted above.
    cpd = {"09-12": 45, "09-13": 34, "09-17": 42, "09-20": 18, "09-23": 317, "09-24": 122, "09-25": 8,
           "09-26": 84, "09-27": 31, "09-29": 127, "09-30": 120, "10-01": 163, "10-02": 95, "10-03": 20,
           "10-07": 78, "10-08": 139, "10-15": 79, "10-16": 31, "10-17": 26, "10-18": 11, "10-19": 9}
    days = sorted(cpd)
    svg = grouped_bars(days, [("commits to the native-fork branch", "--c1", [cpd[d] for d in days])],
                       ylabel="commits per day", dec=0, h=260, vals_on=False)
    b.append(figure(svg, "<b>The native fork, per day, 09-07 → 10-19.</b> Commits reachable from the native integration branch; "
                    "this period has no archived session transcripts, so commits are the only per-day signal available, unlike "
                    "the message-rich New 3DS chapter charted above. Gaps are five rate-limit or session pauses, the longest a "
                    "full week (10-08 → 10-15, right after the Old 3DS campaign's final hardware card)."))
    return "".join(b)


def shipping_it():
    b = []
    b.append('<h2>Chapter 4 · Shipping it (10-08 → 10-19)</h2>')
    b.append('<p>Card 4 (10-08) ended the Old 3DS performance campaign at 59–60 fps (245–256 of 256 frames on the first vblank) in 3D '
             'Grand Prix races. The eleven days that '
             'followed — with a week-long rate-limit pause in the middle — finished the rest of what ships as v1.0.0: the Expansion Kit '
             'confirmed and packaged for the New 3DS (the Old 3DS build ran, too slowly to ship), the bottom-screen dashboard taken from a debug console to a real HUD, a New 3DS '
             'freeze root-caused and fixed, and the disk-streaming defaults this release ships with. Dates below are commit dates on the '
             'native-fork integration branch; this period\'s work is not backed by the session transcripts the earlier chapters quote '
             'directly, so this account is commit- and document-sourced rather than quoted.</p>')
    b.append('<div class="timeline">')
    b.append(event("10-07 → 10-08", "Expansion Kit: save and title separated", "The Expansion Kit gets its own HOME-menu title and its own save file, ending the last blocker to shipping it as a second, side-by-side install. Hardware cards for both consoles staged the same day Old 3DS 3D racing reached 60 fps."))
    b.append(event("10-08 → 10-15", "Rate-limit pause", "A week-long gap in the commit history, the longest of the project.", "bad"))
    b.append(event("10-15 → 10-17", "The bottom screen becomes DASH", "The proven minimap-only bottom screen is extended into a full race dashboard: lap, position, energy, splits, best time, composited into the console's own framebuffer at well under a millisecond of CPU time. A fourth, fully custom drag-and-resize layout mode is designed and mocked up."))
    b.append(event("10-16", "The Old 3DS memory ceiling, reproduced under simulation", "A forced run on the Azahar emulator, under the real 64 MB budget simulation, finds the budget can be exhausted by a single large asset load mid-race under worst-case conditions; traced to the generic archive-file loader, not any one feature. A separate real-hardware capture (heapUsed≈24.2 MB peak) suggests the margin is thin on real hardware too, though the crash itself has not been reproduced there. Left as a known limit rather than a last-minute structural fix.", "bad"))
    b.append(event("10-16", "An audio-crackle hypothesis, tested and refuted", "The leading theory for Old 3DS audio crackle — a vblank-vs-audio-rate mismatch — is measured and ruled out on both logical and empirical grounds; a separate, smaller audio fix ships instead."))
    b.append(event("10-18", "A New 3DS froze at the title screen, every time", "The attract-mode demo's one full-field race demo deterministically freezes a real New 3DS, with no crash dump — see <a href=\"native.html\">The Native Fork</a> for the root cause and fix.", "gold big"))
    b.append(event("10-18 → 10-19", "The custom HUD mode is shelved; the shipped three stay", "The fourth, fully custom dashboard layout is set aside before release; the three fixed placement modes ship. The bottom screen also gets an idle view outside races and a DEBUG-button rename."))
    b.append(event("10-19", "Disk streaming and Japanese inputs become the default", "The Expansion Kit's 64DD disk image streams from the SD card instead of copying into RAM, and streaming becomes the default for every 3DS build; Japanese-region inputs are accepted by default, since the only retail 64DD disk is Japan-only.", "gold"))
    b.append(event("10-19", "v1.0.0", "Four console builds — Old 3DS and New 3DS, each with and without the Expansion Kit — from one source tree, replacing the 09-04 (v0.9.0) New-3DS-only release.", "gold big"))
    b.append('</div>')
    return "".join(b)


def old3ds_section():
    b = []
    b.append('<h2>Chapter 3 · The Old 3DS (09-24 → 10-08)</h2>')
    b.append('<p>Dates and times below are local; quotes are the user\'s messages to the coordinating session. This chapter\'s own '
             'commit and message rhythm is part of the full native-fork chart above; the per-card performance numbers are on the '
             '<a href="old-3ds.html">Old 3DS</a> page.</p>')
    b.append('<div class="timeline">')
    b.append(event("08-11 (chapter 1)", "\"New 3DS only\"", "The project's first deep-research verdict: the Old 3DS 64 MB region cannot hold the port."))
    b.append(event("09-24", "The milestone reopens, with a correction", "A stale note had said the user does not own an Old 3DS; they do. Four parallel subagents work under a simulated 64 MB grant: RDRAM arena 18% used, halved to 8 MiB; <code>audio_table</code> streamed.", "big"))
    b.append(event("09-25", "It fits, in simulation", "The last unattributed heap use is closed; canonical replay 49/49 under the simulated <code>memtype=0</code> grant with 2.65 MiB to spare."))
    b.append(event("09-28 evening", "The console says <code>memtype=0</code>", "Luma 6.6 / arm9loaderhax means hbmenu will not start, so every build goes as a CIA through FBI. The grant matches the simulation byte for byte; the shipped RDRAM-16 build dies in its boot preload."))
    b.append(event("09-29 00:10", "\"it ran ! at like 9fps. but it ran! home button crashed it too\"", "RDRAM 8 build: wall p50 96 ms, GPU busy 3.6 ms. 01:00: \"lets just work on this entirely. also is there anything we can do to avoid the display lists and render things in a more traditional way?\"", "gold big"))
    b.append(event("09-29 day", "The parallel wave", "The audio-lock wait (25.8 ms → 0.2), a single Old 3DS profile flag, the HOME freeze (the bottom-backlight session held open; found by probe apps and a bisect card), ARMv6 SIMD audio synthesis (bit-exact on silicon), the slow-motion discovery, plus flag/LOD/GPU-transform/interpreter follow-ups. 12:19: \"we should working on as many in parallel\"."))
    b.append(event("09-29 13:09", "\"we want this running at 60fps\"", "13:13: \"lets start the design agent. it can also start up subagents to do different parts. a suborchestrator if you will\". A staged retained renderer design (five research and probe subagents) begins.", "big"))
    b.append(event("09-29 15:16", "\"playable but nausea inducing\"", "Frame skipping puts logic at 57–60 ticks/s, drawn 19–28 fps; draw-distance pop-in removed from the preset; draw cost measured at 10.9 µs per draw."))
    b.append(event("09-29 19:39", "First release candidate does not boot", "Deterministic boot crash. A four-title bisect card: cold size-optimised code crashes, stock boots. Code generation reverted to stock; the latent bug was never found.", "bad"))
    b.append(event("09-30 01:10", "Second release candidate boots", "\"framerate in race was like 15fps ... a lot of awkward hitching\". Root cause: synchronous SD log flushes, 26–270 ms on main."))
    b.append(event("09-30 02:09", "\"start as many agents as necessary\"", "Plus a research lead with five lenses and an adversarial review. All seven running agents hit the API session limit together and were resumed; later the same day the disk filled (ENOSPC) and stopped everything.", "big"))
    b.append(event("09-30 → 10-01", "Third release candidate and a hardware profiler", "\"no hitches afaik felt steadier\", but the steady race window barely moved. CPU performance counters: CPI 4.34, fetch stalls 43.6%; a probe found the cliff from CPI 1.06 at 16 KB of hot code to 4.1 at 24 KB.", "gold"))
    b.append(event("10-01 08:57", "\"we should go for a real 60! i still believe it has to be possible.\"", "A pose-replay shortcut gave 40 images/s with visible artifacts and was set aside. An instruction-cache-first native race renderer starts as its own sub-effort.", "big"))
    b.append(event("10-01 22:41", "First native-renderer build freezes at race start", "\"it showes mute city and then instant freeze\". A verify mode had rolled back the live GPU command queue; hardware-only.", "bad"))
    b.append(event("10-02 09:28", "\"time trial was silk 60fps the entire time, no hitching!\"", "Card 2: verify mode checked 199,472 draws, 0 bad; Grand Prix 45–50 fps with log-flush hitches. Then a pre-existing bug surfaced: saves after the first never persisted (the SD rename call returned an undocumented error)."))
    b.append(event("10-02 23:19", "\"omg it ran amazing! i fixed the 3d slider. i want to try it !\"", "Card 3: race median 16.5 ms, 0 race hitches, save survives a power cycle. 23:22: \"lets focus on 3D at 60fps\".", "gold big"))
    b.append(event("10-03 09:38", "Black sky", "An interim stereo configuration on the card-3 build: the native sky path consumed its section but could not draw in stereo.", "bad"))
    b.append(event("10-03 14:36 → 18:38", "3D, first pass", "\"running better! looks good ... just didnt run at 60\"; then \"60fps for 2d for both\", 3D in the 40s and 50s. The right eye is interleaved per chunk."))
    b.append(event("10-03 → 10-07", "Rate-limit pause", "The weekly limit stopped the fleet; one branch's uncommitted edits were recovered on 10-07. 13:10: \"ok let's proceed with as many subagents as possible for concurrency\".", "bad"))
    b.append(event("10-07 22:51", "3D, second pass", "Mute City holds ~60 in 3D with end-mode pacing; heavier venues at 16–18 ms. \"battery died on 3ds\"."))
    b.append(event("10-08 02:08", "\"all 3 crashed\"", "A VRAM-mapping change for the bottom screen put the framebuffer somewhere the CPU could not write to under a real CIA; the emulator does not enforce that mapping.", "bad"))
    b.append(event("10-08 02:56", "\"did a long play on main, was good!\"", "Same build with the VRAM change reverted: 3D Grand Prix race windows at 59–60 fps; verify mode 0 bad draws.", "gold big"))
    b.append(event("10-08 03:01", "This chapter, and the next", "\"can you start updating the postmortem doc with these new advancements / branches ?\" — the request this release's post-mortem traces back to.", "big"))
    b.append('</div>')
    return "".join(b)
