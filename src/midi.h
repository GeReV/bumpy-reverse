#ifndef MIDI_H
#define MIDI_H

#include "bumpy.h"

/* ── MIDI status/meta-event bytes.  Bare (unsuffixed, signed-int) literals —
 * matches every original comparison site exactly. */
#define MIDI_STATUS_SYSEX        0xf0
#define MIDI_STATUS_SYSEX_CONT   0xf7
#define MIDI_META_SET_TEMPO      0x51
#define MIDI_META_END_OF_TRACK   0x2f
#define MIDI_META_CHANNEL_PREFIX 0x20

/* ────────────────────────────────────────────────────────────────────────────
 *  midi.h — MIDI/SMF sequencer + MIDI-to-OPL voice dispatch (Phase-D/E reconstruction).
 *
 *  SKELETON (Task C1): this header declares the MIDI module's GLOBALS (the
 *  load/parse staging scalars, the tempo/division header fields, and the per-track
 *  pointer/time tables midi_parse_file / midi_init_track_table populate) plus the
 *  FULL prototype list for the midi_* / seq_* / opl_event_* / midi_emit_voice_msg_* call
 *  tree.  Phase D (load/parse/track-table pipeline) and Phase E (event-stream cursor)
 *  remain unported (mirroring sound.h's own T2 "globals-only skeleton, PORTED-labeled
 *  prototypes as bodies land" convention) — but Task D2 lands the FIRST 6 function
 *  BODIES in midi.c: the MIDI-to-OPL2 voice-message funnel opl_event_note_on (8ea3),
 *  midi_emit_voice_msg_w1/w2/w3 (8b81/8b6b/8e93), emit_midi_voice_message (8bc8), and
 *  seq_set_channel_param (922c) — the 4 leaves a prior task carved out in game_stubs.c
 *  (seq_set_channel_param / midi_emit_voice_msg_w1 / midi_emit_voice_msg_w3 /
 *  opl_event_note_on) PLUS the 2 that were never stubbed anywhere (midi_emit_voice_msg_w2 /
 *  emit_midi_voice_message, unreferenced by any already-reconstructed caller until now).
 *
 *  Because midi.c previously contributed NO function bodies, midi.obj linked cleanly
 *  alongside the game_stubs.c MIDI carve-out stubs with ZERO duplicate symbols — the
 *  same globals-only-skeleton pattern Phase-5 T2 (anim.obj) / Phase-3 T2 (items.obj)
 *  / Phase-6 T2 (sound.obj) used.  Task D2 removes those 4 game_stubs.c stubs (now
 *  duplicate symbols against midi.obj's real bodies) — see midi.c's ownership block.
 *
 *  Engine call graph (Ghidra seg 1000; addresses cited per-symbol below):
 *    Phase D  load/parse   midi_load_sequence (87cd) stages the song/aux far ptrs +
 *                           flag, calls midi_parse_file (8809) to validate the MThd/
 *                           MTrk chunk headers and fill the per-track pointer table,
 *                           then midi_init_track_table (87a2) seeds each track's
 *                           first event time via midi_read_varlen (8891) /
 *                           midi_process_event (873c).  midi_play_sequence (8977) is
 *                           the device-gated entry (falls through to midi_load_sequence);
 *                           midi_sound_init (89a8) / midi_start_playback (8722) /
 *                           midi_install_tempo_timer (86e9) do the device-select +
 *                           tempo-timer install.  midi_get_track_count (8999) is the
 *                           getter (see the midi_track_count clash note below).
 *    Phase E  event cursor midi_process_event (873c) walks a track's event stream
 *                           (register-entry DS:SI), dispatching meta/tempo/end-of-
 *                           track events and forwarding channel/SysEx/0xF7 bytes to
 *                           the ALREADY-PORTED snddrv_dispatch_b/c/d (sound.c);
 *                           seq_normalize_far_ptr (8a23) rolls SI's excess offset
 *                           into DS (pure register normalization, no global state).
 *    Phase E  voice dispatch snddrv_dispatch_d's mode0/mode1 backends (sound.c) reach
 *                           midi_emit_voice_msg_w3 (8e93) -> _w2 (8b6b) -> _w1 (8b81)
 *                           -> emit_midi_voice_message (8bc8), the shared OPL patch/
 *                           note-register writer (CALL-CHAIN DIRECTION, confirmed via
 *                           raw disasm: w3 CALLs w2, w2 CALLs w1, w1 CALLs
 *                           emit_midi_voice_message — the opposite of a superficial
 *                           name-order reading); seq_set_channel_param (922c) and
 *                           opl_event_note_on (8ea3) are the sibling program-change /
 *                           note-on leaves.  ALL SIX are RECONSTRUCTED in midi.c
 *                           (Task D2) — see the per-fn RECONSTRUCTION FIDELITY notes at
 *                           their definitions there.
 *
 *  Provenance: Ghidra BumpyDecomp decompile + raw disassembly (MCP), address-verified
 *  via get_xrefs_to on every cited word (single-writer/single-reader evidence for the
 *  ambiguous cases — see midi.c's ownership block for the full citations).
 * ──────────────────────────────────────────────────────────────────────────── */

