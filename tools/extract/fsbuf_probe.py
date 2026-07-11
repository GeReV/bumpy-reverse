#!/usr/bin/env python3
"""Identify the fullscreen_buf layout in the FRM4 oracle and compare it to a000
(the displayed bg+sprites) — to confirm it is the engine's CLEAN background
reference (a000 with sprite regions replaced by bg)."""
import importlib.util
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location("cc", os.path.join(ROOT, "tools/composite_check.py"))
cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cc)

PLANE = 0x10000
FP = 8000


def a000_pixels(planes: bytes) -> bytearray:
    out = bytearray(320 * 200)
    for y in range(200):
        for xb in range(40):
            o = y * 40 + xb
            b = (planes[o], planes[PLANE + o], planes[2 * PLANE + o], planes[3 * PLANE + o])
            for bit in range(8):
                m = 1 << (7 - bit)
                v = sum((1 << p) for p in range(4) if b[p] & m)
                out[y * 320 + xb * 8 + bit] = v
    return out


def fsb_planar(fsb: bytes) -> bytearray:
    out = bytearray(320 * 200)
    for y in range(200):
        for xb in range(40):
            o = y * 40 + xb
            b = (fsb[o], fsb[FP + o], fsb[2 * FP + o], fsb[3 * FP + o])
            for bit in range(8):
                m = 1 << (7 - bit)
                v = sum((1 << p) for p in range(4) if b[p] & m)
                out[y * 320 + xb * 8 + bit] = v
    return out


def fsb_packed(fsb: bytes) -> bytearray:
    """4bpp packed linear: 2 px/byte, hi nibble first."""
    out = bytearray(320 * 200)
    for i in range(320 * 200 // 2):
        byte = fsb[i]
        out[i * 2] = (byte >> 4) & 0xF
        out[i * 2 + 1] = byte & 0xF
    return out


def score(a: bytearray, b: bytearray) -> int:
    return sum(1 for i in range(len(a)) if a[i] == b[i])


def main() -> None:
    d = cc.load_frame4(os.path.join(ROOT, "local/build/render/frame_oracle.bin"))
    a000 = a000_pixels(d["planes"])
    fsb = d.get("fullscreen_buf") or b""
    print("fsbuf len=%d (planar needs %d, packed needs %d)" % (len(fsb), 4 * FP, 320 * 200 // 2))
    N = 320 * 200
    for name, dec in (("planar", fsb_planar), ("packed", fsb_packed)):
        if (name == "planar" and len(fsb) >= 4 * FP) or (name == "packed" and len(fsb) >= N // 2):
            px = dec(fsb)
            m = score(px, a000)
            nz = sum(1 for v in px if v)
            print("  %-7s vs a000: match=%d/%d (%.1f%%)  nonzero=%d" % (name, m, N, 100.0 * m / N, nz))


if __name__ == "__main__":
    main()
