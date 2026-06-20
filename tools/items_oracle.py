#!/usr/bin/env python3
"""items_oracle.py — Phase-3 item/exit semantic-state CAPTURE-AS-DISCOVERY harness.

Boots the real (unpacked) BUMPY.EXE under Unicorn — reusing the boot + DGROUP
scaffold of tools/physics_oracle.py (deliberately NOT refactoring it) — drives the
5 done-bar item/exit scenarios, and captures the engine's SEMANTIC game-state
(score / items_remaining / exit-cell / complete-flag / item-code / tilemap item byte)
at the ENTRY and EXIT of each of the 5 hooked item/exit functions.

Unlike the Phase-2 physics oracle (continuous trajectory, desync-prone), this
validates DISCRETE semantic events, so there is no trajectory desync: each scenario
puts the player onto the relevant item/exit cell and we record what the real engine
code does to the semantic state.

DRIVING STRATEGY (documented per scenario in items_model.md):
  Scripting precise key-navigation onto a specific item cell under Unicorn is
  impractical and unbounded (physics_oracle showed a scripted walk advances only a
  couple cells), so — as the task brief sanctions — we SEED the needed state
  directly (set p1_cell onto a discovered item/exit cell + seed the tilemap item
  byte / items_remaining / exit-cell as needed) and then invoke the REAL engine
  function at its entry IP via a synthetic near-call frame. The function body that
  runs is the unmodified original code; only the precondition cell/state is seeded.
  We DISCOVER the live level-1 item layout by scanning the post-boot tilemap layer-C
  bytes (tilemap[cell+0x60] for cells 0..0x2f) — runtime truth, no BUM reversing.

Hooked item/exit functions (Ghidra seg-1000 off -> runtime linear 0x1100+off):
  p1_collect_item            1000:6c14
  p1_collect_item_score      1000:6c95
  check_exit_tile_vert       1000:6372
  move_step_read_item        1000:6627
  teleport_to_next_exit_tile 1000:25ad

Outputs (BOTH gitignored — discovery; NO commit):
  local/build/render/items_trace.bin   (frozen layout — see TRACE LAYOUT below)
  local/build/items_model.md           (per-scenario fn-call sequence + effects + addrs)

RESOLVED semantic-state DGROUP addresses (Ghidra DGROUP 0x203b offsets; read from the
disassembly operands of the 5 functions + read_tile_layer2 6bf4 + start_level 2d14):
  score_lo                   0xa0d4 (u16)   ADD word[0xa0d4] in 6c95
  score_hi                   0xa0d6 (u16)   ADC word[0xa0d6]
  items_remaining            0xa0cf (u8)    DEC byte[0xa0cf] in 6c14   (== brief DAT_203b_a0cf)
  level_exit_cell            0x8572 (u8)     MOV AL,[0x8572] in 6c14
  level_complete_flag        0xa1b1 (u8)     MOV byte[0xa1b1],1 in 6c14
  level_complete_anim_counter 0x8550 (u8)    MOV byte[0x8550],0xf2 in 6c14
  p1_item_code               0x79b8 (u8)     read_tile_layer2 6bf4 writes it; tested in 6c14
  p1_cell                    0x856e (u8)
  anim_target_cell           0x856f (u8)     MOV [0x856f],AL (= level_exit_cell / scan_cell)
  current_level              0x79b2 (u8)     start_level 2d14 (NOTE: brief said 0x8f40 — that
                                             is wrong for this build; 0x79b2 is the real one,
                                             matching physics_oracle OFF_CURRENT_LEVEL)
  copyprotect_flag           0x119a (s8)     start_level 2d14 / [0x119a]
  move_step_count            0x855e (u8)     check_exit_tile_vert 6372 cmp [0x855e],7
  p1_move_step_idx           0x792a (u8)     set to 0 in 6372
  physics_frozen             0xa0ce (u8)     set to 1 in 6372
  p1_pixel_y                 0x9292 (s16)    teleport nudges +0xd
  sound_mode_selector        0x689c (u16)    cmp [0x689c],4 (sound id select; NOT current_level)
  tilemap far ptr            off 0xa0d8 / seg 0xa0da   (LES BX,[0xa0d8]); item byte = ES:[BX+cell+0x60]

TRACE LAYOUT (little-endian) — FROZEN; Task-2 (items_ctest.c) parses this exactly:
  Header:
    +0x00  8 B   magic   b"ITEMTRC1"
    +0x08  2 B   u16     version (=1)
    +0x0A  2 B   u16     n_scenarios
  fn-name string table:
    2 B   u16     n_fn_names
    per name: u8 len, len bytes (ascii)
  Then, per scenario:
    u8        scenario_id
    u8        name_len, name_len bytes (ascii scenario name)
    u8        setup_kind   (0 = scripted, 1 = seeded)
    u8        level
    u8        start_cell   (the seeded/forced p1_cell, or 0xFF)
    u32       n_records
    then n_records records.

  Per RECORD (one item/exit-function call; carries BOTH entry and exit snapshots):
    u16   fn_addr        (Ghidra seg-1000 offset of the hooked fn, e.g. 0x6c14)
    u16   fn_name_idx    (index into the fn-name string table)
    SNAP  entry          (SNAP_SIZE-byte fixed struct, see SNAP below)
    SNAP  exit           (SNAP_SIZE-byte fixed struct)

  SNAP (the semantic state; little-endian) — SNAP_FMT below:
    u16  score_lo                (DGROUP 0xa0d4)
    u16  score_hi                (DGROUP 0xa0d6)
    u8   items_remaining         (0xa0cf)
    u8   level_exit_cell         (0x8572)
    u8   level_complete_flag     (0xa1b1)
    u8   level_complete_anim_counter (0x8550)
    u8   p1_item_code            (0x79b8)
    u8   p1_cell                 (0x856e)
    u8   anim_target_cell        (0x856f)
    u8   current_level           (0x79b2)
    u8   move_step_count         (0x855e)
    u8   physics_frozen          (0xa0ce)
    s16  p1_pixel_y              (0x9292)
    u8   tilemap_item_byte       (tilemap[p1_cell+0x60] — the layer-C item code at p1_cell)
    u8   _pad                    (0)

Run (sandbox disabled — needs unicorn/uv cache access), HARD timeout:
  timeout 1800 uv run python tools/items_oracle.py
"""
from __future__ import annotations
import struct
import os
import collections
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
OUT_TRACE = os.path.join(OUT_DIR, "items_trace.bin")
OUT_MODEL = os.path.join(ROOT, "local/build/items_model.md")

