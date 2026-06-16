#\!/usr/bin/env python3
"""Sweep layouts/widths/bases over op12's plot buffer (0x47000-0x4e000) to find
the playfield raster the vector interpreter drew."""
from __future__ import annotations
import subprocess
from typing import List
from vec_render import render_planar, write_png, EGA

DUMP = "local/build/render/dosemu_ram.bin"
BASE = 0x10000
buf = open(DUMP, "rb").read()
outs: List[str] = []
for base in (0x47000, 0x48000, 0x49000, 0x4a000):
    off = base - BASE
    for w in (160, 256, 320):
        for h in (192, 200):
            for layout in ("seq", "rowint", "byteint"):
                sub = buf[off:off + w * h // 2 + 16]
                if len(sub) < w * h // 2:
                    continue
                rgb = render_planar(sub, w, h, EGA, layout, 0)
                out = "local/build/render/play_%05x_%dx%d_%s.png" % (base, w, h, layout)
                write_png(out, w, h, rgb)
                outs.append(out)
subprocess.run(["montage"] + outs + ["-tile", "6x", "-geometry", "160x120+1+1",
                "-label", "%f", "local/build/render/play_sweep.png"])
print("wrote build/render/play_sweep.png (%d)" % len(outs))
