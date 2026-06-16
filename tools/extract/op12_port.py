#!/usr/bin/env python3
"""Faithful pure-Python transliteration of op12 (Bumpy renderer's masked-blit record
op), phases 1b/2/3 (1c28:066b..0a08), as a pc-dispatch state machine over a flat
linear address space. The 0xcda far-pointer helpers are flat arithmetic with es:di
preserved (confirmed from disasm). Seeded from the post-recursion snapshot
(op12_mid.bin, captured at 0x66b) so phases 1b/2/3 are validated independently of the
phase-1 recursion; output framebuffer is checked against the vec_cpu oracle
(call7_truth.bin). DGROUP runtime segment 0x114b (disasm's 0x103b + 0x110 reloc).
"""
from __future__ import annotations
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BR = os.path.join(ROOT, "local/build/render")
DG = 0x114b0
W = lambda lin: lin & 0xFFFF


class Op12:
    def __init__(self, mem):
        self.m = mem
        self.stack = []

    def gv(self, o):
        return self.m[DG + o] | (self.m[DG + o + 1] << 8)

    def sv(self, o, v):
        self.m[DG + o] = v & 0xFF; self.m[DG + o + 1] = (v >> 8) & 0xFF

    def phase1(self):
        """op12 0x4b0..0x66b: bound check, fill read, coord, dst2, coord-copy (mask
        setup from the stream), the recursion vec_run(0x7e), marker check.
        Returns False if op12 should terminate (marker 2 / out of bounds)."""
        m = self.m; gv = self.gv; sv = self.sv
        stream = self.getlin(0x4e0e, 0x4e10)
        vsrc = gv(0x4e24) | (gv(0x4e26) << 16)
        probe = (stream + vsrc) & 0xFFFFF
        if probe > self.getlin(0x4e0a, 0x4e0c):       # past vec_end -> terminate
            return False
        # 0x4d8: fill word (BE) at stream+0xc; dst = stream+0xe
        p = stream + 0xC
        self.setlin(0x4df6, 0x4df8, p)
        sv(0x4e22, (m[p] << 8) | m[p + 1])
        sv(0x4df6, gv(0x4df6) + 2)
        # 0x503: crd = round-up(vec_src, 0x20) >> 3. Clear vec_src's low 5 bits
        # (and byte[0x4e1e],0xe0); if that CHANGED the value (vec_src wasn't already a
        # multiple of 0x20) round up by +0x20 (0x529), otherwise use it as-is — the
        # 0x520 `je 0x52f` skips the add. (Bug fix: op12_port previously ALWAYS added
        # 0x20, over-rounding when vec_src is already 0x20-aligned, e.g. the 0xb60 .BUM
        # record: 0xb60>>3=0x16c vs the buggy 0x170 — a 4-byte crd/dst2/src shift that
        # desynced the following record. .PAV/.DEC never hit an aligned vec_src so they
        # were unaffected.)
        raw_hi = gv(0x4e26); raw_lo = gv(0x4e24)
        sv(0x4e20, raw_hi); sv(0x4e1e, raw_lo)
        m[DG + 0x4e1e] &= 0xE0
        if raw_hi == gv(0x4e20) and raw_lo == gv(0x4e1e):    # already 0x20-aligned
            val = ((raw_hi << 16) | raw_lo) & 0xFFFFFFFF
        else:                                                # masked -> round up
            val = (((gv(0x4e20) << 16) | gv(0x4e1e)) + 0x20) & 0xFFFFFFFF
        val >>= 3
        sv(0x4e20, (val >> 16) & 0xFFFF); sv(0x4e1e, val & 0xFFFF)
        # 0x542: dst2 = (vend - crd) & ~1
        vend = self.getlin(0x4e0a, 0x4e0c)
        crd = gv(0x4e1e) | (gv(0x4e20) << 16)
        d2 = (vend - crd) & 0xFFFFF
        sv(0x4e18, (d2 >> 4) & 0xFFFF); sv(0x4e16, d2 & 0xF)
        m[DG + 0x4e16] &= 0xFE
        dst2 = (gv(0x4e18) << 4) + gv(0x4e16)
        self.setlin(0x4dfa, 0x4dfc, dst2); self.setlin(0x4dfe, 0x4e00, dst2)
        # 0x583: coord-copy crd bytes from dst(=stream+0xe) -> dst2
        cnt = crd
        si = self.getlin(0x4df6, 0x4df8); di = dst2
        while cnt != 0:
            m[di:di + 4] = m[si:si + 4]
            si += 4; di += 4; cnt = (cnt - 4) & 0xFFFFFFFF
            if cnt == 0:
                break
        self.setlin(0x4df6, 0x4df8, si); self.setlin(0x4dfe, 0x4e00, di)
        # 0x60f: save 6, vend=dst2, vec_src=vsav, recurse
        for o in (0x4e26, 0x4e24, 0x4dfc, 0x4dfa, 0x4e0c, 0x4e0a):
            self.stack.append(gv(o))
        sv(0x4e0c, gv(0x4dfc)); sv(0x4e0a, gv(0x4dfa))
        sv(0x4e26, gv(0x4e2a)); sv(0x4e24, gv(0x4e28))
        self.vec_run_7e()
        for o in (0x4e0a, 0x4e0c, 0x4dfa, 0x4dfc, 0x4e24, 0x4e26):
            sv(o, self.stack.pop())
        if gv(0x4e14) == 0 and gv(0x4e12) == 2:
            return False
        return True

    def vec_run_7e(self):
        """The recursion leaf (1c28:007e): compute srcp/dst2/dst/dstend from vec_src
        (rounded up to 0x10) + the backward word-copy dst->dst2."""
        m = self.m; gv = self.gv; sv = self.sv
        # rounded = vec_src rounded up to 0x10
        sv(0x4e14, gv(0x4e26)); sv(0x4e12, gv(0x4e24))
        m[DG + 0x4e12] &= 0xF0
        if not (gv(0x4e26) == gv(0x4e14) and gv(0x4e24) == gv(0x4e12)):
            v = gv(0x4e12) + 0x10
            if v > 0xFFFF:
                sv(0x4e14, (gv(0x4e14) + 1) & 0xFFFF)
            sv(0x4e12, v & 0xFFFF)
        rounded = gv(0x4e12) | (gv(0x4e14) << 16)
        vend = self.getlin(0x4e0a, 0x4e0c)
        stream = self.getlin(0x4e0e, 0x4e10)
        srcp = (vend - rounded) & 0xFFFFF
        sv(0x4e08, (srcp >> 4) & 0xFFFF); sv(0x4e06, (srcp & 0xF) & 0xFE)
        srcp = self.getlin(0x4e06, 0x4e08)
        self.setlin(0x4dfa, 0x4dfc, srcp + rounded)
        self.setlin(0x4df6, 0x4df8, stream + rounded)
        dst = self.getlin(0x4df6, 0x4df8)
        self.setlin(0x4dfe, 0x4e00, dst + 0x10)
        dstend = self.getlin(0x4dfe, 0x4e00); dst2 = self.getlin(0x4dfa, 0x4dfc)
        # 0x11a: cmp dstend,dst2 -> jb/je-lo do the copy (dstend <= dst2); only the
        # strict dstend > dst2 case skips the copy and returns marker 2. (op12_port had
        # this inverted for the == / > cases — fine for .PAV/.DEC which hit dstend<dst2,
        # but .BUM hits dstend>=dst2 and needs the corrected branch.)
        if dstend > dst2:
            sv(0x4e14, 0); sv(0x4e12, 2); return
        # 0x135: backward word-copy `rounded` bytes dst -> dst2 (when dstend <= dst2)
        di = dst2; si = dst; n = rounded
        while n != 0:
            si -= 2; di -= 2
            m[di:di + 2] = m[si:si + 2]
            n = (n - 2) & 0xFFFFFFFF
        sv(0x4e14, 0); sv(0x4e12, 1)

    def vec_read_record(self):
        """vec_read_record (1c28:0a09): peek the 12-byte big-endian record at the
        stream pointer; set vec_src=w0:w1, coord=w2:w3, opcode=w4. Returns the opcode
        (low 15 bits) or None to terminate. vec_run terminates (CF set) when ANY of:
          - w0 > 0x0f                (source-seg out of range — natural end)
          - w4 & 0x7f00 != 0         (opcode high bits set — invalid opcode)
          - w5 != w0^w1^w2^w3^w4     (XOR checksum mismatch)
        The .DEC streams rely on the opcode-validity/checksum terminator (their end
        record has opcode 0x750), not the w0 check that ends .PAV/.BUM."""
        m = self.m
        sp = self.getlin(0x4e0e, 0x4e10)
        w0 = (m[sp] << 8) | m[sp + 1]
        w1 = (m[sp + 2] << 8) | m[sp + 3]
        w2 = (m[sp + 4] << 8) | m[sp + 5]
        w3 = (m[sp + 6] << 8) | m[sp + 7]
        w4 = (m[sp + 8] << 8) | m[sp + 9]
        w5 = (m[sp + 10] << 8) | m[sp + 11]
        self.sv(0x4e26, w0); self.sv(0x4e24, w1)
        self.sv(0x4e20, w3); self.sv(0x4e1e, w2)
        self.sv(0x4e31, w4)
        self.sv(0x4e35, w0); self.sv(0x4e33, w1)
        if w0 > 0x0F:
            return None
        if (w4 & 0x7F00) != 0:
            return None
        if (w0 ^ w1 ^ w2 ^ w3 ^ w4) != w5:
            return None
        return w4 & 0x7FFF

    def vec_run(self, dispatch_current=True, trace=None):
        """vec_run record loop: dispatch op12/op4 by opcode until opcode<=0.
        dispatch_current: if seeded mid-loop (op12 entry), dispatch the already-read
        record first."""
        gv = self.gv; sv = self.sv
        if dispatch_current:
            op = gv(0x4e31) & 0x7FFF
        else:
            op = self.vec_read_record()
        while op is not None and op > 0:
            if trace is not None:
                trace.append((op, self.getlin(0x4e0e, 0x4e10)))
            if op == 12:
                if not self.phase1():
                    break
                self.run()
            elif op == 4:
                self.op4_handler()
            # vsav update (0x60): vsav = w0:w1 of the record just processed
            sv(0x4e2a, gv(0x4e35)); sv(0x4e28, gv(0x4e33))
            op = self.vec_read_record()

    def op4_handler(self):
        """op4 (1c28:0194): in-place LZSS/RLE decompressor. Pure-Python port.

        The real handler relocates the compressed payload to the top of the buffer
        (+ a 0x400 sliding window) so it can decompress in place from the bottom up;
        that relocation is purely a mechanism — the bytes read, in order, are exactly
        the sequential payload [stream+0xd, stream+vsav). So we snapshot the payload,
        RLE-decode it forward into [stream, stream+vec_src), and leave vec_stream
        unchanged (vec_run then re-reads the freshly decompressed record stream).

        Encoding (escape byte `fill` = byte at stream+0xc):
          V (!= fill)            -> literal V
          fill, fill            -> a single literal `fill` byte
          fill, V (!= fill), C  -> V repeated C times (C==0 means 256)
        Validated byte-exact against the vec_cpu oracle (build/render/op4_truth.bin)."""
        m = self.m
        stream = self.getlin(0x4e0e, 0x4e10)
        vec_src = self.gv(0x4e24) | (self.gv(0x4e26) << 16)
        vsav = self.gv(0x4e28) | (self.gv(0x4e2a) << 16)
        fill = m[stream + 0xC]
        inp = bytes(m[stream + 0xD: stream + vsav])
        out = bytearray()
        p = 0
        n = len(inp)
        while len(out) < vec_src and p < n:
            v0 = inp[p]; p += 1
            if v0 != fill:
                out.append(v0)
                continue
            v1 = inp[p]; p += 1
            if v1 == fill:
                out.append(fill)
            else:
                v2 = inp[p]; p += 1
                out.extend(bytes([v1]) * (v2 if v2 else 256))
        del out[vec_src:]
        m[stream:stream + len(out)] = out
        # The real op4 reads the compressed payload through a 0x400 sliding window at
        # DG:0x4e97, relocating it there end-aligned. A later op12 record's finalize
        # copies that window into the buffer's [dstend, vec_end) region, which the NEXT
        # op12 record reads as its source — so the window must be reproduced or the small
        # .BUM streams decode wrong. (Verified end-aligned == vec_cpu's post-op4 window.)
        win = DG + 0x4E97
        plen = len(inp)
        if plen <= 0x400:
            for i in range(0x400 - plen):
                m[win + i] = 0
            m[win + 0x400 - plen: win + 0x400] = inp
        else:
            m[win: win + 0x400] = inp[-0x400:]

    def run(self):
        m = self.m; gv = self.gv; sv = self.sv
        # ---- phase 1b (0x66b): save 6 words, set up src/srcp/dst/dstend/mask ----
        for o in (0x4e26, 0x4e24, 0x4e2a, 0x4e28, 0x4e0c, 0x4e0a):
            self.stack.append(gv(o))
        srcp = (gv(0x4e08) << 4) + gv(0x4e06)
        vsav = gv(0x4e28) | (gv(0x4e2a) << 16)
        src = srcp + vsav
        sv(0x4e04, W(src >> 4) if False else (src >> 4) & 0xFFFF); sv(0x4e02, src & 0xF)
        crd = gv(0x4e1e) | (gv(0x4e20) << 16)
        t = srcp + crd
        srcp2 = t + 0xE
        sv(0x4e08, (srcp2 >> 4) & 0xFFFF); sv(0x4e06, srcp2 & 0xF)
        vend = (gv(0x4e0c) << 4) + gv(0x4e0a)
        dstend = vend - 0x400
        sv(0x4e00, (dstend >> 4) & 0xFFFF); sv(0x4dfe, dstend & 0xF)
        sv(0x4e0c, gv(0x4dfc)); sv(0x4e0a, gv(0x4dfa))          # mask ptr = dst2
        sv(0x4df8, gv(0x4e10)); sv(0x4df6, gv(0x4e0e))          # dst = stream
        sv(0x4e1c, 0); sv(0x4e1a, 0)
        sv(0x4e14, 0); sv(0x4e12, 0)
        sv(0x4e18, 0); sv(0x4e16, 0)
        # normalize src/srcp/dst the way the helpers leave them (off<16). They already
        # are (we stored seg=lin>>4, off=lin&0xF). dst from stream may have off>=16:
        self._norm(0x4df6, 0x4df8); self._norm(0x4e02, 0x4e04)
        self.bp = 0
        self.plot_loop()

    def _norm(self, off_o, seg_o):
        lin = (self.gv(seg_o) << 4) + self.gv(off_o)
        self.sv(off_o, lin & 0xF); self.sv(seg_o, (lin >> 4) & 0xFFFF)

    def getlin(self, off_o, seg_o):
        return (self.gv(seg_o) << 4) + self.gv(off_o)

    def setlin(self, off_o, seg_o, lin):
        self.sv(off_o, lin & 0xF); self.sv(seg_o, (lin >> 4) & 0xFFFF)

    def plot_loop(self):
        m = self.m; gv = self.gv; sv = self.sv
        while True:
            # 0x71d: decrement bit counter; reload mask when it goes negative (jns)
            cnt = (gv(0x4e16) - 1) & 0xFFFF
            sv(0x4e16, cnt)
            if cnt & 0x8000:                       # counter < 0 -> reload (plain BE32 word)
                mp = self.getlin(0x4e0a, 0x4e0c)
                w32 = (m[mp] << 24) | (m[mp+1] << 16) | (m[mp+2] << 8) | m[mp+3]
                self.setlin(0x4e0a, 0x4e0c, mp + 4)
                sv(0x4e16, 0x1f)
                sv(0x4e26, (w32 >> 16) & 0xFFFF); sv(0x4e24, w32 & 0xFFFF)
            # 0x772: shift mask word left, top bit -> cf
            v = (gv(0x4e26) << 16) | gv(0x4e24)
            cf = (v >> 31) & 1
            v = (v << 1) & 0xFFFFFFFF
            sv(0x4e26, (v >> 16) & 0xFFFF); sv(0x4e24, v & 0xFFFF)
            if cf == 0:
                self.do_plot()                     # 0x789
                # 0x812: done-check only after a plot (fill leaves srcp unchanged)
                if gv(0x4e08) == gv(0x4e04) and gv(0x4e06) == gv(0x4e02):
                    self.finalize(); return
            else:
                self.do_fill()                     # 0x8d7
            # 0x828: relocation gate (fires at most once, then resumes plotting)
            self.phase3_relocate()

    def _advance_dst_check_wrap(self):
        """after writing one dst byte: advance dst, bump counter, wrap at dstend."""
        gv = self.gv; sv = self.sv
        c = (gv(0x4e12) + 1) & 0xFFFF
        sv(0x4e12, c)
        if c == 0:
            sv(0x4e14, (gv(0x4e14) + 1) & 0xFFFF)
        if self.getlin(0x4dfe, 0x4e00) == self.getlin(0x4df6, 0x4df8):
            self.setlin(0x4df6, 0x4df8, DG + 0x4e97)
            sv(0x4e1c, 0); sv(0x4e1a, 1)

    def do_plot(self):                              # 0x789
        m = self.m
        dst = self.getlin(0x4df6, 0x4df8)
        src = self.getlin(0x4e06, 0x4e08)
        m[dst] = m[src]
        self.bp = (src + 1) >> 4                     # bp=ds after normalize (approx)
        self.setlin(0x4e06, 0x4e08, src + 1)
        self.setlin(0x4df6, 0x4df8, dst + 1)
        self._advance_dst_check_wrap()

    def do_fill(self):                              # 0x8d7
        m = self.m; gv = self.gv
        dst = self.getlin(0x4df6, 0x4df8)
        m[dst] = gv(0x4e22) & 0xFF
        self.setlin(0x4df6, 0x4df8, dst + 1)
        self._advance_dst_check_wrap()

    def phase3_relocate(self):                       # 0x828 .. 0x8d4
        """In-place LZ overlap management: when the forward write ptr (dst) catches
        the unread source (srcp <= dst), relocate the unread source [srcp,src) up so
        it ends at vec_end, then resume the plot loop (0x8d4 jmp 0x71d) — it fires at
        most ONCE per trigger, never loops (the loop-back was the MONDE3 hang bug)."""
        gv = self.gv; sv = self.sv; m = self.m
        # 0x828: srcp > dst -> resume plotting
        srcp = self.getlin(0x4e06, 0x4e08); dst = self.getlin(0x4df6, 0x4df8)
        if srcp > dst:
            return
        # 0x843/0x84d: if dst has wrapped into the window, don't relocate
        if gv(0x4e1c) != 0 or gv(0x4e1a) != 0:
            return
        # 0x857: backward memmove of [srcp..src) so it ends at vec_end (= mask ptr)
        sv(0x4dfc, gv(0x4e0c)); sv(0x4dfa, gv(0x4e0a))    # dst2 = vec_end
        di = self.getlin(0x4dfa, 0x4dfc)
        si = self.getlin(0x4e02, 0x4e04)
        srcp_lin = srcp
        while True:
            si -= 1; di -= 1
            m[di] = m[si]
            if si == srcp_lin:
                break
        # 0x8ab..0x8d1: dst2 = di; srcp = dst2; src = vec_end  (0x8d4 jmp 0x71d)
        self.setlin(0x4dfa, 0x4dfc, di)        # dst2 = di
        self.setlin(0x4e06, 0x4e08, di)        # srcp = dst2
        sv(0x4e04, gv(0x4e0c)); sv(0x4e02, gv(0x4e0a))   # src = vec_end

    def finalize(self):                              # 0x934
        gv = self.gv; sv = self.sv; m = self.m
        for o in (0x4e0a, 0x4e0c, 0x4e28, 0x4e2a, 0x4e24, 0x4e26):
            sv(o, self.stack.pop())
        out_len = gv(0x4e24) | (gv(0x4e26) << 16)
        # 0x94c: fill dst with fill byte until op counter reaches out_len (post-wrap
        # fills go to the window)
        while True:
            dst = self.getlin(0x4df6, 0x4df8)
            m[dst] = gv(0x4e22) & 0xFF
            self.setlin(0x4df6, 0x4df8, dst + 1)
            c = (gv(0x4e12) + 1) & 0xFFFF
            sv(0x4e12, c)
            if c == 0:
                sv(0x4e14, (gv(0x4e14) + 1) & 0xFFFF)
            if self.getlin(0x4dfe, 0x4e00) == self.getlin(0x4df6, 0x4df8):
                self.setlin(0x4df6, 0x4df8, DG + 0x4e97)
                sv(0x4e1c, 0); sv(0x4e1a, 1)
            if (gv(0x4e12) | (gv(0x4e14) << 16)) >= out_len:
                break
        # 0x9a9: final copy 0x100*4=1024 bytes window -> dstend
        di = self.getlin(0x4dfe, 0x4e00)
        si = DG + 0x4e97
        for _ in range(0x100):
            m[di:di + 4] = m[si:si + 4]
            si += 4; di += 4


