#!/usr/bin/env python3
"""Byte-exact diff of the C sprite-bank transform output (BSPRITE's SPROUT.BIN,
the transformed data region) against the engine oracle's in-memory bank dump
(bank_inmem.bin) data region.  Exit 0 on exact match, 1 otherwise.

Usage: sprite_diff.py <c_data> <oracle_bank> [data_off=0x800]
"""
import sys


def main() -> None:
    c = open(sys.argv[1], "rb").read()
    data_off = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x800
    r = open(sys.argv[2], "rb").read()[data_off:]
    n = min(len(c), len(r))
    bad = [i for i in range(n) if not (c[i] == r[i])]
    if len(c) == len(r) and not bad:
        print("MATCH exact (%d bytes)" % len(c))
        sys.exit(0)
    if bad:
        i = bad[0]
        print("DIFF at data+%#x: C=%s ref=%s" % (i, c[i:i + 8].hex(), r[i:i + 8].hex()))
    else:
        print("DIFF size: C=%d ref=%d" % (len(c), len(r)))
    sys.exit(1)


if __name__ == "__main__":
    main()