/* ── song/aux far pointers midi_load_sequence (1000:87cd) stages ─────────────────
 *  Split per the project's _off/_seg convention.  midi_song_data_off/_seg together
 *  are Ghidra's "midi_song_data_ptr" (offset half, named) + "DAT_203b_5582" (segment
 *  half, still Ghidra-unnamed) — confirmed via get_xrefs_to: both written only at
 *  1000:87de/87e4 (midi_load_sequence); the offset half is read back at 1000:8b8a
 *  (midi_emit_voice_msg_w1, LDS SI,[0x5580] — an instrument/patch-table lookup into
 *  the loaded song data). */
extern u16 midi_song_data_off;   /* DGROUP 0x5580 — song/sequence data far ptr, offset  (Ghidra: midi_song_data_ptr) */
extern u16 midi_song_data_seg;   /* DGROUP 0x5582 — song/sequence data far ptr, segment (Ghidra: DAT_203b_5582)      */

/* aux far pointer (midi_load_sequence's 2nd param) — CODE-segment resident (like the
 * project's other CS-literal far-ptr pairs, e.g. sound.h's snd_timer_cb_off/_seg).
 * get_xrefs_to shows only the single write at 87eb/87f2 within the decompiled slice;
 * no reader found yet in the functions enumerated this task (aux_ptr's consumer is
 * presumably reached by not-yet-examined Phase-E code). */
extern u16 midi_aux_ptr_off;     /* CODE 0x8485 — aux/instrument-bank far ptr, offset  */
extern u16 midi_aux_ptr_seg;     /* CODE 0x8487 — aux/instrument-bank far ptr, segment */

/* midi_data_seg (CODE 0x8483) — NAME CAVEAT: get_xrefs_to confirms this is ONE
 * physical storage cell (single writer 1000:87d7 in midi_load_sequence, single
 * reader 1000:89a1 in midi_get_track_count's "count==0" fallback).  Ghidra's
 * decompile of midi_load_sequence ALSO surfaces a second, seemingly-distinct name
 * "midi_load_flag" for the exact same store (the value written is in fact the 3rd
 * param, an ushort "flag" — not a segment); no second address backs "midi_load_flag"
 * anywhere in the xref graph, so this is treated as a single global under the
 * cross-function-stable name ("midi_data_seg"), likely itself a carried-over
 * misnomer (same pattern as the midi_track_count/mpu401_present precedent below) —
 * documented, not renamed (out of scope for a globals-only skeleton). */
extern u16 midi_data_seg;        /* CODE 0x8483 (a.k.a. "midi_load_flag" in Ghidra's load_sequence decompile — SAME address) */

/* ── MThd/tempo header fields (parsed by midi_parse_file 1000:8809 / midi_process_event 1000:873c) ──
 *  midi_division: the MThd division/PPQN field (ticks per quarter note).
 *  midi_tempo_lo/_hi: the 24-bit "set tempo" meta-event value (FF 51 03 tt tt tt),
 *  split as the project convention prescribes; midi_install_tempo_timer (1000:86e9)
 *  reads midi_division * 0xf42 / (tempo_hi:tempo_lo) to derive the PIT reload value it
 *  hands to set_timer_slot_raw (sound.h). */
