#!/usr/bin/env python3
"""run_bumpy.py — Boot harness for the reconstructed BUMPY.EXE (Phase-1 Task 3).

Loads BUMPY.EXE under the Unicorn 16-bit DOS emulator (reusing run_bvec.py's
Host scaffold), runs it to exit, and asserts:
  1. INT 10h AX=0x000D (VGA mode 0x0D set) was called.
  2. The program exited cleanly (INT 21h AH=0x4C).

The BUMPY.EXE built in Task 3 has a stub run_game_session (returns immediately),
so the boot path is:
  CRT startup → main() → init_game_session_state_stub() [video_set_mode_0d()]
  → run_game_session_stub() [returns] → CRT exit

Usage:
    uv run python tools/run_bumpy.py
    uv run python tools/run_bumpy.py --exe local/build/src/BUMPY.EXE
    uv run python tools/run_bumpy.py --max-instr 10000000

Exit codes:
    0 — VGA mode 0x0D set + clean exit (PASS)
    1 — program did not exit cleanly or mode was not set (FAIL)
    2 — instruction cap hit without exit (TIMEOUT)
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from typing import Dict, List, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools/emu"))

PSP_SEG = 0x0100
RAM = 0x110000


def load_mz(path: str) -> Tuple[bytes, List[Tuple[int, int]], Dict[str, int]]:
    """Load an MZ executable and return (image_bytes, relocations, header_fields)."""
    with open(path, "rb") as f:
        x = f.read()
    e_crlc, e_cparhdr = struct.unpack_from("<HH", x, 6)
    e_ss, e_sp, _chk, e_ip, e_cs = struct.unpack_from("<HHHHH", x, 0x0E)
    e_lfarlc = struct.unpack_from("<H", x, 0x18)[0]
    img = x[e_cparhdr * 16:]
    relocs = [struct.unpack_from("<HH", x, e_lfarlc + i * 4) for i in range(e_crlc)]
    return img, relocs, dict(ss=e_ss, sp=e_sp, ip=e_ip, cs=e_cs)


class BumpyBootHost:
    """Minimal DOS host for the BUMPY.EXE boot test.

    Tracks INT 10h AX=0x000D (VGA mode 0x0D) and INT 21h AH=0x4C (exit).
    All other interrupts are accepted silently (IRET-noop) so the OW CRT
    startup sequence (INT 21h AH=0x30 version check, etc.) completes cleanly.
    """

    def __init__(self, exe: str) -> None:
        self.exe: str = exe
        self.exited: Optional[int] = None
        self.mode_0d_set: bool = False         # True once INT 10h AX=0x000D fires
        self.int10_calls: List[int] = []       # AX values for every INT 10h call
        self.base: int = 0
        self.alloc: int = 0
        self.alloc_end: int = 0

    def _build_mem(self) -> Tuple[bytearray, Dict[str, int]]:
        """Build the 1 MB DOS image: load MZ, apply relocations, minimal PSP."""
        img, relocs, hdr = load_mz(self.exe)
        base = PSP_SEG + 0x10
        mem = bytearray(RAM)
        mem[base * 16:base * 16 + len(img)] = img
        for off, seg in relocs:
            lin = base * 16 + ((seg * 16 + off) & 0xFFFFF)
            v = ((mem[lin] | (mem[lin + 1] << 8)) + base) & 0xFFFF
            mem[lin] = v & 0xFF
            mem[lin + 1] = v >> 8
        # Minimal PSP: INT 20h at +0, memory top word, empty command tail.
        mem[PSP_SEG * 16] = 0xCD
        mem[PSP_SEG * 16 + 1] = 0x20
        mem[PSP_SEG * 16 + 2] = 0x00
        mem[PSP_SEG * 16 + 3] = 0xA0
        mem[PSP_SEG * 16 + 0x80] = 0x00
        mem[PSP_SEG * 16 + 0x81] = 0x0D
        # IRET stub at 0050:0000 for every still-null IVT vector.
        mem[0x500] = 0xCF
        for v in range(0x100):
            cur = (mem[v * 4] | (mem[v * 4 + 1] << 8)
                   | (mem[v * 4 + 2] << 16) | (mem[v * 4 + 3] << 24))
            if cur == 0:
                mem[v * 4] = 0x00
                mem[v * 4 + 1] = 0x00
                mem[v * 4 + 2] = 0x50
                mem[v * 4 + 3] = 0x00
        self.base = base
        self.alloc = base + (len(img) + 15) // 16 + 0x100
        if self.alloc < 0x1C00:
            self.alloc = 0x1C00
        self.alloc_end = 0x9000
        return mem, hdr

    def load_unicorn(self) -> None:
        """Set up Unicorn with INT hooks and the INT 10h mode-tracking hook."""
        from unicorn import (Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR,
                             UC_HOOK_INSN, UC_HOOK_MEM_WRITE)
        from unicorn.x86_const import (
            UC_X86_INS_OUT,
            UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
            UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
            UC_X86_REG_SP, UC_X86_REG_CS, UC_X86_REG_IP, UC_X86_REG_EFLAGS)

        mem, hdr = self._build_mem()
        base = self.base
        uc = Uc(UC_ARCH_X86, UC_MODE_16)
        uc.mem_map(0, RAM)
        uc.mem_write(0, bytes(mem))
        uc.reg_write(UC_X86_REG_DS, PSP_SEG)
        uc.reg_write(UC_X86_REG_ES, PSP_SEG)
        uc.reg_write(UC_X86_REG_SS, (hdr["ss"] + base) & 0xFFFF)
        uc.reg_write(UC_X86_REG_SP, hdr["sp"])
        uc.reg_write(UC_X86_REG_CS, (hdr["cs"] + base) & 0xFFFF)
        uc.reg_write(UC_X86_REG_IP, hdr["ip"])
        uc.reg_write(UC_X86_REG_EFLAGS, 0x0202)
        self.uc = uc
        self._halted = False

        def cf(set_it: bool) -> None:
            fl = uc.reg_read(UC_X86_REG_EFLAGS)
            uc.reg_write(UC_X86_REG_EFLAGS, (fl | 1) if set_it else (fl & ~1))

        def rdstr(seg: int, off: int) -> bytes:
            out = b""
            while True:
                c = uc.mem_read((seg << 4) + off, 1)[0]
                if c == 0:
                    break
                out += bytes([c])
                off += 1
            return out

        def hook_intr(u: object, intno: int, _: object) -> None:
            ax = uc.reg_read(UC_X86_REG_AX) & 0xFFFF
            ah = (ax >> 8) & 0xFF
            al = ax & 0xFF
            if intno == 0x10:
                # BIOS video interrupt — track mode sets.
                self.int10_calls.append(ax)
                if ah == 0x00 and al == 0x0D:
                    self.mode_0d_set = True
                    print("  INT 10h AX=%04X → VGA mode 0x0D SET" % ax, flush=True)
                elif ah == 0x00:
                    print("  INT 10h AX=%04X (mode set 0x%02x)" % (ax, al), flush=True)
                return
            if intno == 0x20:
                self.exited = 0
                self._halted = True
                uc.emu_stop()
                return
            if intno == 0x21:
                if ah == 0x4C:
                    self.exited = al
                    self._halted = True
                    uc.emu_stop()
                    return
                if ah == 0x30:
                    uc.reg_write(UC_X86_REG_AX, 0x0005)
                    return
                if ah == 0x25:
                    dx = uc.reg_read(UC_X86_REG_DX) & 0xFFFF
                    ds = uc.reg_read(UC_X86_REG_DS) & 0xFFFF
                    uc.mem_write(al * 4, struct.pack("<HH", dx, ds))
                    return
                if ah == 0x35:
                    data = bytes(uc.mem_read(al * 4, 4))
                    off_v, seg_v = struct.unpack("<HH", data)
                    uc.reg_write(UC_X86_REG_BX, off_v)
                    uc.reg_write(UC_X86_REG_ES, seg_v)
                    return
                if ah == 0x48:
                    bx = uc.reg_read(UC_X86_REG_BX) & 0xFFFF
                    avail = self.alloc_end - self.alloc
                    if bx > avail:
                        uc.reg_write(UC_X86_REG_AX, 8)
                        uc.reg_write(UC_X86_REG_BX, avail)
                        cf(True)
                    else:
                        uc.reg_write(UC_X86_REG_AX, self.alloc)
                        self.alloc += bx
                        cf(False)
                    return
                if ah in (0x49, 0x4A):
                    cf(False)
                    return
                # All other INT 21h: silently accept (no-op / CF=0).
                cf(False)
                return
            # All other interrupts: IRET-noop (already installed via IVT stub).

        def hook_out(u: object, port: int, size: int, value: int, _: object) -> None:
            # Accept all I/O port writes silently (VGA sequencer/GC/DAC etc.).
            pass

        uc.hook_add(UC_HOOK_INTR, hook_intr)
        uc.hook_add(UC_HOOK_INSN, hook_out, None, 1, 0, UC_X86_INS_OUT)
        # Keep callbacks alive.
        self._hooks = (hook_intr, hook_out)

    def run_unicorn(self, max_instr: int = 20_000_000) -> int:
        """Run in counted chunks until exit or cap.  Returns total instructions."""
        from unicorn import UcError
        from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP
        uc = self.uc
        chunk = 2_000_000
        total = 0
        while total < max_instr and not self._halted:
            begin = ((uc.reg_read(UC_X86_REG_CS) & 0xFFFF) * 16
                     + (uc.reg_read(UC_X86_REG_IP) & 0xFFFF)) & 0xFFFFF
            try:
                uc.emu_start(begin, 0, count=chunk)
            except UcError as e:
                cs = uc.reg_read(UC_X86_REG_CS)
                ip = uc.reg_read(UC_X86_REG_IP)
                print("  unicorn error %s at cs=%04x ip=%04x" % (e, cs, ip),
                      flush=True)
                break
            total += chunk
            if not self._halted:
                print("  ... %dM instr (cs=%04x ip=%04x)" % (
                    total // 1_000_000,
                    uc.reg_read(UC_X86_REG_CS),
                    uc.reg_read(UC_X86_REG_IP)), flush=True)
        return total


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe",
                    default=os.path.join(ROOT, "local/build/src/BUMPY.EXE"),
                    help="Path to the reconstructed BUMPY.EXE")
    ap.add_argument("--max-instr", type=int, default=20_000_000,
                    help="Hard instruction cap (default 20M; the stub main exits fast)")
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        print("ERROR: %s not found" % args.exe, flush=True)
        sys.exit(1)

    print("booting BUMPY.EXE: %s" % args.exe, flush=True)
    host = BumpyBootHost(args.exe)
    host.load_unicorn()
    total = host.run_unicorn(max_instr=args.max_instr)
    print("ran %d instructions, exit_code=%s" % (total, host.exited), flush=True)
    print("INT 10h calls: %s" % (
        ["AX=%04X" % ax for ax in host.int10_calls] or ["(none)"]), flush=True)

    if host.exited is None:
        print("FAIL: program did not exit (instruction cap hit at %d)" % total,
              flush=True)
        sys.exit(2)

    if not host.mode_0d_set:
        print("FAIL: VGA mode 0x0D (INT 10h AX=0x000D) was NOT set", flush=True)
        sys.exit(1)

    print("PASS: VGA mode 0x0D set + clean exit (code %d)" % host.exited, flush=True)
    sys.exit(0)


if __name__ == "__main__":
    main()
