#!/usr/bin/env python3
"""Extract a000 (displayed), a200 (scratch), and fullscreen_buf (save-under) from the
FRM4 gameplay oracle and compare — to determine the engine's present/erase model:
per-sprite save-under (sprites only in a000; a200/fsbuf hold clean bg) vs. copy-based
double-buffer (sprites in BOTH pages)."""
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location("cc", os.path.join(ROOT, "tools/composite_check.py"))
cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cc)
sys.path.insert(0, os.path.join(ROOT, "tools/extract"))
from vec_render import write_png  # noqa: E402

PLANE = 0x10000
FP = 8000


def page_pixels(planes: bytes, base: int) -> bytearray:
    out = bytearray(320 * 200)
    for y in range(200):
        for xb in range(40):
            o = base + y * 40 + xb
            b = (planes[o], planes[PLANE + o], planes[2 * PLANE + o], planes[3 * PLANE + o])
            for bit in range(8):
                m = 1 << (7 - bit)
                v = 0
                for p in range(4):
                    if b[p] & m:
                        v |= (1 << p)
                out[y * 320 + xb * 8 + bit] = v
    return out


def fsb_pixels(fsb: bytes) -> bytearray:
    out = bytearray(320 * 200)
    for y in range(200):
        for xb in range(40):
            o = y * 40 + xb
            b = (fsb[o], fsb[FP + o], fsb[2 * FP + o], fsb[3 * FP + o])
            for bit in range(8):
                m = 1 << (7 - bit)
                v = 0
                for p in range(4):
                    if b[p] & m:
                        v |= (1 << p)
                out[y * 320 + xb * 8 + bit] = v
    return out


# A fixed, high-contrast 16-color palette (visibility only; exact colours not needed).
PAL = [(0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170), (170, 0, 0), (170, 0, 170),
       (170, 85, 0), (170, 170, 170), (85, 85, 85), (85, 85, 255), (85, 255, 85),
       (85, 255, 255), (255, 85, 85), (255, 85, 255), (255, 255, 85), (255, 255, 255)]


def to_png(pix: bytearray, path: str) -> None:
    rgb = bytearray(320 * 200 * 3)
    for i, v in enumerate(pix):
        rgb[i * 3:i * 3 + 3] = bytes(PAL[v & 15])
    write_png(path, 320, 200, bytes(rgb))


def main() -> None:
    d = cc.load_frame4(os.path.join(ROOT, "local/build/render/frame_oracle.bin"))
    planes = d["planes"]
    fsb = d.get("fullscreen_buf") or b""
    print("tag=%r level=%s planes_len=0x%x fsbuf_len=%d" % (d["tag"], d.get("level"), len(planes), len(fsb)))

    a000 = page_pixels(planes, 0x0000)
    a200 = page_pixels(planes, 0x2000)
    fsbp = fsb_pixels(fsb) if len(fsb) >= 4 * FP else None

    diff = [i for i in range(320 * 200) if a000[i] != a200[i]]
    print("a000 vs a200: differing pixels = %d" % len(diff))
    if diff:
        xs = [i % 320 for i in diff]
        ys = [i // 320 for i in diff]
        x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
        print("  diff bbox: x[%d..%d] y[%d..%d]" % (x0, x1, y0, y1))
        a0nz = sum(1 for y in range(y0, y1 + 1) for x in range(x0, x1 + 1) if a000[y * 320 + x])
        a2nz = sum(1 for y in range(y0, y1 + 1) for x in range(x0, x1 + 1) if a200[y * 320 + x])
        print("  in diff bbox: a000 nonzero=%d  a200 nonzero=%d" % (a0nz, a2nz))

    nz = lambda p: sum(1 for v in p if v)
    print("nonzero pixels: a000=%d a200=%d fsbuf=%s" % (nz(a000), nz(a200), nz(fsbp) if fsbp else "n/a"))
    if fsbp is not None:
        d000 = sum(1 for i in range(320 * 200) if a000[i] != fsbp[i])
        d200 = sum(1 for i in range(320 * 200) if a200[i] != fsbp[i])
        print("fsbuf vs a000: %d differ ; fsbuf vs a200: %d differ" % (d000, d200))

    out = os.path.join(ROOT, "results/overlay-probe")
    os.makedirs(out, exist_ok=True)
    to_png(a000, os.path.join(out, "page_a000.png"))
    to_png(a200, os.path.join(out, "page_a200.png"))
    if fsbp is not None:
        to_png(fsbp, os.path.join(out, "page_fsbuf.png"))
    print("wrote page_a000.png / page_a200.png / page_fsbuf.png")


if __name__ == "__main__":
    main()
