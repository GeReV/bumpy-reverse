#ifndef SOUND_H
#define SOUND_H

#include "bumpy.h"

/* ── L4 hardware ports — MPU-401 (0x330/0x331), OPL2/AdLib (0x388/0x389),
 * PC-speaker gate (0x61), PIT channel-2 tone (0x42 data / 0x43 command), 8259
 * PIC EOI (0x20).  Bare (unsuffixed, signed-int) literals — matches every
 * original outp()/inp() call site exactly. */
#define MPU401_DATA_PORT   0x330
#define MPU401_STATUS_PORT 0x331
#define OPL_ADDR_PORT      0x388   /* also status on IN */
#define OPL_DATA_PORT      0x389
#define PC_SPEAKER_PORT    0x61
/* SND_ prefix: host_timer.c (different PIT channel/purpose) already defines
 * its own PIT_CMD_PORT/PIT_DATA_PORT for channel 0; these are channel-2 tone
 * generation ports, kept distinctly named to avoid the macro collision. */
#define SND_PIT_DATA_PORT  0x42
#define SND_PIT_CMD_PORT   0x43
#define PIC_EOI_PORT       0x20

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

/* timer-SLOT table (DGROUP 0x549c) used by set_timer_slot_raw / get_timer_slot_field.
 *  The engine indexes it as (channel+2)*8 + 0x549c; the host model is BASED at 0x549c so
 *  the same offset arithmetic indexes it 1:1.  Sized to cover channels 0..3
 *  ((3+2)*8+8 = 0x30) — round up to 0x40.  (T4) */
#define SND_TIMER_SLOT_TABLE_LEN 0x40
extern u8  snd_timer_slot_table[SND_TIMER_SLOT_TABLE_LEN];   /* DGROUP 0x549c */

/* L2 device-state extras (T4) — not captured in the SND_SNAP (so not in the semantic
 *  differential); owned here for the faithful bodies + the link. */
extern u8  snd_init_substep_5584;     /* DGROUP 0x5584 — snddrv_init substep flag */
extern u8  snd_select_scratch_83ee;   /* CODE   0x83ee — select-from-mask reset scratch */
extern u16 snd_select_scratch_83ef;   /* CODE   0x83ef — select-from-mask reset scratch */

/* L1 event-wrapper LUTs (DGROUP) — exact image bytes; defined in sound.c (T4). */
extern u8 action_sound_lut_opl_260e[0x100];   /* DGROUP 0x260e */
extern u8 action_sound_lut_std_263e[0x100];   /* DGROUP 0x263e */
extern u8 state_sound_lut_opl_26ce[0x100];    /* DGROUP 0x26ce */
extern u8 state_sound_lut_std_26fe[0x100];    /* DGROUP 0x26fe */
extern u8 contact_sound_lut_opl_276e[0x100];  /* DGROUP 0x276e */
extern u8 contact_sound_lut_std_278e[0x100];  /* DGROUP 0x278e */

/* ── EXTERN — owned elsewhere (see sound.c ownership block for grep evidence) ────
 *  sound_device_state (player.c 0x689c) — the L1 dispatch selector;
 *  the LUT-index globals p1_pending_action / p1_contact_code / tile_below_player /
 *  prev_game_mode (all player.c).  They are already declared in player.h; sound.c
 *  externs them rather than re-declaring here to avoid a redundant/conflicting
 *  declaration. */

/* ── sound function prototypes ───────────────────────────────────────────────────
 *  PORTED (Phase-6 T3 — effect→frame pipeline): play_sound + play_sound_effect
 *  (L1 dispatch) and schedule_timer_callback_a/b/c (L3 tone-submit) have 1:1 bodies
 *  in sound.c.  The rest are still stubbed in game_stubs.c; their L2/L4/L5 ports land
 *  in Phase-6 T4–T6 and move here. */
void play_sound(u8 sound_id);            /* 1000:6e11 — PORTED (T3) */
void play_sound_effect(u8 effect_id);    /* 1000:6e30 — PORTED (T3) */
u16  schedule_timer_callback_a(u16 param_1, u16 param_2, u16 param_3, u16 param_4,
                               u16 param_5, u16 param_6, u16 param_7, u16 param_8);
                                         /* 1000:9488 — PORTED (T3) */
