#!/usr/bin/env python3
"""Decode FLECHE.BIN (the world-map level-select cursor arrow) to PNG, pure-Python.

FLECHE.BIN is a Loriciel .BIN bank (see docs/formats/BIN.md): a BE32 offset
directory at 0, then a data section at 0x800 holding one frame — a 12-byte header
(`width`@-4 in 16-bit words/row, `height`@-2) followed by 4-plane block-interleaved
pixels (palette index 0 = transparent). The single frame is a 16x16 arrow.

The arrow is drawn over the world map, so it is coloured with MONDE1.VEC's embedded
16-colour palette. Output: local/results/sprites/fleche_arrow.png (1:1, RGBA) + an 8x copy.
"""
from __future__ import annotations
import os
import sys
import struct
import zlib
from typing import List, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools/extract"))
import vec_to_png as V          # noqa: E402

GAME = os.path.join(ROOT, "local/build/capture/game")
DATA_BASE = 0x800               # .BIN data section base (directory offsets are relative to it)

Pal = List[Tuple[int, int, int]]
Grid = List[List[int]]


def _be16(b: bytes, o: int) -> int:
    return struct.unpack(">H", b[o:o + 2])[0]


def _be32(b: bytes, o: int) -> int:
    return struct.unpack(">I", b[o:o + 4])[0]


def decode_bin_frame(b: bytes, idx: int = 0, data_base: int = DATA_BASE) -> Tuple[Grid, int, int]:
    """Decode one .BIN frame -> (index_grid, width_px, height). `idx` indexes the BE32
    directory; the entry is an offset (relative to data_base) to the frame's pixels, with
    a 12-byte header just before it (width in 16-bit words/row @-4, height @-2)."""
    fp = data_base + _be32(b, idx * 4)
    w = _be16(b, fp - 4)        # 16-bit words per row (4 words = one 16px block)
    h = _be16(b, fp - 2)
    img: Grid = []
    for r in range(h):
        words = [_be16(b, fp + r * w * 2 + 2 * i) for i in range(w)]
        row = []
        for blk in range(w // 4):
            planes = words[blk * 4:blk * 4 + 4]
            for col in range(16):
                row.append(sum(((planes[p] >> (15 - col)) & 1) << p for p in range(4)))
        img.append(row)
    return img, (w // 4) * 16, h


def write_rgba_png(path: str, w: int, h: int, rgba: bytes) -> None:
    def chunk(t: bytes, d: bytes) -> bytes:
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * w * 4:(y + 1) * w * 4]
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def render_rgba(img: Grid, wpx: int, h: int, pal: Pal, scale: int = 1) -> Tuple[int, int, bytearray]:
    """index grid -> RGBA (index 0 transparent), nearest-neighbour scaled."""
    W, H = wpx * scale, h * scale
    out = bytearray(W * H * 4)
    for y in range(h):
        for x in range(wpx):
            v = img[y][x]
            if v == 0:
                continue
            r, g, b = pal[v]
            for dy in range(scale):
                base = ((y * scale + dy) * W + x * scale) * 4
                for dx in range(scale):
                    o = base + dx * 4
                    out[o], out[o + 1], out[o + 2], out[o + 3] = r, g, b, 255
    return W, H, out


def main() -> None:
    b = open(os.path.join(GAME, "FLECHE.BIN"), "rb").read()
    img, wpx, h = decode_bin_frame(b, 0)
    # The arrow is composited over the world map, so use MONDE1.VEC's embedded palette.
    mem, stream = V.decode_vec_to_framebuffer(open(os.path.join(GAME, "MONDE1.VEC"), "rb").read())
    pal = V.embedded_palette(bytes(mem[stream:stream + V.DECLARED_LEN]))
    out = os.path.join(ROOT, "local/results/sprites")
    os.makedirs(out, exist_ok=True)
    for scale, suffix in ((1, ""), (8, "_8x")):
        W, H, rgba = render_rgba(img, wpx, h, pal, scale)
        write_rgba_png(os.path.join(out, "fleche_arrow%s.png" % suffix), W, H, rgba)
    print("FLECHE.BIN: %dx%d arrow -> results/sprites/fleche_arrow.png (+_8x)" % (wpx, h))


if __name__ == "__main__":
    main()
