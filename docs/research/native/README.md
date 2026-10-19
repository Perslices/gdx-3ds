# 3DS port — working notes

The development history behind this port kept internal progress notes. This page is not one of
them — it's a pointer to the docs that carry forward what matters from them. Source comments and
patch headers may cite internal note names (`<topic>-progress.md`, `<NN>-<topic>-plan.md`,
`integration-verify-<date>.md`) — read that as a pointer to internal history, not a path
guaranteed to resolve in this checkout.

What's published, consolidated, here instead:

| document | what it covers |
|---|---|
| [`docs/ARCHITECTURE.md`](../../ARCHITECTURE.md) | How the whole 3DS port fits together, with source paths: the decomp/port/libultraship split, the vendored renderer carve, the asset pipeline, segments, and the build flags |
| [`docs/NATIVE-RENDERER.md`](../../NATIVE-RENDERER.md) | What the native renderer is, the two tiers, every `[native]` ini key, how each lever is verified, and what is measured on hardware |
| [`docs/RELEASE-NOTES.md`](../../RELEASE-NOTES.md) | What changed, the measured result numbers, and the known issues |
| [`docs/DIAGNOSTICS.md`](../../DIAGNOSTICS.md) | Logs, crash dumps, debug ini keys, and what to attach to a bug report |
| [`docs/HW-DEV-LOOP.md`](../../HW-DEV-LOOP.md) | The WiFi hardware dev loop and the emulator-driving CLI |
| [`docs/3DS-HARDWARE.md`](../../3DS-HARDWARE.md) | Toolchain, build, SD layout, troubleshooting |
| [`docs/postmortem/`](../../postmortem/) | The published account of the port as a whole |
