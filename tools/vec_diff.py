#!/usr/bin/env python3
"""Differential validation tool: compare a chunky-index RAW against an oracle PNG.

Usage:
    vec_diff.py <chunky.RAW> <oracle.png> [--vec <file.VEC>] [--out <render.png>]

Reads a 320×200 16-colour chunky index buffer (64000 bytes, values 0..15),
renders it to RGB using the palette embedded in the given .VEC file (or the
default EGA palette), writes a PNG, then compares pixel-for-pixel against the
oracle PNG. Exits 0 on exact match, 1 on any difference or error.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib
from pathlib import Path
from typing import List, Optional, Tuple

# ---------------------------------------------------------------------------
# Import shared code from the sibling extract package.
# ---------------------------------------------------------------------------
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE / "extract"))

from vec_render import (  # noqa: E402
    EGA,
    be16,
    expand6,
    rle_decode,
    write_png,
)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
WIDTH: int = 320
HEIGHT: int = 200
PIXELS: int = WIDTH * HEIGHT          # 64000
PLANAR_SIZE: int = PIXELS // 2        # 32000 — used only to locate the palette


# ---------------------------------------------------------------------------
# Palette extraction from a decoded VEC buffer (replicates vec_render logic).
# ---------------------------------------------------------------------------

def _palette_from_decoded(buf: bytes) -> List[Tuple[int, int, int]]:
    """Extract the 16-colour 8-bit RGB palette from a fully-decoded VEC buffer.

    The decoded VEC buffer layout is:
        [metadata header] [16 × 3 bytes of 6-bit DAC values] [planar data]
    The header length is ``len(buf) - PLANAR_SIZE``; the palette occupies the
    48 bytes immediately before the planar data (``hdr_len - 48``).
    """
    hdr_len: int = len(buf) - PLANAR_SIZE
    pal_off: int = hdr_len - 48
    palette: List[Tuple[int, int, int]] = []
    for i in range(16):
        o: int = pal_off + i * 3
        palette.append((expand6(buf[o]), expand6(buf[o + 1]), expand6(buf[o + 2])))
    return palette


def read_palette_from_vec(vec_path: str) -> Optional[List[Tuple[int, int, int]]]:
    """Decode *vec_path* and return its embedded 16-colour palette, or ``None``
    if the file is too small to contain a full-screen palette."""
    data: bytes = open(vec_path, "rb").read()
    decoded_size: int = be16(data, 2)
    buf: bytes = bytes(rle_decode(data, 12, decoded_size))
    if len(buf) < PLANAR_SIZE + 48:
        return None
    return _palette_from_decoded(buf)


# ---------------------------------------------------------------------------
# Chunky-index buffer → RGB
# ---------------------------------------------------------------------------

def chunky_to_rgb(raw: bytes, palette: List[Tuple[int, int, int]]) -> bytearray:
    """Convert a 64000-byte chunky index buffer to a packed RGB bytearray."""
    if len(raw) != PIXELS:
        raise ValueError(f"chunky buffer is {len(raw)} bytes; expected {PIXELS}")
    rgb = bytearray(PIXELS * 3)
    for i, idx in enumerate(raw):
        r, g, b = palette[idx & 0x0F]
        o: int = i * 3
        rgb[o] = r
        rgb[o + 1] = g
        rgb[o + 2] = b
    return rgb


# ---------------------------------------------------------------------------
# Minimal stdlib PNG reader (RGB 8-bit only)
# ---------------------------------------------------------------------------

def _paeth(a: int, b: int, c: int) -> int:
    p: int = a + b - c
    pa: int = abs(p - a)
    pb: int = abs(p - b)
    pc: int = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def read_png(path: str) -> Tuple[int, int, bytes]:
    """Read an RGB 8-bit PNG and return ``(width, height, rgb_bytes)``."""
    data: bytes = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a valid PNG file")
    pos: int = 8
    idat_parts: List[bytes] = []
    ihdr_body: Optional[bytes] = None
    while pos < len(data):
        length: int = struct.unpack(">I", data[pos:pos + 4])[0]
        typ: bytes = data[pos + 4:pos + 8]
        body: bytes = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if typ == b"IHDR":
            ihdr_body = body
        elif typ == b"IDAT":
            idat_parts.append(body)
        elif typ == b"IEND":
            break
    if ihdr_body is None:
        raise ValueError(f"{path}: missing IHDR chunk")
    w, h = struct.unpack(">II", ihdr_body[:8])
    bit_depth: int = ihdr_body[8]
    colour_type: int = ihdr_body[9]
    if bit_depth != 8 or colour_type != 2:
        raise ValueError(
            f"{path}: unsupported PNG (bit_depth={bit_depth}, colour_type={colour_type}); "
            "only 8-bit RGB is supported"
        )
    raw: bytes = zlib.decompress(b"".join(idat_parts))
    stride: int = 1 + w * 3
    pixels = bytearray(w * h * 3)
    prev = bytearray(w * 3)
    for y in range(h):
        filt: int = raw[y * stride]
        row = bytearray(raw[y * stride + 1:y * stride + 1 + w * 3])
        if filt == 0:
            pass  # None
        elif filt == 1:  # Sub
            for x in range(3, len(row)):
                row[x] = (row[x] + row[x - 3]) & 0xFF
        elif filt == 2:  # Up
            for x in range(len(row)):
                row[x] = (row[x] + prev[x]) & 0xFF
        elif filt == 3:  # Average
            for x in range(len(row)):
                a: int = row[x - 3] if x >= 3 else 0
                row[x] = (row[x] + (a + prev[x]) // 2) & 0xFF
        elif filt == 4:  # Paeth
            for x in range(len(row)):
                a = row[x - 3] if x >= 3 else 0
                b_: int = prev[x]
                c: int = prev[x - 3] if x >= 3 else 0
                row[x] = (row[x] + _paeth(a, b_, c)) & 0xFF
        else:
            raise ValueError(f"{path}: unknown PNG filter type {filt} at row {y}")
        pixels[y * w * 3:(y + 1) * w * 3] = row
        prev = row
    return w, h, bytes(pixels)


# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------

def compare_rgb(
    rendered: bytes,
    oracle: bytes,
    total_pixels: int,
) -> Tuple[int, int]:
    """Return ``(n_diff_pixels, max_channel_delta)`` between two RGB byte strings."""
    n_diff: int = 0
    max_delta: int = 0
    for i in range(total_pixels):
        o: int = i * 3
        dr: int = abs(rendered[o] - oracle[o])
        dg: int = abs(rendered[o + 1] - oracle[o + 1])
        db: int = abs(rendered[o + 2] - oracle[o + 2])
        pixel_delta: int = max(dr, dg, db)
        if pixel_delta > 0:
            n_diff += 1
            if pixel_delta > max_delta:
                max_delta = pixel_delta
    return n_diff, max_delta


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("raw", metavar="chunky.RAW",
                    help="320×200 chunky index buffer (64000 bytes)")
    ap.add_argument("oracle", metavar="oracle.png",
                    help="Reference PNG to compare against")
    ap.add_argument("--vec", metavar="file.VEC",
                    help="Extract palette from this .VEC file (preferred over EGA default)")
    ap.add_argument("--out", metavar="render.png",
                    help="Where to write the rendered PNG (default: results/renders/<stem>.png)")
    args = ap.parse_args()

    # 1. Read chunky buffer.
    raw: bytes = open(args.raw, "rb").read()
    if len(raw) != PIXELS:
        print(f"ERROR: {args.raw} is {len(raw)} bytes; expected {PIXELS}", file=sys.stderr)
        sys.exit(1)

    # 2. Resolve palette.
    palette: List[Tuple[int, int, int]]
    if args.vec:
        extracted = read_palette_from_vec(args.vec)
        if extracted is not None:
            palette = extracted
        else:
            print(f"WARNING: {args.vec} too small for embedded palette; using EGA default",
                  file=sys.stderr)
            palette = list(EGA)
    else:
        palette = list(EGA)

    # 3. Render chunky → RGB.
    rgb: bytearray = chunky_to_rgb(raw, palette)

    # 4. Determine output PNG path.
    out_path: str
    if args.out:
        out_path = args.out
    else:
        stem: str = Path(args.raw).stem
        out_dir = Path(__file__).resolve().parent.parent / "results" / "renders"
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = str(out_dir / f"{stem}.png")

    # 5. Write rendered PNG (reuse vec_render.write_png).
    write_png(out_path, WIDTH, HEIGHT, bytes(rgb))

    # 6. Load oracle PNG.
    oracle_w, oracle_h, oracle_rgb = read_png(args.oracle)
    if oracle_w != WIDTH or oracle_h != HEIGHT:
        print(
            f"ERROR: oracle {args.oracle} is {oracle_w}×{oracle_h}; "
            f"expected {WIDTH}×{HEIGHT}",
            file=sys.stderr,
        )
        sys.exit(1)

    # 7. Compare pixel-for-pixel.
    n_diff, max_delta = compare_rgb(bytes(rgb), oracle_rgb, PIXELS)

    if n_diff == 0:
        print(f"MATCH exact ({WIDTH}x{HEIGHT})")
        sys.exit(0)
    else:
        print(f"DIFF pixels={n_diff}/{PIXELS} maxchanneldelta={max_delta}")
        sys.exit(1)


if __name__ == "__main__":
    main()
