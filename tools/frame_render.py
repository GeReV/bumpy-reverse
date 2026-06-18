#!/usr/bin/env python3
"""Render frame_oracle.bin (FRM1: 4 VGA planes + DAC palette) to a PNG, to eyeball
the engine's settled level frame (background + entities + players)."""
import struct
import sys
import zlib


def load(path):
    b = open(path, "rb").read()
    assert b[:4] in (b"FRM1", b"FRM2"), b[:4]
    plen = struct.unpack_from("<I", b, 4)[0]
    o = 8
    planes = b[o:o + plen]; o += plen
    dac = b[o:o + 256 * 3]
    return planes, dac


def write_png(path, w, h, rgb):
    def chunk(typ, data):
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3:(y + 1) * w * 3]
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "local/build/render/frame_oracle.bin"
    out = sys.argv[2] if len(sys.argv) > 2 else "local/build/render/frame_oracle.png"
    W, H, PLANE = 320, 200, 0x10000
    planes, dac = load(src)
    pal = [(min(255, dac[i * 3] * 255 // 63),
            min(255, dac[i * 3 + 1] * 255 // 63),
            min(255, dac[i * 3 + 2] * 255 // 63)) for i in range(256)]
    rgb = bytearray(W * H * 3)
    for y in range(H):
        for bx in range(W // 8):
            off = y * 40 + bx
            p0, p1, p2, p3 = (planes[off], planes[PLANE + off],
                              planes[2 * PLANE + off], planes[3 * PLANE + off])
            for bit in range(8):
                m = 0x80 >> bit
                idx = (((p0 & m) and 1) | (((p1 & m) and 1) << 1)
                       | (((p2 & m) and 1) << 2) | (((p3 & m) and 1) << 3))
                x = bx * 8 + bit
                r, g, bl = pal[idx]
                px = (y * W + x) * 3
                rgb[px] = r; rgb[px + 1] = g; rgb[px + 2] = bl
    write_png(out, W, H, rgb)
    print(f"wrote {out} ({W}x{H})")


if __name__ == "__main__":
    main()
