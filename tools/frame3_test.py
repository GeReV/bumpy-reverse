#!/usr/bin/env python3
"""Sanity test for the FRM3 oracle reader.

Run:
    timeout 120 uv run python tools/frame3_test.py

Expected: prints "FRM3 reader OK: level=... p1_obj head=..."
"""
import sys
import importlib.util

spec = importlib.util.spec_from_file_location("cc", "tools/composite_check.py")
cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cc)

src = sys.argv[1] if len(sys.argv) > 1 else "local/build/render/frame_oracle.bin"
d = cc.load_frame3(src)

assert d["tag"] == b"FRM3", d["tag"]
assert len(d["bum"]) == 0xc2, len(d["bum"])
assert len(d["p1_obj"]) == 0x18 and len(d["p2_obj"]) == 0x18
assert len(d["chan_a"]) == 3 and len(d["chan_b"]) == 4
# Each channel record must be exactly 0xc bytes
for rec in d["chan_a"]:
    assert len(rec) == 0xc, len(rec)
for rec in d["chan_b"]:
    assert len(rec) == 0xc, len(rec)
# DG block must be the full 64KB
assert len(d["dg"]) == 0x10000, len(d["dg"])
# chan_tbl_raw: 4 u16 words (8 bytes)
assert len(d["chan_tbl_raw"]) == 8, len(d["chan_tbl_raw"])

print("FRM3 reader OK: level=%d p1_obj head=%s" % (d["level"], d["p1_obj"][:6].hex()))
print("  bum[0x90]=0x%02x (P1 spawn cell)  bum[0x93]=0x%02x (P2 spawn; 0=none)" % (
    d["bum"][0x90], d["bum"][0x93]))
print("  p1_glob hex=%s  p2_glob hex=%s" % (d["p1_glob"].hex(), d["p2_glob"].hex()))
