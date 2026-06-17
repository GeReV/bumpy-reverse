#!/usr/bin/env python3
"""Linear 16-bit disassembly of a raw code blob (capstone).

Usage: disasm16.py <file> <base-hex> [count]
Prints `off: bytes  mnemonic operands` so addresses match Ghidra seg:off.
Linear sweep only — interleaved jump-table data will desync; re-anchor by
passing a new base at a known instruction boundary.
"""
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_16


def main() -> None:
    path = sys.argv[1]
    base = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    with open(path, "rb") as f:
        code = f.read()
    md = Cs(CS_ARCH_X86, CS_MODE_16)
    md.detail = False
    n = 0
    for ins in md.disasm(code, base):
        hexb = ins.bytes.hex()
        print(f"{ins.address:04x}: {hexb:<14} {ins.mnemonic} {ins.op_str}")
        n += 1
        if count and n >= count:
            break
    print(f"; {n} instructions")


if __name__ == "__main__":
    main()
