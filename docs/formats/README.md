# Bumpy's Arcade Fantasy — data file formats

Reverse-engineered from the unpacked `BUMPY.EXE` (see `../02`–`../05`) and from
the real asset files under `originals/old-games/bumpy/`. Extraction scripts live
in `tools/extract/`; their output goes to `build/extract/`.

Loriciel title → French names: `monde`=world, `pavé`=tile/paving, `décor`=scenery,
`fleche`=arrow, `caractères`=characters/font.

## File-type map

| Ext | Per-level | Kind | Endian | Doc |
|-----|-----------|------|--------|-----|
| `.VEC` | no (shared) | Vector-graphics command stream | **big** | [VEC.md](VEC.md) |
| `.PAV` | yes (`D1..D9`) | Level playfield/tiles (vector stream) | **big** | [PAV.md](PAV.md) |
| `.DEC` | yes (`D1..D9`) | Level décor/background (vector stream) | **big** | [DEC.md](DEC.md) |
| `.BUM` | yes (`D1..D9`) | Level objects/masks (vector stream) | **big** | [BUM.md](BUM.md) |
| `.BIN` | no | Sprite/image bank (offset directory) | **big** | [BIN.md](BIN.md) |
| `.CAR` | no | Bitmap font ("caractères") | **big** | [CAR.md](CAR.md) |
| `.BNK` | no | **Standard AdLib OPL2 instrument bank** | little | [BNK.md](BNK.md) |
| `.MID` | no | **Standard MIDI** (format 1, 7 trk, 192 tpqn) | — | use any SMF tool |

`.VEC/.PAV/.DEC/.BUM` are the **same container** (below), all consumed by the
same interpreter (`vec_run`, overlay segment `1c28`). `.BNK`/`.MID` are standard
third-party formats — use existing tooling (adplug/AdPlug, timidity/fluidsynth),
do not re-implement.

## The shared container (VEC/PAV/DEC/BUM)

8-byte big-endian header, then a body of big-endian 16-bit words that the
interpreter reads as a command stream.

```mermaid
packet-beta
title VEC/PAV/DEC/BUM header (8 bytes, big-endian)
0-15: "w0 magic = 0x0000"
16-31: "w1 decoded_size"
32-47: "w2 checksum A"
48-63: "w3 checksum B"
```

| Off | Size | Field | Meaning |
|----:|-----:|-------|---------|
| 0 | 2 | `w0` magic | always `0x0000` |
| 2 | 2 | `w1` decoded_size | output/render-buffer size hint in bytes (≈32000 for a full 320×200 16-colour screen; `0` in `SCORE.VEC`) |
| 4 | 2 | `w2` checksum A | validation word (checked by `vec_read_record`) |
| 6 | 2 | `w3` checksum B | validation word |

The header is actually the first **record**: the body is a short sequence of
records, each a 12-byte big-endian header (`w0..w3` params, `w4` opcode+flag,
`w5` = `w0^w1^w2^w3^w4` XOR checksum) followed by a variable inline data blob.
Record 0 is always `op4` with `w0=0`, `w1=decoded_size`. The per-record checksum
makes the whole stream **walkable in pure Python** (no emulation):
`tools/extract/vec_records.py` covers 98.5–100 % of every file. See
[VEC.md](VEC.md) for the record layout and `vec_run` interpreter.
