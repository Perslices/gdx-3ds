# Next work — native fork (as of 2026-09-20, native/main 8b3ecfa)

State: all ten planned tracks merged; every lever pixel-identical to stock in the emulator, together and
apart; full stack 1.85x on the emulator crowd frame; two hardware sessions (run E: Tier-2, run G: full
stack) with 0 errors. Open on hardware: the HOME hang after run E, the "distant wobble / more pop-in" remark.

## A. Hardware-gated (user runs the card, agent acts on the log)

| # | Task | Card | Gate / outcome |
|---|---|---|---|
| A1 | HOME step diagnosis | SD now (homefix, all=1 interp=0, apt_watch=1) | last `[apt]` line names the step -> fix -> HOME clean in menu / race / transition, both lever states |
| A2 | Wobble + pop-in A/B | `G-Diffuser-3DS-wobble` runs S/M/T/I/G/P | P (rival NATIVE, auto off) decides: if clean -> dynlod hysteresis fix (A4); else per-lever culprit -> targeted fix |
| A3 | hwlink day-one gate | `G-Diffuser-3DS-hwlink` item B | self-relaunch over WiFi works -> SD loop retired; agents push builds and pull logs/captures themselves |
| A4 | dynlod hysteresis | (after A2) | asymmetric dwell / longer LOWER block, tuned from the `[dynlod]` lines; killswitch; no oscillation over a GP |
| A5 | Hardware `[prof]` A/B of the full stack | native1 card with gputrace=1, all=0 vs all=1 vs all=1+interp=1 | first real ms per section on hardware; decides default-on |
| A6 | Stereo on hardware | wobble run Z / native1 H1 | `anchor=` receipt, both eyes, no artifacts under native levers (the emulator never runs the right-eye pass) |
| A7 | 30-min soak, full stack | native1 H2 | 0 errors, heap flat, HOME/sleep/close clean |

## B. Agent-runnable now (no hardware needed)

| # | Task | Why | Gate |
|---|---|---|---|
| B1 | Long-tail replays | owned interp never exercised course editor / EK screens / ending / records / machine creator | replays committed; interp=1 parity 0 divergent on each |
| B2 | `fx_hud` equivalent in the owned interpreter | fx_hud is inert under interp=1 (more draws) | prim/env fold inside owned batches; parity; draws/frame match LUS+fx_hud |
| B3 | Tunnel + 4P + second course replays | cpuclip A/B, split-screen fold, course coverage beyond Mute City | replays committed; parity all paths |
| B4 | Owned interp `dsp` remainder | emu crowd: dsp 20 ms of 30 total after br->0 | per-opcode census on owned path; batch-split classes; target -5 ms emu |
| B5 | Explosions class split (fold=p459 split=e459) | last per-sprite draw class; vertex colour carries one colour | pack env into a second vertex colour or per-class constant; parity |
| B6 | interp M5 prep: delete list | inventory what the bridge still owns (segments, DMA fences, transition capture, VI fallback) | **DONE** — `m5-delete-plan.md` (53 survivors relocated, 6 steps, 3DS-only split) |
| B7 | Owned-interpreter `interp_verify` with Tier-2 levers | untested combination | 0 mismatches over the replay |
| B8 | Release engineering | public repo `native` branch, CI workflow from release repo applied, STATUS.md as README section | fresh-clone `tools/ci-3ds.sh` PASS in the devkitpro container |

## C. After hardware verdicts

| # | Task | Gate |
|---|---|---|
| C1 | Defaults: `[native] all=1`, `interp=1` on | A5 + A6 + A7 green; HOME clean |
| C2 | interp M5: delete bridge + LUS interpreter (keep o2r reader) | C1; 30-min soak; long tail (B1) |
| C3 | v1.0 native release (public repo, release notes, post-mortem addendum) | C2 |
| C4 | Old 3DS milestone: render thread folded into main, memory diet (audio_table, texture set per race), 268 MHz budget | needs an Old 3DS unit (user does not own one) |

Recommended order: A1 -> A2 -> A3 (hardware, in that order) while B1, B3, B6, B8 run as agents now;
A4/A5/A6/A7 as soon as A3 removes the SD round trip; then C1 -> C2 -> C3.