u16  schedule_timer_callback_b(u16 param_1, u16 param_2, u16 param_3, u16 param_4,
                               u16 param_5, u16 param_6);
                                         /* 1000:9502 — PORTED (T3) */
u16  schedule_timer_callback_c(u16 param_1, u16 param_2);
                                         /* 1000:956d — PORTED (T3) */
/* PORTED (Phase-6 T4 — L1 event wrappers): each reads a per-device LUT -> sound id ->
 *  play_sound.  PORTED in sound.c. */
void play_action_sound(void);            /* 1000:63be — PORTED (T4) */
void play_contact_sound(void);           /* 1000:640c — PORTED (T4) */
void play_exit_sound(void);              /* 1000:6305 — PORTED (T4) */
void play_pickup_sound(void);            /* 1000:645d — PORTED (T4) */
void play_event_sound_64c1(void);        /* 1000:64c1 — PORTED (T4) */
void play_state_sound_647e(void);        /* 1000:647e — PORTED (T4); name suffix corrected 2026-07-14
                                             (was play_state_sound_79b9 — 0x79b9 is inside the
                                             unrelated install_keyboard_isr, not this function). */

/* PORTED (Phase-6 T4 — L2 device state machine). */
void sound_select_device(void);                  /* 1000:6de3 — PORTED (T4) */
u16  snddrv_init(void);                           /* 1000:88e5 — PORTED (T4) */
int  select_sound_device_from_mask(u16 mask);     /* 1000:891e — PORTED (T4) */
void snddrv_dispatch_a(void);                     /* 1000:85b5 — PORTED (T4) */
u16  snddrv_dispatch_b(void);                     /* 1000:85db — PORTED (T4) */
void snddrv_dispatch_c(void);                     /* 1000:8600 — PORTED (T4) */
void snddrv_dispatch_d(void);                     /* 1000:8626 — PORTED (T4) */
void snd_busy_delay(u16 count);                   /* 1000:872e — PORTED (T4) */

/* PORTED (Phase-6 T4 — L3 timer-table management). */
int  set_timer_slot_raw(int channel, int value, u16 cb_off, u16 cb_seg);  /* 1000:7df9 */
int  set_timer_slot(int channel, int value, u16 cb_off, u16 cb_seg);      /* 1000:7de8 */
int  arm_timer_callback(int channel, int reload, u16 cb_off, u16 cb_seg); /* 1000:7f2b */
int  disable_timer_callback(int channel);         /* 1000:7f65 — PORTED (T4) */
int  get_timer_slot_field(int slot_index);        /* 1000:7e3d — PORTED (T4) */
void timer_restore(void);                         /* 1000:7fde — PORTED (T4) */
int  set_timer_slot_reg(int channel);             /* 1000:7e1f — PORTED (Task A3; renamed
                                                        from the local `isr_disable_timer_slot`
                                                        + exposed non-static this task so
                                                        src/midi.c's midi_sound_init/
                                                        midi_play_sequence can reuse the SAME
                                                        reconstructed body — see the
                                                        RECONSTRUCTION FIDELITY note at its
                                                        definition in sound.c) */

/* ── PORTED (Task A3 — MPU reset / init substep / timer teardown / status latch) ──────
 *  Finishes the last 4 stubbed leaves of the sound-effect pipeline.  See the per-fn
 *  RECONSTRUCTION FIDELITY notes at each definition in sound.c. */
void record_min_status_code(u16 status);          /* 1000:945b — PORTED (A3) */
int  mpu401_reset_to_uart(void);                  /* 1000:8a75 — PORTED (A3) */
u16  snddrv_init_substep(void);                   /* 1000:8b2a — PORTED (A3) */
void timer_teardown_restore(void);                /* 1000:7fef — PORTED (A3) */

