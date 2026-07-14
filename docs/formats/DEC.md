# `.DEC` — per-level décor / background ("décor" = scenery)

One per level: `D1.DEC`–`D9.DEC`. Uses the shared
[container format](README.md#the-shared-container-vecpavdecbum) and the same
`vec_run` interpreter as `.VEC`. These are medium-sized per-level files. The
confirmed decode (`tools/extract/op12_port.py`, byte-exact against the emulator
oracle) is an `op4`/`op12` `vec_run` stream into the **background tile-grid**
buffer that `.PAV` atlas tiles are placed against — see
[LEVELS.md](LEVELS.md) (the "Background" section) — not line/fill
scenery art; see the caveat below.

The 8-byte big-endian header is standard; see [VEC.md](VEC.md) for the record
layout and interpreter.

`.DEC` streams terminate via the opcode-validity / checksum terminator: the
terminal record carries opcode `0x750`, which fails the `w4 & 0x7f00 != 0`
validity check and signals end-of-stream (unlike `.PAV`/`.BUM` which end on the
`w0 > 0x0f` check).

Decoded by `tools/extract/vec_records.py` (raw-byte record walker, pre-decompress)
and `tools/extract/container.py` (header + opcode histogram); rendered by
`tools/extract/render_levels.py`.

## Files

**Caveat:** as with `.PAV`, the `Distinct opcodes`/`Coord words` columns below
come from `vec_records.py` scanning the still-RLE-compressed file bytes
directly, before `op4` decompression — most of the extra opcode hits are
checksum-collision scanner noise, not real dispatch. The real,
oracle-validated decode dispatches only `op4`/`op12` (see
[VEC.md](VEC.md#opcode-coverage-what-the-python-code-actually-dispatches)).
Kept here as raw empirical data, not as evidence of fill/line-art content.

| File | Bytes | `decoded_size` | Distinct opcodes (raw-byte scan, likely noise) | Coord words |
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
