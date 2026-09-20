# replays — canonical `.gdxrec` input recordings (input data, not game assets)

Format: `port/gdx_input_rec.h`. Stage with `tools/dev/oracle-run.sh --replay <name>` (copies to
sdmc `rec/<name>.gdxrec`, sets `[debug] input_replay=<name>`) or `gdx-dev emu run --replay`.

| File | Build | Pins | Content |
|---|---|---|---|
| `race-mutecity-start.gdxrec` | `native/oracle@c3021cfb`, Azahar | `det=1` (seed1 0x1234abcd, seed2 0x0fedcba9, boot_hold_ticks 230, det_audio 1), renderthread=0, stereo=1 (slider 0) | 6001 ticks: TITLE → START@400 → MAIN_MENU@460 → COURSE_SELECT@1417 → MACHINE_SELECT@2046 → MACHINE_SETTINGS@2604 → **GP_RACE@3226** (Mute City, A held + steering) → retire/results@4895 → quit@6000. SHOTs: title@350, mainmenu@1000, machsel@2300, race00@3300, race01@4000, race02@5000, race04@5900, race03@6000 |

## residuals-track probes

Tick-exact truncations of `race-mutecity-start.gdxrec` (same header, same pins) with their own
`SHOT` sets. They exist because the RetroArch lane cannot afford `fh_every=N` (see
`docs/research/native/residuals-progress.md`, "Gotchas"): a `SHOT` still emits a full
`[fh] frame=… hash=…` receipt with `fh_every=0`, so a sampled hash sequence costs nothing
between shots.

| file | ticks | SHOTs | for |
|---|---|---|---|
| `probe2.gdxrec` | 1811 | 16 | lever bisect — one frame per screen plus 1700..1704 |
| `probe3.gdxrec` | 1811 | 43 | parity gate — every 100 frames, **every frame 1690..1712**, 1750/1800/1810 |
| `probeboot.gdxrec` | 420 | 7 | fast boot/title check (~1 min/run) |
| `probetitle.gdxrec` | 1300 | 9 | 1300 ticks of ZERO input: stays on the title long enough for the audio-driven attract-demo trigger to be reached. Reproduces the boot residual pre-pin. |
