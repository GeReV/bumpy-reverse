"""Extract the actual displayed colours from the DOSBox screenshot canvas and compare
to the real DAC (palette_io.json). Also check for scaler interpolation."""
import os, sys, zlib, struct, json, collections
os.chdir('/home/amirg/fable5-retro-greenfield')


def read_png_rgb(path):
    data = open(path, 'rb').read(); pos = 8; idat = b''; w = h = ctype = 0
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos+4])[0]; typ = data[pos+4:pos+8]; body = data[pos+8:pos+8+ln]
        if typ == b'IHDR':
            w, h, _, ctype = struct.unpack('>IIBB', body[:10])
        elif typ == b'IDAT':
            idat += body
        elif typ == b'IEND':
            break
        pos += 12 + ln
    raw = zlib.decompress(idat); ch = {2: 3, 6: 4}[ctype]; stride = w*ch
    out = bytearray(w*h*3); prev = bytearray(stride); p = 0
    for y in range(h):
        f = raw[p]; p += 1; line = bytearray(raw[p:p+stride]); p += stride
        for i in range(stride):
            a = line[i-ch] if i >= ch else 0; b = prev[i]; c = prev[i-ch] if i >= ch else 0
            x = line[i]
            line[i] = (x + (a if f == 1 else b if f == 2 else (a+b) >> 1 if f == 3 else
                       (a if (abs(b-c) <= abs(a-c) and abs(b-c) <= abs(a+b-2*c)) else
                        b if abs(a-c) <= abs(a+b-2*c) else c) if f == 4 else 0)) & 0xff
        prev = line
        for x in range(w):
            out[(y*w+x)*3:(y*w+x)*3+3] = line[x*ch:x*ch+3]
    return w, h, bytes(out)


w, h, g = read_png_rgb('local/results/oracle/world1_dosbox.png')


def px(x, y):
    o = (y*w+x)*3; return (g[o], g[o+1], g[o+2])


# canvas y in [26,986], skip HUD (~first 70 screen rows of game) -> start y=100
cnt = collections.Counter()
for y in range(100, 985):
    for x in range(2, 1280):
        cnt[px(x, y)] += 1
print("distinct colours in canvas:", len(cnt))
print("top 20 by area:")
for col, n in cnt.most_common(20):
    print("   %s  %d" % (col, n))

# interpolation check: horizontal run at y=500
print("\nhoriz run y=500 x=2..40:", [px(x, 500) for x in range(2, 40, 3)])

# the real DAC (e6'd) for comparison
def e6(v):
    return ((v & 0x3F) << 2) | ((v & 0x3F) >> 4)
io = json.load(open('local/build/render/palette_io.json'))
dac = {int(k, 16): tuple(e6(c) for c in v) for k, v in io['dac'].items()}
print("\nreal DAC (e6) entries:")
for k in sorted(dac):
    print("   %#04x: %s" % (k, dac[k]))
