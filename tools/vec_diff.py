#!/usr/bin/env python3
"""Differential validation tool: compare a chunky-index RAW (or planar .PLN)
against an oracle PNG.

Usage (existing chunky mode):
    vec_diff.py <chunky.RAW> <oracle.png> [--vec <file.VEC>] [--out <render.png>]

Usage (planar mode — no oracle required):
    vec_diff.py --planar <capture.PLN> [--out <render.png>]

Reads a 320x200 16-colour chunky index buffer (64000 bytes, values 0..15),
renders it to RGB using the palette embedded in the given .VEC file (or the
default EGA palette), writes a PNG, then compares pixel-for-pixel against the
oracle PNG. Exits 0 on exact match, 1 on any difference or error.

In --planar mode the .PLN file is a 32768-byte capture from run_bvec.py:
    bytes   0 .. 31999  — 4 planes x 8000 bytes (plane-major order)
    bytes 32000 .. 32767 — 256 x 3 bytes of 6-bit DAC values (R,G,B)
The de-plane step reconstructs the 64000-byte chunky index buffer from the
four planes, applies the captured DAC palette, and writes a PNG.  No oracle
comparison is performed in this mode (exit 0 unless an error occurs).
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
PLANAR_SIZE: int = PIXELS // 2        # 32000 — 4 planes x 8000 bytes
DAC_SIZE: int = 256 * 3              # 768 bytes — 6-bit R,G,B for 256 entries
PLN_SIZE: int = PLANAR_SIZE + DAC_SIZE  # 32768 bytes total in .PLN file


# ---------------------------------------------------------------------------
# Palette extraction from a decoded VEC buffer (replicates vec_render logic).
# ---------------------------------------------------------------------------

def _palette_from_decoded(buf: bytes) -> List[Tuple[int, int, int]]:
    """Extract the 16-colour 8-bit RGB palette from a fully-decoded VEC buffer.

    The decoded VEC buffer layout is:
        [metadata header] [16 x 3 bytes of 6-bit DAC values] [planar data]
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
# Planar capture de-plane: .PLN -> chunky index buffer
# ---------------------------------------------------------------------------

def deplane(pln: bytes) -> Tuple[bytearray, List[Tuple[int, int, int]]]:
    """De-plane a .PLN capture into a 64000-byte chunky index buffer + palette.

    .PLN layout (produced by tools/run_bvec.py Host.dump_planar):
        bytes   0 .. 31999  — plane 0..3 sequential, 8000 bytes each
        bytes 32000 .. 32767 — 256 x 3 bytes DAC (6-bit R,G,B per entry)

    Returns (chunky, palette) where palette is a list of 16 (R8,G8,B8) tuples.
    """
    if len(pln) < PLN_SIZE:
        raise ValueError("PLN too small: %d bytes (need %d)" % (len(pln), PLN_SIZE))
    planes: List[bytes] = [pln[p * 8000:(p + 1) * 8000] for p in range(4)]
    chunky = bytearray(PIXELS)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            byte_off: int = y * 40 + (x >> 3)
            bit_shift: int = 7 - (x & 7)
            idx: int = 0
            for p in range(4):
                idx |= ((planes[p][byte_off] >> bit_shift) & 1) << p
            chunky[y * WIDTH + x] = idx
    # Palette: first 16 DAC entries, 6-bit -> 8-bit via expand6.
    dac_base: int = PLANAR_SIZE
    palette: List[Tuple[int, int, int]] = []
    for i in range(16):
        o: int = dac_base + i * 3
        palette.append((expand6(pln[o]), expand6(pln[o + 1]), expand6(pln[o + 2])))
    return chunky, palette


# ---------------------------------------------------------------------------
# Chunky-index buffer -> RGB
# ---------------------------------------------------------------------------

