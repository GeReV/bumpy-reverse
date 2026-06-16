#!/usr/bin/env python3
"""Pure-Python .VEC renderer: RLE-decode (op4 scheme) then planar -> RGB PNG.

The .VEC body is RLE-compressed (escape byte = first inline byte at offset 12):
  b != escape         -> literal b
  escape, x, count    -> x repeated `count` (count==0 means 256); x != escape
  escape, escape      -> one literal escape byte
The decompressed buffer is a 320x200 16-colour EGA-planar image (its size is the
record-0 header word w1, `decoded_size`). See docs/formats/VEC.md.
"""
from __future__ import annotations
import argparse
import struct
import sys
import zlib
from typing import List, Tuple

# Default EGA/VGA 16-colour palette (RGB).
EGA: List[Tuple[int, int, int]] = [
    (0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170),
    (170, 0, 0), (170, 0, 170), (170, 85, 0), (170, 170, 170),
    (85, 85, 85), (85, 85, 255), (85, 255, 85), (85, 255, 255),
    (255, 85, 85), (255, 85, 255), (255, 255, 85), (255, 255, 255),
]


def be16(b: bytes, o: int) -> int:
    return (b[o] << 8) | b[o + 1]


def rle_decode(data: bytes, start: int, limit: int) -> bytearray:
    """RLE-decode `data` from `start`, producing up to `limit` bytes."""
    escape: int = data[start]
    out = bytearray()
    i: int = start + 1
    n: int = len(data)
    while len(out) < limit and i < n:
        b: int = data[i]; i += 1
        if b != escape:
            out.append(b)
            continue
        if i >= n:
            break
        x: int = data[i]; i += 1
        if x == escape:
            out.append(escape)
            continue
        if i >= n:
            break
        count: int = data[i]; i += 1
        if count == 0:
            count = 256
        out.extend(bytes([x]) * count)
    return out


def expand6(v: int) -> int:
    """Expand a 6-bit VGA DAC component (0..63) to 8-bit, as the VGA hardware does."""
    return ((v & 0x3F) << 2) | ((v & 0x3F) >> 4)


def extract_palette(buf: bytes, planar_size: int) -> Tuple[List[Tuple[int, int, int]], int]:
    """The decoded buffer is `[metadata][16x RGB-6bit palette][planar]`. The
    palette is the 48 bytes immediately before the planar data. Returns the 8-bit
    RGB palette and the offset where the planar data starts."""
    hdr_len: int = len(buf) - planar_size
    pal_off: int = hdr_len - 48
    palette: List[Tuple[int, int, int]] = []
    for i in range(16):
        o = pal_off + i * 3
        palette.append((expand6(buf[o]), expand6(buf[o + 1]), expand6(buf[o + 2])))
    return palette, hdr_len


def render_planar(buf: bytes, w: int, h: int,
                  palette: List[Tuple[int, int, int]] = EGA,
                  layout: str = "seq", hdr: int = 0) -> bytearray:
    """Decode an EGA-planar buffer to packed RGB. `layout` is the plane storage:
    'seq' (4 full planes), 'rowint' (per-scanline 4 planes), 'byteint' (4 bytes
    per 8 px)."""
    wb: int = w // 8
    plane: int = wb * h
    rgb = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            bit: int = 7 - (x % 8)
            v: int = 0
            for p in range(4):
                if layout == "seq":
                    off = hdr + p * plane + y * wb + x // 8
                elif layout == "rowint":
                    off = hdr + y * (wb * 4) + p * wb + x // 8
                else:  # byteint
                    off = hdr + (y * wb + x // 8) * 4 + p
                if 0 <= off < len(buf):
                    v |= ((buf[off] >> bit) & 1) << p
            r, g, bl = palette[v]
            o: int = (y * w + x) * 3
            rgb[o] = r; rgb[o + 1] = g; rgb[o + 2] = bl
    return rgb


def write_png(path: str, w: int, h: int, rgb: bytes) -> None:
    def chunk(typ: bytes, body: bytes) -> bytes:
        return (struct.pack(">I", len(body)) + typ + body +
                struct.pack(">I", zlib.crc32(typ + body) & 0xFFFFFFFF))
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3:(y + 1) * w * 3]
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def decode_vec(path: str) -> bytearray:
    data = open(path, "rb").read()
    return rle_decode(data, 12, be16(data, 2))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("vec")
    ap.add_argument("out_png")
    ap.add_argument("--layout", default="seq", choices=["seq", "rowint", "byteint"])
    ap.add_argument("--size", default="320x200")
    ap.add_argument("--ega", action="store_true", help="use the fixed EGA palette instead of the in-file one")
    args = ap.parse_args()
    w, h = (int(v) for v in args.size.split("x"))
    buf = decode_vec(args.vec)
    planar_size: int = w * h // 2          # 320x200 16-colour = 32000 bytes
    # Always extract (RLE-decode) — universal across all .VEC. Dump the buffer.
    raw_path: str = args.out_png + ".decoded.bin"
    open(raw_path, "wb").write(bytes(buf))
    print("decoded %d bytes -> %s" % (len(buf), raw_path))
    if len(buf) < planar_size + 48:
        print("NOTE: not a full-screen raster (decoded %d < %d). This is the "
              "structured world/level/sprite .VEC layout (task #8) — RLE-decoded "
              "buffer dumped above; pixel render needs the format reversed."
              % (len(buf), planar_size + 48))
        return
    palette, hdr_len = extract_palette(buf, planar_size)
    if args.ega:
        palette = EGA
    print("header %d bytes, palette @%d, planar @%d" % (hdr_len, hdr_len - 48, hdr_len))
    write_png(args.out_png, w, h, render_planar(buf, w, h, palette, args.layout, hdr_len))
    print("wrote", args.out_png)


if __name__ == "__main__":
    main()
