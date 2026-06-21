#!/usr/bin/env python3
"""copyprot_oracle.py — Phase-7b capture oracle for the copy-protection challenge
`copyprotect_challenge` (Ghidra 1000:4015).  CAPTURE-AS-DISCOVERY harness.

Boots the real (unpacked) BUMPY.EXE under Unicorn — REUSING (not refactoring) the
boot + DGROUP-read + INT-21h file-I/O + VGA-planar + FUN_1000_75a2 scripted-input
scaffold of tools/screens_oracle.py / tools/game_oracle.py — then drives a single
synchronous near-call of `copyprotect_challenge` per scenario with a scripted
`+`/`-`/ENTER input stream, capturing the routine's PRESENT-side state so a host
replay harness can validate the reconstruction.

THE ROUTINE (1000:4015, confirmed via the live Ghidra decomp/asm).  The challenge is
CRACKED: it writes `copyprotect_flag = 1` UNCONDITIONALLY at 1000:412e (decomp comment
"COPY PROTECTION DEFEATED HERE"), BEFORE any input is read, and never compares the
entered number against the answer table.  Captured PRESENT parts:

  * Two fmemcpy (1000:a9f5) table copies into stack locals:
      - sprite_id_tbl : DS:0x11b6 -> SS:[BP-0x26], 16 WORDS  (CX=0x20)
      - answer_tbl    : DS:0x11d6 -> SS:[BP-0x36], 16 BYTES  (CX=0x10)
    Recovered values (cross-checked against the loaded image):
      positions: 00 00 46 4f 51 93 9f a8 b4 b3 be c4 ca cb da e1
      answers  : 0 0 4 6 7 5 15 16 24 19 28 26 25 27 17 18
    We capture them as the engine copies them (read SS frame at each fmemcpy return).

  * prng seed: prng_seed_thunk(DAT_203b_119c) at 1000:4065 seeds prng_state0 from
    DGROUP 0x119c (=0x1e61 in the image) and zeroes state1/state2 — so the rand()
    index draws are DETERMINISTIC and the host (src/prng.c) can reproduce them by
    seeding prng_state0=seed.  We capture the seed AND the full 3-word prng state
    snapshot right after the seed call.

  * Random index: `do { i = rand() & 0xf; } while (i < 2);` (rand=1000:93b1, the
    reject loop at 1000:4113..411e).  Index lands in 2..15.  We capture every rand()
    return + the masked value + which draw was ACCEPTED (the first >= 2).

  * Challenge-screen resource load (res 0x90 via set_resource_table/open_resource/
    read_chunked into level_dec_buf @ DGROUP 0x6be8:0x6bea).  We STUB the load (no
    INT 21h): hook open_resource (1000:736f) -> return a sentinel handle and
    read_chunked (1000:745e) -> no-op, after SEEDING level_dec_buf with a known 99-byte
    pattern.  (The challenge present path does not read the loaded bytes for the values
    we validate; the copies the routine makes from DS:0x65a / DS:0x73e into the level
    structs are captured separately via the post-load global snapshot.)

  * Platform-sprite display descriptor: p1_sprite (far ptr @ DGROUP 0x8884 -> the
    0x792e descriptor): word[0]=x=0x90, word[1]=y=100(0x64), word[2]=frame=
    sprite_id_tbl[i].  Captured at the blit_sprite (1000:942a) call boundary.

  * INPUT DIAL state machine (the while(entry_done==0) loop, 1000:4199..420e):
      input_state &0x10 (fire)  -> entry_done=1 (confirm/exit)
      input_state &0x04 (minus) -> if entered_number>0: entered_number--   [draw 0x134b]
      input_state &0x08 (plus)  -> if entered_number<=0x62: entered_number++ [draw 0x1350]
    entered_number is SS:[BP-0x2].  We drive several scripted +/-/ENTER sequences via
    the FUN_1000_75a2 return hook (action bytes 0x04/0x08/0x10) and capture the
    entered_number trajectory (one sample per poll_input boundary) + the confirm.

  * copyprotect_flag (DGROUP 0x119a) at exit (cracked => 1).

WHY A SINGLE call_near IS ENOUGH.  copyprotect_challenge is a self-contained near
(cdecl16near, DS=DGROUP) routine.  We boot to a settled engine state, snapshot it, then
per scenario restore + seed the scripted dial input + call the routine once.  The loop
body sets round_state=0xff after the FIRST confirm (0xff > 2 -> return), so the routine
runs exactly ONE round per call; we therefore exercise the dial with one scripted
sequence per scenario.

Outputs (BOTH gitignored — discovery; only this script is committed):
  local/build/render/copyprot_trace.bin   (frozen layout — see TRACE LAYOUT below)
  local/build/copyprot_model.md           (resolved addrs + tables + dial captures)

TRACE LAYOUT (little-endian) — FROZEN; a later task parses this exactly:
  Header:
    +0x00  8 B   magic   b"CPTRC01\0"
    +0x08  2 B   u16     version (=1)
    +0x0A  2 B   u16     n_scenarios
  Shared (captured once; identical every scenario):
    +..    u16   prng_seed                 (DGROUP 0x119c at the seed call)
    +..    3xu16 prng_state0/1/2           (snapshot right after prng_seed_thunk)
    +..    u8    n_pos, then n_pos u16      (sprite_id_tbl as the engine copied it)
    +..    u8    n_ans, then n_ans u8       (answer_tbl as the engine copied it)
  Then, per scenario:
    u8        scenario_id
    u8        name_len, name_len bytes (ascii)
    u8        n_script, n_script bytes      (the high-level action stream fed: each is
                                            0x04 minus / 0x08 plus / 0x10 fire)
    u8        n_draws, then per draw:
                  u16 rand_ret, u8 masked, u8 accepted(1/0)
    u8        accepted_index               (final index in 2..15)
    u8        expected_answer              (answer_tbl[index])
    u8        n_traj, then n_traj u8        (entered_number sampled at each poll boundary)
    u8        entered_number_final
    u8        entry_done_final
    u8        copyprotect_flag_exit
    u8        desc_x_lo,x_hi (u16 x), u16 y, u16 frame   (p1_sprite descriptor at blit)
    u8        n_descbytes, then n_descbytes bytes        (raw 0x792e descriptor @ blit)

Run (sandbox disabled — needs unicorn/uv cache access), HARD timeout:
  timeout 1800 uv run python tools/copyprot_oracle.py
"""
from __future__ import annotations
import struct
import os
import collections
import time
from typing import Dict, List, Optional, Tuple

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
OUT_TRACE = os.path.join(OUT_DIR, "copyprot_trace.bin")
OUT_MODEL = os.path.join(ROOT, "local/build/copyprot_model.md")

