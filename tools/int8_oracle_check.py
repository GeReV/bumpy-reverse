#!/usr/bin/env python3
"""int8_oracle_check.py — the int8 capture TRUST ANCHOR.

Confirms the DOSBox int8 capture (tools/dosbox/patches/02-int8-snap-capture.patch)
and the Unicorn per-function oracle AGREE on the captured INIT + first frame(s) — i.e.
that the DGROUP calibration (dgroup_seg 0x185f) and the DGROUP field offsets in the
capture patch are RIGHT (the capture is reading the real engine state from the real
places).  If frame-0 calibration agrees, the trust anchor holds and the int8 trace is
trustworthy as a replay input for the Task-7 real-replay gate.

HOW THE CROSS-CHECK WORKS
-------------------------
The capture emits, per FRAME, the player pixel position (p1_pixel_x/y @ 0x9290/0x9292)
AND the player AABB (pvp_p1_x0/x1/y0/y1 @ 0x84c/0x84e/0x850/0x852).  The engine fn
`update_p1_bbox` (Ghidra 1000:5085) is a PURE, deterministic map  pixel -> AABB
(gated by physics_frozen @ 0xa0ce).  So for each captured frame we:

  1. boot the REAL (unpacked) BUMPY.EXE under Unicorn to level 1 — reusing the boot +
     DGROUP + int/VGA scaffold + call_engine_fn primitive of tools/p1_spine_oracle.py
     (the same primitive that oracle uses to invoke real engine fns with seeded
     preconditions; we deliberately re-derive the scaffold here rather than refactor
     the oracle, matching the project's oracle-isolation convention);
  2. SEED the captured frame's p1_pixel_x / p1_pixel_y / physics_frozen into DGROUP;
  3. INVOKE the unmodified engine `update_p1_bbox`;
  4. READ the engine's computed pvp_p1_x0/x1/y0/y1 back from DGROUP and ASSERT they
     equal the captured frame's bbox fields.

This is a GENUINE cross-check: if the capture read p1_pixel_x from the wrong offset, or
the bbox offsets were wrong, or the DGROUP seg were miscalibrated, the engine's
recomputed bbox would NOT match the captured bbox and the check FAILS (non-zero exit).

On a calibration mismatch it prints
    CALIBRATION MISMATCH field=… dosbox=… oracle=…
and exits 1.  On agreement it prints a PASS line and exits 0.

WHY update_p1_bbox is the right anchor: it is the one leading per-tick function whose
output is a closed-form function of two captured INIT/FRAME scalars (the pixel pos) with
no dependency on the KNOWN-DEFERRED read-set gaps (entity_state[0x200] reserved zeros,
view/sprite descriptor content) that Tasks 3/4/5 flagged for the Task-7 replay loop.  It
therefore validates the calibration cleanly without rabbit-holing on deferred-field
noise that only surfaces in a full game_tick replay.

Run (sandbox disabled — needs unicorn/uv cache + the game files; HARD timeout):
  timeout 1800 uv run python tools/int8_oracle_check.py local/build/render/int8_trace.bin
"""
from __future__ import annotations
import os
import struct
import sys
import collections
from typing import Dict, List, Optional, Tuple

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_CODE,
                     UC_HOOK_MEM_UNMAPPED, UC_HOOK_INSN, UC_HOOK_MEM_WRITE,
                     UC_HOOK_MEM_READ, UcError)
from unicorn.x86_const import *

# ---------------------------------------------------------------------------
# Paths (identical lineage to tools/p1_spine_oracle.py)
# ---------------------------------------------------------------------------
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_DIR = os.path.join(ROOT, "local/originals/old-games/bumpy")
EXE = os.path.join(ROOT, "local/originals/unpacked/BUMPY_unpacked.exe")

PSP_SEG = 0x0100
RAM = 0x110000
DG_LIN: int = (0x103b + PSP_SEG + 0x10) * 16   # 0x114b0
CODE_LIN: int = (PSP_SEG + 0x10) * 16          # 0x1100

