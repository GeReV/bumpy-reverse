#!/usr/bin/env python3
"""Pure-Python op12 decoder primitive (no CPU emulation).

Reverse-engineered from tracing the real renderer (see op12_crack.py): each op12
call expands a source block laid out as [pixel bytes][mask bitstream] into a
contiguous destination buffer. The mask is a stream of 32-bit BIG-ENDIAN words
consumed MSB-first, one bit per output byte:

    mask bit 0 -> copy the next pixel byte
    mask bit 1 -> write 0xFF (transparent)

This is the masked-RLE expander at op12's heart; validated byte-for-byte against the
Unicorn trace of level-1 call 1 (run __main__ to check).
"""
from __future__ import annotations
import os
import struct


def expand(pixels: bytes, mask: bytes, out_len: int) -> bytearray:
    """Expand [pixels]+[mask] -> out_len bytes. mask = 32-bit BE words, MSB-first;
    bit 0 = copy next pixel, bit 1 = 0xFF transparent."""
    out = bytearray(out_len)
    pi = 0
    word = 0
    nbits = 0
    mi = 0
    for i in range(out_len):
        if nbits == 0:
            word = (mask[mi] << 24) | (mask[mi + 1] << 16) | (mask[mi + 2] << 8) | mask[mi + 3]
            mi += 4
            nbits = 32
        bit = (word >> 31) & 1
        word = (word << 1) & 0xFFFFFFFF
        nbits -= 1
        if bit:
            out[i] = 0xFF
        else:
            out[i] = pixels[pi]
            pi += 1
    return out


def decode_pav_block(dec: bytes):
    """Decode one op12 block from an op4-decompressed level stream. Layout (validated
    against level 1 D1.PAV): [14-byte header][mask bitstream][pixel bytes], where the
    header's big-endian word at +2 = out_len. mask starts at +0xe, pixels right after
    the mask (out_len bits -> ceil(out_len/32)*4 bytes). Returns the expanded buffer."""
    out_len = (dec[2] << 8) | dec[3]
    mask_off = 0x0e
    mask_bytes = ((out_len + 31) // 32) * 4
    pix_off = mask_off + mask_bytes
    return expand(dec[pix_off:], dec[mask_off:], out_len)


def _validate():
    """Reproduce level-1 call 1's output from its traced pixel+mask data."""
    R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    BR = os.path.join(R, "local/build/render")
    seq = open(os.path.join(BR, "op12_call_seq.bin"), "rb").read()
    ops = [struct.unpack_from("<IBIB", seq, i) for i in range(0, len(seq), 10)]
    output_truth = bytes(o[3] for o in ops)                  # the real dest bytes, in order
    pixels = bytes(o[3] for o in ops if o[1])                # source pixels (copies only)
    mr = open(os.path.join(BR, "op12_maskreads.bin"), "rb").read()
    mask = b"".join(mr[i + 4:i + 8] for i in range(0, len(mr), 8))
    out = expand(pixels, mask, len(ops))
    ok = bytes(out) == output_truth
    print("call 1: %d out bytes, %d pixels, %d mask bytes -> %s" % (
        len(ops), len(pixels), len(mask), "EXACT MATCH" if ok else "MISMATCH"))
    if not ok:
        diff = next(i for i in range(len(ops)) if out[i] != output_truth[i])
        print("  first diff at %d: got %02x want %02x" % (diff, out[diff], output_truth[diff]))


if __name__ == "__main__":
    _validate()
