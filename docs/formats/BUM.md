# `.BUM` — per-level objects / sprite masks

One per level: `D1.BUM`–`D9.BUM`. Uses the shared
[container format](README.md#the-shared-container-vecpavdecbum) and the same
`vec_run` interpreter as `.VEC`. These are the **smallest** per-level files. The
confirmed decode (`tools/extract/op12_port.py`, byte-exact against the emulator
oracle) is an `op4`/`op12` `vec_run` stream into the **per-level map-header
table** (2 + 15×`0xc2` bytes) — see
[LEVELS.md](LEVELS.md#per-level-header-0xc2-bytes) for the decoded layout.

The 8-byte big-endian header is standard; see [VEC.md](VEC.md) for the record
layout and interpreter.

Decoded by `tools/extract/vec_records.py` (raw-byte record walker, pre-decompress)
and `tools/extract/container.py` (header + opcode histogram); rendered by
`tools/extract/render_levels.py`.

## Files

**Caveat:** as with `.PAV`/`.DEC`, the `Distinct opcodes`/`Coord words` columns
below come from `vec_records.py` scanning the still-RLE-compressed file bytes
directly, before `op4` decompression — most of the extra opcode hits are
checksum-collision scanner noise, not real dispatch. The real,
oracle-validated decode dispatches only `op4`/`op12` for every `.BUM` file (see
[VEC.md](VEC.md#opcode-coverage-what-the-python-code-actually-dispatches)).

| File | Bytes | `decoded_size` | Distinct opcodes (raw-byte scan, likely noise) | Coord words |
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

## Sub-shape variants

Two structural variants are visible in the raw-byte scan (see caveat above —
this describes scanner output, not necessarily real dispatched content):

- **Sparse** (`D1`–`D5`, `D7`, `D8`): one or two opcodes (often just `op12`)
  followed by a long coordinate run — a single masked shape, object sprite, or
  placement list. `decoded_size` is a normal non-zero value (e.g. `0x0b60`).
- **Rich** (`D6`, `D9`): many opcodes (12–24 distinct) — more elaborate per-level
  object sets.

## D6/D9 header variant

`D6.BUM` and `D9.BUM` carry non-zero magic words and `decoded_size` values near
zero (`0x0009` and `0x001e` respectively). This is a header variant: despite the
near-zero `decoded_size`, the interpreter still processes the body stream. The
record walker handles these files with a special case for the zeroed-header form.
