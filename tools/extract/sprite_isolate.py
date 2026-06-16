"""Isolate blitted sprites: diff the emulator world-map capture (background + HUD +
sprites) against our pure-Python background render. Differing pixels = the sprites."""
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
            o = (y*w+x)*3
            if ch >= 3:
                out[o:o+3] = line[x*ch:x*ch+3]
            else:
                out[o] = out[o+1] = out[o+2] = line[x]
    return w, h, bytes(out)


ew, eh, emu = read_png('local/build/render/dosemu_vga_p0000.png')
bw, bh, bg = read_png('local/results/levels_png/world1.png')
print("emu %dx%d  bg %dx%d" % (ew, eh, bw, bh))
W, H = min(ew, bw), min(eh, bh)
out = bytearray(W*H*3)
diff = 0
minx = miny = 10**9; maxx = maxy = -1
for y in range(H):
    for x in range(W):
        o = (y*W+x)*3
        e = emu[o:o+3]; g = bg[o:o+3]
        if e != g:
            out[o:o+3] = e; diff += 1
            minx = min(minx, x); maxx = max(maxx, x); miny = min(miny, y); maxy = max(maxy, y)
write_png('local/build/render/sprites_isolated.png', W, H, bytes(out))
print("differing pixels: %d (%.1f%%); bbox x[%d..%d] y[%d..%d]" % (
    diff, 100*diff/(W*H), minx, maxx, miny, maxy))
print("wrote build/render/sprites_isolated.png")
