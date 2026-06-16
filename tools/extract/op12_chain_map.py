"""Ground-truth chain map for level 1, traced from the real renderer (dosemu.py
DOSEMU_TRACE_OP12). Drives the flat-buffer decoder replay (work in progress).
Each op4 loads a file to `dest`; each op12 pass expands [pixels@pix][mask@msk] of
length `n` -> `dst` on the shared flat heap. Addresses are runtime-linear."""

OP4_LOADS = [   # (file, dest, buffer_end)
    ("D1.PAV",     0x472d0, 0x4ead6),
    ("D1.DEC",     0x64750, 0x676e6),
    ("D1.BUM",     0x6f960, 0x704c0),
    ("MONDE1.VEC", 0x67bf0, 0x6f953),
]
# op12 passes: (call, pix, msk, dst, out_len)
OP12_PASSES = [
    (1, 0x4ac64, 0x4e266, 0x472d0, 17260),
    (2, 0x4a774, 0x4dbd2, 0x472d0, 30726),
    (3, 0x65d04, 0x67386, 0x64750, 6891),
    (4, 0x65c04, 0x670f2, 0x64751, 12182),
    (5, 0x7015e, 0x7044c, 0x6f960, 919),
    (6, 0x7012e, 0x70354, 0x6f960, 2912),
    (7, 0x6b140, 0x6ecea, 0x67bf2, 25388),
    (8, 0x69630, 0x6e9a2, 0x67bf0, 32099),
]
FRAMEBUFFER = 0x67bf0   # screen base; [:32000] = 320x200 planar after the chain
