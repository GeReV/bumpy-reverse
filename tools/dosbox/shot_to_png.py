#!/usr/bin/env python3
"""Decode a BUMPYSHOT dump (4 planes x 8000 bytes + 256x3 DAC palette) to a PNG.

Mode 0x0D is 320x200x16 planar: each pixel's 4-bit colour index = bit from each of
the 4 planes at the pixel's bit position (plane0=LSB).  DAC values are 6-bit (0-63);
scale to 8-bit for the PNG.  Usage: shot_to_png.py <shot.bin> <out.png>
"""
import sys
import struct
import zlib

W, H = 320, 200
PLANE = 8000  # 320*200/8


def main() -> None:
    src, out = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    planes = [data[p * PLANE:(p + 1) * PLANE] for p in range(4)]
    pal_off = 4 * PLANE
    pal = [
        (data[pal_off + i * 3] * 255 // 63,
         data[pal_off + i * 3 + 1] * 255 // 63,
         data[pal_off + i * 3 + 2] * 255 // 63)
        for i in range(256)
    ]
    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            byte_i = (y * W + x) >> 3
            bit = 7 - (x & 7)
            idx = 0
            for p in range(4):
                idx |= ((planes[p][byte_i] >> bit) & 1) << p
            row += bytes(pal[idx])
        rows.append(row)
    # minimal PNG (RGB, no filter)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(tag: bytes, body: bytes) -> bytes:
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(out, "wb").write(png)
    # quick stats so a noise-vs-image guess is possible without opening it
    hist: dict[int, int] = {}
    for p in range(4):
        pass
    nz = sum(1 for p in planes for b in p if b)
    print(f"wrote {out}  ({W}x{H})  nonzero plane bytes={nz}/{4*PLANE}")


if __name__ == "__main__":
    main()
