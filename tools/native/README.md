# tools/native/

Host-side helpers for the native renderer fork.

| file | what |
|---|---|
| `machines_harness.sh` | Builds and runs the retained-machine-mesh host harness. `tools/ci-3ds.sh` runs it as one of its host test binaries, so it has to keep working. |
| `machines-azahar-run.sh`, `machines-compare.sh`, `machines-stage-card.sh` | The emulator A/B and SD-card staging loop used while `machines` was being written. |
| `gfx_decode.py` | Decodes a captured PICA vertex batch into readable rows, for eyeballing a verify-mode mismatch. |

The fork's development notes are not published here — see
[`docs/research/native/README.md`](../../docs/research/native/README.md) for what is published
instead. Those notes occasionally cite an agent-protocol file and a worktree bootstrap script
that are local to the development checkout and are likewise not published; nothing in the build
or in `tools/ci-3ds.sh` depends on either.
