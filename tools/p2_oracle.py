#!/usr/bin/env python3
"""p2_oracle.py — Phase-4 Player-2 (AI + move-state + pvp + draw) CAPTURE-AS-DISCOVERY.

Boots the real (unpacked) BUMPY.EXE under Unicorn — reusing the boot + DGROUP + int /
VGA scaffold of tools/physics_oracle.py / tools/items_oracle.py (deliberately NOT
refactoring those) — drives a set of seeded P2 scenarios and captures, at the ENTRY and
EXIT of each hooked P2 function, the P2 trajectory state + the AI decision inputs +
`rng_frame` + (for the draw fn) the produced blit/object descriptor.

This is the P2 analog of the Phase-2 physics capture PLUS the new AI-decision layer.
P2 is AI-controlled (autonomous, NOT key-driven), so — per the Phase-3 pattern — every
scenario SEEDS the relevant preconditions (p2_cell / pixel / move-state / dir-blocked
flags / rng_frame / pvp bboxes) and then INVOKES the REAL engine fn at its entry IP via
a synthetic near-call frame. The function body that runs is the unmodified original
code; only the precondition state is seeded. Crucially we VARY `rng_frame` across the
AI-decision scenarios to exercise the rng branches (select_move_a vs b vs random).

DRIVING STRATEGY: seeded (all scenarios). Scripted key-driving cannot drive P2 — the
AI reads its own globals + rand(), not the keyboard — so seeding the preconditions and
calling the real fn is the faithful, bounded approach (same as the Phase-3 items oracle).

-----------------------------------------------------------------------------------
RESOLVED P2 DGROUP addresses (Ghidra DGROUP 0x203b offsets; read live via Ghidra MCP
from the disassembly operands of the P2 functions — see p2_model.md for the per-fn
provenance). Cross-checked against the Phase-0/6b P2 render globals.
-----------------------------------------------------------------------------------
  p2_pixel_x            0x79ba (s16)   MOV [0x79ba] in p2_step_scripted_move 4c14  (✓ 6b)
  p2_pixel_y            0x79bc (s16)   ADD word[0x79bc] in 4c14                    (✓ 6b)
  p2_move_anim          0x8560 (u16)   MOV [0x8560],AX in 4c14                     (✓ 6b)
  p2_cell               0x8571 (u8)    CMP [0x8571],0xff sentinel (all P2 fns)     (✓ 6b)
  p2_frame_base         0xa0de (u16)   MOV AX,[0xa0de] in draw_p2_sprite 1cea      (✓ 6b)
  p2_move_state         0x8562 (u8)    MOV [0x8562],AL in p2_set_move_state 4bc6
  p2_move_steps_left    0xa1b0 (u8)    DEC [0xa1b0] in 4c14 ; set in 4bc6
  p2_step_idx           0x8563 (u8)    INC [0x8563] in 4c14 ; ==5 gate in 5003
  p2_facing_neg_dx      0x9d2f (u8)    facing flag (negate dx) in 4c14 ; set in 4bc6
  p2_move_toggle        0x8243 (u8)    XOR 1 half-rate gate at head of 4c14
  p2_move_script_ptr    0xa0ba/0xa0bc  far ptr (off/seg) — [anim,dx,dy] 6-B entries
  p2_state_script_tbl   0x2520/0x2522  far-ptr table indexed by move_state*8 in 4bc6
  p2_state_handler_tbl  0x085c         near-ptr table indexed by move_state*2 in 5003
  p2_grid_col           0xa0ca (s16)   p2_update_grid_cell 4b4e (clamp 0..0x12)
  p2_grid_row           0xa0cc (s16)   p2_update_grid_cell 4b4e (clamp 0..0x16)
  p2_obj_far_ptr        0x9b9e/0x9ba0  far ptr to the P2 sprite/object descriptor struct
                                       (LES BX,[0x9b9e] in draw_p2_sprite 1cea & 4b4e)
  p2_set_cell_col       0x8564 (u8)    p2_set_pixel_from_cell 48a9
  p2_set_cell_row       0x8565 (u8)    p2_set_pixel_from_cell 48a9
  rng_frame             0x79b3 (u8)    MOV AL,[0x79b3] before/after rand() in the AI fns
  p2_ai_rng_threshold   0x7920 (u8)    CMP rng_frame,[0x7920] in select_move_a/b
  p2_dir_blocked_0      0xa0e0 (u8)    dispatch flag in p2_ai_dispatch_move 4f4e
  p2_dir_blocked_1      0xa0e1 (u8)    dispatch flag in 4f4e
  p2_dir_blocked_3      0xa1b2 (u8)    dispatch flag in 4f4e
  pvp_p1_bbox           0x84c/0x84e/0x850/0x852  (x0,x1,y0,y1)  check_pvp_collision 50fb
  pvp_p2_bbox           0x854/0x856/0x858/0x85a  (x0,x1,y0,y1)
  pvp_collision_flag    0xa1aa (u8)    set 0/1 in 50fb
  pvp_sound_sel         0x689c (u16)   cmp [0x689c],4 in 50fb (sound id select)
  physics_frozen        0xa0ce (u8)    read in 50fb
  game_mode             0x792c (u8)    cmp [0x792c],0x30 in 50fb
  current_level         0x79b2 (u8)
  copyprotect_flag      0x119a (s8)

Hooked P2 functions (Ghidra seg-1000 off -> runtime linear 0x1100+off):
  p2_set_move_state       1000:4bc6   (move-state setup: loads script ptr/steps/facing)
  p2_step_scripted_move   1000:4c14   (move-step trajectory: advances p2 pixel x/y)
  p2_update_grid_cell     1000:4b4e   (tile-move: p2 pixel -> grid col/row)
  p2_set_pixel_from_cell  1000:48a9   (p2_cell -> p2 pixel x/y)
  p2_ai_dispatch_move     1000:4f4e   (AI dispatch on dir-blocked flags)
  p2_ai_select_move_a     1000:4f04   (AI rng-branch -> state 1/2/3)
  p2_ai_select_move_b     1000:4f89   (AI rng-branch -> state 1/2/4)
  p2_ai_select_move_random 1000:4fd3  (AI rng -> state (rng&3)+(rand()&1)+5)
  p2_run_move_state_handler 1000:5003 (dispatch move_state -> handler tbl)
  check_pvp_collision     1000:50fb   (P1/P2 AABB overlap -> pvp_collision_flag)
  draw_p2_sprite          1000:1cea   (builds the P2 object descriptor + draw call)

Outputs (BOTH gitignored — discovery; NO commit):
  local/build/render/p2_trace.bin   (frozen layout — see TRACE LAYOUT below)
  local/build/p2_model.md           (per-scenario fn-call sequence + rng behavior + addrs)

TRACE LAYOUT (little-endian) — FROZEN; Task-2 (p2_ctest.c) parses this exactly:
  Header:
    +0x00  8 B   magic   b"P2TRACE1"
    +0x08  2 B   u16     version (=1)
    +0x0A  2 B   u16     n_scenarios
  fn-name string table:
    2 B   u16     n_fn_names
    per name: u8 len, len bytes (ascii)
  Then, per scenario:
    u8        scenario_id
    u8        name_len,  name_len bytes (ascii)
    u8        seeded     (1 = seeded, 0 = scripted)  [always 1 this oracle]
    u8        level
    u32       n_records
    then n_records records.

  Per RECORD (one P2-function call; carries BOTH entry and exit snapshots):
    u16   fn_addr        (Ghidra seg-1000 offset of the hooked fn, e.g. 0x4c14)
    u16   fn_name_idx    (index into the fn-name string table)
    SNAP  entry          (P2SNAP_SIZE-byte fixed struct, see SNAP below)
    SNAP  exit           (P2SNAP_SIZE-byte fixed struct)
    u16   script_off     (p2_move_script offset at ENTRY)
    u16   script_seg     (p2_move_script segment at ENTRY)
    u16   script_len     (# script bytes captured)
    script_len bytes     (raw bytes the far ptr points at — the [anim,dx,dy] entries)
    u8    desc_len       (# descriptor bytes captured at EXIT; 0 unless draw_p2_sprite)
    desc_len bytes       (raw P2 object-descriptor struct bytes at the 0x9b9e far ptr)

  P2SNAP (the P2 + AI globals; little-endian) — fixed P2SNAP_FMT (version 2):
    s16  p2_pixel_x        (0x79ba)
    s16  p2_pixel_y        (0x79bc)
    u16  p2_move_anim      (0x8560)
    s16  p2_grid_col       (0xa0ca)
    s16  p2_grid_row       (0xa0cc)
    u8   p2_cell           (0x8571)
    u8   p2_move_state     (0x8562)
    u8   p2_move_steps_left(0xa1b0)
    u8   p2_step_idx       (0x8563)
    u8   p2_facing_neg_dx  (0x9d2f)
    u8   p2_move_toggle    (0x8243)
    u8   rng_frame         (0x79b3)
    u8   p2_ai_threshold   (0x7920)
    u8   p2_dir_blocked_0  (0xa0e0)
    u8   p2_dir_blocked_1  (0xa0e1)
    u8   p2_dir_blocked_3  (0xa1b2)
    u8   pvp_collision_flag(0xa1aa)
    u8   game_mode         (0x792c)
    u8   physics_frozen    (0xa0ce)
    u16  prng_state0       (0x5676 — FULL 3-word prng state at this snapshot, so the
    u16  prng_state1       (0x5678   host can SEED src/prng.c + reproduce the engine's
    u16  prng_state2       (0x567a   rand() in p2_ai_select_move_random — AI DETERMINISM)

  TRACE VERSION HISTORY:
    v1 (T1/T2): SNAP carried prng_state0's low byte + a pad byte (cross-check only).
    v2 (T4):    SNAP carries the FULL prng_state0/1/2 at entry+exit so the host replay
                of p2_ai_select_move_random is deterministic (seed prng -> rand()).
                The SNAP grew 26->30 bytes; tools/p2_ctest.c parses v2.

Run (sandbox disabled — needs unicorn/uv cache access), HARD timeout:
  timeout 1800 uv run python tools/p2_oracle.py
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
OUT_TRACE = os.path.join(OUT_DIR, "p2_trace.bin")
OUT_MODEL = os.path.join(ROOT, "local/build/p2_model.md")

PSP_SEG = 0x0100
RAM = 0x110000

DG_LIN: int = (0x103b + PSP_SEG + 0x10) * 16   # 0x114b0  (identical to the lineage)
CODE_LIN: int = (PSP_SEG + 0x10) * 16          # 0x1100   (Ghidra seg-1000 runtime base)

# ---------------------------------------------------------------------------
# Resolved P2 DGROUP global offsets (Ghidra DGROUP 0x203b offsets)
# ---------------------------------------------------------------------------
OFF_P2_PIXEL_X: int = 0x79ba        # s16
OFF_P2_PIXEL_Y: int = 0x79bc        # s16
OFF_P2_MOVE_ANIM: int = 0x8560      # u16
OFF_P2_CELL: int = 0x8571           # u8
OFF_P2_FRAME_BASE: int = 0xa0de     # u16
OFF_P2_MOVE_STATE: int = 0x8562     # u8
OFF_P2_MOVE_STEPS_LEFT: int = 0xa1b0  # u8
OFF_P2_STEP_IDX: int = 0x8563       # u8
OFF_P2_FACING_NEG_DX: int = 0x9d2f  # u8
OFF_P2_MOVE_TOGGLE: int = 0x8243    # u8
OFF_P2_GRID_COL: int = 0xa0ca       # s16
OFF_P2_GRID_ROW: int = 0xa0cc       # s16
OFF_P2_SET_CELL_COL: int = 0x8564   # u8
OFF_P2_SET_CELL_ROW: int = 0x8565   # u8

OFF_RNG_FRAME: int = 0x79b3         # u8
OFF_P2_AI_THRESHOLD: int = 0x7920   # u8
OFF_P2_DIR_BLOCKED_0: int = 0xa0e0  # u8
OFF_P2_DIR_BLOCKED_1: int = 0xa0e1  # u8
OFF_P2_DIR_BLOCKED_3: int = 0xa1b2  # u8

OFF_PVP_FLAG: int = 0xa1aa          # u8
OFF_PVP_P1_X0: int = 0x84c          # s16
OFF_PVP_P1_X1: int = 0x84e          # s16
OFF_PVP_P1_Y0: int = 0x850          # s16
OFF_PVP_P1_Y1: int = 0x852          # s16
OFF_PVP_P2_X0: int = 0x854          # s16
OFF_PVP_P2_X1: int = 0x856          # s16
OFF_PVP_P2_Y0: int = 0x858          # s16
OFF_PVP_P2_Y1: int = 0x85a          # s16
OFF_PVP_SOUND_SEL: int = 0x689c     # u16

OFF_GAME_MODE: int = 0x792c         # u8
OFF_PHYSICS_FROZEN: int = 0xa0ce    # u8
OFF_CURRENT_LEVEL: int = 0x79b2     # u8
OFF_COPYPROTECT: int = 0x119a       # s8

OFF_P2_MOVE_SCRIPT_PTR: int = 0xa0ba  # far ptr (off @0xa0ba, seg @0xa0bc)
OFF_P2_OBJ_PTR: int = 0x9b9e          # far ptr (off @0x9b9e, seg @0x9ba0)

# prng state words (reconstructed in src/prng.c; addresses from the rand() callee).
# rand() == FUN_1000_93b1 wraps prng_step (1ce5:001f), whose body (DS=0x203b) reads/
# writes [0x5676]=state0, [0x5678]=state1, [0x567a]=state2 then returns AL = low byte
# of the new prng_state0.  We capture the FULL 3-word prng state at each snapshot so
# the host (src/prng.c) can be seeded to the engine's value and reproduce rand() ->
# the AI-random replay is deterministic (Phase-4 T4).  DGROUP 0x203b offsets:
OFF_PRNG_STATE0: int = 0x5676       # u16  (prng_step `MOV [0x5676],AX`)
OFF_PRNG_STATE1: int = 0x5678       # u16  (prng_step `MOV [0x5678],BX`)
OFF_PRNG_STATE2: int = 0x567a       # u16  (prng_step `MOV [0x567a],BP`)

OFF_KEY_STATE_PTR: int = 0x4D42     # near ptr to g_key_state_table base

# ---------------------------------------------------------------------------
# Hooked P2 functions (Ghidra seg-1000 offsets)
# ---------------------------------------------------------------------------
FN_NAMES: Dict[int, str] = {
    0x4bc6: "p2_set_move_state",
    0x4c14: "p2_step_scripted_move",
    0x4b4e: "p2_update_grid_cell",
    0x48a9: "p2_set_pixel_from_cell",
    0x4f4e: "p2_ai_dispatch_move",
    0x4f04: "p2_ai_select_move_a",
    0x4f89: "p2_ai_select_move_b",
    0x4fd3: "p2_ai_select_move_random",
    0x5003: "p2_run_move_state_handler",
    0x50fb: "check_pvp_collision",
    0x1cea: "draw_p2_sprite",
}
DRAW_FN_OFF: int = 0x1cea

# ---------------------------------------------------------------------------
# Trace format constants
# ---------------------------------------------------------------------------
TRACE_MAGIC: bytes = b"P2TRACE1"
TRACE_VERSION: int = 2   # v2 (T4): SNAP carries the full prng_state0/1/2 (was prng0+pad)
# P2SNAP (v2): 5 leading 16-bit fields, then 14 u8 fields, then 3 trailing u16 prng
# words = 10 + 14 + 6 = 30 bytes.
P2SNAP_FMT: str = "<hhHhh" + "B" * 14 + "HHH"
P2SNAP_SIZE: int = struct.calcsize(P2SNAP_FMT)
SNAP_FIELDS = ["px", "py", "anim", "gcol", "grow",
               "cell", "state", "steps_left", "step_idx", "facing", "toggle",
               "rng", "thresh", "blk0", "blk1", "blk3", "pvp", "mode", "frozen",
               "prng0", "prng1", "prng2"]
assert len(SNAP_FIELDS) == 5 + 14 + 3

SCRIPT_CAP_BYTES: int = 96
DESC_CAP_BYTES: int = 16   # P2 object descriptor struct (x,y,frame, + view fields)

# ---------------------------------------------------------------------------
# Scenarios. Each is a dict with: id, name, fn_off (which engine fn to invoke),
# seed (callable(uc-helpers) -> None to set preconditions), note.
# All seeded (P2 is autonomous). The AI scenarios vary rng_frame to exercise the
# select_move_a / select_move_b / select_move_random rng branches.
# ---------------------------------------------------------------------------
# rng_frame branch points (from the decomp):
#   select_move_a/b: branch on (rng_frame < threshold) then (rng_frame & 1).
#   select_move_random: state = (rng_frame & 3) + (rand() & 1) + 5.
# We sweep rng_frame across {below-thresh even, below-thresh odd, >=thresh} and the
# four (rng&3) residues for random.


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

    def rd_s16(off: int) -> int:
        return struct.unpack("<h", bytes(uc.mem_read(DG_LIN + off, 2)))[0]

    def wr8(off: int, v: int) -> None:
        uc.mem_write(DG_LIN + off, bytes([v & 0xFF]))

    def wr16(off: int, v: int) -> None:
        uc.mem_write(DG_LIN + off, struct.pack("<H", v & 0xFFFF))

    def snap() -> bytes:
        return struct.pack(
            P2SNAP_FMT,
            rd_s16(OFF_P2_PIXEL_X), rd_s16(OFF_P2_PIXEL_Y), rd_u16(OFF_P2_MOVE_ANIM),
            rd_s16(OFF_P2_GRID_COL), rd_s16(OFF_P2_GRID_ROW),
            rd8(OFF_P2_CELL), rd8(OFF_P2_MOVE_STATE), rd8(OFF_P2_MOVE_STEPS_LEFT),
            rd8(OFF_P2_STEP_IDX), rd8(OFF_P2_FACING_NEG_DX), rd8(OFF_P2_MOVE_TOGGLE),
            rd8(OFF_RNG_FRAME), rd8(OFF_P2_AI_THRESHOLD),
            rd8(OFF_P2_DIR_BLOCKED_0), rd8(OFF_P2_DIR_BLOCKED_1), rd8(OFF_P2_DIR_BLOCKED_3),
            rd8(OFF_PVP_FLAG), rd8(OFF_GAME_MODE), rd8(OFF_PHYSICS_FROZEN),
            rd_u16(OFF_PRNG_STATE0), rd_u16(OFF_PRNG_STATE1), rd_u16(OFF_PRNG_STATE2))

    def read_script() -> Tuple[int, int, bytes]:
        off, seg = struct.unpack("<HH", bytes(uc.mem_read(DG_LIN + OFF_P2_MOVE_SCRIPT_PTR, 4)))
        lin = (seg * 16 + off) & 0xFFFFF
        try:
            data = bytes(uc.mem_read(lin, SCRIPT_CAP_BYTES))
        except UcError:
            data = b""
        return off, seg, data

    def read_descriptor() -> bytes:
        off, seg = struct.unpack("<HH", bytes(uc.mem_read(DG_LIN + OFF_P2_OBJ_PTR, 4)))
        lin = (seg * 16 + off) & 0xFFFFF
        try:
            return bytes(uc.mem_read(lin, DESC_CAP_BYTES))
        except UcError:
            return b""

    # ---------------------------------------------------------------------------
    # P2-function hooks (entry + exit via dynamic return-address hook)
    # ---------------------------------------------------------------------------
    capturing = {"on": False}
    cur_records: List[bytes] = []
    reached_fns: collections.Counter = collections.Counter()
    pending_exit: dict = {}
    exit_hook_lins: set = set()
    fn_name_list: List[str] = list(dict.fromkeys(FN_NAMES.values()))
    fn_name_idx = {n: i for i, n in enumerate(fn_name_list)}

    def emit_record(fn_off: int, entry_snap: bytes, exit_snap: bytes,
                    sc_off: int, sc_seg: int, sc_bytes: bytes,
                    desc_bytes: bytes) -> None:
        rec = struct.pack("<HH", fn_off, fn_name_idx[FN_NAMES[fn_off]])
        rec += entry_snap + exit_snap
        rec += struct.pack("<HHH", sc_off & 0xFFFF, sc_seg & 0xFFFF, len(sc_bytes))
        rec += sc_bytes
        rec += struct.pack("<B", len(desc_bytes))
        rec += desc_bytes
        cur_records.append(rec)

    def hook_fn_entry(uc: Uc, addr: int, size: int, _: object) -> None:
        if not capturing["on"]:
            return
        fn_off = (addr - CODE_LIN) & 0xFFFF
        if fn_off not in FN_NAMES:
            return
        reached_fns[fn_off] += 1
        entry_snap = snap()
        sc_off, sc_seg, sc_bytes = read_script()
        ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc.mem_read(ss * 16 + sp, 2)))[0]
        ret_lin = (CODE_LIN + ret_off) & 0xFFFFF
        # Stack a list per ret_lin (recursive/nested same-ret unlikely but safe).
        pending_exit.setdefault(ret_lin, []).append(
            (fn_off, entry_snap, sc_off, sc_seg, sc_bytes))
        if ret_lin not in exit_hook_lins:
            exit_hook_lins.add(ret_lin)
            uc.hook_add(UC_HOOK_CODE, hook_fn_exit, None, ret_lin, ret_lin)

    def hook_fn_exit(uc: Uc, addr: int, size: int, _: object) -> None:
        stack = pending_exit.get(addr)
        if not stack:
            return
        (fn_off, entry_snap, sc_off, sc_seg, sc_bytes) = stack.pop()
        exit_snap = snap()
        desc_bytes = read_descriptor() if fn_off == DRAW_FN_OFF else b""
        emit_record(fn_off, entry_snap, exit_snap, sc_off, sc_seg, sc_bytes, desc_bytes)

    for off in FN_NAMES:
        lin = CODE_LIN + off
        uc.hook_add(UC_HOOK_CODE, hook_fn_entry, None, lin, lin)

    # ---------------------------------------------------------------------------
    # Synthetic near-call into an engine fn (seeded scenarios) — Phase-3 pattern.
    # ---------------------------------------------------------------------------
    STOP_OFF = 0x0008

    def call_engine_fn(fn_off: int, arg_word: Optional[int] = None) -> None:
        uc.mem_write(CODE_LIN + STOP_OFF, b"\xF4")  # HLT sentinel
        ss = uc.reg_read(UC_X86_REG_SS) & 0xFFFF
        sp = uc.reg_read(UC_X86_REG_SP) & 0xFFFF
        # Push an optional cdecl arg word (for p2_set_move_state(state)).
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
            # Restore SP past the arg word the cdecl caller would pop.
            if arg_word is not None:
                sp2 = uc.reg_read(UC_X86_REG_SP) & 0xFFFF
                uc.reg_write(UC_X86_REG_SP, (sp2 + 2) & 0xFFFF)

    # ---------------------------------------------------------------------------
    # Boot to level 1 (identical approach to physics_oracle / items_oracle)
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

    print("[p2_oracle] booting BUMPY (level %d)..." % LEVEL, flush=True)

    while total_instr < 400_000_000:
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e); tr["err"] = err; break
        total_instr += CHUNK
        if total_instr % 40_000_000 == 0:
            print("[p2_oracle] %dM instr, countdown=%s" % (
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
            print("[p2_oracle] level loaded (%s) at chunk %d — settling %d ticks" % (
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
        print("[p2_oracle] ERROR: premature exit/fault during boot: exit=%s fault=%s err=%s" % (
            tr.get("exit"), tr.get("fault"), err), flush=True)
        return
    if not opened(BUMNAME):
        print("[p2_oracle] ERROR: level %s never loaded after %dM instructions" % (
            BUMNAME, total_instr // 1_000_000), flush=True)
        return

    print("[p2_oracle] boot complete. Files: %s" % (
        [o[1] for o in tr.get("fileops", [])]), flush=True)

    # Discover the post-boot P2 state for the report (P2 may be inactive — cell 0xff).
    boot_p2_cell = rd8(OFF_P2_CELL)
    boot_threshold = rd8(OFF_P2_AI_THRESHOLD)
    print("[p2_oracle] boot p2_cell=0x%02x p2_ai_threshold=0x%02x p2_pixel=(%d,%d) "
          "move_state=%d steps_left=%d" % (
              boot_p2_cell, boot_threshold, rd_s16(OFF_P2_PIXEL_X),
              rd_s16(OFF_P2_PIXEL_Y), rd8(OFF_P2_MOVE_STATE),
              rd8(OFF_P2_MOVE_STEPS_LEFT)), flush=True)

    # Snapshot a clean post-boot machine state for per-scenario restore.
    boot_ram = bytes(uc.mem_read(0, RAM))
    boot_ctx = uc.context_save()

    def restore_boot_state() -> None:
        uc.mem_write(0, boot_ram)
        uc.context_restore(boot_ctx)
        tr["fault"] = None
        tr["exit"] = None

    # If P2 is inactive after boot (cell==0xff), every P2 fn early-returns at the
    # sentinel check. We must give P2 a live cell so the bodies execute. Choose a
    # mid-field cell with headroom; documented in p2_model.md.
    P2_SEED_CELL = 0x22 if boot_p2_cell == 0xff else boot_p2_cell

    # ---------------------------------------------------------------------------
    # Build scenarios. Each: (id, name, builder) where builder(seed_helpers) primes
    # state then returns a list of (fn_off, arg_word) calls to invoke + a note.
    # ---------------------------------------------------------------------------
    THRESH = boot_threshold if boot_threshold not in (0x00, 0xff) else 0x80

    def seed_p2_active(cell: int = None, state: int = 1) -> None:
        """Activate P2 onto a cell + derive its pixel pos via the real engine fn."""
        c = P2_SEED_CELL if cell is None else cell
        wr8(OFF_P2_CELL, c)
        call_engine_fn(0x48a9)            # p2_set_pixel_from_cell -> p2_pixel_x/y
        call_engine_fn(0x4bc6, state)     # p2_set_move_state(state): load script/steps

    Scenario = Tuple[int, str, str]  # (id, name, note) — recorded; calls done inline
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
        scenario_results.append(dict(id=sc_id, name=name, note=note,
                                     recs=recs, pre=pre))
        print("[p2_oracle] === scenario %d (%s): %d records ===" % (sc_id, name, len(recs)),
              flush=True)

    # --- Scenario 1: P2 idle (set_pixel_from_cell on a live cell; no move) ----------
    def scn_idle(pre):
        wr8(OFF_P2_CELL, P2_SEED_CELL)
        pre["cell"] = P2_SEED_CELL
        call_engine_fn(0x48a9)            # p2_set_pixel_from_cell
        call_engine_fn(0x4b4e)            # p2_update_grid_cell (pixel->grid)
        pre["px"] = rd_s16(OFF_P2_PIXEL_X); pre["py"] = rd_s16(OFF_P2_PIXEL_Y)
    run_one(1, "p2_idle",
            "Seed p2_cell=0x%02x; invoke p2_set_pixel_from_cell + p2_update_grid_cell. "
            "No move-state; P2 stays put. Establishes the cell<->pixel<->grid mapping." % P2_SEED_CELL,
            scn_idle)

    # --- Scenarios 2a-2c: P2 AI decides a move; VARY rng_frame ----------------------
    # select_move_a/b branch on (rng < threshold) then (rng & 1); dispatch chosen by
    # the dir-blocked flags. We seed flags so dispatch routes to select_move_b
    # (the default: dir_blocked_3==0) and sweep rng_frame to hit each rng branch.
    def make_ai_dispatch(rng_val: int, blk0: int, blk1: int, blk3: int):
        def body(pre):
            seed_p2_active(state=1)
            wr8(OFF_RNG_FRAME, rng_val)
            wr8(OFF_P2_AI_THRESHOLD, THRESH)
            wr8(OFF_P2_DIR_BLOCKED_0, blk0)
            wr8(OFF_P2_DIR_BLOCKED_1, blk1)
            wr8(OFF_P2_DIR_BLOCKED_3, blk3)
            pre.update(rng=rng_val, thresh=THRESH, blk0=blk0, blk1=blk1, blk3=blk3)
            call_engine_fn(0x4f4e)        # p2_ai_dispatch_move (full AI decision)
            pre["state_after"] = rd8(OFF_P2_MOVE_STATE)
        return body

    # 2a: rng below threshold, even -> select_move_b path -> state 2 (if blk1==0)
    run_one(2, "p2_ai_rng_below_even",
            "AI dispatch, dir_blocked_3=0 -> select_move_b; rng_frame < threshold & even "
            "-> expect move_state 2 (blk1=0).",
            make_ai_dispatch(THRESH - 2 if THRESH >= 2 else 0, 0, 0, 0))
    # 2b: rng below threshold, odd -> select_move_b path -> state 1 (if blk0==0)
    run_one(3, "p2_ai_rng_below_odd",
            "AI dispatch -> select_move_b; rng_frame < threshold & odd -> expect "
            "move_state 1 (blk0=0).",
            make_ai_dispatch((THRESH - 1) | 1 if THRESH >= 1 else 1, 0, 0, 0))
    # 2c: rng >= threshold -> select_move_b -> state 4
    run_one(4, "p2_ai_rng_above",
            "AI dispatch -> select_move_b; rng_frame >= threshold -> expect move_state 4.",
            make_ai_dispatch(0xff, 0, 0, 0))

    # 2d: route dispatch to select_move_a (dir_blocked_3=1, blocked_0=0)
    run_one(5, "p2_ai_dispatch_select_a",
            "AI dispatch with dir_blocked_3=1, dir_blocked_0=0 -> p2_choose_move_state1 "
            "path; rng below-even.",
            make_ai_dispatch(THRESH - 2 if THRESH >= 2 else 0, 0, 1, 1))

    # --- Scenario 3: select_move_random directly, sweep (rng & 3) -------------------
    def make_random(rng_val: int):
        def body(pre):
            seed_p2_active(state=1)
            wr8(OFF_RNG_FRAME, rng_val)
            pre["rng"] = rng_val
            call_engine_fn(0x4fd3)        # p2_ai_select_move_random
            pre["state_after"] = rd8(OFF_P2_MOVE_STATE)
        return body
    for k, rng_val in enumerate([0x00, 0x01, 0x02, 0x03]):
        run_one(6 + k, "p2_ai_random_rng%d" % rng_val,
                "select_move_random: state=(rng&3)+(rand()&1)+5; rng_frame=0x%02x "
                "(rng&3=%d)." % (rng_val, rng_val & 3),
                make_random(rng_val))

    # --- Scenario 4: P2 move-step trajectory (p2_step_scripted_move) ----------------
    def scn_move_step(pre):
        seed_p2_active(state=1)
        pre["px0"] = rd_s16(OFF_P2_PIXEL_X); pre["py0"] = rd_s16(OFF_P2_PIXEL_Y)
        pre["steps0"] = rd8(OFF_P2_MOVE_STEPS_LEFT)
        # The fn has a half-rate toggle (XOR 1) -> only advances when toggle was 0.
        # Pre-set toggle so the FIRST call advances, and run several steps.
        wr8(OFF_P2_MOVE_TOGGLE, 1)        # XOR 1 -> 0 -> NOT the active branch
        for _ in range(12):
            call_engine_fn(0x4c14)        # p2_step_scripted_move
            call_engine_fn(0x4b4e)        # p2_update_grid_cell (track grid)
        pre["px1"] = rd_s16(OFF_P2_PIXEL_X); pre["py1"] = rd_s16(OFF_P2_PIXEL_Y)
        pre["steps1"] = rd8(OFF_P2_MOVE_STEPS_LEFT)
    run_one(10, "p2_move_step",
            "Seed P2 active + move_state 1, then invoke p2_step_scripted_move x12 "
            "(+update_grid_cell). Captures the P2 trajectory advancing along its move-script.",
            scn_move_step)

    # --- Scenario 5: P2 run_move_state_handler at the step==5 dispatch boundary ------
    def scn_handler(pre):
        seed_p2_active(state=2)
        wr8(OFF_P2_STEP_IDX, 5)           # hit the ==5 dispatch gate in 5003
        pre["state"] = rd8(OFF_P2_MOVE_STATE)
        call_engine_fn(0x5003)            # p2_run_move_state_handler
        pre["state_after"] = rd8(OFF_P2_MOVE_STATE)
    run_one(11, "p2_run_move_state_handler",
            "Seed move_state=2, step_idx=5 -> hit the dispatch gate; invoke "
            "p2_run_move_state_handler (routes to the per-state cell-move handler).",
            scn_handler)

    # --- Scenario 6: pvp collision (seed overlapping vs disjoint P1/P2 bboxes) -------
    def scn_pvp(overlap: bool):
        def body(pre):
            seed_p2_active(state=1)
            wr8(OFF_PHYSICS_FROZEN, 0)
            wr8(OFF_GAME_MODE, 0x10)      # not 0x30 (the early-return mode)
            wr8(0x856d, 0)                # the second early-return guard
            # P1 bbox
            wr16(OFF_PVP_P1_X0, 50); wr16(OFF_PVP_P1_X1, 70)
            wr16(OFF_PVP_P1_Y0, 50); wr16(OFF_PVP_P1_Y1, 70)
            if overlap:
                wr16(OFF_PVP_P2_X0, 60); wr16(OFF_PVP_P2_X1, 80)
                wr16(OFF_PVP_P2_Y0, 60); wr16(OFF_PVP_P2_Y1, 80)
            else:
                wr16(OFF_PVP_P2_X0, 200); wr16(OFF_PVP_P2_X1, 220)
                wr16(OFF_PVP_P2_Y0, 200); wr16(OFF_PVP_P2_Y1, 220)
            pre["overlap"] = overlap
            call_engine_fn(0x50fb)        # check_pvp_collision
            pre["flag_after"] = rd8(OFF_PVP_FLAG)
        return body
    run_one(12, "pvp_overlap",
            "Seed overlapping P1/P2 AABBs (not frozen, mode!=0x30); invoke "
            "check_pvp_collision -> expect pvp_collision_flag=1.",
            scn_pvp(True))
    run_one(13, "pvp_disjoint",
            "Seed disjoint P1/P2 AABBs; invoke check_pvp_collision -> expect "
            "pvp_collision_flag=0.",
            scn_pvp(False))

    # --- Scenario 7: P2 draw (draw_p2_sprite -> the object descriptor) --------------
    def scn_draw(pre):
        seed_p2_active(state=1)
        wr16(OFF_P2_MOVE_ANIM, 3)         # nonzero anim offset
        pre["anim"] = rd_u16(OFF_P2_MOVE_ANIM)
        pre["frame_base"] = rd_u16(OFF_P2_FRAME_BASE)
        pre["px"] = rd_s16(OFF_P2_PIXEL_X); pre["py"] = rd_s16(OFF_P2_PIXEL_Y)
        call_engine_fn(0x1cea)            # draw_p2_sprite -> builds descriptor + draw
        d = read_descriptor()
        pre["desc"] = d.hex()
    run_one(14, "p2_draw",
            "Seed P2 active + move_anim; invoke draw_p2_sprite -> capture the produced "
            "object descriptor (px, py, frame=frame_base+move_anim) at the 0x9b9e far ptr.",
            scn_draw)

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
    print("[p2_oracle] wrote %s (%d B)" % (OUT_TRACE, trace_bytes), flush=True)

    # ---------------------------------------------------------------------------
    # Decode records for the model md
    # ---------------------------------------------------------------------------
    def decode_records(recs: List[bytes]) -> List[dict]:
        out = []
        for r in recs:
            fn_off, name_idx = struct.unpack_from("<HH", r, 0)
            o = 4
            ent = struct.unpack_from(P2SNAP_FMT, r, o); o += P2SNAP_SIZE
            ex = struct.unpack_from(P2SNAP_FMT, r, o); o += P2SNAP_SIZE
            sc_off, sc_seg, sc_len = struct.unpack_from("<HHH", r, o); o += 6
            sc_bytes = r[o:o + sc_len]; o += sc_len
            (desc_len,) = struct.unpack_from("<B", r, o); o += 1
            desc_bytes = r[o:o + desc_len]; o += desc_len
            out.append(dict(fn=FN_NAMES[fn_off], fn_off=fn_off, ent=ent, ex=ex,
                            sc_bytes=sc_bytes, desc=desc_bytes))
        return out

    def sd(t: tuple) -> dict:
        return dict(zip(SNAP_FIELDS, t))

    # ---------------------------------------------------------------------------
    # p2_model.md
    # ---------------------------------------------------------------------------
    L: List[str] = []
    L.append("# Bumpy Phase-4 Player-2 capture model (discovery)\n\n")
    L.append("Generated by `tools/p2_oracle.py`. Capture granularity = P2 "
             "AI-decision / move-state / draw FUNCTION-CALL boundary (entry+exit).\n\n")
    L.append("All scenarios are **seeded** (P2 is AI-controlled / autonomous, not "
             "key-driven): the precondition globals are set, then the REAL engine fn is "
             "invoked at its entry IP via a synthetic near-call (unmodified original "
             "body). The AI scenarios **vary `rng_frame`** to exercise the rng branches.\n\n")

    L.append("## Resolved P2 DGROUP addresses (Ghidra DGROUP 0x203b offsets)\n\n")
    L.append("Read live via Ghidra MCP from the disassembly operands of the P2 fns.\n\n")
    L.append("| symbol | offset | type | provenance |\n|---|---|---|---|\n")
    rows = [
        ("p2_pixel_x", 0x79ba, "s16", "MOV [0x79ba] in p2_step_scripted_move 4c14 (✓ 6b)"),
        ("p2_pixel_y", 0x79bc, "s16", "ADD word[0x79bc] in 4c14 (✓ 6b)"),
        ("p2_move_anim", 0x8560, "u16", "MOV [0x8560],AX in 4c14 (✓ 6b)"),
        ("p2_cell", 0x8571, "u8", "CMP [0x8571],0xff sentinel, all P2 fns (✓ 6b)"),
        ("p2_frame_base", 0xa0de, "u16", "MOV AX,[0xa0de] in draw_p2_sprite 1cea (✓ 6b)"),
        ("p2_move_state", 0x8562, "u8", "MOV [0x8562],AL in p2_set_move_state 4bc6"),
        ("p2_move_steps_left", 0xa1b0, "u8", "DEC [0xa1b0] in 4c14; set in 4bc6"),
        ("p2_step_idx", 0x8563, "u8", "INC [0x8563] in 4c14; ==5 gate in 5003"),
        ("p2_facing_neg_dx", 0x9d2f, "u8", "facing flag (negate dx) in 4c14; set in 4bc6"),
        ("p2_move_toggle", 0x8243, "u8", "XOR 1 half-rate gate at head of 4c14"),
        ("p2_move_script_ptr", 0xa0ba, "far", "off 0xa0ba / seg 0xa0bc; [anim,dx,dy] entries"),
        ("p2_state_script_tbl", 0x2520, "tbl", "far-ptr tbl indexed by state*8 in 4bc6 (0x2520/0x2522)"),
        ("p2_state_handler_tbl", 0x085c, "tbl", "near-ptr tbl indexed by state*2 in 5003"),
        ("p2_grid_col", 0xa0ca, "s16", "p2_update_grid_cell 4b4e (clamp 0..0x12)"),
        ("p2_grid_row", 0xa0cc, "s16", "p2_update_grid_cell 4b4e (clamp 0..0x16)"),
        ("p2_obj_far_ptr", 0x9b9e, "far", "off 0x9b9e/seg 0x9ba0; P2 sprite/object descriptor"),
        ("p2_set_cell_col", 0x8564, "u8", "p2_set_pixel_from_cell 48a9"),
        ("p2_set_cell_row", 0x8565, "u8", "p2_set_pixel_from_cell 48a9"),
        ("rng_frame", 0x79b3, "u8", "MOV AL,[0x79b3] before/after rand() in AI fns"),
        ("p2_ai_rng_threshold", 0x7920, "u8", "CMP rng_frame,[0x7920] in select_move_a/b"),
        ("p2_dir_blocked_0", 0xa0e0, "u8", "dispatch flag in p2_ai_dispatch_move 4f4e"),
        ("p2_dir_blocked_1", 0xa0e1, "u8", "dispatch flag in 4f4e"),
        ("p2_dir_blocked_3", 0xa1b2, "u8", "dispatch flag in 4f4e"),
        ("pvp_p1_bbox", 0x84c, "s16x4", "0x84c/0x84e/0x850/0x852 (x0,x1,y0,y1) in 50fb"),
        ("pvp_p2_bbox", 0x854, "s16x4", "0x854/0x856/0x858/0x85a (x0,x1,y0,y1)"),
        ("pvp_collision_flag", 0xa1aa, "u8", "set 0/1 in check_pvp_collision 50fb"),
        ("pvp_sound_sel", 0x689c, "u16", "cmp [0x689c],4 in 50fb"),
        ("physics_frozen", 0xa0ce, "u8", "read in 50fb"),
        ("game_mode", 0x792c, "u8", "cmp [0x792c],0x30 in 50fb"),
        ("current_level", 0x79b2, "u8", "boot level select"),
        ("copyprotect_flag", 0x119a, "s8", "boot guard"),
    ]
    for sym, off, ty, prov in rows:
        L.append("| %s | 0x%04x | %s | %s |\n" % (sym, off, ty, prov))
    L.append("\n")

    L.append("## Hooked P2 functions (Ghidra seg-1000 offsets)\n\n")
    L.append("| addr (1000:off) | name |\n|---|---|\n")
    for off, nm in sorted(FN_NAMES.items()):
        L.append("| 1000:%04x | %s |\n" % (off, nm))
    L.append("\n")

    L.append("## AI rng-decision behavior (recovered from the decomp + capture)\n\n")
    L.append("`p2_ai_dispatch_move` (4f4e) routes on the dir-blocked flags:\n")
    L.append("- `dir_blocked_3==0` -> `p2_ai_select_move_b` (4f89)\n")
    L.append("- else `dir_blocked_1==0` -> `p2_choose_move_state2` (4e7f)\n")
    L.append("- else `dir_blocked_0==0` -> `p2_choose_move_state1` (4dfa)\n")
    L.append("- else -> `p2_ai_select_move_a` (4f04)\n\n")
    L.append("`select_move_a`/`select_move_b` branch on `rng_frame`:\n")
    L.append("```\n")
    L.append("if (rng_frame < p2_ai_rng_threshold[0x7920]) {\n")
    L.append("    if ((rng_frame & 1) == 0) state = (dir_blocked_1==0) ? 2 : (a?3:4);\n")
    L.append("    else                      state = (dir_blocked_0==0) ? 1 : (a?3:4);\n")
    L.append("} else                        state = (a?3:4);\n")
    L.append("p2_set_move_state(state);   // a=select_move_a tail-state 3, b tail-state 4\n")
    L.append("```\n")
    L.append("`select_move_random` (4fd3): `state = (rng_frame & 3) + (rand() & 1) + 5` "
             "then `p2_set_move_state(state)` — i.e. one of move-states 5..8 chosen by the "
             "low 2 bits of `rng_frame` plus a fresh rand() parity bit.\n\n")

    for sr in scenario_results:
        dec = decode_records(sr["recs"])
        L.append("## Scenario %d — %s\n\n" % (sr["id"], sr["name"]))
        L.append("- **seeded**. %s\n" % sr["note"])
        if sr["pre"]:
            L.append("- pre/post: %s\n" % ", ".join(
                "%s=%s" % (k, v) for k, v in sr["pre"].items() if k != "desc"))
        L.append("- records: %d\n\n" % len(dec))
        L.append("| # | fn | rng | px e->x | py e->x | state e->x | steps e->x | cell | pvp e->x |\n")
        L.append("|---|---|---|---|---|---|---|---|---|\n")
        for i, d in enumerate(dec[:60]):
            e = sd(d["ent"]); x = sd(d["ex"])
            L.append("| %d | %s | %d | %d->%d | %d->%d | %d->%d | %d->%d | %d | %d->%d |\n" % (
                i, d["fn"], e["rng"], e["px"], x["px"], e["py"], x["py"],
                e["state"], x["state"], e["steps_left"], x["steps_left"],
                e["cell"], e["pvp"], x["pvp"]))
        if len(dec) > 60:
            L.append("\n(+%d more records)\n" % (len(dec) - 60))
        # recovered move-script for the move scenarios
        for d in dec:
            if d["fn"] == "p2_step_scripted_move" and d["sc_bytes"]:
                b = d["sc_bytes"]
                ent = []
                for j in range(0, min(len(b), 36), 6):
                    if j + 6 <= len(b):
                        anim, dx, dy = struct.unpack_from("<hhh", b, j)
                        ent.append("[a=%d dx=%d dy=%d]" % (anim, dx, dy))
                L.append("\n- recovered P2 move-script (state-1 entry): %s\n" % " ".join(ent))
                break
        # draw descriptor
        for d in dec:
            if d["fn"] == "draw_p2_sprite" and d["desc"]:
                L.append("\n- P2 draw descriptor (16 B): `%s`\n" % d["desc"].hex())
                vals = struct.unpack_from("<HHHHHHHH", d["desc"], 0)
                L.append("  decoded words: x=%d y=%d frame=%d %s\n" % (
                    vals[0], vals[1], vals[2], list(vals[3:])))
                break
        L.append("\n")

    # ---- P2 sanity per scenario -----------------------------------------------------
    L.append("## P2 sanity per scenario\n\n")
    for sr in scenario_results:
        dec = decode_records(sr["recs"])
        name = sr["name"]
        if name == "p2_move_step":
            pxs = [sd(d["ex"])["px"] for d in dec if d["fn"] == "p2_step_scripted_move"]
            pys = [sd(d["ex"])["py"] for d in dec if d["fn"] == "p2_step_scripted_move"]
            moved = (len(set(pxs)) > 1) or (len(set(pys)) > 1)
            L.append("- **%s**: px range %s py range %s -> P2 pos %s on move-step.\n" % (
                name, (min(pxs), max(pxs)) if pxs else "n/a",
                (min(pys), max(pys)) if pys else "n/a",
                "CHANGES" if moved else "DID NOT CHANGE"))
        elif name.startswith("p2_ai_") and "state_after" in sr["pre"]:
            L.append("- **%s**: rng_frame=0x%02x -> move_state=%d (AI decision).\n" % (
                name, sr["pre"].get("rng", -1) & 0xff, sr["pre"]["state_after"]))
        elif name.startswith("pvp_"):
            L.append("- **%s**: -> pvp_collision_flag=%s.\n" % (
                name, sr["pre"].get("flag_after")))
        elif name == "p2_draw":
            L.append("- **%s**: descriptor captured (len=%d).\n" % (
                name, len(sr["pre"].get("desc", "")) // 2))
        else:
            L.append("- **%s**: %d records captured.\n" % (name, len(dec)))
    # AI varies-with-rng sanity (collect the rng->state pairs from the random scenarios)
    rand_pairs = [(sr["pre"].get("rng"), sr["pre"].get("state_after"))
                  for sr in scenario_results if sr["name"].startswith("p2_ai_random")]
    L.append("\n**AI rng-determinism check** (select_move_random rng->state): %s — "
             "distinct states across rng => AI decision VARIES with rng_frame.\n" % rand_pairs)

    os.makedirs(os.path.dirname(OUT_MODEL), exist_ok=True)
    with open(OUT_MODEL, "w") as f:
        f.write("".join(L))
    print("[p2_oracle] wrote %s" % OUT_MODEL, flush=True)

    # ---------------------------------------------------------------------------
    # Console summary
    # ---------------------------------------------------------------------------
    print("\n[p2_oracle] REACHED P2 functions:", flush=True)
    for off, cnt in sorted(reached_fns.items()):
        print("   1000:%04x  %-28s x%d" % (off, FN_NAMES[off], cnt), flush=True)
    print("[p2_oracle] AI random rng->state pairs:", rand_pairs, flush=True)
    if err:
        print("[p2_oracle] emu error:", err, flush=True)
    if tr.get("call_err"):
        print("[p2_oracle] call_err:", tr["call_err"], flush=True)


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
