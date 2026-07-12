#!/usr/bin/env python3
"""midi_oracle.py — Task C2 Unicorn CAPTURE ORACLE for the MIDI/OPL2 engine.

Forks the tools/sound_oracle.py scaffold (load_mz, the Uc(UC_ARCH_X86, UC_MODE_16)
setup, hook_intr, hook_in/hook_out + UC_HOOK_INSN OUT/IN capture, the call_near()
per-function direct-invocation harness, the hook_fn_entry/hook_fn_exit natural-run
capture, snap()/emit_record, the <MAGIC>01 trace writer + parse_trace round-trip
check) — see that file's header for the shared design. This oracle captures ground
truth from the ORIGINAL (unpacked) binary for the MIDI/SMF sequencer + MIDI-to-OPL2
voice-dispatch call tree; the reconstruction (src/midi.c) does not exist yet
(globals-only skeleton, Task C1) — this oracle does NOT read src/.

── APPROACH CHOSEN: seeded direct calls, grounded in the disassembled register/stack
   ABI of every target (NOT the "PREFERRED natural boot-to-music" path) ──────────────
The brief's PREFERRED path (boot to title/menu music, hook mid-gameplay) was not
attempted: there is no already-established boot script that reaches a point where
midi_play_sequence/midi_process_event run against BUMPY.MID (unlike sound_oracle's
gameplay-tick boot, which reaches play_sound naturally via existing player-move
scaffolding). Reverse-engineering *where* the title screen first triggers MIDI
playback would be its own multi-hour investigation, disproportionate to a first
capture-oracle task.

Instead, every target's REAL calling convention was recovered via Ghidra
disassemble_function/decompile_function_by_address (not just the simplified
decompile view, which hides real SI/BX/DI-relative reads — see per-function
RECONSTRUCTION notes below) and driven with **seeded register/stack inputs that
include the REAL Bumpy.mid file bytes** wherever the ABI reads a file image:

  midi_load_sequence (87cd) IS a stack-arg cdecl16near function: PUSH BP; MOV BP,SP;
  ... LES SI,[BP+8] (2nd arg, far ptr, stored as midi_song_data_off/_seg DGROUP
  0x5580/0x5582); LDS SI,[BP+4] (1st arg, far ptr, stored as midi_aux_ptr_off/_seg
  CODE 0x8485/0x8487) -- THEN CALLs midi_parse_file (8809) WITHOUT reloading DS:SI,
  so midi_parse_file's register-entry DS:SI is whatever the LAST LDS set it to: the
  FIRST stack arg (Task C1's "aux_ptr"), not the second ("song_data")! This is a
  genuine correction to Task C1's midi.h fidelity note ("aux_ptr ... no reader found
  in the functions examined this task") — the reader IS midi_parse_file, reached via
  this exact fall-through. Seeding that first stack arg (aux_off/aux_seg) with a far
  pointer at the REAL loaded Bumpy.mid bytes makes ONE seeded call to
  midi_load_sequence naturally cascade through the REAL parser call tree with REAL
  data: midi_parse_file -> (per real MThd/MTrk chunk) seq_normalize_far_ptr,
  midi_init_track_table -> midi_read_varlen + midi_process_event (per real track's
  first event) -> midi_start_playback -> midi_install_tempo_timer. This is a
  "seeded natural cascade": not blind guesswork (every seed is placed exactly where
  the disassembly shows the ABI expects it), and not the gameplay-boot path either.
  midi_play_sequence (8977) is a 0-stack-arg guard (checks sound_active_device_mask
  != 0x8000) that JMPs (tail-calls, not CALLs) into 87cd reusing the CALLER's stack
  args unchanged -- so it is invoked with the identical 5-word arg list.

  The MIDI-to-OPL2 voice-dispatch leaves (midi_emit_voice_msg_w1/w2/w3,
  emit_midi_voice_message, opl_event_note_on, opl_set_note_params,
  seq_set_channel_param) are genuinely REGISTER-entry (no PUSH BP, ambient
  AL/BX/CX/DX/SI/DI) with NO natural caller reconstructed yet (their real caller,
  the OPL-mode MIDI-channel-message dispatch, is out of scope -- see sound.c's own
  "NOT ORACLE-EXERCISED" precedent for the 9 snddrv_dispatch_b/c/d modeX backends).
  These are captured via the brief's documented FALLBACK: concrete, deterministic,
  reproducible register/memory seeds (documented per-function below), so a later
  host replay can seed the IDENTICAL registers/memory and assert the IDENTICAL OPL
  port sequence -- "same-seeded-input differential is still valid" per the brief.

  opl_read_status / opl2_reset_all_regs / maybe_opl2_detect_chip need NO seeding at
  all: 100% hardcoded immediate OPL register programs / delay loops -- self-contained,
  fully deterministic captures straight out of the box.

FN_NAMES ABI summary (recovered via disassemble_function; * = register-entry):
  midi_load_sequence(87cd)      stack: [aux_off,aux_seg,song_off,song_seg,flag]
  midi_play_sequence(8977)      stack: SAME 5 words (tail-JMP into 87cd)
  midi_parse_file(8809)        *DS:SI = file image (MThd/MTrk walk)
  midi_init_track_table(87a2)   0 args (loops CS:[0x85a1] tracks via BX=0x81cc+4i)
  midi_read_varlen(8891)       *DS:SI = event stream cursor
  midi_process_event(873c)     *DS:SI cursor, BX = per-track table entry ptr
  seq_normalize_far_ptr(8a23)   0 args -- a no-op stub in THIS binary (verified: RET)
  midi_get_track_count(8999)    0 args (pure getter)
  midi_sound_init(89a8)         0 args
  midi_start_playback(8722)     0 args (-> midi_install_tempo_timer)
  midi_install_tempo_timer(86e9) 0 args (reads CODE division/tempo, PIT-programs)
  seq_set_channel_param(922c)  *AL = channel nibble, DS:SI = 1 byte to store
  midi_emit_voice_msg_w1(8b81) *BX = channel(*12 scale), AH = note/vel byte;
                                reads DGROUP 0x5580/0x5582 (song_data far ptr) for
                                its own DS:SI-based instrument-table walk
  midi_emit_voice_msg_w2(8b6b) *AL = channel (-> BL), AH passthrough -> w1
  midi_emit_voice_msg_w3(8e93) *AL = channel nibble, DS:SI = 1 byte (-> AL) -> w2
  emit_midi_voice_message(8bc8)*AL=note idx, BX/DI=chan-struct ptr, CX/DX=params
  opl_event_note_on(8ea3)      *AL = channel nibble, DS:SI = 2 bytes (note,vel)
  opl_set_note_params(9241)     stack: [chan,note_param1,note_param2]; ALSO needs
                                DS:SI seeded (passes through to opl_event_note_on)
  opl_read_status(9056)         0 args (IN AL,0x388)
  opl2_reset_all_regs(8eeb)     0 args (hardcoded ~20-write OPL2 init program)
  maybe_opl2_detect_chip(8fb6)  0 args (hardcoded OPL2 chip-detect probe)

Output: local/build/render/midi_trace.bin, magic MIDTRC01, same per-scenario/per-
record framing as sound_trace.bin (fn_off, name_idx, entry snap, exit snap, n_io,
io events) with a parse_trace round-trip self-check.

Run (sandbox may need disabling if local/build/ is read-only under the harness):
  timeout 400 uv run python tools/midi_oracle.py
"""
from __future__ import annotations
import struct
import os
import collections
from typing import Dict, List, Optional, Tuple

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_CODE,
                     UC_HOOK_MEM_UNMAPPED, UC_HOOK_INSN, UcError)
