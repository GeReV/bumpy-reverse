"""Decode all BUMSPJEU.BIN sprite frames to a PNG sheet (pure Python).
Format (raw frames, ctrl & 0x40 == 0): per row, `w` BE16 words = w/4 blocks of 16px;
each block = 4 colour-plane words [p0,p1,p2,p3] (MSB=left); width = w*4 px, h rows.
0 = transparent. Palette = the world-1 level palette."""
import os, sys, struct
os.chdir('/home/amirg/fable5-retro-greenfield')
sys.path.insert(0, 'tools/render'); sys.path.insert(0, 'tools/extract')
from vec_render import write_png
from op12_port import Op12, DG
b = open('local/build/capture/game/BUMSPJEU.BIN', 'rb').read()
N = len(b); DATA = 0x800
be32 = lambda o: struct.unpack('>I', b[o:o+4])[0]
be16 = lambda o: struct.unpack('>H', b[o:o+2])[0]


def e6(v):
    return ((v & 0x3F) << 2) | ((v & 0x3F) >> 4)


raw = open('local/build/capture/game/MONDE1.VEC', 'rb').read()
S = 0x67bf0; mem = bytearray(0xA0000); mem[S:S+len(raw)] = raw
sv = lambda o, v: (mem.__setitem__(DG+o, v & 0xFF), mem.__setitem__(DG+o+1, (v >> 8) & 0xFF))
setl = lambda a, c, l: (sv(a, l & 0xF), sv(c, (l >> 4) & 0xFFFF))
setl(0x4e0e, 0x4e10, S); setl(0x4e0a, 0x4e0c, S+0x7d63)
sv(0x4e28, len(raw) & 0xFFFF); sv(0x4e2a, (len(raw) >> 16) & 0xFFFF)
Op12(mem).vec_run(dispatch_current=False)
d = bytes(mem[S:S+0x7d63])
PAL = [(e6(d[51+3*k]), e6(d[51+3*k+1]), e6(d[51+3*k+2])) for k in range(16)]


def decode_frame(fp, w, h):
    """4-plane interleaved-16px-blocks -> list of rows of indices (0=transparent)."""
    blocks = w // 4
    wpx = blocks * 16
    img = []
    for r in range(h):
        words = [be16(fp + r*w*2 + 2*i) for i in range(w)]
        row = []
        for blk in range(blocks):
            planes = words[blk*4:blk*4+4]
            for col in range(16):
                row.append(sum(((planes[p] >> (15-col)) & 1) << p for p in range(4)))
        img.append(row)
    return img, wpx


# enumerate frames. The offset table has DATA//4 (512) BE32 entries; entry 511 is a
# 0 terminator (not a frame), so there are 511 real frames, all raw (no ctrl&0x40).
frames = []
for i in range(DATA // 4):
    v = be32(i*4)
    if v == 0:
        continue                              # zero terminator / unused slot
    fp = DATA + v
    if not (DATA <= fp < N):
        break
    w = be16(fp-4); h = be16(fp-2); ctrl = b[fp-10]
    if ctrl & 0x40:
        # mask-RLE codec (prepare_sprite_frames); BUMSPJEU.BIN uses none — verified.
        print("  unexpected mask-RLE frame idx=%d w=%d h=%d ctrl=%#x" % (i, w, h, ctrl))
        continue
    if w == 0 or h == 0 or w % 4 or w > 16 or h > 48 or w*4 > 64:
        continue
    if fp + w*2*h > N:
        continue
    frames.append((i, fp, w, h, ctrl))
print("decodable raw frames: %d / %d real (table has %d slots, last is terminator)"
      % (len(frames), DATA//4 - 1, DATA//4))

# montage
CELL = 40; COLS = 16
ROWS = (len(frames) + COLS - 1) // COLS
W = COLS*CELL; H = ROWS*CELL
img = bytearray(W*H*3)                          # black bg
for n, (i, fp, w, h, ctrl) in enumerate(frames):
    grid, wpx = decode_frame(fp, w, h)
    cx, cy = (n % COLS)*CELL, (n // COLS)*CELL
    for r in range(min(h, CELL)):
        for c in range(min(wpx, CELL)):
            v = grid[r][c]
            if v:
                col = PAL[v]; o = ((cy+r)*W + cx+c)*3
                img[o:o+3] = col
write_png('local/results/sprites/bumspjeu_sheet.png', W, H, bytes(img))
print("wrote results/sprites/bumspjeu_sheet.png (%dx%d, %d frames)" % (W, H, len(frames)))
