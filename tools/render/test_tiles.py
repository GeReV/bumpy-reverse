"""Render a decoded world .VEC buffer as a sprite/tile sheet in several tile
sizes/encodings, to see whether it's a tile bank."""
from __future__ import annotations
import subprocess
import sys
from typing import List, Tuple
from vec_render import decode_vec, write_png, EGA


def tile_planar(buf: bytes, tw: int, th: int, cols: int, chunky: bool) -> Tuple[int, int, bytearray]:
    bytes_per_row = tw // 2 if chunky else (tw // 8) * 4
    tile_bytes = bytes_per_row * th
    ntiles = len(buf) // tile_bytes
    rows = (ntiles + cols - 1) // cols
    W, H = cols * tw, rows * th
    rgb = bytearray(W * H * 3)
    for t in range(ntiles):
        tx, ty = (t % cols) * tw, (t // cols) * th
        base = t * tile_bytes
        for y in range(th):
            for x in range(tw):
                if chunky:
                    b = buf[base + y * bytes_per_row + x // 2]
                    v = (b >> 4) if x % 2 == 0 else (b & 0xF)
                else:
                    v = 0
                    for p in range(4):
                        o = base + y * bytes_per_row + p * (tw // 8) + x // 8
                        if o < len(buf):
                            v |= ((buf[o] >> (7 - x % 8)) & 1) << p
                r, g, bl = EGA[v]
                px = (ty + y) * W + (tx + x)
                rgb[px * 3] = r; rgb[px * 3 + 1] = g; rgb[px * 3 + 2] = bl
    return W, H, rgb


def main() -> None:
    path = sys.argv[1]
    buf = decode_vec(path)
    name = path.split("/")[-1]
    outs: List[str] = []
    for chunky in (False, True):
        for tw, th in ((16, 16), (8, 8), (24, 16), (16, 24), (32, 32)):
            W, H, rgb = tile_planar(buf, tw, th, 20, chunky)
            tag = "%dx%d_%s" % (tw, th, "chunky" if chunky else "planar")
            out = "local/build/render/tiles_%s_%s.png" % (name, tag)
            write_png(out, W, H, rgb)
            outs.append(out)
    subprocess.run(["montage"] + outs + ["-tile", "5x2", "-geometry", "200x180+2+10",
                    "-label", "%t", "local/build/render/tiles_%s.png" % name])
    print("montage -> build/render/tiles_%s.png" % name)


if __name__ == "__main__":
    main()
