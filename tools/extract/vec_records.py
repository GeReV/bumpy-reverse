#!/usr/bin/env python3
"""Pure-Python .VEC/.PAV/.DEC/.BUM record walker (NO emulation).

Each record is 6 big-endian uint16 words: w0 w1 w2 w3 w4 w5, where
  w4 = opcode (low 15 bits; bit 0x8000 is a flag)
  w5 = XOR checksum = w0 ^ w1 ^ w2 ^ w3 ^ w4
After a record's 12-byte head comes a variable amount of inline data (the opcode
determines how much). Because every record is self-checking, we can find record
boundaries by walking: validate a record, then scan forward (word-aligned) to the
next position whose checksum validates -> the gap is that record's inline length.

This recovers the opcode -> inline-length map empirically, with no DOS binary.

Usage: vec_records.py <file> [maxrecords]
"""
import sys, collections


def be(b, o):
    return (b[o] << 8) | b[o + 1]


def valid(b, p):
    if p + 12 > len(b):
        return False
    w = [be(b, p + 2 * i) for i in range(6)]
    return (w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4]) == w[5]


def walk(b):
    pos, recs = 0, []
    if not valid(b, 0):
        return recs, "header record @0 does NOT validate"
    while pos + 12 <= len(b):
        w = [be(b, pos + 2 * i) for i in range(6)]
        op = w[4] & 0x7FFF
        flag = w[4] >> 15
        nxt = pos + 12
        while nxt + 12 <= len(b) and not valid(b, nxt):
            nxt += 2
        inline = nxt - (pos + 12)
        recs.append((pos, op, flag, w, inline))
        if nxt + 12 > len(b):
            break
        pos = nxt
    return recs, None


def dump_blobs(path, b, recs):
    import os
    name = os.path.basename(path)
    outdir = os.path.join("local/build/extract/vec", name)
    os.makedirs(outdir, exist_ok=True)
    rows = ["rec,pos,opcode,flag,w0,w1,w2,w3,inline_len"]
    for idx, (pos, op, flag, w, inline) in enumerate(recs):
        blob = b[pos + 12:pos + 12 + inline]
        open(os.path.join(outdir, "rec%02d_op%05d.bin" % (idx, op)), "wb").write(blob)
        rows.append("%d,%#x,%d,%d,%#x,%#x,%#x,%#x,%d" % (idx, pos, op, flag, w[0], w[1], w[2], w[3], inline))
    open(os.path.join(outdir, "records.csv"), "w").write("\n".join(rows) + "\n")
    return outdir


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path = args[0]
    cap = int(args[1]) if len(args) > 1 else 0
    do_dump = "--dump" in sys.argv
    b = open(path, "rb").read()
    recs, err = walk(b)
    if do_dump and recs:
        print("  dumped %d record blobs -> %s/" % (len(recs), dump_blobs(path, b, recs)))
    print("file: %s (%d bytes)" % (path, len(b)))
    if err:
        print("  WARN:", err)
    covered = (recs[-1][0] + 12 + recs[-1][4]) if recs else 0
    print("  records: %d, bytes covered: %d/%d (%.1f%%)" % (
        len(recs), covered, len(b), 100.0 * covered / len(b)))
    oplen = collections.defaultdict(collections.Counter)
    opcount = collections.Counter()
    for _, op, flag, w, inline in recs:
        oplen[op][inline] += 1
        opcount[op] += 1
    print("  opcode -> count, inline-length histogram:")
    for op in sorted(opcount):
        lens = ", ".join("%dB×%d" % (L, n) for L, n in sorted(oplen[op].items()))
        print("    op%-2d: %4d recs   inline: %s" % (op, opcount[op], lens))
    n = cap or 16
    print("  first %d records (pos op flag w0..w3 inline):" % n)
    for pos, op, flag, w, inline in recs[:n]:
        print("    @%05x op%-2d f%d  %04x %04x %04x %04x  +%dB" % (
            pos, op, flag, w[0], w[1], w[2], w[3], inline))


if __name__ == "__main__":
    main()
