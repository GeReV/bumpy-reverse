#ifndef SOUND_H
#define SOUND_H

#include "bumpy.h"

/* ────────────────────────────────────────────────────────────────────────────
 *  sound.h — sound subsystem (Phase-6 reconstruction).
 *
 *  SKELETON (Phase-6 Task 2): this header declares the sound module's GLOBALS only
 *  (the device/driver state machine scalars + the L3 tone param frame + the timer-
 *  callback / voice / per-effect data tables).  The ~30 sound FUNCTION bodies
 *  (play_sound 6e11 .. opl_play_note 905d — see tools/sound_oracle.py FN_NAMES)
 *  remain stubbed in game_stubs.c this task; their 1:1 bodies port across Phase-6
 *  T3–T6 (L1 dispatch / L2 device / L3 tone / L4 hardware drivers), validated by
 *  the host replay harness tools/sound_ctest.c against the Phase-6 T1 capture
 *  local/build/render/sound_trace.bin (magic "SNDTRC01", version 1).
 *
 *  Because sound.c contributes NO function bodies this task, sound.obj links cleanly
 *  alongside the game_stubs.c sound stubs with ZERO duplicate symbols — the same
 *  globals-only-skeleton pattern Phase-5 T2 (anim.obj) / Phase-4 T2 (player2.obj) /
 *  Phase-3 T2 (items.obj) used.
 *
 *  The five sound layers (Ghidra seg 1000) — full map in tools/sound_oracle.py:
 *    L1 dispatch  play_sound / play_sound_effect (21-case effect->tone switch),
 *                 play_action_sound / play_contact_sound / play_exit_sound /
 *                 play_pickup_sound / play_event_sound / play_state_sound.
 *    L2 device    sound_select_device, snddrv_init, select_sound_device_from_mask,
 *                 snddrv_dispatch_a/b/c/d, snd_busy_delay.
 *    L3 tone      schedule_timer_callback_a/b/c (fill the param frame +
 *                 install a far timer cb), arm/disable/restore timer slots.
 *    L4 hardware  pc_speaker_silence / speaker_gate_*, MPU-401 (0x330/0x331),
 *                 OPL/AdLib opl_write_reg/opl_play_note (0x388/0x389).
 *    L5 ISR       the installed far timer callback (no Ghidra fn boundary).
 *
 *  Provenance for every address/layout: tools/sound_oracle.py header (the frozen
 *  trace layout + resolved DGROUP/CODE offsets) + local/build/sound_model.md (the
 *  Phase-6 T1 capture), grounded in the Ghidra BumpyDecomp + raw disassembly.
 * ──────────────────────────────────────────────────────────────────────────── */

/* ── L3 tone param frame (CODE seg 1000 0x9788..0x979a) ──────────────────────────
 *  The 10-word frame the schedule_timer_callback_a/b/c fns fill before installing
 *  the far PIT timer callback.  Captured in the SND_SNAP as 9 words + 1 byte (the
 *  trailing 0x979a byte); modelled here as 10 contiguous words (the byte lives in
 *  the low half of the 10th).  Owned HERE (no other TU defines it). */
#define SND_PARAM_FRAME_WORDS 10
extern u16 snd_param_frame[SND_PARAM_FRAME_WORDS];   /* CODE 0x9788..0x979a       */

/* timer-callback far pointer the L3 schedulers install (CODE 0x979f seg / 0x97a1
 *  off — split per the project's deliberate _off/_seg convention). */
extern u16 snd_timer_cb_off;          /* CODE 0x97a1 */
extern u16 snd_timer_cb_seg;          /* CODE 0x979f */

/* ── device / driver state machine ──────────────────────────────────────────────
 *  snddrv_mode (CODE 0x85b3): backend selector for snddrv_dispatch_a
 *    (0 => pc_speaker_silence, 1 => OPL all-notes-off, 4 => MPU settle).
 *  sound_init_state (DGROUP 0x557a): 0=uninit; snddrv_init sets 1;
 *    select_sound_device_from_mask advances 1->2.
 *  sound_active_device_mask (DGROUP 0x5586): the detected-device bitmask.
 *  sound_mode (DGROUP 0x683e): speaker-gate branch selector (0 => also strobe). */
extern u16 snddrv_mode;                  /* CODE   0x85b3 */
extern u16 sound_init_state;             /* DGROUP 0x557a */
extern u16 sound_active_device_mask;     /* DGROUP 0x5586 */
extern u16 sound_mode;                   /* DGROUP 0x683e */

/* timer-callback table (DGROUP 0x5516) used by arm_timer_callback / set_timer_slot.
 *  Modelled as a raw byte table (slot layout reconstructed in the L3 port, T5). */
#define SND_TIMER_CB_TABLE_LEN 0x40
extern u8  snd_timer_cb_table[SND_TIMER_CB_TABLE_LEN];   /* DGROUP 0x5516 */

/* voice table (CODE 0x83cc) — 15 bytes cleared by pc_speaker_silence (L4). */
#define SND_VOICE_TABLE_LEN 15
extern u8  snd_voice_table[SND_VOICE_TABLE_LEN];         /* CODE 0x83cc */

/* ── EXTERN — owned elsewhere (see sound.c ownership block for grep evidence) ────
 *  sound_device_state (player.c 0x689c) — the L1 dispatch selector;
 *  the LUT-index globals p1_pending_action / p1_contact_code / tile_below_player /
 *  prev_game_mode (all player.c).  They are already declared in player.h; sound.c
 *  externs them rather than re-declaring here to avoid a redundant/conflicting
 *  declaration. */

/* ── sound function prototypes (bodies stubbed in game_stubs.c this task; the L1–L4
 *    ports land in Phase-6 T3–T6 and move here) ───────────────────────────────── */
void play_sound(u8 sound_id);            /* 1000:6e11 */
void play_action_sound(void);            /* 1000:63be */
void sound_select_device(void);          /* 1000:6de3 */

#endif /* SOUND_H */
