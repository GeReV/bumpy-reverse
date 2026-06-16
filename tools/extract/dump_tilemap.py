#!/usr/bin/env python3
"""Emulator-assisted level extractor: pull the collision tilemap out of a heap dump
produced by tools/render/dosemu.py.

The per-level files (.PAV/.DEC/.BUM) are heterogeneous vec-record streams (op4 RLE,
op12 draw records, and some raw blocks) that only the runtime renderer (vec_run)
fully interprets. Rather than re-implement that interpreter, we let the game itself
load+interpret the level (in tools/render/dosemu.py), then read the assembled
collision grid `tilemap` (DGROUP 0x203b:0xa0d8) straight out of the heap dump.
Tile bytes are the values the movement code switches on (0x0b wall, 0x0e/0x0f
special, etc.).

Run dosemu.py first (it writes build/render/dosemu_ram.bin for the loaded level),
then this. Outputs an ASCII map + the raw grid to build/extract/.
"""
from __future__ import annotations
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DUMP = os.path.join(ROOT, "local/build/render/dosemu_ram.bin")   # uc.mem_read(0x10000, ...)
OUT = os.path.join(ROOT, "local/build/extract")
DG = 0x114b0                  # DGROUP linear in the dosemu image (base 0x110)
WIDTH = 16                    # cells/row (best-fit; the regular top rows align at 16)
ROWS = 24


def main() -> None:
    buf = open(DUMP, "rb").read()

    def rd16(lin: int) -> int:
        o = lin - 0x10000
        return struct.unpack("<H", buf[o:o + 2])[0]

    tmap_off = rd16(DG + 0xA0D8)              # near offset of the grid within DGROUP
    base = DG + tmap_off - 0x10000
    grid = buf[base:base + WIDTH * ROWS]
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, "level_tilemap.bin"), "wb").write(grid)

    print("tilemap @ DGROUP 0x%04x, %dx%d grid" % (tmap_off, WIDTH, ROWS))
    print("legend: '.'=0 empty  '#'=1  hex digit=tile type (2-15)  '*'=>=16\n")
    for r in range(ROWS):
        line = []
        for c in range(WIDTH):
            b = grid[r * WIDTH + c]
            line.append("." if b == 0 else "#" if b == 1 else
                        ("%X" % b if b < 16 else "*"))
        print("  " + "".join(line))
    print("\nraw grid -> build/extract/level_tilemap.bin")
    print("NOTE: extracts whichever level dosemu.py last loaded (currently level 1).")
    print("Driving the emulator through level-select would let this dump all 9.")


if __name__ == "__main__":
    main()