PSP_SEG = 0x0100
RAM = 0x110000

# DGROUP runtime base — identical formula to physics_oracle.py.
DG_LIN: int = (0x103b + PSP_SEG + 0x10) * 16   # 0x114b0
# Ghidra "segment 1000" runtime base (image load base*16). Hooks use 0x1100+off.
CODE_LIN: int = (PSP_SEG + 0x10) * 16          # 0x1100

# ---------------------------------------------------------------------------
# Resolved semantic-state DGROUP global offsets (Ghidra DGROUP 0x203b offsets)
# ---------------------------------------------------------------------------
OFF_SCORE_LO: int = 0xa0d4           # u16
OFF_SCORE_HI: int = 0xa0d6           # u16
OFF_ITEMS_REMAINING: int = 0xa0cf    # u8
OFF_LEVEL_EXIT_CELL: int = 0x8572    # u8
OFF_LEVEL_COMPLETE_FLAG: int = 0xa1b1  # u8
OFF_LEVEL_COMPLETE_ANIM_CTR: int = 0x8550  # u8
OFF_P1_ITEM_CODE: int = 0x79b8       # u8
OFF_P1_CELL: int = 0x856e            # u8
OFF_ANIM_TARGET_CELL: int = 0x856f   # u8
OFF_CURRENT_LEVEL: int = 0x79b2      # u8
OFF_MOVE_STEP_COUNT: int = 0x855e    # u8
OFF_PHYSICS_FROZEN: int = 0xa0ce     # u8
OFF_P1_PIXEL_Y: int = 0x9292         # s16
OFF_P1_MOVE_STEP_IDX: int = 0x792a   # u8
OFF_COPYPROTECT: int = 0x119a        # s8
OFF_SOUND_MODE_SEL: int = 0x689c     # u16 (cmp ==4 sound select)

OFF_TILEMAP_PTR: int = 0xa0d8        # far ptr (off @0xa0d8, seg @0xa0da)
OFF_INPUT_STATE: int = 0x8244        # u8 (boot driving only)
OFF_KEY_STATE_PTR: int = 0x4D42      # near ptr to g_key_state_table base

# ---------------------------------------------------------------------------
# Hooked item/exit functions (Ghidra seg-1000 offsets)
# ---------------------------------------------------------------------------
FN_NAMES: Dict[int, str] = {
    0x6c14: "p1_collect_item",
    0x6c95: "p1_collect_item_score",
    0x6372: "check_exit_tile_vert",
    0x6627: "move_step_read_item",
    0x25ad: "teleport_to_next_exit_tile",
}

# Item-code special-cases (no items_remaining decrement): 0x01 and '#' (0x23).
SPECIAL_ITEM_CODES = (0x01, 0x23)

# Tile codes (from the decomp): layer-C item byte 0..0x2f; exit-detect tile = 0x0c;
# teleport scan tile = 0x0f.
TILE_EXIT_NEIGHBOR: int = 0x0c
TILE_TELEPORT: int = 0x0f

# ---------------------------------------------------------------------------
# Trace format constants
# ---------------------------------------------------------------------------
TRACE_MAGIC: bytes = b"ITEMTRC1"
TRACE_VERSION: int = 1
# SNAP: score_lo,score_hi (u16,u16); then 9 u8; then p1_pixel_y (s16); then item_byte,pad (u8,u8)
SNAP_FMT: str = "<HH BBBBBBBBBB h BB"
SNAP_FMT = SNAP_FMT.replace(" ", "")
SNAP_SIZE: int = struct.calcsize(SNAP_FMT)

SNAP_FIELDS = [
    "score_lo", "score_hi", "items_remaining", "level_exit_cell",
    "level_complete_flag", "level_complete_anim_counter", "p1_item_code",
    "p1_cell", "anim_target_cell", "current_level", "move_step_count",
    "physics_frozen", "p1_pixel_y", "tilemap_item_byte", "pad",
]
assert len(SNAP_FIELDS) == 15

