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

/* snd_tone_param_frame_t — named-field view of snd_param_frame, used identically
   by the 3 schedulers (writers) and the tone_seq_callback_9631/96c4 ISRs
   (readers) below.  The array itself stays a plain u16[SND_PARAM_FRAME_WORDS]
   (tools/sound_ctest.c compares it generically by index against a captured
   trace), so consumers overlay this struct locally via SND_PF rather than
   changing the global's type.
   NOTE: field [9] (marker) occupies the same CODE address (0x979a) as the
   separate snd_isr_state_979a byte array's [0] byte — a real overlap in the
   original engine's memory that this reconstruction models as two distinct
   storage locations (see tone_seq_callback_96c4's comment); not modeled here. */
typedef struct {
    u16 lifetime_count;        /* [0] 0x9788 — retire when this hits 0 */
    u16 pitch_reload;          /* [1] 0x978a — PIT ch2 pitch value */
    u16 step_reload;           /* [2] 0x978c — reload source for step_current */
    u16 pitch_increment;       /* [3] 0x978e — added to pitch_reload each lifetime tick */
    u16 ch2_reload_current;    /* [4] 0x9790 — current PIT ch2 reload (re-armed via set_timer_slot_raw) */
    u16 subsweep_reload;       /* [5] 0x9792 — reload source for subsweep_current */
    u16 ch2_reload_increment;  /* [6] 0x9794 — added to ch2_reload_current each subsweep wrap */
    u16 step_current;          /* [7] 0x9796 — decremented each tick; wrap -> lifetime tick */
    u16 subsweep_current;      /* [8] 0x9798 — decremented each tick; wrap -> ch2 re-arm */
    u16 marker;                /* [9] 0x979a — always seeded 0x0f; see the overlap note above */
} snd_tone_param_frame_t;

/* Overlay macro rather than a function: callers need an lvalue-capable view
   (`SND_PF->field = x;`) of the plain array, matching the sprite_obj_t /
   blit_desc_t overlay pattern used elsewhere in this codebase. */
#define SND_PF ((snd_tone_param_frame_t *)snd_param_frame)

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
 *    snd_emit_raw_sample    (1000:8a07) — the OPL/MPU raw-sample emit (L4).       → T5.
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
 *  snd_emit_raw_sample (1000:8a07) are NO LONGER stubs — they are L4 drivers PORTED 1:1
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
 *  snd_mpu_byte_89e2 / mpu401_write_data_polled convention) PLUS reconciling all 6 already-
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

/* The MT-32/MPU raw-sample param table the device-4 (MT-32, F8) effect path indexes
 *  (DGROUP 0x27ae, file 0x13bee; 2 bytes per effect = a MIDI note-on: {note, velocity},
 *  emitted on percussion channel 10 via snd_emit_raw_sample -> 0x99,note,vel).  The real
 *  table holds 0x30 entries (effect ids 0x00-0x2f): entry 0 is the (0,0) silent slot,
 *  the rest are {note, 0x7f}.  0x2f is the highest id any caller uses (land_sound_tbl_*
 *  top out at 0x2f; direct calls at 0x2a/0x28/0x26).  Copied VERBATIM from the binary.
 *  Kept at [0x100] entries (effect_id is u8, indexes directly) so any id indexes a
 *  defined slot; entries past 0x30 are the adjacent DGROUP object in the original, never
 *  indexed by a real id, left zero here.
 *  RECONSTRUCTION FIDELITY: was a zeroed placeholder (the device-4 SFX path was not
 *  exercised by the validated semantic-state records) — that silenced EVERY MT-32 sound
 *  effect, because the emit then sent note-on velocity 0 = a silent note-off.  Now
 *  populated 1:1 from DGROUP 0x27ae so MT-32 SFX (e.g. the platform-bounce snare) sound. */
typedef struct { u8 note; u8 vel; } snd_opl_sample_t;

static const snd_opl_sample_t snd_opl_sample_table[0x100] = {   /* DGROUP 0x27ae (file 0x13bee) */
    {0x00,0x00},{0x23,0x7f},{0x41,0x7f},{0x28,0x7f},{0x26,0x7f},{0x40,0x7f},{0x3f,0x7f},{0x3e,0x7f},
    {0x27,0x7f},{0x54,0x7f},{0x43,0x7f},{0x47,0x7f},{0x41,0x7f},{0x4d,0x7f},{0x3d,0x7f},{0x29,0x7f},
    {0x2a,0x7f},{0x5b,0x7f},{0x2e,0x7f},{0x25,0x7f},{0x38,0x7f},{0x4f,0x7f},{0x28,0x7f},{0x26,0x7f},
    {0x40,0x7f},{0x3f,0x7f},{0x3e,0x7f},{0x46,0x7f},{0x60,0x7f},{0x44,0x7f},{0x47,0x7f},{0x31,0x7f},
    {0x38,0x7f},{0x42,0x7f},{0x3d,0x7f},{0x29,0x7f},{0x2c,0x7f},{0x2e,0x7f},{0x4e,0x7f},{0x49,0x7f},
    {0x53,0x7f},{0x55,0x7f},{0x58,0x7f},{0x62,0x7f},{0x5a,0x7f},{0x63,0x7f},{0x63,0x7f},{0x45,0x7f},
    /* remaining entries (effect ids 0x30..0xff): zero-initialized (never indexed by a real id) */
};

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
 *  emit a raw two-byte sample from table 0x27ae via snd_emit_raw_sample (PORTED T5).
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
        snd_emit_raw_sample(snd_opl_sample_table[effect_id].note,
                            snd_opl_sample_table[effect_id].vel);
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
 *  (in_PF<<2)|in_CF.  record_min_status_code is PORTED (records into CS:[0x946c] via
 *  last_status_code, not part of any validated frame/port sequence), so we hand it the
 *  available value (param_1) in lieu of reconstructing the host-absent FLAGS register —
 *  observationally identical.
 *  Deeper callee set_timer_slot_raw (1000:7df9, PORTED T4); speaker_gate_reset PORTED T5. */
