# Bumpy puzzle-level format (`D<n>.PAV` / `.DEC` / `.BUM`)

Decoded and rendered by `tools/extract/render_levels.py`; exported by
`tools/extract/export_levels.py`. All three files are `op4`/`op12` `vec_run` streams —
see `VEC.md` for the container format and decode mechanics.

## Worlds vs. levels

- A **world** = the file set `D<n>.{PAV,DEC,BUM}`, `n = 1..9`. The game patches the
  digit into the filename to select a world.
- Each world holds up to **15 puzzle levels**. Puzzle count per world: **15** in worlds
  1, 2, 3, 6, 7, 8; **12** in worlds 4, 5, 9 → **126 total**. The count is derived from
  the decompressed `.BUM` payload length: `(len-2) // 0xc2`.
- Each world also has a `MONDE<n>.VEC` overworld map (see `VEC.md`).

## The three files

Each file decodes into a fixed-size buffer via the `vec_run` stream engine. The `.BUM`
must be decoded in **PAV → DEC → BUM** sequence, sharing the sliding window, because some
worlds' layout tables depend on the window state left by the earlier decodes.

| File | Decompressed size | Contents |
|---|---:|---|
| `D<n>.PAV` | `0x7806` (30726 bytes = 320×192, 4-plane) | World **brush/tile atlas** — 20×12 grid of 16×16 tiles |
| `D<n>.BUM` | `0xb60` (2912 bytes = 2 + 15×0xc2) | **Per-level map-header table** |
| `D<n>.DEC` | `0x2f96` (12182 bytes ≈ 15×0x32c) | Per-level table; `+0x20` = 20×13 **background tile grid** |

The level-load routine maps the decoded buffers as follows: the `.BUM` payload begins at
buffer offset `+2` (the tilemap source), and the `.DEC` payload begins at buffer offset
`+2` as well.

## Per-level header (0xc2 bytes)

A level's data is `bum[2 + idx*0xc2 : 2 + (idx+1)*0xc2]`. Layout:

```
+0x00  layer A : 6 rows × 8 cols  — anim-channel-A entities
+0x30  layer B : 6 rows × 8 cols  — anim-channel-B entities  (col 7 skipped)
+0x60  layer C : 6 rows × 8 cols  — static sprites
+0x90  byte  player-1 (Bumpy) spawn cell    (1-based; 0 = absent)
+0x91  byte  level-exit cell                (1-based; 0 = absent)
+0x92  byte  items_remaining
+0x93  byte  player-2 spawn cell            (1-based; 0 = no enemy)
+0x94  byte  player-2 move state
+0x95  byte  player-2 AI rng threshold
+0x96  byte  player-2 frame-table index
```

Cell index = `row*8 + col`, range 0..47. Spawn values at `+0x90` and `+0x91` are
decremented by 1 when nonzero to obtain the zero-based cell index.

### Layer C — static sprites

Each nonzero cell value maps directly to a `BUMSPJEU` frame index:

```
frame = cell_value + 0x179
```

Cell-to-pixel position (from the position table at DGROUP `0x274`):

```
x = 8 + col * 40
y = 8 + row * 32
```

### Layers A/B — entities (platforms and hazards)

For each nonzero cell, the entity code is remapped through a per-channel remap array
into a descriptor-table index (×4 = far pointer):

- **Layer A**: remap array at `+0x3d3a`; descriptor table at `+0x3d6a`. Frame = `word1`.
  Blit position: `x = col*40`, `y = 24 + row*32`.
- **Layer B**: remap array at `+0x4086`; descriptor table at `+0x40a6`. Frame = `word1 + 0xf1`.
  Blit position: `x = 32 + col*40`, `y = row*32`.

Each descriptor also provides `word0` (y blit offset). Remap byte 0 maps to the null
descriptor and draws nothing. The full remap and descriptor tables are available as
`tools/extract/anim_tables.json`.

### Background — `.PAV` atlas tiles placed by `.DEC`

The `.PAV` atlas is a **20-column grid of 16×16 tiles**. The atlas raster starts at
byte offset 6 (after the 6-byte file header); tile reads must skip this header.

The `.DEC` buffer holds one background grid per level at:

```
dec[2 + lvl*0x32c + cx*0x27 + (cy>>1)*3 + 0x20]
```

where `cx` = column (0..19), `cy` = row × 2 (0, 2, 4, …, 0x18). The grid is 20
columns × 13 rows; each cell occupies **3 bytes** (13 rows × 3 = 0x27 bytes per column).

The view is cleared to palette index 0 before drawing (the base backdrop colour for
the world), so cells with code 0 — which the background-redraw routine skips — show
the base colour rather than black. Pre-fill the canvas with `pal[0]`.

**Normal cell** (`code < 0xf1`):

```
tile_id = code - 1
atlas_col = tile_id % 20
atlas_row = tile_id // 20
draw at (cx*16, cy*8)
```

**Run cell** (`code >= 0xf1`): the background-redraw routine blits
`col_count = (byte)(-code - 5)` sub-tiles read from `dec[slot+1 .. slot+col_count-1]`,
**all at the same `(cx, cy)` position**. The blit is **masked** (palette index 0 =
transparent): the first sub-tile is an opaque base and subsequent sub-tiles overlay
through it. Real data uses only `0xf8` (2 sub-tiles: opaque base + masked overlay),
which is what allows decorative elements such as carousel poles to show through
foreground tiles in world 2.

### Player (Bumpy)

Bumpy is `BUMSPJEU` **frame 0** (the orange ball; frames 0–11 are his animation cycle).
He is placed at the spawn cell given by `header[+0x90] - 1` (zero-based), using the
`posC` pixel-position table.

### Enemy (the P2 AI opponent)

The P2 opponent is present when `header[+0x93] != 0`; this is the case in 64 of the
126 levels. Its starting cell is `header[+0x93] - 1`. Its sprite frame is:

```
frame = P2_FRAME_TABLE[header[+0x96]]
```

`P2_FRAME_TABLE` is an 18-word table at DGROUP `0x2546`:

```
[4427, 346, 350, 354, 358, 362, 366, 370, 374,
  475, 479, 483, 487, 491, 495, 499, 503, 507]
```

The level-entity routine places P2 at `posC[cell] + (7, 7)`. At rest, P2's animation
index is 0, so the displayed frame is `P2_FRAME_TABLE[header[+0x96]] + 0`.

### Per-world palettes

Each world has its own 16-colour 6-bit gameplay palette. The background-redraw and
sprite-blit routines use this palette for all rendering within the world.

## Tools

```
tools/extract/render_levels.py [world...] [--montage]   # render puzzle screenshots
tools/extract/export_levels.py                          # export 126 levels to JSON
tools/extract/anim_tables.json                          # entity remap + descriptor tables
```
