"""Validate op12_port.Op12.op4_handler (pure-Python op4) byte-exact, in-place,
seeded from the op4 entry snapshot, against the vec_cpu oracle output."""
import os, sys
os.chdir('/home/amirg/fable5-retro-greenfield')
sys.path.insert(0, 'tools/extract')
from op12_port import Op12, DG

mem = bytearray(open('local/build/render/op4_seed_mem.bin', 'rb').read())
mem += bytes(max(0, 0xA0000 - len(mem)))
stream = 0x67bf0
vec_src = 18462
truth = open('local/build/render/op4_truth.bin', 'rb').read()

op = Op12(mem)
op.op4_handler()
out = bytes(mem[stream:stream + vec_src])
diffs = [i for i in range(vec_src) if out[i] != truth[i]]
if not diffs:
    print("op4_handler EXACT MATCH vs oracle (%d bytes)" % vec_src)
else:
    print("op4_handler matched %d/%d (%.2f%%); first diff @%d out=%s want=%s" % (
        vec_src - len(diffs), vec_src, 100 * (vec_src - len(diffs)) / vec_src,
        diffs[0], out[diffs[0]:diffs[0] + 8].hex(), truth[diffs[0]:diffs[0] + 8].hex()))
