#!/usr/bin/env python3
"""Emulator harness for the reconstructed BVEC.EXE (the VEC render core slice).

Boots BVEC.EXE on the project's pure-Python 16-bit CPU (tools/emu/vec_cpu.py),
services the INT 21h DOS file calls it makes (open/read/close/seek + a *real*
create/write so the decoded image lands on the host disk), and runs to program
exit. Input .VEC reads are resolved against --in-dir; the file the program
creates is written under --out-dir.

VGA model: the Host class now contains a faithful port of the mode-0D planar VGA
hardware model from tools/emu/pydos.py (vga_write, vga_read, on_out for ports
0x3C4/0x3C5/0x3CE/0x3CF/0x3C8/0x3C9).  Plane-major writes made by
video_blit_planar() are captured in self.plane[0..3], and dumped to
local/build/render/<name>.PLN after the run (32000 planar bytes + 768 DAC bytes).

Usage:
    python3 tools/run_bvec.py --exe local/build/src/BVEC.EXE \\
        --in-dir local/build/capture/game --out-dir local/build/render \\
        --args "TITRE.VEC TITRE.PLN"

    # selftest (no .VEC needed):
    python3 tools/run_bvec.py --args "--selftest"
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from typing import Dict, List, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools/emu"))
import vec_cpu  # noqa: E402

PSP_SEG = 0x0100
RAM = 0x110000


def load_mz(path: str) -> Tuple[bytes, List[Tuple[int, int]], Dict[str, int]]:
    with open(path, "rb") as f:
        x = f.read()
    e_crlc, e_cparhdr = struct.unpack_from("<HH", x, 6)
    e_ss, e_sp, _chk, e_ip, e_cs = struct.unpack_from("<HHHHH", x, 0x0E)
    e_lfarlc = struct.unpack_from("<H", x, 0x18)[0]
    img = x[e_cparhdr * 16:]
    relocs = [struct.unpack_from("<HH", x, e_lfarlc + i * 4) for i in range(e_crlc)]
    return img, relocs, dict(ss=e_ss, sp=e_sp, ip=e_ip, cs=e_cs)


class Host:
    """DOS host: INT 21h file I/O + planar-VGA hardware model (mode 0x0D).

    VGA model ported faithfully from tools/emu/pydos.py (Machine class).
    Plane data is captured from writes to 0xA0000-0xAFFFF via the CPU's
    mmio_write hook.  DAC state is captured from OUT 0x3C8/0x3C9.
    """

    def __init__(self, exe: str, in_dir: str, out_dir: str) -> None:
        self.exe = exe
        self.in_dir = in_dir
        self.out_dir = out_dir
        os.makedirs(out_dir, exist_ok=True)
        # input files resolvable by (upper-cased) basename
        self.in_files: Dict[str, str] = {}
        for f in os.listdir(in_dir):
            self.in_files[f.upper()] = os.path.join(in_dir, f)
        self.read_handles: Dict[int, object] = {}
        self.write_handles: Dict[int, object] = {}
        self.next_handle = 5
        self.exited: Optional[int] = None
        self.cpu: Optional[vec_cpu.CPU] = None
        self.written: List[str] = []

        # ---- VGA hardware state (ported from pydos.py Machine.__init__) ----
        # 4 planes of 64 KB each; only [0:8000] is the mode-0D framebuffer.
        self.plane: List[bytearray] = [bytearray(0x10000) for _ in range(4)]
        # Sequencer and GC register files.
        self.seq: bytearray = bytearray(0x20)
        self.gc: bytearray = bytearray(0x20)
        # DAC: 256 entries of [R, G, B] (6-bit each, values 0..63).
        self.dac: List[List[int]] = [[0, 0, 0] for _ in range(256)]
        # VGA latch registers (one byte per plane).
        self.latch: List[int] = [0, 0, 0, 0]
        # Internal index/sub-channel state.
        self.seq_i: int = 0
        self.gc_i: int = 0
        self.dac_i: int = 0
        self.dac_sub: int = 0

    # ---- boot ----
    def load(self, cmdtail: str) -> None:
        img, relocs, hdr = load_mz(self.exe)
        base = PSP_SEG + 0x10
        mem = bytearray(RAM)
        mem[base * 16:base * 16 + len(img)] = img
        for off, seg in relocs:
            lin = base * 16 + ((seg * 16 + off) & 0xFFFFF)
            v = ((mem[lin] | (mem[lin + 1] << 8)) + base) & 0xFFFF
            mem[lin] = v & 0xFF
            mem[lin + 1] = v >> 8
        # PSP: INT 20h at offset 0, top-of-memory word, command tail at 0x80.
        mem[PSP_SEG * 16] = 0xCD
        mem[PSP_SEG * 16 + 1] = 0x20
        mem[PSP_SEG * 16 + 2] = 0x00
        mem[PSP_SEG * 16 + 3] = 0xA0
        tail = cmdtail.encode("latin1")[:126]
        mem[PSP_SEG * 16 + 0x80] = len(tail)
        mem[PSP_SEG * 16 + 0x81:PSP_SEG * 16 + 0x81 + len(tail)] = tail
        mem[PSP_SEG * 16 + 0x81 + len(tail)] = 0x0D
        cpu = vec_cpu.CPU(mem)
        cpu.s["ds"] = PSP_SEG
        cpu.s["es"] = PSP_SEG
        cpu.s["ss"] = (hdr["ss"] + base) & 0xFFFF
        cpu.r["sp"] = hdr["sp"]
        cpu.s["cs"] = (hdr["cs"] + base) & 0xFFFF
        cpu.ip = hdr["ip"]
        cpu.flags = 0x0202
        cpu.entry_sp = 0x10000
        cpu.int_handler = self.on_int
        cpu.io_in = self.on_in
        cpu.io_out = self.on_out
        # ---- wire VGA MMIO hooks (matches pydos.py Machine.load()) ----
        cpu.mmio_lo = 0xA0000
        cpu.mmio_read = self.vga_read
        cpu.mmio_write = self.vga_write
        self.cpu = cpu
        self.base = base
        self.alloc = base + (len(img) + 15) // 16 + 0x100
        if self.alloc < 0x1C00:
            self.alloc = 0x1C00
        self.alloc_end = 0x9000
        # default IRET stub at 0050:0000 into every still-null IVT vector
        mem[0x500] = 0xCF
        for v in range(0x100):
            cur = (mem[v * 4] | (mem[v * 4 + 1] << 8)
                   | (mem[v * 4 + 2] << 16) | (mem[v * 4 + 3] << 24))
            if cur == 0:
                mem[v * 4] = 0x00
                mem[v * 4 + 1] = 0x00
                mem[v * 4 + 2] = 0x50
                mem[v * 4 + 3] = 0x00

    # ---- helpers ----
    def cf(self, on: bool) -> None:
        assert self.cpu is not None
        self.cpu.flags = (self.cpu.flags | 1) if on else (self.cpu.flags & ~1)

    def rdstr(self, seg: int, off: int) -> bytes:
        assert self.cpu is not None
        out = b""
        while True:
            c = self.cpu.m[(seg << 4) + off]
            if c == 0:
                break
            out += bytes([c])
            off += 1
        return out

    @staticmethod
    def basename(name: str) -> str:
        key = name.upper().split("\\")[-1].split("/")[-1].strip()
        return key

    # ---- INT dispatch ----
    def on_int(self, cpu: "vec_cpu.CPU", n: int) -> bool:
        ax = cpu.r["ax"]
        ah = (ax >> 8) & 0xFF
        al = ax & 0xFF
        if n == 0x21:
            return self.int21(cpu, ah, al)
        if n == 0x10:
            # INT 10h: BIOS video — just accept the mode set silently.
            return True
        if n == 0x20:
            self.exited = 0
            cpu.halted = True
            return True
        # unknown software int: iret-noop
        return True

    def int21(self, cpu: "vec_cpu.CPU", ah: int, al: int) -> bool:
        if ah == 0x4C:
            self.exited = al
            cpu.halted = True
            return True
        if ah == 0x30:
            cpu.r["ax"] = 0x0005
            return True
        if ah == 0x25:                       # set vector AL -> DS:DX
            cpu.w16(al * 4, cpu.r["dx"])
            cpu.w16(al * 4 + 2, cpu.s["ds"])
            return True
        if ah == 0x35:                       # get vector AL -> ES:BX
            cpu.r["bx"] = cpu.r16(al * 4)
            cpu.s["es"] = cpu.r16(al * 4 + 2)
            return True
        if ah in (0x1A, 0x2C, 0x2A, 0x44, 0x33, 0x19, 0x0E, 0x09):
            return True                      # date/ioctl/etc — ignore
        if ah == 0x48:                       # allocate BX paragraphs
            bx = cpu.r["bx"]
            avail = self.alloc_end - self.alloc
            if bx > avail:
                cpu.r["ax"] = 8
                cpu.r["bx"] = avail
                self.cf(True)
            else:
                cpu.r["ax"] = self.alloc
                self.alloc += bx
                self.cf(False)
            return True
        if ah in (0x49, 0x4A):
            self.cf(False)
            return True
        if ah == 0x3D:                       # open DS:DX for read
            name = self.rdstr(cpu.s["ds"], cpu.r["dx"]).decode("latin1")
            path = self.in_files.get(self.basename(name))
            if not path:
                cpu.r["ax"] = 2
                self.cf(True)
                return True
            h = self.next_handle
            self.next_handle += 1
            self.read_handles[h] = open(path, "rb")
            cpu.r["ax"] = h
            self.cf(False)
            return True
        if ah == 0x3C:                       # create DS:DX (truncate) for write
            name = self.rdstr(cpu.s["ds"], cpu.r["dx"]).decode("latin1")
            out_path = os.path.join(self.out_dir, self.basename(name))
            h = self.next_handle
            self.next_handle += 1
            self.write_handles[h] = open(out_path, "wb")
            self.written.append(out_path)
            cpu.r["ax"] = h
            self.cf(False)
            return True
        if ah == 0x3F:                       # read BX -> DS:DX, CX bytes
            h = cpu.r["bx"]
            cx = cpu.r["cx"]
            dst = (cpu.s["ds"] << 4) + cpu.r["dx"]
            f = self.read_handles.get(h)
            data = f.read(cx) if f else b""
            cpu.m[dst:dst + len(data)] = data
            if len(data) < cx:               # zero unread tail (decoders read in place)
                for i in range(len(data), cx):
                    cpu.m[dst + i] = 0
            cpu.r["ax"] = len(data)
            self.cf(False)
            return True
        if ah == 0x40:                       # write BX <- DS:DX, CX bytes
            h = cpu.r["bx"]
            cx = cpu.r["cx"]
            src = (cpu.s["ds"] << 4) + cpu.r["dx"]
            if h in (1, 2):
                dest = sys.stdout.buffer if h == 1 else sys.stderr.buffer
                dest.write(bytes(cpu.m[src:src + cx]))
                dest.flush()
                cpu.r["ax"] = cx
                self.cf(False)
                return True
            f = self.write_handles.get(h)
            if f:
                f.write(bytes(cpu.m[src:src + cx]))
                cpu.r["ax"] = cx
                self.cf(False)
            else:
                cpu.r["ax"] = 6              # invalid handle
                self.cf(True)
            return True
        if ah == 0x3E:                       # close
            h = cpu.r["bx"]
            if h in self.read_handles:
                self.read_handles.pop(h).close()
            elif h in self.write_handles:
                self.write_handles.pop(h).close()
            self.cf(False)
            return True
        if ah == 0x42:                       # seek
            h = cpu.r["bx"]
            f = self.read_handles.get(h) or self.write_handles.get(h)
            if f:
                off = (cpu.r["cx"] << 16) | cpu.r["dx"]
                f.seek(off, al & 0xFF if al in (0, 1, 2) else 0)
                p = f.tell()
                cpu.r["dx"] = (p >> 16) & 0xFFFF
                cpu.r["ax"] = p & 0xFFFF
            self.cf(False)
            return True
        return True                          # default: succeed/no-op

    # ---- ports (VGA sequencer / GC / DAC) ----
    # Ported faithfully from tools/emu/pydos.py Machine.on_out / Machine.on_in.

    def on_in(self, cpu: "vec_cpu.CPU", port: int, size: int) -> int:
        return 0xFF

    def on_out(self, cpu: "vec_cpu.CPU", port: int, size: int, value: int) -> None:
        value &= 0xFFFF
        if port in (0x3C4, 0x3CE):
            # Index write: latch index + optionally the data byte (16-bit OUT).
            reg = self.seq if port == 0x3C4 else self.gc
            if port == 0x3C4:
                self.seq_i = value & 0xFF
            else:
                self.gc_i = value & 0xFF
            if size == 2:
                reg[value & 0xFF] = (value >> 8) & 0xFF
            return
        if port in (0x3C5, 0x3CF):
            # Data write: store into previously latched index.
            reg = self.seq if port == 0x3C5 else self.gc
            idx = self.seq_i if port == 0x3C5 else self.gc_i
            reg[idx] = value & 0xFF
            return
        if port == 0x3C8:
            # DAC write address register.
            self.dac_i = value & 0xFF
            self.dac_sub = 0
            return
        if port == 0x3C9:
            # DAC data: three successive writes = R, G, B for current entry.
            self.dac[self.dac_i & 0xFF][self.dac_sub] = value & 0x3F
            self.dac_sub += 1
            if self.dac_sub == 3:
                self.dac_sub = 0
                self.dac_i = (self.dac_i + 1) & 0xFF
            return

    # ---- VGA planar memory (0xA0000-0xAFFFF) ----
    # Ported faithfully from tools/emu/pydos.py Machine.vga_write / vga_read.

    def vga_write(self, lin: int, val: int) -> None:
        if lin >= 0xB0000:
            return
        off = (lin - 0xA0000) & 0xFFFF
        gc = self.gc
        seq = self.seq
        latch = self.latch
        wm = gc[5] & 3
        mm = seq[2] & 0xF
        bm = gc[8]
        sr = gc[0]
        esr = gc[1]
        fn = (gc[3] >> 3) & 3
        rot = gc[3] & 7
        for p in range(4):
            if not (mm & (1 << p)):
                continue
            lat = latch[p]
            if wm == 1:
                res = lat
            elif wm == 2:
                v = 0xFF if (val & (1 << p)) else 0
                if fn == 1:
                    v &= lat
                elif fn == 2:
                    v |= lat
                elif fn == 3:
                    v ^= lat
                res = (v & bm) | (lat & ~bm)
            elif wm == 3:
                rv = ((val >> rot) | (val << (8 - rot))) & 0xFF if rot else val
                m = bm & rv
                sv = 0xFF if (sr & (1 << p)) else 0
                res = (sv & m) | (lat & ~m)
            else:
                # Write mode 0 (the normal / blit mode).
                v = ((val >> rot) | (val << (8 - rot))) & 0xFF if rot else val
                if esr & (1 << p):
                    v = 0xFF if (sr & (1 << p)) else 0
                if fn == 1:
                    v &= lat
                elif fn == 2:
                    v |= lat
                elif fn == 3:
                    v ^= lat
                res = (v & bm) | (lat & ~bm)
            self.plane[p][off] = res & 0xFF

    def vga_read(self, lin: int) -> int:
        if lin >= 0xB0000:
            return 0
        off = (lin - 0xA0000) & 0xFFFF
        for p in range(4):
            self.latch[p] = self.plane[p][off]
        return self.plane[self.gc[4] & 3][off]

    # ---- run loop with a hard instruction cap + heartbeat ----
    def run(self, max_instr: int = 50_000_000) -> int:
        assert self.cpu is not None
        cpu = self.cpu
        chunk = 1_000_000
        total = 0
        while total < max_instr and not cpu.halted:
            for _ in range(chunk):
                cpu.step()
                if cpu.halted:
                    break
            total += chunk
            if not cpu.halted:
                print("  ... %d Minstr (cs=%04x ip=%04x)" % (
                    total // 1_000_000, cpu.s["cs"], cpu.ip), flush=True)
        return total

    def dump_planar(self, out_name: str) -> str:
        """Write captured VGA planes to <out_dir>/<out_name> and return the path.

        File layout: 32000 planar bytes (host.plane[p][:8000] for p=0..3
        concatenated) followed by 768 DAC bytes (R,G,B for entries 0..255).
        """
        out_path = os.path.join(self.out_dir, self.basename(out_name))
        with open(out_path, "wb") as f:
            for p in range(4):
                f.write(bytes(self.plane[p][:8000]))
            for entry in self.dac:
                f.write(bytes(entry))
        return out_path


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", default=os.path.join(ROOT, "local/build/src/BVEC.EXE"))
    ap.add_argument("--in-dir", default=os.path.join(ROOT, "local/build/capture/game"))
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "local/build/render"))
    ap.add_argument("--args", default="--selftest",
                    help="command tail passed to BVEC (e.g. '--selftest' or 'TITRE.VEC TITRE.PLN')")
    ap.add_argument("--max-instr", type=int, default=50_000_000)
    args = ap.parse_args()

    host = Host(args.exe, args.in_dir, args.out_dir)
    host.load(" " + args.args)
    print("booting BVEC.EXE: %s (args=%r)" % (args.exe, args.args), flush=True)
    total = host.run(max_instr=args.max_instr)
    print("ran %d instructions, exit code=%s" % (total, host.exited), flush=True)
    if host.exited is None:
        print("WARNING: program did not exit cleanly (instruction cap hit)", flush=True)
        sys.exit(2)

    # Capture planar output from VGA plane state.
    # Use the out-arg name from --args if present, else default to SELFTEST.PLN.
    arg_parts = args.args.strip().split()
    if arg_parts and arg_parts[0] == "--selftest":
        pln_name = "SELFTEST.PLN"
    elif len(arg_parts) >= 2:
        pln_name = arg_parts[1]
    else:
        pln_name = "BVEC.PLN"

    pln_path = host.dump_planar(pln_name)
    print("captured planar -> %s" % pln_path, flush=True)

    for p in host.written:
        sz = os.path.getsize(p) if os.path.exists(p) else -1
        print("wrote %s (%d bytes)" % (p, sz), flush=True)
    if host.exited != 0:
        print("WARNING: BVEC.EXE returned non-zero exit code %d" % host.exited, flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
