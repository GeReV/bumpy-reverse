#!/usr/bin/env python3
"""Regenerate tools/extract/anim_tables.json — the layer-A/B entity tables used by
render_levels.py / export_levels.py — directly from the **unpacked game executable**.

No emulator needed. The data is static initialised data in the program's DGROUP (data
segment); only the entity descriptors are far-pointers, which we resolve from the MZ load
layout. Source chain:

    BUMPY.EXE  --tools/tinyprog_unpack.py-->  BUMPY_unpacked.exe  --(this script)-->  anim_tables.json

We locate DGROUP in the file by anchoring on the known posC table (the per-cell pixel
grid, cells 0..3 = (8,8)(48,8)(88,8)(128,8)), then read (offsets confirmed in Ghidra
`spawn_and_draw_level_entities` / `restore_bg_tile_run`; see docs/formats/LEVELS.md):

  posA @0x00f4, posB @0x03f4, posC @0x0274   -- per-cell blit pixel (x,y), `cell*4`-indexed
  layer A:  remap = byte[0x3d3a + code];  descriptor far-ptr = [0x3d6a + remap*4]
  layer B:  remap = byte[0x4086 + code];  descriptor far-ptr = [0x40a6 + remap*4]
            descriptor: word0 = signed y-offset, word1 = frame index
            A frame = word1;  B frame = word1 + 0xf1
  C_bias = 0x179

A descriptor far-ptr (off:seg) resolves to file offset  LMS + seg*16 + off  (LMS = the MZ
header size in paragraphs * 16), because the load module is laid out contiguously from the
file's load-module start and the descriptor seg words are link-time (un-relocated) in the
file. A code is kept only if its descriptor is real: not all-zero, sane signed y-offset
(-64..63), valid frame (0..511). Every code occurring in the 132 levels passes, so the
result drives render_levels identically. (A handful of unused high codes' descriptors are
patched by the running game; those are not in any level, so it makes no difference.)
"""
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_EXE = os.path.join(ROOT, "local/originals/unpacked/BUMPY_unpacked.exe")
OUT = os.path.join(ROOT, "tools/extract/anim_tables.json")

POS_A, POS_B, POS_C = 0x00f4, 0x03f4, 0x0274
A_REMAP, A_DESC = 0x3d3a, 0x3d6a
B_REMAP, B_DESC = 0x4086, 0x40a6
B_FRAME_BIAS = 0xf1
C_BIAS = 0x179
NUM_FRAMES = 512
# posC cells 0..3, used to anchor DGROUP's file offset (DGROUP+0x274).
POSC_ANCHOR = struct.pack("<8H", 8, 8, 48, 8, 88, 8, 128, 8)


def build(exe: bytes) -> dict:
    lms = struct.unpack_from("<H", exe, 8)[0] * 16        # MZ load-module start
    a = exe.find(POSC_ANCHOR)
    if a < 0:
        raise SystemExit("posC anchor not found — is this the unpacked BUMPY executable?")
    dgfile = a - POS_C                                    # DGROUP data at this file offset

    def e16(fo: int) -> int:
        return exe[fo] | (exe[fo + 1] << 8)

    def dg(off: int) -> int:
        return e16(dgfile + off)

    def postable(base: int) -> list[list[int]]:
        return [[dg(base + c * 4), dg(base + c * 4 + 2)] for c in range(48)]

    def codemap(remap_base: int, desc_base: int, frame_bias: int) -> dict[str, dict[str, int]]:
        out: dict[str, dict[str, int]] = {}
        for code in range(1, 64):
            remap = exe[dgfile + remap_base + code]
            if remap == 0:
                continue
            off = dg(desc_base + remap * 4)
            seg = dg(desc_base + remap * 4 + 2)
            tf = lms + seg * 16 + off                      # far-ptr -> file offset
            if tf < 0 or tf + 4 > len(exe):                # target outside the file
                continue
            yoff = e16(tf)
            if yoff >= 0x8000:                             # signed y-offset
                yoff -= 0x10000
            raw = e16(tf + 2)
            frame = raw + frame_bias
            if (raw == 0 and yoff == 0) or not (-64 <= yoff < 64) or not (0 <= frame < NUM_FRAMES):
                continue
            out[str(code)] = {"yoff": yoff, "frame": frame}
        return out

    return {
        "posA": postable(POS_A),
        "posB": postable(POS_B),
        "posC": postable(POS_C),
        "A": codemap(A_REMAP, A_DESC, 0),
        "B": codemap(B_REMAP, B_DESC, B_FRAME_BIAS),
        "C_bias": C_BIAS,
    }


def main() -> None:
    exe_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE
    if not os.path.exists(exe_path):
        raise SystemExit("missing %s — unpack BUMPY.EXE with tools/tinyprog_unpack.py first"
                         % os.path.relpath(exe_path, ROOT))
    tables = build(open(exe_path, "rb").read())
    with open(OUT, "w") as f:
        json.dump(tables, f, indent=1)
    print("wrote %s from %s  (posA/B/C 48 each; A=%d B=%d codes; C_bias=%#x)" % (
        os.path.relpath(OUT, ROOT), os.path.relpath(exe_path, ROOT),
        len(tables["A"]), len(tables["B"]), C_BIAS))


if __name__ == "__main__":
    main()
