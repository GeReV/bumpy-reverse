#!/usr/bin/env python3
"""Diff two unpacked MZ executables — their load-module bodies and relocation tables.

Used in the unpacking write-up (docs/02-unpacking-tinyprog.md) to confirm that our
TinyProg unpack of BUMPY.EXE matches a reference cracked build: how much of the load
module is byte-identical, how the reloc sets overlap, and where the residual byte
differences cluster (as contiguous runs).

Usage: compare_unpacked.py <ours.exe> <cracked.exe>
"""
import struct, sys
from typing import Dict

a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()


def parse(x: bytes) -> Dict:
    """Parse an MZ header -> {body (load module past the header), relocs set, entry regs}."""
    cblp, cp, crlc, cparhdr, mina, maxa, ss, sp, csum, ip, cs, lfarlc = struct.unpack_from("<HHHHHHHHHHHH", x, 2)
    h = cparhdr * 16
    relocs = set(struct.unpack_from("<HH", x, lfarlc + i * 4) for i in range(crlc))
    return dict(body=x[h:], relocs=relocs, cs=cs, ip=ip, ss=ss, sp=sp, nrel=crlc, mina=mina)

A, B = parse(a), parse(b)
n = min(len(A["body"]), len(B["body"]))
same = sum(1 for i in range(n) if A["body"][i] == B["body"][i])
print(f"load_module: ours={len(A['body']):#x} cracked={len(B['body']):#x}; identical {100*same/n:.2f}% over {n:#x}")
print(f"relocs ours={A['nrel']} cracked={B['nrel']}; "
      f"shared={len(A['relocs'] & B['relocs'])} only-ours={len(A['relocs'] - B['relocs'])} only-cracked={len(B['relocs'] - A['relocs'])}")
diffs = [i for i in range(n) if A["body"][i] != B["body"][i]]
print(f"total differing bytes in shared range: {len(diffs)}")
# group diffs into runs
runs = []
for d in diffs:
    if runs and d == runs[-1][1] + 1:
        runs[-1][1] = d
    else:
        runs.append([d, d])
print(f"diff runs: {len(runs)}")
for s, e in runs[:20]:
    print(f"  body[{s:#x}..{e:#x}] ours={A['body'][s:e+1][:16].hex()} cracked={B['body'][s:e+1][:16].hex()}")
