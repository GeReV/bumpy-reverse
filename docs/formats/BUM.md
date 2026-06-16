# `.BUM` — per-level objects / sprite masks

One per level: `D1.BUM`–`D9.BUM`. Uses the shared
[container format](README.md#the-shared-container-vecpavdecbum) and the same
`vec_run` interpreter as `.VEC`. These are the **smallest** per-level files.

The 8-byte big-endian header is standard; the body is the big-endian
opcode/coordinate token stream — see [VEC.md](VEC.md) for the record layout and
interpreter.

Decoded by `tools/extract/vec_records.py` (record walker) and
`tools/extract/container.py` (header + opcode histogram).

## Files

| File | Bytes | `decoded_size` | Distinct opcodes | Coord words |
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

Two structural variants are visible in the data:

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
