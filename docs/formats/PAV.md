# `.PAV` — per-level playfield ("pavé" = paving/tiles)

One per level: `D1.PAV`–`D9.PAV`. Uses the shared
[container format](README.md#the-shared-container-vecpavdecbum) and the same
`vec_run` interpreter as `.VEC` — the level playfield is stored as a **vector
command stream**, not a tilemap array. These are the largest per-level files and
the most opcode-rich, consistent with the main level geometry.

The 8-byte big-endian header is standard (`w0=0`, `w1=decoded_size`,
`w2`/`w3`=checksums). The body is a big-endian opcode/coordinate token stream;
see [VEC.md](VEC.md) for the record layout and interpreter.

Decoded by `tools/extract/vec_records.py` (record walker) and
`tools/extract/container.py` (header + opcode histogram).

## Files

| File | Bytes | `decoded_size` | Distinct opcodes | Coord words |
|------|------:|---------------:|-----------------:|------------:|
| D1.PAV | 15071 | 0x3e7f | 15 | 7205 |
| D2.PAV | 19937 | 0x56fc | 15 | 9572 |
| D3.PAV | 16196 | 0x4426 | 15 | 7512 |
| D4.PAV | 20631 | 0x546e | 15 | 9711 |
| D5.PAV | 24072 | 0x6156 | 13 | 11365 |
| D6.PAV | 19132 | 0x50f8 | 15 | 8468 |
| D7.PAV | 16526 | 0x460f | 15 | 7890 |
| D8.PAV | 12186 | 0x3245 | 12 | 5733 |
| D9.PAV | 25475 | 0x672e | 14 | 12048 |

Opcode 1 dominates several levels (e.g. D6 op1×620, D3 op1×252), consistent
with the primary line/segment draw for the playfield outline.
