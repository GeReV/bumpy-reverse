# `.BUM` — per-level objects / sprite masks ("Bumpy")

One per level, `D1.BUM`..`D9.BUM`. Shared
[container format](README.md#the-shared-container-vecpavdecbum) + `vec_run`
interpreter. The **smallest** per-level files. Two sub-shapes are visible in the
data:

- **Sparse** (`D1–D5, D7, D8`): one or two opcodes (often just `op12`) followed
  by a long coordinate run — looks like a single masked shape / object sprite or
  placement list.
- **Rich** (`D6, D9`): many opcodes (12–24 distinct) — more elaborate per-level
  object sets. Note `D6.BUM`/`D9.BUM` also carry unusual header words
  (`decoded_size`/checksums near 0), so they may use a header variant.

Header is the standard 8-byte big-endian block; body is the big-endian
opcode/coordinate stream ([VEC.md](VEC.md)).

## Files (from `tools/extract/container.py`)

| File | Bytes | `decoded_size` | distinct opcodes | coord words |
|------|------:|---------------:|-----------------:|------------:|
| D1.BUM | 686 | 0x0369 | 4 | 334 |
| D2.BUM | 1120 | 0x0b60 | 1 | 551 |
| D3.BUM | 1049 | 0x0b60 | 2 | 517 |
| D4.BUM | 975 | 0x091a | 1 | 481 |
| D5.BUM | 1083 | 0x091a | 3 | 530 |
| D6.BUM | 2912 | 0x0009 | 15 | 620 |
| D7.BUM | 1415 | 0x0b60 | 2 | 695 |
| D8.BUM | 1322 | 0x0b60 | 4 | 650 |
| D9.BUM | 2330 | 0x001e | 13 | 532 |

## Status

Container parses for all 9 levels. Whether `.BUM` encodes object/enemy placement
vs. sprite-mask geometry (and the header variant in D6/D9) is open — task #8,
helped by the `MASKBUMP.VEC` mask format and the loader's resource indexing.
