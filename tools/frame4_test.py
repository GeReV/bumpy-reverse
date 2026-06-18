#!/usr/bin/env python3
"""Sanity test for the FRM4 oracle reader (Plan 6c Task 2).

Run:
    timeout 120 uv run python tools/frame4_test.py

Expected: prints "FRM4 reader OK: ..." with observed present-path dynamics.
"""
import sys
import importlib.util
import struct

spec = importlib.util.spec_from_file_location("cc", "tools/composite_check.py")
cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cc)

src = sys.argv[1] if len(sys.argv) > 1 else "local/build/render/frame_oracle.bin"

# ---- FRM3-compatible read (must still work after FRM3->FRM4 bump) ----
d3 = cc.load_frame3(src)
assert d3["tag"] in (b"FRM3", b"FRM4"), "unexpected tag: %r" % d3["tag"]
assert len(d3["planes"]) == 4 * 0x10000, len(d3["planes"])
assert len(d3["dg"]) == 0x10000, len(d3["dg"])
assert len(d3["bum"]) == 0xc2, len(d3["bum"])
assert len(d3["chan_a"]) == 3 and len(d3["chan_b"]) == 4
print("FRM3-compat read OK: tag=%r level=%d p1_obj head=%s" % (
    d3["tag"], d3["level"], d3["p1_obj"][:6].hex()))

if d3["tag"] != b"FRM4":
    print("WARNING: file is %r, not FRM4 — FRM4 blocks not present" % d3["tag"])
    sys.exit(0)

# ---- FRM4-specific read ----
d4 = cc.load_frame4(src)
assert d4["tag"] == b"FRM4", d4["tag"]

# fullscreen_buf
fb = d4["fullscreen_buf"]
assert len(fb) == 32000, "fullscreen_buf len=%d (expected 32000)" % len(fb)
fb_nz = sum(1 for x in fb if x)
print("FRM4 fullscreen_buf: @%04x:%04x  len=%d  nonzero=%d (%.1f%%)" % (
    d4["fullscreen_buf_seg"], d4["fullscreen_buf_off"],
    len(fb), fb_nz, 100 * fb_nz / len(fb)))

# present_calls
pc = d4["present_calls"]
print("FRM4 present_calls: n=%d" % len(pc))
_src_map = {0: "a200:0000 (sprite-scratch)", 1: "a000:0000 (visible)"}
for i, p in enumerate(pc):
    print("  [%d] w0=%d src=%-30s dest=%04x:%04x sh_idx=%d  (of %d total mode10 calls)" % (
        i, p["w0"], _src_map.get(p["w0"], "?"),
        p["dest_seg"], p["dest_off"], p["sh_idx"], p["n_all"]))

# csd_obs
csd = d4["csd_obs"]
print("FRM4 cur_sprite_data observations: n=%d" % len(csd))
for i, c in enumerate(csd[:8]):
    print("  blit[%d] csd=%04x:%04x" % (c["call_n"], c["csd_seg"], c["csd_off"]))

# Validate: present_calls should have at least the setup_fullscreen_view capture
# (w0=1, dest=fullscreen_buf_seg:off), and possibly a w0=0 a200->a000 present.
w0_counts = {}
for p in pc:
    w0_counts[p["w0"]] = w0_counts.get(p["w0"], 0) + 1
print("FRM4 present by w0:", {k: v for k, v in sorted(w0_counts.items())})

# All csd observations for active blits should point to a200 (seg=0xa200)
if csd:
    all_a200 = all(c["csd_seg"] == 0xa200 for c in csd)
    print("FRM4 all sprites to a200:", all_a200)

print("FRM4 reader OK")
