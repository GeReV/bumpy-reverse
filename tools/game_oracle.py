#!/usr/bin/env python3
"""game_oracle.py — Scripted-input end-to-end replay harness: capture golden trace.

Boots the real BUMPY.EXE under Unicorn (reusing the same boot/hook scaffold as
sprite_oracle.py — deliberately NOT refactoring that file), drives it to world-1
level-1, then plays back a FIXED scripted key stream over ~100 int8 ticks.

Per tick the harness captures:
  - 4 VGA planes (4 × 0x4000 bytes from the visible page in segment A000h)
  - Named-state values: p1_pixel_x, p1_pixel_y, p1_move_anim, p1_cell,
    game_mode, input_state, move_locked  (from DGROUP 203b)

Output: local/build/render/slice_goldentrace.bin

Trace format (little-endian):
  +0x00   8 B  magic "GTRACE01"
  +0x08   4 B  u32 n_ticks
  +0x0C   4 B  u32 tick_size  (= PLANES_PER_TICK + NAMED_STATE_SIZE = 65536 + 10)
  per tick (tick_size bytes each):
    4 × 0x4000 B  VGA planes (plane 0..3, 0x4000 bytes each = 320×200 planar)
      plane 0 bytes 0x0000..0x3FFF  = a000:0000..a000:3FFF
      ... (only the visible page region, not the full 0x10000 per plane)
    10 B  named-state struct (all little-endian):
      u16  p1_pixel_x   (@ DGROUP:0x9290)
      u16  p1_pixel_y   (@ DGROUP:0x9292)
      u8   p1_move_anim (@ DGROUP:0x824a)
      u8   p1_cell      (@ DGROUP:0x856e)
      u8   game_mode    (@ DGROUP:0x792c)
      u8   input_state  (@ DGROUP:0x8244)
      u8   move_locked  (@ DGROUP:0x8242)
      u8   _pad         (alignment padding, always 0)

Scripted input (TICK_SCRIPT constant, ticks 0-indexed):
  Ticks  0..9   idle      (input_state = 0x00)
  Ticks 10..39  right     (input_state = 0x08, bit 3 → p1_move_right in idle mode)
  Ticks 40..69  left      (input_state = 0x04, bit 2 → p1_move_left in idle mode)
  Ticks 70..79  jump/fire (input_state = 0x10, bit 4 → up/fire in contact resolution)
  Ticks 80..99  idle      (input_state = 0x00)

Scancode and input_state injection notes:
  Standard keyboard scancodes injected into g_key_state_table:
    right = 0x4D  (cursor-right)   → input_state bit 3 (0x08)
    left  = 0x4B  (cursor-left)    → input_state bit 2 (0x04)
    jump  = 0x48  (cursor-up)      → input_state bit 4 (0x10)

  g_key_state_table layout: DGROUP:0x4D42 holds a 2-byte near pointer mbase.
  install_keyboard_isr sets mbase = 0x4D44 (the table itself starts two bytes
  past the pointer). Writes go to: dg * 16 + mbase + (scancode & 0x7F).
  Before install_keyboard_isr has run, mbase = 0 and writes land at
  DGROUP:scancode (unrelated globals — harmless early boot noise).

  During gameplay we ALSO write input_state directly to DGROUP:0x8244 as a
  belt-and-suspenders measure. The game's poll_input (FUN_75a2 bytecode path)
  is NOT patched; the natural keyboard handler reads the key table and returns
  the mapped input value, which poll_input stores in input_state. Our direct
  write makes the value available immediately even if poll_input was missed.

Run (sandbox disabled — needs unicorn/uv cache access):
  timeout 1800 uv run python tools/game_oracle.py
"""
from __future__ import annotations
import struct
import os
import collections
from typing import Dict, List, Tuple

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_CODE,
                     UC_HOOK_MEM_UNMAPPED, UC_HOOK_INSN, UC_HOOK_MEM_WRITE,
                     UC_HOOK_MEM_READ, UcError)
from unicorn.x86_const import *

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_DIR = os.path.join(ROOT, "local/originals/old-games/bumpy")
EXE = os.path.join(ROOT, "local/originals/unpacked/BUMPY_unpacked.exe")
OUT_DIR = os.path.join(ROOT, "local/build/render")
OUT_TRACE = os.path.join(OUT_DIR, "slice_goldentrace.bin")

PSP_SEG = 0x0100
RAM = 0x110000