from unicorn.x86_const import *

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_DIR = os.path.join(ROOT, "local/originals/old-games/bumpy")
EXE = os.path.join(ROOT, "local/originals/unpacked/BUMPY_unpacked.exe")
MID_PATH = os.path.join(GAME_DIR, "Bumpy.mid")
OUT_DIR = os.path.join(ROOT, "local/build/render")
OUT_TRACE = os.path.join(OUT_DIR, "midi_trace.bin")

PSP_SEG = 0x0100
RAM = 0x120000   # slightly larger than sound_oracle's — room for the MID scratch

# DGROUP / CODE runtime bases — identical formula to sound_oracle.py/physics_oracle.py.
DG_LIN: int = (0x103b + PSP_SEG + 0x10) * 16   # 0x114b0
CODE_LIN: int = (PSP_SEG + 0x10) * 16          # 0x1100

# ---------------------------------------------------------------------------
# MIDI DGROUP / CODE global offsets (from src/midi.h + src/sound.h, Task C1 report).
# ---------------------------------------------------------------------------
OFF_MIDI_SONG_DATA_OFF: int = 0x5580     # DGROUP u16 — song/aux far-ptr offset half
OFF_MIDI_SONG_DATA_SEG: int = 0x5582     # DGROUP u16 — segment half
OFF_MIDI_SEQ_STEP_ACTIVE: int = 0x557e   # DGROUP u16 — snddrv_init_substep flag
OFF_SOUND_ACTIVE_DEVICE_MASK: int = 0x5586   # DGROUP u16 — midi_play_sequence's guard

COFF_MIDI_AUX_PTR_OFF: int = 0x8485      # CODE u16
COFF_MIDI_AUX_PTR_SEG: int = 0x8487      # CODE u16
COFF_MIDI_DATA_SEG: int = 0x8483         # CODE u16 (a.k.a. "midi_load_flag", same cell)
COFF_MIDI_DIVISION: int = 0x85a3         # CODE u16 — MThd division/PPQN
COFF_MIDI_TEMPO_LO: int = 0x85a5         # CODE u16
COFF_MIDI_TEMPO_HI: int = 0x85a7         # CODE u8
COFF_MIDI_TRACK_COUNT: int = 0x85a1      # CODE s16 (sound.c-owned; SAME cell)

# Per-track tables: midi_track_ptr_table[16][2] (0x81cc..0x820c) + midi_track_time_
# table[16][2] (0x820c..0x824c), CONFIRMED via disassembly of midi_process_event's
# 'FF 20' (marker/default-channel) handler: `*(byte*)(in_BX+0x80) = *pbVar2` writes
# CS:[BX+0x80] where BX==0x81cc+4*trackidx during midi_init_track_table's loop — i.e.
# EXACTLY the byte immediately following midi_track_time_table's own extent
# (0x81cc+0xc0==0x828c). This is a genuinely NEW table this task discovered (not in
# Task C1's enumeration): a per-track "default MIDI channel" byte, one per 4-byte
# stride, immediately after the ptr+time tables. Captured here as part of the same
# contiguous 0xC0-byte (16 tracks * 3 tables * 4 B/track) block so the snapshot sees
# any of the three tables' state without needing a fourth named symbol.
COFF_MIDI_TRACK_TABLES: int = 0x81cc
MIDI_TRACK_TABLES_LEN: int = 0xC0        # 16 tracks * (ptr 4B + time 4B + chan 4B)

