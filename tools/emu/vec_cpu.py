#!/usr/bin/env python3
"""Minimal 16-bit real-mode x86 interpreter, purpose-built to re-execute Bumpy's
`vec_run`/op12 record renderer in pure Python (no emulator dependency).

Why a CPU and not a hand-port: op12's output geometry is computed per-record by
its own coordinate math + a recursive re-entry into `vec_run` + far-pointer
normaliser helpers (segment 0xcda). All of that code is resident in a snapshot of
the running, relocated game image (build/render/op12_seed_mem.bin), so the most
faithful "port" is to execute the bytes exactly. This interpreter covers the
instruction subset that code path uses; it raises on anything unimplemented so the
subset can be grown deterministically.

Validation: tools/emu/dosemu.py captures a 6000-step ground-truth register trace
(op12_trace.bin) from the same op12 entry. `--validate` runs this CPU against it and
reports the first divergence, so the implementation is provably faithful before we
trust its rendered output.

This is general reversing tooling: seed it with any relocated real-mode image +
register state and run a routine to completion.
"""
from __future__ import annotations
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# 16-bit GP register order (ModR/M reg field encoding)
R16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]
# 8-bit register order
R8 = ["al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"]
SEG = ["es", "cs", "ss", "ds"]

CF, PF, AF, ZF, SF, TF, IF, DF, OF = (1, 4, 16, 64, 128, 256, 512, 1024, 2048)
PARITY = [(bin(i).count("1") & 1) == 0 for i in range(256)]


class Halt(Exception):
    pass


