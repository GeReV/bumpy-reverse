"""Crop individual blitted sprites from the emulator world-map capture using the
background-diff mask: cluster differing pixels into bounding boxes, crop each from the
emulator image (non-sprite pixels -> transparent magenta), upscale, save to results."""
import os, sys, zlib, struct
os.chdir('/home/amirg/fable5-retro-greenfield')
sys.path.insert(0, 'tools/render')
from vec_render import write_png


def read_png(path):
    d = open(path, 'rb').read(); pos = 8; idat = b''; w = h = ct = 0
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]; t = d[pos+4:pos+8]; b = d[pos+8:pos+8+ln]
        if t == b'IHDR':
            w, h, _, ct = struct.unpack('>IIBB', b[:10])
        elif t == b'IDAT':
            idat += b
        elif t == b'IEND':
            break
        pos += 12+ln
    raw = zlib.decompress(idat); ch = {0: 1, 2: 3, 6: 4}[ct]; st = w*ch
    out = bytearray(w*h*3); prev = bytearray(st); p = 0
    for y in range(h):
        f = raw[p]; p += 1; line = bytearray(raw[p:p+st]); p += st
        for i in range(st):
            a = line[i-ch] if i >= ch else 0; bb = prev[i]; c = prev[i-ch] if i >= ch else 0; x = line[i]
            line[i] = (x + (a if f == 1 else bb if f == 2 else (a+bb) >> 1 if f == 3 else
                       (a if (abs(bb-c) <= abs(a-c) and abs(bb-c) <= abs(a+bb-2*c)) else
                        bb if abs(a-c) <= abs(a+bb-2*c) else c) if f == 4 else 0)) & 0xff
        prev = line
        for x in range(w):
            out[(y*w+x)*3:(y*w+x)*3+3] = line[x*ch:x*ch+3] if ch >= 3 else bytes([line[x]])*3
    return w, h, bytes(out)


W, H, emu = read_png('local/build/render/dosemu_vga_p2000.png')
_, _, bg = read_png('local/results/levels_png/world1.png')
mask = [[emu[(y*W+x)*3:(y*W+x)*3+3] != bg[(y*W+x)*3:(y*W+x)*3+3] for x in range(W)] for y in range(H)]

# connected-component cluster (4-neighbour) of mask pixels into bboxes
seen = [[False]*W for _ in range(H)]
boxes = []
for y in range(H):
    for x in range(W):
        if mask[y][x] and not seen[y][x]:
            stack = [(x, y)]; seen[y][x] = True
            x0 = x1 = x; y0 = y1 = y; n = 0
            while stack:
                cx, cy = stack.pop(); n += 1
                x0 = min(x0, cx); x1 = max(x1, cx); y0 = min(y0, cy); y1 = max(y1, cy)
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (-1, -1), (1, -1), (-1, 1)):
                    nx, ny = cx+dx, cy+dy
                    if 0 <= nx < W and 0 <= ny < H and mask[ny][nx] and not seen[ny][nx]:
                        seen[ny][nx] = True; stack.append((nx, ny))
            if n >= 12:
                boxes.append((x0, y0, x1, y1, n))
boxes.sort(key=lambda b: (b[1], b[0]))
os.makedirs('local/results/sprites', exist_ok=True)
print("found %d sprite clusters:" % len(boxes))
for i, (x0, y0, x1, y1, n) in enumerate(boxes):
    cw, ch = x1-x0+1, y1-y0+1
    S = 6
    img = bytearray(b'\xff\x00\xff' * (cw*S*ch*S))   # magenta bg
    for yy in range(ch):
        for xx in range(cw):
            if mask[y0+yy][x0+xx]:
                o = ((y0+yy)*W + x0+xx)*3; col = emu[o:o+3]
                for sy in range(S):
                    for sx in range(S):
                        d = ((yy*S+sy)*cw*S + xx*S+sx)*3
                        img[d:d+3] = col
    name = 'local/results/sprites/sprite_%02d_%dx%d.png' % (i, cw, ch)
    write_png(name, cw*S, ch*S, bytes(img))
    print("  [%d] x[%d..%d] y[%d..%d] %dx%d (%d px) -> %s" % (i, x0, x1, y0, y1, cw, ch, n, name))
