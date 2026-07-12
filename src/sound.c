/* ────────────────────────────────────────────────────────────────────────────
 *  sound.c — sound subsystem (Phase-6 reconstruction).
 *
 *  SKELETON (Phase-6 Task 2): this TU defines ONLY the sound module's GLOBALS — NO
 *  function bodies.  The ~30 sound functions (play_sound 1000:6e11 .. opl_play_note
 *  1000:905d, full map in tools/sound_oracle.py FN_NAMES) remain stubbed in
 *  game_stubs.c this task; their 1:1 bodies port across Phase-6 T3–T6:
 *    T3 L1 dispatch  (play_sound / play_sound_effect 21-case switch + the per-event
 *                     dispatchers play_action/contact/exit/pickup/event/state_sound),
 *    T4 L2 device    (sound_select_device / snddrv_init / select_from_mask /
 *                     snddrv_dispatch_a/b/c/d / snd_busy_delay),
 *    T5 L3 tone      (schedule_timer_callback_a/b/c + arm/set/disable/restore timer),
 *    T6 L4 hardware  (pc_speaker_silence / speaker_gate_* / MPU-401 / OPL drivers).
 *  At each port the corresponding stub is removed from game_stubs.c and the body
 *  reconstructed here, validated by tools/sound_ctest.c against the Phase-6 T1 trace.
 *
 *  Because this TU contributes no function bodies, sound.obj links cleanly alongside
 *  the game_stubs.c sound stubs with ZERO duplicate symbols — the globals-only
 *  skeleton pattern Phase-5 T2 (anim.obj) / Phase-4 T2 (player2.obj) / Phase-3 T2
 *  (items.obj) used.
 *
 *  ── OWNERSHIP / no-duplicate-symbols (grep-verified across the src/ tree) ───────
 *    DEFINED HERE (genuinely new — a grep over the src C TUs finds no other def):
 *      snddrv_mode               CODE   0x85b3   (DAT_1000_85b3)
 *      sound_init_state          DGROUP 0x557a
 *      sound_active_device_mask  DGROUP 0x5586
 *      sound_mode                DGROUP 0x683e
 *      snd_param_frame[10]       CODE   0x9788..0x979a  (the L3 tone param frame)
 *      snd_timer_cb_off/_seg     CODE   0x97a1 / 0x979f (installed far timer cb)
 *      snd_timer_cb_table[0x40]  DGROUP 0x5516   (arm_timer_callback / set_timer_slot)
 *      snd_voice_table[15]       CODE   0x83cc   (cleared by pc_speaker_silence)
 *    None of these names appear in any other src/ TU (checked: player.c, player2.c,
 *    items.c, game.c, level.c, entity.c, globals.c, game_stubs.c) — so defining them
 *    here introduces no duplicate symbol.  The per-effect/per-device sound LUTs that
 *    the move/contact paths index are ALREADY owned by player.c (move_sound_lut_opl_
 *    25ae / move_sound_lut_std_25de / contact_sound_lut_35de) — sound.c does NOT
 *    redefine them; the L1 dispatchers reach them through player.h when they port.
 *    The per-effect EFFECT_TONE_TABLE (the play_sound_effect 21-case data) is encoded
 *    INLINE in the switch body, not as a standalone array, so there is no table to
 *    define yet; it lands with the play_sound_effect port (T3).
 *
 *    EXTERN (owned elsewhere — grep evidence beside each; NOT defined here):
 *      sound_device_state    player.c:225  `s16 sound_device_state;`     (0x689c)
 *                            — the L1 dispatch selector (==4 OPL, -0x8000 mute);
 *                            already declared extern in player.h + items.h.
 *      p1_pending_action     player.c:219  `u8  p1_pending_action;`      (0x7924)
 *      p1_contact_code       player.c:216  `u8  p1_contact_code;`        (0x8551)
 *      tile_below_player     player.c:223  `u8  tile_below_player;`      (0x79b9)
 *      prev_game_mode        player.c:39   `u8  prev_game_mode;`         (0x8552)
 *      — the L1 dispatch LUT-index inputs (play_action/contact/state_sound read
 *      these to index the per-device sound LUTs).  All four are already declared
 *      extern in player.h; sound.c pulls them via that header rather than
 *      re-declaring (avoids a redundant/conflicting declaration).  The host replay
 *      harness tools/sound_ctest.c supplies its own host definitions of these
 *      cross-module globals (it does not link player.obj), mirroring the
 *      items_ctest / anim_chan_ctest convention.
 *
 *  STACK-CHECK PROLOGUE: every original sound fn opens with Turbo C's compiler-
 *  emitted stack-overflow probe; it is NOT game logic and will be intentionally
 *  OMITTED from the future ports (the convention player.c / items.c / anim.c document).
 *
 *  Source of truth: Ghidra BumpyDecomp + raw disassembly + tools/sound_oracle.py +
 *  local/build/sound_model.md (the Phase-6 T1 sound capture).
 * ──────────────────────────────────────────────────────────────────────────── */
#ifndef BUMPY_H
#include <conio.h>   /* outp / inp — the x86 port intrinsics the L4 drivers issue.
                        Skipped on the host replay harness (it #defines BUMPY_H and
                        supplies its own outp/inp capture+replay shims; see
                        tools/sound_ctest.c).  Under wcc these are the real OUT/IN. */
