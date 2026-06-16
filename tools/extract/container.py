#!/usr/bin/env python3
"""Parse the shared Bumpy container format used by .VEC/.PAV/.DEC/.BUM.

All four share an 8-byte big-endian header followed by a command/data body that
the in-game interpreter `vec_run` (overlay seg 1c28) consumes as big-endian
16-bit words: a word whose low 15 bits are < 0x10 is an opcode (dispatched via
the table at DGROUP 0x4e37), otherwise it is coordinate/data. Opcode handler
operand consumption is variable, so the token view below is structural, not a
full decode (see docs/formats/VEC.md).

Usage: container.py <file.{VEC,PAV,DEC,BUM}> ...
Writes a per-file report to build/extract/container/<name>.txt and prints a
one-line summary per file.
"""
import sys, os, struct, collections

OUT = "local/build/extract/container"


def be16(b, o):
    return (b[o] << 8) | b[o + 1]


def parse(path):
    b = open(path, "rb").read()
    n = len(b)
    hdr = {
        "magic_w0": be16(b, 0),          # always 0x0000
        "decoded_size": be16(b, 2),      # ~ rendered/output buffer size in bytes
        "cksum_a": be16(b, 4),
        "cksum_b": be16(b, 6),
    }
    # Tokenize the body as big-endian words.
    opc = collections.Counter()
    coords = 0
    tokens = []
    o = 8
    while o + 1 < n:
        w = be16(b, o)
        low = w & 0x7fff
        if 0 < low < 0x10:
            opc[low] += 1
            tokens.append(("op", low, w & 0x8000))
        elif low == 0:
            tokens.append(("zero", 0, 0))
        else:
            coords += 1
            tokens.append(("xy", w, 0))
        o += 2
    return b, hdr, opc, coords, tokens


def main():
    os.makedirs(OUT, exist_ok=True)
    for path in sys.argv[1:]:
        b, hdr, opc, coords, tokens = parse(path)
        name = os.path.basename(path)
        lines = []
        lines.append("file: %s  (%d bytes)" % (name, len(b)))
        lines.append("header (8 bytes, big-endian):")
        lines.append("  w0 (magic, =0)     : 0x%04x" % hdr["magic_w0"])
        lines.append("  w1 (decoded_size)  : 0x%04x  (%d)" % (hdr["decoded_size"], hdr["decoded_size"]))
        lines.append("  w2 (checksum a)    : 0x%04x" % hdr["cksum_a"])
        lines.append("  w3 (checksum b)    : 0x%04x" % hdr["cksum_b"])
        lines.append("body word stats: %d opcode-words, %d coord/data-words" % (sum(opc.values()), coords))
        lines.append("opcode histogram (low15<0x10): " +
                     ", ".join("op%d=%d" % (k, opc[k]) for k in sorted(opc)))
        lines.append("first 48 body tokens:")
        row = []
        for t in tokens[:48]:
            if t[0] == "op":
                row.append("[OP%d%s]" % (t[1], "+flag" if t[2] else ""))
            elif t[0] == "zero":
                row.append(".")
            else:
                row.append("%04x" % t[1])
        lines.append("  " + " ".join(row))
        open(os.path.join(OUT, name + ".txt"), "w").write("\n".join(lines) + "\n")
        print("%-13s size=%-6d decoded=0x%04x cksum=%04x:%04x opcodes={%s} coords=%d" % (
            name, len(b), hdr["decoded_size"], hdr["cksum_a"], hdr["cksum_b"],
            ",".join("%d:%d" % (k, opc[k]) for k in sorted(opc)), coords))


if __name__ == "__main__":
    main()
