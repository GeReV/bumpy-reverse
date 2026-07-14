# Extraction & rendering tools (`tools/`)

Pure-stdlib Python tools that read the game's original data files (and, for a few,
the unpacked executable directly) with no emulator involved. All are run with
`uv run python tools/...`. Most write output under `local/build/extract/` or
`results/` (both gitignored). Where a script has a real CLI it's noted below;
otherwise it takes no arguments and operates on a hard-coded default file set.

## Container / record level (`.VEC`/`.PAV`/`.DEC`/`.BUM` shared format)

These walk the 12-byte-record container shared by all four extensions — see
[formats/README.md](formats/README.md#the-shared-container-vecpavdecbum) and
[formats/VEC.md](formats/VEC.md).

| Script | What it does |
|--------|---------------|
| `vec_records.py` | Pure-Python record walker: recovers record boundaries and the opcode→inline-length map by validating each record's XOR checksum, with no emulator or prior format knowledge. `vec_records.py <file> [maxrecords]`. |
| `pav_walk.py` | Lighter walker that reports each record's real dispatch opcode (`w4`), the way `vec_run` sees it. `pav_walk.py <file>`. |
| `container.py` | Parses the shared header + records into a structural (not fully decoded) per-file report. `container.py <file.{VEC,PAV,DEC,BUM}> ...` → `local/build/extract/container/<name>.txt`. |
| `op12_port.py` | Not a CLI tool — the production op12 (masked-blit) renderer, a faithful pure-Python transliteration of the in-game state machine. Imported by `render_levels.py` / `vec_to_png.py`; validated byte-for-byte against the emulator oracle. |

## Full-screen `.VEC` images (title, world maps, score, etc.)

| Script | What it does |
|--------|---------------|
| `vec_render.py` | Pure-Python RLE (op4) decoder + planar→PNG renderer for simple `.VEC` bodies. |
| `vec_to_png.py` | Standalone zero-emulator decoder for full-screen `.VEC` images: op4 → `vec_run` record loop → op12 → planar render using the image's own embedded palette. `vec_to_png.py <file.VEC> [out.png]`. See [formats/VEC.md](formats/VEC.md). |
| `render_vec_images.py` | Batch driver over `vec_to_png.py` for the default set of full-screen images (`MONDE1..9.VEC` world maps, `TITRE`, `DESSFIN`, `SCORE`, ...) → `results/levels_png/` and `results/images/`. No arguments. |

## Per-level data (`.PAV`/`.DEC`/`.BUM`)

See [formats/LEVELS.md](formats/LEVELS.md) for the decoded-buffer layouts.

| Script | What it does |
|--------|---------------|
| `extract_levels.py` | Decompresses every `D<n>.PAV/.DEC/.BUM` (single op4 record each) to its raw level buffer and verifies decoded size against the engine's documented buffer sizes. No arguments. |
| `render_levels.py` | Renders full puzzle levels straight from the game files: `.PAV` background (op4/op12) + `.BUM` per-level map-header table (layers A/B/C, spawn/exit/items). `render_levels.py [--montage]`. |
| `export_levels.py` | Exports every level's decoded `.BUM` header table as clean JSON (layer grids, spawn/exit/items/P2 data) via `render_levels.load_bum()`. No arguments → `results/levels/world<n>_lvl<NN>.json` + an index. |

## Sprite / image banks (`.BIN`)

See [formats/BIN.md](formats/BIN.md).

| Script | What it does |
|--------|---------------|
| `binbank.py` | Generic `.BIN` directory carver (BE32 offset table → per-entry blobs); works on both `FLECHE.BIN` and `BUMSPJEU.BIN`. `binbank.py <file.BIN> ...` → `local/build/extract/bin/<name>/NNNN.bin`. |
| `sprite_container.py` | Maps the `BUMSPJEU.BIN` container specifically: frame-offset table + per-frame 12-byte header (dims, ctrl byte) — no pixel decode. |
| `sprite_sheet.py` | Decodes every `BUMSPJEU.BIN` raw-encoded frame to a single PNG sprite sheet, coloured with `MONDE1.VEC`'s palette. No arguments. |
| `render_fleche.py` | Decodes `FLECHE.BIN` (the world-map cursor arrow, a single frame) to PNG. No arguments → `results/sprites/fleche_arrow.png` (+ an 8x copy). |

## Font (`.CAR`)

See [formats/CAR.md](formats/CAR.md).

| Script | What it does |
|--------|---------------|
| `carfont.py` | Carves per-glyph byte ranges from a `.CAR` font via its BE16 offset table (bitmap layout not decoded — for inspection). `carfont.py <file.CAR> ...` → `local/build/extract/car/<name>/glyph_NNN.bin`. |
| `car_sheet.py` | Fully decodes `DDFNT2.CAR` glyph bitmaps and renders a PNG glyph sheet. `car_sheet.py [file.CAR] [out.png]`. |

## Audio bank (`.BNK`)

See [formats/BNK.md](formats/BNK.md).

| Script | What it does |
|--------|---------------|
| `bnkbank.py` | Parses `BUMPY.BNK` per the **published** (non-Loriciel-custom) AdLib `.BNK` header + name index, dumping the raw 30-byte OPL2 instrument records. |

## Data pulled directly from the executable

These read `local/build/unpack/BUMPY_unpacked.exe` (post `tools/tinyprog_unpack.py`,
see [tinyprog.md](tinyprog.md)) instead of a game data file, to recover static
DGROUP tables the engine keeps as compiled-in data rather than a loadable resource.

| Script | What it does |
|--------|---------------|
| `dump_anim_tables.py` | Regenerates `tools/extract/anim_tables.json` — the layer-A/B in-level entity descriptor tables — from DGROUP, anchored on the known `posC` per-cell pixel grid. Feeds `render_levels.py` / `export_levels.py`. `dump_anim_tables.py [exe_path]`. |
| `gen_anim_data.py` | Generates `src/anim_data.c`, the reconstructed C source for the same entity-placement tables, ground-truthed against the Ghidra decomp + asm (`spawn_and_draw_level_entities`, `draw_anim_channels_a/b`). No arguments. |
| `ega_palette_patch.py` | Extracts the EGA Attribute-Controller palette-patch tables (the 16-byte per-screen palettes copied over `img+0x23` when `palette_mode==1`) from the static executable image. No arguments. |

## Executable unpacking & disassembly (`tools/`, not `tools/extract/`)

| Script | What it does |
|--------|---------------|
| `tinyprog_unpack.py` | Recovers the unpacked MZ executable from the TinyProg-packed `BUMPY.EXE` (CRC-keyed anti-tamper + LZSS), by emulating both packing layers. See [tinyprog.md](tinyprog.md). |
| `disasm16.py` | Small 16-bit x86 disassembly utility used ad hoc during reverse-engineering. |