extern u16 midi_division;        /* CODE 0x85a3 — MThd division/PPQN header field */
extern u16 midi_tempo_lo;        /* CODE 0x85a5 — tempo (usec/quarter), low word   */
extern u8  midi_tempo_hi;        /* CODE 0x85a7 — tempo (usec/quarter), high byte  */

/* ── per-track pointer/time tables (midi_parse_file fills ptr; midi_init_track_table
 *  seeds time) ────────────────────────────────────────────────────────────────────
 *  Max 16 tracks (midi_parse_file rejects headers with 0 or >0x10 tracks).  Each
 *  table is CODE-segment resident (CS:[BX] stores in the disassembly); modelled as
 *  a flat 2D word array so the engine's (track_index*4)-byte stride indexes it 1:1,
 *  the same "BASED at X, raw offset arithmetic" convention sound.c's
 *  snd_timer_slot_table uses. */
#define MIDI_MAX_TRACKS 16
extern u16 midi_track_ptr_table[MIDI_MAX_TRACKS][2];   /* CODE 0x81cc..0x820c — per-track {off,seg} into its MTrk chunk */
extern u16 midi_track_time_table[MIDI_MAX_TRACKS][2];  /* CODE 0x820c..0x824c — per-track {time_lo,time_hi} (32-bit next-event clock) */

/* ── seq_set_channel_param's per-channel byte table (Task D2) ─────────────────────
 *  16 bytes, one per MIDI channel (0..15); asm 1000:922c: CS:[0x8473 + (AL&0xf)] = *SI.
 *  Sits immediately before midi_data_seg (CODE 0x8483) — confirmed via disassembly +
 *  get_xrefs_to (single writer: seq_set_channel_param; previously only a
 *  tools/midi_ctest.c harness-side shadow buffer, per the Task C2/C3 notes — now a
 *  real module global). */
#define MIDI_CHAN_PARAM_LEN 16
extern u8 chan_param_table[MIDI_CHAN_PARAM_LEN];       /* CODE 0x8473..0x8483 */

/* ── register-entry ambient standins for the midi_emit_voice_msg_w1/w2/w3 ->
 *  emit_midi_voice_message chain (Task D2) — the SAME "file-scope global stands in
 *  for a caller-supplied register" convention snd_seq_event_al/snd_seq_cursor/
 *  snd_seq_default_chan (sound.h) and snd_busy_delay's own register-args precedent
 *  (sound.c) already use; owned here (not sound.h) since these registers are specific
 *  to this call chain and no existing sound.h global models them. ── */
extern u16 midi_voice_chan;        /* engine BX at midi_emit_voice_msg_w1 entry — channel/index (w1 scales *12) */
extern u8  midi_voice_note_byte;   /* engine AH at midi_emit_voice_msg_w1 entry — forwarded UNCHANGED through w2/w3, becomes AL for emit_midi_voice_message */
extern u8  midi_emit_al;           /* engine AL at emit_midi_voice_message entry — channel/operator-slot selector byte */
extern u8 __far *midi_emit_ptr;    /* engine DS:(BX+DI) at emit_midi_voice_message entry — the 30-byte per-channel OPL patch/note descriptor; BX is always 0 at this call boundary (every reconstructed caller XORs BX,BX or seeds BX=0), so the far offset is folded into this one pointer */

/* ── EXTERN — owned elsewhere (grep + get_xrefs_to verified; NOT redefined here) ──
 *
 *  midi_track_count (sound.c ~line 1533, `s16 midi_track_count;`) is the SAME
 *  address midi_get_track_count (1000:8999) reads and midi_parse_file (8846, sets
 *  tracks_remaining) / midi_process_event (877d, end-of-track decrement) /
 *  midi_init_track_table (87a2, loop count) also touch — CONFIRMED via
 *  get_xrefs_to(1000:85a1): readers midi_get_track_count(8999) + midi_init_track_table
 *  (87a2); writers midi_parse_file(8846), midi_process_event(877d), and sound.c's
 *  mpu401_write_data_polled (1000:8a00, the MPU-poll-timeout residual write T5
 *  already reconstructed).  Per the CRITICAL name-clash rule: same address -> own in
 *  exactly ONE TU (sound.c, pre-existing) and reuse.  sound.h does not yet expose it
 *  (a pre-existing gap outside this task's file list: midi.h/midi.c/Makefile only),
 *  so it is externed directly here rather than duplicated or left undeclared. */