def chunky_to_rgb(raw: bytes, palette: List[Tuple[int, int, int]]) -> bytearray:
    """Convert a 64000-byte chunky index buffer to a packed RGB bytearray."""
    if len(raw) != PIXELS:
        raise ValueError("chunky buffer is %d bytes; expected %d" % (len(raw), PIXELS))
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
        raise ValueError("%s: not a valid PNG file" % path)
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
        raise ValueError("%s: missing IHDR chunk" % path)
    w, h = struct.unpack(">II", ihdr_body[:8])
    bit_depth: int = ihdr_body[8]
    colour_type: int = ihdr_body[9]
    if bit_depth != 8 or colour_type != 2:
        raise ValueError(
            "%s: unsupported PNG (bit_depth=%d, colour_type=%d); only 8-bit RGB is supported"
            % (path, bit_depth, colour_type)
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
            raise ValueError("%s: unknown PNG filter type %d at row %d" % (path, filt, y))
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
    ap.add_argument("raw", metavar="chunky.RAW or capture.PLN",
                    help="320x200 chunky index buffer (64000 bytes) or planar capture (.PLN)")
    ap.add_argument("oracle", metavar="oracle.png", nargs="?",
                    help="Reference PNG to compare against (chunky mode only)")
    ap.add_argument("--planar", action="store_true",
                    help="Input is a .PLN planar capture (32000 planar + 768 DAC bytes); "
                         "de-plane to chunky and render PNG.  If an oracle PNG positional "
                         "arg is also given, performs the same RGB comparison + verdict as "
                         "chunky mode (MATCH exact / DIFF pixels=...).  Without an oracle, "
                         "renders to PNG and exits 0.")
    ap.add_argument("--vec", metavar="file.VEC",
                    help="Extract palette from this .VEC file (chunky mode, preferred over EGA default)")
    ap.add_argument("--out", metavar="render.png",
                    help="Where to write the rendered PNG (default: results/renders/<stem>.png)")
    args = ap.parse_args()

    # Determine output PNG path.
    out_path: str
    if args.out:
        out_path = args.out
    else:
        stem: str = Path(args.raw).stem
        out_dir = Path(__file__).resolve().parent.parent / "results" / "renders"
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = str(out_dir / ("%s.png" % stem))

    if args.planar:
        # ---- planar de-plane mode ----
        pln: bytes = open(args.raw, "rb").read()
        if len(pln) < PLN_SIZE:
            print("ERROR: %s is %d bytes; expected at least %d" % (args.raw, len(pln), PLN_SIZE),
                  file=sys.stderr)
            sys.exit(1)
        chunky, palette = deplane(pln)
        rgb: bytearray = chunky_to_rgb(bytes(chunky), palette)
        write_png(out_path, WIDTH, HEIGHT, bytes(rgb))

        if not args.oracle:
            # Render-only: no oracle supplied — just report success and exit.
            print("PLANAR de-plane ok -> %s" % out_path)
            sys.exit(0)

        # Oracle supplied: run the same comparison as chunky mode.
        oracle_w: int
        oracle_h: int
        oracle_rgb: bytes
        oracle_w, oracle_h, oracle_rgb = read_png(args.oracle)
        if oracle_w != WIDTH or oracle_h != HEIGHT:
            print(
                "ERROR: oracle %s is %dx%d; expected %dx%d"
                % (args.oracle, oracle_w, oracle_h, WIDTH, HEIGHT),
                file=sys.stderr,
            )
            sys.exit(1)

        n_diff: int
        max_delta: int
        n_diff, max_delta = compare_rgb(bytes(rgb), oracle_rgb, PIXELS)

        if n_diff == 0:
            print("MATCH exact (%dx%d)" % (WIDTH, HEIGHT))
            sys.exit(0)
        else:
            print("DIFF pixels=%d/%d maxchanneldelta=%d" % (n_diff, PIXELS, max_delta))
            sys.exit(1)

    # ---- chunky mode (original behaviour) ----
    # 1. Read chunky buffer.
    raw: bytes = open(args.raw, "rb").read()
    if len(raw) != PIXELS:
        print("ERROR: %s is %d bytes; expected %d" % (args.raw, len(raw), PIXELS),
              file=sys.stderr)
        sys.exit(1)

    # 2. Resolve palette.
    palette_list: List[Tuple[int, int, int]]
    if args.vec:
        extracted = read_palette_from_vec(args.vec)
        if extracted is not None:
            palette_list = extracted
        else:
            print("WARNING: %s too small for embedded palette; using EGA default" % args.vec,
                  file=sys.stderr)
            palette_list = list(EGA)
    else:
        palette_list = list(EGA)

    # 3. Render chunky -> RGB.
    rgb = chunky_to_rgb(raw, palette_list)

    # 4. Write rendered PNG (reuse vec_render.write_png).
    write_png(out_path, WIDTH, HEIGHT, bytes(rgb))

    # 5. Oracle comparison (required in chunky mode).
    if not args.oracle:
        print("ERROR: oracle PNG required in chunky mode (or use --planar)", file=sys.stderr)
        sys.exit(1)

    oracle_w, oracle_h, oracle_rgb = read_png(args.oracle)
    if oracle_w != WIDTH or oracle_h != HEIGHT:
        print(
            "ERROR: oracle %s is %dx%d; expected %dx%d"
            % (args.oracle, oracle_w, oracle_h, WIDTH, HEIGHT),
            file=sys.stderr,
        )
        sys.exit(1)

    # 6. Compare pixel-for-pixel.
    n_diff, max_delta = compare_rgb(bytes(rgb), oracle_rgb, PIXELS)

    if n_diff == 0:
        print("MATCH exact (%dx%d)" % (WIDTH, HEIGHT))
        sys.exit(0)
    else:
        print("DIFF pixels=%d/%d maxchanneldelta=%d" % (n_diff, PIXELS, max_delta))
        sys.exit(1)


if __name__ == "__main__":
    main()
