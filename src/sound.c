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
 *    record_min_status_code (1000:945b) — records a min status code from the packed
 *        CPU-flags word; not part of the validated frame.  → T4/T5.
 *    speaker_gate_reset     (1000:9440) — PC-speaker gate reset (L4 port write).  → T5.
 *    FUN_1000_8a07          (1000:8a07) — the OPL/MPU raw-sample emit (L4).       → T5.
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
 *  call sites.  record_min_status_code itself is a stub/no-op (it records a min status
 *  code — not part of the validated frame).  Recorded in docs/reconstruction-fidelity.md.
 * ════════════════════════════════════════════════════════════════════════════ */

/* extern stubs (still-stubbed callees — see header block above). */
extern void record_min_status_code(u16 status);   /* 1000:945b — no-op stub */
extern void speaker_gate_reset(void);              /* 1000:9440 — L4 gate-reset stub */
extern void FUN_1000_8a07(u8 sample_lo, u8 sample_hi); /* 1000:8a07 — sample stub */

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
 *  callback ptr 1000:9631.  param_1 (=2 from every caller) is the status-code argument;
 *  param_2..param_8 are the frame words.  Frame-offset → snd_param_frame[] index map:
 *    0x9788→[0] 0x978a→[1] 0x978c→[2] 0x978e→[3] 0x9790→[4]
 *    0x9792→[5] 0x9794→[6] 0x9796→[7] 0x9798→[8] 0x979a→[9] (byte 0x0f, low half).
 *  Deeper callees FUN_1000_7df9 (scheduler) + speaker_gate_reset stay STUBBED → T4/T5. */
