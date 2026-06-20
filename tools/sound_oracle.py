#!/usr/bin/env python3
"""sound_oracle.py — Phase-6 sound CAPTURE-AS-DISCOVERY harness (+ port-I/O capture).

Boots the real (unpacked) BUMPY.EXE under Unicorn — reusing the boot + DGROUP-read
scaffold of tools/physics_oracle.py / tools/anim_oracle.py (deliberately NOT
refactoring those) — drives sound-triggering scenarios, and captures the engine's
sound subsystem at the *sound-FUNCTION-CALL boundary* (entry + exit), PLUS — the
genuinely new piece — the engine's hardware PORT I/O (x86 OUT/IN) so a later task
can validate the L4 hardware drivers against the real port-write sequence.

The 5 sound layers (Ghidra seg 1000; runtime linear = CODE_LIN + off = 0x1100 + off):

  L1 dispatch  play_sound 6e11, play_sound_effect 6e30 (21-case effect->tone switch
               -> schedule_timer_callback_a/b; sound_device_state==4 -> raw 2-byte
               sample from table 0x27ae via FUN_8a07), play_action_sound 63be (per-
               device LUT 0x260e OPL / 0x263e std by p1_pending_action), play_contact
               _sound 640c, play_exit_sound 6305, play_pickup_sound 645d,
               play_event_sound_64c1, play_state_sound_79b9 647e.
  L2 device    sound_select_device 6de3, snddrv_init 88e5 (guard 0x557a, sets
               snddrv_mode=1), select_sound_device_from_mask 891e, snddrv_dispatch_a
               /b/c/d 85b5/85db/8600/8626, snd_busy_delay 872e.
  L3 tone      schedule_timer_callback_a/b/c 9488/9502/956d (fill param frame
               DAT_1000_9788..979a + install far cb 1000:9631/96c4), arm_timer_callback
               7f2b (table 0x5516), set_timer_slot 7de8, disable_timer_callback 7f65,
               get_timer_slot_field 7e3d, timer_restore 7fde.
  L4 hardware  pc_speaker_silence 9115 (in/out 0x61 low 2 bits + zero voice table
               0x83cc), speaker_gate_reset 9440, speaker_gate_strobe 9451,
               record_status_and_strobe_speaker 946e, mode-4 sample FUN_8a07/8ad0,
               OPL opl_write_reg 9007 (0x388/0x389) / opl_play_note 905d, MPU-401
               FUN_89e2 (0x330/0x331) / FUN_8e2f.
  L5 ISR       far cb 1000:9631 + 96c4 timer-callback tone-sequencer + PIT mux (raw,
               no Ghidra fn boundary; observed via its port 0x61 strobes).

PORT MAP recovered from the live decomp (L4):
  PC speaker : 0x61            (pc_speaker_silence MUST show out(0x61, low2 cleared))
  MPU-401    : 0x330 data / 0x331 status
  OPL/AdLib  : 0x388 addr / 0x389 data

KEY ADDRESS NUANCE: some sound state is in the CODE segment (seg 1000 — the param
frame DAT_1000_9788..979a, snddrv_mode DAT_1000_85b3, voice table 0x83cc) and some
is in DGROUP (seg 0x203b; sound_device_state at DGROUP 0x689c = Ghidra-linear
0x26c4c = runtime DG_LIN+0x689c). We read BOTH regions; the runtime bases CODE_LIN
and DG_LIN are reused verbatim from physics_oracle.py.

Outputs (BOTH gitignored — discovery; only this script is committed):
  local/build/render/sound_trace.bin   (frozen layout — see TRACE LAYOUT below)
  local/build/sound_model.md           (resolved addrs + tone table + state machine +
                                        per-driver port sequences)

TRACE LAYOUT (little-endian) — FROZEN; a later task parses this exactly:
  Header:
    +0x00  8 B   magic   b"SNDTRC01"
    +0x08  2 B   u16     version (=1)
    +0x0A  2 B   u16     n_scenarios
  Then a fn-name string table (so records carry a compact fn_name_idx):
    +..    2 B   u16     n_fn_names
    per name: u8 len, len bytes (ascii)
  Then, per scenario:
    u8        scenario_id
    u8        name_len,  name_len bytes (ascii scenario name)
    u8        seed_device   (the sound_device_state value seeded, or 0xFF=unseeded)
    u32       n_records
    then n_records records.

  Per RECORD (one sound-function call; carries BOTH entry and exit snapshots):
    u16   fn_off         (Ghidra seg-1000 offset of the hooked fn, e.g. 0x6e11)
    u16   fn_name_idx    (index into the fn-name string table)
    SNAP  entry          (SND_SNAP_SIZE-byte fixed struct, see SNAP below)
    SNAP  exit           (SND_SNAP_SIZE-byte fixed struct)
    u16   n_io           (# of port-I/O events captured during this call window;
                          0 for non-L4 fns — we only scope capture to L4 drivers)
    n_io * IO            each IO = (u8 dir 0=OUT 1=IN, u16 port, u8 size, u16 value)

  SND_SNAP (sound globals; little-endian):
    s16  sound_device_state   (DGROUP 0x689c)   — L1 dispatch selector
    u16  snddrv_mode          (CODE  0x85b3)    — L2/L4 backend selector (0/1/4)
    u16  sound_active_mask    (DGROUP 0x5586)
    u16  sound_init_state     (DGROUP 0x557a)
    u16  sound_mode           (DGROUP 0x683e)   — speaker-gate branch selector
    u8   p1_pending_action    (DGROUP 0x856f-ish; resolved below)  — action LUT index
    u8   prev_game_mode       (DGROUP 0x8552)
    u8   p1_contact_code      (DGROUP; resolved below)
    u8   _pad
    u16[N] param_frame        (CODE 0x9788..0x979a, the L3 tone param frame, 10 words)
    u16  timer_cb_off         (CODE 0x97a1)
    u16  timer_cb_seg         (CODE 0x979f)

Run (sandbox disabled — needs unicorn/uv cache access), HARD timeout:
  timeout 2400 uv run python tools/sound_oracle.py
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
OUT_TRACE = os.path.join(OUT_DIR, "sound_trace.bin")
OUT_MODEL = os.path.join(ROOT, "local/build/sound_model.md")

PSP_SEG = 0x0100
RAM = 0x110000

# DGROUP runtime base — identical formula to physics_oracle.py / game_oracle.py.
DG_LIN: int = (0x103b + PSP_SEG + 0x10) * 16   # 0x114b0
# Ghidra "segment 1000" == program load base (image offset 0); a fn at Ghidra
# 1000:off lives at raw image offset `off`, loaded at base*16 = 0x1100.
CODE_LIN: int = (PSP_SEG + 0x10) * 16          # 0x1100

# ---------------------------------------------------------------------------
# DGROUP global offsets (Ghidra DGROUP 0x203b offsets) — sound globals.
# Note: ram0x00026c4c (Ghidra-linear) == DGROUP 0x689c == runtime DG_LIN+0x689c.
# ---------------------------------------------------------------------------
OFF_SOUND_DEVICE_STATE: int = 0x689c   # s16  (L1 selector; ==4 -> OPL path; ==-0x8000 mutes)
OFF_SOUND_ACTIVE_MASK: int = 0x5586    # u16
OFF_SOUND_INIT_STATE: int = 0x557a     # u16
OFF_SOUND_MODE: int = 0x683e           # u16  (speaker-gate branch selector)
OFF_PREV_GAME_MODE: int = 0x8552       # u8
OFF_TIMER_CB_TABLE: int = 0x5516       # u16  (timer_callback_table; arm_timer_callback)

# Physics/control globals reused from physics_oracle.py for boot + input + LUT idx.
OFF_INPUT_STATE: int = 0x8244          # u8
OFF_GAME_MODE: int = 0x792c            # u8
OFF_P1_CELL: int = 0x856e              # u8
OFF_CURRENT_LEVEL: int = 0x79b2        # u8
OFF_COPYPROTECT: int = 0x119a          # s8 (DGROUP 0x119a = copyprotect_flag)
OFF_KEY_STATE_PTR: int = 0x4D42        # near ptr to g_key_state_table base

# LUT-index globals (read for the per-effect / per-action discovery).
# Concrete DGROUP offsets recovered from the disassembly of the dispatchers:
#   play_action_sound 63be:  MOV AL,[0x7924]  -> p1_pending_action  @ DGROUP 0x7924
#   play_contact_sound 640c: MOV AL,[0x8551]  -> p1_contact_code    @ DGROUP 0x8551
#   play_state_sound 647e:   MOV AL,[0x79b9]  -> tile_below_player  @ DGROUP 0x79b9
OFF_P1_PENDING_ACTION: int = 0x7924    # u8  (play_action_sound LUT index 0x260e/0x263e)
OFF_P1_CONTACT_CODE: int = 0x8551      # u8  (play_contact_sound LUT index 0x276e/0x278e)
OFF_TILE_BELOW_PLAYER: int = 0x79b9    # u8  (play_state_sound LUT index 0x26ce/0x26fe)

# CODE-segment (seg 1000) sound globals.
COFF_SNDDRV_MODE: int = 0x85b3         # u16  (DAT_1000_85b3 -> snddrv_mode)
COFF_PARAM_FRAME: int = 0x9788         # 10 words: 0x9788..0x979a (L3 tone param frame)
PARAM_FRAME_WORDS: int = 10            # 0x9788..0x979a inclusive (0x9788,8a,8c,8e,90,92,94,96,98 + byte 0x9a)
COFF_TIMER_CB_OFF: int = 0x97a1        # timer_callback_off
COFF_TIMER_CB_SEG: int = 0x979f        # timer_callback_seg
COFF_VOICE_TABLE: int = 0x83cc         # 15-byte voice table (cleared by pc_speaker_silence)

# ---------------------------------------------------------------------------
# Hooked sound functions (Ghidra seg-1000 offsets) by layer.
# ---------------------------------------------------------------------------
FN_NAMES: Dict[int, str] = {
    # L1 dispatch
    0x6e11: "play_sound",
    0x6e30: "play_sound_effect",
    0x63be: "play_action_sound",
    0x640c: "play_contact_sound",
    0x6305: "play_exit_sound",
    0x645d: "play_pickup_sound",
    0x64c1: "play_event_sound_64c1",
    0x647e: "play_state_sound_79b9",
    # L2 device
    0x6de3: "sound_select_device",
    0x88e5: "snddrv_init",
    0x891e: "select_sound_device_from_mask",
    0x85b5: "snddrv_dispatch_a",
    0x85db: "snddrv_dispatch_b",
    0x8600: "snddrv_dispatch_c",
    0x8626: "snddrv_dispatch_d",
    0x872e: "snd_busy_delay",
    # L3 tone-submit
    0x9488: "schedule_timer_callback_a",
    0x9502: "schedule_timer_callback_b",
    0x956d: "schedule_timer_callback_c",
    0x7f2b: "arm_timer_callback",
    0x7de8: "set_timer_slot",
    0x7f65: "disable_timer_callback",
    0x7e3d: "get_timer_slot_field",
    0x7fde: "timer_restore",
    # L4 hardware (port-I/O scoped to these)
    0x9115: "pc_speaker_silence",
    0x9440: "speaker_gate_reset",
    0x9451: "speaker_gate_strobe",
    0x946e: "record_status_and_strobe_speaker",
    0x8a07: "FUN_8a07_mpu_sample",
    0x8ad0: "FUN_8ad0_mpu_settle",
    0x8e2f: "FUN_8e2f_opl_allnotesoff",
    0x89e2: "FUN_89e2_mpu_io",
    0x9007: "opl_write_reg",
    0x905d: "opl_play_note",
}

# L4 driver fns — the ones whose execution window we scope port-I/O capture to.
L4_FNS: set = {
    0x9115, 0x9440, 0x9451, 0x946e, 0x8a07, 0x8ad0, 0x8e2f, 0x89e2, 0x9007, 0x905d,
}

# Layer membership (for the model md).
FN_LAYER: Dict[int, str] = {}
for _o in (0x6e11, 0x6e30, 0x63be, 0x640c, 0x6305, 0x645d, 0x64c1, 0x647e):
    FN_LAYER[_o] = "L1"
for _o in (0x6de3, 0x88e5, 0x891e, 0x85b5, 0x85db, 0x8600, 0x8626, 0x872e):
    FN_LAYER[_o] = "L2"
for _o in (0x9488, 0x9502, 0x956d, 0x7f2b, 0x7de8, 0x7f65, 0x7e3d, 0x7fde):
    FN_LAYER[_o] = "L3"
for _o in L4_FNS:
    FN_LAYER[_o] = "L4"

# Sound-hardware port set (for filtering the I/O-capture noise from VGA/PIT/keyboard).
SOUND_PORTS: set = {0x61, 0x330, 0x331, 0x388, 0x389}

# Synthesized port-0x61 IN value. There is NO PC-speaker/system-control hardware
# under Unicorn, so any IN AL,0x61 must return a FIXED, run-to-run-deterministic byte
# rather than emulation state. We use 0xFF (matching tools/physics_oracle.py's 0x61
# read). What is validated is NOT this absolute byte but the engine's bit manipulation
# over the replayed IN sequence: e.g. pc_speaker_silence does `IN AL,0x61; AND AL,0xfc;
# OUT 0x61,AL`, so the captured OUT(0x61) == SYNTH_PORT61_IN & 0xfc == 0xFC and is
# stable. A later L4 ctest replays the recorded IN sequence (each IN event carries the
# value the hook returned) so the reconstructed driver sees the same inputs the engine
# saw, then asserts the OUT writes. See local/build/sound_model.md.
SYNTH_PORT61_IN: int = 0xFF

# ---------------------------------------------------------------------------
# Per-effect tone-param table — recovered verbatim from play_sound_effect (6e30).
# schedule_timer_callback_a(2, tone_arg2, tone_arg3, 1, uVar1, uVar2, uVar3, uVar4)
# Tuple = (arg2, arg3, uVar1, uVar2, uVar3, uVar4) with the "_a"/"_b" submit form.
# Documented in sound_model.md; here only for the report.
# ---------------------------------------------------------------------------
EFFECT_TONE_TABLE: Dict[int, str] = {
    0x01: "a(2,0x1e,1000,1,10,0x1c2,1,1)",
    0x02: "a(2,0x28,800,1,0xfff6,0x1c2,1,1)",
    0x03: "a(2,400,0x1b8,1,0xffff,499,4,0xffff)",
    0x04: "a(2,0x5a,0xdc,1,0xffff,100,1,4)",
    0x05: "a(2,0x19,1000,1,10,0x1b8,1,2)",
    0x06: "a(2,0x14,0x44c,1,10,0x1b8,2,5)",
    0x07: "a(2,0xf,0x4b0,1,10,0x1b8,1,3)",
    0x08: "a(2,0x28,0xdc,1,0xfffb,100,1,5)",
    0x09: "a(2,0x1e,0x32,1,0x14,0x1c2,1,1)",
    0x0a: "a(2,0xf,200,1,0x32,0x15d,1,10)",
    0x0b: "b(2,0x28,0x14,499,1,0xfffc)",
    0x0c: "a(2,0x1e,0x4b0,1,10,0x1a4,1,2)",
    0x0d: "a(2,0x14,200,1,0x32,0x15d,2,0xf)",
    0x0e: "a(2,0x32,10,1,4,200,10,0)",
    0x0f: "a(2,400,300,1,2,100,2,1)",
    0x10: "a(2,0x1e,0x4b0,1,10,0x1a4,1,2)",
    0x11: "b(2,0x28,0x14,499,1,0xfffc)",
    0x12: "b(2,0x50,0x1e,499,2,0xfffc)",
    0x13: "a(2,800,300,1,1,100,2,1)",
    0x14: "a(2,0x32,10,1,4,200,10,0)",
    0x15: "a(2,0x1e,600,1,10,0x1c2,1,1)",
}

# ---------------------------------------------------------------------------
# Trace format constants
# ---------------------------------------------------------------------------
TRACE_MAGIC: bytes = b"SNDTRC01"
# version 2 (Phase-6 T4): the SND_SNAP gains the two L3 timer tables (the 0x549c slot
# table the set_timer_slot/get_timer_slot_field path writes/reads, and the 0x5516 cb
# table arm_timer_callback/disable_timer_callback install into) so the L3 timer-table
# install/disable is observable in the semantic-state differential.  v1 had only the
# device/driver scalars + the tone param frame + the timer-cb far ptr.
TRACE_VERSION: int = 2
# L3 timer tables captured into the SND_SNAP (Phase-6 T4).
OFF_TIMER_SLOT_TABLE: int = 0x549c    # DGROUP — set_timer_slot_raw / get_timer_slot_field
TIMER_SLOT_TABLE_LEN: int = 0x40      # covers channels 0..3 at (idx+2)*8+0x549c
TIMER_CB_TABLE_LEN: int = 0x20        # 0x5516 — channels 0..3 at channel*8 (8 B each)
# SND_SNAP: 5 words + 4 bytes + 10 param-frame words + 2 timer-cb words
#           + the 0x549c slot table (0x40 B) + the 0x5516 cb table (0x20 B).
SND_SNAP_FMT: str = ("<hHHHHBBBB" + "H" * PARAM_FRAME_WORDS + "HH"
                     + "%dB" % TIMER_SLOT_TABLE_LEN + "%dB" % TIMER_CB_TABLE_LEN)
SND_SNAP_SIZE: int = struct.calcsize(SND_SNAP_FMT)

# ---------------------------------------------------------------------------
# Scancode / input_state mapping (from physics_oracle.py)
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
# Scenarios. Each: (id, name, seed_device, tick_script, direct_calls).
#   seed_device: sound_device_state value to force before the run (None=leave as is).
#                Use 0 = PC-speaker dispatch path; 4 = OPL path (raw sample / 0x260e LUT).
#   tick_script: list of (n_ticks, input_state, scancode) — gameplay that may trigger
#                play_sound via the move/land/collect paths.
#   direct_calls: list of (fn_off, [args]) — synchronous DIRECT invocations of an L1/L2
#                 entry point, executed in a sub-machine, to deterministically exercise
#                 a layer the gameplay may not reach (so every layer is observed and the
#                 L4 port-write sequences are captured). Documented per scenario.
# ---------------------------------------------------------------------------
Scenario = Tuple[int, str, Optional[int],
                 List[Tuple[int, int, int]],
                 List[Tuple[int, List[int]]]]

SCENARIOS: List[Scenario] = [
    # 1: gameplay move/land on PC-speaker device — the player land/move path calls
    #    play_sound / play_action_sound naturally.
    (1, "gameplay_pcspk", 0, [
        (6,  IS_IDLE,  0),
        (4,  IS_JUMP,  SC_JUMP),
        (24, IS_IDLE,  0),
        (12, IS_RIGHT, SC_RIGHT),
        (6,  IS_IDLE,  0),
    ], []),
    # 2: device init/select path — directly drive sound_select_device which runs
    #    snddrv_init -> select_sound_device_from_mask (L2). Seed unset so the init
    #    guard (0x557a==0) fires.
    (2, "device_init", None, [], [
        (0x6de3, []),     # sound_select_device()
    ]),
    # 3: PC-speaker effect sweep — seed device 0 (non-OPL), directly play each effect
    #    id 1..0x15 via play_sound; exercises L1 switch -> L3 schedule_* -> L4
    #    speaker_gate_reset (port 0x61) and the silence path.
    (3, "pcspk_effects", 0,
     [], [(0x6e11, [eid]) for eid in range(0x01, 0x16)] + [
         (0x9115, []),    # pc_speaker_silence() — MUST emit out(0x61, low2 cleared)
     ]),
    # 4: OPL/AdLib path — seed device 4, play effects so the raw-sample / OPL branch
    #    (FUN_8a07 / opl_play_note 0x388/0x389) runs; plus direct opl_play_note + the
    #    OPL all-notes-off + a direct opl_write_reg to capture the 0x388/0x389 sequence.
    (4, "opl_path", 4,
     [], [(0x6e11, [eid]) for eid in (0x01, 0x0b, 0x21, 0x2c)] + [
         (0x8e2f, []),         # OPL all-notes-off (9 voices)
         (0x9007, []),         # opl_write_reg (raw — captures 0x388/0x389 + delays)
     ]),
    # 5: hardware-driver direct sweep on PC-speaker — directly call each L4 speaker
    #    driver to capture its standalone port-0x61 sequence regardless of dispatch.
    (5, "l4_drivers", 0, [], [
        (0x9115, []),    # pc_speaker_silence
        (0x9440, []),    # speaker_gate_reset
        (0x9451, []),    # speaker_gate_strobe
        (0x946e, []),    # record_status_and_strobe_speaker
    ]),
]

# ---------------------------------------------------------------------------
# Phase-6 T4 SEEDING scenarios.  Several T4-scoped fns are NOT reached by scenarios
# 1–5 (the play_exit/pickup/contact/event L1 wrappers; the L2 snddrv_dispatch_a/b/c/d +
# snd_busy_delay; the L3 timer helpers set_timer_slot/arm_timer_callback/
# disable_timer_callback/get_timer_slot_field/timer_restore).  To validate them under the
# semantic-state differential we SEED each via the existing call_near() direct-invocation
# harness with a chosen entry state, exactly as Phase-4 T4 extended p2_oracle.  Documented
# in local/build/sound_model.md.
#
# A direct call here is a 3-tuple (fn_off, args, preseed) where preseed is a list of
# (space, size, off, value): space 'dg' (DGROUP off) or 'code' (CODE seg off, e.g.
# snddrv_mode @ CODE 0x85b3); size 1 or 2 bytes.  Seeds are applied right before the call.
#
# arm_timer_callback (7f2b) + set_timer_slot (7de8) take a FAR callback pushed as two
# stack words (off, then seg).  We push a CODE-segment far cb (off=0x9631, seg=code_seg —
# the live runtime CODE base) so the captured cb_seg is the relocated base the comparator
# already accounts for via seg_to_trace.
#   call_near pushes args in REVERSE, so [arg0, arg1, off, seg] puts arg0 at [BP+4].
#   set_timer_slot(channel, value, cb_off, cb_seg):  [BP+4]=channel [BP+6]=value [BP+8..]=far cb
#   arm_timer_callback(channel, reload, cb_off, cb_seg): same stack shape.
# (cb seg is filled at run time from code_seg in run_seed_scenario, see the SEED marker.)
# ---------------------------------------------------------------------------
SEED_FAR_CB_OFF = 0x9631   # a real CODE-seg far-cb offset (the schedule_a callback)

T4_SEED_SCENARIOS = [
    # 6: L1 event wrappers not reached by gameplay.  Seed device + the index global, then
    #    direct-call.  std device (0) and OPL device (4) both, so both LUT halves run.
    (6, "t4_l1_wrappers", [
        # play_exit_sound (6305): device-only (id 3 std / 0xd OPL).
        (0x6305, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 0)]),
        (0x6305, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 4)]),
        # play_pickup_sound (645d): id 0xb std / 0x2c OPL.
        (0x645d, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 0)]),
        (0x645d, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 4)]),
        # play_event_sound_64c1: id 0xe std / 0x21 OPL.
        (0x64c1, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 0)]),
        (0x64c1, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 4)]),
        # play_contact_sound (640c): LUT[p1_contact_code]; pick code 1 (std->0xc), code 8
        #   (std->0xe, also runs FUN_6183 — stubbed), and code 0 (->0, no play).
        (0x640c, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 0), ('dg', 1, OFF_P1_CONTACT_CODE, 1)]),
        (0x640c, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 0), ('dg', 1, OFF_P1_CONTACT_CODE, 8)]),
        (0x640c, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 4), ('dg', 1, OFF_P1_CONTACT_CODE, 1)]),
        (0x640c, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 0), ('dg', 1, OFF_P1_CONTACT_CODE, 0)]),
        # play_state_sound_79b9 (647e): LUT[tile_below_player] -> play_sound, then the
        #   player tail p1_try_trigger_pending_action.  Seed input_state=0 so that tail is
        #   INERT (its `input_state & 0x11` guard fails -> no anim/nested sound), isolating
        #   the LUT->play_sound cascade for the differential.  Cover std + OPL, tbp 1 & 7
        #   (std LUT -> 0xb / 0x9) and tbp 0 (-> 0, no play).
        (0x647e, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 0), ('dg', 1, OFF_TILE_BELOW_PLAYER, 1),
                      ('dg', 1, OFF_INPUT_STATE, 0)]),
        (0x647e, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 0), ('dg', 1, OFF_TILE_BELOW_PLAYER, 7),
                      ('dg', 1, OFF_INPUT_STATE, 0)]),
        (0x647e, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 4), ('dg', 1, OFF_TILE_BELOW_PLAYER, 1),
                      ('dg', 1, OFF_INPUT_STATE, 0)]),
        (0x647e, [], [('dg', 2, OFF_SOUND_DEVICE_STATE, 0), ('dg', 1, OFF_TILE_BELOW_PLAYER, 0),
                      ('dg', 1, OFF_INPUT_STATE, 0)]),
    ]),
    # 7: L2 dispatch fan-out + snd_busy_delay.  Seed snddrv_mode (CODE 0x85b3) to each of
    #    the 3 backend selectors (0/1/4); the backends are STUBBED so no port-I/O, and the
    #    validated state (snddrv_mode etc.) is unchanged by the dispatch.
    #    NOTE: the L4 backends loop over real hardware (FUN_8ad0 settle / FUN_8e2f OPL
    #    all-notes-off run thousands of nested L4 calls).  Those nested L4 records are
    #    UNPORTED (validated never) and only bloat the trace, so we seed each dispatch
    #    fan-out with mode=0 (the cheapest backend, pc_speaker_silence ~2 events) which
    #    fully validates the dispatch record's semantic state (unchanged by any branch),
    #    plus dispatch_a once at mode=1/mode=4 for port branch coverage.
    (7, "t4_l2_dispatch", [
        (0x85b5, [], [('code', 2, COFF_SNDDRV_MODE, 0)]),   # dispatch_a -> pc_speaker_silence
        (0x85b5, [], [('code', 2, COFF_SNDDRV_MODE, 1)]),   # dispatch_a -> FUN_8e2f (coverage)
        (0x85b5, [], [('code', 2, COFF_SNDDRV_MODE, 4)]),   # dispatch_a -> FUN_8ad0 (coverage)
        (0x85db, [], [('code', 2, COFF_SNDDRV_MODE, 0)]),   # dispatch_b
        (0x8600, [], [('code', 2, COFF_SNDDRV_MODE, 0)]),   # dispatch_c
        (0x8626, [], [('code', 2, COFF_SNDDRV_MODE, 0)]),   # dispatch_d
        # snd_busy_delay (872e): naked asm, count=CX (seeded small in run_seed_scenario).
        #   Its FUN_89e2 callee is STUBBED on the host (no port-I/O) and it mutates no
        #   validated state — captured as a no-op-on-state record.
        (0x872e, [], []),
    ]),
    # 8: L3 timer-table mgmt — arm/disable on the 0x5516 cb table, set/get on the 0x549c
    #    slot table, timer_restore (-> FUN_7fef stub).  These write/read the timer tables
    #    now captured in the SND_SNAP.  Far cb pushed as (off, seg=code_seg) — SEED marker.
    (8, "t4_l3_timer_table", [
        # arm_timer_callback(channel=1, reload=0x1f4, cb=SEED_FAR_CB_OFF:code_seg)
        (0x7f2b, [1, 0x1f4, SEED_FAR_CB_OFF, "CODE_SEG"], []),
        (0x7f2b, [3, 0x10,  SEED_FAR_CB_OFF, "CODE_SEG"], []),
        # disable_timer_callback(channel=1) -> 0xffff in the cb table.
        (0x7f65, [1], []),
        # set_timer_slot(channel=2, value=0x100, cb=SEED_FAR_CB_OFF:code_seg)
        (0x7de8, [2, 0x100, SEED_FAR_CB_OFF, "CODE_SEG"], []),
        # set_timer_slot(channel=0, value=0x1f4 (=500, OUT OF RANGE) -> returns 0, no write)
        (0x7de8, [0, 0x1f4, SEED_FAR_CB_OFF, "CODE_SEG"], []),
        # get_timer_slot_field(slot_index=2) -> read back the value set above (return only).
        (0x7e3d, [2], []),
        # timer_restore (7fde) -> FUN_7fef stub (no state change).
        (0x7fde, [], []),
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

    # --- port-I/O capture state ----------------------------------------------------
    # io_capture["on"]: how many L4 windows are currently nested-active (>0 => record).
    # io_capture["seq"]: the (dir, port, size, value) list being filled for the
    # innermost-or-outermost L4 window (we attribute all I/O to the active record(s)).
    io_capture = {"depth": 0}
    io_seq: List[Tuple[int, int, int, int]] = []   # current capture buffer

    def hook_in(uc: Uc, port: int, size: int, _: object) -> int:
        io[0] += 1
        # default IN values (status polls etc.)
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
            # PC-speaker / system-control read: FIXED synthesized value (no hardware
            # under Unicorn). MUST NOT use the global IN-counter io[0] — its upper bits
            # would leak host-counter noise into the captured OUT(0x61) (e.g. via
            # pc_speaker_silence's `IN; AND 0xfc; OUT`), making the byte non-reproducible
            # run-to-run. The validated invariant is the engine's bit manipulation over
            # the (now deterministic) replayed IN sequence, not the absolute byte.
            val = SYNTH_PORT61_IN
        elif port == 0x331:
            # MPU-401 status: DSR bit 0x40 CLEAR => "ready" so the poll loop exits and
            # the driver proceeds to write the data port (capturing the OUT we want).
            val = 0x00
        elif port == 0x388:
            # OPL status: not-busy.
            val = 0x00
        else:
            val = 0xFF
        if io_capture["depth"] > 0 and port in SOUND_PORTS:
            # Record the IN event WITH the value the hook returned (dir=1), in order with
            # the OUT events. A later L4 ctest's host in() shim replays this EXACT IN
            # sequence so the reconstructed driver sees the same inputs the engine saw,
            # then asserts the OUT writes. For 0x61 `val` is the deterministic
            # SYNTH_PORT61_IN; for 0x331/0x388 it is the fixed poll-exit constant.
            io_seq.append((1, port, size, val & 0xFFFF))
        return val

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
        # capture sound-hardware OUTs inside an active L4 window
        if io_capture["depth"] > 0 and port in SOUND_PORTS:
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
    DS_SOUND = dg   # DGROUP segment value used when calling sound fns directly

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

    def rd_u16(off: int) -> int:
        return struct.unpack("<H", bytes(uc.mem_read(DG_LIN + off, 2)))[0]

    def rd_s16(off: int) -> int:
        return struct.unpack("<h", bytes(uc.mem_read(DG_LIN + off, 2)))[0]

    def crd8(off: int) -> int:
        return uc.mem_read(CODE_LIN + off, 1)[0]

    def crd_u16(off: int) -> int:
        return struct.unpack("<H", bytes(uc.mem_read(CODE_LIN + off, 2)))[0]

    def dg_block(off: int, n: int) -> bytes:
        return bytes(uc.mem_read(DG_LIN + off, n))

    def snap() -> bytes:
        # CODE-segment param frame: 9 words (0x9788..0x9798) + the byte 0x979a as a word.
        frame = []
        for i in range(PARAM_FRAME_WORDS - 1):
            frame.append(crd_u16(COFF_PARAM_FRAME + i * 2))
        frame.append(crd8(COFF_PARAM_FRAME + 0x12))   # 0x979a (single byte -> low byte)
        # L3 timer tables (DGROUP): the 0x549c slot table + the 0x5516 cb table (T4).
        slot_tbl = dg_block(OFF_TIMER_SLOT_TABLE, TIMER_SLOT_TABLE_LEN)
        cb_tbl = dg_block(OFF_TIMER_CB_TABLE, TIMER_CB_TABLE_LEN)
        return struct.pack(
            SND_SNAP_FMT,
            rd_s16(OFF_SOUND_DEVICE_STATE),
            crd_u16(COFF_SNDDRV_MODE),
            rd_u16(OFF_SOUND_ACTIVE_MASK),
            rd_u16(OFF_SOUND_INIT_STATE),
            rd_u16(OFF_SOUND_MODE),
            rd8(OFF_P1_PENDING_ACTION),
            rd8(OFF_PREV_GAME_MODE),
            rd8(OFF_P1_CONTACT_CODE),
            rd8(OFF_TILE_BELOW_PLAYER),    # the former 'pad' byte now carries the
                                           # play_state_sound_79b9 LUT index (T4).
            *frame,
            crd_u16(COFF_TIMER_CB_OFF),
            crd_u16(COFF_TIMER_CB_SEG),
            *slot_tbl,
            *cb_tbl)

    # ---------------------------------------------------------------------------
    # Sound-function hooks (entry + exit via dynamic return-address hook)
    # ---------------------------------------------------------------------------
    capturing = {"on": False}
    cur_records: List[bytes] = []
    reached: collections.Counter = collections.Counter()  # fn_off -> count
    pending_exit: dict = {}   # ret_lin -> (fn_off, entry_snap, is_l4)
    exit_hook_lins: set = set()
    fn_name_list: List[str] = list(dict.fromkeys(FN_NAMES.values()))
    fn_name_idx = {n: i for i, n in enumerate(fn_name_list)}
    # per-driver aggregate port-write sequences for the model md (first seen per fn).
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
            # Open a port-I/O capture window. Nested L4 calls (e.g. opl_play_note ->
            # opl_write_reg) keep depth>0 so inner I/O is captured; we attribute the
            # full window's I/O to the OUTERMOST L4 record via a per-record buffer mark.
            if io_capture["depth"] == 0:
                io_seq.clear()
            io_capture["depth"] += 1
        # near return address on top of stack (PUSH BP not yet executed at entry).
        ss = uc.reg_read(UC_X86_REG_SS); sp = uc.reg_read(UC_X86_REG_SP)
        ret_off = struct.unpack("<H", bytes(uc.mem_read(ss * 16 + sp, 2)))[0]
        ret_lin = (CODE_LIN + ret_off) & 0xFFFFF
        # io_mark: index into io_seq at entry, so this record's I/O = io_seq[mark:].
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
    # Boot to level 1 (identical approach to physics_oracle.py)
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

    print("[sound_oracle] booting BUMPY (level %d)..." % LEVEL, flush=True)

    while total_instr < 400_000_000:
        try:
            uc.emu_start(begin, 0, count=CHUNK)
        except UcError as e:
            err = str(e); tr["err"] = err; break
        total_instr += CHUNK
        if total_instr % 40_000_000 == 0:
            print("[sound_oracle] heartbeat: %dM instr, countdown=%s" % (
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
            print("[sound_oracle] level loaded (%s) at chunk %d — settling %d ticks" % (
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
        print("[sound_oracle] ERROR: premature exit/fault during boot: exit=%s fault=%s err=%s" % (
            tr.get("exit"), tr.get("fault"), err), flush=True)
        return
    if not opened(BUMNAME):
        print("[sound_oracle] ERROR: level %s never loaded after %dM instructions" % (
            BUMNAME, total_instr // 1_000_000), flush=True)
        return

    print("[sound_oracle] boot complete. Files: %s" % (
        [o[1] for o in tr.get("fileops", [])]), flush=True)

    # Snapshot clean post-boot machine so each scenario starts fresh.
    boot_ram = bytes(uc.mem_read(0, RAM))
    boot_ctx = uc.context_save()

    def restore_boot_state() -> None:
        uc.mem_write(0, boot_ram)
        uc.context_restore(boot_ctx)
        tr["fault"] = None
        tr["exit"] = None

    # ---------------------------------------------------------------------------
    # Direct synchronous call of a near sound fn (cdecl16near, DS=DGROUP).
    # Builds a tiny call frame on the current SS:SP and pushes a NEAR return address
    # into a small landing pad in the CODE segment: a NOP (where the fn returns — so
    # the fn-exit code-hook installed at this addr fires) immediately followed by a
    # HLT we stop emulation on via `until`. The two-byte pad is required: if the ret
    # address == the `until` stop address, Unicorn halts BEFORE running the exit hook
    # there and the top-level direct call's exit record would be lost.
    # ---------------------------------------------------------------------------
    # The whole 0..0xFFFF near range of the code segment overlaps the loaded image,
    # so we cannot carve a permanent landing pad. Instead we pick a near offset at the
    # very top of the segment (0xfffe), temporarily overwrite its 2 bytes with NOP;HLT
    # for the duration of one direct call, then restore the original game bytes. The
    # fn's RET lands on the NOP (firing its exit code-hook), then HLT halts on `until`.
    code_seg = base & 0xFFFF                 # CS value whose *16 == CODE_LIN
    LANDING_OFF = 0xfffe
    LANDING_LIN = CODE_LIN + LANDING_OFF      # ret addr: NOP here (exit hook fires)
    STOP_LIN = LANDING_LIN + 1                # `until` stop at the HLT

    def call_near(fn_off: int, args: List[int], cx: Optional[int] = None) -> None:
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
        if cx is not None:
            # snd_busy_delay (872e) is a naked asm routine: its delay count is CX.  Seed it
            # SMALL so the FUN_89e2 busy loop does not run 64K iterations (trace bloat).
            uc.reg_write(UC_X86_REG_CX, cx & 0xFFFF)
        try:
            uc.emu_start(CODE_LIN + fn_off, STOP_LIN, count=20_000_000)
        except UcError as e:
            tr.setdefault("call_errs", []).append((fn_off, str(e)))
        finally:
            uc.mem_write(LANDING_LIN, saved)

    # ---------------------------------------------------------------------------
    # Run scenarios
    # ---------------------------------------------------------------------------
    def run_scenario(sc: Scenario) -> List[bytes]:
        sc_id, name, seed_dev, ticks, direct = sc
        cur_records.clear()
        if seed_dev is not None:
            uc.mem_write(DG_LIN + OFF_SOUND_DEVICE_STATE, struct.pack("<h", seed_dev))
        capturing["on"] = True
        # gameplay ticks
        flat: List[Tuple[int, int]] = []
        for n, is_v, sc_code in ticks:
            flat.extend([(is_v, sc_code)] * n)
        for (is_val, scancode) in flat:
            inject_input(is_val, scancode)
            nb = cur_lin()
            try:
                uc.emu_start(nb, 0, count=CHUNK)
            except UcError as e:
                tr["err"] = str(e); break
            if tr.get("exit") is not None or tr.get("fault"):
                tr["fault"] = None; tr["exit"] = None
            uc.mem_write(DG_LIN + OFF_INPUT_STATE, bytes([is_val & 0xFF]))
            fire_int(8)
        # direct synchronous calls (re-seed device each, since calls may clobber regs)
        for (fn_off, args) in direct:
            if seed_dev is not None:
                uc.mem_write(DG_LIN + OFF_SOUND_DEVICE_STATE, struct.pack("<h", seed_dev))
            call_near(fn_off, args)
        capturing["on"] = False
        return list(cur_records)

    # ---------------------------------------------------------------------------
    # Phase-6 T4 seeding-scenario runner (preseed globals -> call_near -> capture).
    # ---------------------------------------------------------------------------
    def apply_preseed(preseed) -> None:
        for (space, size, off, val) in preseed:
            base = DG_LIN if space == "dg" else CODE_LIN
            uc.mem_write(base + off, struct.pack("<H" if size == 2 else "<B",
                                                 val & (0xFFFF if size == 2 else 0xFF)))

    def run_seed_scenario(sc_id: int, name: str, calls) -> List[bytes]:
        cur_records.clear()
        capturing["on"] = True
        for (fn_off, args, preseed) in calls:
            apply_preseed(preseed)
            # resolve the "CODE_SEG" placeholder in a far-cb arg to the live runtime base.
            real_args = [code_seg if a == "CODE_SEG" else a for a in args]
            # snd_busy_delay (872e) takes its count in CX — seed small (3) to bound the
            # naked-asm busy loop (else uninitialised CX -> ~64K FUN_89e2 calls, trace bloat).
            cx = 3 if fn_off == 0x872e else None
            call_near(fn_off, real_args, cx=cx)
        capturing["on"] = False
        return list(cur_records)

    scenario_blobs: List[Tuple[Scenario, List[bytes]]] = []
    for sc in SCENARIOS:
        sc_id, name, seed_dev, ticks, direct = sc
        restore_boot_state()
        print("[sound_oracle] === scenario %d (%s) seed_device=%s ===" % (
            sc_id, name, seed_dev), flush=True)
        recs = run_scenario(sc)
        n_io = sum(struct.unpack_from("<H", r, 4 + 2 * SND_SNAP_SIZE)[0] for r in recs)
        print("[sound_oracle]   %d records, %d total port-I/O events" % (len(recs), n_io),
              flush=True)
        scenario_blobs.append((sc, recs))

    # Phase-6 T4 seeding scenarios (6,7,8) — seed each unreached T4 fn via call_near.
    for (sc_id, name, calls) in T4_SEED_SCENARIOS:
        restore_boot_state()
        print("[sound_oracle] === SEED scenario %d (%s) ===" % (sc_id, name), flush=True)
        recs = run_seed_scenario(sc_id, name, calls)
        print("[sound_oracle]   %d records (seeded T4 fns)" % len(recs), flush=True)
        # synthetic 5-tuple scenario shell so the writer/round-trip handle it uniformly.
        scenario_blobs.append(((sc_id, name, None, [], []), recs))

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
            sc_id, name, seed_dev, ticks, direct = sc
            nb = name.encode("ascii")
            f.write(struct.pack("<B", sc_id))
            f.write(struct.pack("<B", len(nb))); f.write(nb)
            f.write(struct.pack("<B", (seed_dev if seed_dev is not None else 0xFF) & 0xFF))
            f.write(struct.pack("<I", len(recs)))
            for r in recs:
                f.write(r)
    trace_bytes = os.path.getsize(OUT_TRACE)
    print("[sound_oracle] wrote %s (%d B)" % (OUT_TRACE, trace_bytes), flush=True)

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
            seed = data[o]; o += 1
            n_rec = struct.unpack_from("<I", data, o)[0]; o += 4
            recs = []
            for _r in range(n_rec):
                fn_off, name_idx = struct.unpack_from("<HH", data, o); o += 4
                ent = struct.unpack_from(SND_SNAP_FMT, data, o); o += SND_SNAP_SIZE
                ex = struct.unpack_from(SND_SNAP_FMT, data, o); o += SND_SNAP_SIZE
                n_io = struct.unpack_from("<H", data, o)[0]; o += 2
                ios = []
                for _i in range(n_io):
                    d, port, sz, val = struct.unpack_from("<BHBH", data, o); o += 6
                    ios.append((d, port, sz, val))
                recs.append(dict(fn_off=fn_off, fn=names[name_idx], ent=ent, ex=ex, io=ios))
            scenarios.append(dict(id=sid, name=nm, seed=seed, recs=recs))
        assert o == len(data), "trailing bytes: parsed %d of %d" % (o, len(data))
        return dict(names=names, scenarios=scenarios)

    parsed = parse_trace(OUT_TRACE)
    print("[sound_oracle] round-trip parse OK: %d scenarios, %d fn-names" % (
        len(parsed["scenarios"]), len(parsed["names"])), flush=True)

    # ---------------------------------------------------------------------------
    # sound_model.md
    # ---------------------------------------------------------------------------
    SNAP_FIELDS = (["sound_device_state", "snddrv_mode", "sound_active_mask",
                    "sound_init_state", "sound_mode", "p1_pending_action",
                    "prev_game_mode", "p1_contact_code", "pad"]
                   + ["pf%d" % i for i in range(PARAM_FRAME_WORDS)]
                   + ["timer_cb_off", "timer_cb_seg"])

    def snap_dict(t: tuple) -> dict:
        return dict(zip(SNAP_FIELDS, t))

    lines: List[str] = []
    lines.append("# Bumpy Phase-6 sound capture model (discovery)\n\n")
    lines.append("Generated by `tools/sound_oracle.py`. Capture granularity = sound "
                 "FUNCTION-CALL boundary (entry+exit), plus per-L4-driver hardware "
                 "PORT-I/O (x86 OUT/IN) capture.\n\n")

    lines.append("## Resolved addresses\n\n")
    lines.append("### DGROUP (Ghidra seg 0x203b; runtime read at DG_LIN+off, "
                 "DG_LIN=0x%05x)\n\n" % DG_LIN)
    lines.append("| symbol | DGROUP off | Ghidra-linear |\n|---|---|---|\n")
    for nm, off in [("sound_device_state", OFF_SOUND_DEVICE_STATE),
                    ("sound_active_device_mask", OFF_SOUND_ACTIVE_MASK),
                    ("sound_init_state", OFF_SOUND_INIT_STATE),
                    ("sound_mode", OFF_SOUND_MODE),
                    ("prev_game_mode", OFF_PREV_GAME_MODE),
                    ("timer_callback_table", OFF_TIMER_CB_TABLE),
                    ("p1_pending_action", OFF_P1_PENDING_ACTION),
                    ("p1_contact_code", OFF_P1_CONTACT_CODE),
                    ("tile_below_player(0x79b9)", OFF_TILE_BELOW_PLAYER)]:
        lines.append("| %s | 0x%04x | ram0x%05x |\n" % (nm, off, 0x203b * 16 + off))
    lines.append("\n### CODE segment (Ghidra seg 1000; runtime CODE_LIN+off, "
                 "CODE_LIN=0x%05x)\n\n" % CODE_LIN)
    lines.append("| symbol | seg-1000 off |\n|---|---|\n")
    for nm, off in [("snddrv_mode (DAT_1000_85b3)", COFF_SNDDRV_MODE),
                    ("param frame DAT_9788..979a", COFF_PARAM_FRAME),
                    ("timer_callback_off", COFF_TIMER_CB_OFF),
                    ("timer_callback_seg", COFF_TIMER_CB_SEG),
                    ("voice table 0x83cc", COFF_VOICE_TABLE)]:
        lines.append("| %s | 0x%04x |\n" % (nm, off))

    lines.append("\n## Hooked functions by layer\n\n")
    lines.append("| layer | seg-1000 off | name | reached |\n|---|---|---|---|\n")
    for off in sorted(FN_NAMES, key=lambda o: (FN_LAYER[o], o)):
        lines.append("| %s | 1000:%04x | %s | %d |\n" % (
            FN_LAYER[off], off, FN_NAMES[off], reached[off]))

    lines.append("\n## Per-effect tone-param table (from play_sound_effect 6e30)\n\n")
    lines.append("Form a()=schedule_timer_callback_a(2,arg2,arg3,1,uVar1,uVar2,uVar3,"
                 "uVar4); b()=schedule_timer_callback_b(...). In sound_device_state==4 "
                 "(OPL) the switch is bypassed: a raw 2-byte sample from table 0x27ae "
                 "is emitted via FUN_8a07 instead.\n\n")
    lines.append("| effect id | submit |\n|---|---|\n")
    for eid in sorted(EFFECT_TONE_TABLE):
        lines.append("| 0x%02x | %s |\n" % (eid, EFFECT_TONE_TABLE[eid]))

    lines.append("\n## Device state machine\n\n")
    lines.append("- `sound_device_state` (DGROUP 0x689c) is the L1 dispatch selector: "
                 "`-0x8000` => muted (play_sound is a no-op); `==4` => OPL/AdLib path "
                 "(raw sample + 0x260e/0x276e/0x26ce LUTs); else => PC-speaker/std path "
                 "(0x263e/0x278e/0x26fe LUTs).\n")
    lines.append("- `sound_init_state` (DGROUP 0x557a): 0=uninit; snddrv_init sets 1; "
                 "select_sound_device_from_mask advances 1->2 and picks the first set "
                 "mask bit as `snddrv_mode` (1<<bit).\n")
    lines.append("- `snddrv_mode` (CODE 0x85b3): backend select for snddrv_dispatch_a "
                 "(0=>pc_speaker_silence, 4=>FUN_8ad0 MPU settle, 1=>FUN_8e2f OPL "
                 "all-notes-off).\n")
    lines.append("- `sound_mode` (DGROUP 0x683e): speaker-gate branch in "
                 "speaker_gate_reset / record_status_and_strobe_speaker (0=>also strobe "
                 "the speaker gate).\n")

    lines.append("\n## Captured L4 port-write sequences (per driver, first occurrence)\n\n")
    lines.append("Sound-hardware ports: 0x61 (PC speaker gate+enable), 0x330/0x331 "
                 "(MPU-401 data/status), 0x388/0x389 (OPL addr/data).\n\n")
    lines.append("**Synthesized status reads (no hardware under Unicorn).** The IN side "
                 "of these sequences reads a FIXED synthesized value, not real hardware "
                 "state: port 0x61 returns SYNTH_PORT61_IN=0x%02x; the MPU-401 status "
                 "0x331 returns 0x00 (DSR clear => ready) and the OPL status 0x388 "
                 "returns 0x00 (not-busy) so the driver poll loops exit. These constants "
                 "are run-to-run deterministic (the captured byte does NOT depend on the "
                 "emulator's IN counter), so the trace is reproducible. The VALIDATED "
                 "invariant is the engine's bit manipulation over the replayed IN "
                 "sequence — e.g. pc_speaker_silence's `IN AL,0x61; AND AL,0xfc; OUT "
                 "0x61,AL` yields OUT(0x61)=0x%02x — NOT the absolute hardware byte. A "
                 "later L4 ctest replays the recorded IN sequence (each IN event below "
                 "carries the returned value) before asserting the OUT writes.\n\n"
                 % (SYNTH_PORT61_IN, SYNTH_PORT61_IN & 0xfc))
    if not driver_io:
        lines.append("_(no L4 driver port I/O captured — see scenario notes)_\n")
    for off in sorted(driver_io):
        seq_io = driver_io[off]
        lines.append("\n### %s (1000:%04x) — %d events\n\n" % (FN_NAMES[off], off, len(seq_io)))
        # compress consecutive identical-port reads
        rows = []
        i = 0
        while i < len(seq_io):
            d, port, sz, val = seq_io[i]
            j = i + 1
            while (j < len(seq_io) and seq_io[j] == seq_io[i] and d == 1):
                j += 1
            cnt = j - i
            dirs = "OUT" if d == 0 else "IN"
            if cnt > 1:
                rows.append("%s 0x%03x = 0x%04x (x%d)" % (dirs, port, val, cnt))
            else:
                rows.append("%s 0x%03x = 0x%04x" % (dirs, port, val))
            i = j
        lines.append("`" + " | ".join(rows) + "`\n")

    lines.append("\n## Per-scenario record summary\n\n")
    for sc, recs in scenario_blobs:
        sc_id, name, seed_dev, ticks, direct = sc
        by_fn = collections.Counter()
        io_total = 0
        for r in recs:
            fn_off = struct.unpack_from("<H", r, 0)[0]
            by_fn[FN_NAMES[fn_off]] += 1
            io_total += struct.unpack_from("<H", r, 4 + 2 * SND_SNAP_SIZE)[0]
        lines.append("### Scenario %d — %s (seed_device=%s)\n\n" % (sc_id, name, seed_dev))
        lines.append("- records: %d, port-I/O events: %d\n" % (len(recs), io_total))
        if by_fn:
            lines.append("- fns: %s\n" % ", ".join(
                "%s x%d" % (k, v) for k, v in by_fn.most_common()))
        else:
            lines.append("- fns: (none reached)\n")

    lines.append("\n## Functions NOT reached\n\n")
    not_reached = [off for off in FN_NAMES if reached[off] == 0]
    if not_reached:
        for off in sorted(not_reached):
            lines.append("- 1000:%04x %s (%s)\n" % (off, FN_NAMES[off], FN_LAYER[off]))
        lines.append("\nThese were not exercised by the current scenarios; a later "
                     "task can seed/drive them directly (the call_near() harness in "
                     "this oracle reaches any near entry point).\n")
    else:
        lines.append("- (all hooked functions reached)\n")

    os.makedirs(os.path.dirname(OUT_MODEL), exist_ok=True)
    with open(OUT_MODEL, "w") as f:
        f.write("".join(lines))
    print("[sound_oracle] wrote %s" % OUT_MODEL, flush=True)

    # ---------------------------------------------------------------------------
    # Console summary
    # ---------------------------------------------------------------------------
    print("\n[sound_oracle] REACHED sound functions:", flush=True)
    for off in sorted(FN_NAMES, key=lambda o: (FN_LAYER[o], o)):
        if reached[off]:
            print("   %s 1000:%04x  %-32s x%d" % (
                FN_LAYER[off], off, FN_NAMES[off], reached[off]), flush=True)
    print("[sound_oracle] L4 drivers with captured port I/O:", flush=True)
    for off in sorted(driver_io):
        seq_io = driver_io[off]
        outs = [(p, v) for (d, p, sz, v) in seq_io if d == 0]
        print("   1000:%04x %-32s %d events, OUTs=%s" % (
            off, FN_NAMES[off], len(seq_io),
            ", ".join("0x%03x=0x%02x" % (p, v) for (p, v) in outs[:8])), flush=True)
    if err:
        print("[sound_oracle] emu error:", err, flush=True)
    if tr.get("call_errs"):
        print("[sound_oracle] call_near errors:", tr["call_errs"], flush=True)


if __name__ == "__main__":
    main()
