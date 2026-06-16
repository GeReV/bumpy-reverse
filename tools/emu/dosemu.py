#!/usr/bin/env python3
"""Minimal DOS program emulator (Unicorn) to BOOT Bumpy and run its real
renderer, so we can read out the framebuffer — the dependency-free alternative to
a debugger-capable DOSBox. This first cut boots from the MZ entry point, serves
INT 21h DOS calls (incl. file I/O from the game dir) and stubs INT 10h/16h, and
traces what it does so we can fill in handlers iteratively.
"""
from __future__ import annotations
import struct
import sys
import os
import zlib
import collections


def write_png_local(path: str, w: int, h: int, rgb: bytes) -> None:
    def chunk(t: bytes, b: bytes) -> bytes:
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    raw = bytearray()
    for y in range(h):
        raw.append(0); raw += rgb[y * w * 3:(y + 1) * w * 3]
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" +
                           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
from typing import Dict, List, Optional
from unicorn import (Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_CODE,
                     UC_HOOK_MEM_UNMAPPED, UC_HOOK_INSN, UC_HOOK_MEM_WRITE,
                     UC_HOOK_MEM_READ, UcError)
from unicorn.x86_const import *

# Anchor all data/output paths to the repo root (tools/render/dosemu.py -> ../../),
# so the script works regardless of the caller's working directory.
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME_DIR = os.path.join(ROOT, "local/originals/old-games/bumpy")
EXE = os.path.join(ROOT, "local/originals/unpacked/BUMPY_unpacked.exe")
OUT_DIR = os.path.join(ROOT, "local/build/render")
PSP_SEG = 0x0100              # program loads at PSP_SEG+0x10
RAM = 0x110000


def load_mz(path: str):
    x = open(path, "rb").read()
    e_crlc, e_cparhdr = struct.unpack_from("<HH", x, 6)
    e_ss, e_sp, _chk, e_ip, e_cs = struct.unpack_from("<HHHHH", x, 0x0E)
    e_lfarlc = struct.unpack_from("<H", x, 0x18)[0]
    img = x[e_cparhdr * 16:]
    relocs = [struct.unpack_from("<HH", x, e_lfarlc + i * 4) for i in range(e_crlc)]
    return img, relocs, dict(ss=e_ss, sp=e_sp, ip=e_ip, cs=e_cs)


class Files:
    def __init__(self) -> None:
        self.handles: Dict[int, object] = {}
        self.next = 5
        self.bydir = {f.upper(): os.path.join(GAME_DIR, f) for f in os.listdir(GAME_DIR)}

    def open(self, name: str) -> int:
        key = name.upper().split("\\")[-1].split("/")[-1].strip()
        if "." in key:                      # normalize 8.3 space padding: "DDFNT2  .CAR" -> "DDFNT2.CAR"
            b, e = key.rsplit(".", 1)
            key = b.strip() + "." + e.strip()
        path = self.bydir.get(key)
        if not path:
            return -1
        h = self.next; self.next += 1
        self.handles[h] = open(path, "rb")
        return h