/* Task A3 state (owned in sound.c). */
extern u16 last_status_code;            /* CODE   0x946c — record_min_status_code latch */
extern u16 midi_seq_step_active;        /* DGROUP 0x557e — snddrv_init_substep flag      */
extern u8  isr_installed_flag;          /* DGROUP 0x54d4 — timer_teardown_restore guard  */
extern u16 saved_timer_vector_off;      /* DGROUP 0x54d8 — saved INT 0Fh vector offset   */
extern u16 saved_timer_vector_seg;      /* DGROUP 0x54da — saved INT 0Fh vector segment  */
extern u16 timer_restore_reload_value;  /* DGROUP 0x54dc — table[index] staged for pit_set_counter0 */
extern u16 timer_restore_table[8];      /* DGROUP 0x54de.. — per-index restore table (out-of-scope extent) */
/* register-entry standins timer_teardown_restore reads (see its FIDELITY note). */
extern u16 snd_isr_restore_index;        /* engine AX */
extern u16 snd_isr_restore_off;          /* engine CX */
extern u16 snd_isr_restore_seg;          /* engine DX */

/* ── PORTED (Phase-6 T5 — L4 hardware backends; the port-write drivers) ──────────────
 *  These issue the engine's real OUT/IN to 0x61 / 0x330-0x331 / 0x388-0x389.  Validated
 *  by the port-write-sequence differential (tools/sound_ctest.c comparator B) where the
 *  OUT sequence is deterministic or recoverable from the record; opl_play_note (905d) +
 *  its driver opl2_all_notes_off (8e2f) read RUNTIME freq tables not in the SND_SNAP ->
 *  documented port-gate exclusion (ported 1:1, registered UNPORTED). */
void pc_speaker_silence(void);                    /* 1000:9115 — PORTED (T5) */
void speaker_gate_reset(void);                    /* 1000:9440 — PORTED (T5) */
void speaker_gate_strobe(void);                   /* 1000:9451 — PORTED (T5) */
void record_status_and_strobe_speaker(void);      /* 1000:946e — PORTED (T5) */
void mpu401_write_data_polled(void);               /* 1000:89e2 — PORTED (T5) MPU byte-out */
void snd_emit_raw_sample(u8 sample_lo, u8 sample_hi); /* 1000:8a07 — PORTED (T5) MPU sample  */
void mpu401_settle_delay(void);                    /* 1000:8ad0 — PORTED (T5) MPU settle  */
void opl2_all_notes_off(void);                     /* 1000:8e2f — PORTED (T5) OPL all-off (excl) */
void opl_write_reg(u8 reg, u8 val);               /* 1000:9007 — PORTED (T5) OPL reg write */
void opl_play_note(u8 param_1, u8 param_2, u16 param_3, u16 param_4); /* 1000:905d (excl) */

/* ── PORTED (Task D1 — OPL2 register-level driver leaves) ────────────────────────────
 *  See the per-fn RECONSTRUCTION FIDELITY notes at each definition in sound.c. */
u8   opl_read_status(void);                        /* 1000:9056 — PORTED (D1) OPL2 status IN */
void opl2_reset_all_regs(void);                    /* 1000:8eeb — PORTED (D1) OPL2 reg init  */
void maybe_opl2_detect_chip(void);                 /* 1000:8fb6 — PORTED (D1) OPL2 chip-detect (ZF-only; see snd_opl_detect_zf's caller, snddrv_init_substep) */
void opl_set_note_params(u16 chan, u8 note_param1, u8 note_param2); /* 1000:9241 — PORTED (D1) */

/* L4 hardware-backend state (owned in sound.c). */
extern u8 opl_reg_shadow_80cc[0x100];   /* CODE   0x80cc — OPL register write-back shadow */
extern u8 opl_fnum_lo_5593[0x100];      /* DGROUP 0x5593 — per-note F-number low (runtime) */
extern u8 opl_fnum_hi_559c[0x100];      /* DGROUP 0x559c — per-note F-number word (runtime) */
extern u8 opl_chan_data_55b4[0x100];    /* DGROUP 0x55b4 — per-channel data (runtime)       */
extern u8 opl_chan_idx_5614[0x100];     /* DGROUP 0x5614 — per-channel block index (runtime) */
extern u8 snd_mpu_byte_89e2;            /* the byte mpu401_write_data_polled writes (engine AH; host-staged) */
extern u8 opl_note_param1;              /* CODE   0x9272 — opl_set_note_params' staged note byte 1 */
extern u8 opl_note_param2;              /* CODE   0x9273 — opl_set_note_params' staged note byte 2 */

