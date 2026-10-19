# G-Diffuser 3DS — Modding

**Texture-pack modding is not available on the 3DS port in this release.** This page documents
the texture-pack workflow that exists in the PC port of G-Diffuser — this repository's own
PC build, not a separate project — and then explains why the 3DS build doesn't have it.

## Building a texture pack (PC port)

The PC port supports drop-in texture packs through its Workshop tab (open the enhancement menu
with **F1**):

1. In-game, open the Workshop tab and run **Dump textures while playing**. This writes a dump
   folder containing one PNG per texture plus `manifest.tsv` — a line per texture key with its
   native width, height, and N64 texel format.
2. Edit or replace the PNGs you want to override. A replacement must be an integer multiple of
   its native size (1x, 2x, 4x, …); `manifest.tsv` records that native size for every key.
   Paletted formats (CI4/CI8) can be dumped but not replaced yet — see the PC-port README's
   Planned Features.
3. Pack the folder into a `.o2r` with `tools/gen_texture_pack.py`:

   ```sh
   python tools/gen_texture_pack.py dump/ my-pack.o2r --name "My Pack" --author you
   ```

   `--check` validates a dump folder, or an existing `.o2r` pack, without writing anything —
   run it before sharing a pack. The script lists every problem it finds (bad PNGs, sizes that
   aren't an integer multiple, keys missing from `manifest.tsv`) instead of stopping at the
   first one.
4. Drop the `.o2r` in a `mods/` folder next to the game executable, enable **texture packs** in
   the Workshop tab, and reload. A numeric filename prefix (for example `10-hifonts.o2r`)
   controls load order when more than one pack touches the same texture.

| Command | Does |
|---|---|
| `gen_texture_pack.py dump/ out.o2r --name N --author A` | Build a pack from a dump folder. |
| `gen_texture_pack.py dump/ --check` | Validate a dump folder; write nothing. |
| `gen_texture_pack.py out.o2r --check` | Validate an existing pack; write nothing. |

## Why the 3DS port doesn't have this

The Workshop system above is built from shared code that happens to be linked into the 3DS
binary too, but the 3DS build doesn't expose it: there's no Asset Dump screen, no "open mods
folder," no "reload packs" control anywhere in the console's touch-screen settings. The folder
Workshop would use doesn't resolve to anything meaningful on a 3DS title either — it's derived
from "the executable's own directory," which a 3DS title doesn't have. Nothing in this release
wires it up, so there is no supported way to install a texture pack on console today.
