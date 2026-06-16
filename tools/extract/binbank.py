#!/usr/bin/env python3
"""Extract sub-resources from Bumpy .BIN banks (FLECHE.BIN, BUMSPJEU.BIN).

A .BIN opens with a directory of big-endian uint32 offsets into the file. We read
offsets while they stay monotonically increasing and in-range; the run is the
directory, and each entry's blob spans [offset[i], offset[i+1]). The final blob
runs to EOF. (BUMSPJEU's entries are evenly 132 bytes apart -> fixed-size sprite
records; FLECHE has a single entry.) Internal sprite encoding is not decoded here
- this carves the records. See docs/formats/BIN.md.

Usage: binbank.py <file.BIN> ...
Extracts to build/extract/bin/<name>/NNNN.bin and prints a summary.
"""
import sys, os, struct

OUT = "local/build/extract/bin"


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def read_dir(b):
    n = len(b)
    offs = []
    o = 0
    prev = -1
    while o + 4 <= n:
        v = be32(b, o)
        # directory ends once an entry would point past where data must begin,
        # or stops increasing.
        if v <= prev or v >= n:
            break
        if offs and o >= offs[0]:   # we've reached the first blob's data
            break
        offs.append(v)
        prev = v
        o += 4
    return offs


def main():
    for path in sys.argv[1:]:
        b = open(path, "rb").read()
        name = os.path.basename(path)
        offs = read_dir(b)
        outdir = os.path.join(OUT, name)
        os.makedirs(outdir, exist_ok=True)
        bounds = offs + [len(b)]
        sizes = []
        for i in range(len(offs)):
            blob = b[bounds[i]:bounds[i + 1]]
            sizes.append(len(blob))
            open(os.path.join(outdir, "%04d.bin" % i), "wb").write(blob)
        uniq = sorted(set(sizes))
        print("%-14s %6d bytes  dir_entries=%d  first_off=0x%x  blob_sizes(unique)=%s -> %s/" % (
            name, len(b), len(offs), offs[0] if offs else 0, uniq[:6], outdir))


if __name__ == "__main__":
    main()
