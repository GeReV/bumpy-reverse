#!/usr/bin/env python3
"""Standalone, ZERO-EMULATOR decoder: a Bumpy full-screen .VEC (MONDE*.VEC world
maps / TITRE etc.) -> PNG, entirely in pure Python — palette and all.

Pipeline (all byte-exact pure-Python ports, validated against the vec_cpu oracle):
    raw .VEC file  ->  op4 (RLE decompress)  ->  vec_run record loop  ->  op12
                   ->  decoded full-screen image (size 0x7d63 = 32099):
                         [99-byte header | 320x200 4-plane planar @ offset 99]
                         the 16-colour palette is 16 x 6-bit RGB triples @ offset 51
                   ->  render planar with that embedded palette  ->  PNG

The palette is SELF-CONTAINED in the image (the game uploads it to the VGA DAC in
level_intro_screen). No roll/correction is applied — the old DX=168 "correct_view"
was a dosemu artifact of reading the planar at offset 0 instead of 99. Validated
99.95% pixel-exact vs a real DOSBox screenshot (results/oracle/world1_dosbox.png).
"""
from __future__ import annotations
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools/render"))
sys.path.insert(0, os.path.join(ROOT, "tools/extract"))
from op12_port import Op12, DG          # noqa: E402
from vec_render import render_planar, write_png  # noqa: E402

W, H = 320, 200
STREAM = 0x67bf0                 # arbitrary 16-aligned load base for the flat buffer
DECLARED_LEN = 0x7d63            # decoded size = 99-byte header + 32000 planar
PALETTE_OFFSET = 51             # 16 x 6-bit RGB triples in the header
PLANAR_OFFSET = 99             # 320x200 4-plane planar data starts here


def _e6(v):
    return ((v & 0x3F) << 2) | ((v & 0x3F) >> 4)


def embedded_palette(decoded):
    """The 16-colour palette embedded in a decoded full-screen .VEC (6-bit RGB)."""
    o = PALETTE_OFFSET
    return [(_e6(decoded[o + 3*k]), _e6(decoded[o + 3*k + 1]), _e6(decoded[o + 3*k + 2]))
            for k in range(16)]


def decode_vec_to_framebuffer(vec_bytes, stream=STREAM, declared_len=DECLARED_LEN):
    """Run the full op4 -> vec_run -> op12 chain on a flat buffer; return the
    (mem, stream) so the caller can read the 320x200 planar framebuffer at `stream`."""
    mem = bytearray(0xA0000)
    mem[stream:stream + len(vec_bytes)] = vec_bytes

    def sv(o, v):
        mem[DG + o] = v & 0xFF; mem[DG + o + 1] = (v >> 8) & 0xFF

    def setlin(off_o, seg_o, lin):
        sv(off_o, lin & 0xF); sv(seg_o, (lin >> 4) & 0xFFFF)

    setlin(0x4e0e, 0x4e10, stream)                    # vec_stream
    setlin(0x4e0a, 0x4e0c, stream + declared_len)     # vec_end (stream-end bound)
    vsav = len(vec_bytes)                             # op4 input end = file size
    sv(0x4e28, vsav & 0xFFFF); sv(0x4e2a, (vsav >> 16) & 0xFFFF)

    Op12(mem).vec_run(dispatch_current=False)
    return mem, stream


def decode_vec_to_png(vec_path, png_path, pal=None):
    vec = open(vec_path, "rb").read()
    mem, stream = decode_vec_to_framebuffer(vec)
    decoded = bytes(mem[stream:stream + DECLARED_LEN])
    if pal is None:
        pal = embedded_palette(decoded)
    rgb = render_planar(decoded, W, H, pal, "seq", PLANAR_OFFSET)
    write_png(png_path, W, H, rgb)
    return mem, stream


def main():
    args = sys.argv[1:]
    if not args:
        vec = os.path.join(ROOT, "local/build/capture/game/MONDE1.VEC")
        out = os.path.join(ROOT, "local/results/levels_png/world1.png")
    else:
        vec = args[0]
        out = args[1] if len(args) > 1 else os.path.splitext(vec)[0] + ".png"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    decode_vec_to_png(vec, out)
    print("wrote", out)


if __name__ == "__main__":
    main()