class CPU:
    def __init__(self, mem: bytearray):
        self.m = mem
        self.r = {n: 0 for n in R16}
        self.s = {n: 0 for n in SEG}
        self.ip = 0
        self.flags = 0
        self.entry_sp = 0
        # decode-time state
        self._seg_override = None
        # optional host hooks (used by pydos.py to boot the real game in pure Python).
        # int_handler(cpu, n) -> True if serviced (else real IVT dispatch happens).
        self.int_handler = None
        self.io_in = None       # io_in(cpu, port, size) -> value
        self.io_out = None      # io_out(cpu, port, size, value)
        self.mmio_lo = 1 << 30  # linear addresses >= this route to mmio hooks (VGA)
        self.mmio_read = None   # mmio_read(lin) -> byte
        self.mmio_write = None  # mmio_write(lin, byte)
        self.halted = False

    # ---- register access -------------------------------------------------
    def g16(self, i: int) -> int:
        return self.r[R16[i]]

    def s16(self, i: int, v: int) -> None:
        self.r[R16[i]] = v & 0xFFFF

    def g8(self, i: int) -> int:
        name = R16[i & 3]
        v = self.r[name]
        return (v >> 8) & 0xFF if i >= 4 else v & 0xFF

    def s8(self, i: int, v: int) -> None:
        name = R16[i & 3]
        cur = self.r[name]
        v &= 0xFF
        if i >= 4:
            self.r[name] = (cur & 0x00FF) | (v << 8)
        else:
            self.r[name] = (cur & 0xFF00) | v

    # ---- memory ----------------------------------------------------------
    def lin(self, seg: int, off: int) -> int:
        return ((seg << 4) + (off & 0xFFFF)) & 0xFFFFF

    def r8(self, lin: int) -> int:
        if lin >= self.mmio_lo:
            return self.mmio_read(lin)
        return self.m[lin]

    wlog = None  # optional (lo, hi, list) to record ordered writes in a linear range
    cur_cs = 0
    cur_ip = 0   # set by step() each instruction; lets w8 attribute writes to code

    def w8(self, lin: int, v: int) -> None:
        if lin >= self.mmio_lo:
            self.mmio_write(lin, v & 0xFF); return
        self.m[lin] = v & 0xFF
        if self.wlog is not None and self.wlog[0] <= lin <= self.wlog[1]:
            self.wlog[2].append((self.cur_cs, self.cur_ip, lin, v & 0xFF))

    def r16(self, lin: int) -> int:
        if lin >= self.mmio_lo:
            return self.mmio_read(lin) | (self.mmio_read(lin + 1) << 8)
        return self.m[lin] | (self.m[lin + 1] << 8)

    def w16(self, lin: int, v: int) -> None:
        if lin >= self.mmio_lo:
            self.mmio_write(lin, v & 0xFF); self.mmio_write(lin + 1, (v >> 8) & 0xFF); return
        self.m[lin] = v & 0xFF
        self.m[lin + 1] = (v >> 8) & 0xFF

    # ---- code fetch ------------------------------------------------------
    def fetch8(self) -> int:
        b = self.m[self.lin(self.s["cs"], self.ip)]
        self.ip = (self.ip + 1) & 0xFFFF
        return b

    def fetch16(self) -> int:
        lo = self.fetch8()
        hi = self.fetch8()
        return lo | (hi << 8)

    def fetchs8(self) -> int:
        b = self.fetch8()
        return b - 256 if b >= 128 else b

    # ---- stack -----------------------------------------------------------
    def push(self, v: int) -> None:
        self.r["sp"] = (self.r["sp"] - 2) & 0xFFFF
        self.w16(self.lin(self.s["ss"], self.r["sp"]), v)

    def pop(self) -> int:
        v = self.r16(self.lin(self.s["ss"], self.r["sp"]))
        self.r["sp"] = (self.r["sp"] + 2) & 0xFFFF
        return v

    # ---- flags -----------------------------------------------------------
    def setf(self, bit: int, on: bool) -> None:
        if on:
            self.flags |= bit
        else:
            self.flags &= ~bit

    def szp(self, v: int, w: int) -> None:
        mask = 0xFFFF if w == 16 else 0xFF
        v &= mask
        self.setf(ZF, v == 0)
        self.setf(SF, bool(v & (0x8000 if w == 16 else 0x80)))
        self.setf(PF, PARITY[v & 0xFF])

    def add(self, a: int, b: int, w: int, carry: int = 0) -> int:
        mask = 0xFFFF if w == 16 else 0xFF
        sign = 0x8000 if w == 16 else 0x80
        res = a + b + carry
        self.setf(CF, res > mask)
        self.setf(AF, ((a ^ b ^ res) & 0x10) != 0)
        r = res & mask
        self.setf(OF, bool((~(a ^ b) & (a ^ r)) & sign))
        self.szp(r, w)
        return r

    def sub(self, a: int, b: int, w: int, borrow: int = 0) -> int:
        mask = 0xFFFF if w == 16 else 0xFF
        sign = 0x8000 if w == 16 else 0x80
        res = a - b - borrow
        self.setf(CF, res < 0)
        self.setf(AF, ((a ^ b ^ res) & 0x10) != 0)
        r = res & mask
        self.setf(OF, bool(((a ^ b) & (a ^ r)) & sign))
        self.szp(r, w)
        return r

    def logic(self, r: int, w: int) -> int:
        mask = 0xFFFF if w == 16 else 0xFF
        r &= mask
        self.setf(CF, False)
        self.setf(OF, False)
        self.setf(AF, False)
        self.szp(r, w)
        return r

    # ---- ModR/M ----------------------------------------------------------
    def modrm(self):
        """Return (mod, reg, rm_is_reg, rm_index_or_linaddr)."""
        b = self.fetch8()
        mod = b >> 6
        reg = (b >> 3) & 7
        rm = b & 7
        if mod == 3:
            return mod, reg, True, rm
        # 16-bit memory addressing
        if rm == 0:
            base = self.r["bx"] + self.r["si"]; dseg = "ds"
        elif rm == 1:
            base = self.r["bx"] + self.r["di"]; dseg = "ds"
        elif rm == 2:
            base = self.r["bp"] + self.r["si"]; dseg = "ss"
        elif rm == 3:
            base = self.r["bp"] + self.r["di"]; dseg = "ss"
        elif rm == 4:
            base = self.r["si"]; dseg = "ds"
        elif rm == 5:
            base = self.r["di"]; dseg = "ds"
        elif rm == 6:
            if mod == 0:
                base = self.fetch16(); dseg = "ds"
            else:
                base = self.r["bp"]; dseg = "ss"
        else:  # rm == 7
            base = self.r["bx"]; dseg = "ds"
        if mod == 1:
            base += self.fetchs8()
        elif mod == 2:
            base += self.fetch16()
        self._last_off = base & 0xFFFF
        seg = self.s[self._seg_override] if self._seg_override else self.s[dseg]
        return mod, reg, False, self.lin(seg, base & 0xFFFF)

    # read/write an rm operand
    def rm_get(self, is_reg, x, w):
        if is_reg:
            return self.g16(x) if w == 16 else self.g8(x)
        return self.r16(x) if w == 16 else self.r8(x)

    def rm_set(self, is_reg, x, v, w):
        if is_reg:
            (self.s16 if w == 16 else self.s8)(x, v)
        else:
            (self.w16 if w == 16 else self.w8)(x, v)

    # ---- run -------------------------------------------------------------
    def cond(self, code: int) -> bool:
        f = self.flags
        c = (code >> 1)
        r = [bool(f & OF), bool(f & CF), bool(f & ZF),
             bool(f & CF) or bool(f & ZF),
             bool(f & SF), bool(f & PF),
             bool(f & SF) != bool(f & OF),
             (bool(f & SF) != bool(f & OF)) or bool(f & ZF)][c]
        return r != bool(code & 1)

    def step(self) -> None:
        self.cur_cs = self.s["cs"]; self.cur_ip = self.ip
        self._seg_override = None
        rep = None
        while True:
            op = self.fetch8()
            if op in (0x26, 0x2E, 0x36, 0x3E):
                self._seg_override = ["es", "cs", "ss", "ds"][(op >> 3) & 3]
                continue
            if op in (0xF2, 0xF3):
                rep = op
                continue
            break
        self.exec(op, rep)

    def exec(self, op: int, rep):
        m = self.m
        # ALU group: 00..3D regular pattern (add/or/adc/sbb/and/sub/xor/cmp)
        alu = op >> 3
        if op < 0x40 and (op & 7) < 6 and alu in (0, 1, 2, 3, 4, 5, 6, 7):
            self.alu_op(op)
            return
        if op in (0x06, 0x0E, 0x16, 0x1E):  # push seg (es/cs/ss/ds)
            self.push(self.s[["es", "cs", "ss", "ds"][(op >> 3) & 3]]); return
        if op in (0x07, 0x17, 0x1F):  # pop seg (es/ss/ds)
            self.s[["es", "cs", "ss", "ds"][(op >> 3) & 3]] = self.pop(); return
        if 0x40 <= op <= 0x47:  # inc r16
            i = op & 7
            self.s16(i, self._inc(self.g16(i), 16))
            return
        if 0x48 <= op <= 0x4F:  # dec r16
            i = op & 7
            self.s16(i, self._dec(self.g16(i), 16))
            return
        if 0x50 <= op <= 0x57:  # push r16
            self.push(self.g16(op & 7)); return
        if 0x58 <= op <= 0x5F:  # pop r16
            self.s16(op & 7, self.pop()); return
        if op == 0x60:  # pusha
            sp = self.r["sp"]
            for nm in ("ax", "cx", "dx", "bx"):
                self.push(self.r[nm])
            self.push(sp)
            for nm in ("bp", "si", "di"):
                self.push(self.r[nm])
            return
        if op == 0x61:  # popa
            for nm in ("di", "si", "bp"):
                self.r[nm] = self.pop()
            self.pop()
            for nm in ("bx", "dx", "cx", "ax"):
                self.r[nm] = self.pop()
            return
        if 0x70 <= op <= 0x7F:  # jcc rel8
            d = self.fetchs8()
            if self.cond(op & 0x0F):
                self.ip = (self.ip + d) & 0xFFFF
            return
        if op in (0x80, 0x81, 0x82, 0x83):  # grp1 rm,imm
            w = 16 if op in (0x81, 0x83) else 8
            mod, reg, isr, x = self.modrm()
            a = self.rm_get(isr, x, w)
            if op == 0x81:
                imm = self.fetch16()
            elif op == 0x83:
                imm = self.fetchs8() & 0xFFFF
            else:
                imm = self.fetch8()
            self.rm_set(isr, x, self.alu_imm(reg, a, imm, w), w)
            return
        if op in (0x84, 0x85):  # test rm,r
            w = 16 if op == 0x85 else 8
            mod, reg, isr, x = self.modrm()
            self.logic(self.rm_get(isr, x, w) & (self.g16(reg) if w == 16 else self.g8(reg)), w)
            return
        if op in (0x86, 0x87):  # xchg rm,r
            w = 16 if op == 0x87 else 8
            mod, reg, isr, x = self.modrm()
            a = self.rm_get(isr, x, w)
            b = self.g16(reg) if w == 16 else self.g8(reg)
            self.rm_set(isr, x, b, w)
            (self.s16 if w == 16 else self.s8)(reg, a)
            return
        if op in (0x88, 0x89, 0x8A, 0x8B):  # mov
            w = 16 if op in (0x89, 0x8B) else 8
            mod, reg, isr, x = self.modrm()
            if op in (0x88, 0x89):  # mov rm, reg
                self.rm_set(isr, x, self.g16(reg) if w == 16 else self.g8(reg), w)
            else:  # mov reg, rm
                (self.s16 if w == 16 else self.s8)(reg, self.rm_get(isr, x, w))
            return
        if op == 0x8C:  # mov rm16, seg
            mod, reg, isr, x = self.modrm()
            self.rm_set(isr, x, self.s[SEG[reg & 3]], 16)
            return
        if op == 0x8D:  # lea
            mod, reg, isr, x = self.modrm()
            self.s16(reg, self._last_off)
            return
        if op == 0x8E:  # mov seg, rm16
            mod, reg, isr, x = self.modrm()
            self.s[SEG[reg & 3]] = self.rm_get(isr, x, 16)
            return
        if op == 0x8F:  # pop rm16
            mod, reg, isr, x = self.modrm()
            self.rm_set(isr, x, self.pop(), 16)
            return
        if op == 0x90:  # nop
            return
        if 0x91 <= op <= 0x97:  # xchg ax, r16
            i = op & 7
            a = self.r["ax"]; self.r["ax"] = self.g16(i); self.s16(i, a)
            return
        if op == 0x98:  # cbw
            al = self.r["ax"] & 0xFF
            self.r["ax"] = (al | 0xFF00) if al & 0x80 else al
            return
        if op == 0x99:  # cwd
            self.r["dx"] = 0xFFFF if self.r["ax"] & 0x8000 else 0
            return
        if op == 0x9A:  # call far ptr16:16
            off = self.fetch16(); seg = self.fetch16()
            self.push(self.s["cs"]); self.push(self.ip)
            self.s["cs"] = seg; self.ip = off
            return
        if op == 0x9C:  # pushf
            self.push(self.flags | 0xF002); return
        if op == 0x9D:  # popf
            self.flags = self.pop(); return
        if op == 0x9E:  # sahf: SF/ZF/AF/PF/CF <- AH
            ah = (self.r["ax"] >> 8) & 0xFF
            self.flags = (self.flags & 0xFF00) | (ah & 0xD5) | 0x02
            return
        if op == 0x9F:  # lahf: AH <- SF/ZF/AF/PF/CF
            fl = (self.flags & 0xD5) | 0x02
            self.r["ax"] = (self.r["ax"] & 0x00FF) | (fl << 8)
            return
        if op in (0xA0, 0xA1):  # mov al/ax, [moffs]
            w = 16 if op == 0xA1 else 8
            off = self.fetch16()
            seg = self.s[self._seg_override] if self._seg_override else self.s["ds"]
            v = self.r16(self.lin(seg, off)) if w == 16 else self.r8(self.lin(seg, off))
            (self.s16 if w == 16 else self.s8)(0, v)
            return
        if op in (0xA2, 0xA3):  # mov [moffs], al/ax
            w = 16 if op == 0xA3 else 8
            off = self.fetch16()
            seg = self.s[self._seg_override] if self._seg_override else self.s["ds"]
            (self.w16 if w == 16 else self.w8)(self.lin(seg, off), self.r["ax"] if w == 16 else self.r["ax"] & 0xFF)
            return
        if op in (0xA4, 0xA5):  # movs
            self.do_string(op, rep, "movs"); return
        if op in (0xAA, 0xAB):  # stos
            self.do_string(op, rep, "stos"); return
        if op in (0xAC, 0xAD):  # lods
            self.do_string(op, rep, "lods"); return
        if op in (0xA6, 0xA7):  # cmps
            self.do_string(op, rep, "cmps"); return
        if op in (0xAE, 0xAF):  # scas
            self.do_string(op, rep, "scas"); return
        if op in (0xA8, 0xA9):  # test al/ax, imm
            w = 16 if op == 0xA9 else 8
            imm = self.fetch16() if w == 16 else self.fetch8()
            self.logic((self.r["ax"] if w == 16 else self.r["ax"] & 0xFF) & imm, w)
            return
        if 0xB0 <= op <= 0xB7:  # mov r8, imm8
            self.s8(op & 7, self.fetch8()); return
        if 0xB8 <= op <= 0xBF:  # mov r16, imm16
            self.s16(op & 7, self.fetch16()); return
        if op in (0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3):  # shift group
            self.shift_group(op); return
        if op == 0xC2:  # ret imm16 (near)
            n = self.fetch16(); self.ip = self.pop()
            self.r["sp"] = (self.r["sp"] + n) & 0xFFFF
            self.check_ret(); return
        if op == 0xC3:  # ret near
            self.ip = self.pop(); self.check_ret(); return
        if op in (0xC4, 0xC5):  # les/lds reg, m16:16
            mod, reg, isr, x = self.modrm()
            self.s16(reg, self.r16(x))
            self.s["es" if op == 0xC4 else "ds"] = self.r16(x + 2)
            return
        if op in (0xC6, 0xC7):  # mov rm, imm
            w = 16 if op == 0xC7 else 8
            mod, reg, isr, x = self.modrm()
            imm = self.fetch16() if w == 16 else self.fetch8()
            self.rm_set(isr, x, imm, w)
            return
        if op == 0xCA:  # retf imm16
            n = self.fetch16(); self.ip = self.pop(); self.s["cs"] = self.pop()
            self.r["sp"] = (self.r["sp"] + n) & 0xFFFF
            self.check_ret(); return
        if op == 0xCB:  # retf
            self.ip = self.pop(); self.s["cs"] = self.pop()
            self.check_ret(); return
        if op == 0xCF:  # iret
            self.ip = self.pop(); self.s["cs"] = self.pop(); self.flags = self.pop()
            self.check_ret(); return
        if op == 0xE2:  # loop
            d = self.fetchs8()
            self.r["cx"] = (self.r["cx"] - 1) & 0xFFFF
            if self.r["cx"] != 0:
                self.ip = (self.ip + d) & 0xFFFF
            return
        if op == 0xE1:  # loope
            d = self.fetchs8()
            self.r["cx"] = (self.r["cx"] - 1) & 0xFFFF
            if self.r["cx"] != 0 and (self.flags & ZF):
                self.ip = (self.ip + d) & 0xFFFF
            return
        if op == 0xE0:  # loopne
            d = self.fetchs8()
            self.r["cx"] = (self.r["cx"] - 1) & 0xFFFF
            if self.r["cx"] != 0 and not (self.flags & ZF):
                self.ip = (self.ip + d) & 0xFFFF
            return
        if op == 0xE3:  # jcxz
            d = self.fetchs8()
            if self.r["cx"] == 0:
                self.ip = (self.ip + d) & 0xFFFF
            return
        if op == 0xE8:  # call rel16
            d = self.fetch16()
            if d >= 0x8000:
                d -= 0x10000
            self.push(self.ip)
            self.ip = (self.ip + d) & 0xFFFF
            return
        if op == 0xE9:  # jmp rel16
            d = self.fetch16()
            if d >= 0x8000:
                d -= 0x10000
            self.ip = (self.ip + d) & 0xFFFF
            return
        if op == 0xEA:  # jmp far
            off = self.fetch16(); seg = self.fetch16()
            self.s["cs"] = seg; self.ip = off
            return
        if op == 0xEB:  # jmp rel8
            d = self.fetchs8(); self.ip = (self.ip + d) & 0xFFFF
            return
        if op in (0xF6, 0xF7):  # grp3 (test/not/neg/mul/imul/div/idiv)
            self.grp3(op); return
        if op == 0xF8:  # clc
            self.setf(CF, False); return
        if op == 0xF9:  # stc
            self.setf(CF, True); return
        if op == 0xFC:  # cld
            self.setf(DF, False); return
        if op == 0xFD:  # std
            self.setf(DF, True); return
        if op == 0xFE:  # grp4 inc/dec rm8
            mod, reg, isr, x = self.modrm()
            a = self.rm_get(isr, x, 8)
            self.rm_set(isr, x, self._inc(a, 8) if reg == 0 else self._dec(a, 8), 8)
            return
        if op == 0xFF:  # grp5
            self.grp5(); return
        if op == 0xCD:  # int n
            n = self.fetch8()
            if self.int_handler and self.int_handler(self, n):
                return
            self.push(self.flags | 0xF002); self.push(self.s["cs"]); self.push(self.ip)
            self.flags &= ~(IF | TF)
            self.ip = self.r16(n * 4); self.s["cs"] = self.r16(n * 4 + 2)
            return
        if op == 0xCE:  # into
            if self.flags & OF:
                self.exec(0xCD, None)  # not reached in this code path normally
            return
        if op == 0xCC:  # int3
            return
        if op in (0xE4, 0xE5):  # in al/ax, imm8
            port = self.fetch8(); w = 2 if op == 0xE5 else 1
            v = self.io_in(self, port, w) if self.io_in else 0xFF
            (self.s16 if w == 2 else self.s8)(0, v)
            return
        if op in (0xE6, 0xE7):  # out imm8, al/ax
            port = self.fetch8(); w = 2 if op == 0xE7 else 1
            if self.io_out:
                self.io_out(self, port, w, self.r["ax"] if w == 2 else self.r["ax"] & 0xFF)
            return
        if op in (0xEC, 0xED):  # in al/ax, dx
            w = 2 if op == 0xED else 1
            v = self.io_in(self, self.r["dx"], w) if self.io_in else 0xFF
            (self.s16 if w == 2 else self.s8)(0, v)
            return
        if op in (0xEE, 0xEF):  # out dx, al/ax
            w = 2 if op == 0xEF else 1
            if self.io_out:
                self.io_out(self, self.r["dx"], w, self.r["ax"] if w == 2 else self.r["ax"] & 0xFF)
            return
        if op == 0xFA:  # cli
            self.setf(IF, False); return
        if op == 0xFB:  # sti
            self.setf(IF, True); return
        if op == 0x9B:  # wait/fwait
            return
        if op == 0xF4:  # hlt
            self.halted = True; return
        raise NotImplementedError("opcode %#04x at %04x:%04x" % (op, self.s["cs"], (self.ip - 1) & 0xFFFF))

    # ---- helpers ---------------------------------------------------------
    def _inc(self, a, w):
        cf = self.flags & CF
        r = self.add(a, 1, w)
        self.setf(CF, bool(cf))  # inc preserves CF
        return r

    def _dec(self, a, w):
        cf = self.flags & CF
        r = self.sub(a, 1, w)
        self.setf(CF, bool(cf))
        return r

    def alu_compute(self, idx, a, b, w):
        if idx == 0:
            return self.add(a, b, w)
        if idx == 1:
            return self.logic(a | b, w)
        if idx == 2:
            return self.add(a, b, w, 1 if self.flags & CF else 0)
        if idx == 3:
            return self.sub(a, b, w, 1 if self.flags & CF else 0)
        if idx == 4:
            return self.logic(a & b, w)
        if idx == 5:
            return self.sub(a, b, w)
        if idx == 6:
            return self.logic(a ^ b, w)
        if idx == 7:
            self.sub(a, b, w); return a  # cmp: discard
        raise AssertionError

    def alu_op(self, op):
        idx = op >> 3
        form = op & 7
        w = 16 if (form & 1) else 8
        if form in (0, 1):  # rm, reg
            mod, reg, isr, x = self.modrm()
            a = self.rm_get(isr, x, w)
            b = self.g16(reg) if w == 16 else self.g8(reg)
            r = self.alu_compute(idx, a, b, w)
            if idx != 7:
                self.rm_set(isr, x, r, w)
        elif form in (2, 3):  # reg, rm
            mod, reg, isr, x = self.modrm()
            a = self.g16(reg) if w == 16 else self.g8(reg)
            b = self.rm_get(isr, x, w)
            r = self.alu_compute(idx, a, b, w)
            if idx != 7:
                (self.s16 if w == 16 else self.s8)(reg, r)
        else:  # 4,5: al/ax, imm
            a = self.r["ax"] if w == 16 else self.r["ax"] & 0xFF
            b = self.fetch16() if w == 16 else self.fetch8()
            r = self.alu_compute(idx, a, b, w)
            if idx != 7:
                (self.s16 if w == 16 else self.s8)(0, r)

    def alu_imm(self, idx, a, imm, w):
        return self.alu_compute(idx, a, imm & (0xFFFF if w == 16 else 0xFF), w)

    def shift_group(self, op):
        w = 16 if op in (0xC1, 0xD1, 0xD3) else 8
        mod, reg, isr, x = self.modrm()
        if op in (0xC0, 0xC1):
            cnt = self.fetch8()
        elif op in (0xD0, 0xD1):
            cnt = 1
        else:
            cnt = self.r["ax"] >> 8 if False else self.r["cx"] & 0xFF
        cnt &= 0x1F
        a = self.rm_get(isr, x, w)
        mask = 0xFFFF if w == 16 else 0xFF
        sign = 0x8000 if w == 16 else 0x80
        if cnt == 0:
            return
        if reg == 0:  # rol
            for _ in range(cnt):
                c = (a >> ((16 if w == 16 else 8) - 1)) & 1
                a = ((a << 1) | c) & mask
            self.setf(CF, bool(a & 1))
        elif reg == 1:  # ror
            for _ in range(cnt):
                c = a & 1
                a = ((a >> 1) | (c << ((16 if w == 16 else 8) - 1))) & mask
            self.setf(CF, bool(a & sign))
        elif reg == 2:  # rcl
            for _ in range(cnt):
                c = (a >> ((16 if w == 16 else 8) - 1)) & 1
                a = ((a << 1) | (1 if self.flags & CF else 0)) & mask
                self.setf(CF, bool(c))
        elif reg == 3:  # rcr
            for _ in range(cnt):
                c = a & 1
                a = ((a >> 1) | ((1 if self.flags & CF else 0) << ((16 if w == 16 else 8) - 1))) & mask
                self.setf(CF, bool(c))
        elif reg in (4, 6):  # shl/sal
            for _ in range(cnt):
                self.setf(CF, bool(a & sign))
                a = (a << 1) & mask
            self.szp(a, w)
        elif reg == 5:  # shr
            for _ in range(cnt):
                self.setf(CF, bool(a & 1))
                a >>= 1
            self.szp(a, w)
        elif reg == 7:  # sar
            s = bool(a & sign)
            for _ in range(cnt):
                self.setf(CF, bool(a & 1))
                a = (a >> 1) | (sign if s else 0)
            a &= mask
            self.szp(a, w)
        self.rm_set(isr, x, a, w)

    def grp3(self, op):
        w = 16 if op == 0xF7 else 8
        mod, reg, isr, x = self.modrm()
        a = self.rm_get(isr, x, w)
        mask = 0xFFFF if w == 16 else 0xFF
        if reg in (0, 1):  # test rm, imm
            imm = self.fetch16() if w == 16 else self.fetch8()
            self.logic(a & imm, w)
        elif reg == 2:  # not
            self.rm_set(isr, x, ~a & mask, w)
        elif reg == 3:  # neg
            self.rm_set(isr, x, self.sub(0, a, w), w)
        elif reg == 4:  # mul
            if w == 8:
                res = (self.r["ax"] & 0xFF) * a
                self.r["ax"] = res & 0xFFFF
                hi = (res >> 8) & 0xFF
            else:
                res = self.r["ax"] * a
                self.r["ax"] = res & 0xFFFF
                self.r["dx"] = (res >> 16) & 0xFFFF
                hi = self.r["dx"]
            self.setf(CF, hi != 0); self.setf(OF, hi != 0)
        elif reg == 5:  # imul
            sa = a - (mask + 1) if a & (sign := (0x8000 if w == 16 else 0x80)) else a
            if w == 8:
                al = self.r["ax"] & 0xFF
                sal = al - 256 if al & 0x80 else al
                res = sal * sa
                self.r["ax"] = res & 0xFFFF
            else:
                ax = self.r["ax"]
                sax = ax - 0x10000 if ax & 0x8000 else ax
                res = sax * sa
                self.r["ax"] = res & 0xFFFF
                self.r["dx"] = (res >> 16) & 0xFFFF
            self.setf(CF, False); self.setf(OF, False)
        elif reg == 6:  # div
            if a == 0:
                raise Halt("div by zero")
            if w == 8:
                dividend = self.r["ax"]
                self.r["ax"] = ((dividend // a) & 0xFF) | (((dividend % a) & 0xFF) << 8)
            else:
                dividend = (self.r["dx"] << 16) | self.r["ax"]
                self.r["ax"] = (dividend // a) & 0xFFFF
                self.r["dx"] = (dividend % a) & 0xFFFF
        elif reg == 7:  # idiv
            if a == 0:
                raise Halt("idiv by zero")
            sign = 0x8000 if w == 16 else 0x80
            sa = a - (mask + 1) if a & sign else a
            if w == 8:
                dividend = self.r["ax"]
                if dividend & 0x8000:
                    dividend -= 0x10000
                self.r["ax"] = ((int(dividend / sa)) & 0xFF) | (((dividend - int(dividend / sa) * sa) & 0xFF) << 8)
            else:
                dividend = (self.r["dx"] << 16) | self.r["ax"]
                if dividend & 0x80000000:
                    dividend -= 0x100000000
                q = int(dividend / sa)
                self.r["ax"] = q & 0xFFFF
                self.r["dx"] = (dividend - q * sa) & 0xFFFF

    def grp5(self):
        mod, reg, isr, x = self.modrm()
        if reg == 0:  # inc rm16
            self.rm_set(isr, x, self._inc(self.rm_get(isr, x, 16), 16), 16)
        elif reg == 1:  # dec rm16
            self.rm_set(isr, x, self._dec(self.rm_get(isr, x, 16), 16), 16)
        elif reg == 2:  # call near rm16
            self.push(self.ip); self.ip = self.rm_get(isr, x, 16)
        elif reg == 3:  # call far m16:16
            off = self.r16(x); seg = self.r16(x + 2)
            self.push(self.s["cs"]); self.push(self.ip)
            self.s["cs"] = seg; self.ip = off
        elif reg == 4:  # jmp near rm16
            self.ip = self.rm_get(isr, x, 16)
        elif reg == 5:  # jmp far m16:16
            off = self.r16(x); seg = self.r16(x + 2)
            self.s["cs"] = seg; self.ip = off
        elif reg == 6:  # push rm16
            self.push(self.rm_get(isr, x, 16))

    def do_string(self, op, rep, kind):
        w = 16 if (op & 1) else 8
        step = (2 if w == 16 else 1) * (-1 if self.flags & DF else 1)
        count = self.r["cx"] if rep else 1
        sseg = self.s[self._seg_override] if self._seg_override else self.s["ds"]
        for _ in range(count):
            if kind == "movs":
                v = self.r16(self.lin(sseg, self.r["si"])) if w == 16 else self.r8(self.lin(sseg, self.r["si"]))
                (self.w16 if w == 16 else self.w8)(self.lin(self.s["es"], self.r["di"]), v)
                self.r["si"] = (self.r["si"] + step) & 0xFFFF
                self.r["di"] = (self.r["di"] + step) & 0xFFFF
            elif kind == "stos":
                v = self.r["ax"] if w == 16 else self.r["ax"] & 0xFF
                (self.w16 if w == 16 else self.w8)(self.lin(self.s["es"], self.r["di"]), v)
                self.r["di"] = (self.r["di"] + step) & 0xFFFF
            elif kind == "lods":
                v = self.r16(self.lin(sseg, self.r["si"])) if w == 16 else self.r8(self.lin(sseg, self.r["si"]))
                (self.s16 if w == 16 else self.s8)(0, v)
                self.r["si"] = (self.r["si"] + step) & 0xFFFF
            elif kind in ("cmps", "scas"):
                if kind == "cmps":
                    a = self.r16(self.lin(sseg, self.r["si"])) if w == 16 else self.r8(self.lin(sseg, self.r["si"]))
                    self.r["si"] = (self.r["si"] + step) & 0xFFFF
                else:
                    a = self.r["ax"] if w == 16 else self.r["ax"] & 0xFF
                b = self.r16(self.lin(self.s["es"], self.r["di"])) if w == 16 else self.r8(self.lin(self.s["es"], self.r["di"]))
                self.r["di"] = (self.r["di"] + step) & 0xFFFF
                self.sub(a, b, w)            # set flags only (CMP)
                if rep:
                    self.r["cx"] = (self.r["cx"] - 1) & 0xFFFF
                    zf = bool(self.flags & ZF)
                    # F3 = REPE (continue while ZF), F2 = REPNE (continue while !ZF)
                    if (rep == 0xF3 and not zf) or (rep == 0xF2 and zf):
                        return
                    if self.r["cx"] == 0:
                        return
                continue
        if rep and kind in ("movs", "stos", "lods"):
            self.r["cx"] = 0

    def check_ret(self):
        if self.r["sp"] > self.entry_sp:
            raise Halt("returned")


def load_cpu(mem_path=None, regs_path=None):
    mem_path = mem_path or os.path.join(ROOT, "local/build/render/op12_seed_mem.bin")
    regs_path = regs_path or os.path.join(ROOT, "local/build/render/op12_seed_regs.json")
    mem = bytearray(open(mem_path, "rb").read())
    mem += bytes(0xA0000 - len(mem))
    regs = json.load(open(regs_path))
    cpu = CPU(mem)
    for n in R16:
        cpu.r[n] = regs[n]
    for n in SEG:
        cpu.s[n] = regs[n]
    cpu.ip = regs["ip"]
    cpu.entry_sp = regs["sp"]
    return cpu, regs


def execute_until_halt(cpu, cap=50_000_000, hook=None):
    """Run the seeded CPU until the top-level op12 call returns (SP > entry_sp).
    If hook is given, it is called as hook(cpu) before every instruction (used by
    op12_crack.py to trace record reads / blits for format reversal)."""
    steps = 0
    try:
        if hook is None:
            while True:
                cpu.step()
                steps += 1
                if steps > cap:
                    break
        else:
            while True:
                hook(cpu)
                cpu.step()
                steps += 1
                if steps > cap:
                    break
    except Halt:
        pass
    return steps


def validate():
    cpu, regs = load_cpu()
    tr = open(os.path.join(ROOT, "local/build/render/op12_trace.bin"), "rb").read()
    nrows = len(tr) // 26
    labels = ["cs", "ip", "ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "ds", "es", "fl"]
    for i in range(nrows):
        # trace[i] is the state AFTER one CPU step (the 0x4b0 instruction ran before
        # the trace hook was armed), so step first then compare.
        try:
            cpu.step()
        except Halt as e:
            print("HALT at step %d: %s" % (i, e)); return
        except NotImplementedError as e:
            print("UNIMPLEMENTED at step %d: %s" % (i, e)); return
        row = struct.unpack_from("<13H", tr, i * 26)
        cur = (cpu.s["cs"], cpu.ip, cpu.r["ax"], cpu.r["bx"], cpu.r["cx"], cpu.r["dx"],
               cpu.r["si"], cpu.r["di"], cpu.r["bp"], cpu.r["sp"], cpu.s["ds"], cpu.s["es"],
               cpu.flags & 0x0CD5)
        for j in range(12):  # compare cs..es (flags compared at index 12 separately)
            if cur[j] != row[j]:
                print("DIVERGE at trace step %d, field %s: cpu=%#06x trace=%#06x"
                      % (i, labels[j], cur[j], row[j]))
                if i:
                    print("  trace prev step %d: %s" % (i - 1, dict(zip(labels, struct.unpack_from("<13H", tr, (i - 1) * 26)))))
                print("  trace this step %d: %s" % (i, dict(zip(labels, row))))
                print("  cpu now:  cs=%04x ip=%04x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x sp=%04x ds=%04x es=%04x fl=%04x"
                      % cur)
                return
    print("OK: %d steps matched trace (cs..es)" % nrows)


def run_full():
    cpu, regs = load_cpu()
    cpu.wlog = (0x6C000, 0x70000, [])
    steps = 0
    try:
        while True:
            cpu.step()
            steps += 1
            if steps > 50_000_000:
                print("step cap hit"); break
    except Halt:
        pass
    print("op12 ran %d instructions" % steps)
    with open(os.path.join(ROOT, "local/build/render/op12_cpu_mem.bin"), "wb") as f:
        f.write(cpu.m[:0xA0000])
    print("post-op12 memory -> build/render/op12_cpu_mem.bin")
    wl = cpu.wlog[2]
    with open(os.path.join(ROOT, "local/build/render/op12_writeseq.bin"), "wb") as f:
        for lin, v in wl:
            f.write(struct.pack("<IB", lin, v))
    print("ordered writes to ES:0 buffer: %d -> build/render/op12_writeseq.bin" % len(wl))
    # quick stride analysis: histogram of forward deltas between consecutive writes
    from collections import Counter
    deltas = Counter()
    base = 0x472d0
    for k in range(1, len(wl)):
        d = wl[k][0] - wl[k - 1][0]
        deltas[d] += 1
    print("top write-address deltas:", deltas.most_common(10))
    print("first 24 write offsets:", [wl[k][0] - base for k in range(min(24, len(wl)))])


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "run":
        run_full()
    else:
        validate()
