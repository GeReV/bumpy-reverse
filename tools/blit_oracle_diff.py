#!/usr/bin/env python3
"""Inspect blit_oracle.bin: per consecutive plane-snapshot pair, report the VGA
bytes the planar blitter changed (the sprite footprint), so the C port has a
byte-exact target. Plane snapshot = 4 * 0x10000 (plane0..3 concatenated)."""
import struct
import sys

PLANE = 0x10000


def load(path: str):
    b = open(path, "rb").read()
    assert b[:4] == b"BLT1", "bad magic"
    n = struct.unpack_from("<H", b, 4)[0]
    o = 6
    caps = []
    for _ in range(n):
        instr, ds, si = struct.unpack_from("<IHH", b, o); o += 8
        desc = b[o:o + 0x20]; o += 0x20
        src_lin, src_len = struct.unpack_from("<II", b, o); o += 8
        src = b[o:o + src_len]; o += src_len
        plen = struct.unpack_from("<I", b, o)[0]; o += 4
        planes = b[o:o + plen]; o += plen
        caps.append(dict(instr=instr, ds=ds, si=si, desc=desc,
                         src_lin=src_lin, src=src, planes=planes))
    return caps


def main() -> None:
    path = sys.argv[1] if len(sys.argv) > 1 else "local/build/render/blit_oracle.bin"
    caps = load(path)
    print(f"{len(caps)} snapshots")
    for i in range(len(caps) - 1):
        a, b = caps[i], caps[i + 1]
        d = a["desc"]
        dst_off, dst_seg = struct.unpack_from("<HH", d, 8)
        voff = (dst_seg * 16 + dst_off - 0xA0000) & 0xFFFFF
        w16, rows = struct.unpack_from("<HH", d, 0x10)
        print(f"\n=== sprite {i}: dst voff={voff:#06x} w={w16} rows={rows} "
              f"sel={d[0x15]:#x} shift={d[0x16]} clip={d[0x17]:#x} ===")
        changed = []
        for p in range(4):
            pa = a["planes"][p * PLANE:(p + 1) * PLANE]
            pb = b["planes"][p * PLANE:(p + 1) * PLANE]
            for off in range(PLANE):
                if pa[off] != pb[off]:
                    changed.append((p, off, pa[off], pb[off]))
        if not changed:
            print("  (no change — not back-to-back?)")
            continue
        offs = sorted(set(o for _, o, _, _ in changed))
        print(f"  {len(changed)} byte-writes across offsets {offs[0]:#x}..{offs[-1]:#x}")
        # group by offset, show per-plane after values
        bycol = {}
        for p, off, ov, nv in changed:
            bycol.setdefault(off, [None, None, None, None])[p] = nv
        for off in sorted(bycol):
            row = (off - voff) // 40 if voff <= off else -1
            col = (off - voff) % 40 if voff <= off else -1
            pv = bycol[off]
            print(f"    off={off:#06x} (row={row:+d} col={col:+d}) planes="
                  + " ".join("--" if v is None else f"{v:02x}" for v in pv))
        # source bytes the blit consumed (first 4*rows*2 plausible)
        print(f"  src_lin={a['src_lin']:#07x} first32={a['src'][:32].hex()}")


if __name__ == "__main__":
    main()
