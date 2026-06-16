#!/usr/bin/env python3
"""Recover TinyProg relocation sites by load-segment differencing.

Runs tools/tinyprog_unpack.py twice on BUMPY.EXE at two different load segments
(0x1000 and 0x2000) and compares the resulting memory images: a 16-bit word that
changed by exactly the segment delta (0x1000) is a segment fixup the stub applied,
i.e. a relocation entry. The delta histogram should be dominated by that one value.
Run from the repo root (paths are relative). See docs/02-unpacking-tinyprog.md.
"""
import importlib.util
from collections import Counter

spec = importlib.util.spec_from_file_location("tp", "tools/tinyprog_unpack.py")
assert spec and spec.loader
tp = importlib.util.module_from_spec(spec); spec.loader.exec_module(tp)
mz = tp.load_mz("local/originals/old-games/bumpy/BUMPY.EXE")
mem1, i1 = tp.run_full(mz, 0x1000)
mem2, i2 = tp.run_full(mz, 0x1000 + 0x1000)


def w(m: bytes, p: int) -> int:
    return m[p] | (m[p + 1] << 8)

b1 = (0x1000 + 0x10) * 16
b2 = (0x2000 + 0x10) * 16
for off in (0x1, 0xbd, 0xf7, 0x757d):
    a = w(mem1, b1 + off); b = w(mem2, b2 + off)
    print(f"off {off:#06x}: run1={a:#06x} run2={b:#06x} delta={(b - a) & 0xffff:#06x}")

c = Counter()
N = i1["top"] - b1
for off in range(0, N - 1, 2):
    a = w(mem1, b1 + off); b = w(mem2, b2 + off)
    if a != b:
        c[(b - a) & 0xffff] += 1
print("delta histogram (most common):", c.most_common(6))
