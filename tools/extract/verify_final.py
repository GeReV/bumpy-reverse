import os, sys, zlib, struct
os.chdir('/home/amirg/fable5-retro-greenfield')
sys.path.insert(0, 'tools/render'); sys.path.insert(0, 'tools/extract')
from op12_port import Op12, DG
from vec_render import render_planar
W, H = 320, 200


def e6(v):
    return ((v & 0x3F) << 2) | ((v & 0x3F) >> 4)


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
    raw = zlib.decompress(idat); ch = {2: 3, 6: 4}[ct]; st = w*ch
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
            out[(y*w+x)*3:(y*w+x)*3+3] = line[x*ch:x*ch+3]
    return w, h, bytes(out)


raw = open('local/build/capture/game/MONDE1.VEC', 'rb').read()
S = 0x67bf0
mem = bytearray(0xA0000); mem[S:S+len(raw)] = raw
sv = lambda o, v: (mem.__setitem__(DG+o, v & 0xFF), mem.__setitem__(DG+o+1, (v >> 8) & 0xFF))
setl = lambda a, b, l: (sv(a, l & 0xF), sv(b, (l >> 4) & 0xFFFF))
setl(0x4e0e, 0x4e10, S); setl(0x4e0a, 0x4e0c, S+0x7d63)
sv(0x4e28, len(raw) & 0xFFFF); sv(0x4e2a, (len(raw) >> 16) & 0xFFFF)
Op12(mem).vec_run(dispatch_current=False)
emb = [(e6(mem[S+51+3*k]), e6(mem[S+51+3*k+1]), e6(mem[S+51+3*k+2])) for k in range(16)]
rgb = render_planar(bytes(mem[S:S+99+W*H//2]), W, H, emb, "seq", 99)

gw, gh, g = read_png('local/results/oracle/world1_dosbox.png')
X0, Y0, SX, SY = 1, 26, 1280/320, 960/200
dbox = lambda x, y: tuple(g[(int(Y0+(y+0.5)*SY)*gw+int(X0+(x+0.5)*SX))*3+k] for k in range(3))
tot = m = 0
for x in range(W):
    for y in range(18, H):       # skip HUD rows only
        if 24 <= x <= 46 and 18 <= y <= 50:   # skip Bumpy sprite
            continue
        o = (y*W+x)*3; tot += 1
        m += (rgb[o], rgb[o+1], rgb[o+2]) == dbox(x, y)
print("planar@99 + embedded palette@51, NO roll: %d/%d = %.2f%% vs DOSBox" % (m, tot, 100*m/tot))
