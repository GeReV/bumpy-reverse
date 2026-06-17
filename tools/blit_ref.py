#!/usr/bin/env python3
"""Faithful Python reference of sprite_blit_planar_vga (1cec:10e1), validated
byte-exact against blit_oracle.bin (engine plane capture).

Algorithm (reconstructed from raw disasm; see the 5b design doc):
  Per row: carry accumulators bx,cx = 0 (cleared in setup 0x210b unless clip
  preload).  Per column: read 4 source bytes = plane0..3 as ax=(p1<<8|p0),
  dx=(p3<<8|p2); ror by `shift`; keep current-column bits with di=(0xff>>shift)
  in both bytes; OR into carry bx,cx; bit mask = OR of all 4 result bytes;
  write planes 0..3 (bl,bh,cl,ch) under GC bit mask (RMW: (val&bm)|(old&~bm)),
  map-mask selecting one plane each.  Then carry-out: bx,cx = saved post-ror
  values, xchg bytes, AND with ~di.  A final spill column writes the carry.
"""
import struct
import sys

PLANE = 0x10000


def ror16(v: int, n: int) -> int:
    n &= 15
    return ((v >> n) | (v << (16 - n))) & 0xFFFF


def load_oracle(path: str):
    b = open(path, "rb").read()
    assert b[:4] == b"BLT2", "expected BLT2 (before+after) format"
    n = struct.unpack_from("<H", b, 4)[0]
    o = 6
    caps = []
    for _ in range(n):
        ds, si = struct.unpack_from("<HH", b, o); o += 4
        desc = b[o:o + 0x20]; o += 0x20
        src_lin, src_len = struct.unpack_from("<II", b, o); o += 8
        src = b[o:o + src_len]; o += src_len
        plen = struct.unpack_from("<I", b, o)[0]; o += 4
        before = b[o:o + plen]; o += plen
        after = b[o:o + plen]; o += plen
        caps.append(dict(desc=desc, src=src, before=before, after=after))
    return caps


def blit(planes, desc, src, cols, src_stride):
    """Apply the planar blit into `planes` (list of 4 bytearrays). Returns planes."""
    dst_off, dst_seg = struct.unpack_from("<HH", desc, 8)
    voff = (dst_seg * 16 + dst_off - 0xA0000) & 0xFFFFF
    dst_stride = struct.unpack_from("<H", desc, 0x0e)[0]
    rows = struct.unpack_from("<H", desc, 0x12)[0]
    shift = desc[0x16]
    di = (0xFF >> shift) * 0x0101
    ndi = (~di) & 0xFFFF
    for row in range(rows):
        bx = cx = 0
        s = row * src_stride
        d = voff + row * dst_stride
        for col in range(cols + 1):
            if col < cols:
                p0, p1, p2, p3 = src[s], src[s + 1], src[s + 2], src[s + 3]
                s += 4
                ax = ror16((p1 << 8) | p0, shift)
                dx = ror16((p3 << 8) | p2, shift)
                sav_ax, sav_dx = ax, dx
                bx |= ax & di
                cx |= dx & di
            both = bx | cx
            bm = ((both >> 8) | both) & 0xFF
            vals = (bx & 0xFF, (bx >> 8) & 0xFF, cx & 0xFF, (cx >> 8) & 0xFF)
            for p in range(4):
                old = planes[p][d]
                planes[p][d] = (vals[p] & bm) | (old & ~bm)
            d += 1
            if col < cols:
                bx = (((sav_ax << 8) | (sav_ax >> 8)) & 0xFFFF) & ndi
                cx = (((sav_dx << 8) | (sav_dx >> 8)) & 0xFFFF) & ndi
            else:
                bx = cx = 0
    return planes


def try_blit(cap, cols, src_stride):
    before = [bytearray(cap["before"][p * PLANE:(p + 1) * PLANE]) for p in range(4)]
    after = [cap["after"][p * PLANE:(p + 1) * PLANE] for p in range(4)]
    got = blit(before, cap["desc"], cap["src"], cols, src_stride)
    return sum(1 for p in range(4) for o in range(PLANE) if got[p][o] != after[p][o])


def main() -> None:
    path = sys.argv[1] if len(sys.argv) > 1 else "local/build/render/blit_oracle.bin"
    caps = load_oracle(path)
    # de-dup identical (desc,src) captures for a compact report
    seen = set()
    uniq = []
    for c in caps:
        k = (c["desc"], c["src"][:64])
        if k not in seen:
            seen.add(k); uniq.append(c)
    print(f"{len(caps)} snapshots ({len(uniq)} distinct desc/src)")
    ok = 0
    fixed_stride = int(sys.argv[2]) if len(sys.argv) > 2 else None
    for i, cap in enumerate(uniq):
        d = cap["desc"]
        cols = struct.unpack_from("<H", d, 0x10)[0]
        full_w = struct.unpack_from("<H", d, 0x0c)[0]   # stored frame width in columns
        shift = d[0x16]
        sel = d[0x15]
        # src row pitch = 4 * desc[0x0c] (the stored full width); confirm this single
        # descriptor-derived formula, else fall back to a search.
        cands = [fixed_stride] if fixed_stride else [4 * full_w, 4 * (cols + 1), 4 * cols]
        best = None
        for st in cands:
            if st is None or st <= 0:
                continue
            if try_blit(cap, cols, st) == 0:
                best = st; break
        if best is not None:
            ok += 1
            print(f"  blit {i}: MATCH  cols={cols} sel={sel} shift={shift} src_stride={best}"
                  f"  (={best/ (cols or 1):.2f}*cols)")
        else:
            d0 = try_blit(cap, cols, 4 * (cols + 1))
            print(f"  blit {i}: DIFF   cols={cols} sel={sel} shift={shift} "
                  f"(best-guess stride 4*(cols+1) -> {d0} byte diffs)")
    print(f"{ok}/{len(uniq)} distinct blits byte-exact")


if __name__ == "__main__":
    main()
