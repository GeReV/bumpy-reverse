# ImHex patterns for Bumpy resource files

[ImHex](https://imhex.werwolv.net/) pattern-language (`.hexpat`) descriptions of the
Loriciel-custom resource formats in *Bumpy's Arcade Fantasy*. They are inspection aids
that complement the pure-Python extractors in `tools/extract/` — they describe the
on-disk structure; the extractors own the actual pixel/audio decode.

All formats here are **big-endian** (`#pragma endian big`).

| Pattern | Describes | Target game files | Spec |
|---------|-----------|-------------------|------|
| `container.hexpat` | Shared container: 8-byte header + reusable 12-byte `VecRecord` + opaque op4-RLE body | `*.VEC`, `D?.PAV`, `D?.DEC`, `D?.BUM` | `../docs/formats/{VEC,PAV,DEC,BUM}.md` |
| `bin.hexpat` | Flat BE32 frame-offset table (relative to data base `0x800`) + per-frame `[12-byte header \| planar pixels]` | `BUMSPJEU.BIN`, `FLECHE.BIN` | `../docs/formats/BIN.md` |
| `car.hexpat` | 4-byte header + BE16 glyph-offset table + per-glyph bitmap blobs | `DDFNT2.CAR` | `../docs/formats/CAR.md` |

`.BNK` (standard AdLib OPL2 bank) and `.MID` (standard MIDI) are intentionally not
covered — use existing tooling.

## Using them in ImHex

1. Open a game file (e.g. `D1.PAV`) in ImHex.
2. *File ▸ Load Pattern…* and pick the matching `.hexpat` (or drop the files into ImHex's
   own `patterns/` directory so they auto-suggest). The standard library
   (`#include <std/...>`) ships with ImHex, so no extra include path is needed in the GUI.

Notes per format:

- **container** — decodes the documented 8-byte header and overlays record 0 (always
  `op4`, with `w1 == decoded_size`) using the reusable `VecRecord` type. The body from
  `0x0D` is `op4`-RLE-compressed and is shown as an opaque `compressed_body` blob; the
  escape byte is at `0x0C`. `tools/extract/vec_records.py` walks that raw, still-compressed
  body directly (a checksum-only scan, no decompression) and is useful for empirical byte
  accounting, but most "records" it finds past record 0 are coincidental checksum hits in
  the compressed data, not real dispatched opcodes — see
  `docs/formats/VEC.md#opcode-coverage-what-the-python-code-actually-dispatches`. The
  actual post-decompression decode (confirmed to need only `op4`/`op12`) lives in
  `tools/extract/op12_port.py`; see "RLE" below for why that decode is not in the pattern.
- **bin** — walks the frame-offset table until an entry points past EOF, then lays a
  `Frame` (header + `width_words * height` planar `u16`s) at each `0x800 + offset - 0xC`.
  The mask-RLE pixel variant (`ctrl & 0x40`) is flagged in a comment but not expanded.
- **car** — reads the BE16 offset table and places a `Glyph` at each entry. `entries[0]`
  is the known "phantom" (a non-glyph blob) and parses as garbage by design; real glyphs
  start at index 1 (`char C → table index C - first_char + 1`).

## Headless tests

`tests/run.sh` generates tiny **synthetic** fixtures (no copyrighted game data) and runs
each pattern through `plcli` (the ImHex Pattern Language CLI), asserting parsed values
with `jq`:

```sh
bash tests/run.sh        # prints "ALL PATTERNS PARSED OK" on success
```

It expects `plcli` and the ImHex stdlib include dir; both are overridable via env:

```sh
PLCLI=/path/to/plcli IMHEX_INCLUDES=/path/to/imhex/includes bash tests/run.sh
```

The defaults point at a local ImHex build under the git-ignored `local/tools/` (the
`plcli` binary built from `WerWolv/PatternLanguage`, and the stdlib extracted from the
ImHex AppImage). `tests/make_fixtures.py` builds the fixtures; `tests/fixtures/` holds
the generated `.bin`s.

## Validation status

These patterns pass the synthetic-fixture harness under `plcli` (correct field values,
big-endian layout, computed placements, bitfield orientation) **and** were run against the
real game files under `local/build/capture/game/`:

- **`car.hexpat` → `DDFNT2.CAR`**: `first_char=0x20`, 126 glyphs (matches the format doc).
- **`container.hexpat`**: parses every `D?.PAV/DEC/BUM`, `MONDE?.VEC`, `TITRE.VEC`, etc.;
  `record0` is always `op4` and `decoded_size` matches the format docs. It also faithfully
  shows the documented oddities: `SCORE.VEC` (the `w1=0` pre-decoded outlier) and
  `D6.BUM`/`D9.BUM` (the header variant — `magic != 0`).
- **`bin.hexpat` → `BUMSPJEU.BIN`**: 512 frame-table entries (frame 0 = 4 words × 15 rows).
  `FLECHE.BIN` is a degenerate single-frame bank loaded via a dedicated path
  (`render_fleche.py`); the generic table walk over-counts it — only frame 0 is real.

Pixel/structure *decode* beyond the header level remains the job of `tools/extract/`; the
patterns describe layout only.

## RLE: why the container body stays opaque

An in-pattern `op4` RLE decode (into an ImHex *section*, then overlaying the decoded
palette/planar image) was evaluated and intentionally dropped. ImHex's
`std::mem::copy_value_to_section` copies an existing **pattern's** bytes, not computed
scalar values, so an op4 decoder written in the pattern language would need a placed
pattern per output byte plus run expansion — disproportionate for an inspection aid. The
byte-exact decode already lives in `tools/extract/op12_port.py` (and the record walk in
`tools/extract/vec_records.py`); the pattern deliberately stops at the static header +
record 0.
