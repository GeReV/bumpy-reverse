#!/usr/bin/env python3
"""Faithful Python reference of the background tile blit, validated against the
engine per-cell plane deltas (bg_oracle.bin BG02).

Reconstructed model (confirmed against the engine):
  - PAV atlas raster (buffer +6) is a 320x192 4-plane PLANE-SEQUENTIAL image:
    byte = raster[plane*7680 + row*40 + bcol], plane=0..3, row=0..191, bcol=0..39.
  - A cell's tile id = map[cx*0x27 + (cy>>1)*3 + col + 0x20] - 1 (col per run rule).
  - atlas_col = tile_id % 20, atlas_row = tile_id // 20.
  - Tile = 16x16: src plane p row (arow*16+ry) byte (acol*2+bx), bx in 0..1.
  - Dest: screen plane p byte (cy*8+ry)*40 + cx*2 + bx.
  - The blit writes 4 bytes/row [t0, t1, 0, 0] opaque (clears 2 bytes ahead).
"""
import struct
import sys

PLANE = 0x10000
PSZ = 40 * 192          # plane size in the atlas raster


def load(path):
    b = open(path, "rb").read()
    assert b[:4] == b"BG02", b[:4]
    n = struct.unpack_from("<H", b, 4)[0]
    o = 6
    al, alen = struct.unpack_from("<II", b, o); o += 8
    atlas = b[o:o + alen]; o += alen
    ml, mlen = struct.unpack_from("<II", b, o); o += 8
    bmap = b[o:o + mlen]; o += mlen
    caps = []
    for _ in range(n):
        rc, cx, cy, fr = struct.unpack_from("<HHHH", b, o); o += 8
        plen = struct.unpack_from("<I", b, o)[0]; o += 4
        planes = b[o:o + plen]; o += plen
        caps.append(dict(run_code=rc, cx=cx, cy=cy, frame=fr, planes=planes))
    # consecutive entries give before/after per cell
    for i in range(len(caps) - 1):
        caps[i]["before"] = caps[i]["planes"]
        caps[i]["after"] = caps[i + 1]["planes"]
    return dict(atlas=atlas[6:], map=bmap, caps=caps[:-1])


def cell_cols(bmap, cx, cy, run_code):
    if run_code < 0xf1:
        cols = [0]
    else:
        col_count = ((-run_code) & 0xff) - 5
        cols = list(range(1, col_count))
    base = cx * 0x27 + (cy >> 1) * 3 + 0x20
    return [(bmap[base + c], c) for c in cols]


def blit_cell(planes, atlas, bmap, cell, clear_ahead=True, mask_overlay=True):
    cx, cy, run_code = cell["cx"], cell["cy"], cell["run_code"]
    cols = cell_cols(bmap, cx, cy, run_code)
    for sub, (code, col) in enumerate(cols):
        tid = code - 1
        acol, arow = tid % 20, tid // 20
        masked = mask_overlay and sub > 0          # run-cell overlays are masked
        # last grid row (cy==24, engine descriptor +0x20==1) clips to the 200-row
        # screen: rows 200..207 are off-screen and not drawn.
        rows_drawn = 16
        if cy * 8 + 16 > 200:
            rows_drawn = 200 - cy * 8
        for ry in range(rows_drawn):
            srow = arow * 16 + ry
            drow = cy * 8 + ry
            for bx in range(2):
                doff = drow * 40 + cx * 2 + bx
                vals = [atlas[p * PSZ + srow * 40 + acol * 2 + bx] for p in range(4)]
                if masked:
                    # per-bit mask: a pixel is opaque where any plane bit is set
                    mask = vals[0] | vals[1] | vals[2] | vals[3]
                    for p in range(4):
                        planes[p][doff] = (vals[p] & mask) | (planes[p][doff] & (~mask & 0xff))
                else:
                    for p in range(4):
                        planes[p][doff] = vals[p]
            if clear_ahead and not masked:
                for bx in range(2, 4):
                    if cx * 2 + bx >= 40:           # clipped at the right screen edge
                        continue
                    doff = drow * 40 + cx * 2 + bx
                    for p in range(4):
                        planes[p][doff] = 0
    return planes


def main():
    o = load(sys.argv[1] if len(sys.argv) > 1 else "local/build/render/bg_oracle.bin")
    caps = o["caps"]
    # A cell's footprint spans cols [cx*2, cx*2+4) (tile + clear-ahead) over rows
    # [cy*8, cy*8+16). The clear-ahead clips at the right edge; row-last cells
    # (cx=19) wrap their snapshot delta into the next row, so compare in-region.
    ok = 0
    total = 0
    for i in range(len(caps)):
        c = caps[i]
        before = [bytearray(c["before"][p * PLANE:(p + 1) * PLANE]) for p in range(4)]
        after = [c["after"][p * PLANE:(p + 1) * PLANE] for p in range(4)]
        got = blit_cell(before, o["atlas"], o["map"], c)
        c0, c1 = c["cx"] * 2, min(c["cx"] * 2 + 4, 40)
        r0, r1 = c["cy"] * 8, c["cy"] * 8 + 16
        diffs = 0
        for p in range(4):
            for row in range(r0, r1):
                for col in range(c0, c1):
                    x = row * 40 + col
                    if got[p][x] != after[p][x]:
                        diffs += 1
        total += 1
        if diffs == 0:
            ok += 1
        elif i < 6:
            print(f"  cell {i} (cx={c['cx']} cy={c['cy']} rc={c['run_code']:#x}): "
                  f"{diffs} in-region diffs")
    print(f"{ok}/{total} cells byte-exact (in-region)")


if __name__ == "__main__":
    main()