extern s16 midi_track_count;     /* sound.c-owned — CODE 0x85a1 (the SMF sequencer's real track count) */

/* midi_seq_step_active (DGROUP 0x557e, sound.c) and the register-entry MIDI-cursor
 * standins snd_seq_event_al / snd_seq_cursor / snd_seq_default_chan, the OPL runtime
 * tables opl_fnum_lo_5593 / opl_fnum_hi_559c / opl_chan_data_55b4 / opl_chan_idx_5614,
 * snddrv_dispatch_a/b/c/d, and opl_write_reg are ALL already declared in sound.h
 * (owned in sound.c) — midi.c pulls them via `#include "sound.h"` rather than
 * re-declaring here (avoids a redundant/conflicting declaration, same convention
 * sound.c itself uses for player.c's cross-module globals via player.h). */

/* ── MIDI/SMF-sequencer function prototypes ───────────────────────────────────────
 *  Task E1 landed the FIRST 5 SMF-parser bodies: midi_read_varlen (8891),
 *  midi_parse_file (8809), midi_init_track_table (87a2), midi_get_track_count
 *  (8999), seq_normalize_far_ptr (8a23).  Task E2 completes the module: the
 *  sequencer-driver + tempo-timer layer (midi_load_sequence 87cd,
 *  midi_start_playback 8722, midi_sound_init 89a8, midi_play_sequence 8977,
 *  midi_install_tempo_timer 86e9) PLUS the per-track event-stream cursor
 *  (midi_process_event 873c) midi_init_track_table calls conditionally — ALL SIX
 *  RECONSTRUCTED in midi.c now (its former game_stubs.c/tools/midi_ctest.c
 *  carve-out stub REMOVED).  Reconstructing midi_process_event also UNBLOCKS
 *  midi_parse_file's/midi_init_track_table's own differential (both real bodies
 *  since Task E1, now PORTED[] in tools/midi_ctest.c — see the per-fn
 *  RECONSTRUCTION FIDELITY notes in midi.c for: midi_read_varlen's packed-return
 *  correction, seq_normalize_far_ptr's real-vs-modelled-no-op correction,
 *  midi_load_sequence's song_data/aux_ptr NAME CAVEAT, and
 *  midi_install_tempo_timer's tempo-ISR carve-out).  The Phase-E MIDI-to-OPL
 *  voice/channel-dispatch block further down (opl_event_note_on /
 *  midi_emit_voice_msg_w1/w2/w3 / emit_midi_voice_message / seq_set_channel_param)
 *  is RECONSTRUCTED in midi.c (Task D2) — see the per-fn RECONSTRUCTION FIDELITY
 *  notes there. */

