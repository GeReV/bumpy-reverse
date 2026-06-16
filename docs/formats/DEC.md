# `.DEC` — per-level décor / background ("décor" = scenery)

One per level: `D1.DEC`–`D9.DEC`. Uses the shared
[container format](README.md#the-shared-container-vecpavdecbum) and the same
`vec_run` interpreter as `.VEC`. These are medium-sized per-level files with
**fewer, sparser opcodes** than `.PAV`, consistent with background/scenery art
drawn behind the playfield.

The 8-byte big-endian header is standard; the body is the big-endian
opcode/coordinate token stream — see [VEC.md](VEC.md) for the record layout and
interpreter.

`.DEC` streams terminate via the opcode-validity / checksum terminator: the
terminal record carries opcode `0x750`, which fails the `w4 & 0x7f00 != 0`
validity check and signals end-of-stream (unlike `.PAV`/`.BUM` which end on the
`w0 > 0x0f` check).

Decoded by `tools/extract/vec_records.py` (record walker) and
`tools/extract/container.py` (header + opcode histogram).

## Files

| File | Bytes | `decoded_size` | Distinct opcodes | Coord words |
|------|------:|---------------:|-----------------:|------------:|
| D1.DEC | 6436 | 0x19ea | 8 | 3159 |
| D2.DEC | 8817 | 0x2f96 | 5 | 4355 |
| D3.DEC | 6219 | 0x27aa | 3 | 3099 |
| D4.DEC | 6061 | 0x1842 | 11 | 2933 |
| D5.DEC | 6235 | 0x18e8 | 8 | 3025 |
| D6.DEC | 10574 | 0x2f96 | 5 | 5086 |
| D7.DEC | 6845 | 0x1b69 | 13 | 3350 |
| D8.DEC | 5470 | 0x1684 | 12 | 2678 |
| D9.DEC | 5307 | 0x154b | 10 | 2617 |

Some levels (D2, D3, D6) are dominated by opcodes 3/6/12/13 with long coordinate
runs, consistent with large filled background regions rather than detailed line art.