/* ── PORTED (Phase-6 T6 — L5 ISR tone-sequencer; reconstructed 1:1, NOT runtime-gated) ──
 *  The PIT (IRQ0 / int-8) timer ISR multiplexer + the far tone-sequencer callbacks it
 *  fires once per tick.  These are reached only through the installed far pointer (the
 *  0x5516 cb table the L3 arm_timer_callback / schedule_timer_callback_* fns populate),
 *  so the Ghidra database has NO function boundary for them and the sound oracle does NOT
 *  hook them — there are no trace records, hence no host differential.  Ported here from
 *  the raw disassembly as DOCUMENTATION of the engine's async per-tick frequency sweep;
 *  the self-modifying-graphics-overlay-blitter precedent (behavior-faithful, not runtime-gated).  See
 *  the RECONSTRUCTION FIDELITY block at the L5 section in sound.c +
 *  docs/reconstruction-fidelity.md. */
void snd_timer_slot_sweep(void);        /* 1000:7c02 core (no EOI) — playable host ISR calls this */
void pit_timer_isr_multiplexer(void);   /* 1000:7c02 — IRQ0/int-8 PIT tick mux           */
void tone_seq_callback_9631(void);      /* 1000:9631 — sweep tone sequencer (sched_a cb)  */
void tone_seq_callback_96c4(void);      /* 1000:96c4 — noise/PRNG tone sequencer (sched_b)*/
void tone_seq_callback_95b5(void);      /* 1000:95b5 — noise/PRNG tone sequencer (sched_c)*/

/* ── PORTED — the 9 snddrv_dispatch_b/c/d MIDI mode{0,1,4} backends ──────────────────
 *  Reconstructed 1:1 from the raw disassembly (their caller, midi_process_event, is
 *  register-entry and NOT reconstructed — separate, not-yet-started MIDI-engine work).
 *  Register-entry (see the RECONSTRUCTION FIDELITY note at their definitions in
 *  sound.c): NOT oracle-exercised / NOT runtime-gated, the same documented-exclusion
 *  precedent as opl_play_note (905d) / opl2_all_notes_off (8e2f) and the L5 ISR sequencer
 *  above. */
void snddrv_dispatch_b_mode0(void);   /* 1000:91cf — MIDI 0xF7 skip           */
void snddrv_dispatch_b_mode1(void);   /* 1000:8e48 — MIDI 0xF7 skip           */
void snddrv_dispatch_c_mode0(void);   /* 1000:91d7 — MIDI 0xF0 skip           */
void snddrv_dispatch_c_mode1(void);   /* 1000:8e50 — MIDI 0xF0 skip           */
void snddrv_dispatch_b_mode4(void);   /* 1000:8af6 — MIDI 0xF7 busy-wait      */
void snddrv_dispatch_c_mode4(void);   /* 1000:8b04 — MIDI 0xF0 busy-wait      */
void snddrv_dispatch_d_mode4(void);   /* 1000:8b0d — channel-msg busy-wait    */
void snddrv_dispatch_d_mode0(void);   /* 1000:91df — channel-msg (PC-speaker) */
void snddrv_dispatch_d_mode1(void);   /* 1000:8e58 — channel-msg (OPL)        */

/* Register-entry standins for the 9 backends above (the ambient AL/DS:SI/CS:[BX+0x80]
 *  midi_process_event would supply — see the RECONSTRUCTION FIDELITY note in sound.c). */
extern u8  snd_seq_event_al;       /* engine AL    — the MIDI event status/data byte  */
extern u8 *snd_seq_cursor;         /* engine DS:SI — the live MIDI-track read cursor  */
extern u8  snd_seq_default_chan;   /* engine CS:[BX+0x80] — per-track default channel */

#endif /* SOUND_H */