# ---------------------------------------------------------------------------
# DGROUP offsets touched by the cross-check (the SAME offsets the capture patch
# uses — that is the whole point: we read/seed from these and the engine confirms
# them by recomputing the dependent bbox).
# ---------------------------------------------------------------------------
OFF_P1_PIXEL_X = 0x9290
OFF_P1_PIXEL_Y = 0x9292
OFF_PHYSICS_FROZEN = 0xa0ce
OFF_PVP_P1_X0 = 0x84c
OFF_PVP_P1_X1 = 0x84e
OFF_PVP_P1_Y0 = 0x850
OFF_PVP_P1_Y1 = 0x852
OFF_CURRENT_LEVEL = 0x79b2
OFF_COPYPROTECT = 0x119a
OFF_KEY_STATE_PTR = 0x4D42

FN_UPDATE_P1_BBOX = 0x5085   # Ghidra 1000:5085 — pure pixel -> AABB
STOP_OFF = 0x0008

# ---------------------------------------------------------------------------
# Trace layout constants — MUST match tools/int8_trace.h byte-for-byte.
# ---------------------------------------------------------------------------
INT8_MAGIC = b"BINT"
INT8_VERSION = 1
HEADER_FMT = "<4sHHHHH"
HEADER_SIZE = struct.calcsize(HEADER_FMT)               # 14
TILEMAP_SIZE = 0x300
ANIM_RECS_LEN = 84
ENTITY_LEN = 0x200
SCALARS_SIZE = 81
INIT_SIZE = TILEMAP_SIZE + ANIM_RECS_LEN + ENTITY_LEN + SCALARS_SIZE   # 1445
FRAME_SIZE = 61

# int8_frame_state field layout (55 bytes) — mirrors struct int8_frame_state.
# 13 s16, then 2 u16 (score), then 22 u8.
FRAME_STATE_S16 = ["p1_pixel_x", "p1_pixel_y", "p1_grid_x_new", "p1_grid_y_new",
                   "p1_grid_x", "p1_grid_y", "p1_grid_x_prev", "p1_grid_y_prev",
                   "pvp_p1_x0", "pvp_p1_x1", "pvp_p1_y0", "pvp_p1_y1",
                   "p1_pixel_y_dup"]
FRAME_STATE_U16 = ["score_lo", "score_hi"]
FRAME_STATE_U8 = ["p1_move_anim", "game_mode", "p1_move_step_idx", "p1_facing_left",
                  "p1_move_steps_left", "input_state", "physics_frozen", "move_override",
                  "p1_cell", "move_locked", "prev_game_mode", "p1_step_col_count",
                  "pending_erase_count", "level_complete_flag", "items_remaining",
                  "level_exit_cell", "level_complete_anim_counter", "p1_item_code",
                  "anim_target_cell", "current_level", "move_step_count",
                  "p2_cell"]   # (p2_move_state + frame_base lo/hi follow; unused here)

# int8_scalars field layout (81 bytes) — mirrors struct int8_scalars.
SCALARS_S16 = ["p1_pixel_x", "p1_pixel_y", "p1_grid_x_new", "p1_grid_y_new",
               "p1_grid_x", "p1_grid_y", "p1_grid_x_prev", "p1_grid_y_prev",
               "p1_scroll_x", "p1_scroll_y", "pvp_p1_x0", "pvp_p1_x1", "pvp_p1_y0",
               "pvp_p1_y1", "pending_erase_x", "pending_erase_y", "sound_device_state"]
SCALARS_U16 = ["score_lo", "score_hi", "p2_frame_base", "g_anim_stream_off",
               "g_anim_stream_seg", "anim_b_stream_off", "anim_b_stream_seg"]
