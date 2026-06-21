#!/usr/bin/env python3
"""physics_oracle.py — Phase-2 physics CAPTURE-AS-DISCOVERY harness.

Boots the real (unpacked) BUMPY.EXE under Unicorn — reusing the boot + scripted-
input + DGROUP-read scaffold of tools/game_oracle.py / tools/sprite_oracle.py
(deliberately NOT refactoring those) — drives 5 scripted movement scenarios, and
captures the engine's player-physics I/O at the engine's *move-step / physics-
FUNCTION-CALL boundary* (NOT the int8-tick rate; the Phase-1 int8 trace is desynced
and unusable, and is neither read nor touched here).

For each call of the hooked physics functions we snapshot ENTRY state and EXIT state
(the physics globals), plus the active move-script bytes (followed through the far
pointer at DGROUP 0xa1ac) and a tilemap window around p1_cell (tilemap far ptr at
DGROUP 0xa0d8). At the two dispatch sites we additionally read the dispatch-table
slot to DISCOVER which game_mode_handlers / move_step_dispatch_tbl target the run
reaches — the non-walk reached set is the Task-4 port list.

Hooked physics functions (Ghidra seg:off -> runtime linear 0x11100+off):
  enter_game_mode               1000:4263
  p1_step_scripted_move         1000:13df
  dispatch_move_step            1000:238e   (CALL [BX+0x43c0] @ 1000:23b0)
  p1_movement_dispatch          1000:1e02   (CALL [BX+0x7ca]  @ 1000:1e32)
  land_on_tile_below            1000:2810
  check_tile_below_ladder_or_land 1000:29a6

Outputs (BOTH gitignored — discovery; NO commit):
  local/build/render/physics_trace.bin   (frozen layout — see TRACE LAYOUT below)
  local/build/physics_model.md           (per-scenario fn-call sequence + scripts)

TRACE LAYOUT (little-endian) — FROZEN; Task-2 (physics_ctest.c) parses this exactly:
  Header:
    +0x00  8 B   magic   b"PHYSTRC1"
    +0x08  2 B   u16     version (=1)
    +0x0A  2 B   u16     n_scenarios
  Then a fn-name string table (so records can carry a compact fn_name_idx):
    +..    2 B   u16     n_fn_names
    per name: u8 len, len bytes (ascii)
  Then, per scenario:
    u8        scenario_id
    u8        name_len,  name_len bytes (ascii scenario name)
    u8        level
    u8        start_cell
    u32       n_records
    then n_records records.

  Per RECORD (one physics-function call; carries BOTH entry and exit snapshots):
    u16   fn_addr        (Ghidra seg-1000 offset of the hooked fn, e.g. 0x4263)
    u16   fn_name_idx    (index into the fn-name string table)
    u16   dispatch_target(seg-1000 offset of the table target this call dispatched
                          to, or 0xFFFF if this fn is not a dispatch site / no call)
    SNAP  entry          (16-byte fixed struct, see SNAP below)
    SNAP  exit           (16-byte fixed struct)
    u16   script_off     (p1_move_script offset at ENTRY)
    u16   script_seg     (p1_move_script segment at ENTRY)
    u16   script_len     (# script bytes captured)
    script_len bytes     (raw bytes the far ptr points at — the [anim,dx,dy] entries)
    u8    tile_base_cell (first cell of the captured tilemap window)
    u8    tile_len       (# tilemap bytes captured)
    tile_len bytes       (raw tilemap window)

  SNAP (16 bytes, the physics globals; little-endian):
    s16  p1_pixel_x      (DGROUP 0x9290)
    s16  p1_pixel_y      (DGROUP 0x9292)
    u8   p1_move_anim    (0x824a)
    u8   game_mode       (0x792c)
    u8   p1_move_step_idx(0x792a)
    u8   p1_facing_left  (0x9bae)
    u8   p1_move_steps_left(0x824d)
    u8   input_state     (0x8244)
    u8   physics_frozen  (0xa0ce)
    u8   move_override   (0xa1a7)
    u8   p1_cell         (0x856e)
    u8   move_locked     (0x8242)
    u8   prev_game_mode  (0x8552)
    u8   p1_step_col_count(0x855e)   (former _pad slot; cursor/move-step col counter)

Run (sandbox disabled — needs unicorn/uv cache access), HARD timeout:
  timeout 1800 uv run python tools/physics_oracle.py
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
OUT_TRACE = os.path.join(OUT_DIR, "physics_trace.bin")
OUT_MODEL = os.path.join(ROOT, "local/build/physics_model.md")

PSP_SEG = 0x0100
RAM = 0x110000

# DGROUP runtime base — identical formula to game_oracle.py / sprite_oracle.py.
DG_LIN: int = (0x103b + PSP_SEG + 0x10) * 16   # 0x114b0
# Ghidra "segment 1000" == the program's load base (image offset 0); a function at
# Ghidra 1000:off lives at raw image offset `off`, loaded at base*16 = 0x1100.
# (Verified: the dispatch CALL sites FF97C043 / FF97CA07 sit at raw image offsets
#  0x23b0 / 0x1e32, exactly the Ghidra 1000:off values — no 0x1000*16 added.)
CODE_LIN: int = (PSP_SEG + 0x10) * 16          # 0x1100  (Ghidra seg-1000 runtime base)

# ---------------------------------------------------------------------------
# DGROUP global offsets (Ghidra DGROUP 0x203b offsets)
# ---------------------------------------------------------------------------
OFF_P1_PIXEL_X: int = 0x9290        # s16
OFF_P1_PIXEL_Y: int = 0x9292        # s16
OFF_P1_MOVE_ANIM: int = 0x824a      # u8
OFF_GAME_MODE: int = 0x792c         # u8
OFF_P1_MOVE_STEP_IDX: int = 0x792a  # u8
OFF_P1_FACING_LEFT: int = 0x9bae    # u8
OFF_P1_MOVE_STEPS_LEFT: int = 0x824d  # u8
OFF_INPUT_STATE: int = 0x8244       # u8
OFF_PHYSICS_FROZEN: int = 0xa0ce    # u8
OFF_MOVE_OVERRIDE: int = 0xa1a7     # u8
OFF_P1_CELL: int = 0x856e           # u8
OFF_MOVE_LOCKED: int = 0x8242       # u8
OFF_PREV_GAME_MODE: int = 0x8552    # u8
OFF_P1_STEP_COL_COUNT: int = 0x855e # u8 (cursor/move-step COLUMN counter; the
                                    #     contact/walk-step resolvers + apply_contact
                                    #     handlers gate on this, NOT 0x824c — captured
                                    #     in the SNAP's former _pad slot so the per-fn
                                    #     differential can seed it distinctly)
OFF_CURRENT_LEVEL: int = 0x79b2     # u8
OFF_COPYPROTECT: int = 0x119a       # s8

OFF_MOVE_SCRIPT_PTR: int = 0xa1ac   # far ptr (off @0xa1ac, seg @0xa1ae)
OFF_TILEMAP_PTR: int = 0xa0d8       # far ptr (off @0xa0d8, seg @0xa0da)

OFF_KEY_STATE_PTR: int = 0x4D42     # near ptr to g_key_state_table base

# ---------------------------------------------------------------------------
# Hooked physics functions (Ghidra seg-1000 offsets)
# ---------------------------------------------------------------------------
FN_NAMES: Dict[int, str] = {
    0x4263: "enter_game_mode",
    0x13df: "p1_step_scripted_move",
    0x238e: "dispatch_move_step",
    0x1e02: "p1_movement_dispatch",
    0x2810: "land_on_tile_below",
    0x29a6: "check_tile_below_ladder_or_land",
}
# Dispatch CALL-site instruction offsets (where the indirect table call happens).
DISPATCH_SITE_MOVE_STEP: int = 0x23b0   # CALL word ptr [BX + 0x43c0] (BX preloaded)
DISPATCH_SITE_HANDLER: int = 0x1e32     # CALL word ptr [BX + 0x7ca]

# Known handler names for the discovery report (addr-1000 offset -> name).
# Resolved live via Ghidra MCP (2026, BumpyDecomp).
KNOWN_HANDLERS: Dict[int, str] = {
    # game_mode_handlers (@0x7ca) targets — the per-mode movement behaviours
    0x28f9: "gamemode_default_idle",
    0x1e5e: "gamemode_21_start",
    0x1e90: "gamemode_22",
    0x1ec2: "gamemode_23_walk",
    0x1f3e: "gamemode_24_walk",
    0x23b6: "gamemode_03_move",
    0x2138: "gamemode_25_contact",
    0x21e7: "gamemode_26_contact",
    0x2470: "enter_mode_0b_jump_start",
    0x28e0: "enter_mode_04_fall",
    0x2810: "land_on_tile_below",
    0x29a6: "check_tile_below_ladder_or_land",
    0x2965: "handle_move_input",
    0x27de: "move_settle",
    0x22b0: "FUN_1000_22b0",
    0x22c1: "run_physics_settle_wrap",
    0x22d2: "advance_physics_freeze",
    0x1e3d: "FUN_1000_1e3d",
    0x248e: "move_anim_step_to_mode0c",
    0x24d7: "move_step_check_walkable",
    0x250a: "move_step_dispatch_input",
    0x2423: "move_walk_right_anim_step",
    # move_step_dispatch_tbl (@0x43c0) substate targets (resolved via MCP)
    0x6305: "play_exit_sound",
    0x6326: "check_exit_tile_horiz",
    0x6372: "check_exit_tile_vert",
    0x647e: "play_state_sound_79b9",
    0x64e2: "cursor_move_up",
    0x64ff: "cursor_move_down",
    0x651c: "cursor_move_left",
    0x6535: "cursor_move_right",
    0x654e: "p1_try_trigger_pending_action",
    0x6587: "p1_try_jump_action",
    0x65e5: "input_state_mask_10",
    0x65fb: "input_state_mask_1d",
    0x6611: "input_state_mask_0f",
    0x6627: "move_step_read_item",
    0x6648: "p1_move_step_with_sound",
    0x6699: "move_step_first_variant",
    0x66d8: "move_step_last_variant",
    0x6717: "move_step_landed",
    0x673a: "move_step_noop",
    0x7111: "move_step_noop_sentinel",  # table slot, no Ghidra fn boundary (likely no-op)
}

# ---------------------------------------------------------------------------
# Trace format constants
# ---------------------------------------------------------------------------
TRACE_MAGIC: bytes = b"PHYSTRC1"
# v2 (Phase 9.1): the final SNAP byte (slot 15) was an always-zero `_pad` in v1;
# it now carries the engine's captured p1_step_col_count (0x855e) so the per-fn
# differential can seed 0x855e distinctly from move_step_count (0x824c) and catch
# wrong-global reads.  Layout size is unchanged, so the VERSION is bumped to make a
# stale v1 trace HARD-FAIL at load (physics_ctest.c) rather than silently read the
# step_col seed as zero (which would re-hide the counter-aliasing bug).
TRACE_VERSION: int = 2
SNAP_FMT: str = "<hhBBBBBBBBBBBB"   # 16 bytes (slot 15 = p1_step_col_count as of v2)
SNAP_SIZE: int = struct.calcsize(SNAP_FMT)  # 16
assert SNAP_SIZE == 16

# Move-script capture: read this many bytes from the far ptr (covers many
# 6-byte [anim,dx,dy] entries; the run consumes them as it advances).
SCRIPT_CAP_BYTES: int = 96
# Tilemap window around p1_cell.
TILE_WIN_BEFORE: int = 16
TILE_WIN_AFTER: int = 16

# ---------------------------------------------------------------------------
# Scancode / input_state mapping (from game_oracle.py / handle_move_input)
# ---------------------------------------------------------------------------
SC_RIGHT: int = 0x4D
SC_LEFT: int = 0x4B
SC_JUMP: int = 0x48
SC_DOWN: int = 0x50
IS_RIGHT: int = 0x08
IS_LEFT: int = 0x04
IS_JUMP: int = 0x10
IS_DOWN: int = 0x02
IS_IDLE: int = 0x00

# ---------------------------------------------------------------------------
# Scenarios: each is (id, name, level, start_cell, tick_script).
# tick_script is a list of (n_ticks, input_state, scancode).
# start_cell: if not None, force p1_cell + recompute pixel pos before the run.
# Walk / ledge / land use level 1 (Phase-1 reach). Jump scenarios also use
# level 1 with vertical headroom mid-field; documented in physics_model.md.
# ---------------------------------------------------------------------------
Scenario = Tuple[int, str, int, Optional[int], List[Tuple[int, int, int]]]

SCENARIOS: List[Scenario] = [
    (1, "walk", 1, None, [
        (6,  IS_IDLE,  0),
        (24, IS_RIGHT, SC_RIGHT),
        (6,  IS_IDLE,  0),
    ]),
    (2, "jump_up", 1, None, [
        (6,  IS_IDLE, 0),
        (4,  IS_JUMP, SC_JUMP),
        (30, IS_IDLE, 0),
    ]),
    # jump_lateral: jump while holding right. NOTE (engine finding): the scripted
    # jump locks lateral displacement for its duration, so px does not advance
    # mid-arc; the right hold takes effect on landing. The vertical arc (py rise
    # then fall, reaching mode 4 fall + the landing leaves) is the captured signal.
    (3, "jump_lateral", 1, None, [
        (6,  IS_IDLE,            0),
        (4,  IS_JUMP | IS_RIGHT, SC_JUMP),
        (30, IS_RIGHT,           SC_RIGHT),
    ]),
    # walk_off_ledge: walk right toward the right-edge cell. On level 1 the row
    # from cell 40 resolves cell-by-cell to the cell-47 edge, which enters move
    # mode 0x2d (the edge/exit contact) rather than a free vertical fall — the
    # walk's own per-cell collision resolution is the "leave the platform" event.
    # The free-fall arc proper is captured by the jump scenarios' descent (mode 4).
    (4, "walk_off_ledge", 1, None, [
        (4,  IS_IDLE,  0),
        (28, IS_RIGHT, SC_RIGHT),
        (8,  IS_IDLE,  0),
    ]),
    (5, "land", 1, None, [
        (4,  IS_IDLE, 0),
        (4,  IS_JUMP, SC_JUMP),
        (36, IS_IDLE, 0),
    ]),
]


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

    # --- minimal VGA planar emulation (copied from game_oracle.py) -----------------
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

    def clear_all_keys() -> None:
        mbase = struct.unpack("<H", bytes(uc.mem_read(dg * 16 + OFF_KEY_STATE_PTR, 2)))[0]
        uc.mem_write(dg * 16 + mbase, bytes(0x80))

    def inject_input(is_val: int, scancode: int) -> None:
        uc.mem_write(DG_LIN + OFF_INPUT_STATE, bytes([is_val & 0xFF]))
        clear_all_keys()
        if scancode != 0:
            set_key(scancode, True)

    # ---------------------------------------------------------------------------
    # Snapshot helpers
    # ---------------------------------------------------------------------------
    def rd8(off: int) -> int:
        return uc.mem_read(DG_LIN + off, 1)[0]

    def rd_s16(off: int) -> int:
        return struct.unpack("<h", bytes(uc.mem_read(DG_LIN + off, 2)))[0]

    def snap() -> bytes:
        return struct.pack(
            SNAP_FMT,
            rd_s16(OFF_P1_PIXEL_X), rd_s16(OFF_P1_PIXEL_Y),
            rd8(OFF_P1_MOVE_ANIM), rd8(OFF_GAME_MODE), rd8(OFF_P1_MOVE_STEP_IDX),
            rd8(OFF_P1_FACING_LEFT), rd8(OFF_P1_MOVE_STEPS_LEFT), rd8(OFF_INPUT_STATE),
            rd8(OFF_PHYSICS_FROZEN), rd8(OFF_MOVE_OVERRIDE), rd8(OFF_P1_CELL),
            rd8(OFF_MOVE_LOCKED), rd8(OFF_PREV_GAME_MODE),
            rd8(OFF_P1_STEP_COL_COUNT))   # former _pad slot now = 0x855e

    def read_script() -> Tuple[int, int, bytes]:
        off, seg = struct.unpack("<HH", bytes(uc.mem_read(DG_LIN + OFF_MOVE_SCRIPT_PTR, 4)))
        lin = (seg * 16 + off) & 0xFFFFF
        try:
            data = bytes(uc.mem_read(lin, SCRIPT_CAP_BYTES))
        except UcError:
            data = b""
        return off, seg, data

    def read_tile_window() -> Tuple[int, int, bytes]:
        # tilemap is a FAR ptr at DGROUP 0xa0d8 (off) / 0xa0da (seg) — see
        # read_tile_at_cell @1000:6bb5 (LES BX,[0xa0d8]).
        off, seg = struct.unpack("<HH", bytes(uc.mem_read(DG_LIN + OFF_TILEMAP_PTR, 4)))
        cell = rd8(OFF_P1_CELL)
        base_cell = max(0, cell - TILE_WIN_BEFORE)
        n = TILE_WIN_BEFORE + TILE_WIN_AFTER
        lin = (seg * 16 + off + base_cell) & 0xFFFFF
        try:
            data = bytes(uc.mem_read(lin, n))
        except UcError:
            data = b""
        return base_cell, len(data), data

    # ---------------------------------------------------------------------------
    # Physics-function hooks (entry + exit via dynamic return-address hook)
    # ---------------------------------------------------------------------------
    # Active capture flag — only record once gameplay has started for a scenario.
    capturing = {"on": False}
    cur_records: List[bytes] = []
    reached_handlers: collections.Counter = collections.Counter()  # game_mode_handlers offset -> count
    reached_substeps: collections.Counter = collections.Counter()  # move_step_dispatch_tbl offset -> count
    reached_modes: collections.Counter = collections.Counter()
    pending_exit: dict = {}   # ret_lin -> (fn_off, entry_snap, sc_off, sc_seg, sc_bytes, tile_base, tile_len, tile_bytes, dispatch_target)
    exit_hook_lins: set = set()
    fn_name_list: List[str] = list(dict.fromkeys(FN_NAMES.values()))
    fn_name_idx = {n: i for i, n in enumerate(fn_name_list)}

    def emit_record(fn_off: int, entry_snap: bytes, exit_snap: bytes,
                    sc_off: int, sc_seg: int, sc_bytes: bytes,
                    tile_base: int, tile_len: int, tile_bytes: bytes,
                    dispatch_target: int) -> None:
        rec = struct.pack("<HHH", fn_off, fn_name_idx[FN_NAMES[fn_off]], dispatch_target & 0xFFFF)
        rec += entry_snap + exit_snap
        rec += struct.pack("<HHH", sc_off & 0xFFFF, sc_seg & 0xFFFF, len(sc_bytes))
        rec += sc_bytes
        rec += struct.pack("<BB", tile_base & 0xFF, tile_len & 0xFF)
        rec += tile_bytes
        cur_records.append(rec)

    def hook_fn_entry(uc: Uc, addr: int, size: int, _: object) -> None:
        if not capturing["on"]:
            return
        fn_off = (addr - CODE_LIN) & 0xFFFF
        if fn_off not in FN_NAMES:
            return
        entry_snap = snap()
        sc_off, sc_seg, sc_bytes = read_script()
        t_base, t_len, t_bytes = read_tile_window()
        # near return address on top of stack (PUSH BP not yet executed at entry).
        ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc.mem_read(ss * 16 + sp, 2)))[0]
        ret_lin = (CODE_LIN + ret_off) & 0xFFFFF
        pending_exit[ret_lin] = (fn_off, entry_snap, sc_off, sc_seg, sc_bytes,
                                 t_base, t_len, t_bytes)
        if ret_lin not in exit_hook_lins:
            exit_hook_lins.add(ret_lin)
            uc.hook_add(UC_HOOK_CODE, hook_fn_exit, None, ret_lin, ret_lin)

    def hook_fn_exit(uc: Uc, addr: int, size: int, _: object) -> None:
        info = pending_exit.pop(addr, None)
        if info is None:
            return
        (fn_off, entry_snap, sc_off, sc_seg, sc_bytes, t_base, t_len, t_bytes) = info
        exit_snap = snap()
        emit_record(fn_off, entry_snap, exit_snap, sc_off, sc_seg, sc_bytes,
                    t_base, t_len, t_bytes, 0xFFFF)

    def hook_dispatch_handler(uc: Uc, addr: int, size: int, _: object) -> None:
        # At 1000:1e32 BX = game_mode*2; target near ptr at DS:[BX+0x7ca].
        if not capturing["on"]:
            return
        bx = uc.reg_read(UC_X86_REG_BX) & 0xFFFF
        ds = uc.reg_read(UC_X86_REG_DS) & 0xFFFF
        target = struct.unpack("<H", bytes(uc.mem_read(ds * 16 + ((bx + 0x7ca) & 0xFFFF), 2)))[0]
        gm = rd8(OFF_GAME_MODE)
        reached_handlers[target] += 1
        reached_modes[gm] += 1

    def hook_dispatch_step(uc: Uc, addr: int, size: int, _: object) -> None:
        # At 1000:23b0 BX = game_mode*0x22 + step_idx*2; target at DS:[BX+0x43c0].
        if not capturing["on"]:
            return
        bx = uc.reg_read(UC_X86_REG_BX) & 0xFFFF
        ds = uc.reg_read(UC_X86_REG_DS) & 0xFFFF
        target = struct.unpack("<H", bytes(uc.mem_read(ds * 16 + ((bx + 0x43c0) & 0xFFFF), 2)))[0]
        reached_substeps[target] += 1

    for off in FN_NAMES:
        lin = CODE_LIN + off
        uc.hook_add(UC_HOOK_CODE, hook_fn_entry, None, lin, lin)
    uc.hook_add(UC_HOOK_CODE, hook_dispatch_handler, None,
                CODE_LIN + DISPATCH_SITE_HANDLER, CODE_LIN + DISPATCH_SITE_HANDLER)
    uc.hook_add(UC_HOOK_CODE, hook_dispatch_step, None,
                CODE_LIN + DISPATCH_SITE_MOVE_STEP, CODE_LIN + DISPATCH_SITE_MOVE_STEP)

    # ---------------------------------------------------------------------------
    # Boot to level 1 (identical approach to game_oracle.py)
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

    print("[physics_oracle] booting BUMPY (level %d)..." % LEVEL, flush=True)

    while total_instr < 400_000_000:
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e); tr["err"] = err; break
        total_instr += CHUNK
        if total_instr % 40_000_000 == 0:
            print("[physics_oracle] %dM instr, countdown=%s" % (
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
            print("[physics_oracle] level loaded (%s) at chunk %d — settling %d ticks" % (
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
        print("[physics_oracle] ERROR: premature exit/fault during boot: exit=%s fault=%s err=%s" % (
            tr.get("exit"), tr.get("fault"), err), flush=True)
        return
    if not opened(BUMNAME):
        print("[physics_oracle] ERROR: level %s never loaded after %dM instructions" % (
            BUMNAME, total_instr // 1_000_000), flush=True)
        return

    print("[physics_oracle] boot complete. Files: %s" % (
        [o[1] for o in tr.get("fileops", [])]), flush=True)

    # Snapshot a clean post-boot machine state so each scenario starts fresh.
    # We re-run scenarios sequentially from this point (no per-scenario re-boot;
    # idle ticks at each scenario's head let the engine settle to idle first).
    def run_scenario(sc: Scenario) -> Tuple[List[bytes], dict]:
        nonlocal_records: List[bytes] = []
        sc_id, name, level, start_cell, ticks = sc
        cur_records.clear()
        local_modes: collections.Counter = collections.Counter()

        # Optionally force the start cell (e.g. ledge scenario).
        if start_cell is not None:
            uc.mem_write(DG_LIN + OFF_P1_CELL, bytes([start_cell & 0xFF]))

        flat: List[Tuple[int, int]] = []
        for n, is_v, sc_code in ticks:
            flat.extend([(is_v, sc_code)] * n)

        capturing["on"] = True
        nonloc_begin = cur_lin()
        for tick_idx, (is_val, scancode) in enumerate(flat):
            inject_input(is_val, scancode)
            nonloc_begin = cur_lin()
            try:
                uc.emu_start(nonloc_begin, 0, count=CHUNK)
            except UcError as e:
                tr["err"] = str(e); break
            if tr.get("exit") is not None or tr.get("fault"):
                break
            uc.mem_write(DG_LIN + OFF_INPUT_STATE, bytes([is_val & 0xFF]))
            local_modes[rd8(OFF_GAME_MODE)] += 1
            fire_int(8)
        capturing["on"] = False
        nonlocal_records = list(cur_records)
        return nonlocal_records, dict(modes=local_modes)

    # Snapshot the full post-boot machine (RAM + register context) so each
    # scenario starts from the SAME clean idle state (no cross-scenario bleed —
    # otherwise e.g. the walk scenario walking into the ledge hole would corrupt
    # the start state of the jump scenarios). Cheap vs re-booting 80M instr/each.
    boot_ram = bytes(uc.mem_read(0, RAM))
    boot_ctx = uc.context_save()

    def restore_boot_state() -> None:
        uc.mem_write(0, boot_ram)
        uc.context_restore(boot_ctx)
        tr["fault"] = None
        tr["exit"] = None

    # ---------------------------------------------------------------------------
    # Run all 5 scenarios
    # ---------------------------------------------------------------------------
    scenario_blobs: List[Tuple[Scenario, List[bytes]]] = []
    scenario_meta: List[dict] = []
    for sc in SCENARIOS:
        sc_id, name, level, start_cell, ticks = sc
        restore_boot_state()   # clean idle start for every scenario
        # settle to idle: a few idle ticks before the scripted move
        print("[physics_oracle] === scenario %d (%s) level=%d start_cell=%s ===" % (
            sc_id, name, level, start_cell), flush=True)
        px0 = rd_s16(OFF_P1_PIXEL_X); py0 = rd_s16(OFF_P1_PIXEL_Y)
        cell0 = rd8(OFF_P1_CELL); gm0 = rd8(OFF_GAME_MODE)
        recs, meta = run_scenario(sc)
        px1 = rd_s16(OFF_P1_PIXEL_X); py1 = rd_s16(OFF_P1_PIXEL_Y)
        cell1 = rd8(OFF_P1_CELL); gm1 = rd8(OFF_GAME_MODE)
        print("[physics_oracle]   %d records | (px,py)=(%d,%d)->(%d,%d) cell=%d->%d gm=%#x->%#x" % (
            len(recs), px0, py0, px1, py1, cell0, cell1, gm0, gm1), flush=True)
        scenario_blobs.append((sc, recs))
        scenario_meta.append(dict(meta, px0=px0, py0=py0, px1=px1, py1=py1,
                                  cell0=cell0, cell1=cell1, gm0=gm0, gm1=gm1))
        if tr.get("fault") or tr.get("exit") is not None:
            print("[physics_oracle]   WARNING: fault/exit during scenario %d: %s %s" % (
                sc_id, tr.get("fault"), tr.get("exit")), flush=True)
            tr["fault"] = None  # allow subsequent scenarios to attempt

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
        for sc, recs in scenario_blobs:
            sc_id, name, level, start_cell, ticks = sc
            nb = name.encode("ascii")
            f.write(struct.pack("<B", sc_id))
            f.write(struct.pack("<B", len(nb))); f.write(nb)
            f.write(struct.pack("<BB", level, (start_cell if start_cell is not None else 0xFF) & 0xFF))
            f.write(struct.pack("<I", len(recs)))
            for r in recs:
                f.write(r)
    trace_bytes = os.path.getsize(OUT_TRACE)
    print("[physics_oracle] wrote %s (%d B)" % (OUT_TRACE, trace_bytes), flush=True)

    # ---------------------------------------------------------------------------
    # Decode records (for the model md) — helpers
    # ---------------------------------------------------------------------------
    def decode_records(recs: List[bytes]) -> List[dict]:
        out = []
        for r in recs:
            fn_off, name_idx, disp = struct.unpack_from("<HHH", r, 0)
            o = 6
            ent = struct.unpack_from(SNAP_FMT, r, o); o += SNAP_SIZE
            ex = struct.unpack_from(SNAP_FMT, r, o); o += SNAP_SIZE
            sc_off, sc_seg, sc_len = struct.unpack_from("<HHH", r, o); o += 6
            sc_bytes = r[o:o + sc_len]; o += sc_len
            t_base, t_len = struct.unpack_from("<BB", r, o); o += 2
            t_bytes = r[o:o + t_len]; o += t_len
            out.append(dict(fn=FN_NAMES[fn_off], fn_off=fn_off, ent=ent, ex=ex,
                            sc_off=sc_off, sc_seg=sc_seg, sc_bytes=sc_bytes,
                            t_base=t_base, t_bytes=t_bytes))
        return out

    # SNAP field order helper
    SNAP_FIELDS = ["px", "py", "anim", "mode", "step", "facing", "steps_left",
                   "input", "frozen", "override", "cell", "locked", "prev_mode",
                   "step_col"]

    def snap_dict(t: tuple) -> dict:
        return dict(zip(SNAP_FIELDS, t))

    # ---------------------------------------------------------------------------
    # physics_model.md
    # ---------------------------------------------------------------------------
    lines: List[str] = []
    lines.append("# Bumpy Phase-2 physics capture model (discovery)\n")
    lines.append("Generated by `tools/physics_oracle.py`. Capture granularity = engine "
                 "move-step / physics-function-call boundary (NOT int8 tick).\n")
    lines.append("\n## Trace layout\n")
    lines.append("See the header comment of `tools/physics_oracle.py` for the frozen "
                 "`physics_trace.bin` layout (magic `PHYSTRC1`, version 2). Each record "
                 "carries fn_addr, fn_name_idx, dispatch_target, a 16-byte entry SNAP and "
                 "16-byte exit SNAP, the active move-script bytes (off/seg + raw), and a "
                 "tilemap window around p1_cell.\n")
    lines.append("\nSNAP fields (16 B, LE): " + ", ".join(SNAP_FIELDS[:-1]) + ".\n")

    # Reached handler set
    lines.append("\n## Reached handler/mode set (the Task-4 port list)\n")
    lines.append("\n### game_mode_handlers (@DGROUP 0x7ca) targets — per-mode behaviours\n\n")
    lines.append("| addr (1000:off) | name | hits |\n|---|---|---|\n")
    for off, cnt in sorted(reached_handlers.items()):
        nm = KNOWN_HANDLERS.get(off, "FUN_1000_%04x?" % off)
        lines.append("| 1000:%04x | %s | %d |\n" % (off, nm, cnt))
    lines.append("\n### move_step_dispatch_tbl (@DGROUP 0x43c0) targets — move-step substates\n\n")
    lines.append("| addr (1000:off) | name | hits |\n|---|---|---|\n")
    for off, cnt in sorted(reached_substeps.items()):
        nm = KNOWN_HANDLERS.get(off, "FUN_1000_%04x?" % off)
        lines.append("| 1000:%04x | %s | %d |\n" % (off, nm, cnt))
    lines.append("\nReached game_mode values: %s\n" % (
        ", ".join("%#x(%d)" % (m, c) for m, c in sorted(reached_modes.items()))))

    # Per scenario
    for (sc, recs), meta in zip(scenario_blobs, scenario_meta):
        sc_id, name, level, start_cell, ticks = sc
        dec = decode_records(recs)
        lines.append("\n## Scenario %d — %s\n" % (sc_id, name))
        lines.append("- level=%d start_cell=%s\n" % (level, start_cell))
        lines.append("- records: %d\n" % len(dec))
        lines.append("- trajectory: (px,py)=(%d,%d)->(%d,%d), cell %d->%d, game_mode %#x->%#x\n" % (
            meta["px0"], meta["py0"], meta["px1"], meta["py1"],
            meta["cell0"], meta["cell1"], meta["gm0"], meta["gm1"]))
        lines.append("- game_modes seen this scenario: %s\n" % (
            ", ".join("%#x" % m for m in sorted(meta["modes"]))))
        # ordered fn-call sequence (compressed)
        lines.append("\n### Ordered physics-fn calls (entry->exit deltas)\n\n")
        lines.append("| # | fn | mode | px e->x | py e->x | anim | steps_left | cell |\n")
        lines.append("|---|---|---|---|---|---|---|---|\n")
        for i, d in enumerate(dec[:120]):
            e = snap_dict(d["ent"]); x = snap_dict(d["ex"])
            lines.append("| %d | %s | %#x->%#x | %d->%d | %d->%d | %d->%d | %d->%d | %d->%d |\n" % (
                i, d["fn"], e["mode"], x["mode"], e["px"], x["px"], e["py"], x["py"],
                e["anim"], x["anim"], e["steps_left"], x["steps_left"], e["cell"], x["cell"]))
        if len(dec) > 120:
            lines.append("\n(+%d more records)\n" % (len(dec) - 120))
        # recovered move-scripts per (mode) — first non-empty script seen per mode
        seen_scripts: Dict[int, bytes] = {}
        for d in dec:
            m = snap_dict(d["ent"])["mode"]
            if d["sc_bytes"] and m not in seen_scripts:
                seen_scripts[m] = d["sc_bytes"]
        lines.append("\n### Recovered move-scripts (by entry game_mode)\n\n")
        for m, b in sorted(seen_scripts.items()):
            entries = []
            for j in range(0, min(len(b), 36), 6):
                anim, dx, dy = struct.unpack_from("<hhh", b, j) if j + 6 <= len(b) else (0, 0, 0)
                entries.append("[a=%d dx=%d dy=%d]" % (anim, dx, dy))
            lines.append("- mode %#x: %s\n" % (m, " ".join(entries)))

    # Trajectory sanity
    lines.append("\n## Trajectory sanity summary\n")
    for (sc, recs), meta in zip(scenario_blobs, scenario_meta):
        sc_id, name, level, start_cell, ticks = sc
        dec = decode_records(recs)
        pys = [snap_dict(d["ex"])["py"] for d in dec] or [meta["py0"]]
        pxs = [snap_dict(d["ex"])["px"] for d in dec] or [meta["px0"]]
        miny, maxy = min(pys), max(pys)
        minx, maxx = min(pxs), max(pxs)
        lines.append("- **%s**: py range [%d..%d] (start %d, rose=%s fell=%s), "
                     "px range [%d..%d], cell %d->%d\n" % (
                         name, miny, maxy, meta["py0"],
                         miny < meta["py0"], maxy > meta["py0"],
                         minx, maxx, meta["cell0"], meta["cell1"]))

    os.makedirs(os.path.dirname(OUT_MODEL), exist_ok=True)
    with open(OUT_MODEL, "w") as f:
        f.write("".join(lines))
    print("[physics_oracle] wrote %s" % OUT_MODEL, flush=True)

    # ---------------------------------------------------------------------------
    # Console summary
    # ---------------------------------------------------------------------------
    print("\n[physics_oracle] REACHED game_mode_handlers (@0x7ca):", flush=True)
    for off, cnt in sorted(reached_handlers.items()):
        nm = KNOWN_HANDLERS.get(off, "FUN_1000_%04x?" % off)
        print("   1000:%04x  %-32s x%d" % (off, nm, cnt), flush=True)
    print("[physics_oracle] REACHED move_step_dispatch_tbl substates (@0x43c0):", flush=True)
    for off, cnt in sorted(reached_substeps.items()):
        nm = KNOWN_HANDLERS.get(off, "FUN_1000_%04x?" % off)
        print("   1000:%04x  %-32s x%d" % (off, nm, cnt), flush=True)
    print("[physics_oracle] reached game_modes: %s" % (
        ", ".join("%#x" % m for m in sorted(reached_modes)),), flush=True)
    if err:
        print("[physics_oracle] emu error:", err, flush=True)


if __name__ == "__main__":
    main()