# DGROUP linear base — identical to sprite_oracle.py formula.
# DG_LIN = (0x103b + 0x110) * 16 = 0x114b0
DG_LIN: int = (0x103b + PSP_SEG + 0x10) * 16  # 0x114b0

# ---------------------------------------------------------------------------
# Named-state symbol addresses (DGROUP offsets from slice_syms.txt)
# ---------------------------------------------------------------------------
OFF_P1_PIXEL_X: int = 0x9290   # int16
OFF_P1_PIXEL_Y: int = 0x9292   # int16
OFF_P1_MOVE_ANIM: int = 0x824a  # byte
OFF_P1_CELL: int = 0x856e       # byte
OFF_GAME_MODE: int = 0x792c     # byte
OFF_INPUT_STATE: int = 0x8244   # byte
OFF_MOVE_LOCKED: int = 0x8242   # byte
OFF_COPYPROTECT: int = 0x119a   # byte (set to 1 to disable protection)
OFF_CURRENT_LEVEL: int = 0x79b2  # byte

# g_key_state_table pointer: DGROUP:0x4D42 holds the 2-byte near pointer mbase.
# After install_keyboard_isr runs, mbase = 0x4D44 (table bytes at DGROUP:0x4D44).
# set_key reads mbase from DGROUP:0x4D42 at call time (matching sprite_oracle.py).
OFF_KEY_STATE_PTR: int = 0x4D42   # address of the near pointer to the table

# Code addresses for reference only — not patched.
# Runtime linear = (0x1000 + base) * 16 + ghidra_offset
#   = 0x1110 * 16 + off = 0x11100 + off
FUN_75A2_LIN: int = 0x11100 + 0x75a2     # = 0x186a2  (bytecode interpreter)
POLL_INPUT_LIN: int = 0x11100 + 0x1dde   # = 0x12ede  (poll_input entry)
HANDLE_INPUT_LIN: int = 0x11100 + 0x1d26  # = 0x12e26  (handle_gameplay_input)

# ---------------------------------------------------------------------------
# Trace format constants
# ---------------------------------------------------------------------------
TRACE_MAGIC: bytes = b"GTRACE01"
PLANES_PER_TICK: int = 4 * 0x4000  # 4 planes × 16384 bytes = 65536 bytes
NAMED_STATE_SIZE: int = 10          # 2+2+1+1+1+1+1+1 = 10 bytes (includes 1 pad)
TICK_SIZE: int = PLANES_PER_TICK + NAMED_STATE_SIZE

# ---------------------------------------------------------------------------
# Scripted input stream (the CANONICAL definition — replay_check.py imports it).
# Each entry is (n_ticks, input_state_byte, right_sc, left_sc, jump_sc).
# Format: list of (tick_count, input_state) pairs (in order).
# Keep this in one place so the Task-7 reconstructed-exe replay uses the same.
# ---------------------------------------------------------------------------
# Physical scancode → input_state bit mapping (from handle_move_input decompile):
#   scancode 0x4D (cursor-right) → input_state bit 3 = 0x08
#   scancode 0x4B (cursor-left)  → input_state bit 2 = 0x04
#   scancode 0x48 (cursor-up)    → input_state bit 4 = 0x10
SC_RIGHT: int = 0x4D
SC_LEFT: int = 0x4B
SC_JUMP: int = 0x48
IS_RIGHT: int = 0x08   # input_state value for "right"
IS_LEFT: int = 0x04    # input_state value for "left"
IS_JUMP: int = 0x10    # input_state value for "jump/up"
IS_IDLE: int = 0x00    # input_state value for "idle"

# TICK_SCRIPT: list of (n_ticks, input_state_byte, scancode_or_0).
# scancode_or_0 is the single key held in g_key_state_table (0 = no key).
TICK_SCRIPT: List[Tuple[int, int, int]] = [
    (10, IS_IDLE,  0),        # ticks  0.. 9 : idle
    (30, IS_RIGHT, SC_RIGHT), # ticks 10..39 : right
    (30, IS_LEFT,  SC_LEFT),  # ticks 40..69 : left
    (10, IS_JUMP,  SC_JUMP),  # ticks 70..79 : jump/fire
    (20, IS_IDLE,  0),        # ticks 80..99 : idle
]
TOTAL_TICKS: int = sum(n for n, _, _ in TICK_SCRIPT)  # 100


