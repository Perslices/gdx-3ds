# Bottom Screen Designer

Local, self-contained page for laying out the 3DS bottom screen (MENU tabs + DASH race HUD)
and pinning notes on it. Open `index.html` in a browser (file:// is fine).

- `current-layout.json` — the in-game layout extracted from `port/3ds/gdx3ds_menu.c` and
  `port/3ds/gdx3ds_bottomhud.c` (pixel rects on 320x240, 8px console cells).
- `embed_seed.py` — copies `current-layout.json` into the page's `<script id="seed">` block;
  rerun after regenerating the layout.
- **Save for Claude** downloads `bottom-design-<time>.json` (`kind: gdx-bottom-screen-design`):
  every screen's elements, numbered note pins, and a `changes` list diffed against the seed.
- Edits autosave in the browser; `index.html#dash` opens a screen directly.