def main():
    import sys
    # "entry": run full op12 (phase1+recursion+1b/2/3) from the op12 entry snapshot;
    # default: run phases 1b/2/3 only from the post-recursion snapshot.
    entry = len(sys.argv) > 1 and sys.argv[1] == "entry"
    seed = "op12_seed_mem.bin" if entry else "op12_mid.bin"
    mem = bytearray(open(os.path.join(BR, seed), "rb").read())
    mem += bytes(max(0, 0xA0000 - len(mem)))
    fb = ((mem[DG + 0x4e10] | (mem[DG + 0x4e10 + 1] << 8)) << 4)
    op = Op12(mem)
    try:
        if entry:
            if op.phase1():
                op.run()
        else:
            op.run()
    except Exception as e:
        print("port raised: %s: %s" % (type(e).__name__, e))
    truth = open(os.path.join(BR, "call7_truth.bin"), "rb").read()
    got = bytes(mem[fb:fb + len(truth)])
    diffs = [i for i in range(min(len(truth), len(got))) if got[i] != truth[i]]
    if not diffs:
        print("EXACT MATCH vs oracle!")
    else:
        match = len(truth) - len(diffs)
        print("matched %d/%d (%.1f%%); first divergence @%#x got=%s want=%s" % (
            match, len(truth), 100 * match / len(truth), diffs[0],
            got[diffs[0]:diffs[0]+6].hex(), truth[diffs[0]:diffs[0]+6].hex()))


if __name__ == "__main__":
    main()
