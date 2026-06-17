#!/usr/bin/env python3
"""Inspect bg_oracle.bin (BG02): per cell, show the map tile id(s), the plane-delta
bounding box, and a few changed bytes — to reconstruct the tile blit (atlas -> screen).
Plane snapshot = 4 * 0x10000 (plane0..3 concatenated)."""
import struct
import sys

PLANE = 0x10000


def load(path):
    b = open(path, "rb").read()
    assert b[:4] == b"BG02", b[:4]
    n = struct.unpack_from("<H", b, 4)[0]
    o = 6
    atlas_lin, atlas_len = struct.unpack_from("<II", b, o); o += 8
    atlas = b[o:o + atlas_len]; o += atlas_len
    map_lin, map_len = struct.unpack_from("<II", b, o); o += 8
    bmap = b[o:o + map_len]; o += map_len
    caps = []
    for _ in range(n):
        run_code, cx, cy, frame = struct.unpack_from("<HHHH", b, o); o += 8
        plen = struct.unpack_from("<I", b, o)[0]; o += 4
        planes = b[o:o + plen]; o += plen
        caps.append(dict(run_code=run_code, cx=cx, cy=cy, frame=frame, planes=planes))
    return dict(atlas=atlas, atlas_lin=atlas_lin, map=bmap, map_lin=map_lin, caps=caps)


def tile_ids(bmap, cx, cy, run_code):
    """map[cx*0x27 + (cy>>1)*3 + col + 0x20] per restore_bg_tile_run."""
    if run_code < 0xf1:
        cols = [0]
    else:
        col_count = (-run_code) & 0xff
        col_count -= 5
        cols = list(range(1, col_count))
    out = []
    base = cx * 0x27 + (cy >> 1) * 3 + 0x20
    for col in cols:
        code = bmap[base + col]
        out.append((col, code, code - 1))
    return out


def main():
    o = load(sys.argv[1] if len(sys.argv) > 1 else "local/build/render/bg_oracle.bin")
    caps = o["caps"]
    print(f"{len(caps)} cells; atlas {len(o['atlas'])}B @{o['atlas_lin']:#x}; map @{o['map_lin']:#x}")
    nshow = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    for i in range(min(nshow, len(caps) - 1)):
        a, b = caps[i], caps[i + 1]
        ids = tile_ids(o["map"], a["cx"], a["cy"], a["run_code"])
        print(f"\n=== cell {i}: cx={a['cx']} cy={a['cy']} run_code={a['run_code']:#x} "
              f"tiles={ids} ===")
        changed = []
        for p in range(4):
            pa = a["planes"][p * PLANE:(p + 1) * PLANE]
            pb = b["planes"][p * PLANE:(p + 1) * PLANE]
            for off in range(PLANE):
                if pa[off] != pb[off]:
                    changed.append((p, off, pa[off], pb[off]))
        if not changed:
            print("  (no change)")
            continue
        offs = sorted(set(x[1] for x in changed))
        print(f"  {len(changed)} byte-writes, offsets {offs[0]:#06x}..{offs[-1]:#06x}")
        # group by offset -> per-plane after values
        bycol = {}
        for p, off, ov, nv in changed:
            bycol.setdefault(off, [None] * 4)[p] = nv
        rows = sorted(bycol)
        # infer stride: consecutive offsets in same row are +1; row jump ~40
        for off in rows[:20]:
            pv = bycol[off]
            row = off // 40
            col = off % 40
            print(f"    off={off:#06x} (row={row} col={col}) "
                  + " ".join("--" if v is None else f"{v:02x}" for v in pv))


if __name__ == "__main__":
    main()
