"""Render a Bumpy .CAR font to a PNG sheet (pure Python). Glyph format (DDFNT2.CAR):
header [first_char, last, cell_w, cell_h]; BE16 offset table from offset 4 up to the
first offset; table[0] is a phantom entry, so char C -> table index (C - first_char + 1).
Each glyph = [width][height][00][height rows of 8px bitmap, MSB-first][2-byte trailer]."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
os.chdir('/home/amirg/fable5-retro-greenfield')
sys.path.insert(0, 'tools/render')
from vec_render import write_png


def load_car(path):
    b = open(path, 'rb').read()
    first = b[0]
    offs = []
    o = 4
    while o + 2 <= len(b):
        v = (b[o] << 8) | b[o+1]
        if offs and o >= offs[0]:
            break
        offs.append(v); o += 2
    bounds = offs + [len(b)]
    return b, first, offs, bounds


def glyph_bitmap(b, bounds, idx):
    g = b[bounds[idx]:bounds[idx+1]]
    if len(g) < 3:
        return 0, 0, []
    w, h = g[0], g[1]
    rows = [g[3 + r] if 3 + r < len(g) else 0 for r in range(h)]
    return w, h, rows


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'local/build/capture/game/DDFNT2.CAR'
    out = sys.argv[2] if len(sys.argv) > 2 else 'local/results/images/ddfnt2_sheet.png'
    b, first, offs, bounds = load_car(path)
    print("%s: first_char=%#x glyphs=%d" % (os.path.basename(path), first, len(offs)))
    chars = list(range(0x20, 0x80))      # printable ASCII
    cols, cell = 16, 12
    rows_n = (len(chars) + cols - 1) // cols
    W, H = cols * cell, rows_n * cell
    img = bytearray(W * H * 3)            # black bg
    for n, ch in enumerate(chars):
        idx = ch - first + 1             # phantom table[0]
        if not (0 <= idx < len(offs)):
            continue
        w, h, rows = glyph_bitmap(b, bounds, idx)
        cx, cy = (n % cols) * cell, (n // cols) * cell
        for r, byte in enumerate(rows):
            for c in range(min(w, 8)):
                if (byte >> (7 - c)) & 1:
                    px, py = cx + c, cy + r
                    o = (py * W + px) * 3
                    img[o] = img[o+1] = img[o+2] = 255
    os.makedirs(os.path.dirname(out), exist_ok=True)
    write_png(out, W, H, bytes(img))
    print("wrote", out, "(%dx%d)" % (W, H))


if __name__ == "__main__":
    main()
