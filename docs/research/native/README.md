# Native renderer — working notes

The native-renderer fork was developed across a large number of short-lived branches, each of
which kept its own progress note, evidence directory and run manifests. Those notes are
**development-local**: they cite branch worktrees, scratch directories, emulator run ids and
per-session artifacts that mean nothing outside the machine they were written on, and they are
not published here.

Source comments and patch headers in this tree cite them by name
(`<track>-progress.md`, `<NN>-<track>-plan.md`, `integration-verify-<date>.md`). Read those
citations as "there is a longer write-up of this decision in the development history", not as a
path you can open in this repository.

What is published instead:

| document | what it covers |
|---|---|
| [`docs/NATIVE-RENDERER.md`](../../NATIVE-RENDERER.md) | What the native renderer is, the two tiers, every `[native]` ini key, how each lever is verified, and what is measured on hardware |
| [`docs/NATIVE-RELEASE-NOTES.md`](../../NATIVE-RELEASE-NOTES.md) | The beta release notes: the result, the caveats, and what has not been run |
| [`docs/3DS-HARDWARE.md`](../../3DS-HARDWARE.md) | Toolchain, build, SD layout, the full ini reference, troubleshooting |
| [`docs/postmortem/`](../../postmortem/) | The published account of the port as a whole |
