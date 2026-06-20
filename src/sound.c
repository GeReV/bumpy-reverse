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
