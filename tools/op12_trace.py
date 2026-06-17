#!/usr/bin/env python3
"""Emit a TRC state trace from the Python reference Op12 on a .VEC, mirroring the
checkpoints added to src/op12.c, so the two traces can be diffed to localize a
divergence.  Prints `TRC ` lines to stdout."""
from __future__ import annotations

import os
import sys
from typing import List, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools/extract"))

from op12_port import Op12, DG  # noqa: E402

STREAM = 0x67bf0
DECLARED_LEN = 0x7d63


def fmt_state(o: Op12, tag: str) -> str:
    gv = o.gv

    def lin(off_o: int, seg_o: int) -> int:
        return (gv(seg_o) << 4) + gv(off_o)

    parts = [
        "TRC %-10s" % tag,
        "rec=%04x/%04x/%04x/%04x" % (gv(0x4e02), gv(0x4e04), gv(0x4e06), gv(0x4e08)),
        "ve=%04x/%04x" % (gv(0x4e0a), gv(0x4e0c)),
        "vs=%04x/%04x" % (gv(0x4e0e), gv(0x4e10)),
        "src16=%04x/%04x" % (gv(0x4e1e), gv(0x4e20)),
        "ctr=%04x/%04x/%04x" % (gv(0x4e12), gv(0x4e14), gv(0x4e16)),
        "msk=%04x/%04x" % (gv(0x4e24), gv(0x4e26)),
        "dst=%05x" % lin(0x4df6, 0x4df8),
        "srcp=%05x" % lin(0x4e06, 0x4e08),
        "src=%05x" % lin(0x4e02, 0x4e04),
        "dstend=%05x" % lin(0x4dfe, 0x4e00),
        "dst2=%05x" % lin(0x4dfa, 0x4dfc),
    ]
    return " ".join(parts)


def main() -> None:
    vec_path = sys.argv[1]
    plot_every = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    vec_bytes = open(vec_path, "rb").read()

    mem = bytearray(0xA0000)
    mem[STREAM:STREAM + len(vec_bytes)] = vec_bytes

    def sv(off: int, v: int) -> None:
        mem[DG + off] = v & 0xFF
        mem[DG + off + 1] = (v >> 8) & 0xFF

    def setlin(off_o: int, seg_o: int, lin_: int) -> None:
        sv(off_o, lin_ & 0xF)
        sv(seg_o, (lin_ >> 4) & 0xFFFF)

    setlin(0x4e0e, 0x4e10, STREAM)
    setlin(0x4e0a, 0x4e0c, STREAM + DECLARED_LEN)
    vsav = len(vec_bytes)
    sv(0x4e28, vsav & 0xFFFF)
    sv(0x4e2a, (vsav >> 16) & 0xFFFF)

    o = Op12(mem)

    # wrap methods with trace emitters
    _phase1 = o.phase1
    _vec_run_7e = o.vec_run_7e
    _run = o.run
    _vec_read_record = o.vec_read_record
    _plot_loop = o.plot_loop

    def phase1():
        print(fmt_state(o, "phase1.in"), flush=True)
        r = _phase1()
        print(fmt_state(o, "phase1.out"), "ret=%d" % r, flush=True)
        return r

    def vec_run_7e():
        print(fmt_state(o, "7e.in"), flush=True)
        _vec_run_7e()
        print(fmt_state(o, "7e.out"), flush=True)

    def run():
        print(fmt_state(o, "run.in"), flush=True)
        s = 0
        for a in range(STREAM, STREAM + 0x7d63):
            s = (s * 31 + o.m[a]) & 0xFFFFFFFF
        print("TRC arenasum   sum=%08x" % s, flush=True)
        _run()

    def vec_read_record():
        op = _vec_read_record()
        print("TRC vrr        op=%s sp=%05x" % (
            ("%d" % op) if op is not None else "None", o.getlin(0x4e0e, 0x4e10)),
            flush=True)
        return op

    counter = [0]

    _phase3 = o.phase3_relocate

    def phase3_relocate():
        gv = o.gv
        srcp = o.getlin(0x4e06, 0x4e08)
        dst = o.getlin(0x4df6, 0x4df8)
        if srcp <= dst and gv(0x4e1c) == 0 and gv(0x4e1a) == 0:
            di = (gv(0x4e0c) << 4) + gv(0x4e0a)
            si = o.getlin(0x4e02, 0x4e04)
            print("TRC reloc      ctr=%d si=%05x di=%05x srcp=%05x dst=%05x" % (
                counter[0], si, di, srcp, dst), flush=True)
        _phase3()

    o.phase3_relocate = phase3_relocate

    def plot_loop():
        m = o.m
        gv = o.gv
        sv2 = o.sv
        getlin = o.getlin
        setlin2 = o.setlin
        while True:
            if counter[0] % plot_every == 0:
                print(fmt_state(o, "plot[%d]" % counter[0]), flush=True)
            counter[0] += 1
            cnt = (gv(0x4e16) - 1) & 0xFFFF
            sv2(0x4e16, cnt)
            if cnt & 0x8000:
                mp = getlin(0x4e0a, 0x4e0c)
                w32 = (m[mp] << 24) | (m[mp + 1] << 16) | (m[mp + 2] << 8) | m[mp + 3]
                setlin2(0x4e0a, 0x4e0c, mp + 4)
                sv2(0x4e16, 0x1f)
                sv2(0x4e26, (w32 >> 16) & 0xFFFF)
                sv2(0x4e24, w32 & 0xFFFF)
            v = (gv(0x4e26) << 16) | gv(0x4e24)
            cf = (v >> 31) & 1
            v = (v << 1) & 0xFFFFFFFF
            sv2(0x4e26, (v >> 16) & 0xFFFF)
            sv2(0x4e24, v & 0xFFFF)
            if cf == 0:
                o.do_plot()
                if gv(0x4e08) == gv(0x4e04) and gv(0x4e06) == gv(0x4e02):
                    print(fmt_state(o, "plot.done"), flush=True)
                    o.finalize()
                    return
            else:
                o.do_fill()
            o.phase3_relocate()

    o.phase1 = phase1
    o.vec_run_7e = vec_run_7e
    o.run = run
    o.vec_read_record = vec_read_record
    o.plot_loop = plot_loop

    o.vec_run(dispatch_current=False)
    print("TRC end", flush=True)


if __name__ == "__main__":
    main()