PSP_SEG = 0x0100
RAM = 0x110000

# DGROUP runtime base — identical formula to screens_oracle.py / sound_oracle.py.
DG_LIN: int = (0x103b + PSP_SEG + 0x10) * 16   # 0x114b0
CODE_LIN: int = (PSP_SEG + 0x10) * 16          # 0x1100

# ---------------------------------------------------------------------------
# Code offsets (Ghidra seg-1000) — resolved from the live disassembly of
# copyprotect_challenge (1000:4015) and its callees.
# ---------------------------------------------------------------------------
OFF_CHALLENGE: int = 0x4015        # copyprotect_challenge entry
OFF_FMEMCPY: int = 0xa9f5          # fmemcpy(dst_seg,dst_off,src_seg,src_off) CX=len
OFF_RAND: int = 0x93b1             # rand()
OFF_PRNG_SEED_THUNK: int = 0x93a4  # prng_seed_thunk(seed) -> prng_seed
OFF_OPEN_RESOURCE: int = 0x736f    # open_resource() (INT 21h c_open) — STUBBED
OFF_READ_CHUNKED: int = 0x745e     # read_chunked(...) — STUBBED
OFF_BLIT_SPRITE: int = 0x942a      # blit_sprite(seg,off=0x792e)
OFF_POLL_INPUT: int = 0x1dde       # poll_input() -> read_input_action; sets input_state
OFF_FUN_75A2: int = 0x75a2         # read_input_action (the input primitive driver hook)

# ---------------------------------------------------------------------------
# DGROUP global offsets.
# ---------------------------------------------------------------------------
OFF_PRNG_SEED_SRC: int = 0x119c    # u16 (DAT_203b_119c — prng seed source)
OFF_PRNG_STATE0: int = 0x5676      # u16
OFF_PRNG_STATE1: int = 0x5678      # u16
OFF_PRNG_STATE2: int = 0x567a      # u16
OFF_PALETTE_MODE: int = 0x541d     # u16
OFF_INPUT_STATE: int = 0x8244      # u8
OFF_COPYPROTECT: int = 0x119a      # s8 (copyprotect_flag) — cracked => 1
OFF_CURRENT_LEVEL: int = 0x79b2    # u8
OFF_LEVEL_DEC_BUF: int = 0x6be8    # u16 (level_dec_buf off)
OFF_LEVEL_DEC_SEG: int = 0x6bea    # u16 (level_dec_seg)
OFF_P1_SPRITE_PTR: int = 0x8884    # far ptr -> 0x792e blit descriptor
OFF_KEY_STATE_PTR: int = 0x4D42    # near ptr to g_key_state_table base

# DS-relative source offsets of the two tables (the fmemcpy `src_off` operands).
SRC_POSITIONS: int = 0x11b6        # 16 words
SRC_ANSWERS: int = 0x11d6          # 16 bytes
P1_SPRITE_DESC_LEN: int = 0x0A     # x,y,frame,src-far descriptor bytes

# Dial input action bytes (the input_state bits the dial loop tests).
ACT_MINUS: int = 0x04
ACT_PLUS: int = 0x08
ACT_FIRE: int = 0x10

# ---------------------------------------------------------------------------
# Trace format constants
# ---------------------------------------------------------------------------
TRACE_MAGIC: bytes = b"CPTRC01\x00"
TRACE_VERSION: int = 1


def load_mz(path: str) -> Tuple[bytes, list, dict]:
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
# Scenarios — each is (id, name, [action bytes]).  The action list is the
# high-level dial stream; a terminating ACT_FIRE is appended automatically so the
# while(entry_done==0) loop always confirms and the routine returns.
# entered_number starts at 0; minus is clamped at 0 (no decrement when already 0),
# plus is clamped at 0x62 (no increment past 0x62).
# ---------------------------------------------------------------------------
SCENARIOS: List[Tuple[int, str, List[int]]] = [
    # 1: pure plus ramp — 5 increments then confirm. entered_number 0->5.
    (1, "plus5", [ACT_PLUS] * 5),
    # 2: plus then minus — up 3, down 1, confirm. 0->3->2.
    (2, "plus3_minus1", [ACT_PLUS, ACT_PLUS, ACT_PLUS, ACT_MINUS]),
    # 3: minus at floor — minus first (clamped at 0), then two plus. stays 0 then ->2.
    (3, "minus_floor_plus2", [ACT_MINUS, ACT_MINUS, ACT_PLUS, ACT_PLUS]),
    # 4: immediate confirm — no +/-, entered_number stays 0.
    (4, "confirm_zero", []),
    # 5: long ramp toward the 0x62 ceiling — many plus (well below ceiling here).
    (5, "plus10", [ACT_PLUS] * 10),
]


