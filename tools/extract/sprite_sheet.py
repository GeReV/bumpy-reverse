#!/usr/bin/env python3
"""Decode all BUMSPJEU.BIN sprite frames to a PNG sheet (pure Python).

Format (raw frames, ctrl & 0x40 == 0): per row, `w` BE16 words = w/4 blocks of
16px; each block = 4 colour-plane words [p0,p1,p2,p3] (MSB = left); width = w*4
px, h rows. Index 0 = transparent. The frames are drawn over the world map, so
they are coloured with MONDE1.VEC's embedded 16-colour palette (via vec_to_png).
See docs/formats/BIN.md.
"""
from __future__ import annotations
import os
import struct
import sys
from typing import List, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools/extract"))
import vec_to_png as V          # noqa: E402
from vec_render import write_png  # noqa: E402

GAME = os.path.join(ROOT, "local/build/capture/game")
DATA = 0x800                    # data base; the frame-offset table is DATA//4 BE32 entries (relative to DATA)


def be16(b: bytes, o: int) -> int:
    return struct.unpack(">H", b[o:o + 2])[0]


def be32(b: bytes, o: int) -> int:
    return struct.unpack(">I", b[o:o + 4])[0]


def decode_frame(b: bytes, fp: int, w: int, h: int) -> Tuple[List[List[int]], int]:
    """4-plane interleaved-16px-blocks -> (rows of palette indices, width_px). 0 = transparent."""
    blocks = w // 4
    img: List[List[int]] = []
    for r in range(h):
        words = [be16(b, fp + r * w * 2 + 2 * i) for i in range(w)]
        row: List[int] = []
        for blk in range(blocks):
            planes = words[blk * 4:blk * 4 + 4]
            for col in range(16):
                row.append(sum(((planes[p] >> (15 - col)) & 1) << p for p in range(4)))
        img.append(row)
    return img, blocks * 16


def raw_frames(b: bytes) -> List[Tuple[int, int, int, int]]:
    """Enumerate decodable raw frames as (table_index, pixel_ptr, w_words, h).

    The offset table has DATA//4 BE32 slots; a 0 entry is an unused/terminator slot.
    BUMSPJEU.BIN holds no mask-RLE (ctrl & 0x40) frames — those are skipped with a
    warning if ever encountered. Implausible dimensions are filtered out.
    """
    n = len(b)
    out: List[Tuple[int, int, int, int]] = []
    for i in range(DATA // 4):
        v = be32(b, i * 4)
        if v == 0:
            continue
        fp = DATA + v
        if not (DATA <= fp < n):
            break
        w, h, ctrl = be16(b, fp - 4), be16(b, fp - 2), b[fp - 10]
        if ctrl & 0x40:
            print("  unexpected mask-RLE frame idx=%d w=%d h=%d ctrl=%#x" % (i, w, h, ctrl))
            continue
        if w == 0 or h == 0 or w % 4 or w > 16 or h > 48 or w * 4 > 64:
            continue
        if fp + w * 2 * h > n:
            continue
        out.append((i, fp, w, h))
    return out


def main() -> None:
    b = open(os.path.join(GAME, "BUMSPJEU.BIN"), "rb").read()
    # Palette: decode MONDE1.VEC and read its embedded 16-colour palette.
    mem, stream = V.decode_vec_to_framebuffer(open(os.path.join(GAME, "MONDE1.VEC"), "rb").read())
    pal = V.embedded_palette(bytes(mem[stream:stream + V.DECLARED_LEN]))

    frames = raw_frames(b)
    print("decodable raw frames: %d / %d real (table has %d slots, last is terminator)"
          % (len(frames), DATA // 4 - 1, DATA // 4))

    CELL, COLS = 40, 16
    rows_n = (len(frames) + COLS - 1) // COLS
    W, H = COLS * CELL, rows_n * CELL
    img = bytearray(W * H * 3)                       # black background
    for n, (i, fp, w, h) in enumerate(frames):
        grid, wpx = decode_frame(b, fp, w, h)
        cx, cy = (n % COLS) * CELL, (n // COLS) * CELL
        for r in range(min(h, CELL)):
            for c in range(min(wpx, CELL)):
                v = grid[r][c]
                if v:
                    o = ((cy + r) * W + cx + c) * 3
                    img[o:o + 3] = pal[v]

    out = os.path.join(ROOT, "local/results/sprites/bumspjeu_sheet.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    write_png(out, W, H, bytes(img))
    print("wrote %s (%dx%d, %d frames)" % (os.path.relpath(out, ROOT), W, H, len(frames)))


if __name__ == "__main__":
    main()
