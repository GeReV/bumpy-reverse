#!/usr/bin/env python3
"""Map the BUMSPJEU.BIN sprite container (CORRECTED model).

The file is a flat BE32 frame-offset table followed by a data section at 0x800:
    table   @0      : BE32 entries, offsets relative to the data base 0x800
    data    @0x800  : per-frame [12-byte header | packed pixels]
    frame i pixels  = file (0x800 + table[i]); the 12-byte header precedes that ptr.

Header (6 BE16 words at frame_ptr-0xc .. frame_ptr), read by prepare_sprite_frames:
    -0xc count   -0x8 .   -0x6 .   -0x4 width   -0x2 height   (and ctrl byte @-0xa/-10)

Confirmed against the runtime: the loader resolves table[i] -> a far pointer
base+0x800+off; the op12 seed's p1 frame 0 sits at file 0x80c, dims 4x16.

Codec: ctrl & 0x40 -> mask-RLE packed pixels (raw frames otherwise). This tool only
maps the container (table + per-frame headers/dims); the pure-Python pixel decode for
the raw-frame case lives in tools/extract/sprite_sheet.py.
"""
import os, struct, sys

DATA = 0x800


def main() -> None:
    os.chdir(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    path = sys.argv[1] if len(sys.argv) > 1 else "local/build/capture/game/BUMSPJEU.BIN"
    b = open(path, "rb").read()
    N = len(b)

    def be32(o: int) -> int:
        return struct.unpack(">I", b[o:o + 4])[0]

    def be16(o: int) -> int:
        return struct.unpack(">H", b[o:o + 2])[0]

    print("%s  %d bytes; data base %#x" % (os.path.basename(path), N, DATA))
    n = 0
    for i in range(DATA // 4):
        fp = DATA + be32(i*4)
        if not (DATA <= fp < N):
            break
        n += 1
    print("%d frame-table entries" % n)
    for i in range(min(n, 16)):
        fp = DATA + be32(i*4)
        print("  frame %2d  pixels@%#06x  w=%d h=%d ctrl=%#x" % (
            i, fp, be16(fp-4), be16(fp-2), b[fp-10]))


if __name__ == "__main__":
    main()
