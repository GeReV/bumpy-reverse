#!/usr/bin/env python3
"""anim_oracle.py — Phase-5 anim-channel FX CAPTURE-AS-DISCOVERY oracle.

Boots the real (unpacked) BUMPY.EXE under Unicorn — reusing the boot + DGROUP + int /
VGA scaffold of tools/p2_oracle.py / tools/physics_oracle.py (deliberately NOT
refactoring those) — drives a set of anim-channel scenarios and captures, at the ENTRY
and EXIT of each of the 7 hooked anim-channel functions, the full channel-record table
(A + B slots, 12 bytes each), the relevant globals, the per-action tile-def + frame
tables needed to replay, the tilemap[cell] byte, and (for draw/erase) the view-descriptor
bytes the engine wrote.

This is the anim-channel analog of the Phase-2 physics / Phase-4 P2 captures.

DRIVING STRATEGY: seeded (all scenarios). The anim subsystem is fed by the player
physics spine (land-FX in player.c) and item collect/teleport (items.c), which set
`anim_target_cell` and call `apply_cell_animation(action_code)`. Channel A has the
allocator `apply_cell_animation`; channel B has NO allocator (it is spawn-populated by
other engine paths, out of scope this phase). Per the Phase-3/4 pattern we SEED the
precondition state (anim_target_cell + a live cell, or a hand-populated channel record)
and then INVOKE the REAL engine fn at its entry IP via a synthetic near-call frame. The
function body that runs is the unmodified original code; only the precondition state is
seeded. This is the faithful, bounded approach — the same one the items/P2 oracles use
for non-key-driven subsystems.

-----------------------------------------------------------------------------------
RESOLVED anim-channel DGROUP addresses (Ghidra DGROUP 0x203b offsets; read live via the
Ghidra MCP from the disassembly operands of the 7 anim fns — see anim_model.md for the
per-fn provenance). Channel-record layout (12 B, confirmed from the steppers' decomp):
  [0]  active flag (0 free / 1 active / 0xff end-of-table terminator)
  [1]  cell
  [2..5] stream ptr (far: off@+2, seg@+4)
  [6]  cur frame byte
  [7]  pad
  [8..11] frame-data ptr (far: off@+8, seg@+10)
-----------------------------------------------------------------------------------
  anim_channels_a_tbl     0x4c70/0x4c72  far-ptr slot table, 3 slots, stride 4
                                         (MOV [BX+0x4c70/0x4c72] in step/draw/erase A)
  anim_channels_b_tbl     0x4cbc/0x4cbe  far-ptr slot table, 4 slots, stride 4
                                         (MOV [BX+0x4cbc/0x4cbe] in step/draw/erase B)
  anim_target_cell        0x856f (u8)    CMP/MOV [0x856f] in apply_cell_animation 69aa
  anim_a_tiledef_tbl      0x2ede/0x2ee0  far-ptr tbl indexed action_code*4 in 69aa
  tilemap_ptr             0xa0d8 (far)   LES BX,[0xa0d8] in 69aa (base tilemap layer)
  g_anim_channel_idx      0x856c (u8)    MOV [0x856c],0 loop idx in step_anim_channels_a
  g_anim_stream_ptr       0xa0be (far)   working stream ptr in step_a
  g_anim_cur_cmd_byte     0x8578 (u8)    cur cmd byte in step_a
  anim_a_frame_tbl        0x3d6a/0x3d6c  far-ptr frame tbl indexed cmd*4 in step_a
  anim_b_loop_idx         0x8566 (u8)    MOV [0x8566],0 loop idx in step_anim_channels_b
  anim_b_stream_ptr       0xa0c2 (far)   working stream ptr in step_b
  anim_b_cur_frame_byte   0x8579 (u8)    cur frame byte in step_b
  anim_b_frame_tbl        0x40a6/0x40a8  far-ptr frame tbl indexed frame*4 in step_b
  anim_a_grid_tbl         0x32be/0x32c0  grid-coord tbls indexed cell*4 in draw/erase A
  anim_b_grid_tbl         0x343e/0x3440  grid-coord tbls indexed cell*4 in draw/erase B
  posA                    0xf4/0xf6      pos tbls indexed cell*4 in draw A
  posB                    0x3f4/0x3f6    pos tbls indexed cell*4 in draw B
  anim_a_erase_view       0x8d4 (far)    view descriptor (draw A erase pass) — 0x80bc
  anim_a_draw_view        0x8e0 (far)    view descriptor (draw A save-under) — 0x93b8
  anim_a_clear_view       0x8c0 (far)    view descriptor (erase A) — restore_bg_view
  anim_b_view0            0x8c8 (far)    view descriptor (draw B pass 0) — 0x80ac
  anim_b_view1            0x8cc (far)    view descriptor (draw B passes) — 0x80ac/80bc
  anim_b_draw_view        0x8d0 (far)    view descriptor (draw B save-under) — 0x93b8
  anim_b_clear_view       0x8bc (far)    view descriptor (erase B) — restore_bg_view
  p1_sprite               0x8884 (far)   blit descriptor; blit_sprite called (0x792e,DS)
                                         [0]=x [+2]=y [+4]=frame
  p1_sprite_desc_off      0x792e         DGROUP off of the descriptor passed to blit_sprite
  current_level           0x79b2 (u8)
  copyprotect_flag        0x119a (s8)

Hooked anim functions (Ghidra seg-1000 off -> runtime linear 0x1100+off):
  apply_cell_animation   1000:69aa  (channel-A allocator, keyed on anim_target_cell)
  step_anim_channels_a   1000:14e4  (advance 3 A channels along their byte-streams)
  step_anim_channels_b   1000:15a1  (advance 4 B channels along their byte-streams)
  draw_anim_channels_a   1000:165e  (erase old cell + blit_sprite + save-under, A)
  draw_anim_channels_b   1000:17c7  (erase old cell + blit_sprite + save-under, B)
  erase_anim_channels_a  1000:1a67  (restore_bg_view current cells, A)
  erase_anim_channels_b  1000:1b2b  (restore_bg_view current cells, B)

Outputs (BOTH gitignored — discovery; NO commit):
  local/build/render/anim_trace.bin   (frozen layout — see TRACE LAYOUT below)
  local/build/anim_model.md           (per-scenario fn-call sequence + addrs + tables)

TRACE LAYOUT (little-endian) — FROZEN; the host replay harness (a later task) parses
this exactly:
  Header:
    +0x00  8 B   magic   b"ANIMTRC1"
    +0x08  2 B   u16     version (=1)
    +0x0A  2 B   u16     n_scenarios
  fn-name string table:
    2 B   u16     n_fn_names
    per name: u8 len, len bytes (ascii)
  Then, per scenario:
    u8        scenario_id
    u8        name_len, name_len bytes (ascii)
    u8        seeded     (1 = seeded, 0 = scripted)  [always 1 this oracle]
    u8        level
    u32       n_records
    then n_records records.

  Per RECORD (one anim-fn call; carries BOTH entry and exit snapshots):
    u16   fn_addr        (Ghidra seg-1000 offset of the hooked fn, e.g. 0x14e4)
    u16   fn_name_idx    (index into the fn-name string table)
    SNAP  entry          (ANIMSNAP_SIZE-byte fixed struct, see SNAP below)
    SNAP  exit           (ANIMSNAP_SIZE-byte fixed struct)
    u8    tile_cell      (anim_target_cell at ENTRY — the cell the tilemap byte is for)
    u8    tile_byte_e    (tilemap[tile_cell] at ENTRY)
    u8    tile_byte_x    (tilemap[tile_cell] at EXIT)
    u8    desc_kind      (0 none / 1 p1_sprite blit descriptor (draw fns))
    u8    desc_len       (# descriptor bytes captured; 0 unless desc_kind!=0)
    desc_len bytes       (raw p1_sprite descriptor bytes at EXIT, 0x8884 far ptr)
    u8    nview          (# view-descriptor blobs captured at EXIT, draw/erase fns)
    per view: u8 view_id, u8 view_len, view_len bytes (raw view-descriptor struct bytes)

  ANIMSNAP (the anim-channel record table + scalar globals; little-endian) ANIMSNAP_FMT:
    u8    a_slots                  (3)   number of A slots that follow
    u8    b_slots                  (4)   number of B slots that follow
    7 * 12 B  channel records      (3 A slots then 4 B slots, 12 raw bytes each)
    u8    anim_target_cell  (0x856f)
    u8    g_anim_channel_idx(0x856c)
    u8    g_anim_cur_cmd_byte(0x8578)
    u8    anim_b_loop_idx   (0x8566)
    u8    anim_b_cur_frame_byte(0x8579)
    u8    current_level     (0x79b2)
    u16   g_anim_stream_off (0xa0be)
    u16   g_anim_stream_seg (0xa0c0)
    u16   anim_b_stream_off (0xa0c2)
    u16   anim_b_stream_seg (0xa0c4)

Run (sandbox disabled — needs unicorn/uv cache access), HARD timeout:
  timeout 1800 uv run python tools/anim_oracle.py
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
OUT_TRACE = os.path.join(OUT_DIR, "anim_trace.bin")
OUT_MODEL = os.path.join(ROOT, "local/build/anim_model.md")

PSP_SEG = 0x0100
RAM = 0x110000

DG_LIN: int = (0x103b + PSP_SEG + 0x10) * 16   # 0x114b0  (identical to the lineage)
CODE_LIN: int = (PSP_SEG + 0x10) * 16          # 0x1100   (Ghidra seg-1000 runtime base)
DG_SEG: int = 0x203b                           # engine DGROUP segment (far-ptr seg half)

# ---------------------------------------------------------------------------
# Resolved anim-channel DGROUP global offsets (Ghidra DGROUP 0x203b offsets)
# ---------------------------------------------------------------------------
OFF_A_TBL_OFF: int = 0x4c70     # anim_channels_a_tbl   (far-ptr slot tbl, off halves)
OFF_A_TBL_SEG: int = 0x4c72     # anim_channels_a_seg_tbl
OFF_B_TBL_OFF: int = 0x4cbc     # anim_channels_b_tbl
OFF_B_TBL_SEG: int = 0x4cbe     # anim_channels_b_seg_tbl
A_SLOTS: int = 3
B_SLOTS: int = 4
SLOT_REC_LEN: int = 12

OFF_ANIM_TARGET_CELL: int = 0x856f   # u8
OFF_A_TILEDEF_OFF: int = 0x2ede      # action_code*4 -> far-ptr tile-def
OFF_A_TILEDEF_SEG: int = 0x2ee0
OFF_TILEMAP_PTR: int = 0xa0d8        # far ptr to base tilemap layer

OFF_G_ANIM_CHANNEL_IDX: int = 0x856c  # u8
OFF_G_ANIM_STREAM_PTR: int = 0xa0be   # far (off 0xa0be / seg 0xa0c0)
OFF_G_ANIM_CUR_CMD_BYTE: int = 0x8578  # u8
OFF_A_FRAME_OFF: int = 0x3d6a         # cmd*4 -> far-ptr frame data
OFF_A_FRAME_SEG: int = 0x3d6c

OFF_B_LOOP_IDX: int = 0x8566          # u8
OFF_B_STREAM_PTR: int = 0xa0c2        # far (off 0xa0c2 / seg 0xa0c4)
OFF_B_CUR_FRAME_BYTE: int = 0x8579    # u8
OFF_B_FRAME_OFF: int = 0x40a6         # frame*4 -> far-ptr frame data
OFF_B_FRAME_SEG: int = 0x40a8

OFF_A_GRID_OFF: int = 0x32be          # cell*4 grid-coord tbls (draw/erase A)
OFF_A_GRID_SEG: int = 0x32c0
OFF_B_GRID_OFF: int = 0x343e          # cell*4 grid-coord tbls (draw/erase B)
OFF_B_GRID_SEG: int = 0x3440
OFF_POSA_OFF: int = 0xf4              # cell*4 pos tbls (draw A)
OFF_POSA_SEG: int = 0xf6
OFF_POSB_OFF: int = 0x3f4             # cell*4 pos tbls (draw B)
OFF_POSB_SEG: int = 0x3f6

# View-descriptor far ptrs (the structs the draw/erase fns write into then pass to the
# graphics overlay save-under / restore_bg_view callees).
OFF_VIEW_A_ERASE: int = 0x8d4
OFF_VIEW_A_DRAW: int = 0x8e0
OFF_VIEW_A_CLEAR: int = 0x8c0
OFF_VIEW_B_0: int = 0x8c8
OFF_VIEW_B_1: int = 0x8cc
OFF_VIEW_B_DRAW: int = 0x8d0
OFF_VIEW_B_CLEAR: int = 0x8bc
VIEW_DESC_LEN: int = 0x20             # view-descriptor struct (fields up to +0x1c used)

OFF_P1_SPRITE_PTR: int = 0x8884       # far ptr to blit descriptor (x,y,frame)
P1_SPRITE_DESC_LEN: int = 8

OFF_CURRENT_LEVEL: int = 0x79b2       # u8
OFF_COPYPROTECT: int = 0x119a         # s8
OFF_KEY_STATE_PTR: int = 0x4D42       # near ptr to g_key_state_table base

# ---------------------------------------------------------------------------
# Phase-8 channel-B caveat closure: the REAL layer-B populator + level (re)load path.
# Channel B has no per-cell allocator; it is spawn-populated by the level-load entity
# orchestrator spawn_and_draw_level_entities (1000:2a78). Level 1's decoded layer-B is
# genuinely EMPTY (raw BUM +0x30 is the vec-ENCODED bitstream, not the runtime grid), so
# to exercise the B steppers/draw/erase on ENGINE-populated records we (re)load a B-firing
# level (start_level loads+vec_decodes D{N}.PAV/DEC/BUM; load_current_level_data copies the
# decoded 0xc2 slice into the runtime tilemap), then invoke the orchestrator. Verified
# (spawn_oracle.py SCAN): levels 2/3/4/5/6/8/9 have a populated decoded layer-B; 1/7 do not.
# These three offsets are read live from the disasm of spawn_and_draw_level_entities (see
# spawn_model.md / spawn_oracle.py — same resolved addresses, not re-derived here).
OFF_SPAWN: int = 0x2a78               # spawn_and_draw_level_entities (channel-B+A populator)
OFF_START_LEVEL: int = 0x2d14         # loads+vec_decodes D{N}.PAV/DEC/BUM
OFF_LOAD_LEVEL_DATA: int = 0x32b0     # copies decoded 0xc2 header slice into the tilemap
B_FIRING_LEVEL: int = 2               # a level whose decoded layer-B is populated

# ---------------------------------------------------------------------------
# Hooked anim functions (Ghidra seg-1000 offsets)
# ---------------------------------------------------------------------------
FN_NAMES: Dict[int, str] = {
    0x69aa: "apply_cell_animation",
    0x14e4: "step_anim_channels_a",
    0x15a1: "step_anim_channels_b",
    0x165e: "draw_anim_channels_a",
    0x17c7: "draw_anim_channels_b",
    0x1a67: "erase_anim_channels_a",
    0x1b2b: "erase_anim_channels_b",
}
DRAW_A_OFF: int = 0x165e
DRAW_B_OFF: int = 0x17c7
ERASE_A_OFF: int = 0x1a67
ERASE_B_OFF: int = 0x1b2b
DRAW_FNS = {DRAW_A_OFF, DRAW_B_OFF}
ERASE_FNS = {ERASE_A_OFF, ERASE_B_OFF}

# per-fn view-descriptor ids captured at EXIT
VIEW_IDS: Dict[int, List[Tuple[int, int]]] = {
    DRAW_A_OFF: [(0, OFF_VIEW_A_ERASE), (1, OFF_VIEW_A_DRAW)],
    DRAW_B_OFF: [(2, OFF_VIEW_B_0), (3, OFF_VIEW_B_1), (4, OFF_VIEW_B_DRAW)],
    ERASE_A_OFF: [(5, OFF_VIEW_A_CLEAR)],
    ERASE_B_OFF: [(6, OFF_VIEW_B_CLEAR)],
}

# ---------------------------------------------------------------------------
# Trace format constants
# ---------------------------------------------------------------------------
TRACE_MAGIC: bytes = b"ANIMTRC1"
TRACE_VERSION: int = 1
# ANIMSNAP: 2 count bytes, 7*12 record bytes, 6 scalar u8, 4 u16 stream halves.
ANIMSNAP_FMT: str = "<BB" + ("B" * (7 * SLOT_REC_LEN)) + "BBBBBB" + "HHHH"
ANIMSNAP_SIZE: int = struct.calcsize(ANIMSNAP_FMT)

# Action codes that drive the channel-A allocator (from player.c / items.c):
#   0x24  land / lava-edge FX (player.c land path, p1_cell-8)
#   0x34  alt land FX (player.c)
#   0x59  'Y' exit-teleport relocate (items.c teleport_to_next_exit_tile)
ALLOC_ACTIONS = [0x24, 0x34, 0x59]


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

    # --- minimal VGA planar emulation (copied from p2_oracle.py / physics_oracle.py) ---
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

    # ---------------------------------------------------------------------------
    # DGROUP read/write helpers
    # ---------------------------------------------------------------------------
    def rd8(off: int) -> int:
        return uc.mem_read(DG_LIN + off, 1)[0]

    def rd_u16(off: int) -> int:
        return struct.unpack("<H", bytes(uc.mem_read(DG_LIN + off, 2)))[0]

    def rd_bytes(off: int, n: int) -> bytes:
        return bytes(uc.mem_read(DG_LIN + off, n))

    def wr8(off: int, v: int) -> None:
        uc.mem_write(DG_LIN + off, bytes([v & 0xFF]))

    def wr16(off: int, v: int) -> None:
        uc.mem_write(DG_LIN + off, struct.pack("<H", v & 0xFFFF))

    def read_far_at(off: int) -> Tuple[int, int]:
        o, s = struct.unpack("<HH", bytes(uc.mem_read(DG_LIN + off, 4)))
        return o, s

    def read_far_target(off: int, n: int) -> bytes:
        o, s = read_far_at(off)
        lin = (s * 16 + o) & 0xFFFFF
        try:
            return bytes(uc.mem_read(lin, n))
        except UcError:
            return b""

    def read_slot_recs() -> bytes:
        """Read all 7 (3 A + 4 B) 12-byte channel records via their far-ptr slot tables."""
        out = bytearray()
        for tbl_off, slots in ((OFF_A_TBL_OFF, A_SLOTS), (OFF_B_TBL_OFF, B_SLOTS)):
            seg_off = tbl_off + 2
            for i in range(slots):
                rec_off = struct.unpack("<H", bytes(
                    uc.mem_read(DG_LIN + tbl_off + i * 4, 2)))[0]
                rec_seg = struct.unpack("<H", bytes(
                    uc.mem_read(DG_LIN + seg_off + i * 4, 2)))[0]
                lin = (rec_seg * 16 + rec_off) & 0xFFFFF
                try:
                    out += bytes(uc.mem_read(lin, SLOT_REC_LEN))
                except UcError:
                    out += b"\x00" * SLOT_REC_LEN
        return bytes(out)

    def write_slot_rec(layer: str, idx: int, rec: bytes) -> None:
        tbl_off = OFF_A_TBL_OFF if layer == "a" else OFF_B_TBL_OFF
        rec_off = struct.unpack("<H", bytes(
            uc.mem_read(DG_LIN + tbl_off + idx * 4, 2)))[0]
        rec_seg = struct.unpack("<H", bytes(
            uc.mem_read(DG_LIN + tbl_off + 2 + idx * 4, 2)))[0]
        lin = (rec_seg * 16 + rec_off) & 0xFFFFF
        uc.mem_write(lin, rec[:SLOT_REC_LEN])

    def read_slot_active(layer: str, idx: int) -> int:
        tbl_off = OFF_A_TBL_OFF if layer == "a" else OFF_B_TBL_OFF
        rec_off = struct.unpack("<H", bytes(
            uc.mem_read(DG_LIN + tbl_off + idx * 4, 2)))[0]
        rec_seg = struct.unpack("<H", bytes(
            uc.mem_read(DG_LIN + tbl_off + 2 + idx * 4, 2)))[0]
        lin = (rec_seg * 16 + rec_off) & 0xFFFFF
        return uc.mem_read(lin, 1)[0]

    def tilemap_byte(cell: int) -> int:
        o, s = read_far_at(OFF_TILEMAP_PTR)
        lin = (s * 16 + o + (cell & 0xFF)) & 0xFFFFF
        try:
            return uc.mem_read(lin, 1)[0]
        except UcError:
            return 0

    def snap() -> bytes:
        recs = read_slot_recs()
        return struct.pack(
            ANIMSNAP_FMT,
            A_SLOTS, B_SLOTS,
            *recs,
            rd8(OFF_ANIM_TARGET_CELL), rd8(OFF_G_ANIM_CHANNEL_IDX),
            rd8(OFF_G_ANIM_CUR_CMD_BYTE), rd8(OFF_B_LOOP_IDX),
            rd8(OFF_B_CUR_FRAME_BYTE), rd8(OFF_CURRENT_LEVEL),
            *read_far_at(OFF_G_ANIM_STREAM_PTR), *read_far_at(OFF_B_STREAM_PTR))

    # ---------------------------------------------------------------------------
    # anim-function hooks (entry + exit via dynamic return-address hook)
    # ---------------------------------------------------------------------------
    capturing = {"on": False}
    cur_records: List[bytes] = []
    reached_fns: collections.Counter = collections.Counter()
    pending_exit: dict = {}
    exit_hook_lins: set = set()
    fn_name_list: List[str] = list(dict.fromkeys(FN_NAMES.values()))
    fn_name_idx = {n: i for i, n in enumerate(fn_name_list)}

    def emit_record(fn_off: int, entry_snap: bytes, exit_snap: bytes,
                    tile_cell: int, tile_e: int, tile_x: int) -> None:
        rec = struct.pack("<HH", fn_off, fn_name_idx[FN_NAMES[fn_off]])
        rec += entry_snap + exit_snap
        rec += struct.pack("<BBB", tile_cell & 0xFF, tile_e & 0xFF, tile_x & 0xFF)
        # descriptor (draw fns only)
        if fn_off in DRAW_FNS:
            desc = read_far_target(OFF_P1_SPRITE_PTR, P1_SPRITE_DESC_LEN)
            rec += struct.pack("<BB", 1, len(desc)) + desc
        else:
            rec += struct.pack("<BB", 0, 0)
        # view descriptors (draw/erase fns)
        views = VIEW_IDS.get(fn_off, [])
        rec += struct.pack("<B", len(views))
        for vid, voff in views:
            vb = read_far_target(voff, VIEW_DESC_LEN)
            rec += struct.pack("<BB", vid, len(vb)) + vb
        cur_records.append(rec)

    def hook_fn_entry(uc: Uc, addr: int, size: int, _: object) -> None:
        if not capturing["on"]:
            return
        fn_off = (addr - CODE_LIN) & 0xFFFF
        if fn_off not in FN_NAMES:
            return
        reached_fns[fn_off] += 1
        tile_cell = rd8(OFF_ANIM_TARGET_CELL)
        entry_snap = snap()
        tile_e = tilemap_byte(tile_cell)
        ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc.mem_read(ss * 16 + sp, 2)))[0]
        ret_lin = (CODE_LIN + ret_off) & 0xFFFFF
        pending_exit.setdefault(ret_lin, []).append(
            (fn_off, entry_snap, tile_cell, tile_e))
        if ret_lin not in exit_hook_lins:
            exit_hook_lins.add(ret_lin)
            uc.hook_add(UC_HOOK_CODE, hook_fn_exit, None, ret_lin, ret_lin)

    def hook_fn_exit(uc: Uc, addr: int, size: int, _: object) -> None:
        stack = pending_exit.get(addr)
        if not stack:
            return
        (fn_off, entry_snap, tile_cell, tile_e) = stack.pop()
        exit_snap = snap()
        tile_x = tilemap_byte(tile_cell)
        emit_record(fn_off, entry_snap, exit_snap, tile_cell, tile_e, tile_x)

    for off in FN_NAMES:
        lin = CODE_LIN + off
        uc.hook_add(UC_HOOK_CODE, hook_fn_entry, None, lin, lin)

    # Pre-register the exit hook at the synthetic-call return address (STOP_OFF). A code
    # hook added MID-emulation does not take effect until the NEXT emu_start in Unicorn,
    # so the FIRST synthetic call to a fn would otherwise miss its exit record. Every
    # call_engine_fn returns to CODE_LIN+STOP_OFF, so pre-registering it here (before any
    # capture starts) makes the very first allocator call's exit record fire.
    STOP_OFF = 0x0008
    exit_hook_lins.add(CODE_LIN + STOP_OFF)
    uc.hook_add(UC_HOOK_CODE, hook_fn_exit, None,
                CODE_LIN + STOP_OFF, CODE_LIN + STOP_OFF)

    # ---------------------------------------------------------------------------
    # Synthetic near-call into an engine fn (seeded scenarios) — Phase-3/4 pattern.
    # STOP_OFF (the HLT return sentinel) is defined above where its exit hook is
    # pre-registered.
    # ---------------------------------------------------------------------------
    def call_engine_fn(fn_off: int, arg_word: Optional[int] = None,
                       count: int = 20_000_000) -> None:
        uc.mem_write(CODE_LIN + STOP_OFF, b"\xF4")  # HLT sentinel
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
            uc.emu_start(CODE_LIN + fn_off, 0, count=count)
        except UcError as e:
            tr["call_err"] = str(e)
        finally:
            uc.hook_del(h)
            if arg_word is not None:
                sp2 = uc.reg_read(UC_X86_REG_SP) & 0xFFFF
                uc.reg_write(UC_X86_REG_SP, (sp2 + 2) & 0xFFFF)

    # ---------------------------------------------------------------------------
    # Boot to level 1 (identical approach to p2_oracle / physics_oracle)
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

    print("[anim_oracle] booting BUMPY (level %d)..." % LEVEL, flush=True)

    while total_instr < 400_000_000:
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e); tr["err"] = err; break
        total_instr += CHUNK
        if total_instr % 40_000_000 == 0:
            print("[anim_oracle] %dM instr, countdown=%s" % (
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
            print("[anim_oracle] level loaded (%s) at chunk %d — settling %d ticks" % (
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
        print("[anim_oracle] ERROR: premature exit/fault during boot: exit=%s fault=%s err=%s" % (
            tr.get("exit"), tr.get("fault"), err), flush=True)
        return
    if not opened(BUMNAME):
        print("[anim_oracle] ERROR: level %s never loaded after %dM instructions" % (
            BUMNAME, total_instr // 1_000_000), flush=True)
        return

    print("[anim_oracle] boot complete. Files: %s" % (
        [o[1] for o in tr.get("fileops", [])]), flush=True)

    # Discover post-boot anim state for the report.
    boot_recs = read_slot_recs()
    boot_target = rd8(OFF_ANIM_TARGET_CELL)
    a_active0 = [read_slot_active("a", i) for i in range(A_SLOTS)]
    b_active0 = [read_slot_active("b", i) for i in range(B_SLOTS)]
    print("[anim_oracle] boot anim_target_cell=0x%02x A-active=%s B-active=%s" % (
        boot_target, a_active0, b_active0), flush=True)
    print("[anim_oracle] boot A slot recs: %s" % " ".join(
        boot_recs[i * SLOT_REC_LEN:(i + 1) * SLOT_REC_LEN].hex()
        for i in range(A_SLOTS)), flush=True)
    print("[anim_oracle] boot B slot recs: %s" % " ".join(
        boot_recs[(A_SLOTS + i) * SLOT_REC_LEN:(A_SLOTS + i + 1) * SLOT_REC_LEN].hex()
        for i in range(B_SLOTS)), flush=True)

    # Pick a live cell to drive the allocator: prefer the engine's current target, else a
    # mid-field cell with a valid tilemap entry.
    SEED_CELL = boot_target if boot_target not in (0x00, 0xff) else 0x40

    # Snapshot clean post-boot machine state for per-scenario restore.
    boot_ram = bytes(uc.mem_read(0, RAM))
    boot_ctx = uc.context_save()

    def restore_boot_state() -> None:
        uc.mem_write(0, boot_ram)
        uc.context_restore(boot_ctx)
        tr["fault"] = None
        tr["exit"] = None

    # ---------------------------------------------------------------------------
    # Phase-8 channel-B caveat closure: (re)load a B-firing level and run the REAL
    # populator. Mirrors tools/spawn_oracle.py's load_level: set current_level, (re)run
    # start_level (load + vec_decode D{N}.PAV/DEC/BUM) then load_current_level_data (copy
    # the decoded 0xc2 slice into the runtime tilemap). The level-load + spawn engine
    # code itself calls the anim draw/erase leaves internally; we hold the anim-fn capture
    # OFF across that work so the only captured B records are the explicit step/draw/erase
    # calls we issue afterwards, operating on the records the engine just populated.
    # ---------------------------------------------------------------------------
    def load_b_firing_level(n: int) -> Tuple[int, int, int]:
        wr8(OFF_CURRENT_LEVEL, n)
        uc.mem_write(DG_LIN + OFF_COPYPROTECT, bytes([1]))   # skip copy-protect
        call_engine_fn(OFF_START_LEVEL, count=80_000_000)
        wr8(OFF_CURRENT_LEVEL, n)
        uc.mem_write(DG_LIN + OFF_COPYPROTECT, bytes([1]))
        call_engine_fn(OFF_LOAD_LEVEL_DATA, count=40_000_000)
        # decoded-tilemap non-zero counts for layers A/B/C (verifies B is populated)
        o, s = read_far_at(OFF_TILEMAP_PTR)
        base = (s * 16 + o) & 0xFFFFF
        LAYER_LEN = 0x30
        try:
            tm = bytes(uc.mem_read(base, LAYER_LEN * 3))
        except UcError:
            tm = b"\x00" * (LAYER_LEN * 3)
        nz = [sum(1 for i in range(LAYER_LEN) if tm[L * LAYER_LEN + i] != 0)
              for L in range(3)]
        return nz[0], nz[1], nz[2]

    # Resolved table slices for the model (read once post-boot).
    def tbl_slice(off: int, n: int) -> bytes:
        return rd_bytes(off, n)

    table_dump = dict(
        tiledef_off=tbl_slice(OFF_A_TILEDEF_OFF, 0x100),
        tiledef_seg=tbl_slice(OFF_A_TILEDEF_SEG, 0x100),
        a_frame_off=tbl_slice(OFF_A_FRAME_OFF, 0x100),
        a_frame_seg=tbl_slice(OFF_A_FRAME_SEG, 0x100),
        b_frame_off=tbl_slice(OFF_B_FRAME_OFF, 0x100),
        b_frame_seg=tbl_slice(OFF_B_FRAME_SEG, 0x100),
    )

    # ---------------------------------------------------------------------------
    # Scenarios
    # ---------------------------------------------------------------------------
    Scenario = Tuple[int, str, str]
    scenario_results: List[dict] = []

    def run_one(sc_id: int, name: str, note: str, body) -> None:
        restore_boot_state()
        cur_records.clear()
        capturing["on"] = True
        pre: dict = {}
        try:
            body(pre)
        except UcError as e:
            tr["scn_err"] = str(e)
        capturing["on"] = False
        recs = list(cur_records)
        scenario_results.append(dict(id=sc_id, name=name, note=note,
                                     recs=recs, pre=pre))
        print("[anim_oracle] === scenario %d (%s): %d records ===" % (
            sc_id, name, len(recs)), flush=True)

    # --- Scenario 1: live engine-tick anim pipeline ---------------------------------
    # Run a handful of real game ticks (fire_int 8) and let the engine drive the anim
    # fns naturally from its game loop (game.c step/draw/erase). Captures whatever the
    # engine had spawned (channel B is spawn-populated, channel A from any land-FX).
    def scn_live_ticks(pre):
        for sc in (0x3D, 0x41, 0x39, 0x1C):
            set_key(sc, False)
        n = 0
        for _ in range(40):
            try:
                fire_int(8)
                uc.emu_start(cur_lin(), 0, count=4_000_000)
            except UcError as e:
                tr["scn_err"] = str(e); break
            n += 1
            if tr.get("exit") is not None or tr.get("fault"):
                break
        pre["ticks"] = n
        pre["a_active"] = [read_slot_active("a", i) for i in range(A_SLOTS)]
        pre["b_active"] = [read_slot_active("b", i) for i in range(B_SLOTS)]
    run_one(1, "live_ticks",
            "Run ~40 real engine ticks (int 8) post-boot; let the game loop drive the "
            "anim step/draw/erase fns naturally. Captures the engine's spawn-populated "
            "channel state (esp. channel B, which has no allocator).",
            scn_live_ticks)

    # --- Scenarios 2a-2c: channel-A allocator (apply_cell_animation) ----------------
    # Seed anim_target_cell to a live cell, invoke apply_cell_animation(action) for each
    # action code the player/item spines use. Verifies a free A slot is claimed (active
    # 0->1), its [1]=cell, stream ptr stamped from the action's tile-def, tilemap[cell]
    # stamped with the tile-def's first byte.
    def make_alloc(action: int):
        def body(pre):
            wr8(OFF_ANIM_TARGET_CELL, SEED_CELL)
            pre["action"] = action
            pre["cell"] = SEED_CELL
            pre["tile_before"] = tilemap_byte(SEED_CELL)
            pre["a_active_before"] = [read_slot_active("a", i) for i in range(A_SLOTS)]
            call_engine_fn(0x69aa, action)        # apply_cell_animation(action)
            pre["a_active_after"] = [read_slot_active("a", i) for i in range(A_SLOTS)]
            pre["tile_after"] = tilemap_byte(SEED_CELL)
        return body
    for k, action in enumerate(ALLOC_ACTIONS):
        run_one(2 + k, "alloc_action_%02x" % action,
                "Seed anim_target_cell=0x%02x; invoke apply_cell_animation(0x%02x). "
                "Claims a channel-A slot keyed on the cell + stamps the action's tile-def "
                "into the base tilemap layer." % (SEED_CELL, action),
                make_alloc(action))

    # --- Scenario 3: allocate A then step/draw/erase the populated channel ----------
    # Drive the full channel-A lifecycle on a seeded cell: allocate, then advance the
    # stepper several times (consuming the byte-stream / frame table), draw it (erase
    # old + blit + save-under), then erase it.
    def scn_a_lifecycle(pre):
        wr8(OFF_ANIM_TARGET_CELL, SEED_CELL)
        pre["cell"] = SEED_CELL
        pre["action"] = ALLOC_ACTIONS[0]
        call_engine_fn(0x69aa, ALLOC_ACTIONS[0])   # apply_cell_animation
        pre["a_active_after_alloc"] = [read_slot_active("a", i) for i in range(A_SLOTS)]
        for _ in range(8):
            call_engine_fn(0x14e4)                 # step_anim_channels_a
            call_engine_fn(0x165e)                 # draw_anim_channels_a
            call_engine_fn(0x1a67)                 # erase_anim_channels_a
        pre["a_active_after_steps"] = [read_slot_active("a", i) for i in range(A_SLOTS)]
        pre["cur_cmd_byte"] = rd8(OFF_G_ANIM_CUR_CMD_BYTE)
    run_one(5, "a_lifecycle",
            "Allocate a channel-A slot (apply_cell_animation), then run step_a + draw_a "
            "+ erase_a x8 to advance the channel along its byte-stream / frame table and "
            "render+erase it. Full channel-A lifecycle.",
            scn_a_lifecycle)

    # --- Scenario 4: channel B step/draw/erase on REAL spawn-populated records --------
    # PHASE-8 CHANNEL-B CAVEAT CLOSURE. Channel B has NO per-cell allocator; the real
    # populator is spawn_and_draw_level_entities (1000:2a78), the level-load entity
    # orchestrator (reconstructed + validated in Phase-8 T2). Phase 5 could not reach it
    # (deferred), so the B step/draw/erase were exercised on a SYNTHETIC hand-seeded slot
    # record — documented there as a coverage caveat. We now CLOSE it: (re)load a B-firing
    # level (level 2; level 1's decoded layer-B is genuinely empty), run the REAL
    # orchestrator to populate the channel-B slot table from the level's layer-B grid,
    # then run step_b + draw_b + erase_b on those ENGINE-populated records — no synthetic
    # seed. The captured records carry the real B channel-record table as ENTRY/EXIT snaps,
    # so the host replay harness (anim_chan_ctest.c) validates B's per-tick bodies against
    # records the engine itself produced.
    # The B record's per-tick STREAM ptr (rec bytes [2..5]) is the one B input the REAL
    # spawn does NOT populate: spawn sets the B record's cell (+1) and frame-DATA ptr
    # (+8/+10) from the level's layer-B grid, and during its active window (slot-0
    # active=1, frame=1) calls draw_b/erase_b per cell — but the per-tick step_b consumes
    # a separate stream ptr that the spawn path never writes (confirmed from the 1000:2a78
    # decomp + the spawn_oracle level-2 B record `001e00000000000002001700`: bytes [2..5]
    # are 0). To exercise step_b's stream-advance on the ENGINE-populated record we point
    # that one field at a small in-DGROUP byte-stream; every OTHER B-record field (cell,
    # frame-data ptr) and the active/frame activation values are taken verbatim from the
    # real spawn. This is the documented residual: cell + frame-data ptr are engine-real;
    # only the step-B stream ptr is harness-supplied (the engine path that writes it is
    # not in any captured trace).
    SCRATCH_STREAM_OFF = 0x6e00   # unused DGROUP scratch for the step-B stream ptr only
    def scn_b_lifecycle(pre):
        capturing["on"] = False                    # don't capture the load/spawn internals
        a_nz, b_nz, c_nz = load_b_firing_level(B_FIRING_LEVEL)
        pre["level"] = B_FIRING_LEVEL
        pre["decoded_nz_ABC"] = (a_nz, b_nz, c_nz)
        # Run the REAL populator: stamps the channel-B slot table (cell + frame-data ptr)
        # from the level's layer-B grid; leaves slot-0 active=0 at exit (engine end-state).
        call_engine_fn(OFF_SPAWN, count=80_000_000)
        real_b0 = read_slot_recs()[A_SLOTS * SLOT_REC_LEN:
                                   (A_SLOTS + 1) * SLOT_REC_LEN]
        pre["b0_after_spawn"] = real_b0.hex()
        pre["b_recs_after_spawn"] = " ".join(
            read_slot_recs()[(A_SLOTS + i) * SLOT_REC_LEN:
                             (A_SLOTS + i + 1) * SLOT_REC_LEN].hex()
            for i in range(B_SLOTS))
        # Re-create the engine's OWN mid-spawn active state on the engine-populated B
        # slot-0: active=1 + frame=1 (the exact values spawn step-1 writes), keeping the
        # spawn's real cell + frame-data ptr. Supply the one missing field — the step-B
        # stream ptr — from DGROUP scratch so step_b advances along a real byte-stream.
        stream = bytes([0x01, 0x02, 0x01, 0xff, 0xff, 0xff])
        uc.mem_write(DG_LIN + SCRATCH_STREAM_OFF, stream)
        rec = bytearray(real_b0)
        rec[0] = 1                                  # active=1 (engine spawn step-1 value)
        rec[6] = 1                                  # frame=1 (engine spawn step-1 value)
        struct.pack_into("<HH", rec, 2, SCRATCH_STREAM_OFF, DG_SEG)  # step-B stream ptr
        write_slot_rec("b", 0, bytes(rec))
        pre["b0_active_record"] = bytes(rec).hex()
        pre["stream"] = stream.hex()
        # Capture the explicit B per-tick lifecycle on the engine-populated record.
        capturing["on"] = True
        for _ in range(6):
            call_engine_fn(0x15a1)                 # step_anim_channels_b
            call_engine_fn(0x17c7)                 # draw_anim_channels_b
            call_engine_fn(0x1b2b)                 # erase_anim_channels_b
        pre["b_active_after"] = [read_slot_active("b", i) for i in range(B_SLOTS)]
        pre["cur_frame_byte"] = rd8(OFF_B_CUR_FRAME_BYTE)
    run_one(6, "b_lifecycle_real_spawn",
            "Channel B has no allocator. CAVEAT CLOSURE (Phase 8): (re)load a B-firing "
            "level (level %d) and run the REAL populator spawn_and_draw_level_entities "
            "(1000:2a78); the channel-B slot-0 record's cell + frame-data ptr are stamped "
            "from the level's real layer-B grid. Re-apply the engine's own active=1/frame=1 "
            "(spawn step-1 values), supply the one field spawn doesn't write (the step-B "
            "stream ptr), then run step_b + draw_b + erase_b x6 on that ENGINE-populated "
            "record. Replaces the Phase-5 fully-synthetic seed (cell + frame-data ptr are "
            "now engine-real; only the step-B stream ptr remains harness-supplied)."
            % B_FIRING_LEVEL,
            scn_b_lifecycle)

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
            f.write(struct.pack("<BB", 1, LEVEL))   # seeded=1
            f.write(struct.pack("<I", len(sr["recs"])))
            for r in sr["recs"]:
                f.write(r)
    trace_bytes = os.path.getsize(OUT_TRACE)
    print("[anim_oracle] wrote %s (%d B)" % (OUT_TRACE, trace_bytes), flush=True)

    # ---------------------------------------------------------------------------
    # Decode records for the model md
    # ---------------------------------------------------------------------------
    def decode_records(recs: List[bytes]) -> List[dict]:
        out = []
        for r in recs:
            fn_off, name_idx = struct.unpack_from("<HH", r, 0)
            o = 4
            ent = r[o:o + ANIMSNAP_SIZE]; o += ANIMSNAP_SIZE
            ex = r[o:o + ANIMSNAP_SIZE]; o += ANIMSNAP_SIZE
            tile_cell, tile_e, tile_x = struct.unpack_from("<BBB", r, o); o += 3
            desc_kind, desc_len = struct.unpack_from("<BB", r, o); o += 2
            desc = r[o:o + desc_len]; o += desc_len
            (nview,) = struct.unpack_from("<B", r, o); o += 1
            views = []
            for _ in range(nview):
                vid, vlen = struct.unpack_from("<BB", r, o); o += 2
                views.append((vid, r[o:o + vlen])); o += vlen
            out.append(dict(fn=FN_NAMES[fn_off], fn_off=fn_off, ent=ent, ex=ex,
                            tile_cell=tile_cell, tile_e=tile_e, tile_x=tile_x,
                            desc=desc, views=views))
        return out

    def snap_recs(snap_bytes: bytes) -> List[bytes]:
        """Slice the 7 12-byte channel records out of an ANIMSNAP blob."""
        out = []
        base = 2
        for i in range(A_SLOTS + B_SLOTS):
            out.append(snap_bytes[base + i * SLOT_REC_LEN:
                                  base + (i + 1) * SLOT_REC_LEN])
        return out

    # ---------------------------------------------------------------------------
    # anim_model.md
    # ---------------------------------------------------------------------------
    L: List[str] = []
    L.append("# Bumpy Phase-5 anim-channel FX capture model (discovery)\n\n")
    L.append("Generated by `tools/anim_oracle.py`. Capture granularity = anim-channel "
             "FUNCTION-CALL boundary (entry+exit) for the 7 anim fns.\n\n")
    L.append("All allocator/step/draw/erase scenarios are **seeded** (the anim subsystem "
             "is fed by the player physics spine + item collect/teleport, not the "
             "keyboard): the precondition globals are set, then the REAL engine fn is "
             "invoked at its entry IP via a synthetic near-call (unmodified original "
             "body). Scenario 1 (`live_ticks`) instead runs real engine ticks and lets "
             "the game loop drive the anim fns naturally.\n\n")

    L.append("## Channel-record layout (12 B; confirmed from the steppers' decomp)\n\n")
    L.append("| offset | field |\n|---|---|\n")
    L.append("| [0] | active flag (0 free / 1 active / 0xff end-of-table terminator) |\n")
    L.append("| [1] | cell |\n")
    L.append("| [2..5] | stream ptr (far: off@+2, seg@+4) |\n")
    L.append("| [6] | cur frame byte |\n")
    L.append("| [7] | pad |\n")
    L.append("| [8..11] | frame-data ptr (far: off@+8, seg@+10) |\n\n")
    L.append("Channel A: %d slots via `anim_channels_a_tbl` 0x%04x / `_seg_tbl` 0x%04x "
             "(stride 4). Channel B: %d slots via `anim_channels_b_tbl` 0x%04x / `_seg_tbl` "
             "0x%04x.\n\n" % (A_SLOTS, OFF_A_TBL_OFF, OFF_A_TBL_SEG,
                              B_SLOTS, OFF_B_TBL_OFF, OFF_B_TBL_SEG))

    L.append("## Resolved anim DGROUP addresses (Ghidra DGROUP 0x203b offsets)\n\n")
    L.append("Read live via Ghidra MCP from the disassembly operands of the 7 anim fns.\n\n")
    L.append("| symbol | offset | provenance |\n|---|---|---|\n")
    rows = [
        ("anim_channels_a_tbl", 0x4c70, "far-ptr slot tbl off-halves; step/draw/erase A"),
        ("anim_channels_a_seg_tbl", 0x4c72, "far-ptr slot tbl seg-halves"),
        ("anim_channels_b_tbl", 0x4cbc, "far-ptr slot tbl off-halves; step/draw/erase B"),
        ("anim_channels_b_seg_tbl", 0x4cbe, "far-ptr slot tbl seg-halves"),
        ("anim_target_cell", 0x856f, "CMP/MOV [0x856f] in apply_cell_animation 69aa"),
        ("anim_a_tiledef_off", 0x2ede, "action*4 -> far-ptr tile-def (off half) in 69aa"),
        ("anim_a_tiledef_seg", 0x2ee0, "action*4 -> far-ptr tile-def (seg half) in 69aa"),
        ("tilemap_ptr", 0xa0d8, "LES BX,[0xa0d8] base tilemap layer in 69aa"),
        ("g_anim_channel_idx", 0x856c, "MOV [0x856c],0 loop idx in step_a 14e4"),
        ("g_anim_stream_ptr", 0xa0be, "working stream far ptr in step_a (off/seg @0xa0be/0xa0c0)"),
        ("g_anim_cur_cmd_byte", 0x8578, "cur cmd byte in step_a 14e4"),
        ("anim_a_frame_off", 0x3d6a, "cmd*4 -> far-ptr frame data (off) in step_a"),
        ("anim_a_frame_seg", 0x3d6c, "cmd*4 -> far-ptr frame data (seg) in step_a"),
        ("anim_b_loop_idx", 0x8566, "MOV [0x8566],0 loop idx in step_b 15a1"),
        ("anim_b_stream_ptr", 0xa0c2, "working stream far ptr in step_b (off/seg @0xa0c2/0xa0c4)"),
        ("anim_b_cur_frame_byte", 0x8579, "cur frame byte in step_b 15a1"),
        ("anim_b_frame_off", 0x40a6, "frame*4 -> far-ptr frame data (off) in step_b"),
        ("anim_b_frame_seg", 0x40a8, "frame*4 -> far-ptr frame data (seg) in step_b"),
        ("anim_a_grid_off", 0x32be, "cell*4 grid-coord tbl (off) in draw/erase A"),
        ("anim_a_grid_seg", 0x32c0, "cell*4 grid-coord tbl (seg) in draw/erase A"),
        ("anim_b_grid_off", 0x343e, "cell*4 grid-coord tbl (off) in draw/erase B"),
        ("anim_b_grid_seg", 0x3440, "cell*4 grid-coord tbl (seg) in draw/erase B"),
        ("posA_off", 0xf4, "cell*4 pos tbl (off) in draw A"),
        ("posA_seg", 0xf6, "cell*4 pos tbl (seg) in draw A"),
        ("posB_off", 0x3f4, "cell*4 pos tbl (off) in draw B"),
        ("posB_seg", 0x3f6, "cell*4 pos tbl (seg) in draw B"),
        ("anim_a_erase_view", 0x8d4, "view descriptor far ptr; draw A erase pass (->0x80bc)"),
        ("anim_a_draw_view", 0x8e0, "view descriptor far ptr; draw A save-under (->0x93b8)"),
        ("anim_a_clear_view", 0x8c0, "view descriptor far ptr; erase A (restore_bg_view)"),
        ("anim_b_view0", 0x8c8, "view descriptor far ptr; draw B pass 0 (->0x80ac)"),
        ("anim_b_view1", 0x8cc, "view descriptor far ptr; draw B passes (->0x80ac/0x80bc)"),
        ("anim_b_draw_view", 0x8d0, "view descriptor far ptr; draw B save-under (->0x93b8)"),
        ("anim_b_clear_view", 0x8bc, "view descriptor far ptr; erase B (restore_bg_view)"),
        ("p1_sprite", 0x8884, "blit descriptor far ptr; blit_sprite(0x792e,0x203b)"),
        ("p1_sprite_desc_off", 0x792e, "DGROUP off of descriptor passed to blit_sprite"),
        ("current_level", 0x79b2, "boot level select"),
        ("copyprotect_flag", 0x119a, "boot guard"),
    ]
    for sym, off, prov in rows:
        L.append("| %s | 0x%04x | %s |\n" % (sym, off, prov))
    L.append("\n")

    L.append("## Hooked anim functions (Ghidra seg-1000 offsets)\n\n")
    L.append("| addr (1000:off) | name | role |\n|---|---|---|\n")
    roles = {
        0x69aa: "channel-A allocator (keyed on anim_target_cell)",
        0x14e4: "step 3 A channels along their byte-streams",
        0x15a1: "step 4 B channels along their byte-streams",
        0x165e: "erase old cell + blit_sprite + save-under (A)",
        0x17c7: "erase old cell + blit_sprite + save-under (B)",
        0x1a67: "restore_bg_view current cells (A)",
        0x1b2b: "restore_bg_view current cells (B)",
    }
    for off, nm in sorted(FN_NAMES.items()):
        L.append("| 1000:%04x | %s | %s |\n" % (off, nm, roles[off]))
    L.append("\n")

    # ---- Per-action tile-def table (channel-A allocator) ---------------------------
    L.append("## Channel-A per-action tile-def table (0x2ede off / 0x2ee0 seg)\n\n")
    L.append("`apply_cell_animation(action_code)` reads a far ptr at "
             "`[action_code*4 + 0x2ede/0x2ee0]`; the first byte of the pointed tile-def "
             "is stamped into `tilemap[anim_target_cell]`, and bytes [+2..+5] become the "
             "channel's stream ptr.\n\n")
    L.append("| action | tiledef far ptr (off:seg) |\n|---|---|\n")
    for action in ALLOC_ACTIONS:
        o = struct.unpack_from("<H", table_dump["tiledef_off"], action * 4)[0] \
            if action * 4 + 2 <= len(table_dump["tiledef_off"]) else 0
        s = struct.unpack_from("<H", table_dump["tiledef_seg"], action * 4)[0] \
            if action * 4 + 2 <= len(table_dump["tiledef_seg"]) else 0
        L.append("| 0x%02x | %04x:%04x |\n" % (action, o, s))
    L.append("\n")

    # ---- Frame tables --------------------------------------------------------------
    def fmt_frame_tbl(off_dump: bytes, seg_dump: bytes, n: int) -> str:
        ents = []
        for i in range(n):
            if i * 4 + 2 > len(off_dump):
                break
            o = struct.unpack_from("<H", off_dump, i * 4)[0]
            s = struct.unpack_from("<H", seg_dump, i * 4)[0]
            ents.append("[%d]=%04x:%04x" % (i, o, s))
        return " ".join(ents)
    L.append("## Frame tables (stepper-indexed far-ptr tables)\n\n")
    L.append("Channel A frame table (0x3d6a off / 0x3d6c seg), first 16 entries "
             "(cmd byte * 4):\n\n`%s`\n\n" % fmt_frame_tbl(
                 table_dump["a_frame_off"], table_dump["a_frame_seg"], 16))
    L.append("Channel B frame table (0x40a6 off / 0x40a8 seg), first 16 entries "
             "(frame byte * 4):\n\n`%s`\n\n" % fmt_frame_tbl(
                 table_dump["b_frame_off"], table_dump["b_frame_seg"], 16))

    # ---- Boot anim state -----------------------------------------------------------
    L.append("## Post-boot anim state (level %d)\n\n" % LEVEL)
    L.append("- anim_target_cell = 0x%02x\n" % boot_target)
    L.append("- channel-A active flags = %s\n" % a_active0)
    L.append("- channel-B active flags = %s\n" % b_active0)
    for i in range(A_SLOTS):
        L.append("- A slot %d: `%s`\n" % (
            i, boot_recs[i * SLOT_REC_LEN:(i + 1) * SLOT_REC_LEN].hex()))
    for i in range(B_SLOTS):
        L.append("- B slot %d: `%s`\n" % (
            i, boot_recs[(A_SLOTS + i) * SLOT_REC_LEN:(A_SLOTS + i + 1) * SLOT_REC_LEN].hex()))
    L.append("\n")

    # ---- Per-scenario detail -------------------------------------------------------
    for sr in scenario_results:
        dec = decode_records(sr["recs"])
        L.append("## Scenario %d — %s\n\n" % (sr["id"], sr["name"]))
        L.append("- **seeded**. %s\n" % sr["note"])
        if sr["pre"]:
            L.append("- pre/post: %s\n" % ", ".join(
                "%s=%s" % (k, v) for k, v in sr["pre"].items()))
        L.append("- records: %d (per-fn: %s)\n\n" % (
            len(dec), dict(collections.Counter(d["fn"] for d in dec))))
        if not dec:
            L.append("- (no anim-fn calls captured in this scenario)\n\n")
            continue
        L.append("| # | fn | tgt cell | tile e->x | A-active e->x | B-active e->x |\n")
        L.append("|---|---|---|---|---|---|\n")
        for i, d in enumerate(dec[:40]):
            er = snap_recs(d["ent"]); xr = snap_recs(d["ex"])
            a_e = [er[j][0] for j in range(A_SLOTS)]
            a_x = [xr[j][0] for j in range(A_SLOTS)]
            b_e = [er[A_SLOTS + j][0] for j in range(B_SLOTS)]
            b_x = [xr[A_SLOTS + j][0] for j in range(B_SLOTS)]
            L.append("| %d | %s | 0x%02x | 0x%02x->0x%02x | %s->%s | %s->%s |\n" % (
                i, d["fn"], d["tile_cell"], d["tile_e"], d["tile_x"],
                a_e, a_x, b_e, b_x))
        if len(dec) > 40:
            L.append("\n(+%d more records)\n" % (len(dec) - 40))
        # show first draw descriptor + views
        for d in dec:
            if d["desc"]:
                L.append("\n- first p1_sprite blit descriptor (8 B, %s): `%s` "
                         "(x=%d y=%d frame=%d)\n" % (
                             d["fn"], d["desc"].hex(),
                             *struct.unpack_from("<HHH", d["desc"] + b"\x00" * 8, 0)[:3]))
                break
        for d in dec:
            if d["views"]:
                L.append("- first view descriptors (%s):\n" % d["fn"])
                for vid, vb in d["views"]:
                    L.append("  - view[%d] (%d B): `%s`\n" % (vid, len(vb), vb.hex()))
                break
        L.append("\n")

    os.makedirs(os.path.dirname(OUT_MODEL), exist_ok=True)
    with open(OUT_MODEL, "w") as f:
        f.write("".join(L))
    print("[anim_oracle] wrote %s" % OUT_MODEL, flush=True)

    # ---------------------------------------------------------------------------
    # Console summary
    # ---------------------------------------------------------------------------
    print("\n[anim_oracle] REACHED anim functions:", flush=True)
    for off in sorted(FN_NAMES):
        cnt = reached_fns.get(off, 0)
        flag = "" if cnt else "  <-- NOT REACHED"
        print("   1000:%04x  %-24s x%d%s" % (off, FN_NAMES[off], cnt, flag), flush=True)
    total_recs = sum(len(sr["recs"]) for sr in scenario_results)
    print("[anim_oracle] total records: %d across %d scenarios" % (
        total_recs, len(scenario_results)), flush=True)
    if err:
        print("[anim_oracle] emu error:", err, flush=True)
    if tr.get("call_err"):
        print("[anim_oracle] call_err:", tr["call_err"], flush=True)
    if tr.get("scn_err"):
        print("[anim_oracle] scn_err:", tr["scn_err"], flush=True)


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