def main() -> None:
    t_start = time.time()
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
    tr: dict = dict(instr=0, ints=collections.Counter(), last_ip=0, mode=None)
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
                uc.reg_write(UC_X86_REG_AX, 0x0D)
            elif ah in (0x01, 0x11):
                fl = uc.reg_read(UC_X86_REG_EFLAGS)
                uc.reg_write(UC_X86_REG_EFLAGS, fl | 0x40)

    def hook_unmapped(uc: Uc, access: int, addr: int, size: int,
                      value: int, _: object) -> bool:
        tr["fault"] = (addr, tr["last_ip"]); uc.emu_stop(); return False

    io = [0]
    cur_scan = [0]

    def hook_in(uc: Uc, port: int, size: int, _: object) -> int:
        io[0] += 1
        if port == 0x40:
            val = (io[0] * 0x11) & 0xFF
        elif port == 0x201:
            val = 0xF0
        elif port == 0x3DA:
            attr_ff[0] = 0
            val = (io[0] & 1) * 0x09
        elif port == 0x60:
            val = cur_scan[0]
        elif port == 0x61:
            val = 0xFF
        elif port == 0x3C7:
            val = 0x00
        else:
            val = 0xFF
        return val

    # --- minimal VGA planar emulation (copied from screens_oracle.py) ----------------
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
    uc.hook_add(UC_HOOK_INSN, hook_in, None, 1, 0, UC_X86_INS_IN)
    uc.hook_add(UC_HOOK_INSN, hook_out, None, 1, 0, UC_X86_INS_OUT)

    # --- iret stubs for uninitialised vectors --------------------------------------
    uc.mem_write(0x500, b"\xCF")
    iret_vec = (0x0050 << 16) | 0x0000
    for v in range(0x100):
        if struct.unpack("<I", uc.mem_read(v * 4, 4))[0] == 0:
            uc.mem_write(v * 4, struct.pack("<I", iret_vec))

    uc.reg_write(UC_X86_REG_DS, PSP_SEG)
    uc.reg_write(UC_X86_REG_ES, PSP_SEG)
    uc.reg_write(UC_X86_REG_SS, (hdr["ss"] + base) & 0xFFFF)
    uc.reg_write(UC_X86_REG_SP, hdr["sp"])
    uc.reg_write(UC_X86_REG_CS, (hdr["cs"] + base) & 0xFFFF)
    uc.reg_write(UC_X86_REG_IP, hdr["ip"])

    dg = (0x103b + base) & 0xFFFF
    DS_SCREEN = dg

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

    def set_key(scancode: int, down: bool) -> None:
        mbase = struct.unpack("<H", bytes(uc.mem_read(dg * 16 + OFF_KEY_STATE_PTR, 2)))[0]
        uc.mem_write(dg * 16 + mbase + (scancode & 0x7F),
                     bytes([scancode if down else 0]))

    def clear_all_keys() -> None:
        mbase = struct.unpack("<H", bytes(uc.mem_read(dg * 16 + OFF_KEY_STATE_PTR, 2)))[0]
        uc.mem_write(dg * 16 + mbase, bytes(0x80))

    # ---------------------------------------------------------------------------
    # DGROUP read helpers
    # ---------------------------------------------------------------------------
    def rd8(off: int) -> int:
        return uc.mem_read(DG_LIN + off, 1)[0]

    def rd_u16(off: int) -> int:
        return struct.unpack("<H", bytes(uc.mem_read(DG_LIN + off, 2)))[0]

    def read_far_target(off: int, n: int) -> bytes:
        o, s = struct.unpack("<HH", bytes(uc.mem_read(DG_LIN + off, 4)))
        lin = (s * 16 + o) & 0xFFFFF
        try:
            return bytes(uc.mem_read(lin, n))
        except UcError:
            return b""

    # ---------------------------------------------------------------------------
    # FUN_1000_75a2 scripted-input driver (the screens_oracle.py mechanism).  The
    # dial reads input through poll_input -> read_input_action (FUN_75a2); we overwrite
    # FUN_75a2's AL return with the next scripted action byte, then 0 when exhausted.
    # ---------------------------------------------------------------------------
    input_driver = {"queue": [], "active": False}
    in75_exit_lins: set = set()
    in75_pending: dict = {}

    def hook_75a2_entry(uc2: Uc, addr: int, size: int, _: object) -> None:
        if not input_driver["active"]:
            return
        ss = uc2.reg_read(UC_X86_REG_SS); sp = uc2.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc2.mem_read(ss * 16 + sp, 2)))[0]
        ret_lin = (CODE_LIN + ret_off) & 0xFFFFF
        in75_pending[ret_lin] = in75_pending.get(ret_lin, 0) + 1
        if ret_lin not in in75_exit_lins:
            in75_exit_lins.add(ret_lin)
            uc2.hook_add(UC_HOOK_CODE, hook_75a2_exit, None, ret_lin, ret_lin)

    def hook_75a2_exit(uc2: Uc, addr: int, size: int, _: object) -> None:
        if in75_pending.get(addr, 0) <= 0:
            return
        in75_pending[addr] -= 1
        q = input_driver["queue"]
        nxt = q.pop(0) if q else 0
        ax = uc2.reg_read(UC_X86_REG_AX) & 0xFF00
        uc2.reg_write(UC_X86_REG_AX, ax | (nxt & 0xFF))

    uc.hook_add(UC_HOOK_CODE, hook_75a2_entry, None,
                CODE_LIN + OFF_FUN_75A2, CODE_LIN + OFF_FUN_75A2)

    # ---------------------------------------------------------------------------
    # Boot to a settled engine state (identical approach to screens_oracle.py).
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
    countdown = None
    SETTLE_TICKS = 80

    print("[copyprot_oracle] booting BUMPY (level %d)..." % LEVEL, flush=True)

    while total_instr < 400_000_000:
        if time.time() - t_start > 1500:
            print("[copyprot_oracle] WALL-CLOCK GUARD tripped during boot", flush=True)
            return
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e); tr["err"] = err; break
        total_instr += CHUNK
        if total_instr % 40_000_000 == 0:
            print("[copyprot_oracle] heartbeat: %dM instr, countdown=%s, %.0fs" % (
                total_instr // 1_000_000, countdown, time.time() - t_start), flush=True)
        if tr.get("exit") is not None or tr.get("fault"):
            break
        begin = cur_lin()
        c = total_instr // CHUNK
        for sc in (0x3D, 0x41, 0x39, 0x1C):
            set_key(sc, False)
        if not opened(PAVNAME):
            force_level()
        if not opened(PAVNAME):
            if c <= 14:
                set_key(0x3D, True); set_key(0x41, True)
            elif c >= 16 and (c // 2) % 2 == 0:
                set_key(0x39, True)
        if countdown is None and opened(BUMNAME):
            countdown = SETTLE_TICKS
            print("[copyprot_oracle] level loaded (%s) at chunk %d — settling %d ticks" % (
                BUMNAME, c, SETTLE_TICKS), flush=True)
        if countdown is not None:
            if countdown > SETTLE_TICKS - 10 and (c // 2) % 2 == 0:
                set_key(0x39, True)
            countdown -= 1
            if countdown <= 0:
                break
        fire_int(8)
        begin = cur_lin()

    if tr.get("exit") is not None or tr.get("fault"):
        print("[copyprot_oracle] ERROR: premature exit/fault during boot: exit=%s fault=%s err=%s" % (
            tr.get("exit"), tr.get("fault"), err), flush=True)
        return
    if not opened(BUMNAME):
        print("[copyprot_oracle] ERROR: level %s never loaded after %dM instructions" % (
            BUMNAME, total_instr // 1_000_000), flush=True)
        return

    print("[copyprot_oracle] boot complete. Files: %s" % (
        [o[1] for o in tr.get("fileops", [])]), flush=True)

    boot_ram = bytes(uc.mem_read(0, RAM))
    boot_ctx = uc.context_save()

    def restore_boot_state() -> None:
        uc.mem_write(0, boot_ram)
        uc.context_restore(boot_ctx)
        tr["fault"] = None
        tr["exit"] = None

    # ---------------------------------------------------------------------------
    # call_near: synchronous near-call of a cdecl16near fn (DS=DGROUP).  RET lands on
    # a NOP;HLT pad at the top of the CODE segment (screens_oracle.py pattern).
    # ---------------------------------------------------------------------------
    code_seg = base & 0xFFFF
    LANDING_OFF = 0xfffe
    LANDING_LIN = CODE_LIN + LANDING_OFF
    STOP_LIN = LANDING_LIN + 1
    CALL_INSN_CAP = 80_000_000

    def call_near(fn_off: int, args: List[int]) -> None:
        saved = bytes(uc.mem_read(LANDING_LIN, 2))
        uc.mem_write(LANDING_LIN, b"\x90\xF4")   # NOP ; HLT
        ss = uc.reg_read(UC_X86_REG_SS) & 0xFFFF
        sp = uc.reg_read(UC_X86_REG_SP) & 0xFFFF
        for a in reversed(args):
            sp = (sp - 2) & 0xFFFF
            uc.mem_write(ss * 16 + sp, struct.pack("<H", a & 0xFFFF))
        sp = (sp - 2) & 0xFFFF
        uc.mem_write(ss * 16 + sp, struct.pack("<H", LANDING_OFF))
        uc.reg_write(UC_X86_REG_SP, sp)
        uc.reg_write(UC_X86_REG_DS, DS_SCREEN)
        uc.reg_write(UC_X86_REG_CS, code_seg)
        uc.reg_write(UC_X86_REG_IP, fn_off)
        try:
            uc.emu_start(CODE_LIN + fn_off, STOP_LIN, count=CALL_INSN_CAP)
        except UcError as e:
            tr.setdefault("call_errs", []).append((fn_off, str(e)))
        finally:
            uc.mem_write(LANDING_LIN, saved)

    # ---------------------------------------------------------------------------
    # Per-call capture state.  Populated by the sub-function hooks while a
    # copyprotect_challenge call runs.  challenge_bp holds BP of the active call so
    # the SS-frame locals (sprite_id_tbl, answer_tbl, entered_number) can be read.
    # ---------------------------------------------------------------------------
    cap: dict = {}

    def reset_cap() -> None:
        cap.clear()
        cap.update(dict(active=False, bp=None, fmemcpy_n=0,
                        positions=[], answers=[], prng_seed=None,
                        prng_state=(0, 0, 0), draws=[], accepted_index=None,
                        expected_answer=None, traj=[], desc=b"", in_present=False))

    reset_cap()

    def read_ss_local(bp: int, disp: int, n: int) -> bytes:
        # disp is the (negative) BP displacement, e.g. -0x26.
        ss = uc.reg_read(UC_X86_REG_SS) & 0xFFFF
        off = (bp + disp) & 0xFFFF
        return bytes(uc.mem_read(ss * 16 + off, n))

    # --- challenge entry: capture BP, mark active ---------------------------------
    def hook_challenge_entry(uc2: Uc, addr: int, size: int, _: object) -> None:
        if not cap.get("arming"):
            return
        cap["active"] = True
        cap["arming"] = False
        # BP is set up by the prologue (PUSH BP; MOV BP,SP) — by the time the first
        # sub-call fires, BP is stable.  We read BP at each capture point directly.

    # --- fmemcpy return: capture the two tables ------------------------------------
    # The challenge issues exactly TWO fmemcpy calls at the very top:
    #   #0 sprite_id_tbl (16 words, src DS:0x11b6)
    #   #1 answer_tbl    (16 bytes, src DS:0x11d6)
    # We snapshot the destination frame after the copy completes.  fmemcpy is cdecl;
    # we hook its RETURN by reading the caller return address pushed at entry, and at
    # that boundary read the SS-frame locals using the caller's BP (challenge BP).
    fmemcpy_ret_lins: set = set()
    fmemcpy_pending: dict = {}

    def hook_fmemcpy_entry(uc2: Uc, addr: int, size: int, _: object) -> None:
        if not cap.get("active"):
            return
        ss = uc2.reg_read(UC_X86_REG_SS); sp = uc2.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc2.mem_read(ss * 16 + sp, 2)))[0]
        ret_lin = (CODE_LIN + ret_off) & 0xFFFFF
        bp = uc2.reg_read(UC_X86_REG_BP) & 0xFFFF
        fmemcpy_pending.setdefault(ret_lin, []).append(bp)
        if ret_lin not in fmemcpy_ret_lins:
            fmemcpy_ret_lins.add(ret_lin)
            uc2.hook_add(UC_HOOK_CODE, hook_fmemcpy_exit, None, ret_lin, ret_lin)

    def hook_fmemcpy_exit(uc2: Uc, addr: int, size: int, _: object) -> None:
        stk = fmemcpy_pending.get(addr)
        if not stk:
            return
        bp = stk.pop()
        if not cap.get("active"):
            return
        n = cap["fmemcpy_n"]
        if n == 0:
            raw = read_ss_local(bp, -0x26, 0x20)
            cap["positions"] = list(struct.unpack("<16H", raw))
        elif n == 1:
            raw = read_ss_local(bp, -0x36, 0x10)
            cap["answers"] = list(raw)
        cap["fmemcpy_n"] = n + 1

    # --- prng_seed_thunk return: capture seed + 3-word prng state ------------------
    prng_ret_lins: set = set()
    prng_pending: dict = {}

    def hook_prng_entry(uc2: Uc, addr: int, size: int, _: object) -> None:
        if not cap.get("active"):
            return
        ss = uc2.reg_read(UC_X86_REG_SS); sp = uc2.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc2.mem_read(ss * 16 + sp, 2)))[0]
        ret_lin = (CODE_LIN + ret_off) & 0xFFFFF
        prng_pending[ret_lin] = prng_pending.get(ret_lin, 0) + 1
        if ret_lin not in prng_ret_lins:
            prng_ret_lins.add(ret_lin)
            uc2.hook_add(UC_HOOK_CODE, hook_prng_exit, None, ret_lin, ret_lin)

    def hook_prng_exit(uc2: Uc, addr: int, size: int, _: object) -> None:
        if prng_pending.get(addr, 0) <= 0 or not cap.get("active"):
            return
        prng_pending[addr] -= 1
        if cap["prng_seed"] is None:
            cap["prng_seed"] = rd_u16(OFF_PRNG_SEED_SRC)
            cap["prng_state"] = (rd_u16(OFF_PRNG_STATE0), rd_u16(OFF_PRNG_STATE1),
                                 rd_u16(OFF_PRNG_STATE2))

    # --- rand return: capture each draw + the reject loop --------------------------
    rand_ret_lins: set = set()
    rand_pending: dict = {}

    def hook_rand_entry(uc2: Uc, addr: int, size: int, _: object) -> None:
        if not cap.get("active") or not cap.get("in_present"):
            return
        # only capture rand() called from the index reject loop (in_present marks the
        # window between load_palette_byteswapped and the dial); the boot/scenarios
        # don't otherwise call rand here.
        ss = uc2.reg_read(UC_X86_REG_SS); sp = uc2.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc2.mem_read(ss * 16 + sp, 2)))[0]
        ret_lin = (CODE_LIN + ret_off) & 0xFFFFF
        rand_pending[ret_lin] = rand_pending.get(ret_lin, 0) + 1
        if ret_lin not in rand_ret_lins:
            rand_ret_lins.add(ret_lin)
            uc2.hook_add(UC_HOOK_CODE, hook_rand_exit, None, ret_lin, ret_lin)

    def hook_rand_exit(uc2: Uc, addr: int, size: int, _: object) -> None:
        if rand_pending.get(addr, 0) <= 0 or not cap.get("active") or not cap.get("in_present"):
            return
        rand_pending[addr] -= 1
        if cap["accepted_index"] is not None:
            return   # only the index reject loop (first accepted) is of interest
        ret = uc2.reg_read(UC_X86_REG_AX) & 0xFFFF
        masked = ret & 0xf
        accepted = masked >= 2
        cap["draws"].append((ret, masked, accepted))
        if accepted:
            cap["accepted_index"] = masked
            if cap["answers"]:
                cap["expected_answer"] = cap["answers"][masked]

    # --- open_resource / read_chunked STUBS (no INT 21h) ---------------------------
    # We mark the present window open at load_palette_byteswapped's predecessor; the
    # resource path runs BEFORE that, so stub it.  open_resource returns a sentinel
    # handle in AX and skips its body; read_chunked returns immediately.  We seed
    # level_dec_buf with a known pattern before the call (done in run_scenario).
    def hook_open_resource(uc2: Uc, addr: int, size: int, _: object) -> None:
        if not cap.get("active"):
            return
        # Force an immediate RET with a sentinel handle (no INT 21h c_open).
        ss = uc2.reg_read(UC_X86_REG_SS); sp = uc2.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc2.mem_read(ss * 16 + sp, 2)))[0]
        uc2.reg_write(UC_X86_REG_SP, (sp + 2) & 0xFFFF)
        uc2.reg_write(UC_X86_REG_AX, 0x0007)   # sentinel handle (>=0)
        uc2.reg_write(UC_X86_REG_IP, ret_off)
        cap["stubbed_open"] = cap.get("stubbed_open", 0) + 1

    def hook_read_chunked(uc2: Uc, addr: int, size: int, _: object) -> None:
        if not cap.get("active"):
            return
        # read_chunked(handle, off, seg, len_lo, len_hi) — cdecl, 5 word args; caller
        # cleans the stack (ADD SP,0xc), so we just RET (pop return addr) with AX=len.
        ss = uc2.reg_read(UC_X86_REG_SS); sp = uc2.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc2.mem_read(ss * 16 + sp, 2)))[0]
        uc2.reg_write(UC_X86_REG_SP, (sp + 2) & 0xFFFF)
        uc2.reg_write(UC_X86_REG_AX, 99)
        uc2.reg_write(UC_X86_REG_DX, 0)
        uc2.reg_write(UC_X86_REG_IP, ret_off)
        cap["stubbed_read"] = cap.get("stubbed_read", 0) + 1
        cap["in_present"] = True   # past the resource load -> present window open

    # --- blit_sprite entry: capture the p1_sprite descriptor -----------------------
    def hook_blit_sprite(uc2: Uc, addr: int, size: int, _: object) -> None:
        if not cap.get("active") or not cap.get("in_present"):
            return
        if cap["desc"]:
            return
        cap["desc"] = read_far_target(OFF_P1_SPRITE_PTR, P1_SPRITE_DESC_LEN)

    # --- poll_input entry: sample entered_number trajectory ------------------------
    # poll_input is called once per dial-loop iteration (1000:419e).  We sample
    # entered_number (SS:[BP-0x2]) just BEFORE the poll resolves the action, using the
    # challenge BP (= caller BP at poll_input entry).
    def hook_poll_input(uc2: Uc, addr: int, size: int, _: object) -> None:
        if not cap.get("active") or not cap.get("in_present"):
            return
        bp = uc2.reg_read(UC_X86_REG_BP) & 0xFFFF
        en = read_ss_local(bp, -0x2, 1)[0]
        cap["traj"].append(en)
        cap["last_bp"] = bp

    uc.hook_add(UC_HOOK_CODE, hook_challenge_entry, None,
                CODE_LIN + OFF_CHALLENGE, CODE_LIN + OFF_CHALLENGE)
    uc.hook_add(UC_HOOK_CODE, hook_fmemcpy_entry, None,
                CODE_LIN + OFF_FMEMCPY, CODE_LIN + OFF_FMEMCPY)
    uc.hook_add(UC_HOOK_CODE, hook_prng_entry, None,
                CODE_LIN + OFF_PRNG_SEED_THUNK, CODE_LIN + OFF_PRNG_SEED_THUNK)
    uc.hook_add(UC_HOOK_CODE, hook_rand_entry, None,
                CODE_LIN + OFF_RAND, CODE_LIN + OFF_RAND)
    uc.hook_add(UC_HOOK_CODE, hook_open_resource, None,
                CODE_LIN + OFF_OPEN_RESOURCE, CODE_LIN + OFF_OPEN_RESOURCE)
    uc.hook_add(UC_HOOK_CODE, hook_read_chunked, None,
                CODE_LIN + OFF_READ_CHUNKED, CODE_LIN + OFF_READ_CHUNKED)
    uc.hook_add(UC_HOOK_CODE, hook_blit_sprite, None,
                CODE_LIN + OFF_BLIT_SPRITE, CODE_LIN + OFF_BLIT_SPRITE)
    uc.hook_add(UC_HOOK_CODE, hook_poll_input, None,
                CODE_LIN + OFF_POLL_INPUT, CODE_LIN + OFF_POLL_INPUT)

    # ---------------------------------------------------------------------------
    # Expand a high-level dial action list into the FUN_75a2 return stream.  Each
    # action becomes [action, 0, 0, 0]: the action is consumed by one poll_input, then
    # the idle re-polls (input_state=0) re-loop the dial without changing state.  A
    # terminating ACT_FIRE pulse confirms and exits the loop.
    # ---------------------------------------------------------------------------
    def expand_script(actions: List[int]) -> List[int]:
        out: List[int] = []
        for a in actions:
            out.extend([a & 0xFF, 0, 0, 0])
        out.extend([ACT_FIRE, 0, 0, 0])
        return out

    SEED_BUF_PATTERN = bytes((0x90 + (i & 0x3f)) & 0xFF for i in range(99))

    def seed_level_dec_buf() -> None:
        o = rd_u16(OFF_LEVEL_DEC_BUF)
        s = rd_u16(OFF_LEVEL_DEC_SEG)
        lin = (s * 16 + o) & 0xFFFFF
        try:
            uc.mem_write(lin, SEED_BUF_PATTERN)
        except UcError:
            pass

    # ---------------------------------------------------------------------------
    # Run one scenario: restore boot, seed, call copyprotect_challenge once.
    # ---------------------------------------------------------------------------
    def run_scenario(sc_id: int, name: str, actions: List[int]) -> dict:
        restore_boot_state()
        reset_cap()
        cap["arming"] = True
        seed_level_dec_buf()
        uc.mem_write(DG_LIN + OFF_INPUT_STATE, bytes([0]))
        clear_all_keys()
        input_driver["queue"] = expand_script(actions)
        input_driver["active"] = True
        call_near(OFF_CHALLENGE, [])
        input_driver["active"] = False
        input_driver["queue"] = []
        cap["active"] = False
        flag = rd8(OFF_COPYPROTECT)
        # final entered_number / entry_done: read from the last-known challenge BP.
        en_final = ed_final = 0
        bp = cap.get("last_bp")
        if bp is not None:
            en_final = read_ss_local(bp, -0x2, 1)[0]
            ed_final = read_ss_local(bp, -0x3, 1)[0]
        desc = cap["desc"]
        dx = struct.unpack_from("<H", desc, 0)[0] if len(desc) >= 2 else 0
        dy = struct.unpack_from("<H", desc, 2)[0] if len(desc) >= 4 else 0
        dframe = struct.unpack_from("<H", desc, 4)[0] if len(desc) >= 6 else 0
        return dict(id=sc_id, name=name, actions=list(actions),
                    draws=list(cap["draws"]),
                    accepted_index=cap["accepted_index"],
                    expected_answer=cap["expected_answer"],
                    traj=list(cap["traj"]), en_final=en_final, ed_final=ed_final,
                    flag=flag, dx=dx, dy=dy, dframe=dframe, desc=desc,
                    stubbed_open=cap.get("stubbed_open", 0),
                    stubbed_read=cap.get("stubbed_read", 0))

    results: List[dict] = []
    shared = {"prng_seed": None, "prng_state": (0, 0, 0), "positions": [], "answers": []}
    for sc_id, name, actions in SCENARIOS:
        print("[copyprot_oracle] === scenario %d (%s) actions=%s ===" % (
            sc_id, name, [hex(a) for a in actions]), flush=True)
        r = run_scenario(sc_id, name, actions)
        # the tables / prng seed are captured identically each run; record once.
        if shared["prng_seed"] is None and cap["prng_seed"] is not None:
            shared["prng_seed"] = cap["prng_seed"]
            shared["prng_state"] = cap["prng_state"]
            shared["positions"] = cap["positions"]
            shared["answers"] = cap["answers"]
        print("[copyprot_oracle]   index=%s answer=%s traj=%s flag=%d desc(x=%d,y=%d,frame=%d) "
              "stub(open=%d,read=%d) draws=%d" % (
                  r["accepted_index"], r["expected_answer"], r["traj"], r["flag"],
                  r["dx"], r["dy"], r["dframe"], r["stubbed_open"], r["stubbed_read"],
                  len(r["draws"])), flush=True)
        results.append(r)

    # ---------------------------------------------------------------------------
    # Sanity: confirm captured tables match the known-recovered values.
    # ---------------------------------------------------------------------------
    EXP_POS = [0x00, 0x00, 0x46, 0x4f, 0x51, 0x93, 0x9f, 0xa8,
               0xb4, 0xb3, 0xbe, 0xc4, 0xca, 0xcb, 0xda, 0xe1]
    EXP_ANS = [0, 0, 4, 6, 7, 5, 15, 16, 24, 19, 28, 26, 25, 27, 17, 18]
    pos_ok = shared["positions"] == EXP_POS
    ans_ok = shared["answers"] == EXP_ANS
    print("[copyprot_oracle] tables: positions match=%s answers match=%s (seed=0x%04x state=%s)" % (
        pos_ok, ans_ok, shared["prng_seed"] or 0, shared["prng_state"]), flush=True)

    # ---------------------------------------------------------------------------
    # Write the frozen trace.
    # ---------------------------------------------------------------------------
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT_TRACE, "wb") as f:
        f.write(TRACE_MAGIC)
        f.write(struct.pack("<HH", TRACE_VERSION, len(results)))
        f.write(struct.pack("<H", shared["prng_seed"] or 0))
        f.write(struct.pack("<HHH", *shared["prng_state"]))
        f.write(struct.pack("<B", len(shared["positions"])))
        for w in shared["positions"]:
            f.write(struct.pack("<H", w & 0xFFFF))
        f.write(struct.pack("<B", len(shared["answers"])))
        for b in shared["answers"]:
            f.write(struct.pack("<B", b & 0xFF))
        for r in results:
            nb = r["name"].encode("ascii")
            f.write(struct.pack("<B", r["id"]))
            f.write(struct.pack("<B", len(nb))); f.write(nb)
            f.write(struct.pack("<B", len(r["actions"])))
            f.write(bytes(a & 0xFF for a in r["actions"]))
            f.write(struct.pack("<B", len(r["draws"])))
            for (ret, masked, accepted) in r["draws"]:
                f.write(struct.pack("<HBB", ret & 0xFFFF, masked & 0xFF, 1 if accepted else 0))
            f.write(struct.pack("<B", (r["accepted_index"] or 0) & 0xFF))
            f.write(struct.pack("<B", (r["expected_answer"] or 0) & 0xFF))
            f.write(struct.pack("<B", len(r["traj"])))
            f.write(bytes(t & 0xFF for t in r["traj"]))
            f.write(struct.pack("<B", r["en_final"] & 0xFF))
            f.write(struct.pack("<B", r["ed_final"] & 0xFF))
            f.write(struct.pack("<B", r["flag"] & 0xFF))
            f.write(struct.pack("<HHH", r["dx"] & 0xFFFF, r["dy"] & 0xFFFF, r["dframe"] & 0xFFFF))
            f.write(struct.pack("<B", len(r["desc"])))
            f.write(r["desc"])
    trace_bytes = os.path.getsize(OUT_TRACE)
    print("[copyprot_oracle] wrote %s (%d B)" % (OUT_TRACE, trace_bytes), flush=True)

    # ---------------------------------------------------------------------------
    # Round-trip parser check (standalone re-parse of the file we just wrote).
    # ---------------------------------------------------------------------------
    def parse_trace(path: str) -> dict:
        data = open(path, "rb").read()
        assert data[:8] == TRACE_MAGIC, "bad magic"
        ver, n_sc = struct.unpack_from("<HH", data, 8)
        assert ver == TRACE_VERSION
        o = 12
        prng_seed = struct.unpack_from("<H", data, o)[0]; o += 2
        prng_state = struct.unpack_from("<HHH", data, o); o += 6
        n_pos = data[o]; o += 1
        positions = list(struct.unpack_from("<%dH" % n_pos, data, o)); o += 2 * n_pos
        n_ans = data[o]; o += 1
        answers = list(data[o:o + n_ans]); o += n_ans
        scs = []
        for _ in range(n_sc):
            sid = data[o]; o += 1
            nl = data[o]; o += 1
            nm = data[o:o + nl].decode("ascii"); o += nl
            n_act = data[o]; o += 1
            acts = list(data[o:o + n_act]); o += n_act
            n_dr = data[o]; o += 1
            draws = []
            for _i in range(n_dr):
                ret, masked, acc = struct.unpack_from("<HBB", data, o); o += 4
                draws.append((ret, masked, acc))
            acc_idx = data[o]; o += 1
            exp_ans = data[o]; o += 1
            n_tr = data[o]; o += 1
            traj = list(data[o:o + n_tr]); o += n_tr
            en_f = data[o]; o += 1
            ed_f = data[o]; o += 1
            flag = data[o]; o += 1
            dx, dy, dframe = struct.unpack_from("<HHH", data, o); o += 6
            ndl = data[o]; o += 1
            desc = data[o:o + ndl]; o += ndl
            scs.append(dict(id=sid, name=nm, actions=acts, draws=draws,
                            accepted_index=acc_idx, expected_answer=exp_ans,
                            traj=traj, en_final=en_f, ed_final=ed_f, flag=flag,
                            dx=dx, dy=dy, dframe=dframe, desc=desc))
        assert o == len(data), "trailing bytes: parsed %d of %d" % (o, len(data))
        return dict(prng_seed=prng_seed, prng_state=prng_state, positions=positions,
                    answers=answers, scenarios=scs)

    parsed = parse_trace(OUT_TRACE)
    assert parsed["positions"] == shared["positions"], "positions round-trip mismatch"
    assert parsed["answers"] == shared["answers"], "answers round-trip mismatch"
    assert len(parsed["scenarios"]) == len(results)
    print("[copyprot_oracle] round-trip parse OK: %d scenarios, seed=0x%04x" % (
        len(parsed["scenarios"]), parsed["prng_seed"]), flush=True)

    # ---------------------------------------------------------------------------
    # copyprot_model.md
    # ---------------------------------------------------------------------------
    lines: List[str] = []
    lines.append("# Bumpy Phase-7b copy-protection challenge capture model (discovery)\n\n")
    lines.append("Generated by `tools/copyprot_oracle.py`. Target: `copyprotect_challenge` "
                 "(Ghidra 1000:4015).  Capture granularity = sub-function call boundaries "
                 "inside one synchronous near-call of the routine, driven by a scripted "
                 "`+`/`-`/ENTER dial input stream.\n\n")
    lines.append("## The routine is CRACKED\n\n")
    lines.append("`copyprotect_challenge` writes `copyprotect_flag = 1` UNCONDITIONALLY at "
                 "1000:412e (decomp: \"COPY PROTECTION DEFEATED HERE\"), BEFORE input is read, "
                 "and never compares the entered number to the answer table.  The original "
                 "set `copyprotect_flag = -1` on mismatch (which reset the player to level 1 "
                 "in start_level).  Captured `copyprotect_flag` at exit = 1 in every "
                 "scenario.\n\n")
    lines.append("## Resolved addresses (from the live Ghidra decomp/asm)\n\n")
    lines.append("### Code (Ghidra seg 0x1000; runtime CODE_LIN+off, CODE_LIN=0x%05x)\n\n" % CODE_LIN)
    lines.append("| symbol | off | role |\n|---|---|---|\n")
    for nm, off, role in [
        ("copyprotect_challenge", OFF_CHALLENGE, "the routine"),
        ("fmemcpy", OFF_FMEMCPY, "table copies (sprite_id_tbl / answer_tbl)"),
        ("prng_seed_thunk", OFF_PRNG_SEED_THUNK, "seeds prng_state0 from DGROUP 0x119c"),
        ("rand", OFF_RAND, "index reject loop do{i=rand()&0xf}while(i<2)"),
        ("open_resource", OFF_OPEN_RESOURCE, "res 0x90 load (STUBBED — no INT 21h)"),
        ("read_chunked", OFF_READ_CHUNKED, "read into level_dec_buf (STUBBED)"),
        ("blit_sprite", OFF_BLIT_SPRITE, "blits 0x792e platform-sprite descriptor"),
        ("poll_input", OFF_POLL_INPUT, "dial loop input -> input_state"),
        ("read_input_action (FUN_75a2)", OFF_FUN_75A2, "input primitive (driver hook)"),
    ]:
        lines.append("| %s | 0x%04x | %s |\n" % (nm, off, role))
    lines.append("\n### DGROUP (Ghidra seg 0x203b; runtime DG_LIN+off, DG_LIN=0x%05x)\n\n" % DG_LIN)
    lines.append("| symbol | off |\n|---|---|\n")
    for nm, off in [("prng seed source (DAT_203b_119c)", OFF_PRNG_SEED_SRC),
                    ("prng_state0", OFF_PRNG_STATE0), ("prng_state1", OFF_PRNG_STATE1),
                    ("prng_state2", OFF_PRNG_STATE2), ("palette_mode", OFF_PALETTE_MODE),
                    ("input_state", OFF_INPUT_STATE), ("copyprotect_flag", OFF_COPYPROTECT),
                    ("level_dec_buf off", OFF_LEVEL_DEC_BUF), ("level_dec_seg", OFF_LEVEL_DEC_SEG),
                    ("p1_sprite far ptr (-> 0x792e)", OFF_P1_SPRITE_PTR)]:
        lines.append("| %s | 0x%04x |\n" % (nm, off))
    lines.append("\n## Stack-local layout (copyprotect_challenge, SUB SP,0x36)\n\n")
    lines.append("| local | BP disp | type |\n|---|---|---|\n")
    lines.append("| answer_tbl[16]   | [BP-0x36] | 16 bytes (fmemcpy from DS:0x11d6) |\n")
    lines.append("| sprite_id_tbl[16]| [BP-0x26] | 16 words (fmemcpy from DS:0x11b6) |\n")
    lines.append("| round_state      | [BP-0x05] | u8 (0; 0xff after confirm) |\n")
    lines.append("| expected_answer  | [BP-0x04] | u8 (= answer_tbl[index]) |\n")
    lines.append("| entry_done       | [BP-0x03] | u8 (0; 1 on ENTER/fire) |\n")
    lines.append("| entered_number   | [BP-0x02] | u8 (dial value, 0..0x62) |\n")
    lines.append("| copy_idx         | [BP-0x01] | u8 (copy loop counter) |\n")
    lines.append("\n## Captured tables\n\n")
    lines.append("Positions (sprite_id_tbl, 16 words, DS:0x11b6) — match recovered: **%s**\n\n"
                 % pos_ok)
    lines.append("```\n%s\n```\n\n" % " ".join("%02x" % w for w in shared["positions"]))
    lines.append("Answers (answer_tbl, 16 bytes, DS:0x11d6) — match recovered: **%s**\n\n"
                 % ans_ok)
    lines.append("```\n%s\n```\n\n" % " ".join("%d" % b for b in shared["answers"]))
    lines.append("## PRNG seed + index draws\n\n")
    lines.append("prng_seed_thunk seeds prng_state0 from DGROUP 0x119c = "
                 "**0x%04x**; state right after seed = (0x%04x, 0x%04x, 0x%04x).  The host "
                 "(src/prng.c) reproduces the index draws by `prng_seed(0x%04x)` then "
                 "`do { i = rand() & 0xf; } while (i < 2);` — the SAME 3-word prng.  Index "
                 "lands in 2..15.\n\n" % (shared["prng_seed"] or 0, *shared["prng_state"],
                                          shared["prng_seed"] or 0))
    lines.append("## Per-scenario dial captures\n\n")
    lines.append("| id | name | actions | draws (ret&0xf) | index | answer | "
                 "entered_number trajectory | en_final | flag |\n")
    lines.append("|---|---|---|---|---|---|---|---|---|\n")
    act_name = {ACT_MINUS: "-", ACT_PLUS: "+", ACT_FIRE: "fire"}
    for r in results:
        acts = " ".join(act_name.get(a, hex(a)) for a in r["actions"]) or "(none)"
        drs = " ".join("%d%s" % (m, "*" if a else "") for (_ret, m, a) in r["draws"])
        traj = " ".join(str(t) for t in r["traj"])
        lines.append("| %d | %s | %s | %s | %s | %s | %s | %d | %d |\n" % (
            r["id"], r["name"], acts, drs, r["accepted_index"], r["expected_answer"],
            traj, r["en_final"], r["flag"]))
    lines.append("\n(`*` marks the ACCEPTED draw — first `rand()&0xf >= 2`.)\n\n")
    lines.append("## Platform-sprite display descriptor (p1_sprite @ 0x792e, at blit)\n\n")
    lines.append("The routine writes `p1_sprite` (far ptr @ DGROUP 0x8884 -> 0x792e): "
                 "word[0]=x=0x90, word[1]=y=100(0x64), word[2]=frame=sprite_id_tbl[index], "
                 "then calls blit_sprite(0x792e).  Captured per scenario:\n\n")
    lines.append("| id | desc x | desc y | frame | raw descriptor |\n|---|---|---|---|---|\n")
    for r in results:
        lines.append("| %d | 0x%x | %d | 0x%x | %s |\n" % (
            r["id"], r["dx"], r["dy"], r["dframe"],
            " ".join("%02x" % b for b in r["desc"]) or "(none)"))
    lines.append("\n## Resource load (res 0x90) — STUBBED\n\n")
    lines.append("`set_resource_table(0x90,0x203b)` then `open_resource()` (INT 21h c_open) "
                 "+ `read_chunked(...)` into level_dec_buf @ 0x6be8:0x6bea (99 bytes).  We "
                 "STUB open_resource (return sentinel handle 0x0007) and read_chunked (no-op, "
                 "AX=99) — NO INT 21h — after seeding level_dec_buf with a known 99-byte "
                 "pattern.  The present-side values we validate (tables, index, dial, "
                 "descriptor, flag) do not depend on the loaded bytes.\n\n")
    lines.append("## Trace format\n\n")
    lines.append("`%s` (frozen). Magic `CPTRC01\\0`, version %d, %d scenarios, %d bytes.  "
                 "See the module docstring for the exact byte layout; a standalone "
                 "round-trip parser re-reads it in this script.\n" % (
                     os.path.basename(OUT_TRACE), TRACE_VERSION, len(results), trace_bytes))

    with open(OUT_MODEL, "w") as f:
        f.write("".join(lines))
    print("[copyprot_oracle] wrote %s" % OUT_MODEL, flush=True)
    print("[copyprot_oracle] DONE in %.0fs" % (time.time() - t_start), flush=True)


if __name__ == "__main__":
    main()
