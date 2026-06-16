# `.CAR` — bitmap font ("caractères")

`DDFNT2.CAR` is a proportional bitmap font. It begins with a 4-byte header followed
by a **big-endian `uint16`** glyph-offset table; each glyph's blob spans
`[offset[i], offset[i+1])`.

```mermaid
packet-beta
title .CAR header + glyph offset table
0-7: "first_char (0x20)"
8-15: "last (0xFF)"
16-23: "dim_a (7)"
24-31: "dim_b (8)"
32-47: "offset[0] (BE16)"
48-63: "offset[1] (BE16)"
64-79: "offset[2] (BE16) ..."
```

| Field | Value (`DDFNT2.CAR`) | Meaning |
|---|---|---|
| `first_char` | `0x20` (space) | code of the first represented character |
| `last` | `0xFF` | last code / count marker |
| `dim_a × dim_b` | `7 × 8` | nominal cell size (cols × rows); actual glyphs are proportional |

The offset table runs from byte `4` up to the value of `offset[0]` (the table
terminates when the read position reaches the first data blob). `DDFNT2.CAR` has
**126 table entries**.

**`table[0]` is a phantom entry** (a 198-byte blob, not a renderable glyph). Real
glyphs are therefore shifted by one: character code `C` maps to table index
`C − first_char + 1`.

## Per-glyph blob layout

| Bytes | Field | Notes |
|---|---|---|
| `0` | `width` | pixel columns used (≤ 8) |
| `1` | `height` | pixel rows |
| `2` | pad | always `0x00` |
| `3 .. 3+height−1` | bitmap rows | one byte per row; 8 px wide, **MSB = leftmost pixel** |
| last 2 | trailer | advance/kerning data; not required for rendering |

Blob size = `3 + height + 2`. Glyphs vary in both width and height (fully
proportional).

## Decoded by

- `tools/extract/car_sheet.py [file.CAR] [out.png]` — renders a PNG font sheet for
  characters `0x20..0x7E`.
- `tools/extract/carfont.py DDFNT2.CAR` — extracts raw per-glyph blobs.