def main() -> None:
    img, relocs, hdr = load_mz(EXE)
    base = PSP_SEG + 0x10
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, RAM)
    uc.mem_write(base * 16, img)
    for off, seg in relocs:
        lin = base * 16 + ((seg * 16 + off) & 0xFFFFF)
        v = struct.unpack("<H", uc.mem_read(lin, 2))[0]
        uc.mem_write(lin, struct.pack("<H", (v + base) & 0xFFFF))
    # minimal PSP
    uc.mem_write(PSP_SEG * 16, b"\xCD\x20")            # INT 20h at PSP:0
    uc.mem_write(PSP_SEG * 16 + 2, struct.pack("<H", 0xA000))  # top of memory

    files = Files()
    tr = dict(instr=0, ints=collections.Counter(), last_ip=0, mode=None, keys=list("\r "))
    free_top = [0x1C00]                 # paragraph bump allocator (above program)
    FREE_END = 0x9000

    def set_cf(set_it: bool):
        fl = uc.reg_read(UC_X86_REG_EFLAGS)
        uc.reg_write(UC_X86_REG_EFLAGS, (fl | 1) if set_it else (fl & ~1))

    def rd(seg, off, n): return bytes(uc.mem_read(seg * 16 + off, n))

    def hook_intr(uc, intno, _):
        ax = uc.reg_read(UC_X86_REG_AX); ah = (ax >> 8) & 0xFF; al = ax & 0xFF
        tr["ints"][(intno, ah)] += 1
        if intno == 0x21:
            if ah == 0x4C:                                     # exit
                tr["exit"] = al; uc.emu_stop(); return
            elif ah == 0x30:                                   # DOS version
                uc.reg_write(UC_X86_REG_AX, 0x0005)
            elif ah == 0x25:                                   # set interrupt vector AL -> DS:DX
                ds = uc.reg_read(UC_X86_REG_DS); dx = uc.reg_read(UC_X86_REG_DX)
                uc.mem_write(al * 4, struct.pack("<HH", dx, ds))
                tr.setdefault("vectors", []).append((al, ds, dx))
            elif ah == 0x35:                                   # get interrupt vector AL -> ES:BX
                off, seg = struct.unpack("<HH", uc.mem_read(al * 4, 4))
                uc.reg_write(UC_X86_REG_BX, off); uc.reg_write(UC_X86_REG_ES, seg)
            elif ah in (0x1A, 0x2C, 0x2A, 0x30, 0x44, 0x33, 0x19, 0x0E):
                pass                                           # date/ioctl/etc — ignore
            elif ah == 0x48:                                   # allocate BX paragraphs
                bx = uc.reg_read(UC_X86_REG_BX)
                avail = FREE_END - free_top[0]
                if bx > avail:
                    uc.reg_write(UC_X86_REG_AX, 8); uc.reg_write(UC_X86_REG_BX, avail); set_cf(True)
                else:
                    uc.reg_write(UC_X86_REG_AX, free_top[0]); free_top[0] += bx; set_cf(False)
            elif ah == 0x49:                                   # free — ok
                set_cf(False)
            elif ah == 0x4A:                                   # resize ES block — ok
                set_cf(False)
            elif ah == 0x3D:                                   # open DS:DX
                name = b""; o = uc.reg_read(UC_X86_REG_DX); ds = uc.reg_read(UC_X86_REG_DS)
                while True:
                    c = uc.mem_read(ds * 16 + o, 1)[0]
                    if c == 0: break
                    name += bytes([c]); o += 1
                h = files.open(name.decode("latin1"))
                tr.setdefault("fileops", []).append(("open", name.decode("latin1"), h))
                fl = uc.reg_read(UC_X86_REG_EFLAGS)
                if h < 0:
                    uc.reg_write(UC_X86_REG_AX, 2); uc.reg_write(UC_X86_REG_EFLAGS, fl | 1)
                else:
                    uc.reg_write(UC_X86_REG_AX, h); uc.reg_write(UC_X86_REG_EFLAGS, fl & ~1)
            elif ah == 0x3F:                                   # read BX -> DS:DX, CX bytes
                h = uc.reg_read(UC_X86_REG_BX); cx = uc.reg_read(UC_X86_REG_CX)
                ds = uc.reg_read(UC_X86_REG_DS); dx = uc.reg_read(UC_X86_REG_DX)
                f = files.handles.get(h)
                data = f.read(cx) if f else b""
                uc.mem_write(ds * 16 + dx, data)
                if len(data) < cx:                                 # EOF/short read: zero the whole
                    # requested-but-unread tail. op4 decodes in-place (expands past the compressed
                    # data), so the post-decode .VEC record stream must read 0 (w4=0 terminator).
                    uc.mem_write(ds * 16 + dx + len(data), b"\x00" * (cx - len(data)))
                uc.reg_write(UC_X86_REG_AX, len(data))
                tr.setdefault("fileops", []).append(("read", h, cx, len(data), (ds * 16 + dx)))
            elif ah == 0x3E:                                   # close
                h = uc.reg_read(UC_X86_REG_BX)
                if h in files.handles: files.handles.pop(h).close()
            elif ah == 0x42:                                   # seek
                h = uc.reg_read(UC_X86_REG_BX); f = files.handles.get(h)
                if f:
                    off = (uc.reg_read(UC_X86_REG_CX) << 16) | uc.reg_read(UC_X86_REG_DX)
                    f.seek(off, al)
                    p = f.tell()
                    uc.reg_write(UC_X86_REG_DX, (p >> 16) & 0xFFFF); uc.reg_write(UC_X86_REG_AX, p & 0xFFFF)
        elif intno == 0x10:
            if ah == 0x00:
                tr["mode"] = al                                # set video mode
        elif intno == 0x16:
            # keyboard: feed queued keys, else report none
            if ah in (0x00, 0x10):
                k = ord(tr["keys"].pop(0)) if tr["keys"] else 0x0D
                uc.reg_write(UC_X86_REG_AX, k)
            elif ah in (0x01, 0x11):
                fl = uc.reg_read(UC_X86_REG_EFLAGS)
                uc.reg_write(UC_X86_REG_EFLAGS, (fl & ~0x40) if tr["keys"] else (fl | 0x40))
        # default: return

    def hook_code(uc, addr, size, _):
        tr["instr"] += 1; tr["last_ip"] = addr
        if addr == 0x3E14:                 # start_level entry
            tr["start_level"] = tr.get("start_level", 0) + 1
            tr["in_sl"] = True
        elif addr == 0x2EDE:               # poll_input entry
            tr["poll"] = tr.get("poll", 0) + 1
        if tr.get("in_sl"):                # after start_level: profile IPs to find the hang loop
            h = tr.setdefault("slips", {})
            h[addr] = h.get(addr, 0) + 1
        if tr["instr"] > 30_000_000:
            uc.emu_stop()

    def hook_unmapped(uc, access, addr, size, value, _):
        tr["fault"] = (addr, tr["last_ip"]); uc.emu_stop(); return False

    io = [0]
    cur_scan = [0]

    def hook_in(uc, port, size, _):
        io[0] += 1
        if port == 0x40:            # PIT ch0 counter — free-running so timing loops progress
            return (io[0] * 0x11) & 0xFF
        if port == 0x201:           # game/joystick port — no joystick
            return 0xF0
        if port == 0x3DA:           # VGA input status — toggle retrace/vsync bits
            attr_ff[0] = 0          # reading 0x3DA resets the Attribute Controller flip-flop
            return (io[0] & 1) * 0x09
        if port == 0x60:            # keyboard data port — current scancode
            return cur_scan[0]
        if port == 0x61:
            return 0xFF
        return 0xFF

    # --- minimal VGA planar emulation (mode 0xD: 4 bit-planes at 0xA000) ---
    seq = bytearray(256); gc = bytearray(256); seq_i = [0]; gc_i = [0]
    latch = [0, 0, 0, 0]
    plane = [bytearray(0x10000) for _ in range(4)]
    dac = [[0, 0, 0] for _ in range(256)]; dac_i = [0]; dac_sub = [0]
    dac_written = set()                            # which DAC entries the game programs
    # Attribute Controller (port 0x3C0): 16 palette regs remap pixel value -> DAC index.
    # 16-colour GRAPHICS mode 0x0D default attribute palette: logical 0..7 -> DAC 0..7,
    # logical 8..15 -> DAC 0x10..0x17 (NOT the text-mode default with 0x14/0x38..0x3F, and
    # NOT identity). The game loads its 16-colour palette into DAC 0..7 + 0x10..0x17, so
    # this mapping makes logical colours read the game's real colours.
    ATTR_DEFAULT = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17]
    attr = bytearray(32)
    for _i, _a in enumerate(ATTR_DEFAULT):
        attr[_i] = _a
    attr_i = [0]; attr_ff = [0]                    # ff: 0 = expect index, 1 = expect data
    # Seed the BIOS-default 16-colour EGA RGB into the DAC slots the attr map points at,
    # so any colour the game doesn't overwrite still has a sane value.
    _ega = [(0, 0, 0), (0, 0, 42), (0, 42, 0), (0, 42, 42), (42, 0, 0), (42, 0, 42),
            (42, 21, 0), (42, 42, 42), (21, 21, 21), (21, 21, 63), (21, 63, 21),
            (21, 63, 63), (63, 21, 21), (63, 21, 63), (63, 63, 21), (63, 63, 63)]
    for _i, _c in enumerate(_ega):
        dac[ATTR_DEFAULT[_i]] = list(_c)

    def hook_out(uc, port, size, value, _):
        value &= 0xFFFF
        if port == 0x3C0:                          # Attribute Controller (index/data)
            if attr_ff[0] == 0:
                attr_i[0] = value & 0x1F           # bits 0-4 = register; bit5 = PAS
                attr_ff[0] = 1
            else:
                if attr_i[0] < 0x20:
                    attr[attr_i[0]] = value & 0xFF
                attr_ff[0] = 0
            return
        if port in (0x3C4, 0x3CE):                 # index (or index+data word)
            reg = seq if port == 0x3C4 else gc
            idx = seq_i if port == 0x3C4 else gc_i
            idx[0] = value & 0xFF
            if size == 2:
                reg[value & 0xFF] = (value >> 8) & 0xFF
        elif port in (0x3C5, 0x3CF):               # data
            reg = seq if port == 0x3C5 else gc
            idx = seq_i if port == 0x3C5 else gc_i
            reg[idx[0]] = value & 0xFF
        elif port == 0x3C8:                        # DAC write index
            dac_i[0] = value & 0xFF; dac_sub[0] = 0
        elif port == 0x3C9:                        # DAC data (R,G,B 6-bit)
            dac[dac_i[0] & 0xFF][dac_sub[0]] = value & 0x3F
            dac_sub[0] += 1
            if dac_sub[0] == 3:
                dac_written.add(dac_i[0] & 0xFF)
                dac_sub[0] = 0; dac_i[0] = (dac_i[0] + 1) & 0xFF

    def hook_vga_write(uc, access, addr, size, value, _):
        off = (addr - 0xA0000) & 0xFFFF
        for k in range(size):
            val = (value >> (8 * k)) & 0xFF
            o = (off + k) & 0xFFFF
            wm = gc[5] & 3; mm = seq[2] & 0xF; bm = gc[8]
            sr = gc[0]; esr = gc[1]; fn = (gc[3] >> 3) & 3; rot = gc[3] & 7
            for p in range(4):
                if not (mm & (1 << p)):
                    continue
                lat = latch[p]
                if wm == 1:
                    res = lat
                elif wm == 2:
                    v = 0xFF if (val & (1 << p)) else 0
                    if fn == 1: v &= lat
                    elif fn == 2: v |= lat
                    elif fn == 3: v ^= lat
                    res = (v & bm) | (lat & ~bm)
                elif wm == 3:
                    rv = ((val >> rot) | (val << (8 - rot))) & 0xFF if rot else val
                    m = bm & rv; sv = 0xFF if (sr & (1 << p)) else 0
                    res = (sv & m) | (lat & ~m)
                else:                              # write mode 0
                    v = ((val >> rot) | (val << (8 - rot))) & 0xFF if rot else val
                    if esr & (1 << p):
                        v = 0xFF if (sr & (1 << p)) else 0
                    if fn == 1: v &= lat
                    elif fn == 2: v |= lat
                    elif fn == 3: v ^= lat
                    res = (v & bm) | (lat & ~bm)
                plane[p][o] = res & 0xFF

    def hook_vga_read(uc, access, addr, size, value, _):
        off = (addr - 0xA0000) & 0xFFFF
        for p in range(4):
            latch[p] = plane[p][off]
        uc.mem_write(addr, bytes([plane[gc[4] & 3][off]]))   # read map select

    # Address-SCOPED counters (only fire at these exact linear addrs -> cheap):
    # op4 handler entry 1c28:0194 and vec_read_record 1c28:0a09 (dosemu base 0x110).
    OV = 0x1100 + (0x1c28 - 0x1000) * 16                      # segment 1c28 -> dosemu linear
    def hook_op4(uc, addr, size, _):
        tr["op4"] = tr.get("op4", 0) + 1
        if os.environ.get("DOSEMU_TRACE_OP12"):
            def w(o):
                return struct.unpack("<H", uc.mem_read(DG_LIN + o, 2))[0]
            lastfile = next((o[1] for o in reversed(tr.get("fileops", [])) if o[0] == "open"), "?")
            es = uc.reg_read(UC_X86_REG_ES); di = uc.reg_read(UC_X86_REG_DI)
            tr.setdefault("op4calls", []).append({
                "dest": ((es * 16 + di) & 0xFFFFF),
                "declared_len": (w(0x4e0a) | (w(0x4e0c) << 16)),
                "lastfile": lastfile})
            # snapshot the op4 entry state for the chosen file (DOSEMU_OP4FILE), so the
            # pure-Python op4 transliteration can be validated against the real output.
            want = os.environ.get("DOSEMU_OP4FILE")
            if want and want in lastfile and not tr.get("op4_snapped"):
                tr["op4_snapped"] = True
                regs = {"stream": (w(0x4e10) << 4) + w(0x4e0e),
                        "vend": (w(0x4e0c) << 4) + w(0x4e0a),
                        "vec_src": w(0x4e24) | (w(0x4e26) << 16),
                        "vsav": w(0x4e28) | (w(0x4e2a) << 16),
                        "dest": (es * 16 + di) & 0xFFFFF, "file": lastfile}
                with open(os.path.join(OUT_DIR, "op4_seed_mem.bin"), "wb") as f:
                    f.write(uc.mem_read(0, 0xA0000))
                import json as _j
                with open(os.path.join(OUT_DIR, "op4_seed.json"), "w") as f:
                    _j.dump(regs, f)
                print("op4 entry snapshot (%s) -> build/render/op4_seed_*" % lastfile)
    def hook_vrr(uc, addr, size, _):
        tr["vrr"] = tr.get("vrr", 0) + 1
    uc.hook_add(UC_HOOK_CODE, hook_op4, None, OV + 0x194, OV + 0x194)
    uc.hook_add(UC_HOOK_CODE, hook_vrr, None, OV + 0xA09, OV + 0xA09)
    NORM = 0x1100 + (0x1cda - 0x1000) * 16 + 0x89             # vec_xform normalizer 1cda:0089
    def hook_norm(uc, addr, size, _):
        sp = uc.reg_read(UC_X86_REG_SP); ss = uc.reg_read(UC_X86_REG_SS)
        ret = struct.unpack("<HH", uc.mem_read(ss * 16 + sp, 4))   # (ip, cs) of lcall return
        callers = tr.setdefault("norm_callers", {})
        key = (ret[1], ret[0])
        callers[key] = callers.get(key, 0) + 1
    uc.hook_add(UC_HOOK_CODE, hook_norm, None, NORM, NORM)

    # vec_run entry (1c28:0000): capture the incoming calling-convention registers
    #   DI:SI = stream far ptr, AX:BX = bytes_read, CX:DX = declared length (-> end bound)
    DG_LIN = (0x103b + 0x110) * 16                            # DGROUP linear in dosemu (0x114b0)
    def hook_vecrun(uc, addr, size, _):
        tr.setdefault("vecrun", []).append({
            "SI": uc.reg_read(UC_X86_REG_SI), "DI": uc.reg_read(UC_X86_REG_DI),
            "AX": uc.reg_read(UC_X86_REG_AX), "BX": uc.reg_read(UC_X86_REG_BX),
            "CX": uc.reg_read(UC_X86_REG_CX), "DX": uc.reg_read(UC_X86_REG_DX)})
    uc.hook_add(UC_HOOK_CODE, hook_vecrun, None, OV + 0x000, OV + 0x000)
    def _commit_seed(tr):
        p = tr.get("pending")
        if not p or tr.get("seeded"):
            return
        tr["seeded"] = True
        with open(os.path.join(OUT_DIR, "op12_seed_mem.bin"), "wb") as f:
            f.write(p["mem"])
        import json as _j
        with open(os.path.join(OUT_DIR, "op12_seed_regs.json"), "w") as f:
            _j.dump(p["regs"], f, indent=2)
        # live 16-colour logical palette (pixel -> attr controller -> DAC), 6-bit RGB
        lpal = [list(dac[attr[i] & 0x3F]) for i in range(16)]
        with open(os.path.join(OUT_DIR, "op12_seed_pal.json"), "w") as f:
            _j.dump(lpal, f)
        print("op12 seed snapshot (top-level call %d) -> build/render/op12_seed_*"
              % p["tlcall"])

    # op12 entry (1c28:04b0): the bounded plot. Capture the end-bound vs current src ptr.
    def hook_op12(uc, addr, size, _):
        tr["op12"] = tr.get("op12", 0) + 1
        if tr.get("watch"):
            sp = uc.reg_read(UC_X86_REG_ESP) & 0xFFFF
            if sp >= 0x01B0:  # top-level op12 call (recursion uses lower SP)
                tr["tlcall"] = tr.get("tlcall", 0) + 1
                tr.setdefault("tlmap", {})  # tlcall -> [low_writes, high_writes]
                if os.environ.get("DOSEMU_TRACE_OP12") and tr["tlcall"] <= 12:
                    def w(o):
                        return struct.unpack("<H", uc.mem_read(DG_LIN + o, 2))[0]
                    ent = {"call": tr["tlcall"], "sp": sp,
                           "out_len": w(0x4e24),          # vec_src off = output length
                           "vec_end": (w(0x4e0c), w(0x4e0a)),
                           "stream": (w(0x4e10), w(0x4e0e)),
                           "es": uc.reg_read(UC_X86_REG_ES),
                           "src_reg": collections.Counter(), "dst_reg": collections.Counter(),
                           "nplots": 0}
                    tr.setdefault("op12calls", []).append(ent)
                    tr["curcall"] = ent
        if tr["op12"] <= 16:
            def w(o):
                return struct.unpack("<H", uc.mem_read(DG_LIN + o, 2))[0]
            tr.setdefault("op12_log", []).append({
                "end": (w(0x4e0c), w(0x4e0a)), "src": (w(0x4e26), w(0x4e24)),
                "opcode": w(0x4e31)})
        # Speculatively snapshot every top-level op12 entry; hook_plot commits the
        # snapshot the instant a framebuffer (high-heap) write occurs, so the seed is
        # always the entry of the level-rendering call (auto-detected per level).
        # DOSEMU_OP12CALL=N forces a specific top-level call index instead.
        forced = os.environ.get("DOSEMU_OP12CALL")
        if tr.get("watch") and not tr.get("seeded") and sp >= 0x01B0:
            tr["call_writes"] = 0  # reset per-call 0x899 write counter
            ss = uc.reg_read(UC_X86_REG_SS)
            tr["pending"] = {
                "regs": {
                    "ax": uc.reg_read(UC_X86_REG_EAX) & 0xFFFF,
                    "bx": uc.reg_read(UC_X86_REG_EBX) & 0xFFFF,
                    "cx": uc.reg_read(UC_X86_REG_ECX) & 0xFFFF,
                    "dx": uc.reg_read(UC_X86_REG_EDX) & 0xFFFF,
                    "si": uc.reg_read(UC_X86_REG_ESI) & 0xFFFF,
                    "di": uc.reg_read(UC_X86_REG_EDI) & 0xFFFF,
                    "bp": uc.reg_read(UC_X86_REG_EBP) & 0xFFFF,
                    "sp": sp, "ds": uc.reg_read(UC_X86_REG_DS), "es": uc.reg_read(UC_X86_REG_ES),
                    "ss": ss, "cs": uc.reg_read(UC_X86_REG_CS),
                    "ip": uc.reg_read(UC_X86_REG_IP), "OV": OV, "DG_LIN": DG_LIN,
                },
                "mem": bytes(uc.mem_read(0, 0xA0000)),
                "tlcall": tr.get("tlcall", 0),
            }
            if forced is not None and tr.get("tlcall") == int(forced):
                _commit_seed(tr)

    # Dump emulator memory the instant the first op12 call returns to its caller,
    # giving an exact reference output for the pure-Python CPU port (no IRQ skew).
    def hook_after(uc, addr, size, _):
        if not tr.get("seeded") or tr.get("after_done"):
            return
        if (uc.reg_read(UC_X86_REG_CS) == tr.get("ret_cs")
                and (uc.reg_read(UC_X86_REG_IP)) == tr.get("ret_ip")
                and (uc.reg_read(UC_X86_REG_ESP) & 0xFFFF) == tr.get("ret_sp")):
            tr["after_done"] = True
            tr["trace_on"] = False
            with open(os.path.join(OUT_DIR, "op12_emu_after.bin"), "wb") as f:
                f.write(uc.mem_read(0, 0xA0000))
            print("emu memory after 1st op12 -> build/render/op12_emu_after.bin")

    # Bounded instruction trace from the first level op12 entry: ground truth to
    # validate the pure-Python CPU port (tools/extract/vec_cpu.py) step-by-step.
    def hook_trace(uc, addr, size, _):
        if not tr.get("trace_on"):
            return
        t = tr.setdefault("trace", [])
        if len(t) >= 2000:
            tr["trace_on"] = False
            return
        rd = uc.reg_read
        t.append((uc.reg_read(UC_X86_REG_CS), uc.reg_read(UC_X86_REG_IP),
                  rd(UC_X86_REG_EAX) & 0xFFFF, rd(UC_X86_REG_EBX) & 0xFFFF,
                  rd(UC_X86_REG_ECX) & 0xFFFF, rd(UC_X86_REG_EDX) & 0xFFFF,
                  rd(UC_X86_REG_ESI) & 0xFFFF, rd(UC_X86_REG_EDI) & 0xFFFF,
                  rd(UC_X86_REG_EBP) & 0xFFFF, rd(UC_X86_REG_ESP) & 0xFFFF,
                  rd(UC_X86_REG_DS), rd(UC_X86_REG_ES), rd(UC_X86_REG_EFLAGS) & 0x0CD5))
    uc.hook_add(UC_HOOK_CODE, hook_trace, None, 1, 0)  # all-address code hook
    uc.hook_add(UC_HOOK_CODE, hook_after, None, 1, 0)  # all-address: detect op12 return

    # blit_sprite (1000:942a): capture p1_sprite descriptor [x, y, frame] at each call.
    BLIT = 0x1100 + 0x942a                                    # main code seg base 0x1100
    def hook_blit(uc, addr, size, _):
        d = uc.mem_read(0x114b0 + 0x792e, 6)                  # p1_sprite @ DGROUP 0x792e
        tr.setdefault("blits", []).append(
            (tr["instr"], d[0] | (d[1] << 8), d[2] | (d[3] << 8), d[4] | (d[5] << 8)))

    def dump_fresh_bum() -> None:
        # Capture the freshly-decoded .BUM level table (DAT_6bf2 = level_bum_buf+2),
        # before gameplay mutates it. Called from the main loop once the level loads.
        if tr.get("bum_dumped"):
            return
        p = uc.mem_read(0x114b0 + 0x6bf2, 4)                  # off, seg of level_bum_buf+2
        lin = ((p[2] | (p[3] << 8)) << 4) + (p[0] | (p[1] << 8)) - 2
        buf = bytes(uc.mem_read(lin, 0xb60))
        if buf[2 + 0x90] == 0 or buf[2 + 0x90] > 48:          # spawn cell sane -> .BUM is decoded
            return
        tr["bum_dumped"] = True
        wn = os.environ.get("DOSEMU_LEVEL", "1")
        os.makedirs(os.path.join(OUT_DIR, "bum"), exist_ok=True)
        open(os.path.join(OUT_DIR, "bum", "world%s.bum" % wn), "wb").write(buf)
        print("dumped fresh .BUM (world %s) -> build/render/bum/world%s.bum" % (wn, wn))
    uc.hook_add(UC_HOOK_CODE, hook_blit, None, BLIT, BLIT)
    uc.hook_add(UC_HOOK_CODE, hook_op12, None, OV + 0x4B0, OV + 0x4B0)
    # op12 pixel-write (1c28:079d `mov es:[di],al`): capture op12's plot dest+value to
    # learn its output buffer layout (gated to the level so it's the level's plot).
    def hook_plot(uc, addr, size, _):
        if not tr.get("watch"):
            return
        es = uc.reg_read(UC_X86_REG_ES); di = uc.reg_read(UC_X86_REG_DI)
        al = uc.reg_read(UC_X86_REG_EAX) & 0xFF
        lin = (es * 16 + di) & 0xFFFFF
        tr.setdefault("plot", {})[lin] = al
        tc = tr.get("tlcall", 0)
        tr.setdefault("tlmap", {})[tc] = tr.get("tlmap", {}).get(tc, 0) + 1
        # The level-render call fills the whole screen (~10k+ 0x899 writes); the
        # record-preprocessing calls write <1500. Trigger on per-call write volume
        # (address-independent, so it finds the renderer on every level).
        tr["call_writes"] = tr.get("call_writes", 0) + 1
        if (tr["call_writes"] == 3000 and not tr.get("seeded")
                and os.environ.get("DOSEMU_OP12CALL") is None):
            _commit_seed(tr)  # this top-level call is the level renderer
    uc.hook_add(UC_HOOK_CODE, hook_plot, None, OV + 0x899, OV + 0x899)   # final row-blit dest

    # op12 masked-blit plot (1c28:079d `mov es:[di],al`): per-call source/dest region
    # histograms, to reverse where each op12 call reads pixels from and writes to.
    TRACE_CALL = int(os.environ.get("DOSEMU_TRACE_CALL", "1"))   # full-seq capture target

    def hook_plot79d(uc, addr, size, _):
        ent = tr.get("curcall")
        if ent is None:
            return
        ds = uc.reg_read(UC_X86_REG_DS); si = uc.reg_read(UC_X86_REG_SI)
        es = uc.reg_read(UC_X86_REG_ES); di = uc.reg_read(UC_X86_REG_DI)
        src = (ds * 16 + si) & 0xFFFFF; dst = (es * 16 + di) & 0xFFFFF
        ent["src_reg"][src & ~0x3FFF] += 1
        ent["dst_reg"][dst & ~0x3FFF] += 1
        ent["nplots"] += 1
        if "first_src" not in ent:
            ent["first_src"] = src; ent["first_dst"] = dst
        ent["last_src"] = src; ent["last_dst"] = dst
        if ent["call"] == TRACE_CALL:
            tr.setdefault("seq", []).append((dst, 1, src, uc.mem_read(src, 1)[0]))

    def hook_fill8e3(uc, addr, size, _):   # op12 transparent fill `mov es:[di],al` (al=0xff)
        ent = tr.get("curcall")
        if ent is None or ent["call"] != TRACE_CALL:
            return
        es = uc.reg_read(UC_X86_REG_ES); di = uc.reg_read(UC_X86_REG_DI)
        tr.setdefault("seq", []).append(((es * 16 + di) & 0xFFFFF, 0, 0, uc.reg_read(UC_X86_REG_AX) & 0xFF))

    def hook_blit899(uc, addr, size, _):   # op12 row-blit `mov es:[di],al` (al=[ds:si])
        ent = tr.get("curcall")
        if ent is None or ent["call"] != TRACE_CALL:
            return
        es = uc.reg_read(UC_X86_REG_ES); di = uc.reg_read(UC_X86_REG_DI)
        ds = uc.reg_read(UC_X86_REG_DS); si = uc.reg_read(UC_X86_REG_SI)
        tr.setdefault("seq", []).append(((es*16+di) & 0xFFFFF, 2, (ds*16+si) & 0xFFFFF,
                                         uc.reg_read(UC_X86_REG_AX) & 0xFF))

    def hook_maskreload(uc, addr, size, _):   # op12 mask reload @ 0x734: ds:si -> 4 mask bytes
        ent = tr.get("curcall")
        if ent is None:
            return
        ds = uc.reg_read(UC_X86_REG_DS); si = uc.reg_read(UC_X86_REG_SI)
        lin = (ds * 16 + si) & 0xFFFFF
        if "mask_start" not in ent:
            ent["mask_start"] = lin            # first mask word address for this call
        if ent["call"] == TRACE_CALL:
            tr.setdefault("maskreads", []).append((lin, bytes(uc.mem_read(lin, 4))))

    if os.environ.get("DOSEMU_TRACE_OP12"):
        uc.hook_add(UC_HOOK_CODE, hook_plot79d, None, OV + 0x79D, OV + 0x79D)
        uc.hook_add(UC_HOOK_CODE, hook_fill8e3, None, OV + 0x8E3, OV + 0x8E3)
        uc.hook_add(UC_HOOK_CODE, hook_blit899, None, OV + 0x899, OV + 0x899)
        uc.hook_add(UC_HOOK_CODE, hook_maskreload, None, OV + 0x734, OV + 0x734)
    # Gated write histogram: once D1.PAV is open the renderer's op12 plot dominates
    # heap writes, so the hottest 2KB page = the playfield buffer. Gate keeps the
    # per-write Python callback cheap before the level decode.
    whist: Dict[int, int] = {}
    def hook_heap_write(uc, access, addr, size, value, _):
        if tr.get("watch"):
            p = addr & ~0x7FF
            whist[p] = whist.get(p, 0) + 1
    uc.hook_add(UC_HOOK_MEM_WRITE, hook_heap_write, None, 0x40000, 0x90000)
    # open_resource(index,4) entry (1000:736f): log the resource index sequence so
    # we can see how far start_level gets (PAV=0, DEC=1, BUM=8, ...).
    OR_LIN = 0x1100 + 0x736F
    def hook_openres(uc, addr, size, _):
        sp = uc.reg_read(UC_X86_REG_SP); ss = uc.reg_read(UC_X86_REG_SS)
        idx = struct.unpack("<H", uc.mem_read(ss * 16 + sp + 2, 2))[0]   # arg1 above retaddr
        tr.setdefault("openres", []).append(idx)
    uc.hook_add(UC_HOOK_CODE, hook_openres, None, OR_LIN, OR_LIN)
    # Null-catcher: execution reaching linear 0..0x4ff means a bad far call/ret/int
    # jumped to null. Fires only on that crash (cheap). Capture regs + stack trail.
    def hook_null(uc, addr, size, _):
        if tr.get("nullhit") or not tr.get("watch"):
            return
        ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
        stack = struct.unpack("<16H", uc.mem_read(ss * 16 + sp, 32))
        tr["nullhit"] = {
            "cs": uc.reg_read(UC_X86_REG_CS), "ip": uc.reg_read(UC_X86_REG_IP),
            "ss": ss, "sp": sp, "ax": uc.reg_read(UC_X86_REG_AX),
            "stack": stack}
        uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hook_null, None, 0x0, 0x4FF)

    uc.hook_add(UC_HOOK_INTR, hook_intr)
    # NOTE: UC_HOOK_CODE (per-instruction Python callback) is left UNREGISTERED for
    # speed — Unicorn enforces emu_start(count=) natively (~50x faster). Re-enable
    # hook_code only when per-instruction profiling/last_ip is needed.
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, hook_unmapped)
    uc.hook_add(UC_HOOK_MEM_WRITE, hook_vga_write, None, 0xA0000, 0xAFFFF)
    uc.hook_add(UC_HOOK_MEM_READ, hook_vga_read, None, 0xA0000, 0xAFFFF)
    try:
        uc.hook_add(UC_HOOK_INSN, hook_in, None, 1, 0, UC_X86_INS_IN)
        uc.hook_add(UC_HOOK_INSN, hook_out, None, 1, 0, UC_X86_INS_OUT)
    except Exception:
        pass

    def cur_lin() -> int:
        return ((uc.reg_read(UC_X86_REG_CS) & 0xFFFF) * 16 + (uc.reg_read(UC_X86_REG_IP) & 0xFFFF)) & 0xFFFFF

    def fire_int(n: int) -> None:
        """Push an IRET frame and redirect to the installed ISR for interrupt n."""
        ip = uc.reg_read(UC_X86_REG_IP) & 0xFFFF; cs = uc.reg_read(UC_X86_REG_CS) & 0xFFFF
        fl = uc.reg_read(UC_X86_REG_EFLAGS) & 0xFFFF
        ss = uc.reg_read(UC_X86_REG_SS) & 0xFFFF; sp = uc.reg_read(UC_X86_REG_SP) & 0xFFFF
        for v in (fl, cs, ip):
            sp = (sp - 2) & 0xFFFF; uc.mem_write(ss * 16 + sp, struct.pack("<H", v))
        uc.reg_write(UC_X86_REG_SP, sp)
        vec = struct.unpack("<I", uc.mem_read(n * 4, 4))[0]
        if vec == 0:
            return
        uc.reg_write(UC_X86_REG_CS, (vec >> 16) & 0xFFFF); uc.reg_write(UC_X86_REG_IP, vec & 0xFFFF)

    uc.reg_write(UC_X86_REG_DS, PSP_SEG); uc.reg_write(UC_X86_REG_ES, PSP_SEG)
    uc.reg_write(UC_X86_REG_SS, (hdr["ss"] + base) & 0xFFFF); uc.reg_write(UC_X86_REG_SP, hdr["sp"])
    uc.reg_write(UC_X86_REG_CS, (hdr["cs"] + base) & 0xFFFF); uc.reg_write(UC_X86_REG_IP, hdr["ip"])

    dg = (0x103b + base) & 0xFFFF                              # DGROUP segment

    # Default IRET handler for hardware-interrupt vectors. The game's init saves the
    # ORIGINAL INT 8 (timer) / INT 0F vectors (INT 21/AH=35) and its own timer ISR
    # chains to them via `pushf; lcall [saved]` — which expects an IRET handler
    # (pops FLAGS+CS+IP, 3 words). Real DOS/BIOS provide such ISRs; this emulator
    # left those vectors null, so the game saved 0000:0000 and chained into null.
    # Install a bare IRET stub (0050:0000) into every still-null vector at boot.
    uc.mem_write(0x500, b"\xCF")                               # IRET at linear 0x500 (seg 0x50)
    iret_vec = (0x0050 << 16) | 0x0000                        # far ptr 0050:0000
    for v in range(0x100):
        if struct.unpack("<I", uc.mem_read(v * 4, 4))[0] == 0:
            uc.mem_write(v * 4, struct.pack("<I", iret_vec))

    def set_key(scancode: int, down: bool) -> None:
        mbase = struct.unpack("<H", uc.mem_read(dg * 16 + 0x4D42, 2))[0]
        uc.mem_write(dg * 16 + mbase + (scancode & 0x7F), bytes([scancode if down else 0]))

    def opened(name: str) -> bool:
        return any(o[0] == "open" and o[1] == name for o in tr.get("fileops", []))

    LEVEL = int(os.environ.get("DOSEMU_LEVEL", "1"))          # which level to drive to
    PAVNAME = "D%d.PAV" % LEVEL
    BUMNAME = "D%d.BUM" % LEVEL

    def force_level() -> None:
        # Force current_level (DGROUP 0x79b2) and skip copy protection: copyprotect_flag
        # (0x119a) = 1 avoids both the level>1 code-wheel challenge and the reset-to-1.
        uc.mem_write(dg * 16 + 0x79B2, bytes([LEVEL & 0xFF]))
        uc.mem_write(dg * 16 + 0x119A, bytes([1]))

    begin = cur_lin(); err = None; CHUNK = 1_000_000; total = 0
    countdown = None
    while total < 400_000_000:
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e)
            cs = uc.reg_read(UC_X86_REG_CS); ip = uc.reg_read(UC_X86_REG_IP)
            lin = (cs * 16 + ip) & 0xFFFFF
            code = bytes(uc.mem_read(lin, 16))
            ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
            stk = struct.unpack("<16H", uc.mem_read(ss * 16 + sp, 32))
            tr["errloc"] = {"cs": cs, "ip": ip, "lin": lin, "code": code.hex(),
                            "ghidra": ((cs - 0x110 + 0x1000) & 0xFFFF, ip),
                            "ss": ss, "sp": sp, "stack": stk}
            break
        total += CHUNK
        if tr.get("exit") is not None or tr.get("fault") or tr.get("nullhit"):
            break
        begin = cur_lin()
        c = total // CHUNK
        if os.environ.get("DOSEMU_MENU"):              # capture the title/menu only
            for sc in (0x3D, 0x41, 0x39, 0x1C):
                set_key(sc, False)
            if c <= 14:
                set_key(0x3D, True); set_key(0x41, True)   # F3+F7 through the mode menus
            if c >= 26:
                break                                       # menu is up — stop and composite
            fire_int(8); begin = cur_lin()
            continue
        # phased keyboard navigation: hold mode keys -> title, then pulse fire
        # (space) title->menu->PLAY->start. Stop driving keys once the level is
        # loading (D1.PAV opened) so Bumpy stays put for a clean world frame.
        for sc in (0x3D, 0x41, 0x39, 0x1C):
            set_key(sc, False)
        if not opened(PAVNAME):
            force_level()                                      # pin current_level + skip protection
        tr["watch"] = opened(PAVNAME)
        if opened(PAVNAME):
            tr.setdefault("ipsamples", []).append(begin)
            if begin < 0x1000 and not tr.get("nullhit"):    # PC fell into IVT/null
                ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
                tr["nullhit"] = {
                    "cs": uc.reg_read(UC_X86_REG_CS), "ip": uc.reg_read(UC_X86_REG_IP),
                    "ss": ss, "sp": sp, "ax": uc.reg_read(UC_X86_REG_AX),
                    "stack": struct.unpack("<16H", uc.mem_read(ss * 16 + sp, 32))}
                break
        if not opened(PAVNAME):
            if c <= 14:
                set_key(0x3D, True); set_key(0x41, True)       # F3+F7: through both mode menus
            elif c >= 16 and (c // 2) % 2 == 0:
                set_key(0x39, True)                            # pulse fire (space)
        # once the full level (PAV+DEC+BUM) is loaded, run a few more frames then stop
        if countdown is None and opened(BUMNAME):
            countdown = int(os.environ.get("DOSEMU_SETTLE", "60"))   # frames to settle before capture
            print("level loaded (%s) at chunk %d — drawing world frames" % (BUMNAME, c))
        if opened(BUMNAME):
            dump_fresh_bum()                                   # capture decoded .BUM (guarded, once)
        # dismiss the level-intro screen with a brief fire pulse, then leave input
        # alone so the first full playfield draw settles (before much motion).
        if countdown is not None and countdown > 48 and (c // 2) % 2 == 0:
            set_key(0x39, True)                                # pulse fire (space)
        if countdown is not None:
            countdown -= 1
            if countdown <= 0:
                break
        fire_int(8)                                            # timer tick -> music/frame timing
        begin = cur_lin()
    print("ran %d chunks (%d Minstr), last mode=%s" % (
        total // CHUNK, total // 1_000_000, hex(tr["mode"]) if tr["mode"] is not None else None))

    fb = bytes(uc.mem_read(0xA0000, 0x10000))
    os.makedirs(OUT_DIR, exist_ok=True)
    open(os.path.join(OUT_DIR, "dosemu_vram.bin"), "wb").write(fb)
    open(os.path.join(OUT_DIR, "dosemu_ram.bin"), "wb").write(bytes(uc.mem_read(0x10000, 0x90000)))  # heap dump (base 0x10000)
    if tr.get("blits"):                                       # blit_sprite (x,y,frame) trace
        import json as _j
        _j.dump(tr["blits"], open(os.path.join(OUT_DIR, "blits.json"), "w"))
        print("captured %d blit_sprite calls -> build/render/blits.json" % len(tr["blits"]))
    # find where content landed: non-zero density per 8 KB across low memory
    dense = []
    for lin in range(0x1C000, 0xB0000, 0x2000):
        chunk = bytes(uc.mem_read(lin, 0x2000))
        nz = sum(1 for b in chunk if b)
        if nz > 400:
            dense.append((lin, nz))
    print("non-zero regions (8KB, >400 nz):", ["%#07x:%d" % (a, n) for a, n in dense[:24]])
    print("file ops (last 16):")
    for op in tr.get("fileops", [])[-16:]:
        print("  ", op)
    print("level files loaded: PAV=%s DEC=%s BUM=%s" % (
        opened("D1.PAV"), opened("D1.DEC"), opened("D1.BUM")))
    print("op4 handler calls=%d  vec_read_record calls=%d  op12 calls=%d" % (
        tr.get("op4", 0), tr.get("vrr", 0), tr.get("op12", 0)))
    print("vec_run entries (%d):" % len(tr.get("vecrun", [])))
    for i, r in enumerate(tr.get("vecrun", [])):
        stream = (r["DI"], r["SI"]); readcount = (r["BX"] << 16) | r["AX"]
        declared_len = (r["DX"] << 16) | r["CX"]
        print("   #%d stream=%04x:%04x read=%d declared_len=%d (CX:DX=%04x:%04x)" % (
            i, stream[0], stream[1], readcount, declared_len, r["CX"], r["DX"]))
    for i, e in enumerate(tr.get("op12_log", [])):
        print("   op12[%d] end=%04x:%04x src=%04x:%04x opcode=%#x" % (
            i, e["end"][0], e["end"][1], e["src"][0], e["src"][1], e["opcode"]))
    top_w = sorted(whist.items(), key=lambda kv: -kv[1])[:12]
    print("hottest write pages (post-PAV, 2KB):",
          ["%#07x=%d" % (p, n) for p, n in top_w])
    plot = tr.get("plot", {})
    if plot:
        lo, hi = min(plot), max(plot)
        print("op12 plot: %d points, dest linear range %#x..%#x (span %d)" % (
            len(plot), lo, hi, hi - lo + 1))
        with open(os.path.join(OUT_DIR, "op12_plot.bin"), "wb") as f:
            for lin in sorted(plot):
                f.write(struct.pack("<IB", lin, plot[lin]))
        print("op12 plot points -> build/render/op12_plot.bin (uint32 linear, uint8 value)")
    trace = tr.get("trace", [])
    if trace:
        with open(os.path.join(OUT_DIR, "op12_trace.bin"), "wb") as f:
            for row in trace:
                f.write(struct.pack("<13H", *row))
        print("op12 trace -> build/render/op12_trace.bin (%d steps, 13 u16/step)" % len(trace))
    tlmap = tr.get("tlmap")
    if tlmap:
        print("per top-level op12 call 0x899-write counts (renderer = the big one):",
              {tc: tlmap[tc] for tc in sorted(tlmap)[:20]})
    if tr.get("seq"):
        seq = tr["seq"]
        with open(os.path.join(OUT_DIR, "op12_call_seq.bin"), "wb") as f:
            for dst, isc, src, val in seq:
                f.write(struct.pack("<IBIB", dst, isc, src, val))
        print("\ncall-seq: %d ops (copy+fill) -> build/render/op12_call_seq.bin" % len(seq))
        mr = tr.get("maskreads", [])
        if mr:
            with open(os.path.join(OUT_DIR, "op12_maskreads.bin"), "wb") as f:
                for lin, b in mr:
                    f.write(struct.pack("<I", lin) + b)
            print("mask reloads: %d (first @%#x, contig=%s) -> op12_maskreads.bin" % (
                len(mr), mr[0][0],
                all(mr[i][0] - mr[i - 1][0] == 4 for i in range(1, len(mr)))))
    if tr.get("op12calls"):
        print("\n=== op12 per-call trace (first frame) ===")
        print("call ES    out_len  pixel_start  mask_start  dst_start  (last_dst)")
        for e in tr["op12calls"]:
            print("%-4d %04x  %5d    %#08x   %#08x  %#08x  %#08x" % (
                e["call"], e["es"], e.get("out_len", 0),
                e.get("first_src", 0), e.get("mask_start", 0),
                e.get("first_dst", 0), e.get("last_dst", 0)))
    if tr.get("op4calls"):
        print("\n=== op4 decompress calls (file-load map) ===")
        for e in tr["op4calls"]:
            print("  dest=%#08x  declared_len=%#x  after-open=%s" % (
                e["dest"], e["declared_len"], e["lastfile"]))
    print("open_resource index sequence:", tr.get("openres", []))
    el = tr.get("errloc")
    if el:
        print("ERR LOC: cs:ip=%04x:%04x (ghidra %04x:%04x) lin=%#x code=%s" % (
            el["cs"], el["ip"], el["ghidra"][0], el["ghidra"][1], el["lin"], el["code"]))
        print("  ss:sp=%04x:%04x stack:" % (el["ss"], el["sp"]),
              " ".join("%04x" % w for w in el["stack"]))
    nh = tr.get("nullhit")
    if nh:
        print("NULL JUMP: cs:ip=%04x:%04x ss:sp=%04x:%04x ax=%04x" % (
            nh["cs"], nh["ip"], nh["ss"], nh["sp"], nh["ax"]))
        print("  stack (ret-addr trail):",
              " ".join("%04x" % w for w in nh["stack"]))
    # IP sampling after PAV opens: cluster by 256-byte region -> where it spins.
    from collections import Counter
    ipc = Counter((s & 0xFFF00) for s in tr.get("ipsamples", []))
    print("post-PAV IP regions (lin&~0xff -> ghidra seg, count):",
          ["%#07x(g%04x)=%d" % (r, ((r >> 4) - 0x110 + 0x1000) & 0xFFFF, n)
           for r, n in ipc.most_common(10)])
    nc = sorted(tr.get("norm_callers", {}).items(), key=lambda kv: -kv[1])[:8]
    # caller CS:IP -> Ghidra address = (CS - 0x110 + 0x1000):IP
    print("normalizer callers (dosemu CS:IP -> ghidra seg):",
          ["%04x:%04x(g%04x)=%d" % (cs, ip, (cs - 0x110 + 0x1000) & 0xFFFF, n) for (cs, ip), n in nc])
    print("instr=%d exit=%s fault=%s err=%s mode=%s vram_nonzero=%d" % (
        total, tr.get("exit"), tr.get("fault"), err,
        hex(tr["mode"]) if tr["mode"] is not None else None,
        sum(1 for b in fb if b)))
    # composite the 4 VGA planes -> 320x200 RGB using the captured DAC palette
    def expand6(v: int) -> int:
        return ((v & 0x3F) << 2) | ((v & 0x3F) >> 4)
    # Full 256-entry DAC table; pixel value goes through the Attribute Controller
    # palette (attr[]) before indexing the DAC (16-colour VGA pipeline).
    pal = [(expand6(dac[i][0]), expand6(dac[i][1]), expand6(dac[i][2])) for i in range(256)]
    for page in (0x0000, 0x2000):
        rgb = bytearray(320 * 200 * 3)
        for y in range(200):
            for x in range(320):
                o = page + y * 40 + (x >> 3); bit = 7 - (x & 7)
                c = sum(((plane[p][o] >> bit) & 1) << p for p in range(4))
                r, g, b = pal[attr[c] & 0x3F]          # pixel -> attr palette -> DAC
                px = (y * 320 + x) * 3
                rgb[px] = r; rgb[px + 1] = g; rgb[px + 2] = b
        write_png_local(os.path.join(OUT_DIR, "dosemu_vga_p%04x.png" % page), 320, 200, rgb)
        if page == 0x0000:                          # 3x nearest-neighbour zoom for inspection
            S = 3; big = bytearray(320 * S * 200 * S * 3)
            for y in range(200 * S):
                row = (y // S) * 320
                for x in range(320 * S):
                    sp = (row + x // S) * 3; dp = (y * 320 * S + x) * 3
                    big[dp:dp + 3] = rgb[sp:sp + 3]
            write_png_local(os.path.join(OUT_DIR, "dosemu_vga_3x.png"), 320 * S, 200 * S, big)
    # Histogram of pixel VALUES (0-15) on the displayed page — tells whether the
    # black holes are index 0 (genuine bg) or indices 8-15 (missing palette load).
    from collections import Counter as _Ctr
    pvh = _Ctr()
    for y in range(200):
        for x in range(320):
            o = y * 40 + (x >> 3); bit = 7 - (x & 7)
            pvh[sum(((plane[p][o] >> bit) & 1) << p for p in range(4))] += 1
    print("pixel-value histogram (idx:count):",
          {i: pvh.get(i, 0) for i in range(16)})
    print("attr palette[0..15]:", list(attr[:16]))
    print("DAC entries the game wrote:", sorted("%#x" % i for i in dac_written))
    print("DAC via attr map (logical 0..15):", [tuple(dac[attr[i]]) for i in range(16)])
    # Per-world settled gameplay palette (16 logical entries, 6-bit RGB) — each world has
    # its own colour theme; render_levels loads these. Saved next to the captured .BUM.
    _wn = os.environ.get("DOSEMU_LEVEL", "1")
    _pal = [list(dac[attr[i] & 0x3F]) for i in range(16)]
    os.makedirs(os.path.join(OUT_DIR, "bum"), exist_ok=True)
    with open(os.path.join(OUT_DIR, "bum", "world%s.pal.json" % _wn), "w") as _f:
        import json as _pj
        _pj.dump(_pal, _f)
    print("wrote per-world palette -> build/render/bum/world%s.pal.json" % _wn)
    if tr.get("vecrun"):                                      # vec_run-entry register sets (debug)
        import json as _vj
        _vj.dump(tr["vecrun"], open(os.path.join(OUT_DIR, "vecrun_w%s.json" % _wn), "w"))
    # Final (settle-end) display palette — overwrites the early-frame palette captured
    # at seed time, so render_levels.py colours the geometry with the real palette.
    if tr.get("seeded"):
        with open(os.path.join(OUT_DIR, "op12_seed_pal.json"), "w") as f:
            import json as _j
            _j.dump([list(dac[attr[i] & 0x3F]) for i in range(16)], f)
    print("wrote build/render/dosemu_vga_p*.png")
    print("INT:", ", ".join("%02xh/ah%02x=%d" % (a, b, n) for (a, b), n in tr["ints"].most_common(14)))
    print("hooked vectors (AH=25):", ["int%02x->%04x:%04x" % v for v in tr.get("vectors", [])])
    ivt9 = struct.unpack("<HH", uc.mem_read(9 * 4, 4))
    print("IVT[9] = %04x:%04x" % (ivt9[1], ivt9[0]))
    dg = (0x103b + base) & 0xFFFF                              # DGROUP segment in dosemu
    mbase = struct.unpack("<H", uc.mem_read(dg * 16 + 0x4d42, 2))[0]
    mat = bytes(uc.mem_read(dg * 16 + mbase, 0x60))
    print("matrix base=[0x4d42]=%#x  bytes[0x38..0x44]=%s" % (mbase, mat[0x38:0x45].hex()))
    # decode the runtime keyboard binding list (channel 0) to find the fire (0x10) key
    p0 = struct.unpack("<HH", uc.mem_read(dg * 16 + 0x4CF2, 4))
    print("binding ch0 ptr = %04x:%04x" % (p0[1], p0[0]))
    if p0[1]:
        bl = bytes(uc.mem_read(p0[1] * 16 + p0[0], 80))
        print("binding bytes:", bl.hex())


if __name__ == "__main__":
    main()