u16 schedule_timer_callback_a(u16 param_1, u16 param_2, u16 param_3, u16 param_4,
                              u16 param_5, u16 param_6, u16 param_7, u16 param_8)
{
    u16 ret_status;
    u8  in_CF = snd_sched_carry_in;   /* modelled entry carry (see FIDELITY note) */

    record_min_status_code(param_1);  /* status word; not part of the validated frame */
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

    record_min_status_code(param_1);
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

    record_min_status_code(param_1);
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
 *  The L4 init/IO callees (mpu401_reset_to_uart, pc_speaker_silence, FUN_8ad0/8e2f/89e2,
 *  the dispatch_b/c/d backends, FUN_8b2a) stay STUBBED with notes.
 *
 *  L3 TIMER-TABLE MGMT.  Two engine tables:
 *    - 0x5516 callback table (arm_timer_callback / disable_timer_callback): per-channel
 *      8-byte slot {current@0, reload@2, cb_off@4, cb_seg@6}, base 0x5516 + channel*8.
 *    - 0x549c slot table (set_timer_slot -> set_timer_slot_raw / get_timer_slot_field):
 *      per-channel 8-byte slot {value@0, 0@2, cb_off@4, cb_seg@6}, base 0x549c +
 *      (channel+2)*8.  set_timer_slot_raw is also the tail the L3 schedulers reach.
 * ════════════════════════════════════════════════════════════════════════════ */

/* still-stubbed callees the T4 bodies reach (see header block). */
extern void mpu401_reset_to_uart(void);   /* 1000:88...  L4 MPU reset  → T5 */
extern void FUN_1000_8b2a(void);          /* 1000:8b2a   snddrv_init substep → T5 */
extern void pc_speaker_silence(void);     /* 1000:9115   L4 → T6 */
extern void FUN_1000_8ad0(void);          /* 1000:8ad0   L4 MPU settle → T6 */
extern void FUN_1000_8e2f(void);          /* 1000:8e2f   L4 OPL all-notes-off → T6 */
extern void FUN_1000_89e2(void);          /* 1000:89e2   L4 timing primitive → T6 */
extern void FUN_1000_91cf(void);          /* 1000:91cf   dispatch_b mode-0 backend → T6 */
extern void FUN_1000_8af6(void);          /* 1000:8af6   dispatch_b mode-4 backend → T6 */
extern void FUN_1000_8e48(void);          /* 1000:8e48   dispatch_b mode-1 backend → T6 */
extern void FUN_1000_91d7(void);          /* 1000:91d7   dispatch_c mode-0 backend → T6 */
extern void FUN_1000_8b04(void);          /* 1000:8b04   dispatch_c mode-4 backend → T6 */
extern void FUN_1000_8e50(void);          /* 1000:8e50   dispatch_c mode-1 backend → T6 */
extern void FUN_1000_91df(void);          /* 1000:91df   dispatch_d mode-0 backend → T6 */
extern void FUN_1000_8b0d(void);          /* 1000:8b0d   dispatch_d mode-4 backend → T6 */
extern void FUN_1000_8e58(void);          /* 1000:8e58   dispatch_d mode-1 backend → T6 */
extern void FUN_1000_7fef(void);          /* 1000:7fef   timer teardown/restore → T5/T6 */
extern void FUN_1000_6183(void);          /* 1000:6183   out-of-scope entity sweep (→ entity) */

/* ════════════════════════════════════════════════════════════════════════════
 *  L1 event-wrapper LUTs — exact image bytes (BUMPY_unpacked.exe DGROUP offsets).
 *  Each table is 0x30 bytes (the established sound-LUT size, cf. player.c
 *  move_sound_lut_*).  Indexed by the wrapper's event/state global. */
u8 action_sound_lut_opl_260e[0x30] = {     /* DGROUP 0x260e — play_action_sound OPL */
    0x00,0x13,0x14,0x15,0x16,0x17,0x17,0x18,0x19,0x1a,0x1b,0x00,0x1c,0x1d,0x1e,0x1f,
    0x20,0x21,0x11,0x00,0x00,0x00,0x00,0x22,0x22,0x00,0x23,0x23,0x23,0x23,0x24,0x00,
    0x00,0x25,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
u8 action_sound_lut_std_263e[0x30] = {     /* DGROUP 0x263e — play_action_sound std */
    0x00,0x01,0x01,0x0e,0x0b,0x03,0x03,0x05,0x06,0x07,0x01,0x00,0x04,0x01,0x01,0x01,
    0x01,0x08,0x12,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x0b,0x0b,0x0b,0x0b,0x0b,0x00,
    0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
u8 state_sound_lut_opl_26ce[0x30] = {      /* DGROUP 0x26ce — play_state_sound OPL */
    0x00,0x03,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x00,0x03,
    0x00,0x03,0x11,0x03,0x03,0x03,0x00,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x11,0x03,
    0x03,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
u8 state_sound_lut_std_26fe[0x30] = {      /* DGROUP 0x26fe — play_state_sound std */
    0x00,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x09,0x09,0x09,0x09,0x0b,0x0b,0x0b,0x00,0x0b,
    0x00,0x09,0x12,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x00,0x0b,0x0b,0x0b,0x0b,0x12,0x0b,
    0x0b,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
u8 contact_sound_lut_opl_276e[0x30] = {    /* DGROUP 0x276e — play_contact_sound OPL */
    0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x0e,0x10,0x10,0x10,0x12,0x0e,0x0a,0x0a,
    0x0a,0x0a,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,
    0x00,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0e,0x10,0x10,0x11,0x12,0x0e,0x0a,0x0a,
};
u8 contact_sound_lut_std_278e[0x30] = {    /* DGROUP 0x278e — play_contact_sound std */
    0x00,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0e,0x10,0x10,0x11,0x12,0x0e,0x0a,0x0a,
    0x0a,0x0a,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0b,
    0x00,0x00,0x23,0x7f,0x41,0x7f,0x28,0x7f,0x26,0x7f,0x40,0x7f,0x3f,0x7f,0x3e,0x7f,
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
 *  reset + FUN_8b2a substep, set snddrv_mode=1 + sound_active_device_mask=1, return a
 *  status bitmask (0=ok; |4 / |1 on a sub-step failure).  The substep_ok flag is always
 *  true here (the L4 reset/8b2a are STUBBED → T5), so the failure ORs never fire — the
 *  decomp's dead `if(!substep_ok)` arms are preserved 1:1. */
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
        FUN_1000_8b2a();
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
 *  Thunk to FUN_1000_7fef (restore the PIT int vector via DOS int 21h + reprogram the
 *  PIT).  FUN_1000_7fef is an L4/L5 hardware/DOS routine, STUBBED → T5/T6. */
void timer_restore(void)
{
    FUN_1000_7fef();
}
