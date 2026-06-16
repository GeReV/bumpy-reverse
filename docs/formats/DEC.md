# `.DEC` — per-level décor / background ("décor" = scenery)

One per level, `D1.DEC`..`D9.DEC`. Shared
[container format](README.md#the-shared-container-vecpavdecbum) + `vec_run`
interpreter. Medium-sized per-level files; **fewer, sparser opcodes** than
`.PAV`, consistent with background/scenery art drawn behind the playfield.

Header is the standard 8-byte big-endian block; body is the big-endian
opcode/coordinate token stream ([VEC.md](VEC.md)).

## Files (from `tools/extract/container.py`)

| File | Bytes | `decoded_size` | distinct opcodes | coord words |
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

Some levels (D2, D3, D6) are dominated by opcodes 3/6/12/13 with long
coordinate runs — likely large filled background regions rather than detailed
line art.

## Status

Container parses for all 9 levels. Opcode semantics shared with
[VEC.md](VEC.md); décor-vs-playfield layering confirmation is task #8.
