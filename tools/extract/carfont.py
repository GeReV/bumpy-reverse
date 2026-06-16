#!/usr/bin/env python3
"""Carve glyphs from a Bumpy .CAR font ("caracteres"). Loriciel-custom; provisional.

Empirical layout (DDFNT2.CAR):
  Header: u8 first_char, u8 count_or_last, u8 dim_a, u8 dim_b
  Then a table of big-endian uint16 offsets (one per glyph) into the file; each
  glyph's bitmap spans [offset[i], offset[i+1]). Glyph bit layout (planar/packed)
  not yet confirmed - this carves the per-glyph byte ranges for inspection.

Usage: carfont.py <file.CAR> ...  -> local/build/extract/car/<name>/glyph_NNN.bin + report
"""
import sys, os

OUT = "local/build/extract/car"


def be16(b: bytes, o: int) -> int:
    return (b[o] << 8) | b[o + 1]


def main() -> None:
    for path in sys.argv[1:]:
        b = open(path, "rb").read()
        name = os.path.basename(path)
        first_char, second, dim_a, dim_b = b[0], b[1], b[2], b[3]
        # Read BE16 offsets from offset 4 while monotonically increasing & in range.
        offs: list[int] = []
        o = 4
        prev = -1
        while o + 2 <= len(b):
            v = be16(b, o)
            if v <= prev or v > len(b):
                break
            if offs and o >= offs[0]:
                break
            offs.append(v)
            prev = v
            o += 2
        outdir = os.path.join(OUT, name)
        os.makedirs(outdir, exist_ok=True)
        bounds = offs + [len(b)]
        sizes = []
        for i in range(len(offs)):
            g = b[bounds[i]:bounds[i + 1]]
            sizes.append(len(g))
            open(os.path.join(outdir, "glyph_%03d.bin" % i), "wb").write(g)
        print("%-12s %5d bytes  hdr=[first=0x%02x b=0x%02x dim=%dx%d]  glyphs=%d  sizes(uniq)=%s -> %s/" % (
            name, len(b), first_char, second, dim_a, dim_b, len(offs), sorted(set(sizes))[:8], outdir))


if __name__ == "__main__":
    main()
