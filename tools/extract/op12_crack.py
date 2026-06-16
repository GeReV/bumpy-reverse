#!/usr/bin/env python3
"""Reverse op12's level pipeline by tracing the faithful CPU (vec_cpu), toward a
direct pure-Python decoder.

Seeded at the FIRST top-level op12 call (DOSEMU_OP12CALL=1), this replays the whole
render frame on our CPU (using pydos.Machine for the INT/port/VGA environment so the
inter-call game code survives) and characterizes each top-level op12 call: the data
it processes (vec_src/vec_end/stream pointers) and where it writes. That separates
the preprocessing calls (records -> heap buffers) from the final composite (call 8 ->
framebuffer), so we can reimplement each layer.
"""
from __future__ import annotations
import os
import struct
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools/extract"))
import vec_cpu      # noqa: E402
import pydos        # noqa: E402

BR = os.path.join(ROOT, "local/build/render")
OP12_IP = 0x4B0
TOPLEVEL_SP = 0x01B0   # top-level op12 call SP threshold (recursion uses lower)


def main():
    cpu, regs = vec_cpu.load_cpu()
    cs_seg = regs["cs"]; dg = regs["ds"]; dglin = dg << 4
    # environment so the inter-call frame code (timers/VGA ports) doesn't fault
    m = pydos.Machine(level=1)
    m.cpu = cpu; m.dg = dg
    m.alloc = 0x8000; m.alloc_end = 0x9000
    cpu.int_handler = m.on_int
    cpu.io_in = m.on_in; cpu.io_out = m.on_out
    cpu.mmio_lo = 0xA0000; cpu.mmio_read = m.vga_read; cpu.mmio_write = m.vga_write
    cpu.entry_sp = 0x10000              # no SP halt; we stop on call count

    calls = []
    state = {"n": 0, "cur": None, "fbwrites": 0}
    FB_LO, FB_HI = 0x67000, 0x70000

    def rd16(off):
        return cpu.m[dglin + off] | (cpu.m[dglin + off + 1] << 8)

    # track framebuffer writes per current call via wlog (cs:ip ignored here)
    cpu.wlog = (FB_LO, FB_HI, [])

    def hook(c):
        if c.s["cs"] == cs_seg and c.ip == OP12_IP and c.r["sp"] >= TOPLEVEL_SP:
            n = state["n"] + 1; state["n"] = n
            # snapshot framebuffer-write count delta for the previous call
            if state["cur"] is not None:
                state["cur"]["fb_writes"] = len(c.wlog[2]) - state["cur"]["fb_at"]
            entry = {
                "call": n, "sp": c.r["sp"],
                "vec_src": (rd16(0x4E26), rd16(0x4E24)),
                "vec_end": (rd16(0x4E0C), rd16(0x4E0A)),
                "stream": (rd16(0x4E10), rd16(0x4E0E)),
                "es": c.s["es"], "fb_at": len(c.wlog[2]),
            }
            calls.append(entry); state["cur"] = entry
            if n >= 10:
                raise vec_cpu.Halt("traced 10 calls")

    try:
        steps = vec_cpu.execute_until_halt(cpu, cap=40_000_000, hook=hook)
    except vec_cpu.Halt:
        steps = -1
    if state["cur"] is not None:
        state["cur"]["fb_writes"] = len(cpu.wlog[2]) - state["cur"]["fb_at"]

    print("traced %d top-level op12 calls" % len(calls))
    print("\ncall  SP    vec_src(seg:off)  vec_end(seg:off)  stream(seg:off)   ES    fb_writes")
    for e in calls:
        print("  %-3d %04x  %04x:%04x       %04x:%04x       %04x:%04x      %04x  %d" % (
            e["call"], e["sp"],
            e["vec_src"][0], e["vec_src"][1], e["vec_end"][0], e["vec_end"][1],
            e["stream"][0], e["stream"][1], e["es"], e.get("fb_writes", 0)))
    # what regions did all writes hit?
    print("\nall framebuffer-region writes by 16KB:",
          Counter((w[2] & ~0x3FFF) for w in cpu.wlog[2]).most_common(8))


if __name__ == "__main__":
    main()