# seq_set_channel_param (922c) writes CS:[0x8473 + (AL&0xf)] — a 16-entry per-channel
# byte table ending exactly at midi_data_seg (0x8483), i.e. immediately BEFORE the
# song/aux staging cells above. Captured so scenario 5 (seq_set_channel_param) is
# observable in the snapshot diff.
COFF_MIDI_CHAN_PARAM_TABLE: int = 0x8473
MIDI_CHAN_PARAM_TABLE_LEN: int = 0x10

# ---------------------------------------------------------------------------
# FN_NAMES — the 21 capture targets (brief's exact list + addresses).
# ---------------------------------------------------------------------------
FN_NAMES: Dict[int, str] = {
    # Parser/sequencer (12) — no direct port I/O; L4 window NOT opened for these,
    # EXCEPT midi_install_tempo_timer (PIT-programming — see L4_FNS note below).
    0x8891: "midi_read_varlen",
    0x8809: "midi_parse_file",
    0x87a2: "midi_init_track_table",
    0x8999: "midi_get_track_count",
    0x8a23: "seq_normalize_far_ptr",
    0x922c: "seq_set_channel_param",
    0x873c: "midi_process_event",
    0x87cd: "midi_load_sequence",
    0x8722: "midi_start_playback",
    0x89a8: "midi_sound_init",
    0x8977: "midi_play_sequence",
    0x86e9: "midi_install_tempo_timer",
    # OPL2/emission (9) — all eventually reach opl_write_reg (0x388/0x389); L4.
    0x8ea3: "opl_event_note_on",
    0x9241: "opl_set_note_params",
    0x9056: "opl_read_status",
    0x8eeb: "opl2_reset_all_regs",
    0x8fb6: "maybe_opl2_detect_chip",
    0x8b81: "midi_emit_voice_msg_w1",
    0x8b6b: "midi_emit_voice_msg_w2",
    0x8e93: "midi_emit_voice_msg_w3",
    0x8bc8: "emit_midi_voice_message",
}

# L4_FNS — port-I/O capture window opened for these (the 9 OPL2/emission fns, whose
# execution provably reaches opl_write_reg's 0x388/0x389 OUT/IN pairs — verified via
# disassembly, not assumed) PLUS midi_install_tempo_timer (reaches set_timer_slot_raw,
# 1000:7df9, sound.c-owned/already-ported, which is documented to PIT-program the
# 8253 — the brief calls out "0x40-0x43 (PIT)... for the emission functions"). If no
# PIT port I/O is actually observed through this fn-boundary (e.g. because the real
# 8253 write lives in a still-deeper not-hooked leaf), that is reported honestly
# rather than fabricated — see the report's "Functions NOT meaningfully capturable"
# section.
L4_FNS: set = {
    0x8ea3, 0x9241, 0x9056, 0x8eeb, 0x8fb6, 0x8b81, 0x8b6b, 0x8e93, 0x8bc8,
    0x86e9,
}

# Sound-hardware port set: OPL2 (0x388/0x389), MPU-401 (0x330/0x331), PIT (0x40-0x43).
SOUND_PORTS: set = {0x388, 0x389, 0x330, 0x331, 0x40, 0x41, 0x42, 0x43}

# ---------------------------------------------------------------------------
# Scratch memory layout (no boot loop — see header). All addresses chosen to sit
# far outside the loaded image (image = 0x1a640 = 108096 B from CODE_LIN=0x1100,
# i.e. ends at 0x1bb40) and outside DGROUP/CODE-seg's own 64KB (DG_LIN=0x114b0).
# ---------------------------------------------------------------------------
MID_SCRATCH_LIN: int = 0x80000     # holds the REAL Bumpy.mid bytes
MID_SCRATCH_SEG: int = MID_SCRATCH_LIN // 16   # 0x8000

VOICE_SCRATCH_LIN: int = 0x90000   # controlled "song_data" struct for w1/w2/w3
VOICE_SCRATCH_SEG: int = VOICE_SCRATCH_LIN // 16   # 0x9000
VOICE_SCRATCH_LEN: int = 0x200

STACK_SEG: int = 0xA800            # dedicated stack, disjoint from everything above
STACK_SP: int = 0xFFFE

# DGROUP scratch offsets for register-entry SI inputs when DS defaults to DGROUP
# (call_near always forces DS=DS_SOUND/DGROUP for the OUTER call) — chosen far from
# every named DGROUP global (see src/sound.h / src/midi.h; nearest named symbols are
# below 0x6900 and the 0x5xxx OPL/timer tables — none within these ranges).
DG_SCRATCH_CHANPARAM_SI: int = 0x7ffe   # 1 byte  — seq_set_channel_param's *SI
DG_SCRATCH_NOTEON_SI: int = 0x7ff0      # 2 bytes — opl_event_note_on's *SI,*SI+1 (direct)
DG_SCRATCH_W3_SI: int = 0x7ffc          # 1 byte  — midi_emit_voice_msg_w3's *SI
DG_SCRATCH_EMITVOICE_BASE: int = 0x0080 # 0x20 bytes — emit_midi_voice_message-direct
                                         # BX=0,DI=0x80 chan-struct (DGROUP-relative,
                                         # since call_near forces DS=DGROUP by default)