u16 schedule_timer_callback_a(u16 param_1, u16 param_2, u16 param_3, u16 param_4,
                              u16 param_5, u16 param_6, u16 param_7, u16 param_8)
{
    u16 ret_status;
    u8  in_CF = snd_sched_carry_in;   /* modelled entry carry (see FIDELITY note) */

    record_min_status_code(param_1);  /* ORIGINAL: packed entry-FLAGS word (in_NT..in_CF
                                        *  bit-OR); param_1 stands in for it (callee is PORTED) */
    ret_status = 0xffff;
    if (!in_CF) {
        SND_PF->lifetime_count       = param_2;   /* DAT_1000_9788 */
        SND_PF->pitch_reload         = param_3;   /* DAT_1000_978a */
        SND_PF->step_current         = param_4;   /* DAT_1000_9796 */
        SND_PF->step_reload          = param_4;   /* DAT_1000_978c */
        SND_PF->pitch_increment      = param_5;   /* DAT_1000_978e */
        SND_PF->ch2_reload_current   = param_6;   /* DAT_1000_9790 */
        SND_PF->subsweep_reload      = param_7;   /* DAT_1000_9792 */
        SND_PF->subsweep_current     = param_7;   /* DAT_1000_9798 */
        SND_PF->ch2_reload_increment = param_8;   /* DAT_1000_9794 */
        SND_PF->marker               = 0xf;       /* DAT_1000_979a (byte 0x0f) */
        snd_timer_cb_off = 0x9631;      /* timer_callback_off (CODE 0x97a1) */
        snd_timer_cb_seg = 0x1000;      /* timer_callback_seg (CODE 0x979f) */
        /* The decomp renders this tail as a void set_timer_slot_raw() call; the disassembly
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

    record_min_status_code(param_1);    /* ORIGINAL: packed entry-FLAGS word (record_min_status_code is PORTED) */
    ret_status = 0xffff;
    if (!in_CF) {
        SND_PF->lifetime_count       = param_2;   /* DAT_1000_9788 */
        SND_PF->pitch_reload         = param_3;   /* DAT_1000_978a */
        SND_PF->ch2_reload_current   = param_4;   /* DAT_1000_9790 */
        SND_PF->subsweep_reload      = param_5;   /* DAT_1000_9792 */
        SND_PF->subsweep_current     = param_5;   /* DAT_1000_9798 */
        SND_PF->ch2_reload_increment = param_6;   /* DAT_1000_9794 */
        SND_PF->marker               = 0xf;       /* DAT_1000_979a (byte 0x0f) */
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

    record_min_status_code(param_1);    /* ORIGINAL: packed entry-FLAGS word (record_min_status_code is PORTED) */
    ret_status = 0xffff;
    if (!in_CF) {
        SND_PF->marker       = 0xf;      /* DAT_1000_979a (byte 0x0f) */
        SND_PF->pitch_reload = param_2;  /* DAT_1000_978a */
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
 *  The L4 init/IO callees pc_speaker_silence/mpu401_settle_delay/opl2_all_notes_off/
 *  mpu401_write_data_polled + the dispatch_b/c/d backends are PORTED (T5, below);
 *  mpu401_reset_to_uart + snddrv_init_substep (1000:8b2a) are PORTED here (Task A3,
 *  just below).
 *
 *  L3 TIMER-TABLE MGMT.  Two engine tables:
 *    - 0x5516 callback table (arm_timer_callback / disable_timer_callback): per-channel
 *      8-byte slot {current@0, reload@2, cb_off@4, cb_seg@6}, base 0x5516 + channel*8.
 *    - 0x549c slot table (set_timer_slot -> set_timer_slot_raw / get_timer_slot_field):
 *      per-channel 8-byte slot {value@0, 0@2, cb_off@4, cb_seg@6}, base 0x549c +
 *      (channel+2)*8.  set_timer_slot_raw is also the tail the L3 schedulers reach.
 * ════════════════════════════════════════════════════════════════════════════ */

/* still-stubbed callees the T4 bodies reach (see header block).
 *  pc_speaker_silence / mpu401_settle_delay / opl2_all_notes_off / mpu401_write_data_polled
 *  are NOW PORTED 1:1 below (Phase-6 T5 L4 hardware backends), declared in sound.h.
 *  mpu401_reset_to_uart + snddrv_init_substep (1000:8b2a) are PORTED here (Task A3). */

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
extern s16 mpu401_present;   /* DGROUP 0x557c — defined below with mpu401_write_data_polled (T5) */

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
 *  Task D1 PORTS maybe_opl2_detect_chip + opl2_reset_all_regs FOR REAL (see their
 *  definitions below, alongside opl_write_reg/opl_read_status) — no longer carve-out
 *  stubs; prototyped in sound.h now.
 *
 *  RECONSTRUCTION FIDELITY (the ZF-only branch): maybe_opl2_detect_chip's asm computes
 *  its detected/not-detected verdict into AH, then executes `AND AH,AH` (setting ZF)
 *  BEFORE popping its saved AX/CX/DX off the stack — so by the time it RETs, the AX
 *  register the caller sees is the CALLER's OWN original AX (restored by the POPs), not
 *  the verdict; only the ZERO FLAG survives to convey the result.  A void-returning C
 *  function cannot convey a flags-only outcome directly, so the branch this fn takes on
 *  that verdict is modelled via the file-scope `snd_opl_detect_zf` (1 = ZF set = "not
 *  detected", matching the snd_sched_carry_in modelled-flag convention) — the REAL
 *  maybe_opl2_detect_chip body (below) SETS this flag from the verdict it actually
 *  computes, so this already-validated ZF-branch keeps its exact existing contract. */
u16 midi_seq_step_active = 1;         /* DGROUP 0x557e — static image init = 0x0001 */
static u8 snd_opl_detect_zf = 1;      /* modelled ZF from maybe_opl2_detect_chip (1=not detected) */

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

/* out-of-scope MIDI-note carve-outs the 9 MIDI dispatch backends (below) reach, PLUS
 *  midi_emit_voice_msg_w1 (1000:8b81) — a NEW carve-out boundary Task D1 discovers:
 *  opl_set_note_params (1000:9241, reconstructed below) tail-calls it before storing the
 *  note params.  All four already carry canonical Ghidra names from a prior naming pass;
 *  none is reconstructed here (genuinely out of scope — separate future MIDI-engine
 *  work), matching the FUN_1000_6183 / pit_set_counter0 precedent. */
extern void seq_set_channel_param(void);     /* 1000:922c — OPL/PC-spk program-change (carve) */
extern void midi_emit_voice_msg_w1(void);    /* 1000:8b81 — OPL program-change entry (carve), reached from opl_set_note_params (D1) */
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
 *  exact register-stand-in values are therefore immaterial to the gate.  DEVIATION: the
 *  table read below masks snd_isr_restore_index with `& (table-length - 1)`, which the asm
 *  (`MOV BX,0x54de; ADD AX,AX; ADD BX,AX`) does NOT do (unconditional, no mask); the mask is
 *  an invented defensive clamp over the table's un-groundable real extent (kept to avoid a
 *  real C-array OOB, not a reproduction of engine behavior) — immaterial since the guarded
 *  body is provably dead per above.  Ported 1:1 for faithfulness + the BUMPY.EXE link
 *  (documented, same "reconstructed as documentation, not runtime-gated" precedent as the
 *  L5 ISR tone sequencer later in this file).
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
 *  UPDATE (Task E2): `midi_process_event` (1000:873c) — described below as
 *  "not-yet-started MIDI-engine work" at the time this section was written — is now
 *  RECONSTRUCTED in src/midi.c; it calls these 9 leaves EXACTLY as this section
 *  already documented (dispatch_b/c/d at 8751/8756/875b), staging
 *  snd_seq_event_al/snd_seq_cursor/snd_seq_default_chan first.  Left unchanged below
 *  (still historically accurate) other than this note — these 9 leaves themselves
 *  remain NOT independently oracle-exercised (see "NOT ORACLE-EXERCISED" below);
 *  midi_process_event's OWN Task C2 capture records validate its callER side
 *  (the staged globals + dispatch call), not these callees' internal bodies.
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
 *  already-committed call sites (`snddrv_dispatch_b_mode0();`-style, no args) are unchanged by this
 *  task (per the brief). `snd_seq_cursor` models DS:SI (read-and-advanced in place by
 *  LODSB, exactly as the asm does); `snd_seq_event_al` models AL; `snd_seq_default_chan`
 *  models the resolved CS:[BX+0x80] byte (BX itself, an ambient per-track table
 *  pointer, is never modelled as a real pointer — nothing in this codebase defines
 *  what it points at, matching the "genuinely out of scope" MIDI-engine boundary).
 *
 *  ── NOT ORACLE-EXERCISED (documented, same precedent as opl_play_note/opl2_all_notes_off) ────
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

/* ── snddrv_dispatch_b_mode0 (1000:91cf) — MIDI 0xF7 skip (device mode 0) ──
 *  0xF7 (SysEx continuation) handler for driver mode 0 (PC-speaker/silent): read a
 *  length byte from the MIDI stream and skip that many bytes; the event is not
 *  otherwise acted on. asm 1000:91cf verbatim: LODSB (len=*SI,SI++); XOR AH,AH;
 *  ADD SI,AX. Register-entry (DS:SI) — see the MIDI-dispatch-backends note above. */
void snddrv_dispatch_b_mode0(void)
{
    u8 len = *snd_seq_cursor;
    snd_seq_cursor++;
    snd_seq_cursor += len;
}

/* ── snddrv_dispatch_b_mode1 (1000:8e48) — MIDI 0xF7 skip (device mode 1) ──
 *  Identical body to _mode0 (91cf) — the 0xF7 skip is device-independent for modes 0/1.
 *  asm 1000:8e48 verbatim (byte-identical to 91cf). Kept as its OWN function (not
 *  merged/shared) per the "no collapsing near-duplicate mode backends" rule. */
void snddrv_dispatch_b_mode1(void)
{
    u8 len = *snd_seq_cursor;
    snd_seq_cursor++;
    snd_seq_cursor += len;
}

/* ── snddrv_dispatch_c_mode0 (1000:91d7) — MIDI 0xF0 skip (device mode 0) ──
 *  0xF0 (SysEx) handler for driver mode 0: same length-prefixed skip as dispatch_b's
 *  mode0/1 (91cf/8e48) — asm 1000:91d7 verbatim (byte-identical body, different event
 *  class / call site). Kept as its own function per the no-collapsing rule. */
void snddrv_dispatch_c_mode0(void)
{
    u8 len = *snd_seq_cursor;
    snd_seq_cursor++;
    snd_seq_cursor += len;
}

/* ── snddrv_dispatch_c_mode1 (1000:8e50) — MIDI 0xF0 skip (device mode 1) ──
 *  Same length-prefixed skip, dispatch_c's mode-1 variant. asm 1000:8e50 verbatim. */
void snddrv_dispatch_c_mode1(void)
{
    u8 len = *snd_seq_cursor;
    snd_seq_cursor++;
    snd_seq_cursor += len;
}

/* ── snddrv_dispatch_b_mode4 (1000:8af6) — MIDI 0xF7 busy-wait (mode 4) ──
 *  OPL/mode-4 handling of the 0xF7 event: reads TWO stream bytes (len, then the first data
 *  byte which stays in AL as snd_busy_delay's initial forwarded byte) and forwards
 *  snd_busy_delay(len-1) more to the MPU. asm 1000:8af6 verbatim: LODSB (CL=len); LODSB
 *  (AL=1st data byte); DEC CL; CALL 0x872e. The real CX's high byte (CH) is whatever the
 *  MIDI player's own CX held at the time — treated as 0. */
void snddrv_dispatch_b_mode4(void)
{
    u8 len;

    len = *snd_seq_cursor;
    snd_seq_cursor++;
    snd_seq_event_al = *snd_seq_cursor;   /* LODSB — AL = 2nd byte; snd_busy_delay forwards it */
    snd_seq_cursor++;
    snd_busy_delay((u16)(u8)(len - 1));
}

/* ── snddrv_dispatch_c_mode4 (1000:8b04) — MIDI 0xF0 busy-wait (mode 4) ──
 *  OPL/mode-4 handling of the 0xF0 event: reads ONE stream byte (a direct byte read +
 *  manual SI++, NOT LODSB) and busy-waits snd_busy_delay(len) — no decrement, unlike
 *  the _b_mode4 sibling. asm 1000:8b04 verbatim: MOV CL,[SI]; INC SI; CALL 0x872e. */
void snddrv_dispatch_c_mode4(void)
{
    u8 len = *snd_seq_cursor;
    snd_seq_cursor++;
    snd_busy_delay((u16)len);
}

/* ── snddrv_dispatch_d_mode4 (1000:8b0d) — MIDI channel-msg busy-wait (mode 4) ──
 *  OPL/mode-4 handling of a channel voice message (dispatch_d's event class, status
 *  nibble >= 0x80): default the channel nibble from the per-track byte (CS:[BX+0x80])
 *  when the low nibble is 0, busy_delay(2), or busy_delay(1) if the event's upper
 *  nibble is 0xC0 (program change). No MIDI-stream (SI) read at all — only AL/BX.
 *  asm 1000:8b0d verbatim: TEST AL,0xf; JNZ +; OR AL,[BX+0x80]; MOV CX,2; MOV AH,AL;
 *  AND AH,0xf0; CMP AH,0xc0; JNZ +; DEC CX; CALL 0x872e. */
void snddrv_dispatch_d_mode4(void)
{
    u8  al = snd_seq_event_al;
    u16 cx = 2;

    if ((al & 0xf) == 0) {
        al = (u8)(al | snd_seq_default_chan);
    }
    if ((u8)(al & 0xf0) == 0xc0) {
        cx = 1;
    }
    snd_seq_event_al = al;              /* AL still holds the (channel-merged) status byte at
                                        *  the CALL — snd_busy_delay forwards it to the MPU first */
    snd_busy_delay(cx);
}

/* ── snddrv_dispatch_d_mode0 (1000:91df) — MIDI channel-msg (PC-spk mode 0) ──
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
void snddrv_dispatch_d_mode0(void)
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

/* ── snddrv_dispatch_d_mode1 (1000:8e58) — MIDI channel-msg (OPL mode 1) ──
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
void snddrv_dispatch_d_mode1(void)
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
 *  a status bitmask of the DETECTED devices (0=none; |4 = MPU-401 present; |1 = OPL2/
 *  AdLib present).  sound_select_device ANDs this mask against the user-requested
 *  device (sound_device_state), so a device selects only when it was BOTH requested and
 *  detected.
 *
 *  RECONSTRUCTION FIDELITY (Ghidra `substep_ok` was a decompiler fiction — CORRECTED):
 *  the raw asm at 1000:88e5 tests the ZERO FLAG left by each sub-call's RETURN, not any
 *  local flag.  Each callee ends `MOV AX,[flag]; AND AX,AX; RET`, and the caller's POP/MOV
 *  between the CALL and the JZ leave ZF untouched:
 *      XOR CX,CX               ; status = 0
 *      CALL mpu401_reset_to_uart ; returns mpu401_present (0x557c)
 *      JZ  +; OR CX,4          ; status |= 4  iff it returned NONZERO (MPU detected)
 *      CALL snddrv_init_substep  ; returns midi_seq_step_active (0x557e)
 *      JZ  +; OR CX,1          ; status |= 1  iff it returned NONZERO (OPL detected)
 *  Ghidra could not model "branch on the callee's ZF" and invented a never-reassigned
 *  `substep_ok` (left `true`), making the two OR arms look dead — they are NOT.  An
 *  earlier reconstruction copied that fiction and hard-coded status=0, which zeroed the
 *  device mask for EVERY device (`req & 0 == 0`) → AdLib never reached snddrv_mode=1
 *  (OPL emit) → the title/intro MIDI was silent.  This body restores the real ZF-based
 *  accumulation: `if (callee() != 0) status |= bit` is exactly `AND AX,AX; JZ skip;
 *  OR CX,bit`.
 *
 *  Host sound gate unaffected: on the sound_ctest host, mpu401_present defaults 0
 *  (mpu401_reset_to_uart returns 0 → no |4) and host_in gives no OPL-timer pattern so
 *  maybe_opl2_detect_chip reports not-detected (snddrv_init_substep returns 0 → no |1)
 *  → status still 0; the captured device_init entry sound_device_state is 0, so the mask
 *  is 0 regardless.  On dosbox-x the emulated OPL2 answers the timer probe → |1 → an
 *  AdLib (bit-0) request survives the AND → snddrv_mode = 1<<0 = 1 (OPL backend). */
u16 snddrv_init(void)
{
    u16 status;

    status = 0xffff;
    if (sound_init_state == 0) {
        sound_init_state = 1;
        status = 0;                        /* XOR CX,CX */
        snd_init_substep_5584 = 1;        /* MOV [0x5584],AX (AX=1) */
        if (mpu401_reset_to_uart() != 0) { /* JZ tests the CALL's return ZF; OR CX,4 on nonzero */
            status = status | 4;
        }
        if (snddrv_init_substep() != 0) {  /* likewise: OR CX,1 when OPL detected */
            status = status | 1;
        }
        snddrv_mode = 1;                   /* MOV CS:[0x85b3],AX (AX=1) */
        sound_active_device_mask = 1;      /* MOV [0x5586],AX */
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
        mpu401_settle_delay();
    } else if (snddrv_mode == 1) {
        opl2_all_notes_off();
    }
}

/* ── snddrv_dispatch_b (1000:85db) ───────────────────────────────────────────────
 *  As _a; the decomp returns AX (an undefined register pass-through).  The backends are
 *  STUBBED (void); the return is the engine's ambient AX, not part of the validated
 *  state — modelled as 0. */
u16 snddrv_dispatch_b(void)
{
    if (snddrv_mode == 0) {
        snddrv_dispatch_b_mode0();
    } else if (snddrv_mode == 4) {
        snddrv_dispatch_b_mode4();
    } else if (snddrv_mode == 1) {
        snddrv_dispatch_b_mode1();
    }
    return 0;   /* decomp returns the ambient in_AX; backends STUBBED → not validated */
}

/* ── snddrv_dispatch_c (1000:8600) ───────────────────────────────────────────────── */
void snddrv_dispatch_c(void)
{
    if (snddrv_mode == 0) {
        snddrv_dispatch_c_mode0();
    } else if (snddrv_mode == 4) {
        snddrv_dispatch_c_mode4();
    } else if (snddrv_mode == 1) {
        snddrv_dispatch_c_mode1();
    }
}

/* ── snddrv_dispatch_d (1000:8626) ───────────────────────────────────────────────── */
void snddrv_dispatch_d(void)
{
    if (snddrv_mode == 0) {
        snddrv_dispatch_d_mode0();
    } else if (snddrv_mode == 4) {
        snddrv_dispatch_d_mode4();
    } else if (snddrv_mode == 1) {
        snddrv_dispatch_d_mode1();
    }
}

/* ── snd_busy_delay (1000:872e) ──────────────────────────────────────────────────
 *  Busy-wait: call the timing primitive mpu401_write_data_polled CX+1 times.  This is a
 *  naked/asm routine — entry AL feeds the first mpu401_write_data_polled (MOV AH,AL), then
 *  a LODSB/LOOP over CX reads bytes from DS:SI and calls mpu401_write_data_polled for each.
 *  The decomp models it as the CX+1-iteration delay loop (the LODSB source bytes are
 *  consumed by the STUBBED timing primitive and do not affect the validated state).
 *  Modelled with an explicit count parameter (the engine's CX); the LODSB byte stream is
 *  not reconstructed (→ T6 with the L4 timing port).  RECONSTRUCTION FIDELITY:
 *  register-args asm routine; see report. */
void snd_busy_delay(u16 count)
{
    /* Despite the name this is the MPU-401 raw-MIDI FORWARDER, not a delay: it sends the
     *  ambient status byte (AL) then LODSB-forwards `count` more stream bytes to the MPU data
     *  port (0x330) via mpu401_write_data_polled.  asm 1000:872e verbatim: MOV AH,AL; CALL
     *  89e2; [loop: LODSB; MOV AH,AL; CALL 89e2].  mpu401_write_data_polled writes AH
     *  (snd_mpu_byte_89e2), so each byte is staged there first.  Fixed 2026-07-13: the prior
     *  reconstruction called mpu401_write_data_polled without staging the byte or advancing
     *  snd_seq_cursor — so mode-4 (MT-32/MPU) forwarded stale bytes AND left the MIDI cursor
     *  misaligned for midi_process_event.  Now advances the real cursor (LODSB) so the MPU
     *  receives the live MIDI event bytes.  Host gate: snd_seq_cursor points at the zero
     *  scratch stream (no captured MIDI) and count stays < 256, so the LODSBs stay in-bounds
     *  and neither snd_mpu_byte_89e2 nor snd_seq_cursor is in the SND_SNAP. */
    snd_mpu_byte_89e2 = snd_seq_event_al;  /* MOV AH,AL (stage the status byte) */
    mpu401_write_data_polled();            /* CALL 0x89e2 (send AH) */
    do {
        snd_seq_event_al = *snd_seq_cursor;    /* LODSB — AL = *SI */
        snd_seq_cursor++;                       /*         SI++     */
        snd_mpu_byte_89e2 = snd_seq_event_al;  /* MOV AH,AL */
        mpu401_write_data_polled();            /* CALL 0x89e2 */
        count = count - 1;
    } while (count != 0);                       /* LOOP */
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
 *  per-channel data (0x55b4 / 0x5614).
 *
 *  CORRECTION (Task D2): the prior "an OPL-init routine fills at RUNTIME (the static
 *  image leaves them zero — they are BSS)" claim is WRONG for at least the low portion
 *  of all 4 tables — verified by reading local/originals/unpacked/BUMPY_unpacked.exe's
 *  own DGROUP data directly (DGROUP:off -> file offset 0x103b*16 + off + e_cparhdr*16,
 *  the same formula tools/midi_oracle.py/sound_oracle.py use for DG_LIN): the image
 *  bytes at 0x5593/0x559c/0x55b4/0x5614 are NOT zero — they are baked, initialised
 *  DGROUP DATA (the 4 tables visibly overlap each other's declared 0x100-byte extent —
 *  e.g. opl_fnum_hi_559c[0] == opl_fnum_lo_5593[9] byte-for-byte, since 0x559c is only
 *  9 bytes past 0x5593 — a real memory-layout fact the 4-separate-array C model can't
 *  fully capture, but each array's OWN byte range is transcribed exact-image-bytes
 *  below, the same "L1 event-wrapper LUTs — exact image bytes" convention
 *  action_sound_lut_opl_260e (game_stubs.c) already uses).  Task D2's own
 *  opl_event_note_on / emit_midi_voice_message differential (validate_midi.sh) is what
 *  surfaced this: with these at their old zero default, the port-write gate diverged
 *  from the MIDI-oracle trace's captured OUT values (e.g. opl_fnum_lo_5593[3] must be 8,
 *  not 0, for opl_event_note_on's OWN oracle-captured OUT sequence to reproduce).
 *  RECONSTRUCTION FIDELITY: bytes past roughly index 0x88 look like adjacent,
 *  unrelated DGROUP data (word-sized values in the 0x0d00-0x1100 range, shared
 *  identically across the overlapping tail of chan_data_55b4/chan_idx_5614) rather than
 *  a continuation of each table's own logical content — transcribed anyway (exact image
 *  bytes, not invented) since the C array's declared 0x100 extent already predates this
 *  task and nothing in the reconstructed call graph indexes that far. */
u8 opl_reg_shadow_80cc[0x100];     /* CODE   0x80cc — OPL register write-back shadow */
u8 opl_fnum_lo_5593[0x100] =       /* DGROUP 0x5593 — per-note F-number low byte */
{
    0x00,0x01,0x02,0x08,0x09,0x0a,0x10,0x11,0x12,0x59,0x01,0x6d,0x01,0x83,0x01,0x9a,
    0x01,0xb2,0x01,0xcc,0x01,0xe8,0x01,0x05,0x02,0x23,0x02,0x44,0x02,0x66,0x02,0x8b,
    0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x02,0x02,0x02,0x02,0x02,0x02,
    0x02,0x02,0x02,0x02,0x02,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,
    0x03,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x05,0x05,0x05,
    0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x06,0x06,0x06,0x06,0x06,0x06,0x06,
    0x06,0x06,0x06,0x06,0x06,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,
    0x07,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,
    0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,
    0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,
    0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,
    0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,
    0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,
    0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x37,0x0f,0x3b,0x0f,0x39,0x0f,0x3a,
    0x0f,0x3a,0x0f,0x3a,0x0f,0x38,0x0f,0xe0,0x0d,0x35,0x0f,0xc3,0x0e,0xcc,0x0e,0xc4,
};
u8 opl_fnum_hi_559c[0x100] =       /* DGROUP 0x559c — per-note F-number/block word */
{
    0x59,0x01,0x6d,0x01,0x83,0x01,0x9a,0x01,0xb2,0x01,0xcc,0x01,0xe8,0x01,0x05,0x02,
    0x23,0x02,0x44,0x02,0x66,0x02,0x8b,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x03,0x03,0x03,0x03,
    0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,
    0x04,0x04,0x04,0x04,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,
    0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x07,0x07,0x07,0x07,
    0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,
    0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,
    0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x37,0x0f,0x3b,0x0f,0x39,0x0f,0x3a,0x0f,0x3a,0x0f,0x3a,0x0f,0x38,0x0f,0xe0,0x0d,
    0x35,0x0f,0xc3,0x0e,0xcc,0x0e,0xc4,0x0e,0xd4,0x0e,0x3c,0x0e,0x36,0x0f,0xb0,0x0d,
};
u8 opl_chan_data_55b4[0x100] =     /* DGROUP 0x55b4 — per-channel feedback/connection */
{
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,
    0x02,0x02,0x02,0x02,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,
    0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x05,0x05,0x05,0x05,
    0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,
    0x06,0x06,0x06,0x06,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,
    0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,
    0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x37,0x0f,0x3b,0x0f,0x39,0x0f,0x3a,0x0f,
    0x3a,0x0f,0x3a,0x0f,0x38,0x0f,0xe0,0x0d,0x35,0x0f,0xc3,0x0e,0xcc,0x0e,0xc4,0x0e,
    0xd4,0x0e,0x3c,0x0e,0x36,0x0f,0xb0,0x0d,0xb0,0x0d,0x3c,0x0f,0x3d,0x0f,0x3e,0x0f,
    0x00,0x00,0x01,0x00,0x02,0x00,0x03,0x00,0x05,0x00,0x07,0x0f,0x08,0xff,0x02,0x00,
};
u8 opl_chan_idx_5614[0x100] =      /* DGROUP 0x5614 — per-channel block index */
{
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,
    0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,
    0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x37,0x0f,0x3b,0x0f,0x39,0x0f,0x3a,0x0f,
    0x3a,0x0f,0x3a,0x0f,0x38,0x0f,0xe0,0x0d,0x35,0x0f,0xc3,0x0e,0xcc,0x0e,0xc4,0x0e,
    0xd4,0x0e,0x3c,0x0e,0x36,0x0f,0xb0,0x0d,0xb0,0x0d,0x3c,0x0f,0x3d,0x0f,0x3e,0x0f,
    0x00,0x00,0x01,0x00,0x02,0x00,0x03,0x00,0x05,0x00,0x07,0x0f,0x08,0xff,0x02,0x00,
    0x87,0x10,0xd5,0x11,0x56,0x11,0x5f,0x11,0x57,0x11,0x67,0x11,0xb8,0x10,0xd6,0x11,
    0x60,0x10,0x60,0x10,0xdc,0x11,0xdd,0x11,0xde,0x11,0x00,0x00,0x01,0x00,0x02,0x00,
    0x03,0x00,0x05,0x01,0x07,0x0f,0x08,0xff,0x02,0x00,0x15,0x54,0x3b,0x10,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf0,0x56,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

/* ── MPU-401 register-byte recovery (host port-write gate) ───────────────────────────
 *  mpu401_write_data_polled writes the byte in register AH; snd_emit_raw_sample writes
 *  args sample_lo/sample_hi; opl_write_reg takes reg=AH/val=AL.  The engine passes these
 *  in registers/args the
 *  SND_SNAP does not serialize, so on the host the replay harness recovers them from the
 *  record's OUT events and publishes them via these file-scope inputs before the call (the
 *  driver still must emit them at the right PORTS in the right ORDER for the gate to pass —
 *  a wrong port/order/extra/missing write diverges).  On the real wcc build these are
 *  unused (the byte arrives in AH from the caller); the host wrappers set them.  Defaults
 *  reproduce the engine's first-seen sequence. */
u8 snd_mpu_byte_89e2 = 0x99;       /* the byte mpu401_write_data_polled writes to 0x330 (engine: AH) */

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

/* MPU-401 poll residual / presence (DGROUP DAT_1000_85a1 / DAT_203b_557c).
   mpu401_write_data_polled writes both on a DSR poll TIMEOUT.
   mpu401_present is STATICALLY INITIALISED to 1 in the image (DGROUP 0x557c = 0x0001):
   the engine assumes an MPU-401 is present at boot, then mpu401_reset_to_uart's handshake
   clears it to 0 only if the chip fails to answer.  An earlier reconstruction left it BSS-0
   (its comment "defaults 0 — no reconstructed code sets it nonzero" missed the static image
   value), which made mpu401_reset_to_uart early-return 0 → snddrv_init never set the MPU
   status bit (|4) → an MT-32 (mask 4) request never selected snddrv_mode=4 (MPU backend) →
   MT-32 was silent.  Faithful value = 1 (verified vs the unpacked image DGROUP). */
s16 midi_track_count;
s16 mpu401_present = 1;                 /* DGROUP 0x557c static init = 0x0001 */

/* ── mpu401_write_data_polled (1000:89e2) — MPU-401 byte-out primitive ────────────────
 *  Poll status port 0x331 until DSR (bit 0x40) is CLEAR (CX from 0 -> up to 0x10000
 *  iters), then write the data byte (engine AH) to data port 0x330.  On a poll TIMEOUT
 *  (bit still set) it records the residual count into midi_track_count/mpu401_present and
 *  writes nothing.  In the capture 0x331 reads 0x00 (DSR clear) immediately, so it always
 *  writes the byte.  asm 1000:89e2: XOR CX,CX; IN AL,0x331; TEST 0x40; LOOPNZ; JNZ fail;
 *  DEC DX(->0x330); MOV AL,AH; OUT DX,AL.  The host byte arrives via snd_mpu_byte_89e2. */
void mpu401_write_data_polled(void)
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

/* ── snd_emit_raw_sample (1000:8a07) — MPU-401 raw 2-byte sample emit ────────────────
 *  Writes the MIDI command 0x99 then the two sample bytes (sample_lo, sample_hi) to the
 *  MPU data port, each via mpu401_write_data_polled (poll-then-write).  asm: MOV AH,0x99;
 *  CALL 89e2; MOV AH,[BP+4]; CALL 89e2; MOV AH,[BP+6]; CALL 89e2.  So the OUT sequence is
 *  0x330=0x99, 0x330=sample_lo, 0x330=sample_hi (each gated by a 0x331 poll).  The AH
 *  byte for mpu401_write_data_polled is staged through snd_mpu_byte_89e2 (the engine's
 *  AH register). */
void snd_emit_raw_sample(u8 sample_lo, u8 sample_hi)
{
    snd_mpu_byte_89e2 = 0x99;            /* MOV AH,0x99 */
    mpu401_write_data_polled();
    snd_mpu_byte_89e2 = sample_lo;       /* MOV AH,[BP+4] */
    mpu401_write_data_polled();
    snd_mpu_byte_89e2 = sample_hi;       /* MOV AH,[BP+6] */
    mpu401_write_data_polled();
}

/* ── mpu401_settle_delay (1000:8ad0) — MPU-401 settle delay ──────────────────────────
 *  Nested loop: outer BL = 9 down to 1 (BH stays 0x90), inner CX = 0x7f down to 1.  Each
 *  inner iteration writes 3 MPU bytes via mpu401_write_data_polled: AH = 0x90+BL
 *  (=BH+BL), then AH = CL, then AH = 0.  asm 1000:8ad0: MOV BX,0x9009; (outer) MOV
 *  CX,0x7f; (inner) MOV AH,BH; ADD AH,BL; CALL 89e2; MOV AH,CL; CALL 89e2; XOR AH,AH;
 *  CALL 89e2; LOOP; DEC BL; JNZ.  Fully deterministic (no external input) → port-write-
 *  gate validated. */
void mpu401_settle_delay(void)
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
            mpu401_write_data_polled();
            snd_mpu_byte_89e2 = cl;                /* AH = CL */
            mpu401_write_data_polled();
            snd_mpu_byte_89e2 = 0;                 /* XOR AH,AH */
            mpu401_write_data_polled();
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

/* ── opl2_all_notes_off (1000:8e2f) — OPL2 all-notes-off ─────────────────────────────
 *  Loop voice = 1..9 calling opl_play_note(0,0,0,voice).  asm 1000:8e2f: MOV BX,1; (loop)
 *  PUSH BX; XOR AX,AX; PUSH AX x3; CALL 905d; ...; INC BX; CMP BX,9; JLE.  Reaches
 *  opl_play_note (a documented port-write-gate exclusion — runtime freq tables), so 8e2f
 *  inherits the exclusion (registered UNPORTED); ported 1:1 for faithfulness + the link. */
void opl2_all_notes_off(void)
{
    int voice_index;

    voice_index = 1;
    do {
        opl_play_note(0, 0, 0, (u16)voice_index);
        voice_index = voice_index + 1;
    } while (voice_index < 10);
}

/* ── opl_read_status (1000:9056) — read the OPL2/AdLib status byte: PORTED (Task D1) ──
 *  IN AL,0x388.  asm 1000:9056 verbatim: PUSH DX; MOV DX,0x388; IN AL,DX; POP DX; RET. */
u8 opl_read_status(void)
{
    return (u8)inp(0x388);
}

/* ── opl2_reset_all_regs (1000:8eeb) — OPL2 full-register init sequence: PORTED (D1) ──
 *  233 back-to-back opl_write_reg calls (each 2 OUT + 41 settle-read status IN's, per
 *  opl_write_reg's own header) programming the ENTIRE OPL2 register file to a known
 *  state: 18 individually-coded 0xff writes (envelope/wave-select-adjacent regs 0x40-
 *  0x42/0x48-0x4a/0x50-0x52/0x43-0x45/0x4b-0x4d/0x53-0x55 — the operator release/sustain
 *  regs for all 9x2 operators), then two zero-fill sweeps (reg 0x01..0x3f, reg 0x60..0xf5
 *  — blanking the rest of the OPL2 register space including the CSW/AM-VIB/freq/level/
 *  waveform regs), then 2 final fixed writes (reg 0x04=0x06 timer-control, reg 0xbd=0x00
 *  rhythm/depth).  asm 1000:8eeb verbatim: 18 explicit MOV AH,n/MOV AL,0xff/CALL 9007
 *  triples (each wrapped in a dead PUSH AX/POP AX in the original — the Watcom-recompiled
 *  1:1 body drops that dead register save/restore, matching the project's stack-probe-
 *  omission convention), then `MOV AX,0x100; (loop) CALL 9007; INC AH; CMP AH,0x40; JNZ
 *  loop` (regs 0x01..0x3f, val 0), then `MOV AX,0x6000; (loop) CALL 9007; INC AH; CMP
 *  AH,0xf6; JNZ loop` (regs 0x60..0xf5, val 0), then `MOV AX,0x406; CALL 9007` and
 *  `MOV AX,0xbd00; CALL 9007`.  18+63+150+1+1 = 233 writes -> 466 total OUT 0x388/0x389
 *  events, byte-for-byte confirmed against the Task C2 MIDI-oracle's sole capture record
 *  for this fn (n_io=10019, out=466, in=9553; local/build/render/midi_trace.bin). */
void opl2_reset_all_regs(void)
{
    u8 reg;

    opl_write_reg(0x40, 0xff);
    opl_write_reg(0x41, 0xff);
    opl_write_reg(0x42, 0xff);
    opl_write_reg(0x48, 0xff);
    opl_write_reg(0x49, 0xff);
    opl_write_reg(0x4a, 0xff);
    opl_write_reg(0x50, 0xff);
    opl_write_reg(0x51, 0xff);
    opl_write_reg(0x52, 0xff);
    opl_write_reg(0x43, 0xff);
    opl_write_reg(0x44, 0xff);
    opl_write_reg(0x45, 0xff);
    opl_write_reg(0x4b, 0xff);
    opl_write_reg(0x4c, 0xff);
    opl_write_reg(0x4d, 0xff);
    opl_write_reg(0x53, 0xff);
    opl_write_reg(0x54, 0xff);
    opl_write_reg(0x55, 0xff);

    for (reg = 0x01; reg != 0x40; reg++) {    /* MOV AX,0x100;  INC AH; CMP AH,0x40; JNZ */
        opl_write_reg(reg, 0x00);
    }
    for (reg = 0x60; reg != 0xf6; reg++) {    /* MOV AX,0x6000; INC AH; CMP AH,0xf6; JNZ */
        opl_write_reg(reg, 0x00);
    }

    opl_write_reg(0x04, 0x06);                /* MOV AX,0x406  */
    opl_write_reg(0xbd, 0x00);                /* MOV AX,0xbd00 */
}

/* ── maybe_opl2_detect_chip (1000:8fb6) — OPL2 chip-detect probe: PORTED (Task D1) ──
 *  The classic AdLib/OPL2 timer-based presence probe: reset both OPL timers (reg
 *  0x04=0x60, then reg 0x04=0x80 — mask both), latch the status byte (status_before;
 *  timer flags should read clear on a real chip), program Timer1's counter (reg
 *  0x02=0xff) and start/unmask Timer1 (reg 0x04=0x21), burn a fixed ~199-iteration
 *  status-read delay (LOOP CX=0xc7) for Timer1 to expire, latch the status byte again
 *  (status_after — should now show the Timer1-expired + IRQ-request flags on a real
 *  chip), then reset/mask the timers again (reg 0x04=0x60, reg 0x04=0x80).  Verdict:
 *  "detected" iff (status_after & 0xe0)==0xc0 AND (status_before & 0xe0)==0.  asm
 *  1000:8fb6 verbatim: 6 opl_write_reg calls -> 12 OUT events + 201 opl_read_status calls
 *  (1 + a 199-iteration LOOP + 1) -> 201 IN events, byte-for-byte confirmed against the
 *  Task C2 MIDI-oracle's sole capture record for this fn (n_io=459, out=12, in=447).
 *
 *  RECONSTRUCTION FIDELITY (ZF-only return — see snddrv_init_substep's own note above):
 *  the asm computes the verdict into AH then executes `AND AH,AH` (setting ZF) BEFORE
 *  popping the ORIGINAL caller's AX/CX/DX back off the stack — so by RET the caller's
 *  AX/CX/DX are its own unchanged values; only the ZERO FLAG (ZF = AH==0, i.e. "NOT
 *  detected") survives to the caller.  Modelled via the file-scope snd_opl_detect_zf flag
 *  (1 = ZF set = not detected), matching the modelled-flag convention already established
 *  at snddrv_init_substep.  In the MIDI-oracle capture (no real OPL2 chip behind the
 *  Unicorn IN hook — port 0x388 always reads 0x00) status_before==status_after==0, so
 *  (status_after&0xe0)==0 != 0xc0 -> "not detected", matching the pre-existing default
 *  (snd_opl_detect_zf==1). */
void maybe_opl2_detect_chip(void)
{
    u8  status_before, status_after;
    u8  detected;
    u16 delay;

    opl_write_reg(0x04, 0x60);
    opl_write_reg(0x04, 0x80);
    status_before = opl_read_status();

    opl_write_reg(0x02, 0xff);
    opl_write_reg(0x04, 0x21);
    delay = 0xc7;                              /* MOV CX,0xc7 */
    do {                                       /* CALL 9056; LOOP 8fd8 */
        (void)opl_read_status();
        delay--;
    } while (delay != 0);
    status_after = opl_read_status();

    opl_write_reg(0x04, 0x60);
    opl_write_reg(0x04, 0x80);

    detected = 0;
    if ((u8)(status_after & 0xe0) == 0xc0 && (u8)(status_before & 0xe0) == 0x00) {
        detected = 1;
    }
    snd_opl_detect_zf = (u8)(detected == 0);   /* AND AH,AH -> ZF = (AH==0) */
}

/* CODE-segment scratch bytes at 1000:9272/0x9273 — immediately following
 * opl_set_note_params's own RET (1000:9271) — the 2 note-trigger bytes it stages for
 * opl_event_note_on. */
u8 opl_note_param1;   /* CODE 0x9272 */
u8 opl_note_param2;   /* CODE 0x9273 */

/* ── opl_set_note_params (1000:9241) — MIDI-voice note-trigger front end: PORTED (D1) ──
 *  Stack-arg near fn: chan=[BP+4] (word), note_param1(byte)=[BP+6], note_param2(byte)=
 *  [BP+8].  Tail-calls the out-of-scope midi_emit_voice_msg_w1 (1000:8b81; BX=chan,
 *  AH=1 in the real asm — see the RECONSTRUCTION FIDELITY note below), stores
 *  note_param1/note_param2 into the CODE-segment scratch bytes at 0x9272/0x9273, then
 *  tail-calls the out-of-scope opl_event_note_on (1000:8ea3; AL=1, DS:SI->0x9272 in the
 *  real asm) to actually trigger the note.  asm 1000:9241 verbatim: PUSH BP; MOV BP,SP;
 *  PUSH DS; PUSH SI; MOV AX,0x1000; MOV DS,AX; MOV SI,0x9272; MOV BX,[BP+4]; MOV AH,1;
 *  CALL 8b81; MOV AX,0x1000; MOV DS,AX; MOV SI,0x9272 (DS:SI reloaded — clobbered by the
 *  w1 call); MOV AL,[BP+6]; MOV [SI],AL; MOV AL,[BP+8]; MOV [SI+1],AL; MOV AL,1;
 *  CALL 8ea3; POP SI; POP DS; POP BP; RET.
 *
 *  RECONSTRUCTION FIDELITY (carve-out register inputs not wired): midi_emit_voice_msg_w1
 *  and opl_event_note_on are out-of-scope carve-out leaves (game_stubs.c no-ops), matching
 *  the project's existing precedent for calling them (e.g. snddrv_dispatch_d_mode0/mode1's
 *  bare `seq_set_channel_param();` / `opl_event_note_on();` calls above) — the real asm's
 *  BX/AH/AL/DS:SI register inputs to those two leaves are therefore NOT wired here (a stub
 *  takes no args); `chan` is read off the stack (matching the real ABI) but only feeds
 *  those carved-out calls, so it is otherwise unused on this host build.
 *
 *  RECONSTRUCTION FIDELITY (port-write differential not meaningful here — a capture-scope
 *  fact, not a weakened gate): opl_set_note_params' OWN code issues ZERO opl_write_reg
 *  calls.  Every one of the 32 OUT events the Task C2 MIDI-oracle capture recorded for
 *  this fn's sole trace record (n_io=688, out=32, in=656) originates from the REAL
 *  (non-stub) midi_emit_voice_msg_w1 (24 OUT: 12 real opl_write_reg calls it makes) +
 *  opl_event_note_on (8 OUT: 4 more) it calls — both out of THIS task's scope, confirmed
 *  by summing their OWN separately-captured records (24+8=32, exact).  Reproducing that
 *  port sequence on the host would require reconstructing those two carve-outs for real,
 *  which is not this task.  So tools/midi_ctest.c registers this fn with the SEMANTIC-
 *  state comparator (not the port-write one): the real asm touches none of the tracked
 *  MIDI sequencer-state globals either (confirmed via the disasm above — it only writes
 *  the untracked opl_note_param1/param2 scratch bytes), so the differential still asserts
 *  something true and reproducible (no sequencer-state corruption) without faking a port
 *  sequence this task cannot produce.  See docs/reconstruction-fidelity.md. */
void opl_set_note_params(u16 chan, u8 note_param1, u8 note_param2)
{
    (void)chan;                 /* real asm: BX=chan (carved-out midi_emit_voice_msg_w1 input) */
    midi_emit_voice_msg_w1();   /* carve-out (see FIDELITY note above); real asm: AH=1 */
    opl_note_param1 = note_param1;
    opl_note_param2 = note_param2;
    opl_event_note_on();        /* carve-out; real asm: AL=1, DS:SI -> 0x9272/0x9273 */
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

/* ════════════════════════════════════════════════════════════════════════════
 *  PC-SPEAKER MUSIC RENDERER (device mode 0) — pcspk_music_render (1000:9136).
 *
 *  When no OPL/MPU device is selected (snddrv_mode==0, i.e. the PC BASE menu choice),
 *  select_sound_device_from_mask installs THIS callback into 0x549c timer slot channel 1
 *  (set_timer_slot_raw(1, 0x64, 0x9136, 0x1000)); the int-8 mux fires it every ~64 ticks.
 *  It renders the MIDI notes that snddrv_dispatch_d_mode0 stashes into snd_voice_table
 *  (CODE 0x83cc, one byte per MIDI channel) onto the PC speaker (PIT ch2 + gate 0x61) as a
 *  2-voice round-robin: each firing it flips pcspk_rr_index 0<->1, picks voice (index+1)
 *  = MIDI channel 1 or 2, and plays that channel's current note.  Reached in the original
 *  ONLY via the installed far pointer (no Ghidra function boundary — like the tempo tick
 *  and the tone callbacks), so it is reconstructed from the raw disassembly (1000:9136).
 *  Reconstructed 2026-07-13 to fix "PC BASE plays no music" — the callback WAS installed
 *  (select_sound_device_from_mask) but never reconstructed, so the sweep dispatched nothing. */

/* chan_param_table (CODE 0x8473, midi.c) — per-MIDI-channel program byte, set by
 *  seq_set_channel_param on a 0xC0 program-change; the renderer reads channels 1/2. */
extern u8 chan_param_table[16];

/* pcspk_rr_index (CODE 0x83ec) — 2-voice round-robin selector (0<->1). */
u16 pcspk_rr_index;

/* pcspk_transpose_map (CODE 0x83f1) — program(byte) -> freq-index transpose offset.  All
 *  zero in the unpacked image (every program -> 0), so the transpose is always 0.
 *  RECONSTRUCTION FIDELITY: the original addresses it as CS:[0x83f1 + program] immediately
 *  below pcspk_freq_table (0x8489); this reconstruction models it as a separate zero array,
 *  so a program byte > 0x97 (which in the image would read into the freq table) is not
 *  bit-exact — immaterial here (the data is zero and BUMPY.MID's channel-1/2 programs stay
 *  in range). */
static const u8 pcspk_transpose_map[0x100] = { 0 };

/* pcspk_freq_table (CODE 0x8489) — note -> PIT ch2 divisor, little-endian words indexed by
 *  (note*2 + 0x30 + transpose) as a BYTE offset.  Dumped verbatim from the unpacked image
 *  (0x130 bytes covers every MIDI note 0..127: max byte index 127*2+0x30 = 0x12e).  Notes
 *  below ~14 map to 0x0001 (near-inaudible); the tail past the real table (idx>=0x116) is
 *  the image's adjacent bytes, which the original reads identically for out-of-range notes. */
static const u8 pcspk_freq_table[0x130] = {
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x01, 0x00, 0xcc, 0xfd, 0x8d, 0xef, 0x1b, 0xe2, 0x6a, 0xd5,
    0x70, 0xc9, 0x22, 0xbe, 0x76, 0xb3, 0x63, 0xa9, 0xe1, 0x9f, 0xe8, 0x96,
    0x70, 0x8e, 0x71, 0x86, 0xe6, 0x7e, 0xc6, 0x77, 0x0d, 0x71, 0xb5, 0x6a,
    0xb8, 0x64, 0x11, 0x5f, 0xbb, 0x59, 0xb1, 0x54, 0xf0, 0x4f, 0x74, 0x4b,
    0x38, 0x47, 0x38, 0x43, 0x73, 0x3f, 0xe3, 0x3b, 0x86, 0x38, 0x5a, 0x35,
    0x5c, 0x32, 0x88, 0x2f, 0xdd, 0x2c, 0x58, 0x2a, 0xf8, 0x27, 0xba, 0x25,
    0x9c, 0x23, 0x9c, 0x21, 0xb9, 0x1f, 0xf1, 0x1d, 0x43, 0x1c, 0xad, 0x1a,
    0x2e, 0x19, 0xc4, 0x17, 0x6e, 0x16, 0x2c, 0x15, 0xfc, 0x13, 0xdd, 0x12,
    0xce, 0x11, 0xce, 0x10, 0xdc, 0x0f, 0xf8, 0x0e, 0x21, 0x0e, 0x56, 0x0d,
    0x97, 0x0c, 0xe2, 0x0b, 0x37, 0x0b, 0x96, 0x0a, 0xfe, 0x09, 0x6e, 0x09,
    0xe7, 0x08, 0x67, 0x08, 0xee, 0x07, 0x7c, 0x07, 0x10, 0x07, 0xab, 0x06,
    0x4b, 0x06, 0xf1, 0x05, 0x9b, 0x05, 0x4b, 0x05, 0xff, 0x04, 0xb7, 0x04,
    0x73, 0x04, 0x33, 0x04, 0xf7, 0x03, 0xbe, 0x03, 0x88, 0x03, 0x55, 0x03,
    0x25, 0x03, 0xf8, 0x02, 0xcd, 0x02, 0xa5, 0x02, 0x7f, 0x02, 0x5b, 0x02,
    0x39, 0x02, 0x19, 0x02, 0xfb, 0x01, 0xdf, 0x01, 0xc4, 0x01, 0xaa, 0x01,
    0x92, 0x01, 0x7c, 0x01, 0x66, 0x01, 0x52, 0x01, 0x3f, 0x01, 0x2d, 0x01,
    0x1c, 0x01, 0x0c, 0x01, 0xfd, 0x00, 0xef, 0x00, 0xe2, 0x00, 0xd5, 0x00,
    0xc9, 0x00, 0xbe, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x40, 0x42, 0x0f, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x53, 0x2e, 0x8b, 0x1e,
};

/* ── pcspk_io_settle (1000:8a3b) — tiny I/O settle delay (LOOP CX=4) between PIT writes. */
static void pcspk_io_settle(void)
{
    volatile u16 cx = 4;                          /* MOV CX,4; LOOP $ */
    while (cx != 0) {
        cx = (u16)(cx - 1);
    }
}

/* ── pcspk_set_pit_ch2 (1000:91b9) — program PIT channel 2 with a 16-bit divisor ────
 *  OUT 0x43,0xB6 (ch2, mode 3, lo/hi byte, binary); then divisor lo then hi to 0x42, each
 *  followed by an I/O settle.  asm 1000:91b9 verbatim. */
static void pcspk_set_pit_ch2(u16 divisor)
{
    outp(0x43, 0xb6);                             /* MOV AL,0xB6; OUT 0x43,AL */
    pcspk_io_settle();                            /* CALL 0x8a3b */
    outp(0x42, (u8)divisor);                       /* OUT 0x42,AL (lo) */
    pcspk_io_settle();
    outp(0x42, (u8)(divisor >> 8));                /* MOV AL,AH; OUT 0x42,AL (hi) */
    pcspk_io_settle();
}

/* ── pcspk_music_render (1000:9136) — PC-speaker music voice tick (installed timer cb) ─
 *  asm 1000:9136 verbatim (see the block header above). */
void pcspk_music_render(void)
{
    u16 voice;          /* AX — 1..2 after the round-robin */
    u8  transpose;      /* CL -> CX (freq-index offset; always 0 here) */
    u8  note;
    u16 divisor;
    u8  port61;

    voice = (u16)(pcspk_rr_index + 1);            /* 913b..913f: rr_index+1 */
    if (voice >= 2) {                              /* 9140 CMP AX,2; JB */
        voice = 0;                                 /* 9145 XOR AX,AX (wrap) */
    }
    pcspk_rr_index = voice;                        /* 9147 store rr_index (0 or 1) */
    voice = (u16)(voice + 1);                      /* 914b INC AX -> voice 1 or 2 */

    /* transpose = pcspk_transpose_map[chan_param_table[voice]]  (both zero -> 0) */
    transpose = pcspk_transpose_map[chan_param_table[voice]];   /* 914c..915f */

    note = snd_voice_table[voice];                 /* 9161..9166: voice_table[voice] */
    if (note < 0x0a) {                              /* 9169 CMP AL,0xA; JAE */
        note = 0;                                  /* 916d XOR AL,AL */
    }
    voice = (u16)((u16)note * 2);                  /* 916f..9171: note*2 */
    if (voice == 0) {                               /* 9175 JE silence (91a9) */
        snd_select_scratch_83ee = 0;               /* 91a9..91ab: speaker-on flag = 0 */
        snd_select_scratch_83ef = 0;               /* 91af: last divisor = 0 */
        port61 = (u8)(inp(0x61) & 0xfc);           /* 91b3..91b5: gate off */
        outp(0x61, port61);                        /* 91a6 OUT 0x61,AL */
        return;                                    /* 91a8 RETF */
    }
    voice = (u16)(voice + 0x30 + transpose);       /* 9177..917a: note*2 + 0x30 + transpose */
    divisor = (u16)(pcspk_freq_table[voice] |      /* 917c..9181: word @ freq_table + index */
                    ((u16)pcspk_freq_table[voice + 1] << 8));
    if (divisor != snd_select_scratch_83ef) {      /* 9184 CMP AX,[0x83ef]; JE */
        pcspk_set_pit_ch2(divisor);                /* 918c CALL 0x91b9 */
    }
    snd_select_scratch_83ef = divisor;             /* 9190: last divisor = divisor */
    if (snd_select_scratch_83ee == 0) {            /* 9194..919a: speaker already on? */
        snd_select_scratch_83ee = 1;               /* 919c..919e: mark on */
        port61 = (u8)(inp(0x61) | 3);              /* 91a2..91a4: gate + enable */
        outp(0x61, port61);                        /* 91a6 OUT 0x61,AL */
    }
    /* 91a8 RETF */
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
 *  CALL 9451(strobe).  record_min_status_code is PORTED (records into CS:[0x946c] via
 *  last_status_code); the validated OUT is the strobe's OUT 0x61 (sound_mode==0 capture:
 *  OUT 0x61=0xfc). */
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
 *  a multiplexer ISR (1000:7c02) that walks the 6-slot timer-SLOT table 0x549c (via the far
 *  pointer [0x54cc]) each tick accumulating a per-slot increment and, when it passes 500,
 *  FAR-CALLS the slot's installed tone-sequencer callback; and the three installed callbacks
 *  (1000:9631 / 96c4 / 95b5) the L3 schedule_timer_callback_a / _b / _c fns install (via
 *  set_timer_slot_raw + the snd_timer_cb_off/seg far ptr).  Each callback advances the
 *  10-word tone param frame snd_param_frame (0x9788..0x979a) it was handed and reprograms
 *  PIT channel 2 (ports 0x42/0x43) / strobes the PC-speaker gate (port 0x61) to sweep the
 *  tone's frequency over its lifetime, retiring the channel (disable + speaker strobe) when
 *  the duration counter (frame[0]) expires.
 *
 *  ── RECONSTRUCTION FIDELITY: reconstructed 1:1 as DOCUMENTATION, NOT runtime-gated ──
 *  Reached ONLY through the installed far pointer (the 0x549c slot table the L3 layer fills),
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
 *  0x945b = record_min_status_code (PORTED); 0x7df9 = set_timer_slot_raw (PORTED); 0x7e1f
 *  = the channel-slot clear (modelled by set_timer_slot_reg below); 0x9451 =
 *  speaker_gate_strobe (PORTED).  Stack-probe / register-save prologue omitted (convention).
 * ════════════════════════════════════════════════════════════════════════════ */

/* set_timer_slot_reg (1000:7e1f) — clear channel's 0x549c slot (value=0, cb=0:0).
 *  Validates channel 0..3, then set_timer_slot_raw(channel, 0, 0, 0) (the asm adds 2 to
 *  BX and tail-calls the 0x7e62 writer with AX=CX=DX=0).  The tone-sequencer retire path
 *  calls it with channel 2.
 *
 *  RECONSTRUCTION FIDELITY (name + linkage, Task E2): this fn was originally
 *  reconstructed here (Task A3) under the LOCAL name `isr_disable_timer_slot` with
 *  `static` linkage (only the 2 L5 tone-sequencer retire paths below called it).
 *  Ghidra's OWN canonical label for 1000:7e1f is `set_timer_slot_reg` (confirmed via
 *  get_function_by_address/decompile_function_by_address: register-entry, `in_AX` =
 *  channel, matching this fn's `channel` param exactly) — Task E2's
 *  midi_sound_init (1000:89a8) and midi_play_sequence (1000:8977) BOTH also CALL
 *  this SAME physical address (`MOV AX,0x0; CALL 0x1000:7e1f`, confirmed via raw
 *  disassembly of both callers), from src/midi.c, a DIFFERENT translation unit.
 *  Reusing the ALREADY-reconstructed body (not duplicating it) requires exposing it;
 *  renamed to match Ghidra's canonical name (avoiding a second, invented name for the
 *  same engine function) and un-`static`-ed + prototyped in sound.h. Pure rename +
 *  linkage change — no behavior differs; sound_ctest.c never referenced the old
 *  name (grep-verified), so this does not affect any already-validated differential. */
int set_timer_slot_reg(int channel)
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
    SND_PF->subsweep_current -= 1;            /* dec word cs:[0x9798] */
    if (SND_PF->subsweep_current == 0) {      /* jne 0x9672 */
        SND_PF->subsweep_current = SND_PF->subsweep_reload;             /* [0x9798] = [0x9792] */
        SND_PF->ch2_reload_current += SND_PF->ch2_reload_increment;     /* [0x9790] += [0x9794] */
        set_timer_slot_raw(2, (int)SND_PF->ch2_reload_current,    /* AX=[0x9790], BX=2     */
                           snd_timer_cb_off, snd_timer_cb_seg);  /* CX=[0x97a1], DX=[0x979f] */
    }
    SND_PF->step_current -= 1;            /* dec word cs:[0x9796] */
    if (SND_PF->step_current != 0) {      /* jne 0x96ba (exit) */
        return;
    }
    SND_PF->lifetime_count -= 1;            /* dec word cs:[0x9788] */
    if (SND_PF->lifetime_count == 0) {      /* jne 0x9691 */
        record_min_status_code(0xff);          /* MOV AX,0xff; CALL 0x945b */
        set_timer_slot_reg(2);             /* MOV AX,2;    CALL 0x7e1f */
        speaker_gate_strobe();                 /* CALL 0x9451 */
        return;                                /* jmp 0x96ba (exit) */
    }
    SND_PF->step_current = SND_PF->step_reload;      /* [0x9796] = [0x978c] */
    SND_PF->pitch_reload += SND_PF->pitch_increment; /* [0x978a] += [0x978e] */
    outp(0x43, 0xb6);                          /* MOV AL,0xb6; OUT 0x43,AL */
    (void)0;                                   /* CALL 0x9434 (noop thunk chain) */
    outp(0x42, (u8)SND_PF->pitch_reload);      /* MOV AX,[0x978a]; OUT 0x42,AL (lo) */
    (void)0;                                   /* CALL 0x9434 */
    ax_hi_lo = (u8)(SND_PF->pitch_reload >> 8);  /* XCHG AH,AL */
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
        outp(0x42, (u8)SND_PF->pitch_reload);       /* MOV AX,[0x978a]; OUT 0x42,AL (lo) */
        (void)0;                                  /* CALL 0x9434 */
        outp(0x42, (u8)(SND_PF->pitch_reload >> 8));/* XCHG AH,AL; OUT 0x42,AL (hi) */
    }
    SND_PF->subsweep_current -= 1;                    /* dec word cs:[0x9798] */
    if (SND_PF->subsweep_current == 0) {              /* jne 0x9766 */
        SND_PF->lifetime_count -= 1;               /* dec word cs:[0x9788] */
        if (SND_PF->lifetime_count == 0) {         /* jne 0x973d */
            record_min_status_code(0xff);          /* MOV AX,0xff; CALL 0x945b */
            set_timer_slot_reg(2);             /* MOV AX,2;    CALL 0x7e1f */
            speaker_gate_strobe();                 /* CALL 0x9451 */
            return;                                /* jmp 0x977e (exit) */
        }
        SND_PF->subsweep_current = SND_PF->subsweep_reload;                /* [0x9798] = [0x9792] */
        SND_PF->ch2_reload_current += SND_PF->ch2_reload_increment;               /* [0x9790] += [0x9794] */
        set_timer_slot_raw(2, (int)SND_PF->ch2_reload_current,          /* AX=[0x9790], BX=2     */
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
        outp(0x42, (u8)SND_PF->pitch_reload);       /* MOV AX,[0x978a]; OUT 0x42,AL (lo) */
        (void)0;                                  /* CALL 0x9434 */
        outp(0x42, (u8)(SND_PF->pitch_reload >> 8));/* XCHG AH,AL; OUT 0x42,AL (hi) */
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

/* ── snd_timer_slot_sweep — the per-PIT-tick core of the int-8 mux (1000:7c02), NO EOI ─────
 *  Walk the 6 slots (8 bytes each) of the 0x549c timer-slot table and, on the 500-tick
 *  (0x1f4) period, fire each slot's installed callback.  This is the faithful body of the
 *  engine's int-8 multiplexer WITHOUT the 8259 EOI, so the playable host's INT8 ISR can call
 *  it every PIT tick while keeping its own EOI / BIOS-vector-chain (see host/host_timer.c).
 *
 *  ── FIDELITY FIX (2026-07-13, verified vs raw disasm 7c02 / 7e62 / 7cde) ──
 *  The earlier reconstruction walked snd_timer_cb_table (DGROUP 0x5516) and read cb_off/cb_seg
 *  at [+4]/[+6].  That was WRONG.  The asm mux (7c02) does `lds bx,[0x54cc]` -> the 0x549c
 *  slot table (the 6-slot table install_interrupt_handler clears and the L3 schedulers /
 *  set_timer_slot_raw write), NOT 0x5516 (a separate arm_timer_callback table serviced by the
 *  0x7e80 sub-sweep, which has no reconstructed users).  The 0x549c slot layout written by
 *  set_timer_slot_raw (7e62) is {increment@0, accumulator@2, cb_seg@4, cb_off@6}, so the tone
 *  callbacks the L3 schedulers install (cb_off 0x9631/0x96c4/0x95b5) live HERE — the old mux
 *  read the wrong table entirely and would dispatch nothing.
 *
 *  Asm per tick (7c40..7c61): AX = [bx] (increment) + [bx+2] (accum); if AX >= 0x1f4 subtract
 *  0x1f4 and far-call {cb_seg=[bx+4], cb_off=[bx+6]}; store [bx+2] = AX.  The store happens
 *  DURING the loop (7c5b) and the callbacks run AFTER it, via the manufactured far-return
 *  trampoline at 0x7c87 — so a callback that re-arms its own slot (set_timer_slot_raw resets
 *  [+2]=0) wins over the loop store.  This two-pass form reproduces that ordering; the
 *  trampoline / register-save / re-entry-counter(0x54d5) plumbing is ambient ISR scaffolding,
 *  modelled as a direct call.  Dispatch is by cb_off (the reconstructed callbacks are near C
 *  fns; cb_seg is the load-base CODE segment). */
/* Cross-module: the MIDI tempo/sequence-advance callback (src/midi.c 1000:864c), installed
 *  into 0x549c timer slot 0 by midi_install_tempo_timer.  Scoped extern (not a midi.h include)
 *  to keep sound.c free of midi.h's global surface — same convention as the other leaf externs. */
extern void midi_tempo_tick(void);

void snd_timer_slot_sweep(void)
{
    u16 fired_off[6];
    int fired = 0;
    int channel;

    for (channel = 0; channel < 6; channel++) {            /* MOV CX,6; lds bx,[0x54cc] */
        u16 idx = (u16)(channel * 8);                      /* 0x549c + channel*8 */
        u16 acc = (u16)(*(u16 *)(snd_timer_slot_table + idx + 0) +   /* AX = [bx]   (increment) */
                        *(u16 *)(snd_timer_slot_table + idx + 2));   /*    + [bx+2] (accumulator)*/
        if (acc >= 0x1f4) {                                /* cmp ax,0x1f4; jl skip */
            acc = (u16)(acc - 0x1f4);                       /* SUB AX,0x1f4 */
            fired_off[fired] = *(u16 *)(snd_timer_slot_table + idx + 6);  /* cb_off @ [bx+6] */
            fired++;
        }
        *(u16 *)(snd_timer_slot_table + idx + 2) = acc;    /* MOV [bx+2],AX (store accumulator) */
    }
    /* Dispatch AFTER the accumulate loop (asm trampoline order); at most one handled slot
     *  fires per tick in practice (tone channel 2), so inter-slot order is immaterial. */
    {
        int i;
        for (i = 0; i < fired; i++) {
            u16 cb_off = fired_off[i];
            if (cb_off == 0x9631) {
                tone_seq_callback_9631();
            } else if (cb_off == 0x96c4) {
                tone_seq_callback_96c4();
            } else if (cb_off == 0x95b5) {
                tone_seq_callback_95b5();
            } else if (cb_off == 0x864c) {
                /* MIDI tempo/sequence advance (slot 0), installed by midi_install_tempo_timer.
                 *  Reconstructed 2026-07-13 (src/midi.c) — dispatching it here is what drives
                 *  audible MIDI: each tick advances the SMF and emits the due OPL/MPU events. */
                midi_tempo_tick();
            } else if (cb_off == 0x9136) {
                /* PC-speaker music voice tick (slot channel 1), installed by
                 *  select_sound_device_from_mask when snddrv_mode==0 (PC BASE).  Renders the
                 *  MIDI notes in snd_voice_table onto PIT ch2 + the speaker gate. */
                pcspk_music_render();
            }
            /* Slots 0(BIOS chain 0x7e7a) / 1(0x7e80 sub-sweep) callback offsets are not
             *  installed by the playable host, so their cb_off never appears here. */
        }
    }
}

/* ── pit_timer_isr_multiplexer (1000:7c02) — the IRQ0 / int-8 PIT tick multiplexer ─────────
 *  The full engine int-8 ISR: the per-tick slot sweep above, then the 8259 EOI (OUT 0x20,0x20)
 *  and (in the original) the manufactured far-return chain to the prior int-8 handler.  Kept as
 *  documentation of the complete asm ISR; the playable host does not call this directly — it
 *  calls snd_timer_slot_sweep() and owns its own EOI / BIOS-vector-chain cadence. */
void pit_timer_isr_multiplexer(void)
{
    snd_timer_slot_sweep();                                /* 6-slot walk + callback dispatch */
    outp(0x20, 0x20);                                      /* OUT 0x20,AL — 8259 EOI */
}