/* Phase D — load/parse/track-table pipeline. */
int  midi_load_sequence(void *song_data, void *aux_ptr, u16 flag);  /* 1000:87cd — RECONSTRUCTED (Task E2); genuine stack-arg cdecl16near; see the song_data/aux_ptr NAME CAVEAT at its definition; PORTED (2 records) */
int  midi_parse_file(void);                                         /* 1000:8809 — RECONSTRUCTED (Task E1); register-entry (DS:SI = file image); PORTED (Task E2, check_tbl=1; 2 records) */
void midi_init_track_table(void);                                   /* 1000:87a2 — RECONSTRUCTED (Task E1); PORTED (Task E2, check_tbl=1; 2 records) */
void midi_start_playback(void);                                     /* 1000:8722 — RECONSTRUCTED (Task E2); PORTED (2 records) */
void midi_install_tempo_timer(void);                                /* 1000:86e9 — RECONSTRUCTED (Task E2); tempo-ISR carve-out — UNPORTED-for-validation (see fidelity note at its definition + docs/reconstruction-fidelity.md) */
void midi_tempo_tick(void);                                         /* 1000:864c — RECONSTRUCTED (2026-07-13); the tempo-ISR per-tick SMF sequence advance midi_install_tempo_timer installs into 0x549c slot 0; far-called by snd_timer_slot_sweep (sound.c). Carve-out (w) lifted. */
s16  midi_get_track_count(void);                                    /* 1000:8999 — RECONSTRUCTED (Task E1); PORTED, register-return checked */
int  midi_play_sequence(void *song, void *aux_ptr, u16 flag);        /* 1000:8977 — RECONSTRUCTED (Task E2); device-gated; falls through (real tail JMP) to midi_load_sequence; PORTED but genuinely 0 records in the Task C2 capture (its OWN function-boundary hook is orphaned by the JMP — see tools/midi_ctest.c) */
void midi_sound_init(void);                                          /* 1000:89a8 — RECONSTRUCTED (Task E2); PORTED (1 record) */

/* Phase D/E — per-track event-stream cursor (register-entry: DS:SI = cursor, BX = track base). */
void seq_normalize_far_ptr(void);   /* 1000:8a23 — RECONSTRUCTED (Task E1); CORRECTION: contrary to an
                                        earlier working note (tools/midi_oracle.py's own header comment)
                                        claiming this is a bare "RET-only no-op", disassemble_function
                                        1000:8a23..8a3a shows it genuinely renormalizes DS:SI (rolls SI's
                                        excess offset into DS); reconstructed as a no-op body here ONLY
                                        because that is a value-preserving identity against THIS
                                        codebase's merged-pointer cursor model (see the fidelity note at
                                        its definition in midi.c) — not because the original function
                                        does nothing. */
u32  midi_read_varlen(void);        /* 1000:8891 — RECONSTRUCTED (Task E1); PORTED, return-value +
                                        cursor-advance checked.  CORRECTION: the task brief's own
                                        characterization of the packed return ("value in the low word,
                                        byte-count in the high word") does not match the asm/decompile —
                                        DX:AX is the FULL decoded (up to 28-bit) VLQ value for the 1/2/3
                                        -byte branches (cross-checked byte-exact against
                                        midi_track_time_table's own "32-bit next-event clock" doc
                                        comment); see the fidelity note at its definition for the 4-byte
                                        branch's bit-layout anomaly. */
u32  midi_process_event(void);      /* 1000:873c — RECONSTRUCTED (Task E2); PORTED (14 records), return
                                        value + linear DS:SI advance checked via an extra_check hook
                                        (tools/midi_ctest.c).  Dispatches meta/tempo/EOT/channel events,
                                        advances DS:SI; see the fidelity note at its definition for the
                                        signed/unsigned CMP equivalence + the raw (non-swapped) meta
                                        type/len read. */

/* Phase E — MIDI-to-OPL voice/channel dispatch (register-entry throughout; reached
 * from the ALREADY-PORTED snddrv_dispatch_b/c/d mode0/mode1 backends in sound.c).
 * RECONSTRUCTED in midi.c (Task D2); call chain w3 -> w2 -> w1 -> emit_midi_voice_message
 * (confirmed via raw disasm — see midi.c's per-fn notes). */
void midi_emit_voice_msg_w1(void);   /* 1000:8b81 — PORTED (D2) */
void midi_emit_voice_msg_w2(void);   /* 1000:8b6b — PORTED (D2) */
void midi_emit_voice_msg_w3(void);   /* 1000:8e93 — PORTED (D2); game_stubs.c stub REMOVED */
void emit_midi_voice_message(void);  /* 1000:8bc8 — PORTED (D2); shared OPL patch/note-register writer w1 funnels into */
void seq_set_channel_param(void);    /* 1000:922c — PORTED (D2); game_stubs.c stub REMOVED */
void opl_event_note_on(void);        /* 1000:8ea3 — PORTED (D2); game_stubs.c stub REMOVED */

#endif /* MIDI_H */