# CODE-segment scratch for opl_event_note_on's SI when reached THROUGH
# opl_set_note_params (which explicitly sets DS=code_seg before its own tail call —
# see the ABI summary above: SI's segment meaning is CALLER-DEPENDENT). Chosen near
# the top of the 64KB code segment view, mirroring call_near's own LANDING_OFF
# (0xfffe) temporary-overwrite-then-restore convention.
CODE_SCRATCH_NOTEON_SI: int = 0xFFF0    # 2 bytes, far from LANDING_OFF (0xfffe)

# ---------------------------------------------------------------------------
# Scenarios: (id, name, description) — informational only; the real driving logic
# lives in run_scenarios() below (each scenario is a short, explicit Python
# function, not a declarative tuple, because the seeding is per-function-bespoke).
# ---------------------------------------------------------------------------
SCENARIO_NAMES: List[Tuple[int, str]] = [
    (1, "parser_real_mid_load_sequence"),
    (2, "get_track_count_and_sound_init"),
    (3, "parser_real_mid_play_sequence"),
    (4, "install_tempo_timer_seeded"),
    (5, "seq_set_channel_param_direct"),
    (6, "opl_self_contained"),
    (7, "voice_dispatch_seeded"),
]

TRACE_MAGIC: bytes = b"MIDTRC01"
TRACE_VERSION: int = 1

# ---------------------------------------------------------------------------
# MIDI_SNAP layout (little-endian) — mirrors SND_SNAP's "fixed struct, both entry
# and exit" convention from sound_oracle.py.
#
#   registers (captured at snapshot time — the brief's "register inputs: SI, AL,
#   the channel byte/BX" ask, generalised to the full register file so ANY of the
#   21 targets' inputs are visible without per-function special-casing):
#     u16 ax, bx, cx, dx, si, di, ds, es
#   MIDI sequencer-state globals (Task C1's enumeration):
#     u16 midi_song_data_off      DGROUP 0x5580
#     u16 midi_song_data_seg      DGROUP 0x5582
#     u16 midi_seq_step_active    DGROUP 0x557e
#     u16 midi_aux_ptr_off        CODE   0x8485
#     u16 midi_aux_ptr_seg        CODE   0x8487
#     u16 midi_data_seg           CODE   0x8483 (a.k.a. midi_load_flag, same cell)
#     u16 midi_division           CODE   0x85a3
#     u16 midi_tempo_lo           CODE   0x85a5
#     u8  midi_tempo_hi           CODE   0x85a7
#     u8  _pad
#     s16 midi_track_count        CODE   0x85a1
#   si_window: 32 B read at (DS:SI) at snapshot time — "a captured WINDOW of the
#     memory the function reads at SI" per the brief, so a host replay can point
#     its cursor at identical bytes.
#   track_tables: 0xC0 B read at CODE 0x81cc..0x828c (ptr+time+chan tables, 16
#     tracks * 3 tables * 4 B).
#   chan_param_table: 0x10 B read at CODE 0x8473..0x8483 (seq_set_channel_param's
#     per-channel byte table).
# ---------------------------------------------------------------------------
MIDI_SNAP_FMT: str = "<8H" "HHHHHHHHBBh" "32s" "%ds" % MIDI_TRACK_TABLES_LEN + "%ds" % MIDI_CHAN_PARAM_TABLE_LEN
MIDI_SNAP_SIZE: int = struct.calcsize(MIDI_SNAP_FMT)