SETUP_SCRIPTED = 0
SETUP_SEEDED = 1


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

    # --- minimal VGA planar emulation (copied from physics_oracle.py) ---------------
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

    # ---------------------------------------------------------------------------
    # DGROUP read/write helpers + semantic SNAP
    # ---------------------------------------------------------------------------
    def rd8(off: int) -> int:
        return uc.mem_read(DG_LIN + off, 1)[0]

    def rd_u16(off: int) -> int:
        return struct.unpack("<H", bytes(uc.mem_read(DG_LIN + off, 2)))[0]

    def rd_s16(off: int) -> int:
        return struct.unpack("<h", bytes(uc.mem_read(DG_LIN + off, 2)))[0]

    def wr8(off: int, v: int) -> None:
        uc.mem_write(DG_LIN + off, bytes([v & 0xFF]))

    def wr_u16(off: int, v: int) -> None:
        uc.mem_write(DG_LIN + off, struct.pack("<H", v & 0xFFFF))

    def tilemap_ptr() -> int:
        off, seg = struct.unpack("<HH", bytes(uc.mem_read(DG_LIN + OFF_TILEMAP_PTR, 4)))
        return (seg * 16 + off) & 0xFFFFF

    def rd_tile(cell_off: int) -> int:
        try:
            return uc.mem_read((tilemap_ptr() + cell_off) & 0xFFFFF, 1)[0]
        except UcError:
            return 0

    def wr_tile(cell_off: int, v: int) -> None:
        uc.mem_write((tilemap_ptr() + cell_off) & 0xFFFFF, bytes([v & 0xFF]))

    def item_byte_at(cell: int) -> int:
        # layer-C item code lives at tilemap[cell + 0x60]
        return rd_tile((cell + 0x60) & 0xFFFF)

    def snap() -> bytes:
        cell = rd8(OFF_P1_CELL)
        return struct.pack(
            SNAP_FMT,
            rd_u16(OFF_SCORE_LO), rd_u16(OFF_SCORE_HI),
            rd8(OFF_ITEMS_REMAINING), rd8(OFF_LEVEL_EXIT_CELL),
            rd8(OFF_LEVEL_COMPLETE_FLAG), rd8(OFF_LEVEL_COMPLETE_ANIM_CTR),
            rd8(OFF_P1_ITEM_CODE), cell, rd8(OFF_ANIM_TARGET_CELL),
            rd8(OFF_CURRENT_LEVEL), rd8(OFF_MOVE_STEP_COUNT),
            rd8(OFF_PHYSICS_FROZEN), rd_s16(OFF_P1_PIXEL_Y),
            item_byte_at(cell), 0)

    # ---------------------------------------------------------------------------
    # Item/exit-function hooks (entry + exit via dynamic return-address hook)
    # ---------------------------------------------------------------------------
    capturing = {"on": False}
    cur_records: List[bytes] = []
    pending_exit: dict = {}     # ret_lin -> (fn_off, entry_snap)
    exit_hook_lins: set = set()
    fn_name_list: List[str] = list(dict.fromkeys(FN_NAMES.values()))
    fn_name_idx = {n: i for i, n in enumerate(fn_name_list)}

    def emit_record(fn_off: int, entry_snap: bytes, exit_snap: bytes) -> None:
        rec = struct.pack("<HH", fn_off, fn_name_idx[FN_NAMES[fn_off]])
        rec += entry_snap + exit_snap
        cur_records.append(rec)

    def hook_fn_entry(uc: Uc, addr: int, size: int, _: object) -> None:
        if not capturing["on"]:
            return
        fn_off = (addr - CODE_LIN) & 0xFFFF
        if fn_off not in FN_NAMES:
            return
        entry_snap = snap()
        ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc.mem_read(ss * 16 + sp, 2)))[0]
        ret_lin = (CODE_LIN + ret_off) & 0xFFFFF
        pending_exit[ret_lin] = (fn_off, entry_snap)
        if ret_lin not in exit_hook_lins:
            exit_hook_lins.add(ret_lin)
            uc.hook_add(UC_HOOK_CODE, hook_fn_exit, None, ret_lin, ret_lin)

    def hook_fn_exit(uc: Uc, addr: int, size: int, _: object) -> None:
        info = pending_exit.pop(addr, None)
        if info is None:
            return
        fn_off, entry_snap = info
        exit_snap = snap()
        emit_record(fn_off, entry_snap, exit_snap)

    for off in FN_NAMES:
        lin = CODE_LIN + off
        uc.hook_add(UC_HOOK_CODE, hook_fn_entry, None, lin, lin)

    # ---------------------------------------------------------------------------
    # Synthetic near-call into an engine fn (seeded scenarios).
    # Builds a near-call frame on the current stack, runs until RET returns to a
    # sentinel address, and returns. Hooks fire normally (entry + return-addr exit).
    # ---------------------------------------------------------------------------
    SENTINEL_OFF = 0x000A   # an iret stub region offset we never use as code; we
    # actually pick the PSP HLT trick: push a known return offset that lands on a
    # one-byte stop. Simpler: push ret addr = a CODE_LIN offset containing 0xF4 HLT
    # we plant. We plant HLT at CODE_LIN + STOP_OFF (unused padding) and stop on it.
    STOP_OFF = 0x0008

    def call_engine_fn(fn_off: int) -> None:
        # Plant a HLT at a scratch code offset; emu_stop on reaching it.
        uc.mem_write(CODE_LIN + STOP_OFF, b"\xF4")
        ss = uc.reg_read(UC_X86_REG_SS) & 0xFFFF
        sp = uc.reg_read(UC_X86_REG_SP) & 0xFFFF
        sp = (sp - 2) & 0xFFFF
        uc.mem_write(ss * 16 + sp, struct.pack("<H", STOP_OFF))
        uc.reg_write(UC_X86_REG_SP, sp)
        stop_flag = {"hit": False}

        def stop_hook(u: Uc, a: int, s: int, _: object) -> None:
            stop_flag["hit"] = True
            u.emu_stop()
        h = uc.hook_add(UC_HOOK_CODE, stop_hook, None,
                        CODE_LIN + STOP_OFF, CODE_LIN + STOP_OFF)
        try:
            uc.emu_start(CODE_LIN + fn_off, 0, count=20_000_000)
        except UcError as e:
            tr["call_err"] = str(e)
        finally:
            uc.hook_del(h)

    # ---------------------------------------------------------------------------
    # Boot to level 1 (identical approach to physics_oracle.py)
    # ---------------------------------------------------------------------------
    LEVEL = 1
    PAVNAME = "D%d.PAV" % LEVEL
    BUMNAME = "D%d.BUM" % LEVEL

    def force_level() -> None:
        wr8(OFF_CURRENT_LEVEL, LEVEL)
        uc.mem_write(DG_LIN + OFF_COPYPROTECT, bytes([1]))

    CHUNK = 1_000_000
    total_instr = 0
    begin = cur_lin()
    err = None
    countdown = None
    SETTLE_TICKS = 80

    print("[items_oracle] booting BUMPY (level %d)..." % LEVEL, flush=True)

    while total_instr < 400_000_000:
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e); tr["err"] = err; break
        total_instr += CHUNK
        if total_instr % 40_000_000 == 0:
            print("[items_oracle] %dM instr, countdown=%s" % (
                total_instr // 1_000_000, countdown), flush=True)
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
            print("[items_oracle] level loaded (%s) at chunk %d — settling %d ticks" % (
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
        print("[items_oracle] ERROR: premature exit/fault during boot: exit=%s fault=%s err=%s" % (
            tr.get("exit"), tr.get("fault"), err), flush=True)
        return
    if not opened(BUMNAME):
        print("[items_oracle] ERROR: level %s never loaded after %dM instr" % (
            BUMNAME, total_instr // 1_000_000), flush=True)
        return

    print("[items_oracle] boot complete. Files: %s" % (
        [o[1] for o in tr.get("fileops", [])]), flush=True)

    # ---------------------------------------------------------------------------
    # DISCOVER the live level-1 item layout: scan layer-C bytes for cells 0..0x2f.
    # ---------------------------------------------------------------------------
    item_layout: List[Tuple[int, int]] = []   # (cell, item_code)
    for cell in range(0x30):
        code = item_byte_at(cell)
        if code != 0:
            item_layout.append((cell, code))
    print("[items_oracle] discovered %d non-zero item cells:" % len(item_layout), flush=True)
    for cell, code in item_layout:
        kind = "SPECIAL" if code in SPECIAL_ITEM_CODES else "normal"
        print("   cell 0x%02x  code 0x%02x (%r) %s" % (
            cell, code, chr(code) if 32 <= code < 127 else "?", kind), flush=True)
    boot_exit_cell = rd8(OFF_LEVEL_EXIT_CELL)
    boot_items_remaining = rd8(OFF_ITEMS_REMAINING)
    print("[items_oracle] level_exit_cell=0x%02x  items_remaining=%d  current_level=%d" % (
        boot_exit_cell, boot_items_remaining, rd8(OFF_CURRENT_LEVEL)), flush=True)

    # Choose representative cells from the discovered layout.
    normal_cells = [c for c, code in item_layout if code not in SPECIAL_ITEM_CODES]
    special_cells = [c for c, code in item_layout if code in SPECIAL_ITEM_CODES]
    NORMAL_CELL = normal_cells[0] if normal_cells else 0x10
    SPECIAL_CELL = special_cells[0] if special_cells else None

    # Snapshot the clean post-boot machine so each scenario starts fresh.
    boot_ram = bytes(uc.mem_read(0, RAM))
    boot_ctx = uc.context_save()

    def restore_boot_state() -> None:
        uc.mem_write(0, boot_ram)
        uc.context_restore(boot_ctx)
        tr["fault"] = None
        tr["exit"] = None
        tr.pop("call_err", None)

    # ---------------------------------------------------------------------------
    # Scenario runners. Each returns (records, setup_kind, start_cell, note).
    # All seed the precondition then invoke the real engine fn(s).
    # ---------------------------------------------------------------------------
    scenario_blobs: List[dict] = []

    def run_collect(name: str, cell: int, force_code: Optional[int] = None) -> dict:
        restore_boot_state()
        cur_records.clear()
        # Seed: place player on the item cell. If force_code given, also set the
        # layer-C item byte (e.g. to plant a specific special/normal code).
        wr8(OFF_P1_CELL, cell & 0xFF)
        if force_code is not None:
            wr_tile((cell + 0x60) & 0xFFFF, force_code)
        pre_item = item_byte_at(cell)
        pre_items_rem = rd8(OFF_ITEMS_REMAINING)
        pre_score = (rd8(OFF_SCORE_LO) | (rd8(OFF_SCORE_HI) << 8)) if False else \
            (rd_u16(OFF_SCORE_LO) | (rd_u16(OFF_SCORE_HI) << 16))
        capturing["on"] = True
        # move_step_read_item runs the real read->collect chain (read_tile_layer2
        # sets p1_item_code from tilemap[cell+0x60], then calls p1_collect_item if !=0).
        call_engine_fn(0x6627)
        capturing["on"] = False
        post_score = rd_u16(OFF_SCORE_LO) | (rd_u16(OFF_SCORE_HI) << 16)
        return dict(name=name, records=list(cur_records), setup=SETUP_SEEDED,
                    start_cell=cell, note="seeded p1_cell=0x%02x item_code=0x%02x; "
                    "invoked move_step_read_item (real read+collect chain)" % (cell, pre_item),
                    pre_item=pre_item, pre_items_rem=pre_items_rem,
                    pre_score=pre_score, post_score=post_score)

    # Scenario 1: collect a NORMAL item.
    print("[items_oracle] === scenario 1 (collect_normal) cell=0x%02x ===" % NORMAL_CELL, flush=True)
    scenario_blobs.append(dict(id=1, **run_collect("collect_normal", NORMAL_CELL)))

    # Scenario 2: collect a SPECIAL item (code 0x01 or '#'=0x23). If level 1 has no
    # native special cell, seed code '#' onto a normal item cell (documented seeded).
    if SPECIAL_CELL is not None:
        s2_cell, s2_force = SPECIAL_CELL, None
        s2_note = "native special cell"
    else:
        s2_cell, s2_force = (normal_cells[1] if len(normal_cells) > 1 else NORMAL_CELL), 0x23
        s2_note = "seeded item_code='#' (0x23) onto a level cell (no native special on L1)"
    print("[items_oracle] === scenario 2 (collect_special) cell=0x%02x %s ===" % (
        s2_cell, s2_note), flush=True)
    blob2 = run_collect("collect_special", s2_cell, force_code=s2_force)
    blob2["note"] += " | " + s2_note
    scenario_blobs.append(dict(id=2, **blob2))

    # Scenario 3: collect the LAST item -> items_remaining 1->0, exit anim + flag.
    # Seed items_remaining=1, place player on a normal item cell, collect.
    print("[items_oracle] === scenario 3 (collect_last) ===", flush=True)
    restore_boot_state()
    cur_records.clear()
    last_cell = NORMAL_CELL
    wr8(OFF_P1_CELL, last_cell & 0xFF)
    wr8(OFF_ITEMS_REMAINING, 1)
    # ensure a collectible (non-special) item byte at the cell
    if item_byte_at(last_cell) in (0,) + SPECIAL_ITEM_CODES:
        wr_tile((last_cell + 0x60) & 0xFFFF, 0x22)   # 0x22='"' a normal item code
    s3_pre_exit = rd8(OFF_LEVEL_EXIT_CELL)
    capturing["on"] = True
    call_engine_fn(0x6627)
    capturing["on"] = False
    scenario_blobs.append(dict(
        id=3, name="collect_last", records=list(cur_records), setup=SETUP_SEEDED,
        start_cell=last_cell,
        note="seeded items_remaining=1 + normal item at p1_cell=0x%02x; invoked "
             "move_step_read_item; expect items_remaining->0, complete-flag set, "
             "anim_target_cell<-level_exit_cell(0x%02x)" % (last_cell, s3_pre_exit)))

    # Scenario 4: EXIT-TILE detection/read (check_exit_tile_vert / move_step_read_item).
    # Seed neighbor tile (+0x30) = exit code 0x0c and move_step_count != 7, then call
    # check_exit_tile_vert. Also re-exercise move_step_read_item on an EMPTY item cell
    # (no-collect path) to capture the read-only branch.
    print("[items_oracle] === scenario 4 (exit_detect) ===", flush=True)
    restore_boot_state()
    cur_records.clear()
    exit_probe_cell = NORMAL_CELL
    wr8(OFF_P1_CELL, exit_probe_cell & 0xFF)
    wr8(OFF_MOVE_STEP_COUNT, 0)            # != 7 so the branch is taken
    wr_tile((exit_probe_cell + 0x30) & 0xFFFF, TILE_EXIT_NEIGHBOR)  # 0x0c neighbor = exit
    capturing["on"] = True
    call_engine_fn(0x6372)                 # check_exit_tile_vert
    # read-only move_step_read_item on an empty cell (item byte 0 -> no collect)
    empty_cell = None
    for cand in range(0x30):
        if cand not in [c for c, _ in item_layout] and item_byte_at(cand) == 0:
            empty_cell = cand
            break
    if empty_cell is not None:
        wr8(OFF_P1_CELL, empty_cell & 0xFF)
        call_engine_fn(0x6627)             # move_step_read_item, no-collect branch
    capturing["on"] = False
    scenario_blobs.append(dict(
        id=4, name="exit_detect", records=list(cur_records), setup=SETUP_SEEDED,
        start_cell=exit_probe_cell,
        note="seeded tilemap[cell+0x30]=0x0c (exit neighbor) + move_step_count=0; "
             "invoked check_exit_tile_vert (expect physics_frozen=1, mode 0x2e enter); "
             "then move_step_read_item on empty cell 0x%s (read-only, no collect)"
             % ("%02x" % empty_cell if empty_cell is not None else "NA")))

    # Scenario 5: REACH EXIT WHEN COMPLETE -> teleport_to_next_exit_tile + advance.
    # Seed level_complete_flag=1, plant a teleport tile (0x0f) ahead, and place a
    # second exit tile to land on; invoke teleport_to_next_exit_tile (the exit-reached
    # action). The current_level++ / start_level(N) loop is wired in src/ (Task 4) and
    # is validated as a state transition there; here we capture the teleport semantics.
    print("[items_oracle] === scenario 5 (reach_exit_complete) ===", flush=True)
    restore_boot_state()
    cur_records.clear()
    tp_start = 0x05
    wr8(OFF_P1_CELL, tp_start & 0xFF)
    wr8(OFF_LEVEL_COMPLETE_FLAG, 1)
    # plant a teleport target tile (0x0f) a few cells ahead in the base layer
    tp_target = 0x0a
    wr_tile(tp_target & 0xFFFF, TILE_TELEPORT)
    s5_pre_level = rd8(OFF_CURRENT_LEVEL)
    capturing["on"] = True
    call_engine_fn(0x25ad)                 # teleport_to_next_exit_tile
    capturing["on"] = False
    s5_post_cell = rd8(OFF_P1_CELL)
    scenario_blobs.append(dict(
        id=5, name="reach_exit_complete", records=list(cur_records), setup=SETUP_SEEDED,
        start_cell=tp_start,
        note="seeded level_complete_flag=1 + teleport tile 0x0f at cell 0x%02x; invoked "
             "teleport_to_next_exit_tile (scans forward, moves p1_cell 0x%02x->0x%02x, "
             "enters mode 0x0f). Level-advance (current_level++ / start_level) wired in "
             "src/game.c Task-4 + validated there as a state transition; current_level "
             "stays %d here." % (tp_target, tp_start, s5_post_cell, s5_pre_level)))

    if tr.get("call_err"):
        print("[items_oracle] NOTE call_err during a scenario: %s" % tr.get("call_err"), flush=True)

    # ---------------------------------------------------------------------------
    # Write the frozen trace
    # ---------------------------------------------------------------------------
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT_TRACE, "wb") as f:
        f.write(TRACE_MAGIC)
        f.write(struct.pack("<HH", TRACE_VERSION, len(scenario_blobs)))
        f.write(struct.pack("<H", len(fn_name_list)))
        for nm in fn_name_list:
            b = nm.encode("ascii")
            f.write(struct.pack("<B", len(b))); f.write(b)
        for sb in scenario_blobs:
            nb = sb["name"].encode("ascii")
            f.write(struct.pack("<B", sb["id"]))
            f.write(struct.pack("<B", len(nb))); f.write(nb)
            f.write(struct.pack("<B", sb["setup"]))
            f.write(struct.pack("<B", LEVEL))
            f.write(struct.pack("<B", sb["start_cell"] & 0xFF))
            f.write(struct.pack("<I", len(sb["records"])))
            for r in sb["records"]:
                f.write(r)
    trace_bytes = os.path.getsize(OUT_TRACE)
    print("[items_oracle] wrote %s (%d B)" % (OUT_TRACE, trace_bytes), flush=True)

    # ---------------------------------------------------------------------------
    # Decode + write items_model.md
    # ---------------------------------------------------------------------------
    def decode(recs: List[bytes]) -> List[dict]:
        out = []
        for r in recs:
            fn_off, name_idx = struct.unpack_from("<HH", r, 0)
            o = 4
            ent = struct.unpack_from(SNAP_FMT, r, o); o += SNAP_SIZE
            ex = struct.unpack_from(SNAP_FMT, r, o); o += SNAP_SIZE
            out.append(dict(fn=FN_NAMES[fn_off], fn_off=fn_off,
                            ent=dict(zip(SNAP_FIELDS, ent)),
                            ex=dict(zip(SNAP_FIELDS, ex))))
        return out

    ADDR_TABLE = [
        ("score_lo", 0xa0d4, "u16"), ("score_hi", 0xa0d6, "u16"),
        ("items_remaining", 0xa0cf, "u8"), ("level_exit_cell", 0x8572, "u8"),
        ("level_complete_flag", 0xa1b1, "u8"),
        ("level_complete_anim_counter", 0x8550, "u8"),
        ("p1_item_code", 0x79b8, "u8"), ("p1_cell", 0x856e, "u8"),
        ("anim_target_cell", 0x856f, "u8"), ("current_level", 0x79b2, "u8"),
        ("move_step_count", 0x855e, "u8"), ("physics_frozen", 0xa0ce, "u8"),
        ("p1_pixel_y", 0x9292, "s16"), ("copyprotect_flag", 0x119a, "s8"),
        ("p1_move_step_idx", 0x792a, "u8"), ("sound_mode_selector", 0x689c, "u16"),
        ("tilemap_ptr (off/seg)", 0xa0d8, "far ptr; item byte = tilemap[cell+0x60]"),
    ]

    lines: List[str] = []
    lines.append("# Bumpy Phase-3 item/exit semantic-state capture model (discovery)\n\n")
    lines.append("Generated by `tools/items_oracle.py`. Capture granularity = item/exit "
                 "FUNCTION-CALL boundary (entry + exit). Validates DISCRETE semantic state "
                 "(score / items_remaining / exit-cell / complete-flag) — no trajectory desync.\n\n")

    lines.append("## Resolved semantic-state DGROUP addresses (Ghidra DGROUP 0x203b offsets)\n\n")
    lines.append("| name | DGROUP off | type |\n|---|---|---|\n")
    for nm, off, ty in ADDR_TABLE:
        lines.append("| %s | 0x%04x | %s |\n" % (nm, off, ty))
    lines.append("\nNOTE: the brief listed `current_level (0x8f40)`; the real `current_level` "
                 "in this build is **0x79b2** (start_level @1000:2d14 reads/writes [0x79b2]; "
                 "matches physics_oracle `OFF_CURRENT_LEVEL`). 0x8f40 is not current_level here. "
                 "`[0x689c]` (==4 selects alternate sound ids) is a mode/state selector, NOT "
                 "current_level.\n\n")

    lines.append("## Per-function recovered effects (from the disassembly + capture)\n\n")
    lines.append("- **p1_collect_item (1000:6c14)**: calls p1_collect_item_score (award); "
                 "`tilemap[p1_cell+0x60]=0` (clear item); if p1_item_code not in {0x01,'#'} "
                 "then `items_remaining--`; when it hits 0: play complete sound, "
                 "`anim_target_cell=level_exit_cell`, `apply_cell_animation('Y')`, "
                 "`level_complete_flag=1`, `level_complete_anim_counter=0xf2`.\n")
    lines.append("- **p1_collect_item_score (1000:6c95)**: queue erase of player cell view; "
                 "32-bit score (score_hi:score_lo) += 250 base; '#'→+250 & inc settle_countdown; "
                 "'/'→+10000; '0'→+50000; else +250.\n")
    lines.append("- **check_exit_tile_vert (1000:6372)**: if move_step_count!=7 AND "
                 "tilemap[p1_cell+0x30]==0x0c (exit): p1_move_step_idx=0, physics_frozen=1, "
                 "enter_game_mode(0x2e), play exit sound. Else no-op.\n")
    lines.append("- **move_step_read_item (1000:6627)**: read_tile_layer2(p1_cell) sets "
                 "p1_item_code=tilemap[p1_cell+0x60]; if !=0 call p1_collect_item.\n")
    lines.append("- **teleport_to_next_exit_tile (1000:25ad)**: scan tilemap forward from "
                 "p1_cell (wrap at 0x30) for tile 0x0f; on hit: anim_target_cell=scan_cell, "
                 "p1_cell=scan_cell, p1_set_pixel_from_cell, p1_pixel_y+=0xd, "
                 "apply_cell_animation(0x27), play sound, enter_game_mode(0x0f), "
                 "dispatch_move_step.\n\n")

    lines.append("## Discovered level-1 item layout (live tilemap layer-C, cell+0x60)\n\n")
    lines.append("level_exit_cell=0x%02x, items_remaining(boot)=%d\n\n" % (
        boot_exit_cell, boot_items_remaining))
    lines.append("| cell | item_code | char | kind |\n|---|---|---|---|\n")
    for cell, code in item_layout:
        kind = "SPECIAL (no decrement)" if code in SPECIAL_ITEM_CODES else "normal"
        ch = chr(code) if 32 <= code < 127 else "."
        lines.append("| 0x%02x | 0x%02x | %s | %s |\n" % (cell, code, ch, kind))
    lines.append("\n")

    lines.append("## Trace layout (`items_trace.bin`, magic `ITEMTRC1`, v1)\n\n")
    lines.append("Header: magic[8], u16 version, u16 n_scenarios, u16 n_fn_names, then "
                 "per name {u8 len, bytes}. Per scenario: u8 id, u8 name_len, name, "
                 "u8 setup_kind (0=scripted 1=seeded), u8 level, u8 start_cell, u32 "
                 "n_records, then records. Per record: u16 fn_addr, u16 fn_name_idx, "
                 "SNAP entry, SNAP exit. SNAP (`%s`, %d B): %s.\n\n" % (
                     SNAP_FMT, SNAP_SIZE, ", ".join(SNAP_FIELDS[:-1])))

    lines.append("## Scenarios + semantic sanity\n\n")
    for sb in scenario_blobs:
        dec = decode(sb["records"])
        setup = "seeded" if sb["setup"] == SETUP_SEEDED else "scripted"
        lines.append("### Scenario %d — %s (%s)\n\n" % (sb["id"], sb["name"], setup))
        lines.append("- setup: %s\n" % sb["note"])
        lines.append("- records: %d\n\n" % len(dec))
        if dec:
            lines.append("| # | fn | score e->x | items_rem e->x | item_code e->x | "
                         "complete e->x | item_byte e->x | cell e->x |\n")
            lines.append("|---|---|---|---|---|---|---|---|\n")
            for i, d in enumerate(dec):
                e, x = d["ent"], d["ex"]
                se = e["score_lo"] | (e["score_hi"] << 16)
                sx = x["score_lo"] | (x["score_hi"] << 16)
                lines.append("| %d | %s | %d->%d | %d->%d | 0x%02x->0x%02x | %d->%d | "
                             "0x%02x->0x%02x | 0x%02x->0x%02x |\n" % (
                                 i, d["fn"], se, sx,
                                 e["items_remaining"], x["items_remaining"],
                                 e["p1_item_code"], x["p1_item_code"],
                                 e["level_complete_flag"], x["level_complete_flag"],
                                 e["tilemap_item_byte"], x["tilemap_item_byte"],
                                 e["p1_cell"], x["p1_cell"]))
            lines.append("\n")
        # sanity verdicts
        verdicts = scenario_sanity(sb["id"], dec)
        lines.append("- **sanity**: %s\n\n" % verdicts)

    os.makedirs(os.path.dirname(OUT_MODEL), exist_ok=True)
    with open(OUT_MODEL, "w") as f:
        f.write("".join(lines))
    print("[items_oracle] wrote %s" % OUT_MODEL, flush=True)

    # ---------------------------------------------------------------------------
    # Console semantic-sanity summary
    # ---------------------------------------------------------------------------
    print("\n[items_oracle] SEMANTIC SANITY:", flush=True)
    for sb in scenario_blobs:
        dec = decode(sb["records"])
        print("  scenario %d (%s): %s" % (
            sb["id"], sb["name"], scenario_sanity(sb["id"], dec)), flush=True)
    print("\n[items_oracle] RESOLVED ADDRESSES:", flush=True)
    for nm, off, ty in ADDR_TABLE:
        print("   %-28s 0x%04x  %s" % (nm, off, ty), flush=True)
    if err:
        print("[items_oracle] emu error:", err, flush=True)


def scenario_sanity(sid: int, dec: List[dict]) -> str:
    """Return a human verdict string for a scenario's semantic sanity."""
    if not dec:
        return "NO RECORDS"
    # Find the p1_collect_item record (the one with the score/items effect) if any.
    collect = next((d for d in dec if d["fn"] == "p1_collect_item"), None)
    checks: List[str] = []
    if sid == 1:  # normal: score up, items down, item byte cleared
        if collect:
            e, x = collect["ent"], collect["ex"]
            se = e["score_lo"] | (e["score_hi"] << 16)
            sx = x["score_lo"] | (x["score_hi"] << 16)
            checks.append("score_up=%s" % (sx > se))
            checks.append("items_down=%s" % (x["items_remaining"] == e["items_remaining"] - 1))
            checks.append("item_cleared=%s" % (x["tilemap_item_byte"] == 0))
        else:
            checks.append("collect MISSING")
    elif sid == 2:  # special: score up, NO decrement
        if collect:
            e, x = collect["ent"], collect["ex"]
            se = e["score_lo"] | (e["score_hi"] << 16)
            sx = x["score_lo"] | (x["score_hi"] << 16)
            checks.append("score_up=%s" % (sx > se))
            checks.append("no_decrement=%s" % (x["items_remaining"] == e["items_remaining"]))
            checks.append("special_code=%s" % (e["p1_item_code"] in (0x01, 0x23)))
        else:
            checks.append("collect MISSING")
    elif sid == 3:  # last: items->0, complete flag set
        if collect:
            e, x = collect["ent"], collect["ex"]
            checks.append("items_to_0=%s" % (x["items_remaining"] == 0))
            checks.append("complete_flag_set=%s" % (e["level_complete_flag"] == 0
                                                    and x["level_complete_flag"] == 1))
            checks.append("anim_ctr=0x%02x" % x["level_complete_anim_counter"])
            checks.append("anim_target=exit(%s)" % (
                x["anim_target_cell"] == x["level_exit_cell"]))
        else:
            checks.append("collect MISSING")
    elif sid == 4:  # exit detect: physics_frozen set by check_exit_tile_vert
        cev = next((d for d in dec if d["fn"] == "check_exit_tile_vert"), None)
        if cev:
            e, x = cev["ent"], cev["ex"]
            checks.append("physics_frozen_set=%s" % (x["physics_frozen"] == 1))
        else:
            checks.append("check_exit MISSING")
        mri = next((d for d in dec if d["fn"] == "move_step_read_item"), None)
        if mri:
            checks.append("read_item_present=True")
    elif sid == 5:  # teleport: p1_cell moves to teleport tile, pixel_y nudged
        tp = next((d for d in dec if d["fn"] == "teleport_to_next_exit_tile"), None)
        if tp:
            e, x = tp["ent"], tp["ex"]
            checks.append("cell_moved=%s" % (x["p1_cell"] != e["p1_cell"]))
            checks.append("anim_target=cell(%s)" % (x["anim_target_cell"] == x["p1_cell"]))
            checks.append("complete_flag=%d" % x["level_complete_flag"])
        else:
            checks.append("teleport MISSING")
    return ", ".join(checks)


if __name__ == "__main__":
    main()
