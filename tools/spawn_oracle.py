#!/usr/bin/env python3
"""spawn_oracle.py — Phase-8 entity-spawn CAPTURE oracle.

Boots the real (unpacked) BUMPY.EXE under Unicorn — reusing the boot + DGROUP +
int / VGA + level-load scaffold of tools/anim_oracle.py / tools/items_oracle.py
(deliberately NOT refactoring those) — loads a REAL level (so the `tilemap`, the
BUM header `+0x90..+0x96`, and the spawn tables `0x3d6a`/`0x3d3a` (A),
`0x40a6`/`0x4086` (B), `0x274`/`0x276` (layer-C pos), `0x2546` (p2_frame_base) are
populated), then drives `spawn_and_draw_level_entities` (1000:2a78) — the
level-load entity-placement orchestrator (channel-B+A populator) — and captures, at
ENTRY and EXIT of the orchestrator, the channel-A + channel-B records and the spawn
globals; and, PER-CELL, the draw/blit descriptors via depth-gated hooks on the leaf
calls `draw_anim_channels_a` (0x165e), `draw_anim_channels_b` (0x17c7), and
`blit_sprite` (0x942a).

This is the spawn analog of the Phase-7 screens HUD per-fill capture: the per-cell
descriptors are snapshotted at each leaf call boundary WHILE inside the orchestrator,
each tagged with its grid (row,col) / cell and the layer (A / B / C) that fired.

DRIVING STRATEGY: boot a real level (the real tilemap + tables come from the engine's
own load path), then INVOKE the REAL orchestrator at its entry IP via a synthetic
near-call frame (the Phase-3/4/5 pattern). The function body that runs is the
unmodified original; only the call is synthetic — the orchestrator reads global state
(tilemap / level_src_ptr / the spawn tables), takes no args, and is idempotent enough
to re-run post-boot to capture a full grid scan. We ALSO emit an entry/exit record for
the engine's own natural call(s) during boot when they happen, but the seeded
post-boot invocation is the canonical full-grid capture.

-----------------------------------------------------------------------------------
RESOLVED spawn DGROUP addresses (Ghidra DGROUP 0x203b offsets; read live from the
disassembly of spawn_and_draw_level_entities 1000:2a78 — see spawn_model.md).

Channel-record layout (12 B, same as the anim steppers):
  [0]  active flag (0 free / 1 active)
  [1]  cell
  [2..5] stream ptr (far: off@+2, seg@+4)
  [6]  cur frame byte
  [7]  pad
  [8..11] frame-data ptr (far: off@+8, seg@+10)

Channel A records (3 slots) live at FIXED DGROUP offsets 0x4c40 / 0x4c4c / 0x4c58
(the orchestrator zeroes [0x4c40]/[0x4c4c]/[0x4c58] directly, and writes the active
slot via the far-ptr slot table 0x4c70/0x4c72 — which points back at 0x4c40).
Channel B records (4 slots) at 0x4c80 / 0x4c8c / 0x4c98 / 0x4ca4 (slot table
0x4cbc/0x4cbe). We read the records directly at the fixed offsets.

Spawn globals (set from the BUM header via tilemap / level_src_ptr):
  p1_cell                0x856e (u8)   header[+0x90], -1 if nonzero
  level_exit_cell        0x8572 (u8)   header[+0x91], -1 if nonzero
  items_remaining        0xa0cf (u8)   header[+0x92]
  p2_cell                0x8571 (s8)   header[+0x93] - 1
  p2_move_state          0x8562 (u8)   header[+0x94]
  p2_ai_rng_threshold    0x7920 (u8)   header[+0x95]
  p2_frame_base          0xa0de (u16)  word[header[+0x96]*2 + 0x2546]

Seed far ptrs / tables (read via the slot table or LES):
  tilemap_ptr            0xa0d8 (far)  base tilemap layer (LES BX,[0xa0d8])
  level_src_ptr          0x75d0 (far)  BUM source (LES BX,[0x75d0]); header @+0x90..
  anim_channels_a_tbl    0x4c70/0x4c72 far-ptr slot tbl (3 slots, stride 4)
  anim_channels_b_tbl    0x4cbc/0x4cbe far-ptr slot tbl (4 slots, stride 4)
  spawn_a_type_tbl       0x3d3a        tilemap_byte -> entity type (u8)
  spawn_a_frame_off/seg  0x3d6a/0x3d6c type*4 -> far-ptr frame data
  spawn_b_type_tbl       0x4086        tilemap_byte(+0x30) -> entity type (u8)
  spawn_b_frame_off/seg  0x40a6/0x40a8 type*4 -> far-ptr frame data
  layerc_pos_x_tbl       0x274         (col*2 + row*0x10)*2 -> p1_sprite x (u16)
  layerc_pos_y_tbl       0x276         (col*2 + row*0x10)*2 -> p1_sprite y (u16)
  p2_frame_base_tbl      0x2546        header[+0x96]*2 -> p2_frame_base (u16)
  p1_sprite              0x8884 (far)  blit descriptor [0]=x [+2]=y [+4]=frame
  p1_sprite_desc_off     0x792e        DGROUP off of descriptor passed to blit_sprite
  current_level          0x79b2 (u8)
  copyprotect_flag       0x119a (s8)

Hooked functions (Ghidra seg-1000 off -> runtime linear 0x1100+off):
  spawn_and_draw_level_entities  1000:2a78  (the orchestrator; entry+exit snapshots)
  draw_anim_channels_a           1000:165e  (leaf — layer-A per-cell draw)
  draw_anim_channels_b           1000:17c7  (leaf — layer-B per-cell draw)
  blit_sprite                    1000:942a  (leaf — layer-C per-cell static sprite)
-----------------------------------------------------------------------------------

Outputs (BOTH gitignored — capture; NO commit):
  local/build/render/spawn_trace.bin   (frozen layout — see TRACE LAYOUT below)
  local/build/spawn_model.md           (resolved addrs + seeds + per-cell descriptors)

TRACE LAYOUT (little-endian) — FROZEN; the host replay harness (a later task) parses
this exactly. **v2 = MULTI-LEVEL** (the header's 2nd u16 is now n_LEVELS, and each
level carries its own SEED block + its own list of runs). Multi-level is required
because layer-B is genuinely empty on level 1 (the raw BUM `+0x30` is the vec-ENCODED
bitstream, NOT the runtime grid) — channel-B only fires on levels 2/3/4/5/6/8/9, so a
single-level capture cannot exercise A+B+C together.
  Header:
    +0x00  8 B   magic   b"SPWNTRC1"
    +0x08  2 B   u16     version (=2)
    +0x0A  2 B   u16     n_levels
  fn-name string table:
    2 B   u16     n_fn_names
    per name: u8 len, len bytes (ascii)
  Then, per LEVEL:
    u8    level
    u16   seed_len, seed_len bytes  (SEED_FMT struct — see below; this level's inputs)
    u16   n_runs
    per RUN (one spawn_and_draw_level_entities call; entry+exit + per-cell fills):
      u8    run_id
      u8    name_len, name_len bytes (ascii)
      u8    seeded     (1 = seeded synthetic call, 0 = natural engine call)
      SPAWNSNAP  entry  (SPAWNSNAP_SIZE-byte fixed struct, see SNAP below)
      SPAWNSNAP  exit   (SPAWNSNAP_SIZE-byte fixed struct)
      u16   n_fills
      per fill:
        u8    layer       (0=A draw, 1=B draw, 2=C blit_sprite)
        u8    cell        (grid cell that fired: row*8+col)
        u8    row
        u8    col
        u8    desc_len, desc_len bytes  (layer A/B: the 12-byte channel record at the
                                         leaf-call boundary; layer C: the 8-byte
                                         p1_sprite blit descriptor)

  SPAWNSNAP (channel records + spawn globals + key seed ptrs; little-endian) FMT:
    u8    a_slots                  (3)
    u8    b_slots                  (4)
    7 * 12 B  channel records      (3 A then 4 B, 12 raw bytes each, FIXED offsets)
    u8    p1_cell          (0x856e)
    u8    level_exit_cell  (0x8572)
    u8    items_remaining  (0xa0cf)
    s8    p2_cell          (0x8571)
    u8    p2_move_state    (0x8562)
    u8    p2_ai_rng_threshold (0x7920)
    u16   p2_frame_base    (0xa0de)
    u8    current_level    (0x79b2)
    u16   tilemap_off (0xa0d8) ; u16 tilemap_seg (0xa0da)
    u16   level_src_off (0x75d0); u16 level_src_seg (0x75d2)

  SEED (SEED_FMT) — the inputs the host replay harness reseeds:
    u16   level_src_off ; u16 level_src_seg
    u16   tilemap_off   ; u16 tilemap_seg
    0xC2 B  bum_header          (level_src_ptr[0..0xC2): the BUM header incl. +0x90..)
    0x30 B  tilemap_layer_a     (tilemap[0x00..0x30): layer-A grid 6x8)
    0x30 B  tilemap_layer_b     (tilemap[0x30..0x60): layer-B grid)
    0x30 B  tilemap_layer_c     (tilemap[0x60..0x90): layer-C grid)
    0x100 B  spawn_a_type_tbl   (0x3d3a; tilemap_byte -> entity type)
    0x400 B  spawn_a_frame_off  (0x3d6a; type*4 far-ptr off halves, 0x100 entries)
    0x400 B  spawn_a_frame_seg  (0x3d6c)
    0x100 B  spawn_b_type_tbl   (0x4086)
    0x400 B  spawn_b_frame_off  (0x40a6)
    0x400 B  spawn_b_frame_seg  (0x40a8)
    0x180 B  layerc_pos_x_tbl   (0x274; 0xC0 u16 entries)
    0x180 B  layerc_pos_y_tbl   (0x276; overlapping +2; 0xC0 u16 entries)
    0x40 B   p2_frame_base_tbl  (0x2546; 0x20 u16 entries)

Run (sandbox disabled — needs unicorn/uv cache access), HARD timeout:
  timeout 1800 uv run python tools/spawn_oracle.py
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
OUT_TRACE = os.path.join(OUT_DIR, "spawn_trace.bin")
OUT_MODEL = os.path.join(ROOT, "local/build/spawn_model.md")

PSP_SEG = 0x0100
RAM = 0x110000

DG_LIN: int = (0x103b + PSP_SEG + 0x10) * 16   # 0x114b0  (identical to the lineage)
CODE_LIN: int = (PSP_SEG + 0x10) * 16          # 0x1100   (Ghidra seg-1000 runtime base)
DG_SEG: int = 0x203b                           # engine DGROUP segment (far-ptr seg half)

# ---------------------------------------------------------------------------
# Resolved spawn DGROUP global offsets (from disasm of 1000:2a78)
# ---------------------------------------------------------------------------
# Channel-record FIXED offsets (the orchestrator zeroes these directly).
A_REC_OFFS: List[int] = [0x4c40, 0x4c4c, 0x4c58]
B_REC_OFFS: List[int] = [0x4c80, 0x4c8c, 0x4c98, 0x4ca4]
A_SLOTS: int = 3
B_SLOTS: int = 4
SLOT_REC_LEN: int = 12
# slot-ptr tables (used by the leaf draw fns; the active slot the orchestrator writes)
OFF_A_TBL_OFF: int = 0x4c70
OFF_B_TBL_OFF: int = 0x4cbc

# Spawn globals
OFF_P1_CELL: int = 0x856e             # u8
OFF_LEVEL_EXIT_CELL: int = 0x8572     # u8
OFF_ITEMS_REMAINING: int = 0xa0cf     # u8
OFF_P2_CELL: int = 0x8571             # s8
OFF_P2_MOVE_STATE: int = 0x8562       # u8
OFF_P2_AI_RNG_THRESHOLD: int = 0x7920  # u8
OFF_P2_FRAME_BASE: int = 0xa0de       # u16

# Seed far ptrs / tables
OFF_TILEMAP_PTR: int = 0xa0d8         # far (off 0xa0d8 / seg 0xa0da)
OFF_LEVEL_SRC_PTR: int = 0x75d0       # far (off 0x75d0 / seg 0x75d2)
OFF_SPAWN_A_TYPE: int = 0x3d3a        # tilemap_byte -> entity type (u8)
OFF_SPAWN_A_FRAME_OFF: int = 0x3d6a   # type*4 -> far-ptr (off)
OFF_SPAWN_A_FRAME_SEG: int = 0x3d6c
OFF_SPAWN_B_TYPE: int = 0x4086        # tilemap_byte(+0x30) -> entity type (u8)
OFF_SPAWN_B_FRAME_OFF: int = 0x40a6
OFF_SPAWN_B_FRAME_SEG: int = 0x40a8
OFF_LAYERC_POS_X: int = 0x274         # (col*2 + row*0x10)*2 -> x (u16)
OFF_LAYERC_POS_Y: int = 0x276         # +2; -> y (u16)
OFF_P2_FRAME_BASE_TBL: int = 0x2546   # header[+0x96]*2 -> p2_frame_base (u16)
OFF_P1_SPRITE_PTR: int = 0x8884       # far ptr to blit descriptor (x,y,frame)
P1_SPRITE_DESC_LEN: int = 8

OFF_CURRENT_LEVEL: int = 0x79b2       # u8
OFF_COPYPROTECT: int = 0x119a         # s8
OFF_KEY_STATE_PTR: int = 0x4D42       # near ptr to g_key_state_table base

BUM_HEADER_LEN: int = 0xC2            # the BUM header byte-region (incl. +0x90..+0x96)
LAYER_LEN: int = 0x30                 # 6x8 grid per layer

# ---------------------------------------------------------------------------
# Hooked functions (Ghidra seg-1000 offsets)
# ---------------------------------------------------------------------------
OFF_SPAWN: int = 0x2a78
OFF_DRAW_A: int = 0x165e
OFF_DRAW_B: int = 0x17c7
OFF_BLIT_SPRITE: int = 0x942a
# Level (re)load path — re-invoked post-boot to scan multiple levels (see B=0 note).
OFF_START_LEVEL: int = 0x2d14          # loads+vec_decodes D{N}.PAV/DEC/BUM
OFF_LOAD_LEVEL_DATA: int = 0x32b0      # copies decoded 0xc2 header slice into tilemap
FN_NAMES: Dict[int, str] = {
    OFF_SPAWN: "spawn_and_draw_level_entities",
    OFF_DRAW_A: "draw_anim_channels_a",
    OFF_DRAW_B: "draw_anim_channels_b",
    OFF_BLIT_SPRITE: "blit_sprite",
}
LEAF_FNS = {OFF_DRAW_A, OFF_DRAW_B, OFF_BLIT_SPRITE}
LEAF_LAYER: Dict[int, int] = {OFF_DRAW_A: 0, OFF_DRAW_B: 1, OFF_BLIT_SPRITE: 2}

# ---------------------------------------------------------------------------
# Trace format constants
# ---------------------------------------------------------------------------
TRACE_MAGIC: bytes = b"SPWNTRC1"
TRACE_VERSION: int = 2          # v2 = multi-level (header n_runs field -> n_levels)
# SPAWNSNAP: 2 count bytes, 7*12 record bytes, spawn globals, level, seed ptrs.
SPAWNSNAP_FMT: str = ("<BB" + ("B" * (7 * SLOT_REC_LEN))
                      + "BBBbBB" + "H" + "B" + "HHHH")
SPAWNSNAP_SIZE: int = struct.calcsize(SPAWNSNAP_FMT)


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

    # --- minimal VGA planar emulation (copied from anim_oracle.py) ---
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

    def rd_s8(off: int) -> int:
        v = uc.mem_read(DG_LIN + off, 1)[0]
        return v - 256 if v >= 128 else v

    def rd_u16(off: int) -> int:
        return struct.unpack("<H", bytes(uc.mem_read(DG_LIN + off, 2)))[0]

    def rd_bytes(off: int, n: int) -> bytes:
        return bytes(uc.mem_read(DG_LIN + off, n))

    def wr8(off: int, v: int) -> None:
        uc.mem_write(DG_LIN + off, bytes([v & 0xFF]))

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

    def read_records() -> bytes:
        """Read all 7 (3 A + 4 B) 12-byte channel records at their FIXED offsets."""
        out = bytearray()
        for off in A_REC_OFFS + B_REC_OFFS:
            out += rd_bytes(off, SLOT_REC_LEN)
        return bytes(out)

    def tilemap_byte(cell: int) -> int:
        o, s = read_far_at(OFF_TILEMAP_PTR)
        lin = (s * 16 + o + (cell & 0xFF)) & 0xFFFFF
        try:
            return uc.mem_read(lin, 1)[0]
        except UcError:
            return 0

    def snap() -> bytes:
        recs = read_records()
        t_off, t_seg = read_far_at(OFF_TILEMAP_PTR)
        l_off, l_seg = read_far_at(OFF_LEVEL_SRC_PTR)
        return struct.pack(
            SPAWNSNAP_FMT,
            A_SLOTS, B_SLOTS,
            *recs,
            rd8(OFF_P1_CELL), rd8(OFF_LEVEL_EXIT_CELL), rd8(OFF_ITEMS_REMAINING),
            rd_s8(OFF_P2_CELL), rd8(OFF_P2_MOVE_STATE), rd8(OFF_P2_AI_RNG_THRESHOLD),
            rd_u16(OFF_P2_FRAME_BASE),
            rd8(OFF_CURRENT_LEVEL),
            t_off, t_seg, l_off, l_seg)

    # ---------------------------------------------------------------------------
    # Hooks: orchestrator entry/exit + depth-gated per-cell leaf descriptor capture
    # ---------------------------------------------------------------------------
    capturing = {"on": False}
    spawn_depth = {"n": 0}                       # >0 while inside the orchestrator
    cur_fills: List[Tuple[int, int, bytes]] = []  # (layer, cell, desc) per leaf call
    run_records: List[dict] = []
    reached_fns: collections.Counter = collections.Counter()
    pending_exit: dict = {}
    exit_hook_lins: set = set()
    fn_name_list: List[str] = list(dict.fromkeys(FN_NAMES.values()))
    fn_name_idx = {n: i for i, n in enumerate(fn_name_list)}

    def read_active_rec(layer: int) -> Tuple[int, bytes]:
        """The active channel record + its cell, for a draw-A/B leaf call.

        The orchestrator stamps cell into rec[+1] then calls the leaf; the active slot
        is the one whose [0]=1. Return (cell, 12-byte record)."""
        offs = A_REC_OFFS if layer == 0 else B_REC_OFFS
        for off in offs:
            rec = rd_bytes(off, SLOT_REC_LEN)
            if rec[0] == 1:
                return rec[1], rec
        # fall back to slot 0 if none flagged active
        rec = rd_bytes(offs[0], SLOT_REC_LEN)
        return rec[1], rec

    def hook_leaf(uc: Uc, addr: int, size: int, _: object) -> None:
        if spawn_depth["n"] <= 0:
            return
        fn_off = (addr - CODE_LIN) & 0xFFFF
        layer = LEAF_LAYER.get(fn_off)
        if layer is None:
            return
        if layer == 2:
            # blit_sprite: arg is the p1_sprite descriptor (x/y/frame just written).
            desc = read_far_target(OFF_P1_SPRITE_PTR, P1_SPRITE_DESC_LEN)
            cell = 0xFF  # layer C is keyed on (row,col)->pos; record desc's frame below
        else:
            cell, desc = read_active_rec(layer)
        cur_fills.append((layer, cell, desc))

    def hook_fn_entry(uc: Uc, addr: int, size: int, _: object) -> None:
        if not capturing["on"]:
            return
        fn_off = (addr - CODE_LIN) & 0xFFFF
        if fn_off != OFF_SPAWN:
            return
        reached_fns[fn_off] += 1
        spawn_depth["n"] += 1
        if spawn_depth["n"] == 1:
            cur_fills.clear()
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
        exit_snap = snap()
        fills = list(cur_fills)
        cur_fills.clear()
        spawn_depth["n"] -= 1
        run_records.append(dict(fn_off=fn_off, entry=entry_snap, exit=exit_snap,
                                fills=fills, seeded=cur_seeded["v"]))

    # leaf hooks
    for off in (OFF_DRAW_A, OFF_DRAW_B, OFF_BLIT_SPRITE):
        lin = CODE_LIN + off
        uc.hook_add(UC_HOOK_CODE, hook_leaf, None, lin, lin)
    # orchestrator entry hook
    uc.hook_add(UC_HOOK_CODE, hook_fn_entry, None,
                CODE_LIN + OFF_SPAWN, CODE_LIN + OFF_SPAWN)

    cur_seeded = {"v": 1}

    # Pre-register exit hook at the synthetic-call return sentinel (a code hook added
    # mid-emulation only takes effect on the NEXT emu_start, so pre-register it).
    STOP_OFF = 0x0008
    exit_hook_lins.add(CODE_LIN + STOP_OFF)
    uc.hook_add(UC_HOOK_CODE, hook_fn_exit, None,
                CODE_LIN + STOP_OFF, CODE_LIN + STOP_OFF)

    # ---------------------------------------------------------------------------
    # Synthetic near-call into an engine fn (seeded) — Phase-3/4/5 pattern.
    # ---------------------------------------------------------------------------
    def call_engine_fn(fn_off: int) -> None:
        uc.mem_write(CODE_LIN + STOP_OFF, b"\xF4")  # HLT sentinel
        ss = uc.reg_read(UC_X86_REG_SS) & 0xFFFF
        sp = uc.reg_read(UC_X86_REG_SP) & 0xFFFF
        sp = (sp - 2) & 0xFFFF
        uc.mem_write(ss * 16 + sp, struct.pack("<H", STOP_OFF))
        uc.reg_write(UC_X86_REG_SP, sp)

        def stop_hook(u: Uc, a: int, s: int, _: object) -> None:
            u.emu_stop()
        h = uc.hook_add(UC_HOOK_CODE, stop_hook, None,
                        CODE_LIN + STOP_OFF, CODE_LIN + STOP_OFF)
        try:
            uc.emu_start(CODE_LIN + fn_off, 0, count=40_000_000)
        except UcError as e:
            tr["call_err"] = str(e)
        finally:
            uc.hook_del(h)

    # ---------------------------------------------------------------------------
    # Boot to level (identical approach to anim_oracle / items_oracle)
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

    print("[spawn_oracle] booting BUMPY (level %d)..." % LEVEL, flush=True)

    while total_instr < 400_000_000:
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e); tr["err"] = err; break
        total_instr += CHUNK
        if total_instr % 40_000_000 == 0:
            print("[spawn_oracle] %dM instr, countdown=%s" % (
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
            print("[spawn_oracle] level loaded (%s) at chunk %d — settling %d ticks" % (
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
        print("[spawn_oracle] ERROR: premature exit/fault during boot: exit=%s fault=%s err=%s" % (
            tr.get("exit"), tr.get("fault"), err), flush=True)
        return
    if not opened(BUMNAME):
        print("[spawn_oracle] ERROR: level %s never loaded after %dM instructions" % (
            BUMNAME, total_instr // 1_000_000), flush=True)
        return

    print("[spawn_oracle] boot complete. Files: %s" % (
        [o[1] for o in tr.get("fileops", [])]), flush=True)

    # Post-boot spawn state for the report.
    boot_recs = read_records()
    print("[spawn_oracle] boot tilemap=%04x:%04x level_src=%04x:%04x" % (
        *read_far_at(OFF_TILEMAP_PTR), *read_far_at(OFF_LEVEL_SRC_PTR)), flush=True)
    print("[spawn_oracle] boot spawn globals: p1_cell=0x%02x exit=0x%02x items=%d "
          "p2_cell=%d p2_move=0x%02x p2_rng=0x%02x p2_fb=0x%04x" % (
              rd8(OFF_P1_CELL), rd8(OFF_LEVEL_EXIT_CELL), rd8(OFF_ITEMS_REMAINING),
              rd_s8(OFF_P2_CELL), rd8(OFF_P2_MOVE_STATE),
              rd8(OFF_P2_AI_RNG_THRESHOLD), rd_u16(OFF_P2_FRAME_BASE)), flush=True)

    # ---------------------------------------------------------------------------
    # Per-level (re)load helper: set current_level, (re)run start_level (loads +
    # vec_decodes D{N}.PAV/DEC/BUM) then load_current_level_data (copies the decoded
    # 0xc2 header slice into the runtime tilemap). The RAW BUM +0x30 region is the
    # vec-ENCODED bitstream; only AFTER decode+copy does tilemap[+0x30] hold the real
    # layer-B entity grid — which for level 1 is genuinely all-zero. We scan levels to
    # find ones whose decoded layer-B is populated.
    # ---------------------------------------------------------------------------
    def load_level(n: int) -> Tuple[int, int, int]:
        wr8(OFF_CURRENT_LEVEL, n)
        uc.mem_write(DG_LIN + OFF_COPYPROTECT, bytes([1]))   # skip copy-protect
        call_engine_fn(OFF_START_LEVEL)
        wr8(OFF_CURRENT_LEVEL, n)
        uc.mem_write(DG_LIN + OFF_COPYPROTECT, bytes([1]))
        call_engine_fn(OFF_LOAD_LEVEL_DATA)
        tm = read_far_blob(OFF_TILEMAP_PTR, LAYER_LEN * 3)
        nz = [sum(1 for i in range(LAYER_LEN) if tm[L * LAYER_LEN + i] != 0)
              for L in range(3)]
        return nz[0], nz[1], nz[2]

    def read_far_blob(off: int, n: int) -> bytes:
        return read_far_target(off, n)

    if os.environ.get("SPAWN_SCAN"):
        print("[spawn_oracle] SCAN: per-level decoded-tilemap non-zero counts "
              "(A/B/C):", flush=True)
        for n in range(1, 10):
            a, b, c = load_level(n)
            print("[spawn_oracle]   level %d: A=%d B=%d C=%d" % (n, a, b, c),
                  flush=True)
        return

    def build_seed() -> Tuple[bytes, bytes, bytes]:
        """Snapshot the host-replay SEED block for the currently-loaded level.

        Returns (seed_bytes, bum_header, tilemap_blob)."""
        t_off, t_seg = read_far_at(OFF_TILEMAP_PTR)
        l_off, l_seg = read_far_at(OFF_LEVEL_SRC_PTR)
        bum_header = read_far_blob(OFF_LEVEL_SRC_PTR, BUM_HEADER_LEN)
        # the orchestrator reads layers A/B/C from tilemap (+0x00 / +0x30 / +0x60)
        tilemap_blob = read_far_blob(OFF_TILEMAP_PTR, LAYER_LEN * 3)
        seed = (struct.pack("<HHHH", l_off, l_seg, t_off, t_seg)
                + bum_header
                + tilemap_blob[0:LAYER_LEN]
                + tilemap_blob[LAYER_LEN:LAYER_LEN * 2]
                + tilemap_blob[LAYER_LEN * 2:LAYER_LEN * 3]
                + rd_bytes(OFF_SPAWN_A_TYPE, 0x100)
                + rd_bytes(OFF_SPAWN_A_FRAME_OFF, 0x400)
                + rd_bytes(OFF_SPAWN_A_FRAME_SEG, 0x400)
                + rd_bytes(OFF_SPAWN_B_TYPE, 0x100)
                + rd_bytes(OFF_SPAWN_B_FRAME_OFF, 0x400)
                + rd_bytes(OFF_SPAWN_B_FRAME_SEG, 0x400)
                + rd_bytes(OFF_LAYERC_POS_X, 0x180)
                + rd_bytes(OFF_LAYERC_POS_Y, 0x180)
                + rd_bytes(OFF_P2_FRAME_BASE_TBL, 0x40))
        return seed, bum_header, tilemap_blob

    # ---------------------------------------------------------------------------
    # Multi-level capture. Level 1's decoded layer-B is genuinely EMPTY (the raw BUM
    # +0x30 region is the vec-ENCODED bitstream, not the runtime grid); to validate
    # channel-B we scan all levels and capture the ones whose decoded tilemap[+0x30]
    # is populated (verified: L2/3/4/5/6/8/9 all have B>0; L1/L7 have B=0). For each
    # level we (re)run start_level (load+vec_decode) + load_current_level_data (copy
    # decoded slice into tilemap), then invoke the orchestrator via the synthetic
    # near-call and capture its entry/exit + per-cell leaf descriptors.
    # ---------------------------------------------------------------------------
    CAPTURE_LEVELS = [int(x) for x in
                      os.environ.get("SPAWN_LEVELS", "1,2,3,4,5,6,7,8,9").split(",")]
    level_blocks: List[dict] = []   # one per captured level
    for lvl in CAPTURE_LEVELS:
        a_nz, b_nz, c_nz = load_level(lvl)
        seed, bum_header, tilemap_blob = build_seed()
        run_records.clear()
        capturing["on"] = True
        cur_seeded["v"] = 1
        spawn_depth["n"] = 0
        call_engine_fn(OFF_SPAWN)
        capturing["on"] = False
        runs = list(run_records)
        run_records.clear()
        tot = sum(len(r["fills"]) for r in runs)
        bf = sum(1 for r in runs for f in r["fills"] if f[0] == 1)
        af = sum(1 for r in runs for f in r["fills"] if f[0] == 0)
        cf = sum(1 for r in runs for f in r["fills"] if f[0] == 2)
        print("[spawn_oracle] level %d: tilemap nz A=%d B=%d C=%d ; "
              "fills A=%d B=%d C=%d (%d runs)" % (
                  lvl, a_nz, b_nz, c_nz, af, bf, cf, len(runs)), flush=True)
        level_blocks.append(dict(level=lvl, seed=seed, bum_header=bum_header,
                                 tilemap_blob=tilemap_blob, runs=runs))

    tot_a = sum(1 for lb in level_blocks for r in lb["runs"]
                for f in r["fills"] if f[0] == 0)
    tot_b = sum(1 for lb in level_blocks for r in lb["runs"]
                for f in r["fills"] if f[0] == 1)
    tot_c = sum(1 for lb in level_blocks for r in lb["runs"]
                for f in r["fills"] if f[0] == 2)
    print("[spawn_oracle] TOTAL fills across %d levels: A=%d B=%d C=%d" % (
        len(level_blocks), tot_a, tot_b, tot_c), flush=True)
    assert tot_a > 0 and tot_b > 0 and tot_c > 0, \
        "need A>0 AND B>0 AND C>0 (got A=%d B=%d C=%d)" % (tot_a, tot_b, tot_c)

    # ---------------------------------------------------------------------------
    # Write the frozen trace (version 2 — multi-level; see TRACE LAYOUT in docstring)
    # ---------------------------------------------------------------------------
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT_TRACE, "wb") as f:
        f.write(TRACE_MAGIC)
        f.write(struct.pack("<HH", TRACE_VERSION, len(level_blocks)))  # n_levels
        f.write(struct.pack("<H", len(fn_name_list)))
        for nm in fn_name_list:
            b = nm.encode("ascii")
            f.write(struct.pack("<B", len(b))); f.write(b)
        # per-level blocks
        for lb in level_blocks:
            f.write(struct.pack("<B", lb["level"]))
            f.write(struct.pack("<H", len(lb["seed"]))); f.write(lb["seed"])
            f.write(struct.pack("<H", len(lb["runs"])))   # n_runs for this level
            for i, r in enumerate(lb["runs"]):
                nm = ("seeded_full_grid" if i == 0 else "run_%d" % i).encode("ascii")
                f.write(struct.pack("<B", i))
                f.write(struct.pack("<B", len(nm))); f.write(nm)
                f.write(struct.pack("<B", r["seeded"]))
                f.write(r["entry"]); f.write(r["exit"])
                f.write(struct.pack("<H", len(r["fills"])))
                for (layer, cell, desc) in r["fills"]:
                    row = (cell // 8) & 0xFF if cell != 0xFF else 0xFF
                    col = (cell % 8) & 0xFF if cell != 0xFF else 0xFF
                    f.write(struct.pack("<BBBB", layer, cell & 0xFF, row, col))
                    f.write(struct.pack("<B", len(desc))); f.write(desc)
    trace_bytes = os.path.getsize(OUT_TRACE)
    print("[spawn_oracle] wrote %s (%d B)" % (OUT_TRACE, trace_bytes), flush=True)

    # ---------------------------------------------------------------------------
    # ROUND-TRIP check: standalone re-parse of the trace we just wrote.
    # ---------------------------------------------------------------------------
    def parse_trace(path: str) -> dict:
        with open(path, "rb") as fh:
            buf = fh.read()
        assert buf[0:8] == TRACE_MAGIC, "bad magic"
        o = 8
        ver, n_levels = struct.unpack_from("<HH", buf, o); o += 4
        (n_names,) = struct.unpack_from("<H", buf, o); o += 2
        names = []
        for _ in range(n_names):
            (ln,) = struct.unpack_from("<B", buf, o); o += 1
            names.append(buf[o:o + ln].decode("ascii")); o += ln
        levels = []
        for _ in range(n_levels):
            (lvl,) = struct.unpack_from("<B", buf, o); o += 1
            (slen,) = struct.unpack_from("<H", buf, o); o += 2
            seed_b = buf[o:o + slen]; o += slen
            (n_runs,) = struct.unpack_from("<H", buf, o); o += 2
            runs = []
            for _ in range(n_runs):
                (rid,) = struct.unpack_from("<B", buf, o); o += 1
                (nl,) = struct.unpack_from("<B", buf, o); o += 1
                rname = buf[o:o + nl].decode("ascii"); o += nl
                (sd,) = struct.unpack_from("<B", buf, o); o += 1
                ent = buf[o:o + SPAWNSNAP_SIZE]; o += SPAWNSNAP_SIZE
                ex = buf[o:o + SPAWNSNAP_SIZE]; o += SPAWNSNAP_SIZE
                (nf,) = struct.unpack_from("<H", buf, o); o += 2
                fills = []
                for _ in range(nf):
                    layer, cell, row, col = struct.unpack_from("<BBBB", buf, o); o += 4
                    (dl,) = struct.unpack_from("<B", buf, o); o += 1
                    fills.append((layer, cell, row, col, buf[o:o + dl])); o += dl
                runs.append(dict(rid=rid, name=rname, seeded=sd, ent=ent, ex=ex,
                                 fills=fills))
            levels.append(dict(level=lvl, seed=seed_b, runs=runs))
        assert o == len(buf), "trailing bytes: parsed %d of %d" % (o, len(buf))
        return dict(ver=ver, names=names, levels=levels)

    parsed = parse_trace(OUT_TRACE)
    assert parsed["ver"] == TRACE_VERSION
    assert len(parsed["levels"]) == len(level_blocks)
    for pl, lb in zip(parsed["levels"], level_blocks):
        assert pl["level"] == lb["level"]
        assert len(pl["seed"]) == len(lb["seed"])
        assert len(pl["runs"]) == len(lb["runs"])
    print("[spawn_oracle] round-trip OK: %d levels, names=%s" % (
        len(parsed["levels"]), parsed["names"]), flush=True)

    # ---------------------------------------------------------------------------
    # spawn_model.md
    # ---------------------------------------------------------------------------
    def snap_recs(snap_bytes: bytes) -> List[bytes]:
        out = []
        base_o = 2
        for i in range(A_SLOTS + B_SLOTS):
            out.append(snap_bytes[base_o + i * SLOT_REC_LEN:
                                  base_o + (i + 1) * SLOT_REC_LEN])
        return out

    def snap_globals(snap_bytes: bytes) -> dict:
        o = 2 + 7 * SLOT_REC_LEN
        (p1c, exitc, items, p2c, p2mv, p2rng) = struct.unpack_from("<BBBbBB", snap_bytes, o)
        o += 6
        (p2fb,) = struct.unpack_from("<H", snap_bytes, o); o += 2
        (lvl,) = struct.unpack_from("<B", snap_bytes, o); o += 1
        (t_off, t_seg, l_off, l_seg) = struct.unpack_from("<HHHH", snap_bytes, o)
        return dict(p1_cell=p1c, level_exit_cell=exitc, items_remaining=items,
                    p2_cell=p2c, p2_move_state=p2mv, p2_ai_rng_threshold=p2rng,
                    p2_frame_base=p2fb, current_level=lvl,
                    tilemap=(t_off, t_seg), level_src=(l_off, l_seg))

    L: List[str] = []
    L.append("# Bumpy Phase-8 entity-spawn capture model\n\n")
    L.append("Generated by `tools/spawn_oracle.py`. Capture target = "
             "`spawn_and_draw_level_entities` (1000:2a78), the level-load entity "
             "placement orchestrator (channel-B+A populator + layer-C static "
             "sprites + P1/P2 spawn from the BUM header).\n\n")
    L.append("The oracle boots the REAL unpacked BUMPY.EXE once, then for EACH "
             "captured level re-runs `start_level` (load + vec_decode of "
             "D{N}.PAV/DEC/BUM) and `load_current_level_data` (copy the decoded 0xc2 "
             "header slice into the runtime tilemap), so the tilemap + BUM header + "
             "spawn tables are populated, then invokes the orchestrator via a "
             "synthetic near-call (unmodified original body) and captures entry/exit "
             "+ per-cell leaf descriptors.\n\n")
    L.append("**Layer-B note (the Phase-8 point):** the RAW BUM `+0x30..+0x5f` "
             "region is the vec-ENCODED bitstream, NOT the runtime grid. Only AFTER "
             "`vec_decode` + `load_current_level_data` does `tilemap[+0x30]` hold the "
             "real layer-B entity grid. Level 1's decoded layer-B is genuinely EMPTY "
             "(B=0); layer-B fires on levels 2/3/4/5/6/8/9. Captured levels: %s.\n\n"
             % ", ".join(str(lb["level"]) for lb in level_blocks))

    L.append("## Channel-record layout (12 B)\n\n")
    L.append("| offset | field |\n|---|---|\n")
    L.append("| [0] | active flag (0 free / 1 active) |\n")
    L.append("| [1] | cell (row*8+col) |\n")
    L.append("| [2..5] | stream ptr (far: off@+2, seg@+4) |\n")
    L.append("| [6] | cur frame byte |\n| [7] | pad |\n")
    L.append("| [8..11] | frame-data ptr (far: off@+8, seg@+10) |\n\n")
    L.append("Channel A records at FIXED DGROUP offsets %s (slot tbl 0x%04x). "
             "Channel B at %s (slot tbl 0x%04x).\n\n" % (
                 ["0x%04x" % x for x in A_REC_OFFS], OFF_A_TBL_OFF,
                 ["0x%04x" % x for x in B_REC_OFFS], OFF_B_TBL_OFF))

    L.append("## Resolved spawn DGROUP addresses (from disasm of 1000:2a78)\n\n")
    L.append("| symbol | offset | role |\n|---|---|---|\n")
    rows = [
        ("p1_cell", 0x856e, "header[+0x90], -1 if nonzero"),
        ("level_exit_cell", 0x8572, "header[+0x91], -1 if nonzero"),
        ("items_remaining", 0xa0cf, "header[+0x92]"),
        ("p2_cell", 0x8571, "header[+0x93] - 1 (s8)"),
        ("p2_move_state", 0x8562, "header[+0x94]"),
        ("p2_ai_rng_threshold", 0x7920, "header[+0x95]"),
        ("p2_frame_base", 0xa0de, "word[header[+0x96]*2 + 0x2546]"),
        ("tilemap_ptr", 0xa0d8, "base tilemap layer (far)"),
        ("level_src_ptr", 0x75d0, "BUM source ptr (far); header @+0x90.."),
        ("anim_channels_a_tbl", 0x4c70, "channel-A far-ptr slot tbl (3 slots)"),
        ("anim_channels_b_tbl", 0x4cbc, "channel-B far-ptr slot tbl (4 slots)"),
        ("spawn_a_type_tbl", 0x3d3a, "tilemap_byte -> entity type (layer A)"),
        ("spawn_a_frame_off", 0x3d6a, "type*4 -> far-ptr frame data (off)"),
        ("spawn_a_frame_seg", 0x3d6c, "type*4 -> far-ptr frame data (seg)"),
        ("spawn_b_type_tbl", 0x4086, "tilemap_byte(+0x30) -> entity type (layer B)"),
        ("spawn_b_frame_off", 0x40a6, "type*4 -> far-ptr frame data (off)"),
        ("spawn_b_frame_seg", 0x40a8, "type*4 -> far-ptr frame data (seg)"),
        ("layerc_pos_x_tbl", 0x274, "(col*2+row*0x10)*2 -> p1_sprite x (u16)"),
        ("layerc_pos_y_tbl", 0x276, "(col*2+row*0x10)*2 -> p1_sprite y (u16)"),
        ("p2_frame_base_tbl", 0x2546, "header[+0x96]*2 -> p2_frame_base (u16)"),
        ("p1_sprite", 0x8884, "layer-C blit descriptor far ptr [0]=x [+2]=y [+4]=frame"),
        ("p1_sprite_desc_off", 0x792e, "DGROUP off of descriptor passed to blit_sprite"),
        ("current_level", 0x79b2, "boot level select"),
        ("copyprotect_flag", 0x119a, "boot guard"),
    ]
    for sym, off, role in rows:
        L.append("| %s | 0x%04x | %s |\n" % (sym, off, role))
    L.append("\n")

    L.append("## Hooked functions (Ghidra seg-1000 offsets)\n\n")
    L.append("| addr | name | role |\n|---|---|---|\n")
    roles = {
        OFF_SPAWN: "orchestrator (entry+exit snapshots)",
        OFF_DRAW_A: "leaf — layer-A per-cell draw",
        OFF_DRAW_B: "leaf — layer-B per-cell draw",
        OFF_BLIT_SPRITE: "leaf — layer-C per-cell static sprite",
    }
    for off in (OFF_SPAWN, OFF_DRAW_A, OFF_DRAW_B, OFF_BLIT_SPRITE):
        L.append("| 1000:%04x | %s | %s |\n" % (off, FN_NAMES[off], roles[off]))
    L.append("\n")

    def grid_str(layer_bytes: bytes) -> str:
        out = []
        for r in range(6):
            out.append(" ".join("%02x" % layer_bytes[r * 8 + c] for c in range(8)))
        return "\n".join(out)

    # Cross-level fill summary
    L.append("## Per-level fill summary\n\n")
    L.append("| level | tilemap nz A/B/C | fills A | fills B | fills C |\n"
             "|---|---|---|---|---|\n")
    for lb in level_blocks:
        tb = lb["tilemap_blob"]
        nz = [sum(1 for i in range(LAYER_LEN) if tb[k * LAYER_LEN + i] != 0)
              for k in range(3)]
        cnt = collections.Counter(f[0] for r in lb["runs"] for f in r["fills"])
        L.append("| %d | %d/%d/%d | %d | %d | %d |\n" % (
            lb["level"], nz[0], nz[1], nz[2],
            cnt.get(0, 0), cnt.get(1, 0), cnt.get(2, 0)))
    L.append("\n")

    lname = {0: "A", 1: "B", 2: "C"}
    # Per-level detail
    for lb in level_blocks:
        lvl = lb["level"]
        seed = lb["seed"]; bum_header = lb["bum_header"]; tilemap_blob = lb["tilemap_blob"]
        t_off, t_seg, l_off, l_seg = struct.unpack_from("<HHHH", seed, 0)
        L.append("## Level %d\n\n" % lvl)
        L.append("### Seeded inputs\n\n")
        L.append("- tilemap ptr = %04x:%04x ; level_src ptr = %04x:%04x\n" % (
            t_off, t_seg, l_off, l_seg))
        L.append("- BUM header (`+0x90..+0x96`): p1=0x%02x exit=0x%02x items=0x%02x "
                 "p2=0x%02x p2_move=0x%02x p2_rng=0x%02x p2_fb_idx=0x%02x\n" % (
                     bum_header[0x90], bum_header[0x91], bum_header[0x92],
                     bum_header[0x93], bum_header[0x94], bum_header[0x95],
                     bum_header[0x96]))
        L.append("\n#### tilemap layer A (+0x00, 6x8)\n```\n%s\n```\n"
                 % grid_str(tilemap_blob[0:LAYER_LEN]))
        L.append("\n#### tilemap layer B (+0x30, 6x8)\n```\n%s\n```\n"
                 % grid_str(tilemap_blob[LAYER_LEN:LAYER_LEN * 2]))
        L.append("\n#### tilemap layer C (+0x60, 6x8)\n```\n%s\n```\n"
                 % grid_str(tilemap_blob[LAYER_LEN * 2:LAYER_LEN * 3]))
        L.append("\n- SEED block total: %d bytes (frozen layout in the docstring).\n\n"
                 % len(seed))

        for i, r in enumerate(lb["runs"]):
            ent_g = snap_globals(r["entry"]); ex_g = snap_globals(r["exit"])
            ex_recs = snap_recs(r["exit"])
            L.append("### Level %d run %d — %s (%s)\n\n" % (
                lvl, i, "seeded_full_grid" if i == 0 else "run_%d" % i,
                "seeded synthetic call" if r["seeded"] else "natural engine call"))
            L.append("#### Spawn globals (entry -> exit)\n\n")
            L.append("| global | entry | exit |\n|---|---|---|\n")
            for k in ("p1_cell", "level_exit_cell", "items_remaining", "p2_cell",
                      "p2_move_state", "p2_ai_rng_threshold", "p2_frame_base",
                      "current_level"):
                L.append("| %s | 0x%04x | 0x%04x |\n" % (
                    k, ent_g[k] & 0xFFFF, ex_g[k] & 0xFFFF))
            L.append("\n#### Channel records at EXIT (populated by the scan)\n\n")
            for j in range(A_SLOTS):
                L.append("- A slot %d (0x%04x): `%s`\n" % (
                    j, A_REC_OFFS[j], ex_recs[j].hex()))
            for j in range(B_SLOTS):
                L.append("- B slot %d (0x%04x): `%s`\n" % (
                    j, B_REC_OFFS[j], ex_recs[A_SLOTS + j].hex()))
            cnt = collections.Counter(f[0] for f in r["fills"])
            L.append("\n#### Per-cell leaf descriptors (%d fills: A=%d B=%d C=%d)\n\n"
                     % (len(r["fills"]), cnt.get(0, 0), cnt.get(1, 0), cnt.get(2, 0)))
            L.append("| # | layer | cell | row,col | descriptor |\n"
                     "|---|---|---|---|---|\n")
            for k, (layer, cell, desc) in enumerate(r["fills"][:80]):
                if cell == 0xFF:
                    rc = "(pos)"; cell_s = "-"
                else:
                    rc = "%d,%d" % (cell // 8, cell % 8); cell_s = "0x%02x" % cell
                if layer == 2 and len(desc) >= 6:
                    x, y, fr = struct.unpack_from("<HHH", desc, 0)
                    ds = "x=%d y=%d frame=%d (%s)" % (x, y, fr, desc.hex())
                else:
                    ds = desc.hex()
                L.append("| %d | %s | %s | %s | %s |\n" % (
                    k, lname[layer], cell_s, rc, ds))
            if len(r["fills"]) > 80:
                L.append("\n(+%d more fills)\n" % (len(r["fills"]) - 80))
            L.append("\n")

    os.makedirs(os.path.dirname(OUT_MODEL), exist_ok=True)
    with open(OUT_MODEL, "w") as f:
        f.write("".join(L))
    print("[spawn_oracle] wrote %s" % OUT_MODEL, flush=True)

    # ---------------------------------------------------------------------------
    # Console summary
    # ---------------------------------------------------------------------------
    print("\n[spawn_oracle] REACHED functions:", flush=True)
    for off in (OFF_SPAWN,):
        cnt = reached_fns.get(off, 0)
        flag = "" if cnt else "  <-- NOT REACHED"
        print("   1000:%04x  %-30s x%d%s" % (off, FN_NAMES[off], cnt, flag), flush=True)
    n_runs = sum(len(lb["runs"]) for lb in level_blocks)
    total_fills = sum(len(r["fills"]) for lb in level_blocks for r in lb["runs"])
    print("[spawn_oracle] levels=%d runs=%d total_fills=%d (A=%d B=%d C=%d)" % (
        len(level_blocks), n_runs, total_fills, tot_a, tot_b, tot_c), flush=True)
    if err:
        print("[spawn_oracle] emu error:", err, flush=True)
    if tr.get("call_err"):
        print("[spawn_oracle] call_err:", tr["call_err"], flush=True)


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
