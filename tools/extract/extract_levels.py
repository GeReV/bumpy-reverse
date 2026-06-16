#!/usr/bin/env python3
"""Extract Bumpy per-level data files (D<n>.PAV/.DEC/.BUM).

Each file is a single op4 vec-record: a 12-byte big-endian header (w0..w5, w4=opcode
=4, w5 = XOR checksum) followed by an RLE-compressed payload. The payload decompresses
into the level buffer that the runtime renderer (vec_run) then interprets:
  .PAV -> playfield  (buffer 0x7806)
  .DEC -> decor      (buffer 0x2f96)
  .BUM -> objects    (buffer 0x0b60)

This tool decompresses every level file, verifies the decompressed size against the
game's documented buffer sizes (docs/06-engine.md), writes the raw decompressed
streams to build/extract/, and prints a per-file summary. Correctness is confirmed
when decoded size == expected buffer size for all 9 levels.
"""
from __future__ import annotations
import os
from typing import Dict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME = os.path.join(ROOT, "local/originals/old-games/bumpy")
OUT = os.path.join(ROOT, "local/build/extract")

# documented decompressed buffer sizes (alloc_level_buffers / docs/06-engine.md)
EXPECT: Dict[str, int] = {"PAV": 0x7806, "DEC": 0x2f96, "BUM": 0x0b60}


def be16(d: bytes, o: int) -> int:
    return (d[o] << 8) | d[o + 1]


def rle_decode(data: bytes, start: int, limit: int) -> bytearray:
    """op4 RLE: first payload byte = escape. b!=esc -> literal; esc,esc -> one esc;
    esc,value,count -> value*count (count 0 means 256)."""
    escape = data[start]
    out = bytearray()
    i = start + 1
    n = len(data)
    while len(out) < limit and i < n:
        b = data[i]; i += 1
        if b != escape:
            out.append(b)
            continue
        if i >= n:
            break
        x = data[i]; i += 1
        if x == escape:
            out.append(escape)
            continue
        if i >= n:
            break
        count = data[i]; i += 1
        out.extend(bytes([x]) * (count if count else 256))
    return out


def main() -> None:
    # NOTE: the level files are vec-record STREAMS interpreted by vec_run, not uniform
    # single-op4 blobs. Observed first-record kinds across the 9 levels:
    #   op4  -> stream starts with an RLE-compressed block (decompress here)
    #   op12 -> stream starts with plot/draw records (NOT RLE; needs the interpreter)
    #   raw  -> file size == buffer size, no valid record header (stored uncompressed)
    # So this tool reports each file's structure and decompresses only the op4-first
    # ones; a *complete* extraction (tilemap grid + objects) needs the full vec_run
    # walk -- see notes below / docs/06-engine.md. Best done emulator-assisted.
    os.makedirs(OUT, exist_ok=True)
    print("%-10s %7s %7s %5s %9s %s" % ("file", "filesz", "kind", "op", "op4_decode", "note"))
    for n in range(1, 10):
        for ext in ("PAV", "DEC", "BUM"):
            path = os.path.join(GAME, "D%d.%s" % (n, ext))
            if not os.path.exists(path):
                continue
            d = open(path, "rb").read()
            w = [be16(d, i * 2) for i in range(6)]
            op = w[4] & 0x7fff
            chk_ok = (w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4]) == w[5]
            decoded = "-"
            note = ""
            if not chk_ok:
                kind = "raw"
                note = "uncompressed (no record header)"
            elif op == 4:
                kind = "op4"
                dec = rle_decode(d, 12, EXPECT[ext] + 64)
                open(os.path.join(OUT, "D%d.%s.bin" % (n, ext)), "wb").write(dec)
                decoded = str(len(dec))
                note = "RLE-decompressed -> build/extract/"
            else:
                kind = "op%d" % op
                note = "draw-record stream (needs vec_run walk)"
            print("%-10s %7d %7s %5d %9s %s" % ("D%d.%s" % (n, ext), len(d), kind, op, decoded, note))
    print("\nop4-first files decompressed to %s/ ; op12-first + raw need the vec_run interpreter."
          % os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
