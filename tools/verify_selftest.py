#!/usr/bin/env python3
"""Verify the SELFTEST.PLN round-trip: captured planar bytes must exactly match
the synthetic test pattern pat[p*8000+i] = (i + p*37) & 0xFF."""
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
pln_path = os.path.join(ROOT, "local/build/render/SELFTEST.PLN")

data = open(pln_path, "rb").read()
print("PLN file size:", len(data))
planar = data[:32000]
mismatches = 0
for p in range(4):
    for i in range(8000):
        expected = (i + p * 37) & 0xFF
        got = planar[p * 8000 + i]
        if got != expected:
            if mismatches < 5:
                print("MISMATCH at p=%d i=%d: expected %d got %d" % (p, i, expected, got))
            mismatches += 1
print("Total planar mismatches:", mismatches, "(expect 0)")
if mismatches == 0:
    print("PASS: round-trip exact")
    sys.exit(0)
else:
    print("FAIL")
    sys.exit(1)
