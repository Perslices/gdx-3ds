# tools/native/

Host-side helpers for the native renderer fork.

| file | what |
|---|---|
| `machines_harness.sh` | Builds and runs the retained-machine-mesh host harness. `tools/ci-3ds.sh` runs it as one of its seven host tests, so it has to keep working. |
| `machines-azahar-run.sh`, `machines-compare.sh`, `machines-stage-card.sh` | The emulator A/B and SD-card staging loop used while `machines` was being written. |
| `gfx_decode.py` | Decodes a captured PICA vertex batch into readable rows, for eyeballing a verify-mode mismatch. |

The fork's development notes live in `docs/research/native/` (`STATUS.md` first). Those
notes occasionally cite an agent-protocol file and a worktree bootstrap script that are
local to the development checkout and are not published here; nothing in the build or in
`tools/ci-3ds.sh` depends on either.
