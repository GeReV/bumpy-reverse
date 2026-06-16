#!/usr/bin/env python3
"""Run the real vec_run/op4 decoder on a .VEC file under Unicorn and capture the
buffer it produces — a ground-truth oracle for the world/level layout that no
amount of static guessing has cracked.

Strategy: load the relocated image, put the .VEC bytes in an input buffer, call
vec_run with the game's register convention, and record EVERY memory write. The
dominant contiguous write region is op4's output buffer (the placed image). We
cap instructions low so we capture op4's work before any post-return derail.

Validate on TITRE first (its captured buffer must match the pure-Python decode),
then trust it for the world files.
"""
from __future__ import annotations
import sys
import collections
from typing import Dict, Tuple
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_INTR, UcError
from unicorn.x86_const import (UC_X86_REG_CS, UC_X86_REG_IP, UC_X86_REG_SS, UC_X86_REG_SP,
                               UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SI, UC_X86_REG_DI,
                               UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX)
import struct
from vec_emu import load_mz, LOAD_SEG

VEC_SEG = 0x3000      # input .VEC buffer (clear of image 0x10000-0x2a640 and stack)
STACK_SEG = 0x7000
RET_SEG = 0x9500


def run(vec_path: str, cx: int, max_instr: int = 2_000_000) -> Dict[int, int]:
    img, relocs = load_mz("local/originals/unpacked/BUMPY_unpacked.exe")
    vec = open(vec_path, "rb").read()
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 0x110000)
    uc.mem_write(LOAD_SEG * 16, img)
    for off, seg in relocs:
        lin = LOAD_SEG * 16 + ((seg * 16 + off) & 0xFFFFF)
        v = struct.unpack("<H", uc.mem_read(lin, 2))[0]
        uc.mem_write(lin, struct.pack("<H", (v + LOAD_SEG) & 0xFFFF))
    uc.mem_write(VEC_SEG * 16, vec)
    uc.mem_write(RET_SEG * 16, b"\xf4")

    writes: Dict[int, int] = {}
    state = {"n": 0}

    def hk_code(uc, addr, size, _):
        state["n"] += 1
        if state["n"] > max_instr:
            uc.emu_stop()

    def hk_write(uc, access, addr, size, value, _):
        for k in range(size):
            writes[addr + k] = (value >> (8 * k)) & 0xFF

    def hk_intr(uc, intno, _):
        sp = uc.reg_read(UC_X86_REG_SP); ss = uc.reg_read(UC_X86_REG_SS)
        ip = struct.unpack("<H", uc.mem_read(ss * 16 + sp, 2))[0]
        cs = struct.unpack("<H", uc.mem_read(ss * 16 + sp + 2, 2))[0]
        uc.reg_write(UC_X86_REG_SP, (sp + 6) & 0xFFFF)
        uc.reg_write(UC_X86_REG_CS, cs); uc.reg_write(UC_X86_REG_IP, ip)

    uc.hook_add(UC_HOOK_CODE, hk_code)
    uc.hook_add(UC_HOOK_MEM_WRITE, hk_write)
    uc.hook_add(UC_HOOK_INTR, hk_intr)

    SS, SP = STACK_SEG, 0xFFF0
    def push(v: int):
        nonlocal SP
        SP = (SP - 2) & 0xFFFF
        uc.mem_write(SS * 16 + SP, struct.pack("<H", v))
    push(RET_SEG); push(0)
    uc.reg_write(UC_X86_REG_SS, SS); uc.reg_write(UC_X86_REG_SP, SP)
    uc.reg_write(UC_X86_REG_SI, 0)             # buf offset
    uc.reg_write(UC_X86_REG_DI, VEC_SEG)       # buf segment
    uc.reg_write(UC_X86_REG_AX, len(vec))      # count
    uc.reg_write(UC_X86_REG_BX, 0)
    uc.reg_write(UC_X86_REG_CX, cx)            # dest offset param (DAT_0098-ish)
    uc.reg_write(UC_X86_REG_DX, 0)
    uc.reg_write(UC_X86_REG_DS, (LOAD_SEG + 0x103b) & 0xFFFF)
    uc.reg_write(UC_X86_REG_ES, VEC_SEG)
    uc.reg_write(UC_X86_REG_CS, LOAD_SEG + 0xc28)
    uc.reg_write(UC_X86_REG_IP, 0)
    try:
        uc.emu_start((LOAD_SEG + 0xc28) * 16, RET_SEG * 16, count=0)
    except UcError as e:
        print("  (stopped: %s after %d instr)" % (e, state["n"]))
    return writes


def dominant_region(writes: Dict[int, int]) -> Tuple[int, int, bytes]:
    """Largest run of write addresses with gaps <= 64 bytes."""
    if not writes:
        return 0, 0, b""
    addrs = sorted(writes)
    best = (0, 0)  # (start, end)
    s = addrs[0]; prev = addrs[0]
    runs = []
    for a in addrs[1:]:
        if a - prev > 64:
            runs.append((s, prev)); s = a
        prev = a
    runs.append((s, prev))
    start, end = max(runs, key=lambda r: r[1] - r[0])
    buf = bytes(writes.get(a, 0) for a in range(start, end + 1))
    return start, end, buf


def main() -> None:
    vec = sys.argv[1]
    cx = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    print("oracle: %s  cx=%#x" % (vec, cx))
    writes = run(vec, cx)
    start, end, buf = dominant_region(writes)
    print("  total writes=%d, dominant region %#x..%#x (%d bytes)" % (len(writes), start, end, len(buf)))
    out = "local/build/render/oracle_%s_cx%x.bin" % (vec.split("/")[-1], cx)
    open(out, "wb").write(buf)
    print("  wrote", out)


if __name__ == "__main__":
    main()
