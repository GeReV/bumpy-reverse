"""Separate the world-map Bumpy+cloud composite into two sprites by colour: the cloud
is blue (drawn first), Bumpy is the red/orange ball + dark face on top. Blue pixels ->
cloud; everything else in the sprite region -> Bumpy. Also read the intro capture's p1
descriptor (dosemu_ram.bin) to locate the world-map Bumpy frame for file decoding."""
import os, sys, zlib, struct
os.chdir('/home/amirg/fable5-retro-greenfield')
sys.path.insert(0, 'tools/render')
from vec_render import write_png


def read_png(path):
    d = open(path, 'rb').read(); pos = 8; idat = b''; w = h = ct = 0
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]; t = d[pos+4:pos+8]; bb = d[pos+8:pos+8+ln]
        if t == b'IHDR':
            w, h, _, ct = struct.unpack('>IIBB', bb[:10])
        elif t == b'IDAT':
            idat += bb
        elif t == b'IEND':
            break
        pos += 12+ln
    raw = zlib.decompress(idat); ch = {0: 1, 2: 3, 6: 4}[ct]; st = w*ch
    out = bytearray(w*h*3); prev = bytearray(st); p = 0
    for y in range(h):
        f = raw[p]; p += 1; line = bytearray(raw[p:p+st]); p += st
        for i in range(st):
            a = line[i-ch] if i >= ch else 0; b2 = prev[i]; c = prev[i-ch] if i >= ch else 0; x = line[i]
            line[i] = (x + (a if f == 1 else b2 if f == 2 else (a+b2) >> 1 if f == 3 else
                       (a if (abs(b2-c) <= abs(a-c) and abs(b2-c) <= abs(a+b2-2*c)) else
                        b2 if abs(a-c) <= abs(a+b2-2*c) else c) if f == 4 else 0)) & 0xff
        prev = line
        for x in range(w):
            out[(y*w+x)*3:(y*w+x)*3+3] = line[x*ch:x*ch+3] if ch >= 3 else bytes([line[x]])*3
    return w, h, bytes(out)


W, H, emu = read_png('local/build/render/dosemu_vga_p2000.png')
_, _, bg = read_png('local/results/levels_png/world1.png')
# sprite region from the earlier crop: x[18..45] y[25..44]
X0, Y0, X1, Y1 = 18, 25, 45, 44
cw, ch = X1-X0+1, Y1-Y0+1
S = 6
cloud = bytearray(b'\xff\x00\xff' * (cw*S*ch*S))
bump = bytearray(b'\xff\x00\xff' * (cw*S*ch*S))
nc = nb = 0
for yy in range(ch):
    for xx in range(cw):
        x, y = X0+xx, Y0+yy
        o = (y*W+x)*3
        e = emu[o:o+3]; g = bg[o:o+3]
        if e == g:
            continue                                  # background, not a sprite pixel
        r, gr, b = e
        is_blue = b > r + 16 and b >= gr              # cloud = bluish
        tgt = cloud if is_blue else bump
        if is_blue:
            nc += 1
        else:
            nb += 1
        for sy in range(S):
            for sx in range(S):
                d = ((yy*S+sy)*cw*S + xx*S+sx)*3
                tgt[d:d+3] = e
os.makedirs('local/results/sprites', exist_ok=True)
write_png('local/results/sprites/cloud_worldmap.png', cw*S, ch*S, bytes(cloud))
write_png('local/results/sprites/bumpy_only.png', cw*S, ch*S, bytes(bump))
print("cloud pixels=%d -> cloud_worldmap.png ; bumpy pixels=%d -> bumpy_only.png" % (nc, nb))

# locate the world-map Bumpy frame in the intro-capture heap dump
ram = open('local/build/render/dosemu_ram.bin', 'rb').read()      # base linear 0x10000
def gw(lin):
    o = lin - 0x10000
    return ram[o] | (ram[o+1] << 8)
DG = 0x114b0
for nm, d in (("p1", 0x792e), ("p2", 0x795a), ("hud", 0x7986)):
    fi = gw(DG+d+4); dec = (gw(DG+d+0xe) << 4) + (gw(DG+d+0xc)); w = gw(DG+d+0x10); h = gw(DG+d+0x12)
    print("%s: frame_idx=%d decoded=%#x w=%d h=%d ctrl=%#x" % (nm, fi, dec, w, h, ram[DG+d+0xb-0x10000]))