SCALARS_U8 = ["p1_move_anim", "game_mode", "p1_move_step_idx", "p1_facing_left",
              "p1_move_steps_left", "input_state", "physics_frozen", "move_override",
              "p1_cell", "move_locked", "prev_game_mode", "p1_step_col_count",
              "pending_erase_count", "level_complete_flag", "items_remaining",
              "level_exit_cell", "level_complete_anim_counter", "p1_item_code",
              "anim_target_cell", "current_level", "move_step_count",
              "g_anim_channel_idx", "g_anim_cur_cmd_byte", "anim_b_loop_idx",
              "anim_b_cur_frame_byte", "p2_cell", "p2_move_state", "p2_ai_threshold",
              "round_continue_flag", "session_continue_flag", "frame_abort_flag",
              "settle_countdown", "rng_frame"]


# ---------------------------------------------------------------------------
# Trace parsing (pure struct, matching int8_trace.h)
# ---------------------------------------------------------------------------
def parse_scalars(buf: bytes, off: int) -> Dict[str, int]:
    out: Dict[str, int] = {}
    o = off
    for n in SCALARS_S16:
        out[n] = struct.unpack_from("<h", buf, o)[0]; o += 2
    for n in SCALARS_U16:
        out[n] = struct.unpack_from("<H", buf, o)[0]; o += 2
    for n in SCALARS_U8:
        out[n] = struct.unpack_from("<B", buf, o)[0]; o += 1
    assert o - off == SCALARS_SIZE, (o - off, SCALARS_SIZE)
    return out


def parse_frame(buf: bytes, off: int) -> Dict[str, int]:
    rng, inp, thash = struct.unpack_from("<BBI", buf, off)
    out: Dict[str, int] = {"rng": rng, "input": inp, "tilemap_hash": thash}
    o = off + 6
    for n in FRAME_STATE_S16:
        out[n] = struct.unpack_from("<h", buf, o)[0]; o += 2
    for n in FRAME_STATE_U16:
        out[n] = struct.unpack_from("<H", buf, o)[0]; o += 2
    for n in FRAME_STATE_U8:
        out[n] = struct.unpack_from("<B", buf, o)[0]; o += 1
    return out


def parse_trace(path: str) -> Tuple[Dict[str, int], Dict[str, int], List[Dict[str, int]]]:
    """Return (header, init_scalars, frames[0..N])."""
    buf = open(path, "rb").read()
    magic, ver, dg, fc, init_sz, fstride = struct.unpack_from(HEADER_FMT, buf, 0)
    if magic != INT8_MAGIC:
        raise SystemExit("bad magic %r (not %r)" % (magic, INT8_MAGIC))
    if ver != INT8_VERSION:
        raise SystemExit("trace version %d != expected %d (stale layout)" % (ver, INT8_VERSION))
    if init_sz != INIT_SIZE or fstride != FRAME_SIZE:
        raise SystemExit("trace init_size=%d/frame_stride=%d != %d/%d (layout drift)"
                         % (init_sz, fstride, INIT_SIZE, FRAME_SIZE))
    header = dict(magic=magic, version=ver, dgroup_seg=dg, frame_count=fc,
                  init_size=init_sz, frame_stride=fstride)
    sc_off = HEADER_SIZE + TILEMAP_SIZE + ANIM_RECS_LEN + ENTITY_LEN
    init = parse_scalars(buf, sc_off)
    fr0 = HEADER_SIZE + init_sz
    n_records = (len(buf) - fr0) // FRAME_SIZE   # = frame_count + 1 (FRAME[0] mirror)
    frames = [parse_frame(buf, fr0 + k * FRAME_SIZE) for k in range(n_records)]
    return header, init, frames


# ---------------------------------------------------------------------------
# MZ loader + DOS file shim (identical to the oracle lineage)
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


