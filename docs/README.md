# Bumpy's Arcade Fantasy — documentation

Reference documentation for the 1992 DOS game *Bumpy's Arcade Fantasy*: its file
formats, executable packaging, copy protection, and data/resource system.

## File formats — [`formats/`](formats/)

| Doc | Format |
|-----|--------|
| [formats/README.md](formats/README.md) | File-type map + the shared VEC/PAV/DEC/BUM container |
| [formats/VEC.md](formats/VEC.md) | `.VEC` vector-graphics command stream (header, records, op4 RLE, planar+palette) |
| [formats/PAV.md](formats/PAV.md) | `.PAV` per-level playfield/tile atlas |
| [formats/DEC.md](formats/DEC.md) | `.DEC` per-level décor / background |
| [formats/BUM.md](formats/BUM.md) | `.BUM` per-level objects / level-header table |
| [formats/LEVELS.md](formats/LEVELS.md) | How the per-level `.PAV/.DEC/.BUM` set forms a playable puzzle |
| [formats/BIN.md](formats/BIN.md) | `.BIN` sprite banks (`BUMSPJEU`, `FLECHE`) |
| [formats/CAR.md](formats/CAR.md) | `.CAR` proportional bitmap font |

`.BNK` (AdLib OPL2 instrument bank) and `.MID` (MIDI) are standard formats — see the
note in [formats/README.md](formats/README.md).

## Executable & systems

| Doc | Topic |
|-----|-------|
| [tinyprog.md](tinyprog.md) | The TinyProg executable-packing format (CRC-keyed anti-tamper + LZSS) |
| [copy-protection.md](copy-protection.md) | The copy-protection mechanism (platform-number challenge + protection data files) |
| [data-files.md](data-files.md) | Data-file inventory, resource descriptor tables, and the load pipeline |
| [06-engine.md](06-engine.md) | Engine internals (draft — to be rewritten from the decompiled source) |
| [ghidra-symbol-map.md](ghidra-symbol-map.md) | Symbol map for the BumpyDecomp Ghidra project — names + identities of every function and the named data labels |
