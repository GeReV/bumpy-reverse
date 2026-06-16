#!/usr/bin/env python3
"""Pure-Python port of the Bumpy vec_run renderer (work in progress).

Goal: render .PAV/.DEC/.BUM/.VEC resources WITHOUT the emulator, by reimplementing
the game's record interpreter (docs/06-engine.md):

  * The file is a stream of 12-byte big-endian records: w0..w5, where w0:w1 is the
    record's source far-pointer, w4 is the opcode (low 15 bits; 0x8000 = end flag),
    and w5 = w0^w1^w2^w3^w4 (XOR checksum). Records terminate when w0 > 0x0f or the
    opcode goes non-positive (signed).
  * op4  = RLE-decompress a block in place (sliding-window).
  * op12 = recursive vector draw: walks coordinate/vertex lists and rasterises them
    (this is the hard part; being ported incrementally and validated against the
    emulator oracle in results/oracle/).

This scaffold implements the loader, op4, and the record walk/dispatch, and stubs
op12 (logging records). Far-pointer normalisation (the runtime 0xcda helpers) is a
no-op here because we use a single flat bytearray address space.
"""
from __future__ import annotations
import os
import sys
from typing import List, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def be16(b: bytes, o: int) -> int:
    return (b[o] << 8) | b[o + 1]


def rle_decode(data: bytes, start: int, limit: int) -> bytearray:
    """op4 RLE: first payload byte = escape; b!=esc -> literal; esc,esc -> one esc;
    esc,value,count -> value*count (count 0 => 256)."""
    esc = data[start]
    out = bytearray()
    i, n = start + 1, len(data)
    while len(out) < limit and i < n:
        b = data[i]; i += 1
        if b != esc:
            out.append(b); continue
        if i >= n:
            break
        x = data[i]; i += 1
        if x == esc:
            out.append(esc); continue
        if i >= n:
            break
        cnt = data[i]; i += 1
        out.extend(bytes([x]) * (cnt if cnt else 256))
    return out


def record(buf: bytes, o: int) -> Tuple[List[int], bool]:
    """Read a 12-byte record at o -> (w0..w5, checksum_ok)."""
    w = [be16(buf, o + i * 2) for i in range(6)]
    ok = (w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4]) == w[5]
    return w, ok


def load_stream(path: str) -> Tuple[bytearray, str]:
    """Return (decoded record stream, note). op4-first files are RLE-decompressed;
    op12-first / raw files are returned as-is for the interpreter to walk."""
    data = open(path, "rb").read()
    w, ok = record(data, 0)
    op = w[4] & 0x7fff
    if ok and op == 4:
        # w1 is the decoded length per the op4 record (verified against buffer sizes)
        return rle_decode(data, 12, 0x10000), "op4 (RLE-decompressed)"
    return bytearray(data), "raw/op12-first (no outer RLE)"


def walk(stream: bytearray, max_records: int = 40) -> None:
    """Walk + dispatch records the way vec_run does (op12 draw still stubbed)."""
    pos = 0
    from collections import Counter
    ophist: Counter = Counter()
    for r in range(max_records):
        if pos + 12 > len(stream):
            print("  end of stream at %#x" % pos); break
        w, ok = record(stream, pos)
        op = w[4] & 0x7fff
        ophist[op] += 1
        flag = " END" if (w[4] & 0x8000) else ""
        print("  rec%-2d @%#06x  w0..w3=%04x %04x %04x %04x  op=%2d%s chk=%s"
              % (r, pos, w[0], w[1], w[2], w[3], op, flag, "ok" if ok else "BAD"))
        if not ok or w[0] > 0x0f:
            print("  -> terminator (w0>0x0f or bad checksum)"); break
        # NOTE: op handlers (op4 in-place / op12 draw) advance the stream via their
        # own pointer walk; this scaffold steps record-by-record to map structure.
        pos += 12
    print("  opcode histogram:", dict(ophist))


def main() -> None:
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "local/originals/old-games/bumpy/D1.PAV")
    stream, note = load_stream(path)
    print("%s: %s, %d bytes" % (os.path.basename(path), note, len(stream)))
    walk(stream)


if __name__ == "__main__":
    main()
