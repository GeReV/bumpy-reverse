# Bumpy puzzle-level format (`D<n>.PAV` / `.DEC` / `.BUM`)

Reverse-engineered from `BumpyDecomp` (Ghidra) + validated byte-exact against the
emulator. Renderer: `tools/extract/render_levels.py` (pure Python, no emulator).

## Worlds vs. levels

- A **world** = the file set `D<n>.{PAV,DEC,BUM}`, `n = 1..9`. `current_level`
  (1..9) selects the world; the game patches the digit into the filename.
- Each world holds up to **15 puzzle levels**, selected by `current_level_index`.
  Real puzzle count per world: **15** in worlds 1, 2, 3, 6, 7, 8; **12** in worlds 4, 5, 9
  → **126 total**. The count is the decoded **payload length** `(len-2)//0xc2`, where the
  payload is the last decompressed record's `vec_src` (or, for the uncompressed `.BUM`s —
  D6/D9, whose first 12 bytes are a terminator so vec_run runs nothing — the file size).
  Worlds 4/5 only fill 12 of the table's 15 slots; the bytes past that are `.DEC` window
  leftover (the in-place op12 finalize copies the shared window into the buffer), **not**
  real layouts — `load_bum()` returns this count so the renderer/exporter skip them.
- Each world also has a `MONDE<n>.VEC` overworld map (see `VEC.md`).

## The three files (all op4/op12 `vec_run` streams)

Each file is a `vec_run` record stream; the pure-Python engine
(`op12_port.Op12.vec_run`) decompresses it into a fixed-size buffer:

| file | decompressed size | holds |
|---|---:|---|
| `D<n>.PAV` | `0x7806` (30726 = 320×192 4-plane) | world **brush/tile atlas** (20×12 tiles of 16×16) |
| `D<n>.BUM` | `0xb60` (2912 = 2 + 15×0xc2) | **per-level map-header table** (the layouts) |
| `D<n>.DEC` | `0x2f96` (12182 ≈ 15×0x32c) | per-level table; `+0x20` = 20×13 **background tile grid** |

`alloc_level_buffers` proves the mapping: `DAT_203b_6bf2 = level_bum_buf + 2`
(tilemap source) and `DAT_203b_6bd2 = level_dec_buf + 2`.

> `.PAV`, `.DEC`, and `.BUM` all decode **byte-exact** pure-Python (`op12_port`,
> validated against the `vec_cpu` 8086 interpreter running the real decode code).
> `.DEC` previously hung `vec_run`; fixed by adding the opcode-validity (`w4 & 0x7f00`)
> + XOR-checksum stream terminators its end record relies on (opcode `0x750`).
>
> **The `.BUM` op12 decoder is fixed (no emulator capture needed).** Two bugs were
> corrected in `op12_port`:
> 1. **`crd` over-rounding** (`phase1` 0x503): the record's `crd = round_up(vec_src, 0x20) >> 3`
>    must skip the `+0x20` when `vec_src` is *already* a multiple of `0x20` (the real
>    code's `0x520 je 0x52f`). The port always added `0x20`, shifting `crd`/`dst2`/`src`
>    by 4 on aligned records (e.g. the 0xb60 layout record) and desyncing the rest of the
>    stream. `.PAV`/`.DEC` never hit an aligned `vec_src`, so only `.BUM` was affected.
> 2. **missing op4 sliding window**: op4 leaves the decompressed payload in the 0x400-byte
>    window at `DG:0x4e97` (end-aligned); a later op12 record's finalize copies that window
>    into the layout table's wrap region. The port now reproduces it.
>
> Some worlds' layout tables overrun the window boundary, so the `.BUM` must be decoded in
> the game's **PAV → DEC → BUM** sequence sharing the window (`render_levels.load_bum()`).
> Result: byte-exact vs the emulator for all 9 worlds, and it **recovers world 3's last
> levels**, which the emulator capture (`build/render/bum/world<n>.bum`, now obsolete) had
> left corrupted (spawn = `0xff`). Ground truth also confirmed by the `blit_sprite` trace
> (every platform = frame 64 = code 1).

## A level = one `0xc2`-byte header in the `.BUM` table

`bum[2 + idx*0xc2 : +0xc2]`. Layout (`load_current_level_data` /
`spawn_and_draw_level_entities`):

```
+0x00  layer A : 6 rows × 8 cols  — anim-channel-A entities
+0x30  layer B : 6 rows × 8 cols  — anim-channel-B entities  (col 7 skipped)
+0x60  layer C : 6 rows × 8 cols  — static sprites
+0x90  byte  p1 (Bumpy) spawn cell      (decremented by 1 if nonzero)
+0x91  byte  level-exit cell            (decremented by 1 if nonzero)
+0x92  byte  items_remaining
+0x93  byte  player-2 spawn cell
+0x94  byte  player-2 move state
+0x95  byte  player-2 AI rng threshold
+0x96  byte  player-2 frame base (→ table @0x2546)
```

Cell index = `row*8 + col` (0..47).

### Layer C — static sprites

`BUMSPJEU` frame index = **`cell_value + 0x179`**; drawn at the cell's pixel
position. Cell→pixel (table `@DGROUP 0x274`): `x = 8 + col*40`, `y = 8 + row*32`.

### Layers A/B — entities (platforms + hazards)

Per nonzero cell, the entity code is remapped (A: `[code+0x3d3a]`, B: `[code+0x4086]`)
×4 into a far-pointer descriptor table (A: `@0x3d6a`, B: `@0x40a6`). The descriptor
gives `word0` (y-offset) and `word1` (frame: A = `word1`, B = `word1+0xf1`). Blit
position per cell from `@0xf4/0xf6` (A: `x=col*40, y=24+row*32`) and `@0x3f4/0x3f6`
(B: `x=32+col*40, y=row*32`). Remap byte 0 → null descriptor → draw nothing. The
extracted maps live in `results/levels/anim_tables.json`.

### Background — `.PAV` atlas tiles placed by `.DEC`

`setup_fullscreen_view → redraw_level_background_tiles → restore_bg_tile_run`:
the `.PAV` is a **20-column grid of 16×16 tiles**. The per-level `.DEC` table holds
a 20×13 background grid at `+0x20` (`dec[2 + lvl*0x32c + cx*0x27 + (cy>>1)*3 + 0x20]`,
cy = 0,2,…,0x18). The view is **cleared to palette index 0 first** (the base backdrop
colour, e.g. world 3's dark purple `(32,0,32)`), so cells with `code == 0` (skipped by
`redraw_level_background_tiles`, which only draws `code != 0`) show that base rather than
black — pre-fill the canvas with `pal[0]`. Each cell occupies **3 bytes** (13 rows × 3 =
0x27 per column). A normal `code` (`<0xf1`) → `tile_id = code-1` → atlas tile
`(id%20, id//20)`, drawn at `(cx*16, cy*8)`. A `code ≥ 0xf1` is a **run** cell: `restore_bg_tile_run` blits
`col_count = (byte)(-code-5)` sub-tiles read from `dec[slot+1 .. slot+col_count-1]`, **all
at the same `(cx,cy)`** (`descriptor[0x14]=cell_x` every iteration). The blit is **masked
(palette index 0 = transparent)**, so the first sub-tile is an opaque base (e.g. a
carousel pole + its backdrop) and the rest overlay through it — this is what lets world 2's
merry-go-round poles show *through* the animal tiles. Real data only ever uses `0xf8`
(= 2 sub-tiles: opaque base + masked overlay). Two earlier mistakes: spreading run
sub-tiles horizontally (→ black gaps), and drawing the overlay opaquely (→ poles vanish
behind animals). **The atlas raster starts at the `.PAV`'s 6-byte header**
(`restore_bg_tile_run` sources `level_pav_buf+6`) — render the `.PAV` planar at
**offset 6**, not 0, or every tile is misaligned.

### Player (Bumpy)

Sprite = `BUMSPJEU` **frame 0** (orange ball; frames 0–11 are his animation).
Drawn at the **spawn cell** (header `+0x90`, 1-based → cell `h[0x90]-1`) via the `posC`
table — for D1 level 0 that's cell 40 = bottom-left, matching the oracle. (At runtime
`draw_p1_sprite` blits `p1_move_anim` at `p1_pixel_x/y`; in-level Bumpy idles at frame 0.)

