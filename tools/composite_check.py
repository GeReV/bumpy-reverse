#!/usr/bin/env python3
"""Render the full background (bg_render_grid logic) from frame_oracle.bin's atlas+map
and diff it against the captured engine frame.  Confirms the bg matches the real
composite and isolates the entity pixels (where bg != frame = drawn sprites).
Writes a diff PNG: background dimmed, entity pixels in magenta."""
import struct
import sys
import importlib.util
from typing import Dict, List

spec = importlib.util.spec_from_file_location("bgref", "tools/bg_blit_ref.py")
bgref = importlib.util.module_from_spec(spec); spec.loader.exec_module(bgref)
PLANE = 0x10000


def load(path: str) -> tuple[bytes, bytes, bytes, bytes]:
    """Read an FRM2 or FRM3 file and return the bg-relevant fields.

    Accepts FRM3 (new) as well as the legacy FRM2 tag so the bg-diff path
    continues to work after the oracle is regenerated.

    Returns a 4-tuple (planes, dac, atlas, bmap) of bytes.
    """
    d = load_frame3(path)
    return d["planes"], d["dac"], d["atlas"], d["map"]


def load_frame3(path: str) -> Dict:
    """Parse an FRM3 oracle file into a dict with all blocks.

    Keys returned:
        tag (bytes)        — b"FRM3" or b"FRM2" (legacy; new-block fields are empty stubs)
        planes (bytes)     — 4 VGA planes, 0x10000 B each (total 0x40000 B)
        dac (bytes)        — 256*3 palette (0..63 per channel)
        atlas (bytes)      — PAV atlas raster (0x8000 B)
        map (bytes)        — level tile map (0x1000 B)
        level (int)        — 1-based level index
        bum (bytes)        — 0xc2 B BUM per-level header for this level
        p1_obj (bytes)     — 0x18 B p1 sprite obj struct (DGROUP:0x792e)
        p2_obj (bytes)     — 0x18 B p2 sprite obj struct (DGROUP:0x795a)
        p1_glob (bytes)    — 6 B: pixel_x(u16) pixel_y(u16) move_anim(u16)
        p2_glob (bytes)    — 6 B: pixel_x(u16) pixel_y(u16) move_anim(u16)
        chan_a (list)       — 3 x bytes(0xc): layer-A channel records
        chan_b (list)       — 4 x bytes(0xc): layer-B channel records
        chan_tbl_raw (bytes)— 8 B: 4 u16 far-ptr table words
                               (A_off, A_seg, B_off, B_seg)
        dg (bytes)         — 0x10000 B full DGROUP snapshot
    """
    with open(path, "rb") as fh:
        b = fh.read()
    tag = b[:4]
    assert tag in (b"FRM2", b"FRM3"), "unexpected tag: %r" % tag

    o = 4
    plen = struct.unpack_from("<I", b, o)[0]; o += 4
    planes = b[o:o + plen]; o += plen
    dac = b[o:o + 256 * 3]; o += 256 * 3
    alen = struct.unpack_from("<I", b, o)[0]; o += 4
    atlas = b[o:o + alen]; o += alen
    mlen = struct.unpack_from("<I", b, o)[0]; o += 4
    bmap = b[o:o + mlen]; o += mlen

    if tag == b"FRM2":
        # Legacy file: return stub values for new fields so callers can test gracefully.
        return dict(tag=tag, planes=planes, dac=dac, atlas=atlas, map=bmap,
                    level=0, bum=b"", p1_obj=b"", p2_obj=b"",
                    p1_glob=b"", p2_glob=b"",
                    chan_a=[], chan_b=[],
                    chan_tbl_raw=b"", dg=b"")

    # FRM3 new blocks
    assert tag == b"FRM3"
    level = struct.unpack_from("<H", b, o)[0]; o += 2
    bum = b[o:o + 0xc2]; o += 0xc2
    p1_obj = b[o:o + 0x18]; o += 0x18
    p2_obj = b[o:o + 0x18]; o += 0x18
    p1_glob = b[o:o + 6]; o += 6
    p2_glob = b[o:o + 6]; o += 6
    # Channel records: layer-A (3 × 0xc), layer-B (4 × 0xc)
    chan_a: List[bytes] = []
    for _ in range(3):
        chan_a.append(b[o:o + 0xc]); o += 0xc
    chan_b: List[bytes] = []
    for _ in range(4):
        chan_b.append(b[o:o + 0xc]); o += 0xc
    chan_tbl_raw = b[o:o + 8]; o += 8
    dg = b[o:o + 0x10000]; o += 0x10000

    return dict(tag=tag, planes=planes, dac=dac, atlas=atlas, map=bmap,
                level=level, bum=bum, p1_obj=p1_obj, p2_obj=p2_obj,
                p1_glob=p1_glob, p2_glob=p2_glob,
                chan_a=chan_a, chan_b=chan_b,
                chan_tbl_raw=chan_tbl_raw, dg=dg)


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