def load_mz(path: str) -> Tuple[bytes, list, dict]:
    x = open(path, "rb").read()
    e_crlc, e_cparhdr = struct.unpack_from("<HH", x, 6)
    e_ss, e_sp, _chk, e_ip, e_cs = struct.unpack_from("<HHHHH", x, 0x0E)
    e_lfarlc = struct.unpack_from("<H", x, 0x18)[0]
    img = x[e_cparhdr * 16:]
    relocs = [struct.unpack_from("<HH", x, e_lfarlc + i * 4) for i in range(e_crlc)]
    return img, relocs, dict(ss=e_ss, sp=e_sp, ip=e_ip, cs=e_cs)


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

    tr: dict = dict(fault=None, exit=None, call_errs=[])

    def set_cf(set_it: bool) -> None:
        fl = uc.reg_read(UC_X86_REG_EFLAGS)
        uc.reg_write(UC_X86_REG_EFLAGS, (fl | 1) if set_it else (fl & ~1))

    # --- minimal safety-net interrupt handling (none of the 21 targets execute an
    # INT themselves — this is defensive only, mirroring sound_oracle's caution). ---
    def hook_intr(uc: Uc, intno: int, _: object) -> None:
        ax = uc.reg_read(UC_X86_REG_AX); ah = (ax >> 8) & 0xFF
        if intno == 0x21 and ah == 0x4C:
            tr["exit"] = ax & 0xFF
            uc.emu_stop()

    def hook_unmapped(uc: Uc, access: int, addr: int, size: int,
                      value: int, _: object) -> bool:
        tr["fault"] = (addr, access)
        uc.emu_stop()
        return False

    # --- port-I/O capture (OPL2 / MPU-401 / PIT) --------------------------------
    io_capture = {"depth": 0}
    io_seq: List[Tuple[int, int, int, int]] = []

    def hook_in(uc: Uc, port: int, size: int, _: object) -> int:
        if port == 0x388:
            val = 0x00     # OPL2 status: not-busy (poll loops exit promptly)
        elif port == 0x331:
            val = 0x00     # MPU-401 status: DSR clear => ready
        elif port in (0x389, 0x330):
            val = 0x00
        else:
            val = 0x00     # PIT 0x40-0x43 — fixed/deterministic (no real hardware)
        if io_capture["depth"] > 0 and port in SOUND_PORTS:
            io_seq.append((1, port, size, val & 0xFFFF))
        return val

    def hook_out(uc: Uc, port: int, size: int, value: int, _: object) -> None:
        value &= 0xFFFF
        if io_capture["depth"] > 0 and port in SOUND_PORTS:
            io_seq.append((0, port, size, value))

    uc.hook_add(UC_HOOK_INTR, hook_intr)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, hook_unmapped)
    uc.hook_add(UC_HOOK_INSN, hook_in, None, 1, 0, UC_X86_INS_IN)
    uc.hook_add(UC_HOOK_INSN, hook_out, None, 1, 0, UC_X86_INS_OUT)

    # iret stubs for uninitialised vectors (defensive only).
    uc.mem_write(0x500, b"\xCF")
    iret_vec = (0x0050 << 16) | 0x0000
    for v in range(0x100):
        if struct.unpack("<I", uc.mem_read(v * 4, 4))[0] == 0:
            uc.mem_write(v * 4, struct.pack("<I", iret_vec))

    dg = (0x103b + base) & 0xFFFF
    DS_SOUND = dg
    code_seg = base & 0xFFFF

    uc.reg_write(UC_X86_REG_DS, DS_SOUND)
    uc.reg_write(UC_X86_REG_ES, DS_SOUND)
    uc.reg_write(UC_X86_REG_SS, STACK_SEG)
    uc.reg_write(UC_X86_REG_SP, STACK_SP)
    uc.reg_write(UC_X86_REG_CS, code_seg)
    uc.reg_write(UC_X86_REG_IP, 0)

    print("[midi_oracle] image=%d B loaded at CODE_LIN=0x%x, DGROUP DG_LIN=0x%x" % (
        len(img), CODE_LIN, DG_LIN), flush=True)

    # --- load the REAL Bumpy.mid bytes into MID_SCRATCH -------------------------
    mid_bytes = open(MID_PATH, "rb").read()
    uc.mem_write(MID_SCRATCH_LIN, mid_bytes)
    print("[midi_oracle] loaded %s (%d B) at seg 0x%x" % (
        MID_PATH, len(mid_bytes), MID_SCRATCH_SEG), flush=True)

    # --- the VOICE_SCRATCH controlled "song_data" struct for w1/w2/w3 -----------
    voice_buf = bytearray(VOICE_SCRATCH_LEN)
    struct.pack_into("<H", voice_buf, 0x0c, 0x0040)   # instrument-table base offset
    struct.pack_into("<H", voice_buf, 0x10, 0x0080)   # 2nd-stage table base offset
    for chan in range(4):
        struct.pack_into("<H", voice_buf, 0x40 + chan * 12, 1)   # per-chan index=1
    uc.mem_write(VOICE_SCRATCH_LIN, bytes(voice_buf))

    # --- emit_midi_voice_message-direct DGROUP chan-struct (BX=0, DI=0x80) ------
    emitvoice_struct = bytes([0] + [ (i + 1) & 0xFF for i in range(0x1F) ])
    uc.mem_write(DG_LIN + DG_SCRATCH_EMITVOICE_BASE, emitvoice_struct)

    # --- register-entry SI scratch bytes -----------------------------------------
    uc.mem_write(DG_LIN + DG_SCRATCH_CHANPARAM_SI, bytes([0x2A]))
    uc.mem_write(DG_LIN + DG_SCRATCH_NOTEON_SI, bytes([60, 100]))     # note,velocity
    uc.mem_write(DG_LIN + DG_SCRATCH_W3_SI, bytes([72]))
    uc.mem_write(CODE_LIN + CODE_SCRATCH_NOTEON_SI, bytes([55, 90]))  # note,velocity

    # Snapshot the freshly-set-up (pre-scenario) machine so each scenario starts
    # from an identical, isolated baseline (no boot loop -> this is cheap).
    base_ram = bytes(uc.mem_read(0, RAM))
    base_ctx = uc.context_save()

    def restore_base() -> None:
        uc.mem_write(0, base_ram)
        uc.context_restore(base_ctx)
        tr["fault"] = None
        tr["exit"] = None

    # ---------------------------------------------------------------------------
    # call_near — IDENTICAL mechanism to sound_oracle.py's: a synthetic near-call
    # frame (push args, push a landing-pad return address, jump to fn_off, run
    # until the landing pad's HLT). Works uniformly for BOTH stack-arg AND
    # register-entry targets — for the latter, args=[] (no stack words) and the
    # caller seeds AX/BX/CX/DX/SI/DI via uc.reg_write() beforehand (call_near
    # itself only ever writes DS/CS/IP/SP, never clobbering caller-seeded regs).
    # ---------------------------------------------------------------------------
    LANDING_OFF = 0xfffe
    LANDING_LIN = CODE_LIN + LANDING_OFF
    STOP_LIN = LANDING_LIN + 1

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
        uc.reg_write(UC_X86_REG_DS, DS_SOUND)
        uc.reg_write(UC_X86_REG_CS, code_seg)
        uc.reg_write(UC_X86_REG_IP, fn_off)
        try:
            uc.emu_start(CODE_LIN + fn_off, STOP_LIN, count=20_000_000)
        except UcError as e:
            tr.setdefault("call_errs", []).append((fn_off, str(e)))
        finally:
            uc.mem_write(LANDING_LIN, saved)

    # ---------------------------------------------------------------------------
    # Snapshot helpers
    # ---------------------------------------------------------------------------
    def rd_u16_dg(off: int) -> int:
        return struct.unpack("<H", bytes(uc.mem_read(DG_LIN + off, 2)))[0]

    def rd_u16_code(off: int) -> int:
        return struct.unpack("<H", bytes(uc.mem_read(CODE_LIN + off, 2)))[0]

    def rd_u8_code(off: int) -> int:
        return uc.mem_read(CODE_LIN + off, 1)[0]

    def rd_s16_code(off: int) -> int:
        return struct.unpack("<h", bytes(uc.mem_read(CODE_LIN + off, 2)))[0]

    def snap() -> bytes:
        ax = uc.reg_read(UC_X86_REG_AX) & 0xFFFF
        bx = uc.reg_read(UC_X86_REG_BX) & 0xFFFF
        cx = uc.reg_read(UC_X86_REG_CX) & 0xFFFF
        dx = uc.reg_read(UC_X86_REG_DX) & 0xFFFF
        si = uc.reg_read(UC_X86_REG_SI) & 0xFFFF
        di = uc.reg_read(UC_X86_REG_DI) & 0xFFFF
        ds = uc.reg_read(UC_X86_REG_DS) & 0xFFFF
        es = uc.reg_read(UC_X86_REG_ES) & 0xFFFF
        try:
            si_window = bytes(uc.mem_read((ds * 16 + si) & 0xFFFFF, 32))
        except UcError:
            si_window = b"\x00" * 32
        track_tables = bytes(uc.mem_read(CODE_LIN + COFF_MIDI_TRACK_TABLES,
                                         MIDI_TRACK_TABLES_LEN))
        chan_param_table = bytes(uc.mem_read(CODE_LIN + COFF_MIDI_CHAN_PARAM_TABLE,
                                             MIDI_CHAN_PARAM_TABLE_LEN))
        return struct.pack(
            MIDI_SNAP_FMT,
            ax, bx, cx, dx, si, di, ds, es,
            rd_u16_dg(OFF_MIDI_SONG_DATA_OFF),
            rd_u16_dg(OFF_MIDI_SONG_DATA_SEG),
            rd_u16_dg(OFF_MIDI_SEQ_STEP_ACTIVE),
            rd_u16_code(COFF_MIDI_AUX_PTR_OFF),
            rd_u16_code(COFF_MIDI_AUX_PTR_SEG),
            rd_u16_code(COFF_MIDI_DATA_SEG),
            rd_u16_code(COFF_MIDI_DIVISION),
            rd_u16_code(COFF_MIDI_TEMPO_LO),
            rd_u8_code(COFF_MIDI_TEMPO_HI),
            0,
            rd_s16_code(COFF_MIDI_TRACK_COUNT),
            si_window,
            track_tables,
            chan_param_table)

    # ---------------------------------------------------------------------------
    # Function entry/exit hooks (identical mechanism to sound_oracle.py).
    # ---------------------------------------------------------------------------
    capturing = {"on": False}
    cur_records: List[bytes] = []
    reached: collections.Counter = collections.Counter()
    pending_exit: dict = {}
    exit_hook_lins: set = set()
    fn_name_list: List[str] = list(dict.fromkeys(FN_NAMES.values()))
    fn_name_idx = {n: i for i, n in enumerate(fn_name_list)}
    driver_io: Dict[int, List[Tuple[int, int, int, int]]] = {}

    def emit_record(fn_off: int, entry_snap: bytes, exit_snap: bytes,
                    io_events: List[Tuple[int, int, int, int]]) -> None:
        rec = struct.pack("<HH", fn_off, fn_name_idx[FN_NAMES[fn_off]])
        rec += entry_snap + exit_snap
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
        is_l4 = fn_off in L4_FNS
        if is_l4:
            if io_capture["depth"] == 0:
                io_seq.clear()
            io_capture["depth"] += 1
        ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc.mem_read(ss * 16 + sp, 2)))[0]
        ret_lin = (CODE_LIN + ret_off) & 0xFFFFF
        io_mark = len(io_seq) if is_l4 else 0
        pending_exit.setdefault(ret_lin, []).append((fn_off, entry_snap, is_l4, io_mark))
        if ret_lin not in exit_hook_lins:
            exit_hook_lins.add(ret_lin)
            uc.hook_add(UC_HOOK_CODE, hook_fn_exit, None, ret_lin, ret_lin)

    def hook_fn_exit(uc: Uc, addr: int, size: int, _: object) -> None:
        stack = pending_exit.get(addr)
        if not stack:
            return
        (fn_off, entry_snap, is_l4, io_mark) = stack.pop()
        exit_snap = snap()
        io_events: List[Tuple[int, int, int, int]] = []
        if is_l4:
            io_events = list(io_seq[io_mark:])
            io_capture["depth"] -= 1
            if io_capture["depth"] == 0:
                io_seq.clear()
            if fn_off not in driver_io and io_events:
                driver_io[fn_off] = io_events
        emit_record(fn_off, entry_snap, exit_snap, io_events)

    for off in FN_NAMES:
        lin = CODE_LIN + off
        uc.hook_add(UC_HOOK_CODE, hook_fn_entry, None, lin, lin)

    # ---------------------------------------------------------------------------
    # Scenario runner
    # ---------------------------------------------------------------------------
    def run(fn) -> List[bytes]:
        cur_records.clear()
        capturing["on"] = True
        fn()
        capturing["on"] = False
        return list(cur_records)

    def sc1_parser_load_sequence() -> None:
        # aux (1st stack arg, BP+4/BP+6) = the REAL Bumpy.mid far ptr -> this is
        # what midi_parse_file's DS:SI actually reads (see header's ABI note).
        # song_data (2nd stack arg) also points at the same real bytes (stored,
        # unused by the parse itself). flag(3rd)=0x1234 for visibility.
        call_near(0x87cd, [0, MID_SCRATCH_SEG, 0, MID_SCRATCH_SEG, 0x1234])

    def sc2_get_track_count_and_sound_init() -> None:
        call_near(0x8999, [])
        call_near(0x89a8, [])

    def sc3_parser_play_sequence() -> None:
        uc.mem_write(DG_LIN + OFF_SOUND_ACTIVE_DEVICE_MASK, struct.pack("<H", 1))
        call_near(0x8977, [0, MID_SCRATCH_SEG, 0, MID_SCRATCH_SEG, 0x5678])

    def sc4_install_tempo_timer_seeded() -> None:
        uc.mem_write(CODE_LIN + COFF_MIDI_DIVISION, struct.pack("<H", 0x60))
        uc.mem_write(CODE_LIN + COFF_MIDI_TEMPO_LO, struct.pack("<H", 0xA120))
        uc.mem_write(CODE_LIN + COFF_MIDI_TEMPO_HI, bytes([0x07]))
        call_near(0x86e9, [])

    def sc5_seq_set_channel_param_direct() -> None:
        uc.reg_write(UC_X86_REG_AL, 0x05)
        uc.reg_write(UC_X86_REG_SI, DG_SCRATCH_CHANPARAM_SI)
        call_near(0x922c, [])

    def sc6_opl_self_contained() -> None:
        call_near(0x9056, [])   # opl_read_status
        call_near(0x8eeb, [])   # opl2_reset_all_regs
        call_near(0x8fb6, [])   # maybe_opl2_detect_chip

    def sc7_voice_dispatch_seeded() -> None:
        # w1/w2/w3 + opl_set_note_params all transitively need midi_song_data_off/
        # _seg pointing at a valid (controlled) "song data" struct.
        uc.mem_write(DG_LIN + OFF_MIDI_SONG_DATA_OFF, struct.pack("<H", 0))
        uc.mem_write(DG_LIN + OFF_MIDI_SONG_DATA_SEG, struct.pack("<H", VOICE_SCRATCH_SEG))

        uc.reg_write(UC_X86_REG_BX, 0)
        uc.reg_write(UC_X86_REG_AX, 0x40)   # AH=note/vel byte (AL irrelevant to w1)
        call_near(0x8b81, [])               # midi_emit_voice_msg_w1(chan=0)

        uc.reg_write(UC_X86_REG_BX, 1)
        uc.reg_write(UC_X86_REG_AX, 0x50)
        call_near(0x8b81, [])               # midi_emit_voice_msg_w1(chan=1)

        uc.reg_write(UC_X86_REG_AX, 0x0060)   # AL=0 (chan), AH=note/vel byte 0x60
        call_near(0x8b6b, [])                 # midi_emit_voice_msg_w2(chan=0)

        uc.reg_write(UC_X86_REG_AX, 0x02)     # AL=channel nibble 2
        uc.reg_write(UC_X86_REG_SI, DG_SCRATCH_W3_SI)
        call_near(0x8e93, [])                 # midi_emit_voice_msg_w3(chan=2)

        uc.reg_write(UC_X86_REG_BX, 0)
        uc.reg_write(UC_X86_REG_DI, DG_SCRATCH_EMITVOICE_BASE)
        uc.reg_write(UC_X86_REG_AX, 60)        # AL = note index
        uc.reg_write(UC_X86_REG_CX, 0x40)
        uc.reg_write(UC_X86_REG_DX, 0x10)
        call_near(0x8bc8, [])                  # emit_midi_voice_message direct

        uc.reg_write(UC_X86_REG_AX, 3)          # AL = channel nibble 3
        uc.reg_write(UC_X86_REG_SI, DG_SCRATCH_NOTEON_SI)
        call_near(0x8ea3, [])                   # opl_event_note_on direct (DS=DGROUP)

        # opl_set_note_params(chan,note_param1,note_param2): internally sets
        # DS=code_seg before tail-calling opl_event_note_on -> SI must be a
        # CODE-relative offset for THIS call site (context-dependent ABI, see
        # header note).
        uc.reg_write(UC_X86_REG_SI, CODE_SCRATCH_NOTEON_SI)
        call_near(0x9241, [2, 55, 99])

    scenario_fns = [
        (1, "parser_real_mid_load_sequence", sc1_parser_load_sequence),
        (2, "get_track_count_and_sound_init", sc2_get_track_count_and_sound_init),
        (3, "parser_real_mid_play_sequence", sc3_parser_play_sequence),
        (4, "install_tempo_timer_seeded", sc4_install_tempo_timer_seeded),
        (5, "seq_set_channel_param_direct", sc5_seq_set_channel_param_direct),
        (6, "opl_self_contained", sc6_opl_self_contained),
        (7, "voice_dispatch_seeded", sc7_voice_dispatch_seeded),
    ]

    scenario_blobs: List[Tuple[int, str, List[bytes]]] = []
    for (sc_id, name, fn) in scenario_fns:
        restore_base()
        print("[midi_oracle] === scenario %d (%s) ===" % (sc_id, name), flush=True)
        recs = run(fn)
        n_io = sum(struct.unpack_from("<H", r, 4 + 2 * MIDI_SNAP_SIZE)[0] for r in recs)
        print("[midi_oracle]   %d records, %d total port-I/O events" % (len(recs), n_io),
              flush=True)
        if tr.get("fault"):
            print("[midi_oracle]   WARNING: mem fault during scenario: %s" % (tr["fault"],),
                  flush=True)
        scenario_blobs.append((sc_id, name, recs))

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
        for sc_id, name, recs in scenario_blobs:
            nb = name.encode("ascii")
            f.write(struct.pack("<B", sc_id))
            f.write(struct.pack("<B", len(nb))); f.write(nb)
            f.write(struct.pack("<B", 0xFF))    # no single per-scenario "seed device"
            f.write(struct.pack("<I", len(recs)))
            for r in recs:
                f.write(r)
    trace_bytes = os.path.getsize(OUT_TRACE)
    print("[midi_oracle] wrote %s (%d B)" % (OUT_TRACE, trace_bytes), flush=True)

    # ---------------------------------------------------------------------------
    # Round-trip parser self-check
    # ---------------------------------------------------------------------------
    def parse_trace(path: str) -> dict:
        data = open(path, "rb").read()
        assert data[:8] == TRACE_MAGIC, "bad magic: %r" % (data[:8],)
        ver, n_sc = struct.unpack_from("<HH", data, 8)
        assert ver == TRACE_VERSION, "bad version: %d" % ver
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
            _seed = data[o]; o += 1
            n_rec = struct.unpack_from("<I", data, o)[0]; o += 4
            recs = []
            for _r in range(n_rec):
                fn_off, name_idx = struct.unpack_from("<HH", data, o); o += 4
                ent = struct.unpack_from(MIDI_SNAP_FMT, data, o); o += MIDI_SNAP_SIZE
                ex = struct.unpack_from(MIDI_SNAP_FMT, data, o); o += MIDI_SNAP_SIZE
                n_io = struct.unpack_from("<H", data, o)[0]; o += 2
                ios = []
                for _i in range(n_io):
                    d, port, sz, val = struct.unpack_from("<BHBH", data, o); o += 6
                    ios.append((d, port, sz, val))
                recs.append(dict(fn_off=fn_off, fn=names[name_idx], ent=ent, ex=ex, io=ios))
            scenarios.append(dict(id=sid, name=nm, recs=recs))
        assert o == len(data), "trailing bytes: parsed %d of %d" % (o, len(data))
        return dict(names=names, scenarios=scenarios)

    parsed = parse_trace(OUT_TRACE)
    print("[midi_oracle] round-trip parse OK: %d scenarios, %d fn-names" % (
        len(parsed["scenarios"]), len(parsed["names"])), flush=True)

    # ---------------------------------------------------------------------------
    # Console summary — per-function record counts + concrete substance examples.
    # ---------------------------------------------------------------------------
    print("\n[midi_oracle] REACHED functions (fn_off, name, count):", flush=True)
    for off in sorted(FN_NAMES):
        print("   1000:%04x  %-28s x%d%s" % (
            off, FN_NAMES[off], reached[off], "  [L4]" if off in L4_FNS else ""),
            flush=True)
    not_reached = [off for off in FN_NAMES if reached[off] == 0]
    if not_reached:
        print("\n[midi_oracle] NOT reached (0 records — reported, not fabricated):",
              flush=True)
        for off in sorted(not_reached):
            print("   1000:%04x  %s" % (off, FN_NAMES[off]), flush=True)

    print("\n[midi_oracle] L4 drivers with captured port I/O (first occurrence):",
          flush=True)
    for off in sorted(driver_io):
        seq_io = driver_io[off]
        outs = [(p, v) for (d, p, sz, v) in seq_io if d == 0]
        print("   1000:%04x %-28s %d events, OUTs=%s%s" % (
            off, FN_NAMES[off], len(seq_io),
            ", ".join("0x%03x=0x%02x" % (p, v) for (p, v) in outs[:10]),
            " ..." if len(outs) > 10 else ""), flush=True)

    l4_no_io = sorted(o for o in L4_FNS if reached[o] and o not in driver_io)
    if l4_no_io:
        print("\n[midi_oracle] L4 fns reached but with ZERO port I/O observed "
              "(reported honestly, not fabricated):", flush=True)
        for off in l4_no_io:
            print("   1000:%04x  %s" % (off, FN_NAMES[off]), flush=True)

    # A concrete midi_read_varlen substance example: real MThd bytes decode.
    def find_first(fn_off: int):
        for sc in scenario_blobs:
            for r in sc[2]:
                if struct.unpack_from("<H", r, 0)[0] == fn_off:
                    return r
        return None

    rv = find_first(0x8891)
    if rv is not None:
        ent = struct.unpack_from(MIDI_SNAP_FMT, rv, 4)
        ex = struct.unpack_from(MIDI_SNAP_FMT, rv, 4 + MIDI_SNAP_SIZE)
        print("\n[midi_oracle] midi_read_varlen example: entry SI-window=%s "
              "-> exit AX:DX=%04x:%04x" % (
                  ent[-3].hex()[:16], ex[0], ex[3]), flush=True)

    if tr.get("call_errs"):
        print("\n[midi_oracle] call_near errors:", tr["call_errs"], flush=True)


if __name__ == "__main__":
    main()
