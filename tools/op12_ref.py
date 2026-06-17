#!/usr/bin/env python3
"""op12 reference-arena extractor.

Given a .VEC path, runs the exact op4 -> Op12.vec_run flow on a flat mem
buffer (via vec_to_png.decode_vec_to_framebuffer), then dumps the resulting
decode-arena region — mem[stream : stream+declared_len] — to a <name>.ARENA
file for byte-for-byte comparison against the C decoder's arena (Task 2).

Usage:
    python3 tools/op12_ref.py local/build/capture/game/MASKBUMP.VEC
    python3 tools/op12_ref.py MASKBUMP.VEC --out /tmp/maskbump.ARENA
    python3 tools/op12_ref.py MASKBUMP.VEC --png maskbump_ref.png \\
        --oracle results/images/maskbump.png
"""
from __future__ import annotations

import argparse
import os
import sys
from typing import List, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools/extract"))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from vec_to_png import (  # noqa: E402
    DECLARED_LEN,
    PLANAR_OFFSET,
    STREAM,
    W,
    H,
    decode_vec_to_framebuffer,
    embedded_palette,
)
from vec_render import render_planar, write_png  # noqa: E402
from vec_diff import read_png  # noqa: E402


Pal = List[Tuple[int, int, int]]


def extract_arena(vec_path: str, stream: int = STREAM,
                  declared_len: int = DECLARED_LEN) -> Tuple[bytes, int, int]:
    """Run op4 -> Op12.vec_run on *vec_path* and return (arena_bytes, base, length).

    arena_bytes is mem[stream : stream + declared_len] — the full decoded-image
    region.  base is the stream linear address; length is declared_len.
    Print the arena base/length so the caller knows the linear range.
    """
    vec_bytes: bytes = open(vec_path, "rb").read()
    mem, base = decode_vec_to_framebuffer(vec_bytes, stream=stream,
                                          declared_len=declared_len)
    arena: bytes = bytes(mem[base : base + declared_len])
    return arena, base, declared_len


def render_arena_to_rgb(arena: bytes, pal: Optional[Pal] = None) -> bytes:
    """De-plane the decode arena (a full-screen .VEC decoded buffer) to 8-bit RGB.

    Uses the palette embedded in the arena header (bytes 51..98) unless *pal*
    is supplied explicitly.
    """
    if pal is None:
        pal = embedded_palette(arena)
    return bytes(render_planar(arena, W, H, pal, "seq", PLANAR_OFFSET))


def compare_rgb(a: bytes, b: bytes, n_pixels: int) -> Tuple[int, int]:
    """Return (n_diff_pixels, max_channel_delta) between two 8-bit RGB byte strings."""
    n_diff: int = 0
    max_delta: int = 0
    for i in range(n_pixels):
        o: int = i * 3
        delta: int = max(abs(a[o] - b[o]),
                        abs(a[o + 1] - b[o + 1]),
                        abs(a[o + 2] - b[o + 2]))
        if delta > 0:
            n_diff += 1
            if delta > max_delta:
                max_delta = delta
    return n_diff, max_delta


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vec", metavar="file.VEC", help="Input .VEC file")
    ap.add_argument("--out", metavar="file.ARENA",
                    help="Output arena file (default: <stem>.ARENA in same dir as .VEC)")
    ap.add_argument("--png", metavar="file.png",
                    help="Render the de-planed framebuffer to this PNG")
    ap.add_argument("--oracle", metavar="oracle.png",
                    help="Oracle PNG to compare against (prints pixel-diff verdict; "
                         "requires --png)")
    args = ap.parse_args()

    vec_path: str = os.path.abspath(args.vec)
    if not os.path.exists(vec_path):
        print("ERROR: %s not found" % vec_path, file=sys.stderr)
        sys.exit(1)

    stem: str = os.path.splitext(os.path.basename(vec_path))[0].lower()
    arena_out: str = (args.out if args.out else
                      os.path.join(os.path.dirname(vec_path), stem + ".ARENA"))

    arena, base, length = extract_arena(vec_path)
    with open(arena_out, "wb") as f:
        f.write(arena)
    print("arena base=0x%x length=0x%x (%d bytes)" % (base, length, length))
    print("wrote %s" % arena_out)

    if args.png:
        rgb: bytes = render_arena_to_rgb(arena)
        write_png(args.png, W, H, rgb)
        print("wrote framebuffer PNG -> %s" % args.png)

        if args.oracle:
            _ow: int
            _oh: int
            oracle_rgb: bytes
            _ow, _oh, oracle_rgb = read_png(args.oracle)
            n_diff, max_delta = compare_rgb(rgb, bytes(oracle_rgb), W * H)
            if n_diff == 0:
                print("oracle MATCH exact (%dx%d) vs %s" % (W, H, args.oracle))
            else:
                print("oracle DIFF pixels=%d/%d maxchanneldelta=%d vs %s"
                      % (n_diff, W * H, max_delta, args.oracle))
                sys.exit(1)
    elif args.oracle:
        print("WARNING: --oracle requires --png; skipping oracle comparison",
              file=sys.stderr)


if __name__ == "__main__":
    main()
