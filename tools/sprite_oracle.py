#!/usr/bin/env python3
"""sprite_oracle.py — GROUND-TRUTH capture of the engine's prepared-sprite bytes.

Boots the real BUMPY.EXE under the project's Unicorn DOS emulator (the boot+hook
scaffold is copied from tools/emu/dosemu.py — we deliberately do NOT refactor that
file), drives it to a level, and captures the *prepared sprite frames* the engine
produces in its decode scratch (DGROUP 0x56ee). For each frame drawn we record the
sprite object's ctrl byte, width, height and the expanded prepared bytes. We also
dump the global `palette_mode` and the 256-byte `pixel_bitrev_lut` as a file prefix.

This is the byte-exact validation target for the reconstructed C sprite renderer,
and it resolves several unknowns (the LUT contents, palette_mode, the prep length
formula). It is also the *arbiter* run: we observe whether the prepared bytes in the
scratch actually match the `prepare_sprite_frames@1cec` expansion semantics.

Output: local/build/render/sprite_oracle.bin
  prefix : [u8 palette_mode][256 bytes pixel_bitrev_lut]
  per frame record (little-endian, packed):
    [u16 idx][u8 ctrl][u16 w][u16 h][u16 prep_len][prep_len prepared bytes]

Run with the sandbox disabled (uv cache + unicorn):
  uv run python tools/sprite_oracle.py
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

# --- paths (anchored to repo root: tools/sprite_oracle.py -> ../..) -----------
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_DIR = os.path.join(ROOT, "local/originals/old-games/bumpy")
EXE = os.path.join(ROOT, "local/originals/unpacked/BUMPY_unpacked.exe")
OUT_DIR = os.path.join(ROOT, "local/build/render")
OUT_BIN = os.path.join(OUT_DIR, "sprite_oracle.bin")
PSP_SEG = 0x0100
RAM = 0x110000

# DGROUP-relative data (linear = DG_LIN + offset). DG_LIN matches dosemu.py.
DG_LIN = (0x103b + 0x110) * 16                 # 0x114b0
OFF_PALETTE_MODE = 0x541d                       # u8, {1,2} on the VGA path
OFF_BITREV_LUT = 0x66f0                         # 256 bytes (build_bit_reverse_lut)
OFF_P1 = 0x792e                                 # p1 sprite object
OFF_P2 = 0x795a                                 # p2 sprite object
# Sprite object struct layout (from prepare_sprite_frames / init_sprite_structs):
#   +0x0a frame index   +0x0b ctrl byte
#   +0x0c/0x0e prepared-frame far ptr (off/seg)
#   +0x10 width(words)  +0x12 height
OBJ_CTRL = 0x0b
OBJ_PREP_OFF = 0x0c
OBJ_PREP_SEG = 0x0e
OBJ_W = 0x10
OBJ_H = 0x12

# blit_sprite entry, Ghidra 1000:942a. Main code segment maps simply: dosemu loads
# the MZ image (Ghidra base seg 0x1000) at linear 0x1100, so runtime linear of a
# Ghidra 1000:off byte is 0x1100 + off (this is exactly dosemu.py's BLIT formula).
BLIT_LIN = 0x1100 + 0x942a


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
        if "." in key:
            b, e = key.rsplit(".", 1)
            key = b.strip() + "." + e.strip()
        path = self.bydir.get(key)
        if not path:
            return -1
        h = self.next; self.next += 1
        self.handles[h] = open(path, "rb")
        return h


def prep_len(ctrl: int, w_words: int, h: int) -> int:
    """Length in bytes of one expanded frame in the decode scratch.

    Per prepare_sprite_frames: a 12-byte header is copied, then for each of
    (w_words >> 2) * h source units, 8 prepared bytes are written. Both the
    0x20 and non-0x20 pixel-format paths emit the same 8-bytes-per-unit count.
    """
    units = (w_words >> 2) * h
    return 12 + 8 * units


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
    tr = dict(instr=0, ints=collections.Counter(), last_ip=0, mode=None, keys=list("\r "))
    free_top = [0x1C00]
    FREE_END = 0x9000

    def set_cf(set_it: bool) -> None:
        fl = uc.reg_read(UC_X86_REG_EFLAGS)
        uc.reg_write(UC_X86_REG_EFLAGS, (fl | 1) if set_it else (fl & ~1))

    def hook_intr(uc, intno, _) -> None:
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
                    uc.reg_write(UC_X86_REG_AX, 8); uc.reg_write(UC_X86_REG_BX, avail); set_cf(True)
                else:
                    uc.reg_write(UC_X86_REG_AX, free_top[0]); free_top[0] += bx; set_cf(False)
            elif ah == 0x49:
                set_cf(False)
            elif ah == 0x4A:
                set_cf(False)
            elif ah == 0x3D:
                name = b""; o = uc.reg_read(UC_X86_REG_DX); ds = uc.reg_read(UC_X86_REG_DS)
                while True:
                    c = uc.mem_read(ds * 16 + o, 1)[0]
                    if c == 0:
                        break
                    name += bytes([c]); o += 1
                h = files.open(name.decode("latin1"))
                tr.setdefault("fileops", []).append(("open", name.decode("latin1"), h))
                fl = uc.reg_read(UC_X86_REG_EFLAGS)
                if h < 0:
                    uc.reg_write(UC_X86_REG_AX, 2); uc.reg_write(UC_X86_REG_EFLAGS, fl | 1)
                else:
                    uc.reg_write(UC_X86_REG_AX, h); uc.reg_write(UC_X86_REG_EFLAGS, fl & ~1)
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
                    uc.reg_write(UC_X86_REG_DX, (p >> 16) & 0xFFFF); uc.reg_write(UC_X86_REG_AX, p & 0xFFFF)
        elif intno == 0x10:
            if ah == 0x00:
                tr["mode"] = al
        elif intno == 0x16:
            if ah in (0x00, 0x10):
                k = ord(tr["keys"].pop(0)) if tr["keys"] else 0x0D
                uc.reg_write(UC_X86_REG_AX, k)
            elif ah in (0x01, 0x11):
                fl = uc.reg_read(UC_X86_REG_EFLAGS)
                uc.reg_write(UC_X86_REG_EFLAGS, (fl & ~0x40) if tr["keys"] else (fl | 0x40))

    def hook_unmapped(uc, access, addr, size, value, _) -> bool:
        tr["fault"] = (addr, tr["last_ip"]); uc.emu_stop(); return False

    io = [0]
    cur_scan = [0]

    def hook_in(uc, port, size, _):
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

    # --- minimal VGA planar emulation (mode 0xD), copied from dosemu.py ---------
    seq = bytearray(256); gc = bytearray(256); seq_i = [0]; gc_i = [0]
    latch = [0, 0, 0, 0]
    plane = [bytearray(0x10000) for _ in range(4)]
    dac = [[0, 0, 0] for _ in range(256)]; dac_i = [0]; dac_sub = [0]
    dac_written = set()
    ATTR_DEFAULT = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17]
    attr = bytearray(32)
    for _i, _a in enumerate(ATTR_DEFAULT):
        attr[_i] = _a
    attr_i = [0]; attr_ff = [0]
    _ega = [(0, 0, 0), (0, 0, 42), (0, 42, 0), (0, 42, 42), (42, 0, 0), (42, 0, 42),
            (42, 21, 0), (42, 42, 42), (21, 21, 21), (21, 21, 63), (21, 63, 21),
            (21, 63, 63), (63, 21, 21), (63, 21, 63), (63, 63, 21), (63, 63, 63)]
    for _i, _c in enumerate(_ega):
        dac[ATTR_DEFAULT[_i]] = list(_c)

    def hook_out(uc, port, size, value, _) -> None:
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
                dac_written.add(dac_i[0] & 0xFF)
                dac_sub[0] = 0; dac_i[0] = (dac_i[0] + 1) & 0xFF

    def hook_vga_write(uc, access, addr, size, value, _) -> None:
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

    def hook_vga_read(uc, access, addr, size, value, _) -> None:
        off = (addr - 0xA0000) & 0xFFFF
        for p in range(4):
            latch[p] = plane[p][off]
        uc.mem_write(addr, bytes([plane[gc[4] & 3][off]]))

    # --- sprite capture: hook blit_sprite, snapshot the just-prepared object -----
    # frames keyed by (idx, ctrl, w, h) so each distinct prepared frame is captured
    # once. We read the populated sprite object out of DGROUP, resolve its prepared
    # far ptr, and copy prep_len bytes out of the decode scratch.
    frames: "collections.OrderedDict[Tuple[int, int, int, int], bytes]" = collections.OrderedDict()

    def capture_obj(obj_lin: int) -> None:
        d = bytes(uc.mem_read(obj_lin, 0x18))
        idx = d[0x0a]
        ctrl = d[OBJ_CTRL]
        poff = d[OBJ_PREP_OFF] | (d[OBJ_PREP_OFF + 1] << 8)
        pseg = d[OBJ_PREP_SEG] | (d[OBJ_PREP_SEG + 1] << 8)
        w = d[OBJ_W] | (d[OBJ_W + 1] << 8)
        h = d[OBJ_H] | (d[OBJ_H + 1] << 8)
        if (pseg == 0 and poff == 0) or w == 0 or h == 0 or w > 256 or h > 256:
            return
        n = prep_len(ctrl, w, h)
        if n <= 12 or n > 0x4000:
            return
        prep_lin = (pseg * 16 + poff) & 0xFFFFF
        # The prepared ptr points at scratch+0xc (past the 12-byte header); back up to
        # include the header so the record holds the full expanded frame.
        start = prep_lin - 12
        if start < 0 or start + n > 0xA0000:
            return
        data = bytes(uc.mem_read(start, n))
        key = (idx, ctrl, w, h)
        if key not in frames:
            frames[key] = data
            if os.environ.get("SPRITE_ORACLE_DEBUG"):
                scratch = DG_LIN + 0x56ee
                where = ("DGROUP-scratch" if scratch <= prep_lin < scratch + 0x4000
                         else ("DGROUP-other" if DG_LIN <= prep_lin < DG_LIN + 0x10000 else "heap"))
                print("   [dbg] ctrl=%#x w=%d h=%d prep_lin=%#x (%s) header_ctrl=%#x exp40=%d" % (
                    ctrl, w, h, prep_lin, where, data[2], int(bool(ctrl & 0x40))), flush=True)

    def hook_blit(uc, addr, size, _) -> None:
        if not tr.get("watch"):
            return
        # blit_sprite(off, seg) is a near call; args sit above the near return addr.
        ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
        try:
            a_off, a_seg = struct.unpack("<HH", uc.mem_read(ss * 16 + sp + 2, 4))
        except UcError:
            a_off, a_seg = OFF_P1, 0x203b
        obj_lin = (a_seg * 16 + a_off) & 0xFFFFF
        # only trust the two known sprite objects (p1 / p2) for layout certainty
        if a_off in (OFF_P1, OFF_P2):
            capture_obj(obj_lin)
        else:
            capture_obj(obj_lin)

    uc.hook_add(UC_HOOK_CODE, hook_blit, None, BLIT_LIN, BLIT_LIN)

    # --- CONFIRMATION PROBE: is decode_2bpp_planes (1cec:00aa) the load-time bank
    # transform? Overlay seg S -> runtime linear 0x1100 + (S-0x1000)*16 + off
    # (dosemu's overlay formula). Count calls + log first few param ptrs. -----------
    DEC2BPP_LIN = 0x1100 + (0x1cec - 0x1000) * 16 + 0x00aa   # = 0xe06a
    dec2 = dict(calls=0, samples=[])

    def hook_dec2bpp(uc, addr, size, _) -> None:
        dec2["calls"] += 1
        if len(dec2["samples"]) < 8:
            ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
            # near __cdecl: param_1 (far, 4B) then param_2 (far/near) above ret addr
            args = bytes(uc.mem_read(ss * 16 + sp + 2, 8))
            dec2["samples"].append((tr.get("instr_total", 0), args.hex()))

    uc.hook_add(UC_HOOK_CODE, hook_dec2bpp, None, DEC2BPP_LIN, DEC2BPP_LIN)

    # Overlay-mapping check: does blit_sprite_vga (1cec:31b7) fire at the dosemu
    # formula address? It MUST run if sprites are drawn. If 0, the overlay-seg->linear
    # mapping for 1cec is wrong (overlay swapping) and DEC2BPP_LIN is meaningless.
    BLITVGA_LIN = 0x1100 + (0x1cec - 0x1000) * 16 + 0x31b7
    bvga = dict(calls=0)

    def hook_bvga(uc, addr, size, _) -> None:
        bvga["calls"] += 1
        # On the first blit, the 0x2000 overlay (the real blit code) is resident.
        # CALL 0x2000:fcad / fc2d -> runtime linear 0x1100+(0x2000-0x1000)*16+off.
        if bvga["calls"] == 1:
            base2000 = 0x1100 + (0x2000 - 0x1000) * 16   # 0x21100? -> actually 0x11100
            bvga["base2000"] = base2000
            bvga["dump"] = bytes(uc.mem_read(base2000, 0x10000))  # whole 0x2000 segment
            bvga["fc2d"] = bytes(uc.mem_read(base2000 + 0xfc2d, 48))
            bvga["fcad"] = bytes(uc.mem_read(base2000 + 0xfcad, 48))

    uc.hook_add(UC_HOOK_CODE, hook_bvga, None, BLITVGA_LIN, BLITVGA_LIN)

    # --- BLIT ORACLE (5b): snapshot the planar blitter sprite_blit_planar_vga
    # (1cec:10e1) at each call. SI = the 0x18-byte blit descriptor built by
    # setup/clip (DGROUP 0x26bd5): [0]=src far ptr, [8]=dest(VGA) far ptr,
    # [0x10]=width, [0x12]=rows, [0x15]=sel, [0x16]=shift, [0x17]=clip-flags.
    # We record (descriptor, source bytes, all 4 VGA planes) at ENTRY of each
    # call; consecutive plane snapshots give before/after for one sprite (only
    # setup/clip run between back-to-back calls -> no intervening VGA writes).
    PLANAR_LIN = 0x1100 + (0x1cec - 0x1000) * 16 + 0x10e1   # = 0xf0a1 (entry)
    PLANAR_EXIT_LIN = PLANAR_LIN + (0x2139 - 0x10e1)        # = 0x100f9 (single exit)
    SETUP_LIN = 0x1100 + (0x1cec - 0x1000) * 16 + 0x103d    # sprite_blit_setup entry
    # DGROUP window captured at setup entry (holds the chain's input globals + the
    # descriptor-in-progress).  Runtime DGROUP offset = Ghidra(203b)-offset + 0x139.
    DGWIN_OFF = 0x5000
    DGWIN_LEN = 0x2000
    BLIT_ORACLE = bool(os.environ.get("BLIT_ORACLE"))
    MAX_BLIT_CAPS = int(os.environ.get("BLIT_ORACLE_CAPS", "24"))
    SRC_CHUNK = 0x4000
    blit_caps: list = []
    pending: dict = {}
    setup_ctx: dict = {}

    def snap_planes() -> bytes:
        return b"".join(bytes(plane[p]) for p in range(4))

    def hook_setup(uc, addr, size, _) -> None:
        # sprite_blit_setup(103d) ENTRY: DI = the sprite object ptr; capture the
        # object struct + the DGROUP window (view bounds, cur_sprite_data, etc.).
        if not (BLIT_ORACLE and tr.get("watch")) or len(blit_caps) >= MAX_BLIT_CAPS:
            return
        ds = uc.reg_read(UC_X86_REG_DS) & 0xFFFF
        di = uc.reg_read(UC_X86_REG_DI) & 0xFFFF
        try:
            obj = bytes(uc.mem_read((ds * 16 + di) & 0xFFFFF, 0x20))
            dgwin = bytes(uc.mem_read((DG_LIN + DGWIN_OFF) & 0xFFFFF, DGWIN_LEN))
        except UcError:
            return
        setup_ctx.clear()
        setup_ctx.update(setup_ds=ds, setup_di=di, obj=obj, dgwin=dgwin)

    def hook_planar(uc, addr, size, _) -> None:
        # ENTRY: record descriptor + source + the BEFORE planes for this one blit.
        if not (BLIT_ORACLE and tr.get("watch")) or len(blit_caps) >= MAX_BLIT_CAPS:
            return
        ds = uc.reg_read(UC_X86_REG_DS) & 0xFFFF
        si = uc.reg_read(UC_X86_REG_SI) & 0xFFFF
        desc = bytes(uc.mem_read((ds * 16 + si) & 0xFFFFF, 0x20))
        src_off, src_seg = struct.unpack_from("<HH", desc, 0)
        src_lin = (src_seg * 16 + src_off) & 0xFFFFF
        try:
            src = bytes(uc.mem_read(src_lin, SRC_CHUNK))
        except UcError:
            src = b""
        pending.clear()
        pending.update(ds=ds, si=si, desc=desc, src_lin=src_lin, src=src,
                       before=snap_planes(), **setup_ctx)

    def hook_planar_exit(uc, addr, size, _) -> None:
        # EXIT (0x2139): pair the AFTER planes with the pending entry capture.
        if not (BLIT_ORACLE and pending) or len(blit_caps) >= MAX_BLIT_CAPS:
            return
        cap = dict(pending); cap["after"] = snap_planes()
        blit_caps.append(cap)
        pending.clear()

    uc.hook_add(UC_HOOK_CODE, hook_setup, None, SETUP_LIN, SETUP_LIN)
    uc.hook_add(UC_HOOK_CODE, hook_planar, None, PLANAR_LIN, PLANAR_LIN)
    uc.hook_add(UC_HOOK_CODE, hook_planar_exit, None, PLANAR_EXIT_LIN, PLANAR_EXIT_LIN)

    # --- BG ORACLE (6a): the background TILE build. restore_bg_tile_run
    # (1000:0a90) is called per playfield cell during start_level; it reads the
    # tile id from the level map (_cur_level_ptr, far ptr @ DGROUP 0x6bca) and
    # blits 16xH tiles from the PAV atlas (far ptr @ DGROUP 0x6fa6/0x6fa8). We
    # capture its args (run_code, cell_x, cell_y, frame) + the VGA planes at each
    # call; consecutive snapshots give before/after for one cell (the build is
    # back-to-back blits with no other VGA writes between).
    RBTR_LIN = 0x1100 + 0x0a90                          # restore_bg_tile_run entry
    ATLAS_PTR_OFF = 0x6fa6                              # level_pav_buf off / +2 seg
    MAP_PTR_OFF = 0x6bca                                # _cur_level_ptr (far)
    BG_ORACLE = bool(os.environ.get("BG_ORACLE"))
    MAX_BG_CAPS = int(os.environ.get("BG_ORACLE_CAPS", "48"))
    # capture cells with cell_y < BG_LO_CY OR cell_y >= BG_MIN_CY (covers the top rows
    # plus the bottom rows incl. the cy=24 screen-clip edge in one compact oracle).
    BG_MIN_CY = int(os.environ.get("BG_ORACLE_MIN_CY", "0"))
    BG_LO_CY = int(os.environ.get("BG_ORACLE_LO_CY", "999"))
    bg_caps: list = []
    bg_static: dict = {}

    def hook_restore_bg(uc, addr, size, _) -> None:
        # ENTRY: args + planes. Consecutive entries give a clean per-cell delta:
        # during the build the cells draw back-to-back with no other VGA writes,
        # so planes(cell N+1) - planes(cell N) is exactly cell N's tile (the
        # entry/exit window is NOT clean -- the build redraws over leftover menu
        # content, so most of a tile's bytes are unchanged within a single call).
        if not (BG_ORACLE and tr.get("watch")) or len(bg_caps) >= MAX_BG_CAPS:
            return
        try:
            ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
            # near __cdecl: [SP]=ret, [SP+2]=run_code, +4=cell_x, +6=cell_y, +8=frame
            run_code, cell_x, cell_y, frame = struct.unpack(
                "<HHHH", uc.mem_read(ss * 16 + sp + 2, 8))
            if not (cell_y < BG_LO_CY or cell_y >= BG_MIN_CY):
                return
            if not bg_static:
                a_off, a_seg = struct.unpack("<HH", uc.mem_read(DG_LIN + ATLAS_PTR_OFF, 4))
                a_base = (a_seg * 16 + a_off) & 0xFFFFF       # buffer base (raster at +6)
                m_off, m_seg = struct.unpack("<HH", uc.mem_read(DG_LIN + MAP_PTR_OFF, 4))
                m_base = (m_seg * 16 + m_off) & 0xFFFFF
                bg_static["atlas_lin"] = a_base
                bg_static["atlas"] = bytes(uc.mem_read(a_base, 0x8000))
                bg_static["map_lin"] = m_base
                bg_static["map"] = bytes(uc.mem_read(m_base, 0x1000))
        except UcError:
            return
        bg_caps.append(dict(run_code=run_code, cell_x=cell_x, cell_y=cell_y,
                            frame=frame, planes=snap_planes()))

    uc.hook_add(UC_HOOK_CODE, hook_restore_bg, None, RBTR_LIN, RBTR_LIN)

    uc.hook_add(UC_HOOK_INTR, hook_intr)
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

    dg = (0x103b + base) & 0xFFFF

    uc.mem_write(0x500, b"\xCF")
    iret_vec = (0x0050 << 16) | 0x0000
    for v in range(0x100):
        if struct.unpack("<I", uc.mem_read(v * 4, 4))[0] == 0:
            uc.mem_write(v * 4, struct.pack("<I", iret_vec))

    def set_key(scancode: int, down: bool) -> None:
        mbase = struct.unpack("<H", uc.mem_read(dg * 16 + 0x4D42, 2))[0]
        uc.mem_write(dg * 16 + mbase + (scancode & 0x7F), bytes([scancode if down else 0]))

    def opened(name: str) -> bool:
        return any(o[0] == "open" and o[1] == name for o in tr.get("fileops", []))

    LEVEL = int(os.environ.get("DOSEMU_LEVEL", "1"))
    PAVNAME = "D%d.PAV" % LEVEL
    BUMNAME = "D%d.BUM" % LEVEL

    def force_level() -> None:
        uc.mem_write(dg * 16 + 0x79B2, bytes([LEVEL & 0xFF]))
        uc.mem_write(dg * 16 + 0x119A, bytes([1]))

    # --- FRAME ORACLE (6b): snapshot the full level frame (all 4 VGA planes + the
    # DAC palette) once the level has settled, as the composite ground truth.
    FRAME_ORACLE = bool(os.environ.get("FRAME_ORACLE"))
    FRAME_SETTLE = int(os.environ.get("FRAME_SETTLE", "40"))
    frame_snap: dict = {}

    begin = cur_lin(); err = None; CHUNK = 1_000_000; total = 0
    countdown = None
    print("[sprite_oracle] booting BUMPY (level %d)..." % LEVEL, flush=True)
    while total < 400_000_000:
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e)
            tr["err"] = err
            break
        total += CHUNK
        if total % 20_000_000 == 0:
            print("[sprite_oracle] %d Minstr, frames=%d, watch=%s" % (
                total // 1_000_000, len(frames), bool(tr.get("watch"))), flush=True)
        if tr.get("exit") is not None or tr.get("fault"):
            break
        begin = cur_lin()
        c = total // CHUNK
        for sc in (0x3D, 0x41, 0x39, 0x1C):
            set_key(sc, False)
        if not opened(PAVNAME):
            force_level()
        tr["watch"] = opened(PAVNAME)
        if BLIT_ORACLE and len(blit_caps) >= MAX_BLIT_CAPS:
            print("[sprite_oracle] blit oracle: captured %d planar-blit snapshots" % len(blit_caps), flush=True)
            break
        if BG_ORACLE and len(bg_caps) >= MAX_BG_CAPS:
            print("[sprite_oracle] bg oracle: captured %d tile-blit snapshots" % len(bg_caps), flush=True)
            break
        if not opened(PAVNAME):
            if c <= 14:
                set_key(0x3D, True); set_key(0x41, True)
            elif c >= 16 and (c // 2) % 2 == 0:
                set_key(0x39, True)
        if countdown is None and opened(BUMNAME):
            countdown = int(os.environ.get("DOSEMU_SETTLE", "120"))
            print("[sprite_oracle] level loaded (%s) at chunk %d — capturing frames" % (BUMNAME, c), flush=True)
        if countdown is not None and countdown > 96 and (c // 2) % 2 == 0:
            set_key(0x39, True)
        if FRAME_ORACLE and countdown is not None and countdown <= FRAME_SETTLE and not frame_snap:
            frame_snap["planes"] = snap_planes()
            frame_snap["dac"] = b"".join(bytes(dac[i]) for i in range(256))
            # also capture the PAV atlas + the level map so the bg can be rebuilt and
            # diffed against this frame (atlas far ptr @ 0x6fa6/0x6fa8, map @ 0x6bca).
            try:
                a_off, a_seg = struct.unpack("<HH", uc.mem_read(DG_LIN + 0x6fa6, 4))
                m_off, m_seg = struct.unpack("<HH", uc.mem_read(DG_LIN + 0x6bca, 4))
                frame_snap["atlas"] = bytes(uc.mem_read((a_seg * 16 + a_off) & 0xFFFFF, 0x8000))
                frame_snap["map"] = bytes(uc.mem_read((m_seg * 16 + m_off) & 0xFFFFF, 0x1000))
            except UcError:
                frame_snap["atlas"] = b""; frame_snap["map"] = b""
            # --- 6b: capture entity runtime state at this exact settled frame -----
            # Level index (1-based)
            frame_snap["level"] = LEVEL
            # Entity-state reads: BUM far-ptr deref + sprite objs + globals +
            # anim channel records + full DGROUP snapshot.  Any of these can
            # fault with UcError if the BUM far ptr resolves outside mapped
            # memory (e.g. on levels other than level 1 during development).
            # Guard the whole block and fall back to zero-padded sentinels so
            # the FRM3 writer can proceed and the reader won't misparse layout.
            try:
                # BUM per-level header block (lives outside DGROUP).
                # tilemap far ptr @ DGROUP:0xa0d8 → already points at the
                # current level's 0xc2-byte block base (re-pointed at level
                # load: see diag_clean.c switchD_e093_caseD_bb which copies
                # level_src_ptr data into the tilemap buffer byte-by-byte).
                # Read 0xc2 bytes directly from that base — no +2 skip, no
                # (LEVEL-1)*0xc2 term.
                bum_off, bum_seg = struct.unpack("<HH", uc.mem_read(DG_LIN + 0xa0d8, 4))
                bum_block_lin = (bum_seg * 16 + bum_off) & 0xFFFFF
                hdr_lin = bum_block_lin
                frame_snap["bum"] = bytes(uc.mem_read(hdr_lin, 0xC2))
                # Sprite object structs (each 0x18 bytes, fixed DGROUP addresses).
                # P1 obj @ 0x792e, P2 obj @ 0x795a.
                frame_snap["p1_obj"] = bytes(uc.mem_read(DG_LIN + 0x792e, 0x18))
                frame_snap["p2_obj"] = bytes(uc.mem_read(DG_LIN + 0x795a, 0x18))
                # Player globals — NOT contiguous; read each field separately.
                # p1: pixel_x@0x9290(2B)  pixel_y@0x9292(2B)  move_anim@0x824a(2B)
                frame_snap["p1_glob"] = (bytes(uc.mem_read(DG_LIN + 0x9290, 2))
                                         + bytes(uc.mem_read(DG_LIN + 0x9292, 2))
                                         + bytes(uc.mem_read(DG_LIN + 0x824a, 2)))
                # p2: pixel_x@0x79ba(2B)  pixel_y@0x79bc(2B)  move_anim@0x8560(2B)
                frame_snap["p2_glob"] = (bytes(uc.mem_read(DG_LIN + 0x79ba, 2))
                                         + bytes(uc.mem_read(DG_LIN + 0x79bc, 2))
                                         + bytes(uc.mem_read(DG_LIN + 0x8560, 2)))
                # Anim channel records: read by fixed DGROUP offsets (stride 0xc).
                # Layer-A: 3 records at 0x4c40 / 0x4c4c / 0x4c58
                # Layer-B: 4 records at 0x4c80 / 0x4c8c / 0x4c98 / 0x4ca4
                raw_a = bytes(uc.mem_read(DG_LIN + 0x4c40, 3 * 0xc))
                raw_b = bytes(uc.mem_read(DG_LIN + 0x4c80, 4 * 0xc))
                frame_snap["chan_a_raw"] = raw_a
                frame_snap["chan_b_raw"] = raw_b
                # Also capture the raw far-ptr table words so Tasks 4-6 can verify
                # that the table pointers resolve into the captured records.
                # A: off@0x4c70  seg@0x4c72  B: off@0x4cbc  seg@0x4cbe  (4 u16 = 8 bytes)
                frame_snap["chan_tbl_raw"] = (bytes(uc.mem_read(DG_LIN + 0x4c70, 2))
                                              + bytes(uc.mem_read(DG_LIN + 0x4c72, 2))
                                              + bytes(uc.mem_read(DG_LIN + 0x4cbc, 2))
                                              + bytes(uc.mem_read(DG_LIN + 0x4cbe, 2)))
                # Full 64KB DGROUP snapshot — supersets all per-field captures above.
                # Tasks 4-6 can read any static table by DGROUP offset without re-running.
                frame_snap["dg"] = bytes(uc.mem_read(DG_LIN, 0x10000))
            except UcError as _ent_err:
                print("[sprite_oracle] WARNING: entity capture UcError (%s) — using zero sentinels" % _ent_err, flush=True)
                frame_snap.setdefault("bum", bytes(0xC2))
                frame_snap.setdefault("p1_obj", bytes(0x18))
                frame_snap.setdefault("p2_obj", bytes(0x18))
                frame_snap.setdefault("p1_glob", bytes(6))
                frame_snap.setdefault("p2_glob", bytes(6))
                frame_snap.setdefault("chan_a_raw", bytes(3 * 0xc))
                frame_snap.setdefault("chan_b_raw", bytes(4 * 0xc))
                frame_snap.setdefault("chan_tbl_raw", bytes(8))
                frame_snap.setdefault("dg", bytes(0x10000))
            print("[sprite_oracle] frame oracle: captured settled level frame (countdown=%d)" % countdown, flush=True)
            break
        if countdown is not None:
            countdown -= 1
            # stop early once we have enough distinct frames AND the level has settled
            # (FRAME_ORACLE wants the fully-settled frame, so it ignores this early-out)
            if not FRAME_ORACLE and ((len(frames) >= 6 and countdown <= 96) or countdown <= 0):
                break
            if countdown <= 0:
                break
        fire_int(8)
        begin = cur_lin()

    os.makedirs(OUT_DIR, exist_ok=True)
    pmode = uc.mem_read(DG_LIN + OFF_PALETTE_MODE, 1)[0]
    lut = bytes(uc.mem_read(DG_LIN + OFF_BITREV_LUT, 256))

    # Dump the in-memory (transformed) BUMSPJEU bank for byte-exact 5a validation.
    # Bank far ptr = sprite-sheet ptr the objects use (DGROUP 0xa0c6 off / 0xa0c8 seg).
    BANK_SIZE = 89116                                   # == on-disk BUMSPJEU.BIN size
    bank_off = struct.unpack("<H", uc.mem_read(DG_LIN + 0xa0c6, 2))[0]
    bank_seg = struct.unpack("<H", uc.mem_read(DG_LIN + 0xa0c8, 2))[0]
    bank_lin = (bank_seg * 16 + bank_off) & 0xFFFFF
    bank_inmem = bytes(uc.mem_read(bank_lin, BANK_SIZE))
    with open(os.path.join(OUT_DIR, "bank_inmem.bin"), "wb") as bf:
        bf.write(bank_inmem)
    print("[sprite_oracle] in-mem bank @%#x (off=%#x seg=%#x) %d B -> bank_inmem.bin"
          % (bank_lin, bank_off, bank_seg, len(bank_inmem)), flush=True)
    print("[sprite_oracle]   table[0..16]=%s data[0x800..0x810]=%s"
          % (bank_inmem[:16].hex(), bank_inmem[0x800:0x810].hex()), flush=True)

    print("[sprite_oracle] ran %d Minstr; mode=%s; palette_mode=%d; PAV=%s BUM=%s" % (
        total // 1_000_000, hex(tr["mode"]) if tr["mode"] is not None else None,
        pmode, opened(PAVNAME), opened(BUMNAME)), flush=True)
    if err:
        print("[sprite_oracle] emu error:", err, flush=True)
    # Dump the 203b overlay thunks (203b -> DGROUP base 0x114b0). If the overlay
    # manager patched them with far jumps (0xEA off seg), follow to the loaded code.
    for nm, foff in (("f87d", 0xf87d), ("f8fd", 0xf8fd)):
        b = bytes(uc.mem_read(DG_LIN + foff, 16))
        line = "[sprite_oracle] thunk 203b:%s @%#x = %s" % (nm, DG_LIN + foff, b.hex())
        if b[0] == 0xEA:
            toff, tseg = struct.unpack_from("<HH", b, 1)
            line += "  -> FAR JMP %04x:%04x (lin %#x)" % (tseg, toff, (tseg * 16 + toff) & 0xFFFFF)
        print(line, flush=True)
    print("[sprite_oracle] decode_2bpp_planes(1cec:00aa @%#x) calls=%d" % (
        DEC2BPP_LIN, dec2["calls"]), flush=True)
    for it, args in dec2["samples"]:
        print("   [dec2bpp] args=%s" % args, flush=True)
    print("[sprite_oracle] blit_sprite_vga(1cec:31b7 @%#x) calls=%d" % (BLITVGA_LIN, bvga["calls"]), flush=True)
    if bvga.get("dump"):
        ov_path = os.path.join(OUT_DIR, "ov2000.bin")
        with open(ov_path, "wb") as f:
            f.write(bvga["dump"])
        nz = sum(1 for b in bvga["dump"] if b)
        print("[sprite_oracle] ov2000 base=%#x nonzero=%d -> %s" % (
            bvga["base2000"], nz, ov_path), flush=True)
        print("[sprite_oracle]   @fc2d: %s" % bvga["fc2d"].hex(), flush=True)
        print("[sprite_oracle]   @fcad: %s" % bvga["fcad"].hex(), flush=True)
    print("[sprite_oracle] LUT first16:", lut[:16].hex(), flush=True)
    print("[sprite_oracle] LUT nonzero=%d distinct=%d" % (
        sum(1 for b in lut if b), len(set(lut))), flush=True)

    # --- write the oracle file -------------------------------------------------
    with open(OUT_BIN, "wb") as f:
        f.write(struct.pack("<B", pmode & 0xFF))
        f.write(lut)
        for (idx, ctrl, w, h), data in frames.items():
            f.write(struct.pack("<HBHHH", idx, ctrl, w, h, len(data)))
            f.write(data)
    print("[sprite_oracle] wrote %s : %d frames" % (OUT_BIN, len(frames)), flush=True)
    for (idx, ctrl, w, h), data in frames.items():
        print("   frame idx=%d ctrl=%#04x w=%d h=%d prep_len=%d  head=%s" % (
            idx, ctrl, w, h, len(data), data[:16].hex()), flush=True)

    # --- FRAME ORACLE output (6b): the settled full level frame ------------------
    #
    # FRM3 byte layout (all little-endian; offsets are from start of file):
    #   +0x00  4 B  magic "FRM3"
    #   +0x04  4 B  u32 planes_len (= 4 * 0x10000 = 0x40000)
    #   +0x08  planes_len B  4 VGA planes (plane 0..3, 0x10000 B each)
    #   +0x48008  0x300 B  DAC palette (256 * 3 bytes, 0..63 per channel)
    #   next   4 B  u32 atlas_len (= 0x8000)
    #   next   atlas_len B  PAV atlas raster
    #   next   4 B  u32 map_len (= 0x1000)
    #   next   map_len B  level tile map
    #   --- FRM3 new blocks (appended after atlas/map) ---
    #   next   2 B  u16 level  (1-based level index)
    #   next   0xc2 B  BUM per-level header for this level
    #                  (from tilemap@ DGROUP:0xa0d8 → block base, no offset;
    #                   tilemap ptr is re-pointed per level at load time)
    #   next   0x18 B  p1_sprite obj struct  (DGROUP:0x792e)
    #   next   0x18 B  p2_sprite obj struct  (DGROUP:0x795a)
    #   next   6 B  p1_glob: pixel_x(u16) pixel_y(u16) move_anim(u16)
    #                         (@0x9290  @0x9292  @0x824a)
    #   next   6 B  p2_glob: pixel_x(u16) pixel_y(u16) move_anim(u16)
    #                         (@0x79ba  @0x79bc  @0x8560)
    #   next   3*0xc B  Layer-A channel records  (DGROUP:0x4c40, stride 0xc)
    #   next   4*0xc B  Layer-B channel records  (DGROUP:0x4c80, stride 0xc)
    #   next   8 B  chan_tbl_raw: 4 u16 far-ptr table words
    #                (A_off@0x4c70, A_seg@0x4c72, B_off@0x4cbc, B_seg@0x4cbe)
    #   next   0x10000 B  full DGROUP snapshot (DG_LIN, 64KB)
    #                     supersets all per-field captures; Tasks 4-6 index by offset
    if FRAME_ORACLE and frame_snap:
        fr_path = os.path.join(OUT_DIR, "frame_oracle.bin")
        atlas = frame_snap.get("atlas", b"")
        bmap = frame_snap.get("map", b"")
        with open(fr_path, "wb") as f:
            # --- existing FRM2-compatible blocks (offsets preserved) ---------------
            f.write(b"FRM3")
            f.write(struct.pack("<I", len(frame_snap["planes"])))
            f.write(frame_snap["planes"])                            # 4 * 0x10000
            f.write(frame_snap["dac"])                               # 256 * 3 (0..63)
            f.write(struct.pack("<I", len(atlas)))
            f.write(atlas)                                           # PAV atlas (0x8000)
            f.write(struct.pack("<I", len(bmap)))
            f.write(bmap)                                            # level map (0x1000)
            # --- FRM3 new blocks ---------------------------------------------------
            f.write(struct.pack("<H", frame_snap["level"]))          # u16 level
            f.write(frame_snap["bum"])                               # 0xc2 B BUM header
            f.write(frame_snap["p1_obj"])                            # 0x18 B p1 obj
            f.write(frame_snap["p2_obj"])                            # 0x18 B p2 obj
            f.write(frame_snap["p1_glob"])                           # 6 B p1 globals
            f.write(frame_snap["p2_glob"])                           # 6 B p2 globals
            f.write(frame_snap["chan_a_raw"])                        # 3*0xc B layer-A ch
            f.write(frame_snap["chan_b_raw"])                        # 4*0xc B layer-B ch
            f.write(frame_snap["chan_tbl_raw"])                      # 8 B far-ptr words
            f.write(frame_snap["dg"])                                # 0x10000 B DGROUP
        print("[sprite_oracle] wrote %s : full frame + palette + atlas/map + entity state" % fr_path, flush=True)

    # --- BG ORACLE output (6a): tile-blit snapshots + the PAV atlas --------------
    if BG_ORACLE:
        bg_path = os.path.join(OUT_DIR, "bg_oracle.bin")
        atlas = bg_static.get("atlas", b"")
        bmap = bg_static.get("map", b"")
        with open(bg_path, "wb") as f:
            f.write(b"BG02")
            f.write(struct.pack("<H", len(bg_caps)))
            f.write(struct.pack("<II", bg_static.get("atlas_lin", 0), len(atlas)))
            f.write(atlas)
            f.write(struct.pack("<II", bg_static.get("map_lin", 0), len(bmap)))
            f.write(bmap)
            for cap in bg_caps:
                f.write(struct.pack("<HHHH", cap["run_code"], cap["cell_x"], cap["cell_y"], cap["frame"]))
                f.write(struct.pack("<I", len(cap["planes"])))
                f.write(cap["planes"])                               # 4 * 0x10000
        print("[sprite_oracle] wrote %s : %d tile-build snapshots, atlas %d B @%#x, map %d B @%#x" % (
            bg_path, len(bg_caps), len(atlas), bg_static.get("atlas_lin", 0),
            len(bmap), bg_static.get("map_lin", 0)), flush=True)
        for i, cap in enumerate(bg_caps[:16]):
            print("   bg[%d] run_code=%#x cell_x=%d cell_y=%d frame=%d" % (
                i, cap["run_code"], cap["cell_x"], cap["cell_y"], cap["frame"]), flush=True)

    # --- BLIT ORACLE output (5b): planar-blit snapshots --------------------------
    if BLIT_ORACLE:
        blt_path = os.path.join(OUT_DIR, "blit_oracle.bin")
        empty_obj = b"\x00" * 0x20
        empty_dg = b"\x00" * DGWIN_LEN
        with open(blt_path, "wb") as f:
            f.write(b"BLT3")
            f.write(struct.pack("<H", len(blit_caps)))
            f.write(struct.pack("<HH", DGWIN_OFF, DGWIN_LEN))        # chain-window meta
            for cap in blit_caps:
                f.write(struct.pack("<HH", cap["ds"], cap["si"]))
                f.write(cap["desc"])                                  # 0x20 bytes
                # chain inputs (object + DGROUP window) — zero-filled if setup missed
                f.write(struct.pack("<HH", cap.get("setup_ds", 0), cap.get("setup_di", 0)))
                f.write(cap.get("obj", empty_obj))                    # 0x20 bytes
                f.write(cap.get("dgwin", empty_dg))                   # DGWIN_LEN
                f.write(struct.pack("<II", cap["src_lin"], len(cap["src"])))
                f.write(cap["src"])
                f.write(struct.pack("<I", len(cap["before"])))       # 4 * 0x10000
                f.write(cap["before"])
                f.write(cap["after"])                                # same length
        print("[sprite_oracle] wrote %s : %d planar-blit snapshots" % (blt_path, len(blit_caps)), flush=True)
        for i, cap in enumerate(blit_caps):
            d = cap["desc"]
            srcp = struct.unpack_from("<HH", d, 0)
            dstp = struct.unpack_from("<HH", d, 8)
            w16, rows = struct.unpack_from("<HH", d, 0x10)
            print("   blit[%d] src=%04x:%04x dst=%04x:%04x w=%d rows=%d sel=%#x shift=%d clip=%#x desc=%s" % (
                i, srcp[1], srcp[0], dstp[1], dstp[0], w16, rows, d[0x15], d[0x16], d[0x17],
                d.hex()), flush=True)


if __name__ == "__main__":
    main()