# ---------------------------------------------------------------------------
# The Unicorn boot harness — a trimmed copy of tools/p1_spine_oracle.py's scaffold,
# kept self-contained (oracle-isolation convention).  Exposes seed/read/call_engine_fn.
# ---------------------------------------------------------------------------
class Engine:
    def __init__(self) -> None:
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

        self.uc = uc
        self.base = base
        self.hdr = hdr
        self.files = Files()
        self.tr: dict = dict(ints=collections.Counter(), last_ip=0, keys=list("\r "))
        self.free_top = [0x1C00]
        self.FREE_END = 0x9000
        self.dg = (0x103b + base) & 0xFFFF
        self._install_hooks()
        self._init_regs()

    # ---- low-level helpers ----
    def set_cf(self, set_it: bool) -> None:
        fl = self.uc.reg_read(UC_X86_REG_EFLAGS)
        self.uc.reg_write(UC_X86_REG_EFLAGS, (fl | 1) if set_it else (fl & ~1))

    def rd8(self, off: int) -> int:
        return self.uc.mem_read(DG_LIN + off, 1)[0]

    def rd_s16(self, off: int) -> int:
        return struct.unpack("<h", bytes(self.uc.mem_read(DG_LIN + off, 2)))[0]

    def wr8(self, off: int, v: int) -> None:
        self.uc.mem_write(DG_LIN + off, bytes([v & 0xFF]))

    def wr16(self, off: int, v: int) -> None:
        self.uc.mem_write(DG_LIN + off, struct.pack("<H", v & 0xFFFF))

    def cur_lin(self) -> int:
        return ((self.uc.reg_read(UC_X86_REG_CS) & 0xFFFF) * 16
                + (self.uc.reg_read(UC_X86_REG_IP) & 0xFFFF)) & 0xFFFFF

    def opened(self, name: str) -> bool:
        return any(o[0] == "open" and o[1] == name for o in self.tr.get("fileops", []))

    def set_key(self, scancode: int, down: bool) -> None:
        mbase = struct.unpack("<H", bytes(self.uc.mem_read(DG_LIN + OFF_KEY_STATE_PTR, 2)))[0]
        self.uc.mem_write(DG_LIN + mbase + (scancode & 0x7F),
                          bytes([scancode if down else 0]))

    def fire_int(self, n: int) -> None:
        uc = self.uc
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

    # ---- hooks (int21/16/10 shim + VGA, mirrors the oracle) ----
    def _install_hooks(self) -> None:
        uc = self.uc
        tr = self.tr
        files = self.files
        free_top = self.free_top
        FREE_END = self.FREE_END

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
                        self.set_cf(True)
                    else:
                        uc.reg_write(UC_X86_REG_AX, free_top[0])
                        free_top[0] += bx
                        self.set_cf(False)
                elif ah in (0x49, 0x4A):
                    self.set_cf(False)
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
        attr = bytearray(32)
        for _i, _a in enumerate([0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17]):
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

    def _init_regs(self) -> None:
        uc = self.uc; hdr = self.hdr; base = self.base
        uc.reg_write(UC_X86_REG_DS, PSP_SEG)
        uc.reg_write(UC_X86_REG_ES, PSP_SEG)
        uc.reg_write(UC_X86_REG_SS, (hdr["ss"] + base) & 0xFFFF)
        uc.reg_write(UC_X86_REG_SP, hdr["sp"])
        uc.reg_write(UC_X86_REG_CS, (hdr["cs"] + base) & 0xFFFF)
        uc.reg_write(UC_X86_REG_IP, hdr["ip"])

    # ---- boot to level 1 (identical to p1_spine_oracle.py) ----
    def boot(self, level: int = 1) -> bool:
        uc = self.uc
        tr = self.tr
        PAVNAME = "D%d.PAV" % level
        BUMNAME = "D%d.BUM" % level

        def force_level() -> None:
            self.wr8(OFF_CURRENT_LEVEL, level)
            uc.mem_write(DG_LIN + OFF_COPYPROTECT, bytes([1]))

        CHUNK = 1_000_000
        total_instr = 0
        begin = self.cur_lin()
        countdown: Optional[int] = None
        SETTLE_TICKS = 80
        print("[int8_oracle_check] booting BUMPY (level %d)..." % level, flush=True)
        while total_instr < 400_000_000:
            try:
                uc.emu_start(begin, 0, count=CHUNK)
            except UcError as e:
                tr["err"] = str(e); break
            total_instr += CHUNK
            if total_instr % 40_000_000 == 0:
                print("[int8_oracle_check] %dM instr, countdown=%s" % (
                    total_instr // 1_000_000, countdown), flush=True)
            if tr.get("exit") is not None or tr.get("fault"):
                break
            begin = self.cur_lin()
            c = total_instr // CHUNK
            for sc in (0x3D, 0x41, 0x39, 0x1C):
                self.set_key(sc, False)
            if not self.opened(PAVNAME):
                force_level()
            if not self.opened(PAVNAME):
                if c <= 14:
                    self.set_key(0x3D, True); self.set_key(0x41, True)
                elif c >= 16 and (c // 2) % 2 == 0:
                    self.set_key(0x39, True)
            if countdown is None and self.opened(BUMNAME):
                countdown = SETTLE_TICKS
                print("[int8_oracle_check] level loaded (%s) at chunk %d — settling %d ticks" % (
                    BUMNAME, c, SETTLE_TICKS), flush=True)
            if countdown is not None:
                if countdown > SETTLE_TICKS - 10 and (c // 2) % 2 == 0:
                    self.set_key(0x39, True)
                countdown -= 1
                if countdown <= 0:
                    break
            self.fire_int(8)
            begin = self.cur_lin()

        if tr.get("exit") is not None or tr.get("fault"):
            print("[int8_oracle_check] ERROR: premature exit/fault during boot: "
                  "exit=%s fault=%s err=%s" % (tr.get("exit"), tr.get("fault"),
                                               tr.get("err")), flush=True)
            return False
        if not self.opened(BUMNAME):
            print("[int8_oracle_check] ERROR: level %s never loaded after %dM instr" % (
                BUMNAME, total_instr // 1_000_000), flush=True)
            return False
        print("[int8_oracle_check] boot complete. p1_pixel=(%d,%d)" % (
            self.rd_s16(OFF_P1_PIXEL_X), self.rd_s16(OFF_P1_PIXEL_Y)), flush=True)
        # snapshot a clean post-boot machine so every frame check starts identical
        self.boot_ram = bytes(uc.mem_read(0, RAM))
        self.boot_ctx = uc.context_save()
        return True

    def restore_boot_state(self) -> None:
        self.uc.mem_write(0, self.boot_ram)
        self.uc.context_restore(self.boot_ctx)
        self.tr["fault"] = None
        self.tr["exit"] = None

    # ---- invoke a real engine fn via a synthetic near-call frame ----
    def call_engine_fn(self, fn_off: int) -> None:
        uc = self.uc
        uc.mem_write(CODE_LIN + STOP_OFF, b"\xF4")   # HLT sentinel
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
            uc.emu_start(CODE_LIN + fn_off, 0, count=20_000_000)
        except UcError as e:
            self.tr["call_err"] = str(e)
        finally:
            uc.hook_del(h)


# ---------------------------------------------------------------------------
# The cross-check
# ---------------------------------------------------------------------------
BBOX_FIELDS = [("pvp_p1_x0", OFF_PVP_P1_X0), ("pvp_p1_x1", OFF_PVP_P1_X1),
               ("pvp_p1_y0", OFF_PVP_P1_Y0), ("pvp_p1_y1", OFF_PVP_P1_Y1)]


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: int8_oracle_check.py <trace.bin> [n_frames=3]", file=sys.stderr)
        return 2
    trace_path = sys.argv[1]
    n_check = int(sys.argv[2]) if len(sys.argv) > 2 else 3

    header, init, frames = parse_trace(trace_path)
    print("[int8_oracle_check] trace %s: dgroup_seg=%#x frame_count=%d (records=%d)" % (
        trace_path, header["dgroup_seg"], header["frame_count"], len(frames)), flush=True)
    print("[int8_oracle_check] INIT: current_level=%d game_mode=%#x p1_pixel=(%d,%d) "
          "bbox=(%d,%d,%d,%d)" % (
              init["current_level"], init["game_mode"], init["p1_pixel_x"],
              init["p1_pixel_y"], init["pvp_p1_x0"], init["pvp_p1_x1"],
              init["pvp_p1_y0"], init["pvp_p1_y1"]), flush=True)

    if init["current_level"] < 1:
        print("CALIBRATION MISMATCH field=current_level dosbox=%d oracle=>=1 "
              "(trace never reached a real level)" % init["current_level"])
        return 1

    # Frame 0 mirrors INIT; check up to n_check leading frames (each is a closed-form
    # pixel->bbox map, so every frame is an independent calibration witness).
    n_check = max(1, min(n_check, len(frames)))

    eng = Engine()
    if not eng.boot(level=1):
        print("[int8_oracle_check] BLOCKED: could not boot the engine oracle", flush=True)
        return 3

    mismatches = 0
    checked = 0
    for k in range(n_check):
        fr = frames[k]
        eng.restore_boot_state()
        # SEED the captured frame's pixel pos + frozen flag, then run the real fn.
        eng.wr16(OFF_P1_PIXEL_X, fr["p1_pixel_x"])
        eng.wr16(OFF_P1_PIXEL_Y, fr["p1_pixel_y"])
        eng.wr8(OFF_PHYSICS_FROZEN, fr["physics_frozen"])
        eng.call_engine_fn(FN_UPDATE_P1_BBOX)
        oracle_bbox = {name: eng.rd_s16(off) for name, off in BBOX_FIELDS}

        frame_ok = True
        for name, _off in BBOX_FIELDS:
            dval = fr[name]
            oval = oracle_bbox[name]
            if dval != oval:
                tag = "INIT/frame0" if k == 0 else "frame %d" % k
                print("CALIBRATION MISMATCH field=%s dosbox=%d oracle=%d (%s, "
                      "seeded px=%d py=%d frozen=%d)" % (
                          name, dval, oval, tag, fr["p1_pixel_x"], fr["p1_pixel_y"],
                          fr["physics_frozen"]))
                frame_ok = False
                mismatches += 1
        if frame_ok:
            print("[int8_oracle_check] frame %d OK: px=%d py=%d -> bbox=(%d,%d,%d,%d) "
                  "[dosbox==oracle]" % (
                      k, fr["p1_pixel_x"], fr["p1_pixel_y"],
                      oracle_bbox["pvp_p1_x0"], oracle_bbox["pvp_p1_x1"],
                      oracle_bbox["pvp_p1_y0"], oracle_bbox["pvp_p1_y1"]), flush=True)
        checked += 1
        if k == 0 and not frame_ok:
            # A frame-0 mismatch is a calibration failure — do not proceed.
            print("[int8_oracle_check] FAIL: frame-0 calibration mismatch — the DGROUP "
                  "calibration or a field offset is wrong. Fix the offset + re-capture.",
                  flush=True)
            return 1

    if mismatches == 0:
        print("PASS: oracle cross-check — DOSBox INIT + first %d frame(s) AGREE with the "
              "Unicorn oracle (update_p1_bbox: pixel->AABB calibration confirmed; "
              "dgroup_seg=%#x, p1_pixel @0x9290/0x9292, bbox @0x84c/0x84e/0x850/0x852)." % (
                  checked, header["dgroup_seg"]), flush=True)
        return 0
    print("[int8_oracle_check] FAIL: %d field mismatch(es) across %d frames." % (
        mismatches, checked), flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
