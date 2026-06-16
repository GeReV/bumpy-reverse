#!/usr/bin/env python3
"""Parse a standard AdLib instrument bank (.BNK) per the published AdLib format.

This is NOT a Loriciel-custom format: BUMPY.BNK uses the documented AdLib Inc.
BNK layout (little-endian, "ADLIB-" signature), the same format adplug/AdPlug and
other OPL2 tools already read. We parse the header + name index and dump the raw
30-byte OPL2 instrument records so existing tooling can consume them.

Reference layout:
  Header (28 bytes, little-endian):
    u8  version_major, u8 version_minor
    char signature[6]   = "ADLIB-"
    u16 num_used
    u16 num_instruments
    u32 offset_names
    u32 offset_data
    u8  filler[8]
  Name index @ offset_names: num_instruments * { u16 index; u8 used; char name[9] }
  Instrument data @ offset_data: 30-byte OPL2 register records (modulator+carrier)

Usage: bnkbank.py <file.BNK> ...  -> build/extract/bnk/<name>/{instruments.csv,NNN_<name>.sbi}
"""
import sys, os, struct

OUT = "local/build/extract/bnk"


def main():
    for path in sys.argv[1:]:
        b = open(path, "rb").read()
        name = os.path.basename(path)
        ver = (b[0], b[1])
        sig = b[2:8].decode("latin1")
        num_used, num_inst = struct.unpack_from("<HH", b, 8)
        off_names, off_data = struct.unpack_from("<II", b, 12)
        outdir = os.path.join(OUT, name)
        os.makedirs(outdir, exist_ok=True)
        rows = ["index,used,name,data_offset"]
        names = []
        for i in range(num_inst):
            rec = off_names + i * 12
            if rec + 12 > len(b):
                break
            idx, used = struct.unpack_from("<HB", b, rec)
            nm = b[rec + 3:rec + 12].split(b"\0")[0].decode("latin1", "replace")
            data_off = off_data + idx * 30
            rows.append("%d,%d,%s,0x%x" % (idx, used, nm, data_off))
            if used:
                names.append(nm)
            # carve the 30-byte OPL2 instrument record
            inst = b[data_off:data_off + 30]
            if len(inst) == 30:
                safe = "".join(c if c.isalnum() else "_" for c in nm) or ("inst%03d" % i)
                open(os.path.join(outdir, "%03d_%s.sbi.raw" % (i, safe)), "wb").write(inst)
        open(os.path.join(outdir, "instruments.csv"), "w").write("\n".join(rows) + "\n")
        print("%-12s ver=%d.%d sig=%r used=%d total=%d names@0x%x data@0x%x -> %d named, %s/" % (
            name, ver[0], ver[1], sig, num_used, num_inst, off_names, off_data, len(names), outdir))
        print("   sample instruments:", ", ".join(names[:12]))


if __name__ == "__main__":
    main()