### Enemy (the P2 AI opponent)

Present only when header `+0x93 != 0` (64 of the 126 levels). Its starting cell is
`h[0x93]-1`, and its sprite frame is `P2_FRAME_TABLE[h[0x96]]` — a 18-word table at DGROUP
`0x2546` (`[4427,346,350,354,358,362,366,370,374,475,479,483,487,491,495,499,503,507]`).
`p2_set_pixel_from_cell` places it at `posC[cell] + (7,7)`; `draw_p2_sprite` blits frame
`p2_frame_base + p2_move_anim` (anim = 0 at rest). E.g. world 6 level 3 = the green spiky
creature at cell 1 (frame 491); world 2 level 1 = the red car at cell 10 (frame 495).

### Per-world palettes

Each world has its own 16-colour 6-bit gameplay palette in `build/render/bum/world<n>.pal.json`.
World 6's emulator capture was wrong (blue-ish indices 0–5,13); it was corrected by
extracting the true colours from `results/oracle/world6_level2.png` (red brick backdrop,
green platforms) — index 4 = green, index 0 = `(8,0,0)` dark-red base.

## Tools

```
tools/extract/export_levels.py              # 126 levels -> results/levels/world<n>_lvl<NN>.json + index.json
tools/extract/render_levels.py [world...] [--montage]
                                            # render all puzzles -> results/levels_png/
                                            #   world<n>_lvl<NN>.png + world<n>_montage.png
```

`render_levels.load_bum()` decodes `.BUM` purely in Python (PAV→DEC→BUM chain); the old
`tools/render/capture_bums.sh` emulator capture is no longer needed.

Palette: **each world has its own 16-colour gameplay palette** (world 1 blue, 2 red,
3 purple, 6 red, …), captured per world to `build/render/bum/world<n>.pal.json` and loaded
by `render_levels.world_palette()` (world 6's was oracle-corrected — see above). Ground
truth = the DOSBox captures in `results/oracle/world<n>_level<N>.png`. (The old per-world
emulator grabs `render_levels_emu.sh` produced — `world<n>_oracle.png` — were redundant and
unreliable, e.g. the wrong screen/palette for world 6, and have been removed.)

**Render status:** complete and **fully pure-Python** — `.PAV`/`.DEC`/`.BUM` all decode
byte-exact with no emulator. Renders the `.DEC` tile background + layers A/B/C
(platforms/objects) + Bumpy at his spawn cell + the AI enemy when present. Matches the
DOSBox oracles; the only remaining differences are the **randomized reward objects**
(layer C). (Per-world gameplay palettes still come from an emulator DAC capture —
`build/render/bum/world<n>.pal.json`.)
