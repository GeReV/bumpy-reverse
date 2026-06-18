#!/usr/bin/env python3
"""Render the full background (bg_render_grid logic) from frame_oracle.bin's atlas+map
and diff it against the captured engine frame.  Confirms the bg matches the real
composite and isolates the entity pixels (where bg != frame = drawn sprites).
Writes a diff PNG: background dimmed, entity pixels in magenta."""
import struct
import sys
import importlib.util

spec = importlib.util.spec_from_file_location("bgref", "tools/bg_blit_ref.py")
bgref = importlib.util.module_from_spec(spec); spec.loader.exec_module(bgref)
PLANE = 0x10000


def load(path):
    b = open(path, "rb").read()
    assert b[:4] == b"FRM2", b[:4]
    plen = struct.unpack_from("<I", b, 4)[0]; o = 8
    planes = b[o:o + plen]; o += plen
    dac = b[o:o + 256 * 3]; o += 256 * 3
    alen = struct.unpack_from("<I", b, o)[0]; o += 4
    atlas = b[o:o + alen]; o += alen
    mlen = struct.unpack_from("<I", b, o)[0]; o += 4
    bmap = b[o:o + mlen]; o += mlen
    return planes, dac, atlas, bmap


def idx_at(planes, x, y):
    off = y * 40 + x // 8
    m = 0x80 >> (x & 7)
    return (((planes[off] & m) and 1) | (((planes[PLANE + off] & m) and 1) << 1)
            | (((planes[2 * PLANE + off] & m) and 1) << 2)
            | (((planes[3 * PLANE + off] & m) and 1) << 3))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "local/build/render/frame_oracle.bin"
    planes, dac, atlas, bmap = load(src)
    raster = atlas[6:]
    # render the full bg grid into a fresh plane buffer
    bg = [bytearray(PLANE) for _ in range(4)]
    for cy in range(0, 25, 2):
        for cx in range(20):
            rc = bmap[cx * 0x27 + (cy >> 1) * 3 + 0x20]
            bgref.blit_cell(bg, raster, bmap, dict(cx=cx, cy=cy, run_code=rc))
    bgflat = b"".join(bytes(p) for p in bg)

    # compare bg vs frame, per pixel (within the 320x200 playfield)
    W, H = 320, 200
    match = diff = 0
    for y in range(H):
        for x in range(W):
            if idx_at(bgflat, x, y) == idx_at(planes, x, y):
                match += 1
            else:
                diff += 1
    tot = W * H
    print(f"bg vs frame: {match}/{tot} pixels match ({100*match/tot:.1f}%), "
          f"{diff} differ (entities/overlays)")

    # diff PNG: bg dimmed, entity pixels magenta
    pal = [(min(255, dac[i*3]*255//63), min(255, dac[i*3+1]*255//63),
            min(255, dac[i*3+2]*255//63)) for i in range(256)]
    rgb = bytearray(W * H * 3)
    for y in range(H):
        for x in range(W):
            fi = idx_at(planes, x, y)
            if idx_at(bgflat, x, y) == fi:
                r, g, b = pal[fi]; r//=3; g//=3; b//=3
            else:
                r, g, b = 255, 0, 255
            px = (y*W+x)*3; rgb[px]=r; rgb[px+1]=g; rgb[px+2]=b
    out = "local/build/render/composite_diff.png"
    frspec = importlib.util.spec_from_file_location("fr", "tools/frame_render.py")
    fr = importlib.util.module_from_spec(frspec); frspec.loader.exec_module(fr)
    fr.write_png(out, W, H, rgb)
    print(f"(entities highlighted magenta) -> {out}")


if __name__ == "__main__":
    main()
