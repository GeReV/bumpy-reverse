#!/usr/bin/env python3
"""Walk a .PAV/.VEC record stream the way vec_run does: op4 records are RLE and
consume a computed payload length; report each record's real dispatch opcode w4."""
from __future__ import annotations
import sys


def main() -> None:
    d = open(sys.argv[1], "rb").read()

    def be(o: int) -> int:
        return (d[o] << 8) | d[o + 1]

    def rec(o: int) -> tuple[list[int], bool]:
        w = [be(o + i * 2) for i in range(6)]
        chk = w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4]
        return w, chk == w[5]

    def rle_consume(start: int, limit: int) -> int:
        esc = d[start]
        i = start + 1
        out = 0
        while out < limit and i < len(d):
            b = d[i]
            i += 1
            if b != esc:
                out += 1
                continue
            if i >= len(d):
                break
            x = d[i]
            i += 1
            if x == esc:
                out += 1
                continue
            if i >= len(d):
                break
            cnt = d[i]
            i += 1
            out += cnt if cnt else 256
        return i - start

    pos = 0
    for r in range(12):
        if pos + 12 > len(d):
            print("  end of stream at %d" % pos)
            break
        w, ok = rec(pos)
        op = w[4]
        print("rec%d @%#06x  w0..w3=%04x %04x %04x %04x  op=%d (0x%x) chk_ok=%s"
              % (r, pos, w[0], w[1], w[2], w[3], op, op, ok))
        if op == 4:
            plen = rle_consume(pos + 12, w[1])
            print("      op4 RLE: %d decoded from %d payload bytes -> next @%#06x"
                  % (w[1], plen, pos + 12 + plen))
            pos = pos + 12 + plen
        elif op <= 0:
            print("      terminator (op<=0) -> vec_run loop exits")
            break
        else:
            print("      non-op4 opcode -> dispatch table idx %d" % ((op & 0x7fff) - 1))
            break


if __name__ == "__main__":
    main()
