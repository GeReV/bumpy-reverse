# `.PAV` — per-level playfield ("pavé" = paving/tiles)

One per level: `D1.PAV`–`D9.PAV`. Uses the shared
[container format](README.md#the-shared-container-vecpavdecbum) and the same
`vec_run` interpreter as `.VEC`. Despite the shared container's vector-stream
framing, the confirmed decode (`tools/extract/op12_port.py`, byte-exact against
the emulator oracle) is a single `op4` RLE-decompress straight into the
320×192 4-plane **brush/tile atlas raster** — see [LEVELS.md](LEVELS.md) for how
`.DEC` places atlas tiles onto the per-level grid. It is not drawn from line/fill
primitives; see the opcode-coverage note below.

The 8-byte big-endian header is standard (`w0=0`, `w1=decoded_size`,
`w2`/`w3`=checksums). See [VEC.md](VEC.md) for the record layout and interpreter,
and its "Opcode coverage" note on why the table below over-counts opcodes.

Decoded by `tools/extract/vec_records.py` (raw-byte record walker, pre-decompress)
and `tools/extract/container.py` (header + opcode histogram); rendered by
`tools/extract/render_levels.py`.

## Files

**Caveat:** the `Distinct opcodes`/`Coord words` columns below come from
`vec_records.py` scanning the still-RLE-compressed file bytes directly (it has no
decompressor), using only the 16-bit XOR checksum to find "record" boundaries.
Across thousands of word-aligned scan positions in compressed data, spurious
checksum matches are expected, so most of these opcode hits are scanner noise,
not real interpreter dispatch — the real, oracle-validated decode dispatches only
`op4` for every `.PAV` file (see [VEC.md](VEC.md#opcode-coverage-what-the-python-code-actually-dispatches)).
Kept here as raw empirical data, not as evidence of line-art content.

| File | Bytes | `decoded_size` | Distinct opcodes (raw-byte scan, likely noise) | Coord words |
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
