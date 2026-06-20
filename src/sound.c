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

/* ════════════════════════════════════════════════════════════════════════════
 *  Phase-6 T3 — effect→frame pipeline (L1 core dispatch + L3 tone-submit).
 *
 *  STILL-STUBBED callees these bodies reach (faithful-signature stubs in
 *  game_stubs.c for the BUMPY.EXE link; host stubs in tools/sound_ctest.c for the
 *  replay harness).  None contributes to the validated semantic state (the device-
 *  guarded dispatch + the 10-word tone param frame + the installed far cb ptr):
 *    record_min_status_code (1000:945b) — records a min status code from the packed
 *        CPU-flags word; not part of the validated frame.  → T4/T5.
 *    FUN_1000_7df9          (1000:7df9) — the PIT/timer scheduler.               → T4.
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
extern void FUN_1000_7df9(void);                   /* 1000:7df9 — scheduler stub */
extern void speaker_gate_reset(void);              /* 1000:9440 — L4 gate-reset stub */
extern void FUN_1000_8a07(u8 sample_lo, u8 sample_hi); /* 1000:8a07 — sample stub */

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
        FUN_1000_7df9();
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
        FUN_1000_7df9();
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
        FUN_1000_7df9();
        speaker_gate_reset();
        ret_status = 0;
    }
    return ret_status;
}