def _expand_script(script: List[Tuple[int, int, int]]) -> List[Tuple[int, int]]:
    """Expand TICK_SCRIPT into a flat list of (input_state, scancode) per tick."""
    flat: List[Tuple[int, int]] = []
    for n, is_val, sc in script:
        flat.extend([(is_val, sc)] * n)
    return flat


# ---------------------------------------------------------------------------
# MZ loader (identical to sprite_oracle.py)
# ---------------------------------------------------------------------------
def load_mz(path: str) -> Tuple[bytes, list, dict]:
    x = open(path, "rb").read()
    e_crlc, e_cparhdr = struct.unpack_from("<HH", x, 6)
    e_ss, e_sp, _chk, e_ip, e_cs = struct.unpack_from("<HHHHH", x, 0x0E)
    e_lfarlc = struct.unpack_from("<H", x, 0x18)[0]
    img = x[e_cparhdr * 16:]
    relocs = [struct.unpack_from("<HH", x, e_lfarlc + i * 4) for i in range(e_crlc)]
    return img, relocs, dict(ss=e_ss, sp=e_sp, ip=e_ip, cs=e_cs)


# ---------------------------------------------------------------------------
# Files stub (identical to sprite_oracle.py)
# ---------------------------------------------------------------------------
class Files:
    def __init__(self) -> None:
        self.handles: Dict[int, object] = {}
        self.next = 5
        self.bydir = {f.upper(): os.path.join(GAME_DIR, f) for f in os.listdir(GAME_DIR)}

    def open(self, name: str) -> int:
        key = name.upper().split("\\")[-1].split("/")[-1].strip()
        if "." in key:
            b, e = key.rsplit(".", 1)
            key = b.strip() + "." + e.strip()
        path = self.bydir.get(key)
        if not path:
            return -1
        h = self.next; self.next += 1
        self.handles[h] = open(path, "rb")
        return h


