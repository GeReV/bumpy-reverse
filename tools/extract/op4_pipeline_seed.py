"""Full chain from the op4 entry seed: vec_run dispatches op4 (decompress in place)
then re-reads the decompressed record stream, dispatches op12, renders framebuffer.
Compare framebuffer vs the known-good op12 render (call7_truth.bin)."""
import os, sys
os.chdir('/home/amirg/fable5-retro-greenfield')
sys.path.insert(0, 'tools/extract')
from op12_port import Op12, DG

mem = bytearray(open('local/build/render/op4_seed_mem.bin', 'rb').read())
mem += bytes(max(0, 0xA0000 - len(mem)))
stream = 0x67bf0
op = Op12(mem)

trace = []
try:
    op.vec_run(dispatch_current=False, trace=trace)
except Exception as e:
    import traceback
    traceback.print_exc()
print("dispatched ops (op, stream_ptr):", [(o, hex(s)) for o, s in trace[:20]],
      "... total", len(trace))

truth = open('local/build/render/call7_truth.bin', 'rb').read()
fb = stream
got = bytes(mem[fb:fb + len(truth)])
diffs = [i for i in range(min(len(truth), len(got))) if got[i] != truth[i]]
if not diffs:
    print("FRAMEBUFFER EXACT MATCH vs call7_truth (%d bytes)" % len(truth))
else:
    print("framebuffer matched %d/%d (%.2f%%); first diff @%#x got=%s want=%s" % (
        len(truth) - len(diffs), len(truth), 100 * (len(truth) - len(diffs)) / len(truth),
        diffs[0], got[diffs[0]:diffs[0]+6].hex(), truth[diffs[0]:diffs[0]+6].hex()))
