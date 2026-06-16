import os, sys
os.chdir('/home/amirg/fable5-retro-greenfield')
sys.path.insert(0, 'tools/extract')
from vec_cpu import CPU, Halt

mem = bytearray(open('local/build/render/op4_seed_mem.bin', 'rb').read())
mem += bytes(max(0, 0xA0000 - len(mem)))
stream = 0x67bf0; vec_src = 18462
cpu = CPU(mem)
cpu.s['cs'] = 0xd38; cpu.ip = 0x194
cpu.s['ds'] = 0x114b; cpu.s['es'] = 0x114b; cpu.s['ss'] = 0x1b6c
ss = 0x1b6c << 4; sp = 0x1c8
mem[ss+sp] = 0x34; mem[ss+sp+1] = 0x12
cpu.r['sp'] = sp; cpu.entry_sp = sp
try:
    while True:
        cpu.step()
except Halt:
    pass
out = bytes(mem[stream:stream+vec_src])
open('local/build/render/op4_truth.bin', 'wb').write(out)
print("saved op4_truth.bin (%d bytes)" % len(out))
print("head:", out[:24].hex())
print("tail:", out[-24:].hex())

# ---- test a clean sequential RLE decoder against it ----
src = open('local/build/capture/game/MONDE1.VEC', 'rb').read()
fill = src[0xc]
print("fill =", hex(fill))
p = 0xd
res = bytearray()
while len(res) < vec_src and p < len(src):
    v0 = src[p]; p += 1
    if v0 != fill:
        res.append(v0)
    else:
        v1 = src[p]; p += 1
        if v1 == fill:
            res.append(fill)
        else:
            v2 = src[p]; p += 1
            cnt = v2 if v2 else 256
            res.extend([v1] * cnt)
res = bytes(res[:vec_src])
diffs = [i for i in range(min(len(res), len(out))) if res[i] != out[i]]
if len(res) == len(out) and not diffs:
    print("SEQUENTIAL RLE == oracle: EXACT (%d bytes), input consumed %d/%d" % (len(res), p, len(src)))
else:
    print("seq len %d vs %d; diffs %d; first %s" % (
        len(res), len(out), len(diffs), diffs[0] if diffs else "-"))
    if diffs:
        d = diffs[0]
        print("  @%d seq=%s ora=%s" % (d, res[d:d+8].hex(), out[d:d+8].hex()))
