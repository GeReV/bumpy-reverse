#!/usr/bin/env python3
"""screens_oracle.py — Phase-7 front-end (menus/title/highscore/intro + HUD)
CAPTURE-AS-DISCOVERY harness (+ DAC OUT-capture, seeded decoded image buffer).

Boots the real (unpacked) BUMPY.EXE under Unicorn — reusing the boot + DGROUP-read +
OUT-capture + call_near() scaffold of tools/sound_oracle.py / tools/anim_oracle.py
(deliberately NOT refactoring those) — then drives the ~20 screen/HUD functions
(Ghidra seg 1000) at the FUNCTION-CALL boundary (entry + exit), capturing:

  * an entry+exit DGROUP snapshot of the screen globals (current_level, palette_mode,
    menu_option2_setting, input_state, score_lo/hi, timing_flag_accumulator);
  * the `render_descriptor_ptr` view-struct bytes (far ptr @ DGROUP 0x0574 -> a 0x22-byte
    struct; the screen builders write fields +2..+0x20) at ENTRY and EXIT;
  * the `p1_sprite` blit descriptor (far ptr @ DGROUP 0x8884 -> the 0x792e descriptor;
    `*p1_sprite=0x30`, `p1_sprite[1]=cursor_index*0x10+0x70`) at EXIT — this is how the
    menu cursor LOCAL `cursor_index` is OBSERVED, since it lives in a stack local;
  * the menu/name-entry RETURN value (run_main_menu returns selected_item, a local);
  * the seeded decoded image buffer `fullscreen_buf` (first 0x40 header bytes incl. the
    embedded palette @ +0x33) — the engine's own post-vec_decode buffer, captured so the
    descriptor capture is deterministic without re-running file I/O;
  * for `upload_vga_dac_palette` (1000:9864 -> far overlay 2036:0000 ->
    DGROUP[palette_mode*2+0x6976] handler): the captured (port,value) DAC OUT sequence
    on ports 0x3c8 (index) / 0x3c9 (RGB).

WHY SEEDING IS (mostly) FREE HERE.  The screen fns load their image via
open_resource(idx)/read_chunked (INT 21h file I/O) then vec_decode into fullscreen_buf.
Unlike a pure synthetic call, the engine's resource table maps idx -> a real game file
(TITRE.VEC res 0x11, BUMPRESE.VEC res 1, SCORE.VEC res 3, MONDE<n>.VEC res level+7),
ALL present in GAME_DIR, and tools/sound_oracle.py's `Files` INT-21h handler already
serves them.  So we let the engine's NATURAL load+decode run inside the captured call
window and SNAPSHOT the fullscreen_buf the engine itself produced (the brief's preferred
seeding).  For fns reached only via a deeper menu branch we additionally seed input via
the keyboard FUN_1000_75a2 path and via call_near() (the sound/anim direct-invoke
pattern).  Documented per scenario in local/build/screens_model.md.

KEY ADDRESS NUANCE — the menu cursor state is a LOCAL, not DGROUP.  In run_main_menu
(1000:35a5) `cursor_index`/`selected_item` are stack locals.  They are OBSERVABLE via
(a) the p1_sprite blit descriptor (0x792e): `*p1_sprite=0x30`, `p1_sprite[1]=
cursor_index*0x10+0x70`; (b) the RETURN value (selected_item).  `menu_option2_setting`
@ DGROUP 0x79b5 IS a global.  So per menu iteration we capture the render_descriptor_ptr
+ p1_sprite descriptors + the return value + menu_option2_setting.

Outputs (BOTH gitignored — discovery; only this script is committed):
  local/build/render/screens_trace.bin   (frozen layout — see TRACE LAYOUT below)
  local/build/screens_model.md           (resolved addrs + descriptor layout + the menu
                                           / name-entry state machine + DAC sequence)

TRACE LAYOUT (little-endian) — FROZEN; a later task parses this exactly:
  Header:
    +0x00  8 B   magic   b"SCRTRC01"
    +0x08  2 B   u16     version (=1)
    +0x0A  2 B   u16     n_scenarios
  Then a fn-name string table:
    +..    2 B   u16     n_fn_names
    per name: u8 len, len bytes (ascii)
  Then, per scenario:
    u8        scenario_id
    u8        name_len,  name_len bytes (ascii scenario name)
    u32       n_records
    then n_records records.

  Per RECORD (one screen-function call; carries BOTH entry and exit snapshots):
    u16   fn_off         (Ghidra seg-1000 offset of the hooked fn, e.g. 0x35a5)
    u16   fn_name_idx    (index into the fn-name string table)
    SCRSNAP entry        (SCRSNAP_SIZE-byte fixed struct, see SCRSNAP below)
    SCRSNAP exit         (SCRSNAP_SIZE-byte fixed struct)
    u16   ret_val        (the fn's AX return at exit; meaningful for run_main_menu /
                          enter_highscore_name; 0 otherwise)
    u8    render_desc_len, render_desc_len bytes  (render_descriptor_ptr struct @ EXIT)
    u8    p1_sprite_len,  p1_sprite_len bytes      (0x792e blit descriptor @ EXIT)
    u8    seed_len,       seed_len bytes           (fullscreen_buf header @ EXIT, the
                                                    seeded decoded image; 0 if buf empty)
    u16   n_io           (# DAC OUT/IN events captured during this call window; nonzero
                          only for upload_vga_dac_palette / fns that call it)
    n_io * IO            each IO = (u8 dir 0=OUT 1=IN, u16 port, u8 size, u16 value)

  SCRSNAP (screen globals; little-endian):
    u8   current_level         (DGROUP 0x79b2)
    u8   palette_mode          (DGROUP 0x541d, low byte; word read separately not needed)
    u8   menu_option2_setting  (DGROUP 0x79b5)
    u8   input_state           (DGROUP 0x8244)
    u16  score_lo              (DGROUP 0xa0d4)
    u16  score_hi              (DGROUP 0xa0d6)
    u8   timing_flag_accum     (DGROUP 0x854f)
    u8   game_state_928d       (DGROUP 0x928d)
    u16  palette_mode_word     (DGROUP 0x541d as a word — dispatch index)
    u8[8] highscore_name0      (DGROUP 0x8f0, row-0 name buffer, 8 bytes)

Run (sandbox disabled — needs unicorn/uv cache access), HARD timeout:
  timeout 2400 uv run python tools/screens_oracle.py
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
OUT_TRACE = os.path.join(OUT_DIR, "screens_trace.bin")
OUT_MODEL = os.path.join(ROOT, "local/build/screens_model.md")

PSP_SEG = 0x0100
RAM = 0x110000

# DGROUP runtime base — identical formula to sound_oracle.py / physics_oracle.py.
DG_LIN: int = (0x103b + PSP_SEG + 0x10) * 16   # 0x114b0
# Ghidra "segment 1000" == program load base (image offset 0); a fn at Ghidra 1000:off
# lives at raw image offset `off`, loaded at base*16 = 0x1100.
CODE_LIN: int = (PSP_SEG + 0x10) * 16          # 0x1100

# ---------------------------------------------------------------------------
# DGROUP global offsets (resolved from the live Ghidra disassembly of the screen fns):
#   run_main_menu 35a5:  CMP [0x541d],1 (palette_mode); TEST [0x8244] (input_state);
#       MOV [0x79b5] menu_option2_setting; LES BX,[0x574] render_descriptor_ptr;
#       LES BX,[0x8884] p1_sprite; MOV [0x854f] timing_flag_accumulator.
#   show_title_and_init 3ed4:  MOV [0x79b2],1 current_level; MOV [0x928d],1.
#   level_intro_screen 3852:   PUSH [0xa0d6]/[0xa0d4] -> draw_number(score_lo,score_hi,7).
#   highscore_enter_name 59d3: name buffer base row*8 + 0x8f0.
# ---------------------------------------------------------------------------
OFF_CURRENT_LEVEL: int = 0x79b2        # u8
OFF_PALETTE_MODE: int = 0x541d         # u16 (dispatch index for upload_vga_dac_palette)
OFF_MENU_OPTION2: int = 0x79b5         # u8  (menu_option2_setting — option-2 sub-setting)
OFF_INPUT_STATE: int = 0x8244          # u8
OFF_SCORE_LO: int = 0xa0d4             # u16
OFF_SCORE_HI: int = 0xa0d6             # u16
OFF_TIMING_FLAG: int = 0x854f          # u8  (timing_flag_accumulator)
OFF_GAME_STATE_928D: int = 0x928d      # u8  (set 1 at title init, 0xff on quit)
OFF_HIGHSCORE_NAME0: int = 0x8f0       # name buffer for table row 0 (8 bytes/entry)

OFF_RENDER_DESC_PTR: int = 0x0574      # far ptr -> render_descriptor view struct (DAT_0574)
RENDER_DESC_LEN: int = 0x22            # struct fields written up to +0x20 (read 0..0x21)
OFF_P1_SPRITE_PTR: int = 0x8884        # far ptr -> 0x792e blit descriptor (x,y,frame)
P1_SPRITE_DESC_LEN: int = 0x0A         # *p1=0x30, [1]=y/cursor, [2]=frame, [3..4]=src far

OFF_FULLSCREEN_BUF: int = 0x7926       # near off of decoded image buffer
OFF_FULLSCREEN_SEG: int = 0x7928       # seg of decoded image buffer
SEED_HEADER_LEN: int = 0x40            # decoded-image header (incl. embedded palette @+0x33)

OFF_COPYPROTECT: int = 0x119a          # s8 (copyprotect_flag)
OFF_KEY_STATE_PTR: int = 0x4D42        # near ptr to g_key_state_table base

# ---------------------------------------------------------------------------
# Hooked screen / HUD functions (Ghidra seg-1000 offsets).
# ---------------------------------------------------------------------------
FN_NAMES: Dict[int, str] = {
    # Text / number formatters
    0x0816: "draw_number",
    0x07f0: "draw_text_at",
    0x603d: "draw_number_sprites",
    # HUD
    0x51d8: "draw_hud_composite",
    # Title
    0x2ef8: "init_title_graphics",
    0x2fac: "show_title_background",
    0x3ed4: "show_title_and_init",
    # Menu
    0x35a5: "run_main_menu",
    0x0f7a: "show_menu_select_screen",
    # Highscore
    0x5681: "show_highscore_screen",
    0x57e1: "render_highscore_table",
    0x5c87: "enter_highscore_name",
    0x59d3: "highscore_enter_name",
    # Level intro
    0x3852: "level_intro_screen",
    0x0d9d: "show_level_intro_screen",
    # Transition / palette
    0x3467: "play_iris_wipe_transition",
    0x9864: "upload_vga_dac_palette",
}

# fns whose execution window we scope DAC port-I/O capture to. upload_vga_dac_palette is
# THE DAC writer (1000:9864 -> 2036:0000 -> DGROUP[palette_mode*2+0x6976] handler doing
# out 0x3c8/0x3c9). play_iris_wipe_transition (3467) progressively reprograms the DAC too.
DAC_FNS: set = {0x9864, 0x3467}
# DAC ports we capture (index/data). Other VGA ports are emulated, not recorded.
DAC_PORTS: set = {0x3c8, 0x3c9}

# ---------------------------------------------------------------------------
# Trace format constants
# ---------------------------------------------------------------------------
TRACE_MAGIC: bytes = b"SCRTRC01"
TRACE_VERSION: int = 1
# SCRSNAP: see header docstring. 4 bytes + 2 words + 2 bytes + 1 word + 8 name bytes.
SCRSNAP_FMT: str = "<BBBB" + "HH" + "BB" + "H" + "8B"
SCRSNAP_SIZE: int = struct.calcsize(SCRSNAP_FMT)

# ---------------------------------------------------------------------------
# Scancode / input_state mapping (from sound_oracle.py / physics_oracle.py).
# In the screen state machines the input_state bits are repurposed:
#   run_main_menu:        1=up, 2=down, 0x10=fire/select
#   *_enter_name:         1=left, 2=right, 4=prev char, 8=next char, 0x10=done
#   level_intro_screen:   1=up,2=down,4=left,8=right,0x10=start
# ---------------------------------------------------------------------------
IS_UP: int = 0x01
IS_DOWN: int = 0x02
IS_LEFT: int = 0x04
IS_RIGHT: int = 0x08
IS_FIRE: int = 0x10

# ---------------------------------------------------------------------------
# Scenarios. Each: (id, name, calls) where calls is a list of
#   (fn_off, args, input_script)
# input_script: a list of input_state bytes fed (one per poll/iteration) to the
# state machine by patching DGROUP 0x8244 each time poll_input/FUN_75a2 would read it.
# We drive the state machine by pre-loading a small queue and re-injecting on each
# poll via the keyboard FUN_1000_75a2 path AND a direct input_state patch.
# ---------------------------------------------------------------------------
Scenario = Tuple[int, str, List[Tuple[int, List[int], List[int]]]]


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
            # The screen state machines take input through FUN_1000_75a2 (see the input
            # driver below), not int16 directly. We only stub int16 so any incidental
            # BIOS keyboard poll returns Enter / "no key waiting".
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

    # --- DAC port-I/O capture state ------------------------------------------------
    io_capture = {"depth": 0}
    io_seq: List[Tuple[int, int, int, int]] = []

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
        if io_capture["depth"] > 0 and port in DAC_PORTS:
            io_seq.append((1, port, size, val & 0xFFFF))
        return val

    # --- minimal VGA planar emulation (copied from sound_oracle.py) ----------------
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
        if io_capture["depth"] > 0 and port in DAC_PORTS:
            io_seq.append((0, port, size, value))
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

    def rd_bytes(off: int, n: int) -> bytes:
        return bytes(uc.mem_read(DG_LIN + off, n))

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

    def read_fullscreen_header() -> bytes:
        o = rd_u16(OFF_FULLSCREEN_BUF)
        s = rd_u16(OFF_FULLSCREEN_SEG)
        lin = (s * 16 + o) & 0xFFFFF
        try:
            return bytes(uc.mem_read(lin, SEED_HEADER_LEN))
        except UcError:
            return b""

    def snap() -> bytes:
        name0 = rd_bytes(OFF_HIGHSCORE_NAME0, 8)
        return struct.pack(
            SCRSNAP_FMT,
            rd8(OFF_CURRENT_LEVEL),
            rd8(OFF_PALETTE_MODE),
            rd8(OFF_MENU_OPTION2),
            rd8(OFF_INPUT_STATE),
            rd_u16(OFF_SCORE_LO),
            rd_u16(OFF_SCORE_HI),
            rd8(OFF_TIMING_FLAG),
            rd8(OFF_GAME_STATE_928D),
            rd_u16(OFF_PALETTE_MODE),
            *name0)

    # ---------------------------------------------------------------------------
    # screen-function hooks (entry + exit via dynamic return-address hook)
    # ---------------------------------------------------------------------------
    capturing = {"on": False}
    cur_records: List[bytes] = []
    reached: collections.Counter = collections.Counter()
    pending_exit: dict = {}
    exit_hook_lins: set = set()
    fn_name_list: List[str] = list(dict.fromkeys(FN_NAMES.values()))
    fn_name_idx = {n: i for i, n in enumerate(fn_name_list)}
    # per-fn aggregate DAC sequences for the model md (first seen per fn).
    dac_io: Dict[int, List[Tuple[int, int, int, int]]] = {}

    def emit_record(fn_off: int, entry_snap: bytes, exit_snap: bytes, ret_val: int,
                    render_desc: bytes, p1_sprite: bytes, seed: bytes,
                    io_events: List[Tuple[int, int, int, int]]) -> None:
        rec = struct.pack("<HH", fn_off, fn_name_idx[FN_NAMES[fn_off]])
        rec += entry_snap + exit_snap
        rec += struct.pack("<H", ret_val & 0xFFFF)
        rec += struct.pack("<B", len(render_desc)) + render_desc
        rec += struct.pack("<B", len(p1_sprite)) + p1_sprite
        rec += struct.pack("<B", len(seed)) + seed
        rec += struct.pack("<H", len(io_events))
        for (d, port, size, value) in io_events:
            rec += struct.pack("<BHBH", d, port, size, value)
        cur_records.append(rec)

    def hook_fn_entry(uc: Uc, addr: int, size: int, _: object) -> None:
        if not capturing["on"]:
            return
        fn_off = (addr - CODE_LIN) & 0xFFFF
        if fn_off not in FN_NAMES:
            return
        reached[fn_off] += 1
        entry_snap = snap()
        is_dac = fn_off in DAC_FNS
        if is_dac:
            if io_capture["depth"] == 0:
                io_seq.clear()
            io_capture["depth"] += 1
        ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc.mem_read(ss * 16 + sp, 2)))[0]
        ret_lin = (CODE_LIN + ret_off) & 0xFFFFF
        io_mark = len(io_seq) if is_dac else 0
        pending_exit.setdefault(ret_lin, []).append(
            (fn_off, entry_snap, is_dac, io_mark))
        if ret_lin not in exit_hook_lins:
            exit_hook_lins.add(ret_lin)
            uc.hook_add(UC_HOOK_CODE, hook_fn_exit, None, ret_lin, ret_lin)

    def hook_fn_exit(uc: Uc, addr: int, size: int, _: object) -> None:
        stack = pending_exit.get(addr)
        if not stack:
            return
        (fn_off, entry_snap, is_dac, io_mark) = stack.pop()
        exit_snap = snap()
        ret_val = uc.reg_read(UC_X86_REG_AX) & 0xFFFF
        render_desc = read_far_target(OFF_RENDER_DESC_PTR, RENDER_DESC_LEN)
        p1_sprite = read_far_target(OFF_P1_SPRITE_PTR, P1_SPRITE_DESC_LEN)
        seed = read_fullscreen_header()
        io_events: List[Tuple[int, int, int, int]] = []
        if is_dac:
            io_events = list(io_seq[io_mark:])
            io_capture["depth"] -= 1
            if io_capture["depth"] == 0:
                io_seq.clear()
            if fn_off not in dac_io and io_events:
                dac_io[fn_off] = io_events
        emit_record(fn_off, entry_snap, exit_snap, ret_val,
                    render_desc, p1_sprite, seed, io_events)

    for off in FN_NAMES:
        lin = CODE_LIN + off
        uc.hook_add(UC_HOOK_CODE, hook_fn_entry, None, lin, lin)

    # ---------------------------------------------------------------------------
    # Boot to level 1 (identical approach to sound_oracle.py). This drives the engine
    # through the title/menu front-end (loading TITRE.VEC/BUMPRESE.VEC etc. via the real
    # resource-table file I/O), proving the seed-via-natural-load path works, then on into
    # the level. We snapshot the post-boot machine so each scenario starts deterministic.
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

    print("[screens_oracle] booting BUMPY (level %d)..." % LEVEL, flush=True)

    while total_instr < 400_000_000:
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e); tr["err"] = err; break
        total_instr += CHUNK
        if total_instr % 40_000_000 == 0:
            print("[screens_oracle] heartbeat: %dM instr, countdown=%s" % (
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
            print("[screens_oracle] level loaded (%s) at chunk %d — settling %d ticks" % (
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
        print("[screens_oracle] ERROR: premature exit/fault during boot: exit=%s fault=%s err=%s" % (
            tr.get("exit"), tr.get("fault"), err), flush=True)
        return
    if not opened(BUMNAME):
        print("[screens_oracle] ERROR: level %s never loaded after %dM instructions" % (
            BUMNAME, total_instr // 1_000_000), flush=True)
        return

    print("[screens_oracle] boot complete. Files: %s" % (
        [o[1] for o in tr.get("fileops", [])]), flush=True)

    boot_ram = bytes(uc.mem_read(0, RAM))
    boot_ctx = uc.context_save()

    def restore_boot_state() -> None:
        uc.mem_write(0, boot_ram)
        uc.context_restore(boot_ctx)
        tr["fault"] = None
        tr["exit"] = None

    # ---------------------------------------------------------------------------
    # Direct synchronous call of a near screen fn (cdecl16near, DS=DGROUP) — the
    # call_near() pattern from sound_oracle.py / anim_oracle.py. The fn's RET lands on a
    # 2-byte NOP;HLT landing pad temporarily written at the top of the CODE segment.
    # ---------------------------------------------------------------------------
    code_seg = base & 0xFFFF
    LANDING_OFF = 0xfffe
    LANDING_LIN = CODE_LIN + LANDING_OFF
    STOP_LIN = LANDING_LIN + 1

    # an upper bound on instructions per synthetic screen call. The name-entry and menu
    # loops poll input; we feed a bounded keyboard script so they terminate, but cap hard.
    CALL_INSN_CAP = 60_000_000

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
    # Single, structure-faithful input driver: FUN_1000_75a2 is the engine's one input
    # primitive (returns the action byte in AL, 0 = no input). EVERY screen state machine
    # reads input through it — directly (highscore_enter_name 59d3 / enter_highscore_name
    # 5c87) or via poll_input (1dde), which calls FUN_75a2 then stores AL into input_state
    # 0x8244 (run_main_menu 35a5 / level_intro_screen 3852 / wait_keypress 328f).
    #
    # We drive the player by hooking FUN_75a2's RETURN and overwriting AL with the next
    # scripted action byte; when the script is EXHAUSTED we return 0 (no input). This 0 is
    # important: the menu's post-action drain loop `do { k = FUN_75a2(); } while (k != 0);`
    # and wait_keypress's poll only progress when FUN_75a2 eventually returns 0. So each
    # scripted ACTION pulse is followed by a 0 (built by `_pulse()` below) so the action is
    # consumed once and the drain/idle calls then see "no key". A terminating IS_FIRE pulse
    # ends every loop (menu select / name-entry done / intro start / wait_keypress).
    # This is the engine's real input path (no guessed loop-PCs), so captured descriptors
    # reflect exactly what the engine builds for those keypresses.
    # ---------------------------------------------------------------------------
    FUN_75A2_OFF = 0x75a2
    input_driver = {"queue": [], "active": False}
    in75_exit_lins: set = set()
    in75_pending: dict = {}   # ret_lin -> count of nested FUN_75a2 awaiting injection

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
        nxt = q.pop(0) if q else 0   # exhausted -> no input (lets drain/idle loops settle)
        ax = uc2.reg_read(UC_X86_REG_AX) & 0xFF00
        uc2.reg_write(UC_X86_REG_AX, ax | (nxt & 0xFF))

    uc.hook_add(UC_HOOK_CODE, hook_75a2_entry, None,
                CODE_LIN + FUN_75A2_OFF, CODE_LIN + FUN_75A2_OFF)

    def seed_input_state(off: int, val: int) -> None:
        uc.mem_write(DG_LIN + off, bytes([val & 0xFF]))

    # Expand a high-level action list into the FUN_75a2 return stream: each action becomes
    # [action, 0, 0, 0] — the action is taken once, then the drain/idle FUN_75a2 calls in
    # that iteration see 0. A trailing IS_FIRE pulse terminates the loop. 4 zeros per pulse
    # comfortably covers the menu drain loop + a poll; harmless for the name-entry loops
    # (they re-poll the next iteration). After exhaustion the driver returns 0 forever, so
    # if a terminating action is omitted the loop relies on the trailing IS_FIRE we append.
    def expand_script(actions: List[int], terminate: bool = True) -> List[int]:
        out: List[int] = []
        for a in actions:
            out.extend([a & 0xFF, 0, 0, 0])
        if terminate:
            out.extend([IS_FIRE, 0, 0, 0])
        return out

    # ---------------------------------------------------------------------------
    # Scenarios — one record-set each. Each call is (fn_off, args, input_script).
    # input_script is the sequence of input_state bytes the state machine consumes.
    # ---------------------------------------------------------------------------
    SCENARIOS: List[Scenario] = [
        # 1: title/HUD build — these load+decode a real resource then build descriptors.
        #    No input loop. draw_hud_composite + draw_number cover the HUD/number path.
        (1, "title_and_hud", [
            (0x2fac, [], []),                 # show_title_background (res 2 BUMPRESE? TITRE)
            (0x2ef8, [], []),                 # init_title_graphics (-> draw_hud_composite)
            (0x51d8, [], []),                 # draw_hud_composite (direct)
            (0x0816, [0x0539, 0, 7, 0, 8], []),  # draw_number(0x539, 0, width=7) at x=0 y=8
            (0x603d, [0x270f, 0, 7, 0xb0, 0x41], []),  # draw_number_sprites(9999,7,x,y)
        ]),
        # 2: menu cursor STATE MACHINE — drive up/down/fire. cursor starts 0; script
        #    down,down,fire selects item 2 (which cycles menu_option2_setting then keeps
        #    selecting==0xff), so add a further fire on item 3 to actually return. We
        #    observe cursor_index via the p1_sprite descriptor each iteration and the
        #    return value (selected_item) at exit.
        (2, "menu_cursor", [
            # navigate cursor down to item 3; the appended terminating IS_FIRE selects it.
            (0x35a5, [], [IS_DOWN, IS_DOWN, IS_DOWN]),
        ]),
        # 3: menu option-2 cycle — navigate to option 2 then fire twice to cycle the
        #    global menu_option2_setting (0->1->2), then up to item 0; the terminating
        #    IS_FIRE selects item 0 and returns.
        (3, "menu_option2", [
            (0x35a5, [], [IS_DOWN, IS_DOWN, IS_FIRE, IS_FIRE, IS_UP, IS_UP]),
        ]),
        # 4: highscore screen + table render (qualified=0 path -> wait_keypress).
        (4, "highscore", [
            (0x5681, [], []),                 # show_highscore_screen -> render_highscore_table
        ]),
        # 5: highscore name-entry state machine — right,right (advance chars), prev, done.
        #    row 0. Captures the 8-char name buffer @ 0x8f0 being edited.
        (5, "name_entry_table", [
            # advance/retreat chars (4=prev,8=next) + cycle letters (1/2); terminating
            # IS_FIRE (bit 0x10 = done) ends the loop. highscore_enter_name(row=0).
            (0x59d3, [0], [IS_RIGHT, IS_DOWN, IS_RIGHT]),
        ]),
        # 6: menu-select name entry (the 6-char enter_highscore_name interactive form).
        (6, "name_entry_select", [
            (0x5c87, [4, 4], [IS_RIGHT, IS_DOWN]),  # enter_highscore_name(x=4,y=4)
        ]),
        # 7: level intro screen state machine — move around then start (terminating fire).
        (7, "level_intro", [
            (0x3852, [], [IS_LEFT, IS_RIGHT, IS_DOWN, IS_UP]),
        ]),
        # 8: show_level_intro_screen + show_menu_select_screen (sprite-glyph text rows).
        #    Both end on a 0x10 input action via FUN_75a2 (the appended terminator).
        (8, "intro_select_glyphs", [
            (0x0d9d, [], []),                 # show_level_intro_screen
            (0x0f7a, [], [IS_RIGHT]),         # show_menu_select_screen (-> enter_highscore_name)
        ]),
        # 9: title init that sets current_level=1 (calls show_highscore_screen first),
        #    + the iris-wipe transition + the DAC upload in isolation.
        (9, "title_init_dac", [
            (0x3467, [], []),                 # play_iris_wipe_transition (progressive DAC)
            (0x9864, [], []),                 # upload_vga_dac_palette (DAC port writes)
            (0x07f0, [0, 8, 0x14, 0x19], []), # draw_text_at(x,y,w,h)
            (0x3ed4, [], []),                 # show_title_and_init (wait_keypress -> fire)
        ]),
    ]

    # ---------------------------------------------------------------------------
    # Run scenarios
    # ---------------------------------------------------------------------------
    def run_scenario(sc: Scenario) -> List[bytes]:
        sc_id, name, calls = sc
        cur_records.clear()
        capturing["on"] = True
        for (fn_off, args, script) in calls:
            # reset input each call; load the scripted action queue the FUN_75a2 return
            # hook injects. expand_script turns each high-level action into an
            # [action,0,0,0] pulse and appends a terminating IS_FIRE pulse so every loop
            # (menu select / name-entry done / intro start / wait_keypress) ends; after
            # exhaustion the driver returns 0 (no input).
            seed_input_state(OFF_INPUT_STATE, 0)
            clear_all_keys()
            input_driver["queue"] = expand_script(script) if script else expand_script([])
            input_driver["active"] = True
            call_near(fn_off, args)
            input_driver["active"] = False
            input_driver["queue"] = []
        capturing["on"] = False
        return list(cur_records)

    scenario_blobs: List[Tuple[Scenario, List[bytes]]] = []
    for sc in SCENARIOS:
        sc_id, name, calls = sc
        restore_boot_state()
        print("[screens_oracle] === scenario %d (%s) ===" % (sc_id, name), flush=True)
        recs = run_scenario(sc)
        n_io = sum(_record_io_count(r) for r in recs)
        print("[screens_oracle]   %d records, %d DAC I/O events" % (len(recs), n_io),
              flush=True)
        scenario_blobs.append((sc, recs))

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
            sc_id, name, calls = sc
            nb = name.encode("ascii")
            f.write(struct.pack("<B", sc_id))
            f.write(struct.pack("<B", len(nb))); f.write(nb)
            f.write(struct.pack("<I", len(recs)))
            for r in recs:
                f.write(r)
    trace_bytes = os.path.getsize(OUT_TRACE)
    print("[screens_oracle] wrote %s (%d B)" % (OUT_TRACE, trace_bytes), flush=True)

    # ---------------------------------------------------------------------------
    # Round-trip parser check (standalone re-parse of the file we just wrote).
    # ---------------------------------------------------------------------------
    def parse_trace(path: str) -> dict:
        data = open(path, "rb").read()
        assert data[:8] == TRACE_MAGIC, "bad magic"
        ver, n_sc = struct.unpack_from("<HH", data, 8)
        assert ver == TRACE_VERSION
        o = 12
        n_names = struct.unpack_from("<H", data, o)[0]; o += 2
        names = []
        for _ in range(n_names):
            ln = data[o]; o += 1
            names.append(data[o:o + ln].decode("ascii")); o += ln
        scenarios = []
        for _ in range(n_sc):
            sid = data[o]; o += 1
            nl = data[o]; o += 1
            nm = data[o:o + nl].decode("ascii"); o += nl
            n_rec = struct.unpack_from("<I", data, o)[0]; o += 4
            recs = []
            for _r in range(n_rec):
                fn_off, name_idx = struct.unpack_from("<HH", data, o); o += 4
                ent = struct.unpack_from(SCRSNAP_FMT, data, o); o += SCRSNAP_SIZE
                ex = struct.unpack_from(SCRSNAP_FMT, data, o); o += SCRSNAP_SIZE
                ret_val = struct.unpack_from("<H", data, o)[0]; o += 2
                rdl = data[o]; o += 1
                render_desc = data[o:o + rdl]; o += rdl
                psl = data[o]; o += 1
                p1_sprite = data[o:o + psl]; o += psl
                sdl = data[o]; o += 1
                seed = data[o:o + sdl]; o += sdl
                n_io = struct.unpack_from("<H", data, o)[0]; o += 2
                ios = []
                for _i in range(n_io):
                    d, port, sz, val = struct.unpack_from("<BHBH", data, o); o += 6
                    ios.append((d, port, sz, val))
                recs.append(dict(fn_off=fn_off, fn=names[name_idx], ent=ent, ex=ex,
                                 ret=ret_val, render_desc=render_desc, p1_sprite=p1_sprite,
                                 seed=seed, io=ios))
            scenarios.append(dict(id=sid, name=nm, recs=recs))
        assert o == len(data), "trailing bytes: parsed %d of %d" % (o, len(data))
        return dict(names=names, scenarios=scenarios)

    parsed = parse_trace(OUT_TRACE)
    print("[screens_oracle] round-trip parse OK: %d scenarios, %d fn-names" % (
        len(parsed["scenarios"]), len(parsed["names"])), flush=True)

    # ---------------------------------------------------------------------------
    # screens_model.md
    # ---------------------------------------------------------------------------
    SNAP_FIELDS = ["current_level", "palette_mode", "menu_option2_setting", "input_state",
                   "score_lo", "score_hi", "timing_flag_accum", "game_state_928d",
                   "palette_mode_word", "name0"]

    lines: List[str] = []
    lines.append("# Bumpy Phase-7 screens capture model (discovery)\n\n")
    lines.append("Generated by `tools/screens_oracle.py`. Capture granularity = screen "
                 "FUNCTION-CALL boundary (entry+exit), plus the render_descriptor_ptr + "
                 "p1_sprite blit descriptors, the seeded decoded image buffer, and the "
                 "upload_vga_dac_palette DAC OUT sequence (ports 0x3c8/0x3c9).\n\n")

    lines.append("## Resolved addresses (from the live Ghidra decomp/asm)\n\n")
    lines.append("### DGROUP (Ghidra seg 0x203b; runtime read at DG_LIN+off, "
                 "DG_LIN=0x%05x)\n\n" % DG_LIN)
    lines.append("| symbol | DGROUP off | Ghidra-linear |\n|---|---|---|\n")
    for nm, off in [("current_level", OFF_CURRENT_LEVEL),
                    ("palette_mode", OFF_PALETTE_MODE),
                    ("menu_option2_setting", OFF_MENU_OPTION2),
                    ("input_state", OFF_INPUT_STATE),
                    ("score_lo", OFF_SCORE_LO),
                    ("score_hi", OFF_SCORE_HI),
                    ("timing_flag_accumulator", OFF_TIMING_FLAG),
                    ("game_state (0x928d)", OFF_GAME_STATE_928D),
                    ("highscore name row0 (0x8f0)", OFF_HIGHSCORE_NAME0),
                    ("render_descriptor_ptr (far, DAT_0574)", OFF_RENDER_DESC_PTR),
                    ("p1_sprite (far, ->0x792e)", OFF_P1_SPRITE_PTR),
                    ("fullscreen_buf off", OFF_FULLSCREEN_BUF),
                    ("fullscreen_buf seg", OFF_FULLSCREEN_SEG)]:
        lines.append("| %s | 0x%04x | ram0x%05x |\n" % (nm, off, 0x203b * 16 + off))

    lines.append("\n## render_descriptor_ptr view-struct layout\n\n")
    lines.append("`render_descriptor_ptr` is a FAR pointer at DGROUP 0x0574/0x0576 "
                 "(`_render_descriptor_ptr` / `DAT_0574`). The screen builders write a "
                 "0x22-byte view struct through it; observed field writes:\n\n")
    lines.append("| off | meaning (from the decomp) |\n|---|---|\n")
    lines.append("| +2/+4 | image far ptr = fullscreen_buf+99 : fullscreen_buf_seg |\n")
    lines.append("| +6/+8 | src x / src y (tile coords) |\n")
    lines.append("| +0xa/+0xc | width / height (tiles, e.g. 0x14 x 0x19 fullscreen) |\n")
    lines.append("| +0xe | flags (1 = fullscreen blit) |\n")
    lines.append("| +0x10/+0x12 | (HUD) tile-source far ptr (sprite glyph table) |\n")
    lines.append("| +0x14/+0x16 | dest x / dest y |\n")
    lines.append("| +0x18/+0x1a | (HUD) sub-extent w/h |\n")
    lines.append("| +0x1c/+0x1e/+0x20 | clip x / clip w / clip h |\n")

    lines.append("\n## p1_sprite blit descriptor (0x792e via far ptr @ 0x8884)\n\n")
    lines.append("Captured at EXIT. The menu/name-entry/glyph code writes the sprite "
                 "descriptor at DGROUP 0x792e (pointed to by the far ptr @ 0x8884) then "
                 "calls blit_sprite(0x792e). Field layout: word[0]=dest x, word[1]=dest "
                 "y (run_main_menu sets it to `cursor_index*0x10 + 0x70` — THIS is how the "
                 "menu cursor LOCAL is observed), word[2]=frame, word[3..4]=source far "
                 "ptr, `*p1_sprite=0x30` in the menu.\n")

    lines.append("\n## Menu / name-entry STATE MACHINES\n\n")
    lines.append("`run_main_menu` (1000:35a5) returns `selected_item` (a stack LOCAL). "
                 "`cursor_index` is also a stack local. Both are observed without reading "
                 "stack memory: (a) cursor_index via the p1_sprite descriptor word[1] = "
                 "cursor_index*0x10+0x70; (b) selected_item via the AX return value "
                 "(0xff while still selecting). input_state bits: 1=up, 2=down, 0x10=fire. "
                 "Option 2 (case 2) cycles `menu_option2_setting` (DGROUP 0x79b5, a "
                 "GLOBAL) 0->1->2->0 instead of returning. The name-entry loops "
                 "(highscore_enter_name 59d3 / enter_highscore_name 5c87) edit an 8-/6-char "
                 "name buffer (base row*8+0x8f0) and re-poll via FUN_1000_75a2; bits "
                 "1=left,2=right,4=prev char,8=next char,0x10=done.\n")

    lines.append("\n## Resource-buffer SEEDING\n\n")
    lines.append("The screen fns load their image via open_resource(idx)/read_chunked "
                 "(INT 21h) then vec_decode into fullscreen_buf. The resource table maps "
                 "idx to a real game file present in GAME_DIR (TITRE.VEC=res 0x11, "
                 "BUMPRESE.VEC=res 1, SCORE.VEC=res 3, MONDE<n>.VEC=res level+7), served "
                 "by this oracle's INT-21h Files handler. So the engine's NATURAL "
                 "load+decode runs INSIDE the captured call window; we SNAPSHOT the first "
                 "0x%02x bytes of the fullscreen_buf the engine itself produced (the "
                 "decoded-image header, which carries the embedded 16-colour 6-bit-RGB "
                 "palette at +0x33 that upload_vga_dac_palette pushes to the DAC). This "
                 "makes the screen-build descriptors deterministic without re-running file "
                 "I/O. No bytes are injected by hand.\n" % SEED_HEADER_LEN)

    lines.append("\n## upload_vga_dac_palette DAC dispatch\n\n")
    lines.append("`upload_vga_dac_palette` (1000:9864) is a thunk to the FAR overlay "
                 "`dispatch_by_palette_mode_2036` (2036:0000), which indirect-calls the "
                 "handler at `DGROUP[palette_mode*2 + 0x6976]`. That handler does the real "
                 "`out 0x3c8` (index) / `out 0x3c9` (R,G,B) writes. We scope DAC OUT/IN "
                 "capture (ports 0x3c8/0x3c9) to the upload_vga_dac_palette AND "
                 "play_iris_wipe_transition windows; `play_iris_wipe_transition` (3467) "
                 "calls upload_vga_dac_palette 4x/step x10 steps during the rectangle wipe, "
                 "so the iris-wipe record carries the nested DAC sequence (the outer DAC "
                 "window absorbs the nested upload calls).\n\n")
    lines.append("OBSERVED: under this boot `palette_mode == 2` throughout, and the "
                 "standalone `upload_vga_dac_palette` records carry 0 DAC events (the "
                 "palette_mode==2 dispatch handler does not itself touch 0x3c8/0x3c9 here). "
                 "The captured DAC `out 0x3c8` (index) / `out 0x3c9` (RGB triples) writes "
                 "all come from inside the `play_iris_wipe_transition` window — emitted by "
                 "the per-step view-blit (FUN_1000_7b4a) the wipe runs before each "
                 "upload_vga_dac_palette — so the iris-wipe record carries the full "
                 "50-write sequence. The harness scopes DAC capture to BOTH fns so the "
                 "writes are captured regardless of which one emits them. A later task can "
                 "force palette_mode 0/1 to exercise the other dispatch handlers.\n")

    lines.append("\n## Hooked functions\n\n")
    lines.append("| seg-1000 off | name | reached |\n|---|---|---|\n")
    for off in sorted(FN_NAMES):
        lines.append("| 1000:%04x | %s | %d |\n" % (off, FN_NAMES[off], reached[off]))

    lines.append("\n## Captured DAC sequences (per fn, first occurrence)\n\n")
    if not dac_io:
        lines.append("_(no DAC port I/O captured — see scenario notes)_\n")
    for off in sorted(dac_io):
        seq_io = dac_io[off]
        outs = [(p, v) for (d, p, sz, v) in seq_io if d == 0]
        lines.append("\n### %s (1000:%04x) — %d events (%d OUT)\n\n" % (
            FN_NAMES[off], off, len(seq_io), len(outs)))
        head = seq_io[:24]
        rows = []
        for (d, port, sz, val) in head:
            rows.append("%s 0x%03x=0x%02x" % ("OUT" if d == 0 else "IN", port, val))
        suffix = " ..." if len(seq_io) > 24 else ""
        lines.append("`" + " | ".join(rows) + suffix + "`\n")

    lines.append("\n## Per-scenario record summary\n\n")
    for sc, recs in scenario_blobs:
        sc_id, name, calls = sc
        by_fn = collections.Counter()
        io_total = 0
        for r in recs:
            fn_off = struct.unpack_from("<H", r, 0)[0]
            by_fn[FN_NAMES[fn_off]] += 1
            io_total += _record_io_count(r)
        lines.append("### Scenario %d — %s\n\n" % (sc_id, name))
        lines.append("- records: %d, DAC I/O events: %d\n" % (len(recs), io_total))
        if by_fn:
            lines.append("- fns: %s\n" % ", ".join(
                "%s x%d" % (k, v) for k, v in by_fn.most_common()))
        else:
            lines.append("- fns: (none reached)\n")

    lines.append("\n## Functions NOT reached\n\n")
    not_reached = [off for off in FN_NAMES if reached[off] == 0]
    if not_reached:
        for off in sorted(not_reached):
            lines.append("- 1000:%04x %s\n" % (off, FN_NAMES[off]))
        lines.append("\nThese were not exercised by the current scenarios; the call_near() "
                     "harness in this oracle can reach any near entry point if a later "
                     "task needs them.\n")
    else:
        lines.append("- (all hooked functions reached)\n")

    os.makedirs(os.path.dirname(OUT_MODEL), exist_ok=True)
    with open(OUT_MODEL, "w") as f:
        f.write("".join(lines))
    print("[screens_oracle] wrote %s" % OUT_MODEL, flush=True)

    # ---------------------------------------------------------------------------
    # Console summary
    # ---------------------------------------------------------------------------
    print("\n[screens_oracle] REACHED screen functions:", flush=True)
    for off in sorted(FN_NAMES):
        if reached[off]:
            print("   1000:%04x  %-28s x%d" % (off, FN_NAMES[off], reached[off]), flush=True)
    nr = [FN_NAMES[o] for o in FN_NAMES if reached[o] == 0]
    if nr:
        print("[screens_oracle] NOT reached: %s" % ", ".join(nr), flush=True)
    print("[screens_oracle] DAC fns with captured port I/O:", flush=True)
    for off in sorted(dac_io):
        seq_io = dac_io[off]
        outs = [(p, v) for (d, p, sz, v) in seq_io if d == 0]
        print("   1000:%04x %-28s %d events, first OUTs=%s" % (
            off, FN_NAMES[off], len(seq_io),
            ", ".join("0x%03x=0x%02x" % (p, v) for (p, v) in outs[:6])), flush=True)
    if err:
        print("[screens_oracle] emu error:", err, flush=True)
    if tr.get("call_errs"):
        print("[screens_oracle] call_near errors:", tr["call_errs"][:10], flush=True)


# ---------------------------------------------------------------------------
# Record helpers (module-level so the writer and round-trip share them).
# A record's variable-length tail: [u16 ret][u8 rdl + rdl][u8 psl + psl][u8 sdl + sdl]
# [u16 n_io][n_io * 6]. We need the n_io count for per-scenario tallies.
# ---------------------------------------------------------------------------
def _record_io_count(rec: bytes) -> int:
    o = 4 + 2 * SCRSNAP_SIZE          # past fn_off,fn_idx, entry, exit
    o += 2                            # ret_val
    rdl = rec[o]; o += 1 + rdl
    psl = rec[o]; o += 1 + psl
    sdl = rec[o]; o += 1 + sdl
    n_io = struct.unpack_from("<H", rec, o)[0]
    return n_io


if __name__ == "__main__":
    main()
