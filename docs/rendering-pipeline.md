# Rendering pipeline — BGI overlay dispatch, VGA double-buffer, blit paths

Faithful documentation of how the engine puts pixels on screen, reconstructed from the
`BumpyDecomp` Ghidra decomp + raw disassembly of the non-decompiling BGI overlay
blitters. Addresses are `segment:offset` in the unpacked image (or `DGROUP:off`, DGROUP
segment `0x203b`). This documents the **original**; the `src/` reconstruction mirrors it
(with deviations recorded in [reconstruction-fidelity.md](reconstruction-fidelity.md)).

## 1. BGI graphics-driver overlay

Graphics output goes through a Borland-BGI-style **overlay** loaded at segment `1ab9`.
Two engine entry thunks dispatch into it by "mode":

| Engine fn | Addr | Calls | Purpose |
|---|---|---|---|
| `restore_bg_view`   | `1000:80bc` | `bgi_set_mode_01` (`1ab9:0d77`) | background / erase blit |
| `render_player_view`| `1000:93b8` | `bgi_set_mode_10` (`1ab9:1028`) | planar region COPY (save-under / read-back / present-style) |

Both take a far pointer to a **view descriptor** in `DX:AX`. Each mode thunk dispatches
through a per-`palette_mode` vector table in DGROUP, but **gated on the descriptor's
lead word**:

- `bgi_set_mode_10` (`1ab9:1028`): if `view->word[0] < 2`, set flags `[0x541f]=1`,
  `[0x5420]=0`, then `call DGROUP[palette_mode*2 + 0x5698]`. Otherwise it is a **no-op**
  (this is how code-embedded / inactive views are skipped).
- `bgi_set_mode_01` (`1ab9:0d77`): keys on `view->word[0x0e]` instead; sets
  `[0x541f]=0`, `[0x5420]=1`, then `call DGROUP[palette_mode*2 + 0x555e]`.

**Dispatch vectors** (near offsets within overlay `1ab9`; values read from a live DGROUP
snapshot). For VGA, `palette_mode = 2`:

| Table | DGROUP base | pm=2 handler |
|---|---|---|
| mode-01 (`restore_bg_view`) | `0x555e` | `1ab9:0aa0` — masked bg-tile blit (the 6a target) |
| mode-10 (`render_player_view`) | `0x5698` | `1ab9:0db0` — planar rectangular copy |

The mode-10 handler additionally **sub-dispatches** on `view->word[0x1c]` through
`DGROUP[idx*2 + 0x568a]`:

| idx | handler | operation |
|---|---|---|
| 0 | `1ab9:0de0` | full planar copy (4-plane loop, GC Read-Map-Select + `rep movsw`) |
| 1,2 | `0f35`,`0ec3` | `ret` (no-op) |
| 3,4,5 | `0ecc`,`0ec4`,`0ed4` | masked copy (per-word AND-mask + OR), differing mask presets |
| 6 | `0e3c` | masked copy variant (all 4 planes per word) |

### The mode-10 planar copy (`1ab9:0db0`)

This overlay routine does not decompile (self-modifying, jump-table — same family as the
`1cec:10e1` sprite blitter and the `1ab9:0aa0` tile blitter); reconstructed from disasm:

- Programs VGA **Graphics Controller** `0x3CE/0x3CF` index 4 (**Read Map Select**) and
  loops a plane counter 0..3, reading each VGA plane in turn.
- Inner loop (`1ab9:0e09`): `rep movsw` copies `[0x5431]/2` words per row for `[0x5433]`
  rows; source row stride `0x28` (=40, the 320-px scanline pitch), dest stride `[0x5429]`.
- Source/dest set up in `1ab9:052d`: **source** = `DGROUP[word[0]*4 + 0x5415]` (the page
  pointer table, see §2) + `(si_y*40 + si_x)`; **dest** = `les view+0x10` (an explicit far
  pointer in the descriptor) + `(di_y*40 + di_x)`. `1cda:0089` paragraph-normalizes the
  far pointer between planes (so an offset that overflows 0xFFFF rolls into the segment).

So mode-10 is a **planar rectangular copy** between a VGA page and a destination buffer —
used for save-unders, read-backs, and buffer presents, **not** for drawing a sprite.

## 2. VGA double-buffer (two pages, a000 / a200)

The engine keeps two VGA pages within the 64 KB plane window:

- **page 0** = `a000:0000` → plane offset `0x0000` (the first 200×40 = `0x1F40` bytes)
- **page 1** = `a200:0000` → plane offset `0x2000`

`sprite_table_base` (`DGROUP:0x5415`, two far pointers): `[0] = a200:0000`, `[1] =
a000:0000`. The **current draw page** is held in `cur_sprite_data_off/seg`
(`DGROUP:0x56e2 / 0x56e4`); `set_sprite_table_ptr` (`1cec:2dd2`) /
`dispatch_palette_mode_with_src_ptr` (`1cec:2d6d`) select it, and the sprite blitter
writes there. Across a frame the engine issues blits to **both** pages (~50/50 observed
over many calls).

### What is *not* present (verified dynamically)

Over a full settled level (873 `bgi_set_mode_10` calls hooked), **every** active call
sourced from page 0 (`a000`); **none** sourced page 1 (`a200`). There is **no
`a200 → a000` memory-copy present**. The `bgi_set_mode_10` calls observed are all
read-backs / save-unders (VGA → system memory), e.g. per-entity background capture into
DGROUP scratch buffers and a level-start full capture into `fullscreen_buf`.

