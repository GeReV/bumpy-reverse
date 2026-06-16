#!/usr/bin/env python3
"""Pure-Python op12 decoder primitive (no CPU emulation) — a compact reference.

This documents the masked-RLE expander at op12's heart, reverse-engineered by
tracing the real renderer. Each op12 call expands a source block laid out as
[pixel bytes][mask bitstream] into a contiguous destination buffer. The mask is a
stream of 32-bit BIG-ENDIAN words consumed MSB-first, one bit per output byte:

    mask bit 0 -> copy the next pixel byte
    mask bit 1 -> write 0xFF (transparent)

This was validated byte-for-byte against the Unicorn trace of level-1 call 1. The
production renderer uses the fuller state-machine port in op12_port.py; this file
is a standalone, readable model of the core codec.
"""
from __future__ import annotations


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


def decode_pav_block(dec: bytes) -> bytearray:
    """Decode one op12 block from an op4-decompressed level stream. Layout (validated
    against level 1 D1.PAV): [14-byte header][mask bitstream][pixel bytes], where the
    header's big-endian word at +2 = out_len. mask starts at +0xe, pixels right after
    the mask (out_len bits -> ceil(out_len/32)*4 bytes). Returns the expanded buffer."""
    out_len = (dec[2] << 8) | dec[3]
    mask_off = 0x0e
    mask_bytes = ((out_len + 31) // 32) * 4
    pix_off = mask_off + mask_bytes
    return expand(dec[pix_off:], dec[mask_off:], out_len)
