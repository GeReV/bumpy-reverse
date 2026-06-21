#!/usr/bin/env python3
"""p1_spine_oracle.py — Phase-9 T3 Player-1 per-tick SPINE CAPTURE-AS-DISCOVERY.

Boots the real (unpacked) BUMPY.EXE under Unicorn — reusing the boot + DGROUP + int /
VGA scaffold of tools/p2_oracle.py (deliberately NOT refactoring it) — drives a set of
seeded P1 scenarios and captures, at the ENTRY and EXIT of each hooked P1 per-tick
function, the P1 semantic state (grid cell / bbox scalars + the all_entries predicate)
and, for the render fns, the DESCRIPTOR bytes the engine wrote into the view / p1_sprite
struct.

This is the symmetric P1 analog of tools/p2_oracle.py: P1 is normally key-driven, but —
exactly like the P2 oracle — every scenario SEEDS the relevant preconditions (pixel /
grid / scroll / bbox / pending-erase / level-complete state) and then INVOKES the REAL
engine fn at its entry IP via a synthetic near-call frame.  The function body that runs
is the unmodified original code; only the precondition state is seeded.

Hooked P1 functions (Ghidra seg-1000 off -> runtime linear 0x1100+off):
  p1_update_grid_cell        1000:1473  (p1 pixel -> grid col/row, clamped)
  p1_advance_grid_history    1000:138c  (grid cur->prev, new->cur)
  render_p1_view             1000:1bd7  (view scroll + render descriptor build)
  erase_p1_view              1000:19e4  (prev-cell erase descriptor build)
  update_p1_bbox             1000:5085  (p1 pixel -> AABB)
  draw_p1_sprite             1000:1cb2  (build p1_sprite obj descriptor + blit)
  restore_bg_pending         1000:1a20  (deferred bg-restore: decrement + descriptor)
  all_entries_flag_set       1000:3e8a  (level-complete predicate -> u8)
  init_view_anim_descriptors 1000:535e  (one-time view/anim descriptor struct-init)

Outputs (BOTH gitignored — discovery; NO commit):
  local/build/render/p1_spine_trace.bin   (magic P1SPINE1; layout in TRACE LAYOUT below)
  local/build/p1_spine_model.md           (per-scenario fn-call sequence + addrs)

TRACE LAYOUT (little-endian) — FROZEN; tools/p1_spine_ctest.c parses this exactly:
  Header:
    +0x00  8 B   magic   b"P1SPINE1"
    +0x08  2 B   u16     version (=1)
    +0x0A  2 B   u16     n_scenarios
  fn-name string table:
    2 B   u16     n_fn_names
    per name: u8 len, len bytes (ascii)
  Then, per scenario:
    u8        scenario_id
    u8        name_len,  name_len bytes (ascii)
    u8        seeded     (1)
    u8        level
    u32       n_records
    then n_records records.

  Per RECORD (one P1-function call; carries BOTH entry and exit snapshots):
    u16   fn_addr        (Ghidra seg-1000 offset, e.g. 0x1473)
    u16   fn_name_idx    (index into the fn-name string table)
    SNAP  entry          (P1SNAP_SIZE-byte fixed struct, see SNAP below)
    SNAP  exit           (P1SNAP_SIZE-byte fixed struct)
    u8    p1view_len     (# bytes of the p1_view descriptor at EXIT; render_p1_view)
    p1view_len bytes
    u8    p1erase_len    (# bytes of the p1_erase_view descriptor at EXIT; erase_p1_view)
    p1erase_len bytes
    u8    pend_len       (# bytes of the pending_erase_view descriptor at EXIT)
    pend_len bytes
    u8    obj_len        (# bytes of the p1_sprite obj descriptor at EXIT; draw_p1_sprite)
    obj_len bytes
    u8    init_len       (# bytes of the multi-view init blob at EXIT; init_view_anim)
    init_len bytes

  P1SNAP (the P1 spine globals; little-endian) — fixed P1SNAP_FMT:
    s16  p1_pixel_x        (0x9290)
    s16  p1_pixel_y        (0x9292)
    s16  p1_grid_x_new     (0x9d36)
    s16  p1_grid_y_new     (0x9d38)
    s16  p1_grid_x         (0x857a)
    s16  p1_grid_y         (0x857c)
    s16  p1_grid_x_prev    (0x8882)
    s16  p1_grid_y_prev    (0x8e88)
    s16  p1_scroll_x       (0x9ba4)
    s16  p1_scroll_y       (0x9b9c)
    s16  p1_bbox_left      (0x84c)
    s16  p1_bbox_right     (0x84e)
    s16  p1_bbox_top       (0x850)
    s16  p1_bbox_bottom    (0x852)
    s16  pending_erase_x   (0x9b9a)
    s16  pending_erase_y   (0x9ba2)
    u8   p1_move_anim      (0x824a)
    u8   physics_frozen    (0xa0ce)
    u8   pending_erase_count (0xa1a8)
    u8   all_entries_flag  (the AL return of all_entries_flag_set; capture-only)
    u8   level_complete_flag (0xa1b1)
    u8   pad

Run (sandbox disabled — needs unicorn/uv cache access), HARD timeout:
  timeout 1800 uv run python tools/p1_spine_oracle.py
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
OUT_TRACE = os.path.join(OUT_DIR, "p1_spine_trace.bin")
OUT_MODEL = os.path.join(ROOT, "local/build/p1_spine_model.md")

PSP_SEG = 0x0100
RAM = 0x110000

DG_LIN: int = (0x103b + PSP_SEG + 0x10) * 16   # 0x114b0
CODE_LIN: int = (PSP_SEG + 0x10) * 16          # 0x1100

# ---------------------------------------------------------------------------
# Resolved P1 spine DGROUP global offsets (Ghidra DGROUP 0x203b offsets; read live
# from the disassembly operands of the P1 fns).
# ---------------------------------------------------------------------------
OFF_P1_PIXEL_X = 0x9290
OFF_P1_PIXEL_Y = 0x9292
OFF_P1_GRID_X_NEW = 0x9d36
OFF_P1_GRID_Y_NEW = 0x9d38
OFF_P1_GRID_X = 0x857a
OFF_P1_GRID_Y = 0x857c
OFF_P1_GRID_X_PREV = 0x8882
OFF_P1_GRID_Y_PREV = 0x8e88
OFF_P1_SCROLL_X = 0x9ba4
OFF_P1_SCROLL_Y = 0x9b9c
OFF_P1_BBOX_L = 0x84c
OFF_P1_BBOX_R = 0x84e
OFF_P1_BBOX_T = 0x850
OFF_P1_BBOX_B = 0x852
OFF_PEND_X = 0x9b9a
OFF_PEND_Y = 0x9ba2
OFF_P1_MOVE_ANIM = 0x824a            # u8 (read as word in draw_p1_sprite hidden gate)
OFF_PHYSICS_FROZEN = 0xa0ce
OFF_PEND_COUNT = 0xa1a8
OFF_LEVEL_COMPLETE_FLAG = 0xa1b1

# far pointers (off,seg)
OFF_P1_OBJ_PTR = 0x8884              # p1_sprite
OFF_P1_VIEW_PTR = 0x8b8             # p1_view (render)
OFF_P1_ERASE_PTR = 0x8c4           # p1_erase_view
OFF_PEND_VIEW_PTR = 0x8e4          # pending_erase_view
OFF_MOVE_DESC_PTR = 0x8246         # move_descriptor_table

# init_view_anim_descriptors view far-ptrs (the full set it initialises).
INIT_VIEW_PTRS = [
    ("render_descriptor_ptr", 0x574),
    ("p1_view", 0x8b8), ("p2_view", 0x8ec),
    ("anim_b_clear_view", 0x8bc), ("anim_a_clear_view", 0x8c0),
    ("p1_erase_view", 0x8c4), ("p2_erase_view", 0x8e8),
    ("p2_anim_clear_8c8", 0x8c8), ("p2_anim_clear_8cc", 0x8cc),
    ("anim_b_draw_view", 0x8d0), ("anim_a_erase_view", 0x8d4),
    ("p2_anim_erase_8d8", 0x8d8), ("p2_anim_erase_8dc", 0x8dc),
    ("anim_a_draw_view", 0x8e0), ("pending_erase_view", 0x8e4),
]

OFF_CURRENT_LEVEL = 0x79b2
OFF_COPYPROTECT = 0x119a
OFF_KEY_STATE_PTR = 0x4D42

DESC_CAP = 0x22   # capture 0x22 descriptor bytes (covers the +0x20 fields)

# ---------------------------------------------------------------------------
# Hooked P1 functions (Ghidra seg-1000 offsets)
# ---------------------------------------------------------------------------
FN_NAMES: Dict[int, str] = {
    0x1473: "p1_update_grid_cell",
    0x138c: "p1_advance_grid_history",
    0x1bd7: "render_p1_view",
    0x19e4: "erase_p1_view",
    0x5085: "update_p1_bbox",
    0x1cb2: "draw_p1_sprite",
    0x1a20: "restore_bg_pending",
    0x3e8a: "all_entries_flag_set",
    0x535e: "init_view_anim_descriptors",
}
FN_RENDER_VIEW = 0x1bd7
FN_ERASE_VIEW = 0x19e4
FN_PEND = 0x1a20
FN_DRAW = 0x1cb2
FN_INIT = 0x535e
FN_ALL_ENTRIES = 0x3e8a

TRACE_MAGIC = b"P1SPINE1"
TRACE_VERSION = 1
# 16 s16 + 6 u8 = 32 + 6 = 38 bytes.
P1SNAP_FMT = "<" + "h" * 16 + "B" * 6
P1SNAP_SIZE = struct.calcsize(P1SNAP_FMT)
SNAP_FIELDS = ["px", "py", "gxn", "gyn", "gx", "gy", "gxp", "gyp",
               "sx", "sy", "bl", "br", "bt", "bb", "pex", "pey",
               "anim", "frozen", "pcount", "all_set", "lcf", "pad"]
assert len(SNAP_FIELDS) == 22


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
            return 0
        if port == 0x61:
            return 0xFF
        return 0xFF

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

    def rd8(off: int) -> int:
        return uc.mem_read(DG_LIN + off, 1)[0]

    def rd_s16(off: int) -> int:
        return struct.unpack("<h", bytes(uc.mem_read(DG_LIN + off, 2)))[0]

    def wr8(off: int, v: int) -> None:
        uc.mem_write(DG_LIN + off, bytes([v & 0xFF]))

    def wr16(off: int, v: int) -> None:
        uc.mem_write(DG_LIN + off, struct.pack("<H", v & 0xFFFF))

    last_all_set = [0]

    def snap() -> bytes:
        return struct.pack(
            P1SNAP_FMT,
            rd_s16(OFF_P1_PIXEL_X), rd_s16(OFF_P1_PIXEL_Y),
            rd_s16(OFF_P1_GRID_X_NEW), rd_s16(OFF_P1_GRID_Y_NEW),
            rd_s16(OFF_P1_GRID_X), rd_s16(OFF_P1_GRID_Y),
            rd_s16(OFF_P1_GRID_X_PREV), rd_s16(OFF_P1_GRID_Y_PREV),
            rd_s16(OFF_P1_SCROLL_X), rd_s16(OFF_P1_SCROLL_Y),
            rd_s16(OFF_P1_BBOX_L), rd_s16(OFF_P1_BBOX_R),
            rd_s16(OFF_P1_BBOX_T), rd_s16(OFF_P1_BBOX_B),
            rd_s16(OFF_PEND_X), rd_s16(OFF_PEND_Y),
            rd8(OFF_P1_MOVE_ANIM), rd8(OFF_PHYSICS_FROZEN), rd8(OFF_PEND_COUNT),
            last_all_set[0] & 0xFF, rd8(OFF_LEVEL_COMPLETE_FLAG), 0)

    def read_far_desc(ptr_off: int, n: int = DESC_CAP) -> bytes:
        off, seg = struct.unpack("<HH", bytes(uc.mem_read(DG_LIN + ptr_off, 4)))
        lin = (seg * 16 + off) & 0xFFFFF
        try:
            return bytes(uc.mem_read(lin, n))
        except UcError:
            return b""

    def read_init_blob() -> bytes:
        """Concat the EXIT bytes of every view init_view_anim_descriptors writes."""
        out = b""
        for _name, ptr in INIT_VIEW_PTRS:
            out += read_far_desc(ptr, DESC_CAP)
        return out

    # -------------------------------------------------------------------------
    # P1-function hooks (entry + exit via dynamic return-address hook).
    # all_entries_flag_set returns AL — capture it at the exit hook.
    # -------------------------------------------------------------------------
    capturing = {"on": False}
    cur_records: List[bytes] = []
    reached_fns: collections.Counter = collections.Counter()
    pending_exit: dict = {}
    exit_hook_lins: set = set()
    fn_name_list: List[str] = list(dict.fromkeys(FN_NAMES.values()))
    fn_name_idx = {n: i for i, n in enumerate(fn_name_list)}

    def emit_record(fn_off: int, entry_snap: bytes, exit_snap: bytes,
                    p1view: bytes, p1erase: bytes, pend: bytes,
                    obj: bytes, init_blob: bytes) -> None:
        rec = struct.pack("<HH", fn_off, fn_name_idx[FN_NAMES[fn_off]])
        rec += entry_snap + exit_snap
        for blob in (p1view, p1erase, pend, obj):
            rec += struct.pack("<B", len(blob)) + blob
        rec += struct.pack("<H", len(init_blob)) + init_blob
        cur_records.append(rec)

    def hook_fn_entry(uc: Uc, addr: int, size: int, _: object) -> None:
        if not capturing["on"]:
            return
        fn_off = (addr - CODE_LIN) & 0xFFFF
        if fn_off not in FN_NAMES:
            return
        reached_fns[fn_off] += 1
        if fn_off == FN_ALL_ENTRIES:
            last_all_set[0] = 0   # not yet known at entry
        entry_snap = snap()
        ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc.mem_read(ss * 16 + sp, 2)))[0]
        ret_lin = (CODE_LIN + ret_off) & 0xFFFFF
        pending_exit.setdefault(ret_lin, []).append((fn_off, entry_snap))
        if ret_lin not in exit_hook_lins:
            exit_hook_lins.add(ret_lin)
            uc.hook_add(UC_HOOK_CODE, hook_fn_exit, None, ret_lin, ret_lin)

    def hook_fn_exit(uc: Uc, addr: int, size: int, _: object) -> None:
        stack = pending_exit.get(addr)
        if not stack:
            return
        (fn_off, entry_snap) = stack.pop()
        if fn_off == FN_ALL_ENTRIES:
            last_all_set[0] = uc.reg_read(UC_X86_REG_AX) & 0xFF
        exit_snap = snap()
        p1view = read_far_desc(OFF_P1_VIEW_PTR) if fn_off == FN_RENDER_VIEW else b""
        p1erase = read_far_desc(OFF_P1_ERASE_PTR) if fn_off == FN_ERASE_VIEW else b""
        pend = read_far_desc(OFF_PEND_VIEW_PTR) if fn_off == FN_PEND else b""
        # Capture the p1_sprite obj for draw (the descriptor gate) AND for
        # p1_update_grid_cell (which READS the sprite origin words at +0x14/+0x16 — the
        # ctest must seed that origin to reproduce the grid math).
        obj = (read_far_desc(OFF_P1_OBJ_PTR)
               if fn_off in (FN_DRAW, 0x1473) else b"")
        init_blob = read_init_blob() if fn_off == FN_INIT else b""
        emit_record(fn_off, entry_snap, exit_snap, p1view, p1erase, pend, obj, init_blob)

    for off in FN_NAMES:
        lin = CODE_LIN + off
        uc.hook_add(UC_HOOK_CODE, hook_fn_entry, None, lin, lin)

    STOP_OFF = 0x0008

    def call_engine_fn(fn_off: int, arg_word: Optional[int] = None) -> None:
        uc.mem_write(CODE_LIN + STOP_OFF, b"\xF4")
        ss = uc.reg_read(UC_X86_REG_SS) & 0xFFFF
        sp = uc.reg_read(UC_X86_REG_SP) & 0xFFFF
        if arg_word is not None:
            sp = (sp - 2) & 0xFFFF
            uc.mem_write(ss * 16 + sp, struct.pack("<H", arg_word & 0xFFFF))
        sp = (sp - 2) & 0xFFFF
        uc.mem_write(ss * 16 + sp, struct.pack("<H", STOP_OFF))
        uc.reg_write(UC_X86_REG_SP, sp)

        def stop_hook(u: Uc, a: int, s: int, _: object) -> None:
            u.emu_stop()
        h = uc.hook_add(UC_HOOK_CODE, stop_hook, None,
                        CODE_LIN + STOP_OFF, CODE_LIN + STOP_OFF)
        try:
            uc.emu_start(CODE_LIN + fn_off, 0, count=20_000_000)
        except UcError as e:
            tr["call_err"] = str(e)
        finally:
            uc.hook_del(h)
            if arg_word is not None:
                sp2 = uc.reg_read(UC_X86_REG_SP) & 0xFFFF
                uc.reg_write(UC_X86_REG_SP, (sp2 + 2) & 0xFFFF)

    # ---------------------------------------------------------------------------
    # Boot to level 1
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

    print("[p1_spine_oracle] booting BUMPY (level %d)..." % LEVEL, flush=True)

    while total_instr < 400_000_000:
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e); tr["err"] = err; break
        total_instr += CHUNK
        if total_instr % 40_000_000 == 0:
            print("[p1_spine_oracle] %dM instr, countdown=%s" % (
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
            print("[p1_spine_oracle] level loaded (%s) at chunk %d — settling %d ticks" % (
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
        print("[p1_spine_oracle] ERROR: premature exit/fault during boot: exit=%s fault=%s err=%s" % (
            tr.get("exit"), tr.get("fault"), err), flush=True)
        return
    if not opened(BUMNAME):
        print("[p1_spine_oracle] ERROR: level %s never loaded after %dM instructions" % (
            BUMNAME, total_instr // 1_000_000), flush=True)
        return

    print("[p1_spine_oracle] boot complete. Files: %s" % (
        [o[1] for o in tr.get("fileops", [])]), flush=True)
    print("[p1_spine_oracle] boot p1_pixel=(%d,%d) move_anim=%d frozen=%d "
          "p1_view_ptr=%04x:%04x" % (
              rd_s16(OFF_P1_PIXEL_X), rd_s16(OFF_P1_PIXEL_Y), rd8(OFF_P1_MOVE_ANIM),
              rd8(OFF_PHYSICS_FROZEN),
              *struct.unpack("<HH", bytes(uc.mem_read(DG_LIN + OFF_P1_VIEW_PTR, 4)))[::-1]),
          flush=True)

    boot_ram = bytes(uc.mem_read(0, RAM))
    boot_ctx = uc.context_save()

    def restore_boot_state() -> None:
        uc.mem_write(0, boot_ram)
        uc.context_restore(boot_ctx)
        tr["fault"] = None
        tr["exit"] = None

    scenario_results: List[dict] = []

    def run_one(sc_id: int, name: str, note: str, body) -> None:
        restore_boot_state()
        cur_records.clear()
        capturing["on"] = True
        pre = {}
        try:
            body(pre)
        except UcError as e:
            tr["scn_err"] = str(e)
        capturing["on"] = False
        recs = list(cur_records)
        scenario_results.append(dict(id=sc_id, name=name, note=note, recs=recs, pre=pre))
        print("[p1_spine_oracle] === scenario %d (%s): %d records ===" % (
            sc_id, name, len(recs)), flush=True)

    # --- Scenario 0: init_view_anim_descriptors (one-time struct-init) ---------------
    # NOTE: call init TWICE.  The exit-capture hook is installed at the synthetic
    # return address (STOP_OFF) on the FIRST call (during the entry hook), but the
    # per-call stop_hook is registered before emu_start and fires first on that first
    # return — so the FIRST call does not emit.  The SECOND call reuses the now-present
    # exit hook and records.  init is idempotent (pure struct-init), so the second
    # call's EXIT state is identical to the first's.
    def scn_init(pre):
        call_engine_fn(0x535e)
        call_engine_fn(0x535e)
        pre["done"] = 1
    run_one(0, "init_view_anim_descriptors",
            "Invoke init_view_anim_descriptors -> capture the EXIT descriptor bytes of "
            "all 15 view structs it initialises.",
            scn_init)

    # --- Scenario 1: grid-cell + grid-history from a seeded pixel pos -----------------
    def make_grid(px, py):
        def body(pre):
            # Ensure init has run so the view/sprite ptrs are populated.
            call_engine_fn(0x535e)
            wr16(OFF_P1_PIXEL_X, px); wr16(OFF_P1_PIXEL_Y, py)
            pre["px"] = px; pre["py"] = py
            call_engine_fn(0x1473)   # p1_update_grid_cell
            call_engine_fn(0x138c)   # p1_advance_grid_history
            pre["gxn"] = rd_s16(OFF_P1_GRID_X_NEW); pre["gyn"] = rd_s16(OFF_P1_GRID_Y_NEW)
        return body
    run_one(1, "p1_grid_midfield", "Seed mid-field pixel; update_grid_cell + advance.",
            make_grid(120, 90))
    run_one(2, "p1_grid_topleft", "Seed top-left pixel (clamps to 0,0).",
            make_grid(8, 8))
    run_one(3, "p1_grid_botright", "Seed bottom-right pixel (clamps to 0x12/0x16).",
            make_grid(400, 300))

    # --- Scenario 4: render_p1_view (scroll + descriptor) ----------------------------
    def make_render(gx, gy):
        def body(pre):
            call_engine_fn(0x535e)
            wr16(OFF_P1_GRID_X, gx); wr16(OFF_P1_GRID_Y, gy)
            pre["gx"] = gx; pre["gy"] = gy
            call_engine_fn(0x1bd7)   # render_p1_view
            pre["sx"] = rd_s16(OFF_P1_SCROLL_X); pre["sy"] = rd_s16(OFF_P1_SCROLL_Y)
        return body
    run_one(4, "p1_render_default", "grid (8,8) -> default scroll 4/4.", make_render(8, 8))
    run_one(5, "p1_render_edge", "grid (0x12,0x16) -> edge scroll (0x14-gx / 0x19-gy).",
            make_render(0x12, 0x16))

    # --- Scenario 6: erase_p1_view (prev-cell descriptor) ----------------------------
    def scn_erase(pre):
        call_engine_fn(0x535e)
        wr16(OFF_P1_GRID_X, 0x12); wr16(OFF_P1_GRID_Y, 0x16)
        call_engine_fn(0x1bd7)               # set the scroll first (render)
        wr16(OFF_P1_GRID_X_PREV, 5); wr16(OFF_P1_GRID_Y_PREV, 7)
        pre["gxp"] = 5; pre["gyp"] = 7
        call_engine_fn(0x19e4)               # erase_p1_view
    run_one(6, "p1_erase", "Seed prev-cell (5,7) + scroll; erase_p1_view descriptor.",
            scn_erase)

    # --- Scenario 7: update_p1_bbox --------------------------------------------------
    def make_bbox(px, py, frozen):
        def body(pre):
            wr16(OFF_P1_PIXEL_X, px); wr16(OFF_P1_PIXEL_Y, py)
            wr8(OFF_PHYSICS_FROZEN, frozen)
            pre["px"] = px; pre["py"] = py; pre["frozen"] = frozen
            call_engine_fn(0x5085)   # update_p1_bbox
            pre["bl"] = rd_s16(OFF_P1_BBOX_L); pre["br"] = rd_s16(OFF_P1_BBOX_R)
        return body
    run_one(7, "p1_bbox", "Seed pixel (100,80), not frozen; update_p1_bbox.",
            make_bbox(100, 80, 0))
    run_one(8, "p1_bbox_frozen", "Frozen -> bbox unchanged (gate fires).",
            make_bbox(100, 80, 1))

    # --- Scenario 9: draw_p1_sprite (object descriptor) ------------------------------
    def make_draw(anim):
        def body(pre):
            call_engine_fn(0x535e)
            wr16(OFF_P1_PIXEL_X, 123); wr16(OFF_P1_PIXEL_Y, 77)
            wr8(OFF_P1_MOVE_ANIM, anim)
            pre["anim"] = anim
            call_engine_fn(0x1cb2)   # draw_p1_sprite
            d = read_far_desc(OFF_P1_OBJ_PTR)
            pre["obj"] = d.hex()
        return body
    run_one(9, "p1_draw", "Seed pixel + move_anim=5; draw_p1_sprite -> obj descriptor.",
            make_draw(5))
    run_one(10, "p1_draw_hidden", "move_anim=100 (hidden) -> draw_p1_sprite no-op.",
            make_draw(100))

    # --- Scenario 11: restore_bg_pending ---------------------------------------------
    def make_pend(count):
        def body(pre):
            call_engine_fn(0x535e)
            wr8(OFF_PEND_COUNT, count)
            wr16(OFF_PEND_X, 6); wr16(OFF_PEND_Y, 9)
            pre["count"] = count
            call_engine_fn(0x1a20)   # restore_bg_pending
            pre["count_after"] = rd8(OFF_PEND_COUNT)
        return body
    run_one(11, "p1_pending_active", "pending_erase_count=3 -> dec to 2 + descriptor.",
            make_pend(3))
    run_one(12, "p1_pending_idle", "pending_erase_count=0 -> no-op.",
            make_pend(0))

    # --- Scenario 13: all_entries_flag_set -------------------------------------------
    # Seed a small move-descriptor table inside far_mem-equivalent: point the far ptr
    # at a scratch DGROUP region and write 9-byte records (record0 unused; records
    # 1..N have [0] flag; first [0]==0xff terminates).
    SCRATCH = 0x0a00   # a clear low DGROUP scratch region for the table
    def make_all_entries(flags):
        """flags: list of record[0] bytes for records 1..N; terminator appended."""
        def body(pre):
            # point move_descriptor_table at DG:SCRATCH (off=SCRATCH, seg=dg)
            uc.mem_write(DG_LIN + OFF_MOVE_DESC_PTR, struct.pack("<HH", SCRATCH, dg))
            # clear the table region
            uc.mem_write(DG_LIN + SCRATCH, b"\x00" * 0x100)
            # records: index 1.. ; each 9 bytes, [0]=flag
            for i, fl in enumerate(flags, start=1):
                uc.mem_write(DG_LIN + SCRATCH + i * 9, bytes([fl & 0xFF]))
            # terminator at the record after the last
            uc.mem_write(DG_LIN + SCRATCH + (len(flags) + 1) * 9, bytes([0xff]))
            pre["flags"] = flags
            call_engine_fn(0x3e8a)   # all_entries_flag_set
            pre["ret"] = last_all_set[0]
        return body
    run_one(13, "all_entries_all_set", "records [1,1,1] -> predicate returns 1.",
            make_all_entries([1, 1, 1]))
    run_one(14, "all_entries_one_clear", "records [1,0,1] -> predicate returns 0.",
            make_all_entries([1, 0, 1]))
    run_one(15, "all_entries_empty", "record 1 is the terminator -> returns 1 (seed).",
            make_all_entries([]))

    # ---------------------------------------------------------------------------
    # Write the frozen trace
    # ---------------------------------------------------------------------------
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT_TRACE, "wb") as f:
        f.write(TRACE_MAGIC)
        f.write(struct.pack("<HH", TRACE_VERSION, len(scenario_results)))
        f.write(struct.pack("<H", len(fn_name_list)))
        for nm in fn_name_list:
            b = nm.encode("ascii")
            f.write(struct.pack("<B", len(b))); f.write(b)
        for sr in scenario_results:
            nb = sr["name"].encode("ascii")
            f.write(struct.pack("<B", sr["id"]))
            f.write(struct.pack("<B", len(nb))); f.write(nb)
            f.write(struct.pack("<BB", 1, LEVEL))
            f.write(struct.pack("<I", len(sr["recs"])))
            for r in sr["recs"]:
                f.write(r)
    trace_bytes = os.path.getsize(OUT_TRACE)
    print("[p1_spine_oracle] wrote %s (%d B)" % (OUT_TRACE, trace_bytes), flush=True)

    # ---------------------------------------------------------------------------
    # Model md
    # ---------------------------------------------------------------------------
    def decode_records(recs: List[bytes]) -> List[dict]:
        out = []
        for r in recs:
            fn_off, name_idx = struct.unpack_from("<HH", r, 0)
            o = 4
            ent = struct.unpack_from(P1SNAP_FMT, r, o); o += P1SNAP_SIZE
            ex = struct.unpack_from(P1SNAP_FMT, r, o); o += P1SNAP_SIZE
            blobs = {}
            for key in ("p1view", "p1erase", "pend", "obj"):
                (ln,) = struct.unpack_from("<B", r, o); o += 1
                blobs[key] = r[o:o + ln]; o += ln
            (iln,) = struct.unpack_from("<H", r, o); o += 2
            blobs["init"] = r[o:o + iln]; o += iln
            out.append(dict(fn=FN_NAMES[fn_off], fn_off=fn_off, ent=ent, ex=ex, **blobs))
        return out

    def sd(t):
        return dict(zip(SNAP_FIELDS, t))

    L: List[str] = []
    L.append("# Bumpy Phase-9 T3 Player-1 per-tick spine capture model (discovery)\n\n")
    L.append("Generated by `tools/p1_spine_oracle.py`. Capture granularity = P1 "
             "per-tick FUNCTION-CALL boundary (entry+exit). Symmetric P1 analog of "
             "tools/p2_oracle.py.\n\n")
    L.append("## Hooked P1 functions (Ghidra seg-1000 offsets)\n\n")
    L.append("| addr (1000:off) | name |\n|---|---|\n")
    for off, nm in sorted(FN_NAMES.items()):
        L.append("| 1000:%04x | %s |\n" % (off, nm))
    L.append("\n")
    for sr in scenario_results:
        dec = decode_records(sr["recs"])
        L.append("## Scenario %d — %s\n\n" % (sr["id"], sr["name"]))
        L.append("- **seeded**. %s\n" % sr["note"])
        if sr["pre"]:
            L.append("- pre/post: %s\n" % ", ".join(
                "%s=%s" % (k, v) for k, v in sr["pre"].items() if k not in ("obj",)))
        L.append("- records: %d\n\n" % len(dec))
        for d in dec:
            e = sd(d["ent"]); x = sd(d["ex"])
            L.append("  - **%s**: px=%d py=%d  gxn %d->%d gyn %d->%d  gx %d->%d gy %d->%d  "
                     "scroll %d/%d  bbox(%d,%d,%d,%d)  anim=%d frozen=%d pcount %d->%d "
                     "all_set=%d\n" % (
                         d["fn"], x["px"], x["py"], e["gxn"], x["gxn"], e["gyn"], x["gyn"],
                         e["gx"], x["gx"], e["gy"], x["gy"], x["sx"], x["sy"],
                         x["bl"], x["br"], x["bt"], x["bb"], x["anim"], x["frozen"],
                         e["pcount"], x["pcount"], x["all_set"]))
            if d["obj"]:
                L.append("    p1_sprite obj (hex): `%s`\n" % d["obj"].hex())
            if d["p1view"]:
                L.append("    p1_view desc (hex): `%s`\n" % d["p1view"].hex())
            if d["p1erase"]:
                L.append("    p1_erase_view desc (hex): `%s`\n" % d["p1erase"].hex())
            if d["pend"]:
                L.append("    pending_erase_view desc (hex): `%s`\n" % d["pend"].hex())
            if d["init"]:
                L.append("    init blob len=%d\n" % len(d["init"]))
        L.append("\n")

    os.makedirs(os.path.dirname(OUT_MODEL), exist_ok=True)
    with open(OUT_MODEL, "w") as f:
        f.write("".join(L))
    print("[p1_spine_oracle] wrote %s" % OUT_MODEL, flush=True)

    print("\n[p1_spine_oracle] REACHED P1 functions:", flush=True)
    for off, cnt in sorted(reached_fns.items()):
        print("   1000:%04x  %-28s x%d" % (off, FN_NAMES[off], cnt), flush=True)
    if err:
        print("[p1_spine_oracle] emu error:", err, flush=True)
    if tr.get("call_err"):
        print("[p1_spine_oracle] call_err:", tr["call_err"], flush=True)


# ---------------------------------------------------------------------------
# MZ loader + DOS file shim (identical to the lineage)
# ---------------------------------------------------------------------------
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


if __name__ == "__main__":
    main()