# ---------------------------------------------------------------------------
# Main entry
# ---------------------------------------------------------------------------
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
    uc.mem_write(PSP_SEG * 16, b"\xCD\x20")
    uc.mem_write(PSP_SEG * 16 + 2, struct.pack("<H", 0xA000))

    files = Files()
    tr: dict = dict(instr=0, ints=collections.Counter(), last_ip=0, mode=None,
                    keys=list("\r "))
    free_top = [0x1C00]
    FREE_END = 0x9000

    def set_cf(set_it: bool) -> None:
        fl = uc.reg_read(UC_X86_REG_EFLAGS)
        uc.reg_write(UC_X86_REG_EFLAGS, (fl | 1) if set_it else (fl & ~1))

    def hook_intr(uc: Uc, intno: int, _: object) -> None:
        ax = uc.reg_read(UC_X86_REG_AX); ah = (ax >> 8) & 0xFF; al = ax & 0xFF
        tr["ints"][(intno, ah)] += 1
        if intno == 0x21:
            if ah == 0x4C:
                tr["exit"] = al; uc.emu_stop(); return
            elif ah == 0x30:
                uc.reg_write(UC_X86_REG_AX, 0x0005)
            elif ah == 0x25:
                ds = uc.reg_read(UC_X86_REG_DS); dx = uc.reg_read(UC_X86_REG_DX)
                uc.mem_write(al * 4, struct.pack("<HH", dx, ds))
            elif ah == 0x35:
                off, seg = struct.unpack("<HH", uc.mem_read(al * 4, 4))
                uc.reg_write(UC_X86_REG_BX, off); uc.reg_write(UC_X86_REG_ES, seg)
            elif ah in (0x1A, 0x2C, 0x2A, 0x30, 0x44, 0x33, 0x19, 0x0E):
                pass
            elif ah == 0x48:
                bx = uc.reg_read(UC_X86_REG_BX)
                avail = FREE_END - free_top[0]
                if bx > avail:
                    uc.reg_write(UC_X86_REG_AX, 8)
                    uc.reg_write(UC_X86_REG_BX, avail)
                    set_cf(True)
                else:
                    uc.reg_write(UC_X86_REG_AX, free_top[0])
                    free_top[0] += bx
                    set_cf(False)
            elif ah == 0x49:
                set_cf(False)
            elif ah == 0x4A:
                set_cf(False)
            elif ah == 0x3D:
                name = b""
                o = uc.reg_read(UC_X86_REG_DX)
                ds = uc.reg_read(UC_X86_REG_DS)
                while True:
                    c = uc.mem_read(ds * 16 + o, 1)[0]
                    if c == 0:
                        break
                    name += bytes([c]); o += 1
                h = files.open(name.decode("latin1"))
                tr.setdefault("fileops", []).append(("open", name.decode("latin1"), h))
                fl = uc.reg_read(UC_X86_REG_EFLAGS)
                if h < 0:
                    uc.reg_write(UC_X86_REG_AX, 2)
                    uc.reg_write(UC_X86_REG_EFLAGS, fl | 1)
                else:
                    uc.reg_write(UC_X86_REG_AX, h)
                    uc.reg_write(UC_X86_REG_EFLAGS, fl & ~1)
            elif ah == 0x3F:
                h = uc.reg_read(UC_X86_REG_BX); cx = uc.reg_read(UC_X86_REG_CX)
                ds = uc.reg_read(UC_X86_REG_DS); dx = uc.reg_read(UC_X86_REG_DX)
                f = files.handles.get(h)
                data = f.read(cx) if f else b""
                uc.mem_write(ds * 16 + dx, data)
                if len(data) < cx:
                    uc.mem_write(ds * 16 + dx + len(data), b"\x00" * (cx - len(data)))
                uc.reg_write(UC_X86_REG_AX, len(data))
            elif ah == 0x3E:
                h = uc.reg_read(UC_X86_REG_BX)
                if h in files.handles:
                    files.handles.pop(h).close()
            elif ah == 0x42:
                h = uc.reg_read(UC_X86_REG_BX); f = files.handles.get(h)
                if f:
                    off = (uc.reg_read(UC_X86_REG_CX) << 16) | uc.reg_read(UC_X86_REG_DX)
                    f.seek(off, al)
                    p = f.tell()
                    uc.reg_write(UC_X86_REG_DX, (p >> 16) & 0xFFFF)
                    uc.reg_write(UC_X86_REG_AX, p & 0xFFFF)
        elif intno == 0x10:
            if ah == 0x00:
                tr["mode"] = al
        elif intno == 0x16:
            if ah in (0x00, 0x10):
                k = ord(tr["keys"].pop(0)) if tr["keys"] else 0x0D
                uc.reg_write(UC_X86_REG_AX, k)
            elif ah in (0x01, 0x11):
                fl = uc.reg_read(UC_X86_REG_EFLAGS)
                uc.reg_write(UC_X86_REG_EFLAGS,
                             (fl & ~0x40) if tr["keys"] else (fl | 0x40))

    def hook_unmapped(uc: Uc, access: int, addr: int, size: int,
                      value: int, _: object) -> bool:
        tr["fault"] = (addr, tr["last_ip"]); uc.emu_stop(); return False

    io = [0]
    cur_scan = [0]

    def hook_in(uc: Uc, port: int, size: int, _: object) -> int:
        io[0] += 1
        if port == 0x40:
            return (io[0] * 0x11) & 0xFF
        if port == 0x201:
            return 0xF0
        if port == 0x3DA:
            attr_ff[0] = 0
            return (io[0] & 1) * 0x09
        if port == 0x60:
            return cur_scan[0]
        if port == 0x61:
            return 0xFF
        return 0xFF

    # --- minimal VGA planar emulation (mode 0xD) — copied from sprite_oracle.py ---
    seq = bytearray(256); gc = bytearray(256)
    seq_i = [0]; gc_i = [0]
    latch = [0, 0, 0, 0]
    plane = [bytearray(0x10000) for _ in range(4)]
    dac = [[0, 0, 0] for _ in range(256)]; dac_i = [0]; dac_sub = [0]
    ATTR_DEFAULT = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17]
    attr = bytearray(32)
    for _i, _a in enumerate(ATTR_DEFAULT):
        attr[_i] = _a
    attr_i = [0]; attr_ff = [0]
    crtc = bytearray(256); crtc_i = [0]

    def hook_out(uc: Uc, port: int, size: int, value: int, _: object) -> None:
        value &= 0xFFFF
        if port == 0x3C0:
            if attr_ff[0] == 0:
                attr_i[0] = value & 0x1F; attr_ff[0] = 1
            else:
                if attr_i[0] < 0x20:
                    attr[attr_i[0]] = value & 0xFF
                attr_ff[0] = 0
            return
        if port in (0x3C4, 0x3CE):
            reg = seq if port == 0x3C4 else gc
            idx = seq_i if port == 0x3C4 else gc_i
            idx[0] = value & 0xFF
            if size == 2:
                reg[value & 0xFF] = (value >> 8) & 0xFF
        elif port in (0x3C5, 0x3CF):
            reg = seq if port == 0x3C5 else gc
            idx = seq_i if port == 0x3C5 else gc_i
            reg[idx[0]] = value & 0xFF
        elif port == 0x3C8:
            dac_i[0] = value & 0xFF; dac_sub[0] = 0
        elif port == 0x3C9:
            dac[dac_i[0] & 0xFF][dac_sub[0]] = value & 0x3F
            dac_sub[0] += 1
            if dac_sub[0] == 3:
                dac_sub[0] = 0; dac_i[0] = (dac_i[0] + 1) & 0xFF
        elif port == 0x3D4:
            crtc_i[0] = value & 0xFF
            if size == 2:
                crtc[value & 0xFF] = (value >> 8) & 0xFF
        elif port == 0x3D5:
            crtc[crtc_i[0]] = value & 0xFF

    def hook_vga_write(uc: Uc, access: int, addr: int, size: int,
                       value: int, _: object) -> None:
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
                else:
                    v = ((val >> rot) | (val << (8 - rot))) & 0xFF if rot else val
                    if esr & (1 << p):
                        v = 0xFF if (sr & (1 << p)) else 0
                    if fn == 1: v &= lat
                    elif fn == 2: v |= lat
                    elif fn == 3: v ^= lat
                    res = (v & bm) | (lat & ~bm)
                plane[p][o] = res & 0xFF

    def hook_vga_read(uc: Uc, access: int, addr: int, size: int,
                      value: int, _: object) -> None:
        off = (addr - 0xA0000) & 0xFFFF
        for p in range(4):
            latch[p] = plane[p][off]
        uc.mem_write(addr, bytes([plane[gc[4] & 3][off]]))

    uc.hook_add(UC_HOOK_INTR, hook_intr)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, hook_unmapped)
    uc.hook_add(UC_HOOK_MEM_WRITE, hook_vga_write, None, 0xA0000, 0xAFFFF)
    uc.hook_add(UC_HOOK_MEM_READ, hook_vga_read, None, 0xA0000, 0xAFFFF)
    try:
        uc.hook_add(UC_HOOK_INSN, hook_in, None, 1, 0, UC_X86_INS_IN)
        uc.hook_add(UC_HOOK_INSN, hook_out, None, 1, 0, UC_X86_INS_OUT)
    except Exception:
        pass

    # --- iret stubs for uninitialised vectors (same as sprite_oracle.py) -----------
    uc.mem_write(0x500, b"\xCF")
    iret_vec = (0x0050 << 16) | 0x0000
    for v in range(0x100):
        if struct.unpack("<I", uc.mem_read(v * 4, 4))[0] == 0:
            uc.mem_write(v * 4, struct.pack("<I", iret_vec))

    # --- register setup (identical to sprite_oracle.py) ----------------------------
    uc.reg_write(UC_X86_REG_DS, PSP_SEG)
    uc.reg_write(UC_X86_REG_ES, PSP_SEG)
    uc.reg_write(UC_X86_REG_SS, (hdr["ss"] + base) & 0xFFFF)
    uc.reg_write(UC_X86_REG_SP, hdr["sp"])
    uc.reg_write(UC_X86_REG_CS, (hdr["cs"] + base) & 0xFFFF)
    uc.reg_write(UC_X86_REG_IP, hdr["ip"])

    dg = (0x103b + base) & 0xFFFF  # DGROUP segment (runtime value)

    # ---------------------------------------------------------------------------
    # fire_int — push flags/cs/ip, load new cs:ip from vector table
    # ---------------------------------------------------------------------------
    def fire_int(n: int) -> None:
        ip = uc.reg_read(UC_X86_REG_IP) & 0xFFFF
        cs = uc.reg_read(UC_X86_REG_CS) & 0xFFFF
        fl = uc.reg_read(UC_X86_REG_EFLAGS) & 0xFFFF
        ss = uc.reg_read(UC_X86_REG_SS) & 0xFFFF
        sp = uc.reg_read(UC_X86_REG_SP) & 0xFFFF
        for val in (fl, cs, ip):
            sp = (sp - 2) & 0xFFFF
            uc.mem_write(ss * 16 + sp, struct.pack("<H", val))
        uc.reg_write(UC_X86_REG_SP, sp)
        vec = struct.unpack("<I", uc.mem_read(n * 4, 4))[0]
        if vec == 0:
            return
        uc.reg_write(UC_X86_REG_CS, (vec >> 16) & 0xFFFF)
        uc.reg_write(UC_X86_REG_IP, vec & 0xFFFF)

    def cur_lin() -> int:
        return ((uc.reg_read(UC_X86_REG_CS) & 0xFFFF) * 16
                + (uc.reg_read(UC_X86_REG_IP) & 0xFFFF)) & 0xFFFFF

    def opened(name: str) -> bool:
        return any(o[0] == "open" and o[1] == name for o in tr.get("fileops", []))

    # ---------------------------------------------------------------------------
    # Key-injection helpers (mirrors sprite_oracle.py exactly)
    # ---------------------------------------------------------------------------
    def set_key(scancode: int, down: bool) -> None:
        """Write into g_key_state_table, reading mbase from DGROUP:OFF_KEY_STATE_PTR.

        Before install_keyboard_isr: mbase = 0, write lands at DGROUP:scancode.
        After install_keyboard_isr: mbase = 0x4D44, write lands at correct entry.
        Matches sprite_oracle.py set_key() exactly.
        """
        mbase = struct.unpack("<H", bytes(uc.mem_read(dg * 16 + OFF_KEY_STATE_PTR, 2)))[0]
        uc.mem_write(dg * 16 + mbase + (scancode & 0x7F),
                     bytes([scancode if down else 0]))

    def clear_all_keys() -> None:
        """Zero 0x80 bytes starting at the current key table (mbase)."""
        mbase = struct.unpack("<H", bytes(uc.mem_read(dg * 16 + OFF_KEY_STATE_PTR, 2)))[0]
        uc.mem_write(dg * 16 + mbase, bytes(0x80))

    def inject_input(is_val: int, scancode: int) -> None:
        """Inject one tick's input.

        Writes input_state directly AND sets the corresponding key in the table
        so that poll_input (via FUN_75a2 keyboard handler) also sees it.
        """
        uc.mem_write(DG_LIN + OFF_INPUT_STATE, bytes([is_val & 0xFF]))
        clear_all_keys()
        if scancode != 0:
            set_key(scancode, True)

    # ---------------------------------------------------------------------------
    # VGA plane snapshot (visible page = 320×200 EGA/VGA planar)
    # Each of the 4 planes contributes 0x4000 bytes for the 320×200 visible page.
    # The engine uses double-buffering; during gameplay the visible page is at
    # a000:0000 (CRTC start 0x0000) or a200:0000 (CRTC start 0x2000).
    # We always capture from plane[p][0:0x4000] (the a000 page base) since
    # present_frame(1) flips which page is being displayed, but our VGA model
    # captures all writes to a000..afff consistently into plane[p][addr-a000].
    # The per-plane byte at index i is for pixel column (i*8)//40 of row i//40
    # in planar EGA layout (one bit per pixel per plane, 8 pixels per byte).
    # ---------------------------------------------------------------------------
    def snap_planes() -> bytes:
        """Snapshot the visible-page portion of each plane (4 × 0x4000 bytes)."""
        return b"".join(bytes(plane[p][:0x4000]) for p in range(4))

    def read_named_state() -> bytes:
        """Read the 7 named-state fields from DGROUP and pack into 10 bytes."""
        p1x = struct.unpack("<H", bytes(uc.mem_read(DG_LIN + OFF_P1_PIXEL_X, 2)))[0]
        p1y = struct.unpack("<H", bytes(uc.mem_read(DG_LIN + OFF_P1_PIXEL_Y, 2)))[0]
        anim = uc.mem_read(DG_LIN + OFF_P1_MOVE_ANIM, 1)[0]
        cell = uc.mem_read(DG_LIN + OFF_P1_CELL, 1)[0]
        gmode = uc.mem_read(DG_LIN + OFF_GAME_MODE, 1)[0]
        istate = uc.mem_read(DG_LIN + OFF_INPUT_STATE, 1)[0]
        mlocked = uc.mem_read(DG_LIN + OFF_MOVE_LOCKED, 1)[0]
        # 2+2+1+1+1+1+1+1 = 10 bytes (last byte = padding 0)
        return struct.pack("<HHBBBBBx", p1x, p1y, anim, cell, gmode, istate, mlocked)

    # ---------------------------------------------------------------------------
    # Phase 1: Boot to level — same approach as sprite_oracle.py
    # ---------------------------------------------------------------------------
    LEVEL = 1
    PAVNAME = "D%d.PAV" % LEVEL
    BUMNAME = "D%d.BUM" % LEVEL

    def force_level() -> None:
        uc.mem_write(DG_LIN + OFF_CURRENT_LEVEL, bytes([LEVEL & 0xFF]))
        uc.mem_write(DG_LIN + OFF_COPYPROTECT, bytes([1]))

    CHUNK = 1_000_000
    total_instr = 0
    begin = cur_lin()
    err = None
    countdown = None  # ticks remaining to settle after BUM load
    SETTLE_TICKS = 80  # wait 80 int8 ticks after BUM load before capturing

    print("[game_oracle] booting BUMPY (level %d)..." % LEVEL, flush=True)

    # Boot loop: identical to sprite_oracle.py — run CHUNK instructions at a time,
    # inject menu keys, wait for BUM file to load, then start the settle countdown.
    # FUN_75a2 is NOT patched; the game's natural keyboard handler reads the key
    # state table and returns the mapped value. set_key writes to the correct table
    # entry (mbase read from DGROUP:OFF_KEY_STATE_PTR at call time).
    while total_instr < 400_000_000:
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e); tr["err"] = err; break
        total_instr += CHUNK
        if total_instr % 20_000_000 == 0:
            print("[game_oracle] %dM instr, countdown=%s" % (
                total_instr // 1_000_000, countdown), flush=True)
        if tr.get("exit") is not None or tr.get("fault"):
            break
        begin = cur_lin()
        c = total_instr // CHUNK

        # Clear temporary boot keys before re-injection (matches sprite_oracle.py)
        for sc in (0x3D, 0x41, 0x39, 0x1C):
            set_key(sc, False)

        if not opened(PAVNAME):
            force_level()
        if not opened(PAVNAME):
            # Still in menu — same key injection sequence as sprite_oracle.py:
            # early chunks: F3 (0x3D) + unknown (0x41) to skip intro
            # later chunks: Space (0x39) alternating to confirm menu selection
            if c <= 14:
                set_key(0x3D, True); set_key(0x41, True)
            elif c >= 16 and (c // 2) % 2 == 0:
                set_key(0x39, True)

        if countdown is None and opened(BUMNAME):
            countdown = SETTLE_TICKS
            print("[game_oracle] level loaded (%s) at chunk %d — settling %d ticks" % (
                BUMNAME, c, SETTLE_TICKS), flush=True)

        if countdown is not None:
            # During settle: press space to advance through level-intro screen
            if countdown > SETTLE_TICKS - 10 and (c // 2) % 2 == 0:
                set_key(0x39, True)
            countdown -= 1
            if countdown <= 0:
                break

        fire_int(8)
        begin = cur_lin()

    if tr.get("exit") is not None or tr.get("fault"):
        print("[game_oracle] ERROR: premature exit/fault during boot: exit=%s fault=%s err=%s" % (
            tr.get("exit"), tr.get("fault"), err), flush=True)
        return

    if not opened(BUMNAME):
        print("[game_oracle] ERROR: level %s never loaded after %dM instructions" % (
            BUMNAME, total_instr // 1_000_000), flush=True)
        return

    print("[game_oracle] boot complete. Files opened: %s" % (
        [o[1] for o in tr.get("fileops", [])]), flush=True)

    # ---------------------------------------------------------------------------
    # Phase 2: Scripted tick loop — capture TOTAL_TICKS frames
    # ---------------------------------------------------------------------------
    tick_flat = _expand_script(TICK_SCRIPT)
    tick_records: List[bytes] = []

    print("[game_oracle] starting scripted capture: %d ticks" % TOTAL_TICKS, flush=True)
    print("[game_oracle] script: %s" % [
        "idle×%d" % n if is_v == 0 else
        "right×%d" % n if is_v == IS_RIGHT else
        "left×%d" % n if is_v == IS_LEFT else
        "jump×%d" % n
        for n, is_v, _ in TICK_SCRIPT], flush=True)

    for tick_idx, (is_val, scancode) in enumerate(tick_flat):
        if tick_idx % 10 == 0:
            try:
                px = struct.unpack("<h", bytes(uc.mem_read(DG_LIN + OFF_P1_PIXEL_X, 2)))[0]
                py = struct.unpack("<h", bytes(uc.mem_read(DG_LIN + OFF_P1_PIXEL_Y, 2)))[0]
                gm = uc.mem_read(DG_LIN + OFF_GAME_MODE, 1)[0]
                cell = uc.mem_read(DG_LIN + OFF_P1_CELL, 1)[0]
            except UcError:
                px, py, gm, cell = 0, 0, 0, 0
            print("[game_oracle] tick %3d/%d  p1_pixel=(%d,%d) game_mode=%#x cell=%d" % (
                tick_idx, TOTAL_TICKS, px, py, gm, cell), flush=True)

        # Inject this tick's input before running the game
        inject_input(is_val, scancode)

        # Run one CHUNK of instructions (the game processes one tick)
        begin = cur_lin()
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e); tr["err"] = err; break
        if tr.get("exit") is not None or tr.get("fault"):
            print("[game_oracle] ERROR at tick %d: exit=%s fault=%s err=%s" % (
                tick_idx, tr.get("exit"), tr.get("fault"), err), flush=True)
            break

        # Re-inject input_state in case poll_input or other code cleared it
        uc.mem_write(DG_LIN + OFF_INPUT_STATE, bytes([is_val & 0xFF]))

        # Capture: planes + named state
        planes_snap = snap_planes()
        state_snap = read_named_state()
        tick_records.append(planes_snap + state_snap)

        # Fire INT 8 to advance the timer tick (unblocks rotate_timing_flags_and_wait)
        fire_int(8)
        begin = cur_lin()

    actual_ticks = len(tick_records)
    print("[game_oracle] captured %d ticks" % actual_ticks, flush=True)

    # ---------------------------------------------------------------------------
    # Print player position summary (sanity check: must change during right/left)
    # ---------------------------------------------------------------------------
    print("\n[game_oracle] Player x/y trajectory (every 5 ticks):", flush=True)
    print("  tick  p1_pixel_x  p1_pixel_y  input_state  game_mode", flush=True)
    for i, rec in enumerate(tick_records):
        if i % 5 == 0 or i < 5:
            # named-state starts at PLANES_PER_TICK offset
            ns = rec[PLANES_PER_TICK:]
            px, py = struct.unpack_from("<hh", ns, 0)
            anim = ns[4]; cell = ns[5]; gm = ns[6]; istate = ns[7]; mlocked = ns[8]
            print("  %4d  %10d  %10d  %#12x  %#10x" % (i, px, py, istate, gm),
                  flush=True)

    # Verify movement: p1_pixel_x should differ between tick 0 and tick 39
    if actual_ticks >= 40:
        ns0 = tick_records[0][PLANES_PER_TICK:]
        ns39 = tick_records[39][PLANES_PER_TICK:]
        x0, y0 = struct.unpack_from("<hh", ns0, 0)
        x39, y39 = struct.unpack_from("<hh", ns39, 0)
        if x0 != x39 or y0 != y39:
            print("[game_oracle] MOVEMENT CONFIRMED: p1 pos changed from (%d,%d) at tick 0 "
                  "to (%d,%d) at tick 39" % (x0, y0, x39, y39), flush=True)
        else:
            print("[game_oracle] WARNING: p1 position did NOT change (x=%d y=%d) — "
                  "input injection may not be working" % (x0, y0), flush=True)

    # ---------------------------------------------------------------------------
    # Write golden trace
    # ---------------------------------------------------------------------------
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT_TRACE, "wb") as f:
        f.write(TRACE_MAGIC)
        f.write(struct.pack("<II", actual_ticks, TICK_SIZE))
        for rec in tick_records:
            # Pad or truncate each record to exactly TICK_SIZE bytes
            if len(rec) < TICK_SIZE:
                rec = rec + bytes(TICK_SIZE - len(rec))
            f.write(rec[:TICK_SIZE])

    trace_bytes = os.path.getsize(OUT_TRACE)
    print("[game_oracle] wrote %s (%d ticks, %d B/tick, total %d B)" % (
        OUT_TRACE, actual_ticks, TICK_SIZE, trace_bytes), flush=True)
    print("[game_oracle] scancode mapping used:", flush=True)
    print("  right = SC 0x%02X → input_state 0x%02X (bit 3)" % (SC_RIGHT, IS_RIGHT),
          flush=True)
    print("  left  = SC 0x%02X → input_state 0x%02X (bit 2)" % (SC_LEFT,  IS_LEFT),
          flush=True)
    print("  jump  = SC 0x%02X → input_state 0x%02X (bit 4)" % (SC_JUMP,  IS_JUMP),
          flush=True)

    if err:
        print("[game_oracle] emu error:", err, flush=True)
    if tr.get("fault"):
        print("[game_oracle] fault at addr=%#x last_ip=%#x" % tr["fault"], flush=True)


if __name__ == "__main__":
    main()