#include <dos.h>     /* union REGS / struct SREGS / int86x — the DOS INT 21h vector
                        get/set (AH=0x25/0x35) timer_teardown_restore issues.  Skipped
                        on the host replay harness (BUMPY_H defined); it supplies a
                        minimal type-checking-only shim (see tools/sound_ctest.c) since
                        that guarded body is provably unreached on every replay scenario
                        (isr_installed_flag stays 0 — see the fn's own FIDELITY note). */
#endif
#include "sound.h"
#include "player.h"   /* extern sound_device_state + p1_pending_action /
                          p1_contact_code / tile_below_player / prev_game_mode
                          (owned by player.c; pulled here, not re-declared). */

/* ── L3 tone param frame (CODE 0x9788..0x979a) ──────────────────────────────── */
u16 snd_param_frame[SND_PARAM_FRAME_WORDS];

/* installed far timer-callback pointer (CODE 0x97a1 off / 0x979f seg). */
u16 snd_timer_cb_off;
u16 snd_timer_cb_seg;

/* ── device / driver state machine scalars ──────────────────────────────────── */
u16 snddrv_mode;                  /* CODE   0x85b3 — snddrv_dispatch backend select */
u16 sound_init_state;             /* DGROUP 0x557a — 0 uninit / 1 / 2               */
u16 sound_active_device_mask;     /* DGROUP 0x5586 — detected-device bitmask        */
u16 sound_mode;                   /* DGROUP 0x683e — speaker-gate branch selector   */

/* ── data tables (zero-initialised; populated by the future L3/L4 ports) ────────── */
u8  snd_timer_cb_table[SND_TIMER_CB_TABLE_LEN];   /* DGROUP 0x5516 */
u8  snd_voice_table[SND_VOICE_TABLE_LEN];         /* CODE   0x83cc */

/* ── L3 timer-SLOT table (DGROUP 0x549c) ─────────────────────────────────────────
 *  set_timer_slot_raw (1000:7e62) writes per-channel slots at (channel+2)*8 + 0x549c;
 *  get_timer_slot_field (1000:7e3d) reads them.  Modelled as a raw byte table BASED at
 *  0x549c (so the engine's (idx+2)*8 + 0x549c offset arithmetic indexes it 1:1).  The
 *  word at slot+0 holds the value, +2 a zero, +4/+6 the far callback off/seg.  Owned
 *  here (no other TU defines DGROUP 0x549c). */
u8  snd_timer_slot_table[SND_TIMER_SLOT_TABLE_LEN];   /* DGROUP 0x549c */

/* ── L2 device-state extras (owned here) ─────────────────────────────────────────
 *  snddrv_init writes DAT_203b_5584 = 1 (a per-init substep flag, DGROUP 0x5584);
 *  select_sound_device_from_mask's no-device reset path clears DAT_1000_83ee/83ef
 *  (CODE) — a small selected-device scratch pair.  None is captured in the SND_SNAP
 *  (so not in the semantic differential); defined for the faithful body + the link. */
u8  snd_init_substep_5584;        /* DGROUP 0x5584 — snddrv_init substep flag */
u8  snd_select_scratch_83ee;      /* CODE   0x83ee — select-from-mask reset scratch (byte) */
u16 snd_select_scratch_83ef;      /* CODE   0x83ef — select-from-mask reset scratch (word) */

/* ════════════════════════════════════════════════════════════════════════════
 *  Phase-6 T3 — effect→frame pipeline (L1 core dispatch + L3 tone-submit).
 *
 *  STILL-STUBBED callees these bodies reach (faithful-signature stubs in
 *  game_stubs.c for the BUMPY.EXE link; host stubs in tools/sound_ctest.c for the
 *  replay harness).  None contributes to the validated semantic state (the device-
 *  guarded dispatch + the 10-word tone param frame + the installed far cb ptr):
 *    speaker_gate_reset     (1000:9440) — PC-speaker gate reset (L4 port write).  → T5.
 *    FUN_1000_8a07          (1000:8a07) — the OPL/MPU raw-sample emit (L4).       → T5.
 *  record_min_status_code (1000:945b) is NO LONGER stubbed — PORTED below (Task A3).
 *
 *  ── RECONSTRUCTION FIDELITY: the record_min_status_code flags-carry convention ──
 *  schedule_timer_callback_a/b/c open (Borland/Turbo-C convention) by reading the
 *  CPU FLAGS captured at entry (PUSHF; in_CF/in_PF/…) and packing them into a status
 *  word handed to record_min_status_code (1000:945b).  The frame-fill + cb install
 *  then happen only `if (!in_CF)` — i.e. only when the entry carry is CLEAR; on a
 *  set carry the fn returns 0xFFFF with the frame untouched.  The Phase-6 T1 capture
 *  recorded the frame as FILLED, so the engine entered with carry clear.
 *
 *  On the host there is no incoming CPU carry to read.  We model it with a file-scope
 *  scalar `snd_sched_carry_in` defaulting to 0 (carry CLEAR), faithfully reproducing
 *  the engine's captured behaviour (frame fills).  DEVIATION from a literal parameter:
 *  the brief specifies "model the carry-in as a parameter (default clear / 0)"; a true
 *  C parameter would force an extra trailing arg onto every schedule_timer_callback_*
 *  call, breaking the 1:1 transcription of play_sound_effect's 21-case switch (whose
 *  calls must mirror the decomp verbatim).  A file-scope carry scalar carries the
 *  identical observable effect (frame fills iff carry clear) without perturbing the
 *  call sites.  record_min_status_code is PORTED below (Task A3; it records a min
 *  status code — not part of the validated frame, but no longer a no-op).  Recorded in
 *  docs/reconstruction-fidelity.md (the Phase-6 "L1 dispatch + L3 tone-submit" row +
 *  the flags-carry-convention deviation).
 * ════════════════════════════════════════════════════════════════════════════ */

/* still-stubbed callees (see header block above).  speaker_gate_reset (1000:9440) +
 *  FUN_1000_8a07 (1000:8a07) are NO LONGER stubs — they are L4 drivers PORTED 1:1
 *  below (Phase-6 T5), declared in sound.h. */

/* ── record_min_status_code (1000:945b) — PORTED (Task A3): status/result-code latch ──
 *  Latches a status/result code into last_status_code (CODE 0x946c), keeping the
 *  lowest/most-severe value ever seen: if the new value is 0xff (a hard-failure
 *  sentinel) OR is <= the current latch, it replaces it.  asm 1000:945b verbatim:
 *  CMP AX,0xff; JZ .set; CMP CS:[946c],AX; JC .skip; .set: MOV CS:[946c],AX; .skip: RET.
 *
 *  RECONSTRUCTION FIDELITY (register-entry arg modelled as a conventional parameter):
 *  the asm passes the status ONLY in AX — no stack push at any call site (confirmed via
 *  disassembly: schedule_timer_callback_a/b/c do `MOV AX,[BP+n]; CALL 0x945b` with zero
 *  intervening instructions at 1000:9492/9495, 1000:949d.. (a/b analogues), and
 *  record_status_and_strobe_speaker/the L5 ISR tone sequencers do `MOV AX,imm; CALL
 *  0x945b` at 1000:9474/947c and 1000:9479/947c and the tone_seq_callback_* retire
 *  paths).  A literal register-entry reconstruction would need a global stand-in (the
 *  snd_mpu_byte_89e2 / FUN_1000_89e2 convention) PLUS reconciling all 6 already-
 *  reconstructed/validated call sites.  Since every call site already stages the exact
 *  intended value immediately before the call with nothing in between, the value this
 *  fn receives is bit-identical whether modelled as AX or as a C parameter — so the
 *  conventional `u16 status` parameter form is kept (matching the pre-existing stub
 *  signature every call site already uses), avoiding any change to
 *  schedule_timer_callback_a/b/c, record_status_and_strobe_speaker, or
 *  tone_seq_callback_9631/96c4/95b5.  Not part of any validated SND_SNAP field (the
 *  harness does not capture last_status_code), so this fn is not oracle-exercised by
 *  the semantic-state differential either way — see docs/reconstruction-fidelity.md. */
u16 last_status_code;   /* CODE 0x946c — the min/most-severe status-code latch */

void record_min_status_code(u16 status)
{
    if (status == 0xff || status <= last_status_code) {
        last_status_code = status;
    }
}

/* set_timer_slot_raw (1000:7df9) — the L3 timer-slot-table writer the schedulers tail
 *  into (BX=channel, AX=value, CX:DX=far cb).  PORTED below (T4); the schedulers call it
 *  with their faithful register setup (channel 2, value=param_5, far cb 0x9631/.. :0x1000). */
int set_timer_slot_raw(int channel, int value, u16 cb_off, u16 cb_seg);

/* Modelled entry-CPU-carry for the schedulers (see FIDELITY note above): 0 = clear,
 *  so the frame fills exactly as the engine captured.  The engine reads the real
 *  PUSHF carry; we default it clear. */
static u8 snd_sched_carry_in = 0;

/* The OPL raw-sample param table the effect_id==(device 4) path indexes (CODE 0x27ae,
 *  2 bytes per effect).  Reconstructed only as far as the dispatch needs it; the actual
 *  table data + the FUN_1000_8a07 emit land with the L4/L5 sample port (T5).  Kept as a
 *  zero table so the dispatch indexes a defined object on the host; the device-4 path is
 *  not exercised by the validated semantic-state records (which assert the param frame). */
static const u8 snd_opl_sample_table[0x200];   /* CODE 0x27ae */

/* ── play_sound (1000:6e11) — L1 entry guard ─────────────────────────────────────
 *  Trivial device guard: dispatch to play_sound_effect unless the device is muted
 *  (sound_device_state == -0x8000).  Stack-probe prologue omitted (project convention). */
void play_sound(u8 sound_id)
{
    if (sound_device_state != -0x8000) {
        play_sound_effect(sound_id);
    }
}

/* ── play_sound_effect (1000:6e30) — the 21-case effect→tone-param switch ─────────
 *  Dispatch on effect id to per-effect tone parameters and submit them to the tone
 *  engine (schedule_timer_callback_a / _b).  In OPL mode (sound_device_state == 4)
 *  emit a raw two-byte sample from table 0x27ae via FUN_1000_8a07 (stub → T5).
 *  Switch ported VERBATIM from the decomp (cases NOT collapsed); the goto LAB_70d6
 *  tail (cases 1/9/0xc/0x10/0x15 share `uVar3=1; tone_arg2=0x1e`) is preserved 1:1. */
void play_sound_effect(u8 effect_id)
{
    u16 tone_arg3;
    u16 tone_arg2;
    u16 uVar1;
    u16 uVar2;
    u16 uVar3;
    u16 uVar4;

    if (sound_device_state == 4) {
        FUN_1000_8a07(snd_opl_sample_table[(u16)effect_id * 2 + 0],
                      snd_opl_sample_table[(u16)effect_id * 2 + 1]);
        return;
    }
    switch (effect_id) {
    case 0x01:
        uVar4 = 1;
        uVar2 = 0x1c2;
        uVar1 = 10;
        tone_arg3 = 1000;
        goto LAB_1000_70d6;
    case 0x02:
        uVar4 = 1;
        uVar3 = 1;
        uVar2 = 0x1c2;
        uVar1 = 0xfff6;
        tone_arg3 = 800;
        tone_arg2 = 0x28;
        break;
    case 0x03:
        uVar4 = 0xffff;
        uVar3 = 4;
        uVar2 = 499;
        uVar1 = 0xffff;
        tone_arg3 = 0x1b8;
        tone_arg2 = 400;
        break;
    case 0x04:
        uVar4 = 4;
        uVar3 = 1;
        uVar2 = 100;
        uVar1 = 0xffff;
        tone_arg3 = 0xdc;
        tone_arg2 = 0x5a;
        break;
    case 0x05:
        uVar4 = 2;
        uVar3 = 1;
        uVar2 = 0x1b8;
        uVar1 = 10;
        tone_arg3 = 1000;
        tone_arg2 = 0x19;
        break;
    case 0x06:
        uVar4 = 5;
        uVar3 = 2;
        uVar2 = 0x1b8;
        uVar1 = 10;
        tone_arg3 = 0x44c;
        tone_arg2 = 0x14;
        break;
    case 0x07:
        uVar4 = 3;
        uVar3 = 1;
        uVar2 = 0x1b8;
        uVar1 = 10;
        tone_arg3 = 0x4b0;
        tone_arg2 = 0xf;
        break;
    case 0x08:
        uVar4 = 5;
        uVar3 = 1;
        uVar2 = 100;
        uVar1 = 0xfffb;
        tone_arg3 = 0xdc;
        tone_arg2 = 0x28;
        break;
    case 0x09:
        uVar4 = 1;
        uVar2 = 0x1c2;
        uVar1 = 0x14;
        tone_arg3 = 0x32;
        goto LAB_1000_70d6;
    case 0x0a:
        uVar4 = 10;
        uVar3 = 1;
        uVar2 = 0x15d;
        uVar1 = 0x32;
        tone_arg3 = 200;
        tone_arg2 = 0xf;
        break;
    case 0x0b:
        schedule_timer_callback_b(2, 0x28, 0x14, 499, 1, 0xfffc);
        return;
    case 0x0c:
        uVar4 = 2;
        uVar2 = 0x1a4;
        uVar1 = 10;
        tone_arg3 = 0x4b0;
        goto LAB_1000_70d6;
    case 0x0d:
        uVar4 = 0xf;
        uVar3 = 2;
        uVar2 = 0x15d;
        uVar1 = 0x32;
        tone_arg3 = 200;
        tone_arg2 = 0x14;
        break;
    case 0x0e:
        uVar4 = 0;
        uVar3 = 10;
        uVar2 = 200;
        uVar1 = 4;
        tone_arg3 = 10;
        tone_arg2 = 0x32;
        break;
    case 0x0f:
        uVar4 = 1;
        uVar3 = 2;
        uVar2 = 100;
        uVar1 = 2;
        tone_arg3 = 300;
        tone_arg2 = 400;
        break;
    case 0x10:
        uVar4 = 2;
        uVar2 = 0x1a4;
        uVar1 = 10;
        tone_arg3 = 0x4b0;
        goto LAB_1000_70d6;
    case 0x11:
        schedule_timer_callback_b(2, 0x28, 0x14, 499, 1, 0xfffc);
        return;
    case 0x12:
        schedule_timer_callback_b(2, 0x50, 0x1e, 499, 2, 0xfffc);
        return;
    case 0x13:
        uVar4 = 1;
        uVar3 = 2;
        uVar2 = 100;
        uVar1 = 1;
        tone_arg3 = 300;
        tone_arg2 = 800;
        break;
    case 0x14:
        uVar4 = 0;
        uVar3 = 10;
        uVar2 = 200;
        uVar1 = 4;
        tone_arg3 = 10;
        tone_arg2 = 0x32;
        break;
    case 0x15:
        uVar4 = 1;
        uVar2 = 0x1c2;
        uVar1 = 10;
        tone_arg3 = 600;
LAB_1000_70d6:
        uVar3 = 1;
        tone_arg2 = 0x1e;
        break;
    default:
        goto switchD_1000_6e7e_default;
    }
    schedule_timer_callback_a(2, tone_arg2, tone_arg3, 1, uVar1, uVar2, uVar3, uVar4);
switchD_1000_6e7e_default:
    return;
}

/* ── schedule_timer_callback_a (1000:9488) — L3 tone-submit (8-arg frame) ─────────
 *  Records prior CPU-flags status, then (if carry clear) fills the 10-word tone param
 *  frame snd_param_frame[0..9] (CODE 0x9788..0x979a) from the args and installs the far
 *  callback ptr 1000:9631.  param_2..param_8 are the frame words.  Frame-offset →
 *  snd_param_frame[] index map:
 *    0x9788→[0] 0x978a→[1] 0x978c→[2] 0x978e→[3] 0x9790→[4]
 *    0x9792→[5] 0x9794→[6] 0x9796→[7] 0x9798→[8] 0x979a→[9] (byte 0x0f, low half).
 *  NOTE on the record_min_status_code arg: the ORIGINAL (decomp 1000:9488) does NOT pass
 *  param_1 — it passes the PACKED ENTRY-FLAGS word, the bit-OR of the CPU flags captured at
 *  entry: (in_NT<<14)|(in_OF<<11)|(in_IF<<9)|(in_TF<<8)|(in_SF<<7)|(in_ZF<<6)|(in_AF<<4)|
 *  (in_PF<<2)|in_CF.  Because record_min_status_code is a no-op stub (records into CS:[0x946c],
 *  not part of any validated frame/port sequence), we hand it the available value (param_1)
 *  in lieu of reconstructing the host-absent FLAGS register — observationally identical.
 *  Deeper callee FUN_1000_7df9 = set_timer_slot_raw (PORTED T4); speaker_gate_reset PORTED T5. */
u16 schedule_timer_callback_a(u16 param_1, u16 param_2, u16 param_3, u16 param_4,
                              u16 param_5, u16 param_6, u16 param_7, u16 param_8)
{
    u16 ret_status;
    u8  in_CF = snd_sched_carry_in;   /* modelled entry carry (see FIDELITY note) */

    record_min_status_code(param_1);  /* ORIGINAL: packed entry-FLAGS word (in_NT..in_CF
                                        *  bit-OR); param_1 stands in (callee is a no-op stub) */
    ret_status = 0xffff;
    if (!in_CF) {
        snd_param_frame[0] = param_2;   /* DAT_1000_9788 */
        snd_param_frame[1] = param_3;   /* DAT_1000_978a */
        snd_param_frame[7] = param_4;   /* DAT_1000_9796 */
        snd_param_frame[2] = param_4;   /* DAT_1000_978c */
        snd_param_frame[3] = param_5;   /* DAT_1000_978e */
        snd_param_frame[4] = param_6;   /* DAT_1000_9790 */
        snd_param_frame[5] = param_7;   /* DAT_1000_9792 */
        snd_param_frame[8] = param_7;   /* DAT_1000_9798 */
        snd_param_frame[6] = param_8;   /* DAT_1000_9794 */
        snd_param_frame[9] = 0xf;       /* DAT_1000_979a (byte 0x0f) */
        snd_timer_cb_off = 0x9631;      /* timer_callback_off (CODE 0x97a1) */
        snd_timer_cb_seg = 0x1000;      /* timer_callback_seg (CODE 0x979f) */
        /* The decomp renders this tail as a void FUN_1000_7df9(); the disassembly
         *  (1000:94ec MOV AX,[BP+0xe]=param_6; MOV BX,0x2=channel; CALL 0x7df9, with
         *  CX=0x9631=cb_off / DX=0x1000=cb_seg already loaded) shows it is the timer-slot
         *  writer set_timer_slot_raw(channel=2, value=param_6, cb_off=0x9631, cb_seg=0x1000)
         *  — the register-passed args the void rendering hides.  Ported faithfully to the
         *  asm (T4) so the 0x549c slot-table install is reproduced + validated. */
        set_timer_slot_raw(2, (int)param_6, 0x9631, 0x1000);
        speaker_gate_reset();
        ret_status = 0;
    }
    return ret_status;
}

/* ── schedule_timer_callback_b (1000:9502) — L3 tone-submit (6-arg frame) ─────────
 *  As _a but a smaller frame, installs the far callback 1000:96c4.  Frame words written:
 *    [0]=param_2 [1]=param_3 [4]=param_4 [5]=param_5 [8]=param_5 [6]=param_6 [9]=0x0f. */
u16 schedule_timer_callback_b(u16 param_1, u16 param_2, u16 param_3, u16 param_4,
                              u16 param_5, u16 param_6)
{
    u16 ret_status;
    u8  in_CF = snd_sched_carry_in;

    record_min_status_code(param_1);    /* ORIGINAL: packed entry-FLAGS word (no-op stub) */
    ret_status = 0xffff;
    if (!in_CF) {
        snd_param_frame[0] = param_2;   /* DAT_1000_9788 */
        snd_param_frame[1] = param_3;   /* DAT_1000_978a */
        snd_param_frame[4] = param_4;   /* DAT_1000_9790 */
        snd_param_frame[5] = param_5;   /* DAT_1000_9792 */
        snd_param_frame[8] = param_5;   /* DAT_1000_9798 */
        snd_param_frame[6] = param_6;   /* DAT_1000_9794 */
        snd_param_frame[9] = 0xf;       /* DAT_1000_979a (byte 0x0f) */
        snd_timer_cb_off = 0x96c4;      /* timer_callback_off (CODE 0x97a1) */
        snd_timer_cb_seg = 0x1000;      /* timer_callback_seg (CODE 0x979f) */
        /* asm 1000:9557 MOV AX,[BP+0xa]=param_4; MOV BX,0x2; CALL 0x7df9 -> the writer
         *  with channel=2, value=param_4, cb_off=0x96c4, cb_seg=0x1000 (see _a note). */
        set_timer_slot_raw(2, (int)param_4, 0x96c4, 0x1000);
        speaker_gate_reset();
        ret_status = 0;
    }
    return ret_status;
}

/* ── schedule_timer_callback_c (1000:956d) — L3 tone-submit (single frame arg) ─────
 *  As _a/_b but installs the far callback 1000:95b5 with a single frame word
 *  (snd_param_frame[1]) + the 0x0f marker.  No caller in this task's scope reaches it,
 *  but it ports here with the rest of the L3 tone-submit trio. */
u16 schedule_timer_callback_c(u16 param_1, u16 param_2)
{
    u16 ret_status;
    u8  in_CF = snd_sched_carry_in;

    record_min_status_code(param_1);    /* ORIGINAL: packed entry-FLAGS word (no-op stub) */
    ret_status = 0xffff;
    if (!in_CF) {
        snd_param_frame[9] = 0xf;       /* DAT_1000_979a (byte 0x0f) */
        snd_param_frame[1] = param_2;   /* DAT_1000_978a */
        snd_timer_cb_off = 0x95b5;      /* timer_callback_off (CODE 0x97a1) */
        snd_timer_cb_seg = 0x1000;      /* timer_callback_seg (CODE 0x979f) */
        /* asm 1000:959f MOV AX,[BP+0x8]; MOV BX,0x2; CALL 0x7df9.  [BP+0x8] is a THIRD
         *  stack slot the 2-arg decomp does not surface; schedule_timer_callback_c has no
         *  caller in any captured scenario (never reached), so the value is indeterminate
         *  here.  Modelled as 0 (the validated record set is unaffected — _c is unreached);
         *  faithful to the decomp's 2-arg signature + the writer-call shape. */
        set_timer_slot_raw(2, 0, 0x95b5, 0x1000);
        speaker_gate_reset();
        ret_status = 0;
    }
    return ret_status;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Phase-6 T4 — L1 event wrappers + L2 device state machine + L3 timer-table mgmt.
 *
 *  L1 EVENT WRAPPERS.  Each reads a per-device/per-state byte LUT (the OPL table when
 *  sound_device_state == 4, else the standard table) indexed by an event/state global,
 *  computes a sound id, and (if non-zero) calls the T3-ported play_sound.  The LUT bytes
 *  are static DGROUP data in the binary (read via `[index + base]`, default DS=DGROUP);
 *  reconstructed here as named C arrays carrying the exact image bytes (dumped from
 *  BUMPY_unpacked.exe at the DGROUP offsets the disassembly names: 0x260e/0x263e/
 *  0x276e/0x278e/0x26ce/0x26fe).  Owned here; no other TU defines them.
 *
 *  STILL-STUBBED callees these wrappers reach (faithful-signature, not validated state):
 *    FUN_1000_6183 (1000:6183) — an out-of-scope entity-management sweep play_contact_sound
 *        calls for contact codes 0xe..0x11; not a sound fn (→ entity subsystem).  STUBBED.
 *    p1_try_trigger_pending_action (1000:654e) — RECONSTRUCTED in player.c (Phase-2);
 *        play_state_sound_79b9 calls it (not stubbed — the real symbol links).
 *
 *  L2 DEVICE STATE.  sound_select_device -> snddrv_init (guard sound_init_state 0x557a)
 *  -> select_sound_device_from_mask scans the detected-device bitmask and latches the
 *  selected device into snddrv_mode (0x85b3) + sound_active_device_mask (0x5586).  The
 *  snddrv_dispatch_a/b/c/d fns fan out by snddrv_mode to the L4 backends (STUBBED → T5/T6).
 *  The L4 init/IO callees pc_speaker_silence/FUN_8ad0/8e2f/89e2 + the dispatch_b/c/d
 *  backends are PORTED (T5, below); mpu401_reset_to_uart + snddrv_init_substep
 *  (FUN_1000_8b2a) are PORTED here (Task A3, just below).
 *
 *  L3 TIMER-TABLE MGMT.  Two engine tables:
 *    - 0x5516 callback table (arm_timer_callback / disable_timer_callback): per-channel
 *      8-byte slot {current@0, reload@2, cb_off@4, cb_seg@6}, base 0x5516 + channel*8.
 *    - 0x549c slot table (set_timer_slot -> set_timer_slot_raw / get_timer_slot_field):
 *      per-channel 8-byte slot {value@0, 0@2, cb_off@4, cb_seg@6}, base 0x549c +
 *      (channel+2)*8.  set_timer_slot_raw is also the tail the L3 schedulers reach.
 * ════════════════════════════════════════════════════════════════════════════ */

/* still-stubbed callees the T4 bodies reach (see header block).
 *  pc_speaker_silence / FUN_1000_8ad0 / FUN_1000_8e2f / FUN_1000_89e2 are NOW PORTED 1:1
 *  below (Phase-6 T5 L4 hardware backends), declared in sound.h.  mpu401_reset_to_uart
 *  + snddrv_init_substep (FUN_1000_8b2a) are PORTED here (Task A3). */

/* ── mpu401_reset_to_uart (1000:8a75) — PORTED (Task A3): MPU-401 chip reset → UART ──
 *  Gated on mpu401_present (DGROUP 0x557c): if nonzero, poll port 0x331 for DSR
 *  (bit 0x40) CLEAR (up to 5000 iterations); on success send the reset command 0xFF to
 *  0x331 and retry (up to 3x) polling 0x331 for the ACK-ready bit (0x80) CLEAR, then
 *  read 0x330 expecting the ACK byte 0xFE; once ACKed, poll 0x331 for DSR CLEAR once
 *  more and, on success, send the "enter UART mode" command 0x3F to 0x331.  Any timeout
 *  (either poll) or a non-0xFE ACK byte clears mpu401_present (chip declared absent).
 *  asm 1000:8a75 verbatim (LAB_1000_8a93 = the 3x retry loop around the ACK poll+check;
 *  the final DSR poll before the 0x3F send is NOT retried — a timeout there falls
 *  straight to the failure path, matching the disassembly's direct JNZ 0x8ac0).
 *
 *  Register-return: the engine returns CONCAT22(in_DX, mpu401_present) — a 32-bit value
 *  whose high word is the ambient (uninitialised-from-the-decompiler's-view) DX
 *  register, not real data.  Modelled as `int` returning mpu401_present alone (the low
 *  word — the only part any caller could rely on); the high-word DX is a calling-
 *  convention artifact, not modelled.  NOT oracle-exercised: 8a75 is not one of the 10
 *  L4 driver entry points the oracle scopes port-I/O capture to (tools/sound_oracle.py
 *  L4_FNS), so it has no port-write-sequence record — and mpu401_present defaults 0 (no
 *  reconstructed code sets it nonzero), so its unconditionally-called site (snddrv_init,
 *  below) exercises only the early-return no-op path, which is why the already-
 *  validated device_init scenario is unaffected by wiring in this real body. */
extern s16 mpu401_present;   /* DGROUP 0x557c — defined below with FUN_1000_89e2 (T5) */

int mpu401_reset_to_uart(void)
{
    u16 cx;
    u8  al;
    u8  retry;

    if (mpu401_present != 0) {
        cx = 0x1388;                                  /* poll DSR (0x40) CLEAR */
        do {
            al = (u8)inp(0x331);
            cx--;
        } while (cx != 0 && (al & 0x40) != 0);
        if ((al & 0x40) == 0) {
            outp(0x331, 0xff);                         /* MOV AL,0xff; OUT DX,AL */
            retry = 3;
            for (;;) {
                cx = 0x1388;                            /* poll ACK-ready (0x80) CLEAR */
                do {
                    al = (u8)inp(0x331);
                    cx--;
                } while (cx != 0 && (al & 0x80) != 0);
                if ((al & 0x80) == 0 && (u8)inp(0x330) == 0xfe) {
                    cx = 0x1388;                        /* poll DSR (0x40) CLEAR again */
                    do {
                        al = (u8)inp(0x331);
                        cx--;
                    } while (cx != 0 && (al & 0x40) != 0);
                    if ((al & 0x40) == 0) {
                        outp(0x331, 0x3f);               /* enter UART mode */
                        return mpu401_present;
                    }
                    break;                              /* timeout — no retry, straight to fail */
                }
                retry--;
                if (retry == 0) break;                  /* DEC BX; JNZ (3 attempts total) */
            }
        }
        mpu401_present = 0;                              /* MOV word ptr [0x557c],0 */
    }
    return mpu401_present;
}

/* ── snddrv_init_substep (1000:8b2a) — PORTED (Task A3): snddrv_init's OPL init substep ─
 *  Sets midi_seq_step_active (DGROUP 0x557e) = 1, calls the OPL-chip-detect leaf
 *  maybe_opl2_detect_chip (1000:8fb6); if it reports "not detected" clears the flag,
 *  else runs the OPL register-init leaf opl2_reset_all_regs (1000:8eeb).  Returns the
 *  flag.  asm 1000:8b2a verbatim.
 *
 *  CARVE-OUT: maybe_opl2_detect_chip + opl2_reset_all_regs are further out-of-scope
 *  leaves this port newly reaches (guidance: carve out anything beyond the 4 target
 *  fns) — both already carry canonical Ghidra names (a prior naming pass) but neither
 *  is reconstructed here: maybe_opl2_detect_chip is an OPL2-status-register probe
 *  (reads port 0x388 into AL/DL, tests bit patterns) and opl2_reset_all_regs is an
 *  ~20-register OPL init sequence with 2 status-poll loops — genuine OPL-driver work,
 *  separate from this task's MPU/init/timer/status-latch scope.  Faithful-signature
 *  no-op carve-out stubs in game_stubs.c + tools/sound_ctest.c (the established
 *  `void f(void)` convention, e.g. FUN_1000_6183 / the 3 MIDI-note carve-outs above).
 *
 *  RECONSTRUCTION FIDELITY (the ZF-only branch): maybe_opl2_detect_chip's asm computes
 *  its detected/not-detected verdict into AH, then executes `AND AH,AH` (setting ZF)
 *  BEFORE popping its saved AX/CX/DX off the stack — so by the time it RETs, the AX
 *  register the caller sees is the CALLER's OWN original AX (restored by the POPs), not
 *  the verdict; only the ZERO FLAG survives to convey the result.  A void-returning C
 *  stub cannot convey a flags-only outcome, so the branch this fn takes on that verdict
 *  is modelled via the file-scope `snd_opl_detect_zf` (defaulting to "not detected",
 *  matching the snd_sched_carry_in modelled-flag convention).  The exact default is
 *  IMMATERIAL to the validated state: both destinations (clearing
 *  midi_seq_step_active, a global not in the SND_SNAP, or calling the equally-carved-
 *  out opl2_reset_all_regs) have zero observable effect on any comparator-asserted
 *  global — so wiring this real body into snddrv_init's already-validated call site
 *  (below) cannot regress the device_init scenario either way. */
u16 midi_seq_step_active;             /* DGROUP 0x557e */
static u8 snd_opl_detect_zf = 1;      /* modelled ZF from maybe_opl2_detect_chip (1=not detected) */

extern void maybe_opl2_detect_chip(void);   /* 1000:8fb6 — OPL2 chip-status probe (carve) */
extern void opl2_reset_all_regs(void);      /* 1000:8eeb — OPL2 register init (carve) */

u16 snddrv_init_substep(void)
{
    midi_seq_step_active = 1;
    maybe_opl2_detect_chip();
    if (snd_opl_detect_zf) {
        midi_seq_step_active = 0;
    } else {
        opl2_reset_all_regs();
    }
    return midi_seq_step_active;
}

/* out-of-scope MIDI-note carve-outs the 9 MIDI dispatch backends (below) reach — a
 *  NEW carve-out boundary this task discovered (see the "MIDI dispatch backends"
 *  section).  All three already carry canonical Ghidra names from a prior naming pass;
 *  none is reconstructed here (genuinely out of scope — separate future MIDI-engine
 *  work), matching the FUN_1000_6183 precedent below (and the pit_set_counter0 /
 *  maybe_opl2_detect_chip / opl2_reset_all_regs carve-outs Task A3 adds just above and
 *  just below). */
extern void seq_set_channel_param(void);     /* 1000:922c — OPL/PC-spk program-change (carve) */
extern void midi_emit_voice_msg_w3(void);    /* 1000:8e93 — OPL program-change fwd chain (carve) */
extern void opl_event_note_on(void);         /* 1000:8ea3 — OPL note-on -> opl_play_note (carve) */

extern void FUN_1000_6183(void);          /* 1000:6183   out-of-scope entity sweep (→ entity) */

/* ── timer_teardown_restore (1000:7fef) — PORTED (Task A3): int-8/int-0Fh vector teardown ─
 *  Register-entry (no stack args — confirmed via disasm: AX/CX/DX are read directly,
 *  never [BP+n]).  Gated on isr_installed_flag (DGROUP 0x54d4): if zero, no-op.  Else:
 *  disables interrupts (PUSHF;CLI), restores interrupt vector 8 (the PIT/IRQ0 timer) to
 *  the caller-supplied far pointer (CX=offset, DX=segment) via DOS INT 21h AH=0x25;
 *  then saves the CURRENT vector 0x0F (DOS INT 21h AH=0x35) into DGROUP 0x54d8/0x54da
 *  before overwriting it with the game's own handler at CS:0x8086 (INT 21h AH=0x25);
 *  then reads a per-index reload value from a table at DGROUP 0x54de (indexed by the
 *  caller-supplied AX), stages it at DGROUP 0x54dc, and tail-calls pit_set_counter0
 *  (1000:7f9a) with that value in AX (the register-entry reload arg); finally re-enables
 *  interrupts (POPF).  asm 1000:7fef verbatim.
 *
 *  RECONSTRUCTION FIDELITY (register-entry args + provably-dead body): timer_restore
 *  (1000:7fde, already PORTED/validated in T4) is the sole reconstructed caller and
 *  calls this fn with NO args (`timer_teardown_restore();`) — matching timer_restore's
 *  existing validated contract, which this task must not disturb.  The engine's real
 *  AX (table index) / CX:DX (vector-8 restore far ptr) inputs are modelled as the
 *  file-scope snd_isr_restore_index/off/seg globals (the snd_mpu_byte_89e2 register-
 *  entry convention), defaulting to 0.  isr_installed_flag (DGROUP 0x54d4) defaults 0
 *  and is never set nonzero by any function this codebase reconstructs or exercises —
 *  so the entire guarded body (the vector restore, the INT 21h calls, the table read,
 *  the pit_set_counter0 call) is PROVABLY UNREACHED by every scenario in the trace; the
 *  exact register-stand-in values are therefore immaterial to the gate.  Ported 1:1 for
 *  faithfulness + the BUMPY.EXE link (documented, same "reconstructed as documentation,
 *  not runtime-gated" precedent as the L5 ISR tone sequencer later in this file).
 *
 *  CARVE-OUT: pit_set_counter0 (1000:7f9a) is a further out-of-scope PIT hardware-init
 *  leaf this port reaches (also called by the unrelated, unreconstructed
 *  uninstall_interrupt_handler / pit_set_counter0_wrap — a separate ISR-install
 *  subsystem, out of this task's scope) — faithful-signature no-op carve-out stub in
 *  game_stubs.c + tools/sound_ctest.c.
 *
 *  HOST-BUILD NOTE: the 3 DOS INT 21h calls use int86x()/union REGS/struct SREGS (the
 *  project's established DOS-interrupt convention — see config_screens.c/input.c/
 *  gfx_palette.c/video.c), available via <dos.h> (pulled in transitively by sound.h ->
 *  bumpy.h on the real wcc build, and via this file's own guarded <dos.h> include).  The
 *  host replay harness (BUMPY_H defined) supplies a minimal type-checking-only shim
 *  (tools/sound_ctest.c) since the guarded body above is provably unreached there. */
u8  isr_installed_flag;         /* DGROUP 0x54d4 — set by the (unreconstructed,
                                    out-of-scope) interrupt-install routine */
u16 saved_timer_vector_off;     /* DGROUP 0x54d8 — saved INT 0Fh vector offset */
u16 saved_timer_vector_seg;     /* DGROUP 0x54da — saved INT 0Fh vector segment */
u16 timer_restore_reload_value; /* DGROUP 0x54dc — table[index] staged for pit_set_counter0 */
u16 timer_restore_table[8];     /* DGROUP 0x54de.. — per-index restore table (unknown
                                    real extent — out of scope; sized defensively) */
u16 snd_isr_restore_index;      /* engine AX — table index into timer_restore_table */
u16 snd_isr_restore_off;        /* engine CX — vector-8 restore far-ptr OFFSET */
u16 snd_isr_restore_seg;        /* engine DX — vector-8 restore far-ptr SEGMENT */

extern void pit_set_counter0(void);   /* 1000:7f9a — PIT hardware init (carve, out of scope) */

void timer_teardown_restore(void)
{
    union REGS  r;
    struct SREGS sr;

    if (isr_installed_flag == 0) {              /* CMP [0x54d4],0; JZ (skip to epilogue) */
        return;
    }
    /* PUSHF; CLI — not modelled on the host (no CPU IF to save/restore). */
    r.h.ah = 0x25; r.h.al = 0x08;                /* AH=0x25 AL=8: DOS set-vector(int 8) */
    sr.ds  = snd_isr_restore_seg;
    r.x.dx = snd_isr_restore_off;
    int86x(0x21, &r, &r, &sr);

    r.x.ax = 0x350f;                             /* AH=0x35 AL=0xF: DOS get-vector(int 0Fh) */
    int86x(0x21, &r, &r, &sr);
    saved_timer_vector_off = r.x.bx;            /* MOV [0x54d8],BX */
    saved_timer_vector_seg = sr.es;             /* MOV BX,ES; MOV [0x54da],BX */

    r.h.ah = 0x25; r.h.al = 0x0f;                /* AH=0x25 AL=0xF: DOS set-vector(int 0Fh) */
    sr.ds  = 0x1000;
    r.x.dx = 0x8086;
    int86x(0x21, &r, &r, &sr);

    timer_restore_reload_value =                 /* MOV BX,0x54de; ADD AX,AX; ADD BX,AX;
                                                     MOV AX,[BX] */
        timer_restore_table[snd_isr_restore_index &
                             (sizeof(timer_restore_table) / sizeof(timer_restore_table[0]) - 1)];
    /* POPF — not modelled on the host (see above). */
    pit_set_counter0();                          /* CALL 0x7f9a (carve — out of scope) */
}

/* ════════════════════════════════════════════════════════════════════════════
 *  MIDI dispatch backends — snddrv_dispatch_b/c/d's 9 mode{0,1,4} backends.
 *
 *  snddrv_dispatch_b/c/d (85db/8600/8626, already PORTED above) fan out by snddrv_mode
 *  to these 9 leaves.  Their ONLY real caller is `midi_process_event` (1000:8751 calls
 *  dispatch_b, 8756 dispatch_c, 875b dispatch_d — confirmed via Ghidra xrefs), a MIDI
 *  track-event parser that is ITSELF register-entry (`unaff_SI`) and NOT reconstructed
 *  anywhere in this codebase — it is separate, not-yet-started MIDI-engine work (this
 *  task closes out the sound-EFFECT pipeline's last stub gaps, "before the MIDI engine
 *  work begins" per the task brief). Verified via the raw disassembly (not just the
 *  decompiler's simplified "return in_AX" view, which hides the real SI/BX-relative
 *  reads): all 9 read/advance the caller's DS:SI (the live MIDI-track byte cursor) and/or
 *  AL (the MIDI event status byte) and/or CS:[BX+0x80] (a per-track default-channel
 *  byte) directly as REGISTERS — no stack args, matching the existing snd_busy_delay
 *  register-entry precedent (src/sound.c, "RECONSTRUCTION FIDELITY: register-args asm
 *  routine").
 *
 *  ── RECONSTRUCTION FIDELITY (register-entry; applies to all 9 below) ──────────────
 *  Modelled with the SAME convention as snd_mpu_byte_89e2 (the other ambient-register
 *  standin already in this file): file-scope globals stand in for the registers a
 *  reconstructed CALLER would otherwise supply as arguments, because the true caller
 *  (midi_process_event) is unreconstructed and dispatch_b/c/d's own EXISTING,
 *  already-committed call sites (`FUN_1000_9xxx();`, no args) are unchanged by this
 *  task (per the brief). `snd_seq_cursor` models DS:SI (read-and-advanced in place by
 *  LODSB, exactly as the asm does); `snd_seq_event_al` models AL; `snd_seq_default_chan`
 *  models the resolved CS:[BX+0x80] byte (BX itself, an ambient per-track table
 *  pointer, is never modelled as a real pointer — nothing in this codebase defines
 *  what it points at, matching the "genuinely out of scope" MIDI-engine boundary).
 *
 *  ── NOT ORACLE-EXERCISED (documented, same precedent as opl_play_note/FUN_8e2f) ────
 *  None of the 9 is hooked in tools/sound_oracle.py (no FN_NAMES/L4_FNS entries) and
 *  none has a tools/sound_ctest.c PORTED[] entry. Reason: the oracle's call_near()
 *  harness seeds DGROUP/CODE memory + explicit stack args, not CPU registers, and the
 *  SND_SNAP captures neither SI/AL/BX nor the MIDI byte stream they read — so a
 *  differential here could not observe a real captured ground truth for what these fns
 *  actually do; it would either be gate-trivial (mode0/mode1: no OUT, no SND_SNAP field
 *  touched, PASS either way) or would exercise genuinely uncontrolled/ambient register
 *  content (mode4's snd_busy_delay count, or the two complex note handlers) with no way
 *  to tell a correct port from an incorrect one. Forcing a scenario through them would
 *  also newly reach the 3 out-of-scope carve-outs above (still no-op stubs) — which
 *  would raise the trace's UNPORTED count above the 25 baseline the combined-task gate
 *  holds fixed — a documented, deliberate scope decision (see docs/reconstruction-fidelity.md
 *  + the task report), invoking the brief's own escape hatch ("if the backends
 *  genuinely cannot be exercised by the oracle... report as a documented not-exercised
 *  UNPORTED... and proceed"). All 9 are ported 1:1 here for faithfulness + the
 *  BUMPY.EXE link, exactly like the L5 ISR tone-sequencer (pit_timer_isr_multiplexer /
 *  tone_seq_callback_*) already in this file.
 * ════════════════════════════════════════════════════════════════════════════ */
u8  snd_seq_event_al;         /* engine AL   — the MIDI event status/data byte  */
u8  snd_seq_default_chan;     /* engine CS:[BX+0x80] — per-track default channel */

/* snd_seq_cursor's default backing: dispatch_b/c/d (already-ported, already-validated)
 *  unconditionally reach one of the 9 backends below via their mode0 branch whenever
 *  snddrv_mode==0 — which DOES happen on the validated semantic-state replay path (the
 *  t4_l2_dispatch scenario seeds mode 0), so these register-entry leaves ARE incidentally
 *  invoked even though nothing asserts their outcome. A NULL/uninitialised snd_seq_cursor
 *  would fault on the very first LODSB. There is no captured real MIDI stream to point it
 *  at (out of scope; not fabricated), so it defaults to a zero-filled scratch buffer —
 *  memory-safe, never inspected/asserted by any comparator, NOT a stand-in for real
 *  track data. */
static u8 snd_seq_scratch_stream[4096];
u8 *snd_seq_cursor = snd_seq_scratch_stream;   /* engine DS:SI — the live MIDI-track cursor */

/* ── FUN_1000_91cf (1000:91cf) — snddrv_dispatch_b_mode0: MIDI 0xF7 skip (device mode 0) ──
 *  0xF7 (SysEx continuation) handler for driver mode 0 (PC-speaker/silent): read a
 *  length byte from the MIDI stream and skip that many bytes; the event is not
 *  otherwise acted on. asm 1000:91cf verbatim: LODSB (len=*SI,SI++); XOR AH,AH;
 *  ADD SI,AX. Register-entry (DS:SI) — see the MIDI-dispatch-backends note above. */
void FUN_1000_91cf(void)
{
    u8 len = *snd_seq_cursor;
    snd_seq_cursor++;
    snd_seq_cursor += len;
}

/* ── FUN_1000_8e48 (1000:8e48) — snddrv_dispatch_b_mode1: MIDI 0xF7 skip (device mode 1) ──
 *  Identical body to _mode0 (91cf) — the 0xF7 skip is device-independent for modes 0/1.
 *  asm 1000:8e48 verbatim (byte-identical to 91cf). Kept as its OWN function (not
 *  merged/shared) per the "no collapsing near-duplicate mode backends" rule. */
void FUN_1000_8e48(void)
{
    u8 len = *snd_seq_cursor;
    snd_seq_cursor++;
    snd_seq_cursor += len;
}

/* ── FUN_1000_91d7 (1000:91d7) — snddrv_dispatch_c_mode0: MIDI 0xF0 skip (device mode 0) ──
 *  0xF0 (SysEx) handler for driver mode 0: same length-prefixed skip as dispatch_b's
 *  mode0/1 (91cf/8e48) — asm 1000:91d7 verbatim (byte-identical body, different event
 *  class / call site). Kept as its own function per the no-collapsing rule. */
void FUN_1000_91d7(void)
{
    u8 len = *snd_seq_cursor;
    snd_seq_cursor++;
    snd_seq_cursor += len;
}

/* ── FUN_1000_8e50 (1000:8e50) — snddrv_dispatch_c_mode1: MIDI 0xF0 skip (device mode 1) ──
 *  Same length-prefixed skip, dispatch_c's mode-1 variant. asm 1000:8e50 verbatim. */
void FUN_1000_8e50(void)
{
    u8 len = *snd_seq_cursor;
    snd_seq_cursor++;
    snd_seq_cursor += len;
}

/* ── FUN_1000_8af6 (1000:8af6) — snddrv_dispatch_b_mode4: MIDI 0xF7 busy-wait (mode 4) ──
 *  OPL/mode-4 handling of the 0xF7 event: reads TWO stream bytes (len, then a second
 *  byte that is read but never used) and busy-waits snd_busy_delay(len-1). asm 1000:8af6
 *  verbatim: LODSB (CL=len); LODSB (discard); DEC CL; CALL 0x872e. The real CX's high
 *  byte (CH) is whatever the (unreconstructed) MIDI player's own CX held at the time —
 *  not modelled (never exercised; see the note above) — treated as 0. */
void FUN_1000_8af6(void)
{
    u8 len;

    len = *snd_seq_cursor;
    snd_seq_cursor++;
    (void)*snd_seq_cursor;         /* LODSB — consumed, value unused */
    snd_seq_cursor++;
    snd_busy_delay((u16)(u8)(len - 1));
}

/* ── FUN_1000_8b04 (1000:8b04) — snddrv_dispatch_c_mode4: MIDI 0xF0 busy-wait (mode 4) ──
 *  OPL/mode-4 handling of the 0xF0 event: reads ONE stream byte (a direct byte read +
 *  manual SI++, NOT LODSB) and busy-waits snd_busy_delay(len) — no decrement, unlike
 *  the _b_mode4 sibling. asm 1000:8b04 verbatim: MOV CL,[SI]; INC SI; CALL 0x872e. */
void FUN_1000_8b04(void)
{
    u8 len = *snd_seq_cursor;
    snd_seq_cursor++;
    snd_busy_delay((u16)len);
}

/* ── FUN_1000_8b0d (1000:8b0d) — snddrv_dispatch_d_mode4: MIDI channel-msg busy-wait (mode 4) ──
 *  OPL/mode-4 handling of a channel voice message (dispatch_d's event class, status
 *  nibble >= 0x80): default the channel nibble from the per-track byte (CS:[BX+0x80])
 *  when the low nibble is 0, busy_delay(2), or busy_delay(1) if the event's upper
 *  nibble is 0xC0 (program change). No MIDI-stream (SI) read at all — only AL/BX.
 *  asm 1000:8b0d verbatim: TEST AL,0xf; JNZ +; OR AL,[BX+0x80]; MOV CX,2; MOV AH,AL;
 *  AND AH,0xf0; CMP AH,0xc0; JNZ +; DEC CX; CALL 0x872e. */
void FUN_1000_8b0d(void)
{
    u8  al = snd_seq_event_al;
    u16 cx = 2;

    if ((al & 0xf) == 0) {
        al = (u8)(al | snd_seq_default_chan);
    }
    if ((u8)(al & 0xf0) == 0xc0) {
        cx = 1;
    }
    snd_busy_delay(cx);
}

/* ── FUN_1000_91df (1000:91df) — snddrv_dispatch_d_mode0: MIDI channel-msg (PC-spk mode 0) ──
 *  Channel voice message handler for driver mode 0 (PC-speaker/silent device). 0xC0
 *  (program change, tested on the ORIGINAL undefaulted AL) tail-calls the out-of-scope
 *  seq_set_channel_param (1000:922c). Otherwise default the channel nibble from
 *  CS:[BX+0x80] when 0, then index snd_voice_table[channel] (CODE 0x83cc — the SAME
 *  table pc_speaker_silence zeroes): on 0x90 (note-on; 2 stream bytes: note, velocity)
 *  store the note (or 0 if velocity==0); on 0x80 (note-off; 2 stream bytes, both
 *  discarded) store 0; any other status: skip 2 stream bytes, no write. asm 1000:91df
 *  verbatim. RECONSTRUCTION FIDELITY: channel is a 4-bit nibble (0..15) but
 *  snd_voice_table is 15 bytes (SND_VOICE_TABLE_LEN, matching pc_speaker_silence's
 *  clear-loop count) — channel==15 is a genuine 1-byte-OOB index in the ORIGINAL image
 *  too (writes an unlabeled CODE-segment byte at 0x83cc+15, not any named table); not
 *  bounds-checked here (that would invent a safety net the binary doesn't have) — moot
 *  in practice since this leaf is never invoked (not oracle-exercised; see above). */
void FUN_1000_91df(void)
{
    u8 al = snd_seq_event_al;
    u8 chan;
    u8 note;
    u8 velocity;

    if ((u8)(al & 0xf0) == 0xc0) {
        seq_set_channel_param();
        return;
    }
    if ((al & 0xf) == 0) {
        al = (u8)(al | snd_seq_default_chan);
    }
    chan = (u8)(al & 0xf);
    al = (u8)(al & 0xf0);
    if (al == 0x90) {
        note = *snd_seq_cursor; snd_seq_cursor++;         /* LODSB — note number */
        velocity = *snd_seq_cursor; snd_seq_cursor++;     /* LODSB — velocity    */
        snd_voice_table[chan] = (velocity == 0) ? 0 : note;
    } else if (al == 0x80) {
        (void)*snd_seq_cursor; snd_seq_cursor++;          /* LODSB — note (unused)     */
        (void)*snd_seq_cursor; snd_seq_cursor++;          /* LODSB — velocity (unused) */
        snd_voice_table[chan] = 0;
    } else {
        (void)*snd_seq_cursor; snd_seq_cursor++;          /* LODSB — skip 2 stream bytes */
        (void)*snd_seq_cursor; snd_seq_cursor++;
    }
}

/* ── FUN_1000_8e58 (1000:8e58) — snddrv_dispatch_d_mode1: MIDI channel-msg (OPL mode 1) ──
 *  Channel voice message handler for driver mode 1 (OPL/AdLib): default the channel
 *  nibble from CS:[BX+0x80] when 0. If channel > 8 (only 9 OPL voices, 0..8) OR the
 *  status nibble is none of 0xC0/0x90/0x80, skip the event's data bytes without acting
 *  (1 byte for a 0xC0-shaped status, else 2) — asm 1000:8e58's 8e83 tail is the SAME
 *  code for both the channel>8 skip AND the unrecognized-status fallthrough. Else
 *  dispatch: 0xC0 (program change) -> the out-of-scope midi_emit_voice_msg_w3
 *  (1000:8e93); 0x90 (note-on) -> the out-of-scope opl_event_note_on (1000:8ea3, which
 *  itself calls the ALREADY-PORTED opl_play_note); 0x80 (note-off) -> read the CODE
 *  0x80cc shadow byte for register (0xb0+channel), clear its key-on bit (AND 0xdf), and
 *  call the ALREADY-PORTED opl_write_reg directly, then discard 2 stream bytes
 *  (note+velocity). asm 1000:8e58 verbatim. */
void FUN_1000_8e58(void)
{
    u8 al = snd_seq_event_al;
    u8 chan;
    u8 status;

    if ((al & 0xf) == 0) {
        al = (u8)(al | snd_seq_default_chan);
    }
    chan = (u8)(al & 0xf);
    status = (u8)(al & 0xf0);
    if (chan <= 8) {
        if (status == 0xc0) {
            midi_emit_voice_msg_w3();
            return;
        } else if (status == 0x90) {
            opl_event_note_on();
            return;
        } else if (status == 0x80) {
            u8 reg = (u8)(0xb0 + chan);
            u8 shadow = (u8)(opl_reg_shadow_80cc[reg] & 0xdf);
            opl_write_reg(reg, shadow);
            (void)*snd_seq_cursor; snd_seq_cursor++;      /* LODSB — note (unused)     */
            (void)*snd_seq_cursor; snd_seq_cursor++;      /* LODSB — velocity (unused) */
            return;
        }
    }
    /* channel > 8, OR an unrecognized status nibble: skip the event's data bytes
     *  without acting — 1 byte for a program-change-shaped status, else 2. */
    snd_seq_cursor++;
    if (status != 0xc0) {
        snd_seq_cursor++;
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  L1 event-wrapper LUTs — exact image bytes (BUMPY_unpacked.exe DGROUP offsets).
 *  Each table is 0x30 bytes (the established sound-LUT size, cf. player.c
 *  move_sound_lut_*).  Indexed by the wrapper's event/state global. */
u8 action_sound_lut_opl_260e[0x100] = {
    0x00,0x13,0x14,0x15,0x16,0x17,0x17,0x18,0x19,0x1a,0x1b,0x00,0x1c,0x1d,0x1e,0x1f,
    0x20,0x21,0x11,0x00,0x00,0x00,0x00,0x22,0x22,0x00,0x23,0x23,0x23,0x23,0x24,0x00,
    0x00,0x25,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x01,0x0e,0x0b,0x03,0x03,0x05,0x06,0x07,0x01,0x00,0x04,0x01,0x01,0x01,
    0x01,0x08,0x12,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x0b,0x0b,0x0b,0x0b,0x0b,0x00,
    0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x04,0x04,0x04,0x00,0x00,0x00,0x04,0x04,0x04,0x04,0x00,0x04,0x04,0x04,0x00,
    0x04,0x00,0x00,0x04,0x04,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x02,0x02,0x02,0x00,0x00,0x00,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x00,
    0x02,0x00,0x00,0x02,0x02,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x03,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x00,0x03,
    0x00,0x03,0x11,0x03,0x03,0x03,0x00,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x11,0x03,
    0x03,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x09,0x09,0x09,0x09,0x0b,0x0b,0x0b,0x00,0x0b,
};
u8 action_sound_lut_std_263e[0x100] = {
    0x00,0x01,0x01,0x0e,0x0b,0x03,0x03,0x05,0x06,0x07,0x01,0x00,0x04,0x01,0x01,0x01,
    0x01,0x08,0x12,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x0b,0x0b,0x0b,0x0b,0x0b,0x00,
    0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x04,0x04,0x04,0x00,0x00,0x00,0x04,0x04,0x04,0x04,0x00,0x04,0x04,0x04,0x00,
    0x04,0x00,0x00,0x04,0x04,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x02,0x02,0x02,0x00,0x00,0x00,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x00,
    0x02,0x00,0x00,0x02,0x02,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x03,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x00,0x03,
    0x00,0x03,0x11,0x03,0x03,0x03,0x00,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x11,0x03,
    0x03,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x09,0x09,0x09,0x09,0x0b,0x0b,0x0b,0x00,0x0b,
    0x00,0x09,0x12,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x00,0x0b,0x0b,0x0b,0x0b,0x12,0x0b,
    0x0b,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x2f,0x2f,0x2f,0x2f,0x2f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
u8 state_sound_lut_opl_26ce[0x100] = {
    0x00,0x03,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x00,0x03,
    0x00,0x03,0x11,0x03,0x03,0x03,0x00,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x11,0x03,
    0x03,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x09,0x09,0x09,0x09,0x0b,0x0b,0x0b,0x00,0x0b,
    0x00,0x09,0x12,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x00,0x0b,0x0b,0x0b,0x0b,0x12,0x0b,
    0x0b,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x2f,0x2f,0x2f,0x2f,0x2f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x2f,0x2f,0x2f,0x2f,0x2f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x0d,0x0d,0x0d,0x0d,0x0d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x0d,0x0d,0x0d,0x0d,0x0d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x0e,0x10,0x10,0x10,0x12,0x0e,0x0a,0x0a,
    0x0a,0x0a,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,
    0x00,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0e,0x10,0x10,0x11,0x12,0x0e,0x0a,0x0a,
    0x0a,0x0a,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0b,
    0x00,0x00,0x23,0x7f,0x41,0x7f,0x28,0x7f,0x26,0x7f,0x40,0x7f,0x3f,0x7f,0x3e,0x7f,
    0x27,0x7f,0x54,0x7f,0x43,0x7f,0x47,0x7f,0x41,0x7f,0x4d,0x7f,0x3d,0x7f,0x29,0x7f,
};
u8 state_sound_lut_std_26fe[0x100] = {
    0x00,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x09,0x09,0x09,0x09,0x0b,0x0b,0x0b,0x00,0x0b,
    0x00,0x09,0x12,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x00,0x0b,0x0b,0x0b,0x0b,0x12,0x0b,
    0x0b,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x2f,0x2f,0x2f,0x2f,0x2f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x2f,0x2f,0x2f,0x2f,0x2f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x0d,0x0d,0x0d,0x0d,0x0d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x0d,0x0d,0x0d,0x0d,0x0d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x0e,0x10,0x10,0x10,0x12,0x0e,0x0a,0x0a,
    0x0a,0x0a,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,
    0x00,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0e,0x10,0x10,0x11,0x12,0x0e,0x0a,0x0a,
    0x0a,0x0a,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0b,
    0x00,0x00,0x23,0x7f,0x41,0x7f,0x28,0x7f,0x26,0x7f,0x40,0x7f,0x3f,0x7f,0x3e,0x7f,
    0x27,0x7f,0x54,0x7f,0x43,0x7f,0x47,0x7f,0x41,0x7f,0x4d,0x7f,0x3d,0x7f,0x29,0x7f,
    0x2a,0x7f,0x5b,0x7f,0x2e,0x7f,0x25,0x7f,0x38,0x7f,0x4f,0x7f,0x28,0x7f,0x26,0x7f,
    0x40,0x7f,0x3f,0x7f,0x3e,0x7f,0x46,0x7f,0x60,0x7f,0x44,0x7f,0x47,0x7f,0x31,0x7f,
    0x38,0x7f,0x42,0x7f,0x3d,0x7f,0x29,0x7f,0x2c,0x7f,0x2e,0x7f,0x4e,0x7f,0x49,0x7f,
};
u8 contact_sound_lut_opl_276e[0x100] = {
    0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x0e,0x10,0x10,0x10,0x12,0x0e,0x0a,0x0a,
    0x0a,0x0a,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,
    0x00,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0e,0x10,0x10,0x11,0x12,0x0e,0x0a,0x0a,
    0x0a,0x0a,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0b,
    0x00,0x00,0x23,0x7f,0x41,0x7f,0x28,0x7f,0x26,0x7f,0x40,0x7f,0x3f,0x7f,0x3e,0x7f,
    0x27,0x7f,0x54,0x7f,0x43,0x7f,0x47,0x7f,0x41,0x7f,0x4d,0x7f,0x3d,0x7f,0x29,0x7f,
    0x2a,0x7f,0x5b,0x7f,0x2e,0x7f,0x25,0x7f,0x38,0x7f,0x4f,0x7f,0x28,0x7f,0x26,0x7f,
    0x40,0x7f,0x3f,0x7f,0x3e,0x7f,0x46,0x7f,0x60,0x7f,0x44,0x7f,0x47,0x7f,0x31,0x7f,
    0x38,0x7f,0x42,0x7f,0x3d,0x7f,0x29,0x7f,0x2c,0x7f,0x2e,0x7f,0x4e,0x7f,0x49,0x7f,
    0x53,0x7f,0x55,0x7f,0x58,0x7f,0x62,0x7f,0x5a,0x7f,0x63,0x7f,0x63,0x7f,0x45,0x7f,
    0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,0x01,0x01,0xff,0x00,0x01,0x00,0x11,0x28,
    0x3b,0x10,0x05,0x00,0x00,0x00,0x00,0x01,0x01,0xff,0x01,0x00,0x20,0x28,0x3b,0x10,
    0x02,0x00,0x03,0x00,0x02,0x01,0x04,0x00,0x00,0x00,0x01,0x01,0xff,0x00,0x01,0x00,
    0x2e,0x28,0x3b,0x10,0xb8,0x00,0x00,0x01,0x01,0xff,0x01,0x00,0x42,0x28,0x3b,0x10,
    0x0f,0x00,0x00,0x00,0x00,0x07,0x07,0xff,0x03,0x00,0x4e,0x28,0x3b,0x10,0x0e,0x00,
    0x00,0x00,0x00,0x07,0x07,0xff,0x03,0x00,0x5c,0x28,0x3b,0x10,0x08,0x00,0x09,0x00,
};
u8 contact_sound_lut_std_278e[0x100] = {
    0x00,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0e,0x10,0x10,0x11,0x12,0x0e,0x0a,0x0a,
    0x0a,0x0a,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0b,
    0x00,0x00,0x23,0x7f,0x41,0x7f,0x28,0x7f,0x26,0x7f,0x40,0x7f,0x3f,0x7f,0x3e,0x7f,
    0x27,0x7f,0x54,0x7f,0x43,0x7f,0x47,0x7f,0x41,0x7f,0x4d,0x7f,0x3d,0x7f,0x29,0x7f,
    0x2a,0x7f,0x5b,0x7f,0x2e,0x7f,0x25,0x7f,0x38,0x7f,0x4f,0x7f,0x28,0x7f,0x26,0x7f,
    0x40,0x7f,0x3f,0x7f,0x3e,0x7f,0x46,0x7f,0x60,0x7f,0x44,0x7f,0x47,0x7f,0x31,0x7f,
    0x38,0x7f,0x42,0x7f,0x3d,0x7f,0x29,0x7f,0x2c,0x7f,0x2e,0x7f,0x4e,0x7f,0x49,0x7f,
    0x53,0x7f,0x55,0x7f,0x58,0x7f,0x62,0x7f,0x5a,0x7f,0x63,0x7f,0x63,0x7f,0x45,0x7f,
    0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,0x01,0x01,0xff,0x00,0x01,0x00,0x11,0x28,
    0x3b,0x10,0x05,0x00,0x00,0x00,0x00,0x01,0x01,0xff,0x01,0x00,0x20,0x28,0x3b,0x10,
    0x02,0x00,0x03,0x00,0x02,0x01,0x04,0x00,0x00,0x00,0x01,0x01,0xff,0x00,0x01,0x00,
    0x2e,0x28,0x3b,0x10,0xb8,0x00,0x00,0x01,0x01,0xff,0x01,0x00,0x42,0x28,0x3b,0x10,
    0x0f,0x00,0x00,0x00,0x00,0x07,0x07,0xff,0x03,0x00,0x4e,0x28,0x3b,0x10,0x0e,0x00,
    0x00,0x00,0x00,0x07,0x07,0xff,0x03,0x00,0x5c,0x28,0x3b,0x10,0x08,0x00,0x09,0x00,
    0x0a,0x0b,0x0c,0x00,0x00,0x0d,0x07,0x07,0xff,0x00,0x03,0x00,0x6a,0x28,0x3b,0x10,
    0x19,0x00,0x00,0x00,0x14,0x00,0x1a,0x00,0x1b,0x00,0x1c,0x00,0x1d,0x1d,0xff,0x00,
};

/* ── play_action_sound (1000:63be) ───────────────────────────────────────────────
 *  Unless prev_game_mode is 3 or 0xf, look up the sound id for the pending action in
 *  the OPL table (device==4) else the standard table, indexed by p1_pending_action;
 *  if non-zero, play it.  Stack-probe prologue omitted (project convention). */
void play_action_sound(void)
{
    u8 sound_id;

    if ((prev_game_mode != 3) && (prev_game_mode != 0xf)) {
        if (sound_device_state == 4) {
            sound_id = action_sound_lut_opl_260e[p1_pending_action];
        } else {
            sound_id = action_sound_lut_std_263e[p1_pending_action];
        }
        if (sound_id != 0) {
            play_sound(sound_id);
        }
    }
}

/* ── play_contact_sound (1000:640c) ──────────────────────────────────────────────
 *  Look up the sound id for p1_contact_code (OPL table 0x276e if device==4 else std
 *  0x278e); if non-zero, play it.  For contact codes 0xe..0x11 also runs FUN_1000_6183
 *  (an out-of-scope entity sweep, STUBBED). */
void play_contact_sound(void)
{
    u8 sound_id;

    if (sound_device_state == 4) {
        sound_id = contact_sound_lut_opl_276e[p1_contact_code];
    } else {
        sound_id = contact_sound_lut_std_278e[p1_contact_code];
    }
    if (sound_id != 0) {
        play_sound(sound_id);
    }
    if ((0xd < p1_contact_code) && (p1_contact_code < 0x12)) {
        FUN_1000_6183();
    }
}

/* ── play_exit_sound (1000:6305) ─────────────────────────────────────────────────
 *  Level-exit/door sound: 0xd on OPL (device==4) else 3.  Always plays. */
void play_exit_sound(void)
{
    u8 sound_id;

    if (sound_device_state == 4) {
        sound_id = 0xd;
    } else {
        sound_id = 3;
    }
    play_sound(sound_id);
}

/* ── play_pickup_sound (1000:645d) ───────────────────────────────────────────────
 *  Item-pickup sound: 0x2c on OPL (device==4) else 0xb.  Always plays. */
void play_pickup_sound(void)
{
    u8 sound_id;

    if (sound_device_state == 4) {
        sound_id = 0x2c;
    } else {
        sound_id = 0xb;
    }
    play_sound(sound_id);
}

/* ── play_event_sound_64c1 (1000:64c1) ───────────────────────────────────────────
 *  Fixed event sound: 0x21 on OPL (device==4) else 0xe.  Always plays. */
void play_event_sound_64c1(void)
{
    u8 sound_id;

    if (sound_device_state == 4) {
        sound_id = 0x21;
    } else {
        sound_id = 0xe;
    }
    play_sound(sound_id);
}

/* ── play_state_sound_79b9 (1000:647e) ───────────────────────────────────────────
 *  Look up the sound id for the tile-below-player state (OPL table 0x26ce if device==4
 *  else std 0x26fe), indexed by tile_below_player; if non-zero, play it; then run
 *  p1_try_trigger_pending_action (1000:654e, RECONSTRUCTED in player.c). */
void play_state_sound_79b9(void)
{
    u8 sound_id;

    if (sound_device_state == 4) {
        sound_id = state_sound_lut_opl_26ce[tile_below_player];
    } else {
        sound_id = state_sound_lut_std_26fe[tile_below_player];
    }
    if (sound_id != 0) {
        play_sound(sound_id);
    }
    p1_try_trigger_pending_action();
}

/* ════════════════════════════════════════════════════════════════════════════
 *  L2 — sound-device state machine.
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── sound_select_device (1000:6de3) ─────────────────────────────────────────────
 *  Unless the device is force-muted (sound_device_state == 0x8000), initialise the
 *  driver (snddrv_init returns a device-mask) and select a device from
 *  sound_device_state & mask.  Stack-probe prologue omitted. */
void sound_select_device(void)
{
    u16 device_mask;

    if ((u16)sound_device_state != 0x8000) {
        device_mask = snddrv_init();
        select_sound_device_from_mask((u16)sound_device_state & device_mask);
    }
}

/* ── snddrv_init (1000:88e5) ──────────────────────────────────────────────────────
 *  One-time driver init (guard sound_init_state 0x557a==0): set state=1, run the MPU
 *  reset + snddrv_init_substep, set snddrv_mode=1 + sound_active_device_mask=1, return
 *  a status bitmask (0=ok; |4 / |1 on a sub-step failure).  The decomp's `substep_ok`
 *  is a LOCAL flag the ORIGINAL never actually reassigns from either callee's return
 *  (both are called for effect, their results discarded) — it stays true, so the
 *  failure ORs never fire; the decomp's dead `if(!substep_ok)` arms are preserved 1:1.
 *  mpu401_reset_to_uart + snddrv_init_substep are PORTED (Task A3): mpu401_reset_to_uart
 *  is gated on mpu401_present (defaults 0 — early-return no-op here); snddrv_init_substep
 *  only touches midi_seq_step_active (not in the SND_SNAP) — so wiring in their real
 *  bodies does not perturb this fn's already-validated device_init semantic-state
 *  record. */
u16 snddrv_init(void)
{
    u16 status;
    u8  substep_ok;

    status = 0xffff;
    if (sound_init_state == 0) {
        sound_init_state = 1;
        substep_ok = 1;
        snd_init_substep_5584 = 1;        /* DAT_203b_5584 = 1 */
        mpu401_reset_to_uart();
        status = 0;
        if (!substep_ok) {
            status = 4;
            substep_ok = 0;
        }
        snddrv_init_substep();
        if (!substep_ok) {
            status = status | 1;
        }
        snddrv_mode = 1;
        sound_active_device_mask = 1;
    }
    return status;
}

/* ── select_sound_device_from_mask (1000:891e) ───────────────────────────────────
 *  If init state==1, advance to 2 and scan up to 16 bits of the candidate mask; the
 *  first set bit selects the device (1<<bit -> snddrv_mode + sound_active_device_mask),
 *  returned.  If no bit is set, reset the timer slot (the asm's set_timer_slot_raw call
 *  with channel=1, value=0x64, cb=0x9136:0x1000), clear the select scratch (0x83ee/83ef),
 *  and clear the selection.  Returns the selected device (or -1 if state != 1). */
int select_sound_device_from_mask(u16 param_1)
{
    u16 uVar1;
    int bit_index;

    bit_index = -1;
    if (sound_init_state == 1) {
        sound_init_state = 2;
        bit_index = 0;
        do {
            uVar1 = param_1 & 1;
            param_1 = param_1 >> 1;
            if (uVar1 != 0) {
                snddrv_mode = (u16)(1 << ((u8)bit_index & 0x1f));
                sound_init_state = 2;
                sound_active_device_mask = snddrv_mode;
                return (int)snddrv_mode;
            }
            bit_index = bit_index + 1;
        } while (bit_index != 0x10);
        /* no candidate bit set: reset path.  asm 1000:895c sets BX=1, AX=0x64,
         *  CX:DX=0x9136:0x1000 then CALL 0x7df9 (set_timer_slot_raw). */
        set_timer_slot_raw(1, 0x64, 0x9136, 0x1000);
        bit_index = 0;
        snd_select_scratch_83ee = 0;      /* DAT_1000_83ee = 0 (byte) */
        snd_select_scratch_83ef = 0;      /* DAT_1000_83ef = 0 (word) */
        snddrv_mode = (u16)bit_index;
        sound_active_device_mask = (u16)bit_index;
    }
    return bit_index;
}

/* ── snddrv_dispatch_a (1000:85b5) ───────────────────────────────────────────────
 *  Fan out by snddrv_mode (0/4/1) to the L4 backend (all STUBBED → T6). */
void snddrv_dispatch_a(void)
{
    if (snddrv_mode == 0) {
        pc_speaker_silence();
    } else if (snddrv_mode == 4) {
        FUN_1000_8ad0();
    } else if (snddrv_mode == 1) {
        FUN_1000_8e2f();
    }
}

/* ── snddrv_dispatch_b (1000:85db) ───────────────────────────────────────────────
 *  As _a; the decomp returns AX (an undefined register pass-through).  The backends are
 *  STUBBED (void); the return is the engine's ambient AX, not part of the validated
 *  state — modelled as 0. */
u16 snddrv_dispatch_b(void)
{
    if (snddrv_mode == 0) {
        FUN_1000_91cf();
    } else if (snddrv_mode == 4) {
        FUN_1000_8af6();
    } else if (snddrv_mode == 1) {
        FUN_1000_8e48();
    }
    return 0;   /* decomp returns the ambient in_AX; backends STUBBED → not validated */
}

/* ── snddrv_dispatch_c (1000:8600) ───────────────────────────────────────────────── */
void snddrv_dispatch_c(void)
{
    if (snddrv_mode == 0) {
        FUN_1000_91d7();
    } else if (snddrv_mode == 4) {
        FUN_1000_8b04();
    } else if (snddrv_mode == 1) {
        FUN_1000_8e50();
    }
}

/* ── snddrv_dispatch_d (1000:8626) ───────────────────────────────────────────────── */
void snddrv_dispatch_d(void)
{
    if (snddrv_mode == 0) {
        FUN_1000_91df();
    } else if (snddrv_mode == 4) {
        FUN_1000_8b0d();
    } else if (snddrv_mode == 1) {
        FUN_1000_8e58();
    }
}

/* ── snd_busy_delay (1000:872e) ──────────────────────────────────────────────────
 *  Busy-wait: call the timing primitive FUN_1000_89e2 CX+1 times.  This is a naked/asm
 *  routine — entry AL feeds the first FUN_89e2 (MOV AH,AL), then a LODSB/LOOP over CX
 *  reads bytes from DS:SI and calls FUN_89e2 for each.  The decomp models it as the
 *  CX+1-iteration delay loop (the LODSB source bytes are consumed by the STUBBED timing
 *  primitive and do not affect the validated state).  Modelled with an explicit count
 *  parameter (the engine's CX); the LODSB byte stream is not reconstructed (→ T6 with the
 *  L4 timing port).  RECONSTRUCTION FIDELITY: register-args asm routine; see report. */
void snd_busy_delay(u16 count)
{
    FUN_1000_89e2();                       /* AH=AL; CALL 0x89e2 (first iteration) */
    do {
        FUN_1000_89e2();                   /* LODSB; MOV AH,AL; CALL 0x89e2 */
        count = count - 1;
    } while (count != 0);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  L3 — timer-table management.
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── set_timer_slot_raw (1000:7df9 + 7e62) — the L3 slot-table writer ─────────────
 *  Validate channel (0..3) and value (0..499); if OK write the slot at
 *  (channel+2)*8 + 0x549c.  Per the asm (7e62 MOV [BX+6],CX; MOV [BX+4],DX, where the
 *  caller passed CX=offset, DX=segment via LDS): the slot layout is
 *  {value@0, 0@2, cb_seg@4, cb_off@6} — i.e. SEG at +4, OFF at +6 (the reverse of
 *  arm_timer_callback's {off@4, seg@6}).  Returns 1 on a valid store, else 0.  Reached by
 *  set_timer_slot, the L3 schedulers, and select_sound_device_from_mask's reset path. */
int set_timer_slot_raw(int channel, int value, u16 cb_off, u16 cb_seg)
{
    u16 idx;

    if ((-1 < channel) && (channel < 4)) {
        if ((-1 < value) && (value < 500)) {
            idx = (u16)((channel + 2) * 8);          /* (BX+2)*8 + 0x549c */
            *(u16 *)(snd_timer_slot_table + idx + 0) = (u16)value;
            *(u16 *)(snd_timer_slot_table + idx + 2) = 0;
            *(u16 *)(snd_timer_slot_table + idx + 4) = cb_seg;   /* DX (segment) */
            *(u16 *)(snd_timer_slot_table + idx + 6) = cb_off;   /* CX (offset) */
            return 1;
        }
    }
    return 0;
}

/* ── set_timer_slot (1000:7de8) — validated slot store ────────────────────────────
 *  The Ghidra prototype labels the args (value, slot_index) but the disassembly
 *  (1000:7deb MOV BX,[BP+4]; 7dee MOV AX,[BP+6]; LDS CX,[BP+8]) passes [BP+4] as the
 *  CHANNEL (BX, validated 0..3 in 7df9) and [BP+6] as the VALUE (AX, validated 0..499),
 *  with [BP+8] the far callback (CX:DX).  Reconstructed with the asm semantics; tail into
 *  set_timer_slot_raw.  Returns 1 on a valid store, else 0. */
int set_timer_slot(int channel, int value, u16 cb_off, u16 cb_seg)
{
    return set_timer_slot_raw(channel, value, cb_off, cb_seg);
}

/* ── arm_timer_callback (1000:7f2b) — install a channel callback into table 0x5516 ──
 *  Validate channel (0..3); write the channel's 8-byte slot at 0x5516 + channel*8:
 *  reload@2 = reload, current@0 = 0, cb_off@4 / cb_seg@6 = the far callback.  Returns
 *  1 on success, else 0.  callback is a far pointer (off+seg) per the asm LES [BP+8]. */
int arm_timer_callback(int channel, int reload, u16 cb_off, u16 cb_seg)
{
    u16 idx;

    if ((-1 < channel) && (channel < 4)) {
        idx = (u16)(channel * 8);                    /* 0x5516 + channel*8 */
        *(u16 *)(snd_timer_cb_table + idx + 2) = (u16)reload;   /* reload */
        *(u16 *)(snd_timer_cb_table + idx + 0) = 0;             /* current */
        *(u16 *)(snd_timer_cb_table + idx + 4) = cb_off;        /* far cb off */
        *(u16 *)(snd_timer_cb_table + idx + 6) = cb_seg;        /* far cb seg (ES) */
        return 1;
    }
    return 0;
}

/* ── disable_timer_callback (1000:7f65) — disable a channel in table 0x5516 ─────────
 *  Validate channel (0..3); set the channel's reload@2 and current@0 words to 0xffff.
 *  Returns 1 on success, else 0. */
int disable_timer_callback(int channel)
{
    u16 idx;

    if ((-1 < channel) && (channel < 4)) {
        idx = (u16)(channel * 8);
        *(u16 *)(snd_timer_cb_table + idx + 2) = 0xffff;   /* reload */
        *(u16 *)(snd_timer_cb_table + idx + 0) = 0xffff;   /* current */
        return 1;
    }
    return 0;
}

/* ── get_timer_slot_field (1000:7e3d) — read a slot value from table 0x549c ─────────
 *  Validate slot_index (0..3); return the word at (slot_index+2)*8 + 0x549c (the value
 *  set_timer_slot_raw stored), else 0. */
int get_timer_slot_field(int slot_index)
{
    u16 idx;

    if ((-1 < slot_index) && (slot_index < 4)) {
        idx = (u16)((slot_index + 2) * 8);
        return (int)*(u16 *)(snd_timer_slot_table + idx);
    }
    return 0;
}

/* ── timer_restore (1000:7fde) — thunk to the timer teardown/restore ──────────────
 *  Thunk to timer_teardown_restore (restore the PIT int vector via DOS int 21h +
 *  reprogram the PIT).  timer_teardown_restore is PORTED (Task A3); its guarded body
 *  is a provable no-op here (isr_installed_flag defaults 0 and this fn's own call site
 *  passes no args), so wiring it in does not perturb this fn's already-validated
 *  t4_l3_timer_table semantic-state records. */
void timer_restore(void)
{
    timer_teardown_restore();
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Phase-6 T5 — L4 HARDWARE BACKENDS (1:1 ports of the engine's port-I/O drivers).
 *
 *  These issue the engine's real x86 OUT/IN to the sound hardware ports:
 *    PC speaker / PIT : 0x61  (gate + speaker-enable, low 2 bits)
 *    MPU-401          : 0x330 data / 0x331 status (DSR bit 0x40)
 *    OPL2 / AdLib     : 0x388 addr/status / 0x389 data
 *  On the real DOS build (wcc) outp()/inp() (<conio.h>) hit the hardware; on the host
 *  replay harness (tools/sound_ctest.c) they are the capture(OUT)/replay(IN) shims, so
 *  the reconstructed driver's OUT sequence is diffed against the engine's captured one
 *  and its IN polls see the EXACT bytes the engine saw (the port-write-sequence gate).
 *
 *  Every body below is transcribed from the live disassembly (cited per fn); the Turbo-C
 *  stack-probe / register-save prologue is omitted (project convention).  No body reads
 *  un-captured CPU registers for its PORT sequence except where noted (89e2/8a07/9007/905d
 *  take a register/arg byte — documented at each, and at the harness wrappers).
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── OPL2 register shadow (CODE 0x80cc) + the runtime freq/channel tables (DGROUP) ──
 *  opl_write_reg mirrors each written value into a shadow table at CODE 0x80cc + reg;
 *  opl_play_note reads per-note F-number/block tables (DGROUP 0x5593 / 0x559c) and
 *  per-channel data (0x55b4 / 0x5614) that an OPL-init routine fills at RUNTIME (the
 *  static image leaves them zero — they are BSS).  Owned here (no other TU defines
 *  these addresses); zero-initialised.  Because the engine populates the freq tables at
 *  runtime and the SND_SNAP does NOT capture them, opl_play_note's exact note OUTs are
 *  not host-reproducible from the trace — opl_play_note (905d) + FUN_8e2f (8e2f, which
 *  drives it) are therefore a DOCUMENTED port-write-gate EXCLUSION (ported 1:1 for the
 *  link + faithfulness, registered UNPORTED).  See docs/reconstruction-fidelity.md. */
u8 opl_reg_shadow_80cc[0x100];     /* CODE   0x80cc — OPL register write-back shadow */
u8 opl_fnum_lo_5593[0x100];        /* DGROUP 0x5593 — per-note F-number low byte (runtime) */
u8 opl_fnum_hi_559c[0x100];        /* DGROUP 0x559c — per-note F-number/block word (runtime) */
u8 opl_chan_data_55b4[0x100];      /* DGROUP 0x55b4 — per-channel feedback/connection (runtime) */
u8 opl_chan_idx_5614[0x100];       /* DGROUP 0x5614 — per-channel block index (runtime) */

/* ── MPU-401 register-byte recovery (host port-write gate) ───────────────────────────
 *  FUN_89e2 writes the byte in register AH; FUN_8a07 writes args sample_lo/sample_hi;
 *  opl_write_reg takes reg=AH/val=AL.  The engine passes these in registers/args the
 *  SND_SNAP does not serialize, so on the host the replay harness recovers them from the
 *  record's OUT events and publishes them via these file-scope inputs before the call (the
 *  driver still must emit them at the right PORTS in the right ORDER for the gate to pass —
 *  a wrong port/order/extra/missing write diverges).  On the real wcc build these are
 *  unused (the byte arrives in AH from the caller); the host wrappers set them.  Defaults
 *  reproduce the engine's first-seen sequence. */
u8 snd_mpu_byte_89e2 = 0x99;       /* the byte FUN_89e2 writes to 0x330 (engine: AH) */

/* ── L5 ISR sequencer PRNG/state bytes (CODE 0x979b..0x979e) ─────────────────────────
 *  The 96c4/95b5 noise sequencers keep their LFSR/PRNG state in the CS-segment bytes
 *  just PAST the 10-word L3 param frame: 0x979a is the per-tick fire counter (it ALIASES
 *  snd_param_frame[9]'s low byte — the schedulers seed it 0x0f as the frame's 0x0f marker,
 *  the ISR then `inc`/`test 0x10`s it each tick), and 0x979b..0x979e hold the two PRNG
 *  words the ISR self-modifies.  Modelled here as a 5-byte region BASED at 0x979a so the
 *  ISR's exact byte/word offset arithmetic (0x979a,0x979b,0x979c,0x979d) indexes it 1:1;
 *  the alias of [0] onto the frame's [9] low byte is the faithful image behaviour.  Owned
 *  here; not captured in the SND_SNAP (the ISR is never host-run — see the L5 block). */
u8 snd_isr_state_979a[5];          /* CODE 0x979a..0x979e — ISR tick counter + PRNG state */

/* MPU-401 poll residual / presence (DGROUP DAT_1000_85a1 / DAT_203b_557c).  FUN_89e2
   writes both on a DSR poll TIMEOUT.  Not read in the reconstruction (the L5 MPU init
   that gates on mpu401_present is carved out), modelled for write-side fidelity. */
s16 midi_track_count;
s16 mpu401_present;

/* ── FUN_89e2 (1000:89e2) — MPU-401 byte-out primitive ──────────────────────────────
 *  Poll status port 0x331 until DSR (bit 0x40) is CLEAR (CX from 0 -> up to 0x10000
 *  iters), then write the data byte (engine AH) to data port 0x330.  On a poll TIMEOUT
 *  (bit still set) it records the residual count into midi_track_count/mpu401_present and
 *  writes nothing.  In the capture 0x331 reads 0x00 (DSR clear) immediately, so it always
 *  writes the byte.  asm 1000:89e2: XOR CX,CX; IN AL,0x331; TEST 0x40; LOOPNZ; JNZ fail;
 *  DEC DX(->0x330); MOV AL,AH; OUT DX,AL.  The host byte arrives via snd_mpu_byte_89e2. */
void FUN_1000_89e2(void)
{
    u16 cx;
    u8  status;

    cx = 0;
    do {
        status = (u8)inp(0x331);
        cx = (u16)(cx - 1);              /* LOOPNZ: CX-- */
    } while (cx != 0 && (status & 0x40) != 0);
    if ((status & 0x40) != 0) {
        /* poll timed out (DSR still busy) — record the residual loop count into
         *  midi_track_count/mpu401_present (both = the post-LOOPNZ residual, 0 here), then
         *  write nothing.  Not exercised by the capture (0x331 reads DSR-clear). */
        midi_track_count = (s16)cx;
        mpu401_present = (s16)cx;
        return;
    }
    outp(0x330, snd_mpu_byte_89e2);      /* MOV AL,AH; OUT 0x330,AL */
}

/* ── FUN_1000_8a07 (1000:8a07) — MPU-401 raw 2-byte sample emit ──────────────────────
 *  Writes the MIDI command 0x99 then the two sample bytes (sample_lo, sample_hi) to the
 *  MPU data port, each via FUN_89e2 (poll-then-write).  asm: MOV AH,0x99; CALL 89e2;
 *  MOV AH,[BP+4]; CALL 89e2; MOV AH,[BP+6]; CALL 89e2.  So the OUT sequence is
 *  0x330=0x99, 0x330=sample_lo, 0x330=sample_hi (each gated by a 0x331 poll).  The AH
 *  byte for FUN_89e2 is staged through snd_mpu_byte_89e2 (the engine's AH register). */
void FUN_1000_8a07(u8 sample_lo, u8 sample_hi)
{
    snd_mpu_byte_89e2 = 0x99;            /* MOV AH,0x99 */
    FUN_1000_89e2();
    snd_mpu_byte_89e2 = sample_lo;       /* MOV AH,[BP+4] */
    FUN_1000_89e2();
    snd_mpu_byte_89e2 = sample_hi;       /* MOV AH,[BP+6] */
    FUN_1000_89e2();
}

/* ── FUN_1000_8ad0 (1000:8ad0) — MPU-401 settle delay ───────────────────────────────
 *  Nested loop: outer BL = 9 down to 1 (BH stays 0x90), inner CX = 0x7f down to 1.  Each
 *  inner iteration writes 3 MPU bytes via FUN_89e2: AH = 0x90+BL (=BH+BL), then AH = CL,
 *  then AH = 0.  asm 1000:8ad0: MOV BX,0x9009; (outer) MOV CX,0x7f; (inner) MOV AH,BH;
 *  ADD AH,BL; CALL 89e2; MOV AH,CL; CALL 89e2; XOR AH,AH; CALL 89e2; LOOP; DEC BL; JNZ.
 *  Fully deterministic (no external input) → port-write-gate validated. */
void FUN_1000_8ad0(void)
{
    u8  bl;
    u8  cl;
    u8  saved;

    saved = snd_mpu_byte_89e2;
    bl = 9;
    do {
        cl = 0x7f;
        do {
            snd_mpu_byte_89e2 = (u8)(0x90 + bl);   /* AH = BH(0x90) + BL */
            FUN_1000_89e2();
            snd_mpu_byte_89e2 = cl;                /* AH = CL */
            FUN_1000_89e2();
            snd_mpu_byte_89e2 = 0;                 /* XOR AH,AH */
            FUN_1000_89e2();
            cl = (u8)(cl - 1);                     /* LOOP */
        } while (cl != 0);
        bl = (u8)(bl - 1);                         /* DEC BL */
    } while (bl != 0);
    snd_mpu_byte_89e2 = saved;
}

/* ── opl_write_reg (1000:9007) — OPL2/AdLib FM register write ────────────────────────
 *  Entry AH = register index, AL = value.  Shadow the value into CODE 0x80cc + reg, then
 *  select the register on the addr port (OUT 0x388, reg), burn 6 status reads (the OPL
 *  addr-write settle), write the value on the data port (OUT 0x389, val), then burn 35
 *  status reads (the data-write settle).  asm 1000:9007: shadow MOV [0x80cc+AH],AL;
 *  XCHG AH,AL; OUT 0x388,AL; 6x IN 0x388; INC DX; OUT 0x389,AL; DEC DX; 35x IN 0x388.
 *  reg/val arrive in registers; on the host they are staged through opl_write_reg's args
 *  (the harness recovers them from the record's two OUT events). */
void opl_write_reg(u8 reg, u8 val)
{
    int i;

    opl_reg_shadow_80cc[reg] = val;     /* MOV [0x80cc+AH],AL (CS shadow) */
    outp(0x388, reg);                   /* OUT 0x388,AL (register select) */
    for (i = 0; i < 6; i++) {           /* addr-write settle: 6 status reads */
        (void)inp(0x388);
    }
    outp(0x389, val);                   /* OUT 0x389,AL (data) */
    for (i = 0; i < 35; i++) {          /* data-write settle: 35 status reads */
        (void)inp(0x388);
    }
}

/* ── opl_play_note (1000:905d) — program an OPL2 channel to play a note ──────────────
 *  Faithful transcription of the 4-register note program (1000:905d disassembly):
 *    (1) opl_write_reg(0x08, 0x00)                — CSW/NOTE-SEL clear.
 *    (2) reg 0x40+slot: F-number-derived level    — reads DGROUP fnum table 0x5593 +
 *        a CODE shadow byte at 0x80cc, applies the 0x20-(DH>>4) attenuation, masks 0x3f,
 *        ORs the top 2 bits of the shadow byte; AH (reg) = 0x43 + table[note].
 *    (3) reg 0xA0+chan: F-number low              — from DGROUP word table 0x559c indexed
 *        by 0x5614[chan], masked 0x3ff.
 *    (4) reg 0xB0+chan: key-on/block/F-number high — block from 0x55b4[chan]-1, F-hi bits.
 *  Args (from the asm stack/regs): param_1=[BP+4] key/feedback byte, param_2=[BP+6]
 *  attenuation, param_3=[BP+8] channel index, param_4=[BP+0xa] note index.  The freq
 *  tables (0x5593/0x559c/0x55b4/0x5614) are RUNTIME-populated and NOT in the SND_SNAP, so
 *  this fn's exact note OUTs are a documented port-write-gate exclusion (registered
 *  UNPORTED); it is ported 1:1 here for faithfulness + the BUMPY.EXE link. */
void opl_play_note(u8 param_1, u8 param_2, u16 param_3, u16 param_4)
{
    u8  fnum;          /* DL : 0x5593[note] + 0x43 (register index for step 2)            */
    u8  shadow;        /* AL : CODE 0x80cc[fnum + 0xf - ...]; top-2-bits carry into level */
    u8  dh;            /* DH : attenuation scratch                                        */
    u8  level;         /* the 0x40-register value                                         */
    u8  block_chan;    /* CL : 0x55b4[chan] - 1                                           */
    u16 fword;         /* AX : 0x559c[2*0x5614[chan]] & 0x3ff                             */
    u8  ah;

    opl_write_reg(0x08, 0x00);                            /* (1) */

    fnum = (u8)(opl_fnum_lo_5593[param_4 & 0xff] + 0x43); /* DL = [0x5593+note]+0x43 */
    /* CODE 0x80cc shadow byte at *(0x80cc + fnum).  The decomp forms the address as
       CONCAT11((0x33 < fnum) - 0x80, fnum + 0xcc): the low byte (fnum + 0xcc) and the
       carry (0x33 < fnum) reconstruct the absolute offset 0x80cc + fnum, so the array
       index is fnum — NOT the bare low byte (fnum - 0x34), which was the prior off-by. */
    shadow = opl_reg_shadow_80cc[fnum];
    dh = (u8)((u8)(0 - param_2) >> 4);                    /* SUB DH,[BP+6]; SHR x4 (DH starts 0) */
    dh &= 0x3f;
    level = (u8)(0x20 - dh);                              /* MOV AH,0x20; SUB AH,DH */
    level &= 0x3f;
    level |= (u8)(shadow & 0xc0);                         /* AND AL,0xc0; OR DH,AL */
    opl_write_reg(fnum, level);                           /* (2) reg=0x43+tbl, val=level */

    block_chan = (u8)(opl_chan_data_55b4[param_3 & 0xff] - 1);   /* CL = [0x55b4+chan]-1 */
    fword = (u16)(opl_fnum_hi_559c[(u8)(opl_chan_idx_5614[param_3 & 0xff] * 2)] |
                  (opl_fnum_hi_559c[(u8)(opl_chan_idx_5614[param_3 & 0xff] * 2) + 1] << 8));
    fword &= 0x3ff;
    opl_write_reg((u8)(0xa0 + (u8)param_4), (u8)(fword & 0xff));  /* (3) reg=0xA0+chan-ish */

    ah = (u8)(fword >> 8);
    ah &= 0x3;                                            /* AND AL,0x3 (F-hi bits)  */
    ah = (u8)(ah + (u8)((block_chan & 7) << 2));          /* + (CL&7)<<2 (block)     */
    ah = (u8)(ah + param_1);                              /* + [BP+4] (key-on bit)   */
    opl_write_reg((u8)(0xb0 + (u8)param_4), ah);          /* (4) reg=0xB0+chan, key-on */
}

/* ── FUN_1000_8e2f (1000:8e2f) — OPL2 all-notes-off ─────────────────────────────────
 *  Loop voice = 1..9 calling opl_play_note(0,0,0,voice).  asm 1000:8e2f: MOV BX,1; (loop)
 *  PUSH BX; XOR AX,AX; PUSH AX x3; CALL 905d; ...; INC BX; CMP BX,9; JLE.  Reaches
 *  opl_play_note (a documented port-write-gate exclusion — runtime freq tables), so 8e2f
 *  inherits the exclusion (registered UNPORTED); ported 1:1 for faithfulness + the link. */
void FUN_1000_8e2f(void)
{
    int voice_index;

    voice_index = 1;
    do {
        opl_play_note(0, 0, 0, (u16)voice_index);
        voice_index = voice_index + 1;
    } while (voice_index < 10);
}

/* ── pc_speaker_silence (1000:9115) — silence the PC speaker ─────────────────────────
 *  Zero the 15-byte voice table (CODE 0x83cc), clear the select scratch byte (CODE
 *  0x83ee), then clear the low 2 bits of port 0x61 (gate + speaker-enable): IN 0x61;
 *  AND 0xfc; OUT 0x61.  asm 1000:9115 verbatim.  Deterministic given the 0x61 IN replay
 *  (capture: IN 0x61=0xff -> OUT 0x61=0xfc) → port-write-gate validated. */
void pc_speaker_silence(void)
{
    int i;
    u8  port61;

    for (i = 0; i < SND_VOICE_TABLE_LEN; i++) {   /* MOV BX,0x83cc; CX,0xf; LOOP zero */
        snd_voice_table[i] = 0;
    }
    snd_select_scratch_83ee = 0;                  /* MOV CS:[0x83ee],0 */
    port61 = (u8)inp(0x61);                        /* IN AL,0x61 */
    port61 = (u8)(port61 & 0xfc);                  /* AND AL,0xfc */
    outp(0x61, port61);                            /* OUT 0x61,AL */
}

/* ── speaker_gate_reset (1000:9440) — PC-speaker gate reset ──────────────────────────
 *  If sound_mode (DGROUP 0x683e) == 0: IN 0x61; OR 0x3; OUT 0x61 (raise gate+enable).
 *  Else: falls into speaker_gate_strobe (IN 0x61; AND 0xfc; OUT 0x61).  asm 1000:9440:
 *  CMP [0x683e],0; JNZ 9451(strobe); IN 0x61; OR AL,0x3; CALL <thunk-noop>; OUT 0x61.
 *  The intervening CALL 0x9434 is a chain of JMP thunks that returns with AL unchanged.
 *  Deterministic (capture sound_mode==0: IN 0x61=0xff -> OUT 0x61=0xff). */
void speaker_gate_reset(void)
{
    u8 port61;

    if (sound_mode != 0) {
        speaker_gate_strobe();          /* JNZ 0x9451 (the AND-0xfc strobe path) */
        return;
    }
    port61 = (u8)inp(0x61);             /* IN AL,0x61 */
    port61 = (u8)(port61 | 0x3);        /* OR AL,0x3 */
    outp(0x61, port61);                 /* OUT 0x61,AL */
}

/* ── speaker_gate_strobe (1000:9451) — strobe/ack the PC-speaker gate ────────────────
 *  IN 0x61; AND 0xfc; OUT 0x61 (clear gate+enable).  asm 1000:9451 verbatim (the CALL
 *  0x9434 between AND and OUT is the noop thunk chain).  Deterministic (IN 0x61=0xff ->
 *  OUT 0x61=0xfc). */
void speaker_gate_strobe(void)
{
    u8 port61;

    port61 = (u8)inp(0x61);             /* IN AL,0x61 */
    port61 = (u8)(port61 & 0xfc);       /* AND AL,0xfc */
    outp(0x61, port61);                 /* OUT 0x61,AL */
}

/* ── record_status_and_strobe_speaker (1000:946e) — latch status, maybe strobe ──────
 *  If sound_mode (DGROUP 0x683e) == 0: record_min_status_code(0) then speaker_gate_strobe.
 *  Else: record_min_status_code(0xff) only.  asm 1000:946e: MOV AX,[0x683e]; CMP 0;
 *  MOV AX,0xff; JZ .latch_strobe; MOV AX,0; CALL 945b; JMP end; .latch_strobe: CALL 945b;
 *  CALL 9451(strobe).  record_min_status_code is a stub (records into CS:[0x946c]); the
 *  validated OUT is the strobe's OUT 0x61 (sound_mode==0 capture: OUT 0x61=0xfc). */
void record_status_and_strobe_speaker(void)
{
    if (sound_mode == 0) {
        record_min_status_code(0);      /* MOV AX,0; CALL 0x945b */
        speaker_gate_strobe();          /* CALL 0x9451 */
    } else {
        record_min_status_code(0xff);   /* MOV AX,0xff; CALL 0x945b */
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Phase-6 T6 — L5 ISR TONE-SEQUENCER (the PIT timer-callback tone engine).
 *
 *  These are the engine's IRQ0 / int-8 (PIT, ~18.2 Hz reprogrammed faster) machinery:
 *  a multiplexer ISR (1000:7c02) that walks the 6-channel timer-callback table 0x5516
 *  each tick decrementing a per-channel counter and, when it wraps past 500, FAR-CALLS
 *  the channel's installed tone-sequencer callback; and the three installed callbacks
 *  (1000:9631 / 96c4 / 95b5) the L3 schedule_timer_callback_a / _b / _c fns install (via
 *  set_timer_slot_raw + the snd_timer_cb_off/seg far ptr).  Each callback advances the
 *  10-word tone param frame snd_param_frame (0x9788..0x979a) it was handed and reprograms
 *  PIT channel 2 (ports 0x42/0x43) / strobes the PC-speaker gate (port 0x61) to sweep the
 *  tone's frequency over its lifetime, retiring the channel (disable + speaker strobe) when
 *  the duration counter (frame[0]) expires.
 *
 *  ── RECONSTRUCTION FIDELITY: reconstructed 1:1 as DOCUMENTATION, NOT runtime-gated ──
 *  Reached ONLY through the installed far pointer (the 0x5516 table the L3 layer fills),
 *  these routines have NO Ghidra function boundary and are NOT hooked by the sound oracle
 *  (tools/sound_oracle.py FN_NAMES) — so the Phase-6 T1 capture contains ZERO records for
 *  0x7c02 / 0x9631 / 0x96c4 / 0x95b5, and there is no host differential to gate them.  Each
 *  body below is transcribed VERBATIM from the raw disassembly (cited per fn) and is
 *  structurally faithful to the decomp + to the L3 param frame it consumes; but the engine's
 *  async per-PIT-tick frequency sweep — driven by hardware interrupts mutating the frame
 *  out from under the foreground game loop — is NOT host-replayable as a deterministic
 *  differential.  They are therefore BEHAVIOR-FAITHFUL reconstructions kept for
 *  documentation, the same precedent as the self-modifying-graphics-overlay blitters
 *  (sprite_blit / bg_render): faithful to what the binary does, validated by inspection
 *  against the asm rather than by a runtime gate.  See docs/reconstruction-fidelity.md.
 *
 *  The CS-relative entry-guard `call 0x9439; jne exit` reads sound_mode (DGROUP 0x683e) and
 *  skips the body when sound_mode != 0; 0x9434 is the noop JMP-thunk chain (AL preserved);
 *  0x945b = record_min_status_code (no-op stub); 0x7df9 = set_timer_slot_raw (PORTED); 0x7e1f
 *  = the channel-slot clear (modelled by isr_disable_timer_slot below); 0x9451 =
 *  speaker_gate_strobe (PORTED).  Stack-probe / register-save prologue omitted (convention).
 * ════════════════════════════════════════════════════════════════════════════ */

/* isr_disable_timer_slot (1000:7e1f) — clear channel's 0x549c slot (value=0, cb=0:0).
 *  Validates channel 0..3, then set_timer_slot_raw(channel, 0, 0, 0) (the asm adds 2 to
 *  BX and tail-calls the 0x7e62 writer with AX=CX=DX=0).  The tone-sequencer retire path
 *  calls it with channel 2. */
static int isr_disable_timer_slot(int channel)
{
    if ((-1 < channel) && (channel < 4)) {
        set_timer_slot_raw(channel, 0, 0, 0);
        return 1;
    }
    return 0;
}

/* ── tone_seq_callback_9631 (1000:9631) — sweep tone sequencer (schedule_a's installed cb) ─
 *  Per-tick: gated on sound_mode==0 (entry guard 0x9439).  Decrement the sub-sweep counter
 *  frame[8] (0x9798); on wrap reload it from frame[5] (0x9792), advance the channel-2 reload
 *  value frame[4] (0x9790 += frame[6] @0x9794) and re-arm the slot (set_timer_slot_raw(2,
 *  frame[4], cb_off, cb_seg)).  Decrement the step counter frame[7] (0x9796); when it too
 *  wraps, decrement the lifetime counter frame[0] (0x9788): if it hits 0 RETIRE (record 0xff,
 *  disable slot 2, strobe gate); else reload frame[7] from frame[2] (0x978c), advance the
 *  pitch frame[1] (0x978a += frame[3] @0x978e) and reprogram PIT ch2 (0x43<-0xb6, 0x42<-lo,
 *  0x42<-hi).  asm 1000:9631 verbatim. */
void tone_seq_callback_9631(void)
{
    u8 ax_hi_lo;

    if (sound_mode != 0) {              /* call 0x9439; jne 0x96ba (exit) */
        return;
    }
    snd_param_frame[8] -= 1;            /* dec word cs:[0x9798] */
    if (snd_param_frame[8] == 0) {      /* jne 0x9672 */
        snd_param_frame[8] = snd_param_frame[5];                 /* [0x9798] = [0x9792] */
        snd_param_frame[4] += snd_param_frame[6];                /* [0x9790] += [0x9794] */
        set_timer_slot_raw(2, (int)snd_param_frame[4],           /* AX=[0x9790], BX=2     */
                           snd_timer_cb_off, snd_timer_cb_seg);  /* CX=[0x97a1], DX=[0x979f] */
    }
    snd_param_frame[7] -= 1;            /* dec word cs:[0x9796] */
    if (snd_param_frame[7] != 0) {      /* jne 0x96ba (exit) */
        return;
    }
    snd_param_frame[0] -= 1;            /* dec word cs:[0x9788] */
    if (snd_param_frame[0] == 0) {      /* jne 0x9691 */
        record_min_status_code(0xff);          /* MOV AX,0xff; CALL 0x945b */
        isr_disable_timer_slot(2);             /* MOV AX,2;    CALL 0x7e1f */
        speaker_gate_strobe();                 /* CALL 0x9451 */
        return;                                /* jmp 0x96ba (exit) */
    }
    snd_param_frame[7] = snd_param_frame[2];   /* [0x9796] = [0x978c] */
    snd_param_frame[1] += snd_param_frame[3];  /* [0x978a] += [0x978e] */
    outp(0x43, 0xb6);                          /* MOV AL,0xb6; OUT 0x43,AL */
    (void)0;                                   /* CALL 0x9434 (noop thunk chain) */
    outp(0x42, (u8)snd_param_frame[1]);        /* MOV AX,[0x978a]; OUT 0x42,AL (lo) */
    (void)0;                                   /* CALL 0x9434 */
    ax_hi_lo = (u8)(snd_param_frame[1] >> 8);  /* XCHG AH,AL */
    outp(0x42, ax_hi_lo);                      /* OUT 0x42,AL (hi) */
}

/* ── tone_seq_callback_96c4 (1000:96c4) — noise/PRNG tone sequencer (schedule_b's cb) ──────
 *  Per-tick (gated on sound_mode==0): bump the per-tick fire counter at 0x979a (= the ISR
 *  state byte, seeded 0x0f by the scheduler's frame[9] marker); every time the low nibble
 *  carries past 0x10 run one PRNG step over the two state words at 0x979b/0x979d
 *  (xor/add/xor mixing with the 0x2345 / 0x4567 constants) and reprogram PIT ch2 in MODE 2
 *  (0x43<-0xb4) with the pitch frame[1].  Then run the same sub-sweep/step/lifetime ladder
 *  as 9631 (frame[8]/frame[0] counters, the re-arm + retire), and finally roll the PRNG
 *  word (ROL 0x979b) and toggle the speaker gate from port 0x61 (mode-2-ish bit dance).
 *  asm 1000:96c4 verbatim. */
void tone_seq_callback_96c4(void)
{
    u16 ax;
    u8  port61;

    if (sound_mode != 0) {                     /* call 0x9439; jne 0x977e (exit) */
        return;
    }
    snd_isr_state_979a[0] += 1;                /* inc byte cs:[0x979a] */
    if ((snd_isr_state_979a[0] & 0x10) != 0) { /* test byte cs:[0x979a],0x10; je 0x971e */
        ax = 0;
        ax ^= *(u16 *)(snd_isr_state_979a + 1);   /* xor ax, cs:[0x979b] */
        ax += 0x2345;                             /* add ax, 0x2345 */
        ax ^= *(u16 *)(snd_isr_state_979a + 2);   /* xor ax, cs:[0x979c] */
        ax ^= *(u16 *)(snd_isr_state_979a + 3);   /* xor ax, cs:[0x979d] */
        *(u16 *)(snd_isr_state_979a + 1) = ax;    /* mov cs:[0x979b], ax */
        ax += 0x4567;                             /* add ax, 0x4567 */
        ax ^= *(u16 *)(snd_isr_state_979a + 2);   /* xor ax, cs:[0x979c] */
        *(u16 *)(snd_isr_state_979a + 3) = ax;    /* mov cs:[0x979d], ax */
        snd_isr_state_979a[0] = 0;                /* xor ax,ax; mov cs:[0x979a],al */
        outp(0x43, 0xb4);                         /* MOV AL,0xb4; OUT 0x43,AL (mode 2) */
        (void)0;                                  /* CALL 0x9434 */
        outp(0x42, (u8)snd_param_frame[1]);       /* MOV AX,[0x978a]; OUT 0x42,AL (lo) */
        (void)0;                                  /* CALL 0x9434 */
        outp(0x42, (u8)(snd_param_frame[1] >> 8));/* XCHG AH,AL; OUT 0x42,AL (hi) */
    }
    snd_param_frame[8] -= 1;                    /* dec word cs:[0x9798] */
    if (snd_param_frame[8] == 0) {              /* jne 0x9766 */
        snd_param_frame[0] -= 1;               /* dec word cs:[0x9788] */
        if (snd_param_frame[0] == 0) {         /* jne 0x973d */
            record_min_status_code(0xff);          /* MOV AX,0xff; CALL 0x945b */
            isr_disable_timer_slot(2);             /* MOV AX,2;    CALL 0x7e1f */
            speaker_gate_strobe();                 /* CALL 0x9451 */
            return;                                /* jmp 0x977e (exit) */
        }
        snd_param_frame[8] = snd_param_frame[5];                /* [0x9798] = [0x9792] */
        snd_param_frame[4] += snd_param_frame[6];               /* [0x9790] += [0x9794] */
        set_timer_slot_raw(2, (int)snd_param_frame[4],          /* AX=[0x9790], BX=2     */
                           snd_timer_cb_off, snd_timer_cb_seg); /* CX=[0x97a1], DX=[0x979f] */
    }
    *(u16 *)(snd_isr_state_979a + 1) =                          /* mov ax,[0x979b]; rol ax,1 */
        (u16)((*(u16 *)(snd_isr_state_979a + 1) << 1) |
              (*(u16 *)(snd_isr_state_979a + 1) >> 15));        /* mov [0x979b],ax */
    port61 = (u8)inp(0x61);                     /* IN AL,0x61 */
    /* AH = (AH & 2) | 1 then AL = (AL & 0xfd) | AH.  AH starts as the rol'd word's high byte;
     *  the engine reuses AX from the ROL — AH = (ax>>8).  The 0x61 bit dance toggles bit1
     *  (speaker data) / sets bit0 (gate) from the PRNG. */
    {
        u8 ah = (u8)(*(u16 *)(snd_isr_state_979a + 1) >> 8);   /* AH = rolled hi byte */
        ah = (u8)((ah & 2) | 1);               /* AND AH,2; OR AH,1 */
        port61 = (u8)((port61 & 0xfd) | ah);   /* AND AL,0xfd; OR AL,AH */
    }
    outp(0x61, port61);                        /* OUT 0x61,AL */
}

/* ── tone_seq_callback_95b5 (1000:95b5) — noise/PRNG tone sequencer (schedule_c's cb) ──────
 *  The variant 96c4 installed by schedule_timer_callback_c.  Same PRNG-stepped noise engine
 *  as 96c4 but a shorter ladder: the PRNG step additionally masks AX with 0xdb6d (the LFSR
 *  tap mask) and there is NO sub-sweep/lifetime counter section — after the optional PRNG
 *  step it just rolls 0x979b and does the port-0x61 speaker bit dance.  asm 1000:95b5 verbatim. */
void tone_seq_callback_95b5(void)
{
    u16 ax;
    u8  port61;

    if (sound_mode != 0) {                     /* call 0x9439; jne 0x9627 (exit) */
        return;
    }
    snd_isr_state_979a[0] += 1;                /* inc byte cs:[0x979a] */
    if ((snd_isr_state_979a[0] & 0x10) != 0) { /* test byte cs:[0x979a],0x10; je 0x960f */
        ax = 0;
        ax ^= *(u16 *)(snd_isr_state_979a + 1);   /* xor ax, cs:[0x979b] */
        ax += 0x2345;                             /* add ax, 0x2345 */
        ax ^= *(u16 *)(snd_isr_state_979a + 2);   /* xor ax, cs:[0x979c] */
        ax ^= *(u16 *)(snd_isr_state_979a + 3);   /* xor ax, cs:[0x979d] */
        ax &= 0xdb6d;                             /* and ax, 0xdb6d (LFSR tap mask) */
        *(u16 *)(snd_isr_state_979a + 1) = ax;    /* mov cs:[0x979b], ax */
        ax += 0x4567;                             /* add ax, 0x4567 */
        ax ^= *(u16 *)(snd_isr_state_979a + 2);   /* xor ax, cs:[0x979c] */
        *(u16 *)(snd_isr_state_979a + 3) = ax;    /* mov cs:[0x979d], ax */
        snd_isr_state_979a[0] = 0;                /* xor ax,ax; mov cs:[0x979a],al */
        outp(0x43, 0xb4);                         /* MOV AL,0xb4; OUT 0x43,AL (mode 2) */
        (void)0;                                  /* CALL 0x9434 */
        outp(0x42, (u8)snd_param_frame[1]);       /* MOV AX,[0x978a]; OUT 0x42,AL (lo) */
        (void)0;                                  /* CALL 0x9434 */
        outp(0x42, (u8)(snd_param_frame[1] >> 8));/* XCHG AH,AL; OUT 0x42,AL (hi) */
    }
    *(u16 *)(snd_isr_state_979a + 1) =                          /* mov ax,[0x979b]; rol ax,1 */
        (u16)((*(u16 *)(snd_isr_state_979a + 1) << 1) |
              (*(u16 *)(snd_isr_state_979a + 1) >> 15));        /* mov [0x979b],ax */
    port61 = (u8)inp(0x61);                     /* IN AL,0x61 */
    {
        u8 ah = (u8)(*(u16 *)(snd_isr_state_979a + 1) >> 8);   /* AH = rolled hi byte */
        ah = (u8)((ah & 2) | 1);               /* AND AH,2; OR AH,1 */
        port61 = (u8)((port61 & 0xfd) | ah);   /* AND AL,0xfd; OR AL,AH */
    }
    outp(0x61, port61);                        /* OUT 0x61,AL */
}

/* ── pit_timer_isr_multiplexer (1000:7c02) — the IRQ0 / int-8 PIT tick multiplexer ─────────
 *  The installed hardware ISR (set as the int-8 vector at the timer-init routine 1000:7c34
 *  via DOS int 21h AH=25 AL=8).  Per tick it sets DS=DGROUP (0x103b), loads the far pointer
 *  to the 6-channel timer-callback table (DGROUP [0x54cc] -> base 0x5516), and walks 6 slots
 *  of 8 bytes {current@0, reload@2, cb_off@4, cb_seg@6}: current += reload; if it reaches the
 *  500-tick (0x1f4) period it subtracts 500 and FAR-CALLS the slot's installed callback
 *  (cb_seg:cb_off) — that is where tone_seq_callback_9631 / 96c4 / 95b5 run.  After the sweep
 *  it sends the 8259 EOI (OUT 0x20,0x20) and chains/returns to the prior int-8 handler.
 *
 *  The original is a hand-written naked ISR (CS-scratch register save/restore at 0x7cce.., a
 *  manufactured far-return frame at 0x7c87, the int-21h vector chain).  Reconstructed here at
 *  the LOGICAL level — the per-channel period accumulate + far-callback dispatch + EOI — over
 *  the same 0x5516 slot layout (snd_timer_cb_table); the register-save/EOI/vector-chain
 *  plumbing is the ambient ISR scaffolding, noted not transcribed.  Dispatches the channel-2
 *  far callback by its installed offset (the value the L3 schedulers wrote: 0x9631 / 0x96c4 /
 *  0x95b5) so the engine's actual per-tick tone advance is reproduced.  NOT runtime-gated
 *  (see the L5 FIDELITY block) — documentation of the async sweep driver. */
void pit_timer_isr_multiplexer(void)
{
    int channel;

    for (channel = 0; channel < 6; channel++) {            /* MOV CX,6; (loop) */
        u16 idx = (u16)(channel * 8);                      /* 0x5516 + channel*8 */
        u16 cur = *(u16 *)(snd_timer_cb_table + idx + 0);  /* [bx]     = current  */
        u16 reload = *(u16 *)(snd_timer_cb_table + idx + 2);/* [bx+2]   = reload   */
        u16 acc = (u16)(cur + reload);                     /* AX = [bx] + [bx+2]  */
        if (acc >= 0x1f4) {                                /* cmp ax,0x1f4; jge fire */
            u16 cb_off = *(u16 *)(snd_timer_cb_table + idx + 4);   /* push [bx+4] */
            u16 cb_seg = *(u16 *)(snd_timer_cb_table + idx + 6);   /* push [bx+6] */
            acc = (u16)(acc - 0x1f4);                       /* SUB AX,0x1f4 */
            /* FAR-CALL cb_seg:cb_off (the manufactured far frame at 0x7c87 returns here).
             *  On the host we dispatch by the installed CODE-seg offset to the reconstructed
             *  callback (cb_seg is the load-base CODE segment). */
            (void)cb_seg;
            if (cb_off == 0x9631) {
                tone_seq_callback_9631();
            } else if (cb_off == 0x96c4) {
                tone_seq_callback_96c4();
            } else if (cb_off == 0x95b5) {
                tone_seq_callback_95b5();
            }
        }
        *(u16 *)(snd_timer_cb_table + idx + 2) = acc;      /* MOV [bx+2],AX (store back) */
    }
    outp(0x20, 0x20);                                      /* OUT 0x20,AL — 8259 EOI */
}