> **Unresolved:** which page is actually *displayed*, and whether the engine page-flips
> via the VGA CRTC Start-Address registers (`3D4` idx `0x0C/0x0D`). The project's Unicorn
> VGA model does not faithfully track the CRTC display start (a single implausible value
> `0xDF00` was seen, never reprogrammed), so the display-page-selection mechanism could
> not be confirmed from emulation. The two pages differ only in the just-drawn sprite
> rows (≈214 bytes), consistent with double-buffering of an in-flight animation frame.

### `fullscreen_buf` (background save-under)

`fullscreen_buf` (`DGROUP:0x7926/0x7928` → `0x67bf:0000`, 32000 B = 4 planes × 8000)
holds a save-under of the clean background. `setup_fullscreen_view` (`1000:483c`) captures
it (copies `a000` → `fullscreen_buf` via mode-10 sub-handler 0, after
`redraw_level_background_tiles`). The erase path (`restore_bg_view` with `[0x541f]=0`)
copies `fullscreen_buf` → `a000` to restore background before redrawing a moved entity.

## 3. Background tiles

`start_level` → `restore_bg_tile_run` (`1000:0a90`) builds the render descriptor
(`_render_descriptor_ptr`): atlas source = `level_pav_buf+6 : level_pav_seg`, dims
`0x14×0x18`; per tile-run column it looks up `tile_id = map[cx*0x27 + (cy>>1)*3 + col +
0x20] - 1`, `atlas_col = tile_id % 0x14`, `atlas_row = (tile_id / 0x14) * 2`, and calls
`restore_bg_view` → the mode-01 masked tile blitter (`1ab9:0aa0`). Run cells
(`run_code >= 0xf1`) overlay `(-run_code & 0xff) - 5` masked sub-tiles. The full-grid loop
lives in `redraw_level_background_tiles` (`1000:2a0a`) / `start_level`.

(Format details: [formats/LEVELS.md](formats/LEVELS.md). Atlas/tile model validated 6a.)

## 4. Sprites

`blit_sprite` (`1000:942a`) → `blit_sprite_vga` (`1cec:31b7`) → `prepare_sprite_frames`
(`1cec:2ded`, anim-frame select) + `dispatch_palette_mode_with_src_ptr` → the planar
masked sprite blitter `FUN_1cec_10e1` (GC/Sequencer map-mask + bit-mask, sub-byte shift,
inter-column carry). The blitter consumes a 0x18-byte descriptor built by
`object_list`/`clip`/`setup` from a sprite object. Sprites write to `cur_sprite_data`
(the current double-buffer page). (Pipeline reconstructed + validated byte-exact in 5a/5b;
codec/blit details in [formats/SPRITES](formats/) and the sprite specs.)

## 5. Level entities

`spawn_and_draw_level_entities` (`1000:2a78`) is the level-load placement orchestrator. It
reads the per-level BUM header (via the `tilemap` pointer it byte-copies the current
level's 0xC2-byte header into) and scans the 6×8 grid across three layers:

- **Layer A** (`+0x00`): remap `cv` via `DGROUP[cv+0x3d3a]` → descriptor
  `DGROUP[0x3d6a + remap*4]` → `{word0=yoff, word1=frame}`; populate channel-A record;
  `draw_anim_channels_a` (`1000:165e`) blits it (pos from `posA` table `DGROUP:0xf4/0xf6`).
- **Layer B** (`+0x30`, col 7 skipped): remap `0x4086` → descriptor `0x40a6`;
  `draw_anim_channels_b` (`1000:17c7`); frame already includes the `+0xf1` bias in the
  descriptor data.
- **Layer C** (`+0x60`): static sprite, `frame = cell + 0x179`, pos from `posC`
  (`DGROUP:0x274`, interleaved XY, 4 B/cell); blits via the shared `p1_sprite` obj.
- **Player 1**: `draw_p1_sprite` (`1000:1cb2`) — obj fields from `p1_pixel_x/y` +
  `p1_move_anim` (hidden when `==100`).
- **Player 2 / enemy**: `draw_p2_sprite` (`1000:1cea`) — present when `p2_cell != -1`
  (`= bum[0x93]-1`); frame = `P2_FRAME_TABLE[bum[0x96]] + p2_move_anim`.

The animation **channels** (`step_anim_channels_a/_b`, tables `0x3d6a`/`0x40a6`) animate a
small moving subset per frame; the bulk of A/B entities are placed statically at load.

**Exit and items are game-state, not separate sprites.** `level_exit_cell` (`bum+0x91`) and
`items_remaining` (`bum+0x92`) are read only by `p1_collect_item` (collision / scoring) and
written by `spawn_and_draw_level_entities` (init) — **no draw routine reads them** (verified by
xref). The visible exit door and collectible items are drawn as ordinary cells of the level
layer grids (background / layer A/B/C), already covered above; the HUD item count is drawn
separately by the HUD routines (`draw_hud_composite`/`draw_icon_row`). (An earlier
reconstruction note claimed "un-ported exit/item sprites" as residue — that was a
misattribution; there is no dedicated exit/item draw to port.)

> **Note on `render_player_view` in the channel draw path:** `draw_anim_channels_a/_b`
> call `restore_bg_view` (erase) and `render_player_view` (mode-10) around each
> `blit_sprite`. Per §1–§2 these are erase/save-under/read-back operations on the
> double-buffer pages, **not** a second visible draw of the entity. The visible entity
> pixels come from `blit_sprite` writing the current page. (Earlier reconstruction notes
> that treated `render_player_view` as a missing visible entity draw, or as
> "out of scope," were mistaken — corrected here.)
