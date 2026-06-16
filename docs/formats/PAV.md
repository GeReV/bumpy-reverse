# `.PAV` — per-level playfield ("pavé" = paving/tiles)

One per level, `D1.PAV`..`D9.PAV`. Uses the shared
[container format](README.md#the-shared-container-vecpavdecbum) and the same
`vec_run` interpreter as `.VEC` — i.e. the level's playfield is stored as a
**vector command stream**, not a tilemap array. These are the largest per-level
files and the most opcode-rich, consistent with the main level geometry.

Header is the standard 8-byte big-endian block (`w0=0`, `w1=decoded_size`,
`w2/w3=checksums`). Body = big-endian opcode/coordinate token stream (see
[VEC.md](VEC.md)).

## Files (from `tools/extract/container.py`)

| File | Bytes | `decoded_size` | distinct opcodes | coord words |
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

Opcode 1 dominates several levels (e.g. D6 `op1×620`, D3 `op1×252`) — likely the
primary line/segment draw for the playfield outline.

## Status

Container parses cleanly for all 9 levels. Exact opcode semantics shared with
[VEC.md](VEC.md) (task #7). Confirming the playfield→collision-geometry mapping
(and which `open_resource` index loads `.PAV`) is part of task #8.
