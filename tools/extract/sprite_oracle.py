"""Drive prepare_sprite_frames in vec_cpu (real game code) for a sprite frame and read
back the decoded pointer + dims. Validate vs the seed's p1 (decoded@0x4723c, 4x16)."""
import os, sys
os.chdir('/home/amirg/fable5-retro-greenfield')
sys.path.insert(0, 'tools/extract')
from vec_cpu import CPU, Halt
DG = 0x114b0
SS = 0x1b6c


def run_prepare(frame_idx, obj=0x792e):
    mem = bytearray(open('local/build/render/op12_seed_mem.bin', 'rb').read())
    mem += bytes(max(0, 0xA0000 - len(mem)))
    gw = lambda o: mem[DG+o] | (mem[DG+o+1] << 8)
    sw = lambda o, v: (mem.__setitem__(DG+o, v & 0xFF), mem.__setitem__(DG+o+1, (v >> 8) & 0xFF))
    sw(obj+4, frame_idx)                       # set frame index on the descriptor
    ss = SS << 4
    # object list at SS:0x310 = [far ptr to obj][NULL]
    def w16(lin, v):
        mem[lin] = v & 0xFF; mem[lin+1] = (v >> 8) & 0xFF
    w16(ss+0x310, obj); w16(ss+0x312, 0x114b)
    w16(ss+0x314, 0); w16(ss+0x316, 0)
    # [bp+4] = far ptr to the list
    w16(ss+0x304, 0x310); w16(ss+0x306, SS)
    cpu = CPU(mem)
    cpu.s['cs'] = 0xdfc; cpu.ip = 0x2ded
    cpu.s['ds'] = 0x114b; cpu.s['es'] = 0x114b; cpu.s['ss'] = SS
    cpu.r['bp'] = 0x300
    sp = 0x200
    mem[ss+sp-4] = 0; mem[ss+sp-3] = 0; mem[ss+sp-2] = 0; mem[ss+sp-1] = 0  # far ret frame
    cpu.r['sp'] = sp - 4; cpu.entry_sp = sp - 4
    steps = 0
    try:
        while True:
            cpu.step(); steps += 1
            if steps > 3_000_000:
                return None, "cap"
    except Halt:
        pass
    except Exception as e:
        return None, "%s: %s" % (type(e).__name__, e)
    dec = (gw(obj+0xe) << 4) + gw(obj+0xc)
    return (gw(obj+0x10), gw(obj+0x12), dec, gw(obj+0xb)), mem


seed = open('local/build/render/op12_seed_mem.bin', 'rb').read()
print("seed p1: w=%d h=%d decoded=%#x ctrl=%#x" % (
    seed[DG+0x792e+0x10] | (seed[DG+0x792e+0x11] << 8),
    seed[DG+0x792e+0x12] | (seed[DG+0x792e+0x13] << 8),
    (seed[DG+0x792e+0xe] | (seed[DG+0x792e+0xf] << 8)) << 4 | (seed[DG+0x792e+0xc] | (seed[DG+0x792e+0xd] << 8)),
    seed[DG+0x792e+0xb]))
for fi in (0, 1, 2):
    res, mem = run_prepare(fi)
    if res is None:
        print("frame %d: FAILED (%s)" % (fi, mem)); continue
    w, h, dec, ctrl = res
    print("frame %d: w=%d h=%d decoded=%#x ctrl=%#x  decoded[:32]=%s" % (
        fi, w, h, dec, ctrl, bytes(mem[dec:dec+32]).hex()))
