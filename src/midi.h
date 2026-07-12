#ifndef MIDI_H
#define MIDI_H

#include "bumpy.h"

/* ────────────────────────────────────────────────────────────────────────────
 *  midi.h — MIDI/SMF sequencer + MIDI-to-OPL voice dispatch (Phase-D/E reconstruction).
 *
 *  SKELETON (Task C1): this header declares the MIDI module's GLOBALS only (the
 *  load/parse staging scalars, the tempo/division header fields, and the per-track
 *  pointer/time tables midi_parse_file / midi_init_track_table populate) plus the
 *  FULL prototype list for the midi_* / seq_* / opl_event_* / midi_emit_voice_msg_* call
 *  tree.  No function BODIES land here this task — they port across Phase D (load/
 *  parse/track-table pipeline) and Phase E (event-stream cursor + MIDI-to-OPL voice
 *  dispatch), mirroring sound.h's own T2 "globals-only skeleton, PORTED-labeled
 *  prototypes as bodies land" convention.
 *
 *  Because midi.c contributes NO function bodies this task, midi.obj links cleanly
 *  alongside the game_stubs.c MIDI carve-out stubs (seq_set_channel_param /
 *  midi_emit_voice_msg_w3 / opl_event_note_on) with ZERO duplicate symbols — the
 *  same globals-only-skeleton pattern Phase-5 T2 (anim.obj) / Phase-3 T2 (items.obj)
 *  / Phase-6 T2 (sound.obj) used.
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
 *                           midi_emit_voice_msg_w1 (8b81) -> _w2 (8b6b) -> _w3 (8e93)
 *                           -> emit_midi_voice_message (8bc8), the shared OPL patch/
 *                           note-register writer; seq_set_channel_param (922c) and
 *                           opl_event_note_on (8ea3) are the sibling program-change /
 *                           note-on leaves.  midi_emit_voice_msg_w3 / seq_set_channel_
 *                           param / opl_event_note_on are CURRENTLY carve-out no-op
 *                           stubs in game_stubs.c (a prior task's documented boundary);
 *                           prototyped here for the call-tree map, NOT claimed by
 *                           midi.c this task (their stubs stay in game_stubs.c until a
 *                           Phase-E task ports real bodies and removes them there).
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

/* ── MIDI/SMF-sequencer function prototypes (Phase D/E — NOT YET PORTED) ──────────
 *  All bodies are still either a game_stubs.c carve-out no-op (the 3 register-entry
 *  OPL-note leaves, noted per-symbol below) or simply UNDEFINED pending Phase-D/E
 *  reconstruction — nothing in the currently-reconstructed call graph reaches the
 *  rest yet, so game_stubs.c carries no stub for them.  Declared now so the full,
 *  address-cited call-tree map lives in one place ahead of the Phase-D/E body work. */

/* Phase D — load/parse/track-table pipeline. */
int  midi_load_sequence(void *song_data, void *aux_ptr, u16 flag);  /* 1000:87cd */
int  midi_parse_file(void);                                         /* 1000:8809 — register-entry (DS:SI = file image) */
void midi_init_track_table(void);                                   /* 1000:87a2 */
void midi_start_playback(void);                                     /* 1000:8722 */
void midi_install_tempo_timer(void);                                /* 1000:86e9 */
int  midi_get_track_count(void);                                    /* 1000:8999 */
int  midi_play_sequence(void *song, void *aux_ptr, u16 flag);        /* 1000:8977 — device-gated; falls through to midi_load_sequence */
void midi_sound_init(void);                                          /* 1000:89a8 */

/* Phase D/E — per-track event-stream cursor (register-entry: DS:SI = cursor, BX = track base). */
void seq_normalize_far_ptr(void);   /* 1000:8a23 — rolls SI's excess offset into DS (pure register op, no globals) */
u32  midi_read_varlen(void);        /* 1000:8891 — decode a 7-bits/byte variable-length quantity at DS:SI */
void midi_process_event(void);     /* 1000:873c — dispatch meta/tempo/EOT/channel events, advance DS:SI */

/* Phase E — MIDI-to-OPL voice/channel dispatch (register-entry throughout; reached
 * from the ALREADY-PORTED snddrv_dispatch_b/c/d mode0/mode1 backends in sound.c). */
void midi_emit_voice_msg_w1(void);   /* 1000:8b81 */
void midi_emit_voice_msg_w2(void);   /* 1000:8b6b */
void midi_emit_voice_msg_w3(void);   /* 1000:8e93 — CARVE-OUT no-op stub currently in game_stubs.c */
void emit_midi_voice_message(void);  /* 1000:8bc8 — shared OPL patch/note-register writer w1->w2->w3 funnel into */
void seq_set_channel_param(void);    /* 1000:922c — CARVE-OUT no-op stub currently in game_stubs.c */
void opl_event_note_on(void);        /* 1000:8ea3 — CARVE-OUT no-op stub currently in game_stubs.c */

#endif /* MIDI_H */
