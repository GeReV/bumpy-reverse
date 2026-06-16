#!/usr/bin/env python3
"""Scan the dosemu heap dump for a rendered playfield: render 320x192 (and x200)
4-plane EGA at a sweep of offsets and montage them so the level can be spotted."""
from __future__ import annotations
import subprocess
from typing import List
from vec_render import render_planar, write_png, EGA

DUMP = "local/build/render/dosemu_ram.bin"      # main RAM from 0x10000
BASE = 0x10000


def main() -> None:
    buf = open(DUMP, "rb").read()
    outs: List[str] = []
    # dense regions seen: 0x46000..0x6e000 linear -> dump offset (lin - BASE)
    for lin in range(0x44000, 0x70000, 0x2000):
        off = lin - BASE
        for (w, h) in ((320, 192), (320, 200)):
            sub = buf[off:off + w * h // 2 + 8]
            if len(sub) < w * h // 2:
                continue
            rgb = render_planar(sub, w, h, EGA, "seq", 0)
            out = "local/build/render/scan_%05x_%dx%d.png" % (lin, w, h)
            write_png(out, w, h, rgb)
            outs.append(out)
    # montage in a grid
    subprocess.run(["montage"] + outs + ["-tile", "6x", "-geometry", "160x100+1+1",
                    "-label", "%f", "local/build/render/scan_heap.png"])
    print("wrote build/render/scan_heap.png  (%d candidates)" % len(outs))


if __name__ == "__main__":
    main()
