/* ────────────────────────────────────────────────────────────────────────────
 *  midi.c — MIDI/SMF sequencer + MIDI-to-OPL voice dispatch (Phase-D/E reconstruction).
 *
 *  SKELETON (Task C1): this TU defines ONLY the MIDI module's GLOBALS — NO function
 *  bodies.  The midi_* / seq_* / opl_event_* / midi_emit_voice_msg_* call tree (full map +
 *  addresses in midi.h) remains either a game_stubs.c carve-out no-op (the 3
 *  register-entry OPL-note leaves) or simply undefined pending Phase D (load/parse/
 *  track-table pipeline) and Phase E (event-stream cursor + MIDI-to-OPL voice
 *  dispatch).  At each port the corresponding game_stubs.c stub (where one exists)
 *  is removed and the body reconstructed here.
 *
 *  Because this TU contributes no function bodies, midi.obj links cleanly alongside
 *  the game_stubs.c MIDI carve-out stubs (seq_set_channel_param / midi_emit_voice_
 *  msg_w3 / opl_event_note_on) with ZERO duplicate symbols — the same globals-only
 *  skeleton pattern Phase-5 T2 (anim.obj) / Phase-3 T2 (items.obj) / Phase-6 T2
 *  (sound.obj) used.
 *
 *  ── OWNERSHIP / no-duplicate-symbols (grep + Ghidra get_xrefs_to verified) ───────
 *    DEFINED HERE (genuinely new — a grep over the src/ C TUs finds no other def,
 *    and no other src/ TU references these addresses):
 *      midi_song_data_off/_seg    DGROUP 0x5580 / 0x5582  (song/aux far ptr halves
 *                                 midi_load_sequence 1000:87cd stages; Ghidra names
 *                                 the halves "midi_song_data_ptr" / "DAT_203b_5582")
 *      midi_aux_ptr_off/_seg      CODE   0x8485 / 0x8487  (aux far ptr halves)
 *      midi_data_seg              CODE   0x8483  (single cell; see the NAME CAVEAT
 *                                 in midi.h — Ghidra's decompile of midi_load_sequence
 *                                 shows a second, unbacked name "midi_load_flag" for
 *                                 this SAME address; get_xrefs_to confirms one writer/
 *                                 one reader, so it is ONE global here, not two)
 *      midi_division               CODE   0x85a3  (MThd division/PPQN header field)
 *      midi_tempo_lo/_hi           CODE   0x85a5 / 0x85a7  (24-bit tempo, split)
 *      midi_track_ptr_table[16][2] CODE   0x81cc..0x820c  (per-track {off,seg})
 *      midi_track_time_table[16][2] CODE  0x820c..0x824c  (per-track {time_lo,time_hi})
 *    None of these names or addresses appear in any other src/ TU (checked: sound.c,
 *    sound.h, player.c, player2.c, items.c, game.c, level.c, entity.c, globals.c,
 *    game_stubs.c) — so defining them here introduces no duplicate symbol.
 *
 *    EXTERN (owned elsewhere — NOT defined here):
 *      midi_track_count   sound.c:1533  `s16 midi_track_count;`  (CODE 0x85a1)
 *        — NAME-CLASH RESOLUTION (per the task's CRITICAL rule): sound.c's
 *        midi_track_count was added by the MPU-401 poll-timeout residual work
 *        (mpu401_write_data_polled, 1000:89e2) as a likely-misnomer write-side
 *        model.  This task confirmed via Ghidra get_xrefs_to(1000:85a1) that it is
 *        the EXACT SAME physical address the SMF sequencer's own track counter
 *        uses: readers midi_get_track_count (1000:8999, its primary getter) and
 *        midi_init_track_table (1000:87a2, its loop bound); writers midi_parse_file
 *        (1000:8846, sets tracks_remaining from the MThd header) and
 *        midi_process_event (1000:877d, decrements on end-of-track), IN ADDITION to
 *        sound.c's own mpu401_write_data_polled (1000:8a00, the poll-timeout
 *        residual).  SAME address -> owned in exactly ONE TU (sound.c, pre-
 *        existing) and reused, per the rule.  sound.h does not yet expose it (a
 *        pre-existing gap — outside this task's file list, which is midi.h/midi.c/
 *        Makefile only), so midi.h externs it directly instead of duplicating the
 *        definition or leaving it undeclared for the Phase-D body work.
 *      midi_seq_step_active   sound.h:154 (DGROUP 0x557e) — the snddrv_init_substep
 *        flag added by the timer/init task; midi.c does not redefine it.
 *      snd_seq_event_al / snd_seq_cursor / snd_seq_default_chan   sound.h:224-226 —
 *        the register-entry MIDI-cursor standins the dispatch backends already use;
 *        NOT redeclared here (pulled via `#include "sound.h"` below).
 *      opl_fnum_lo_5593 / opl_fnum_hi_559c / opl_chan_data_55b4 / opl_chan_idx_5614
 *        sound.h:183-187 — the OPL runtime tables emit_midi_voice_message (1000:8bc8)
 *        indexes (confirmed: its DI=0x5593+AX addressing matches opl_fnum_lo_5593's
 *        cited address exactly); NOT redefined here.
 *      snddrv_dispatch_a/b/c/d, opl_write_reg   sound.h — already PORTED (T4/T5);
 *        midi_process_event / midi_play_sequence / midi_sound_init /
 *        emit_midi_voice_message call these; NOT redeclared here.
 *      seq_set_channel_param / midi_emit_voice_msg_w3 / opl_event_note_on
 *        game_stubs.c:112-114 — the 3 already-carved-out register-entry OPL-note
 *        leaves (a prior task's documented boundary).  midi.h prototypes them (they
 *        are part of the midi_* / seq_* / opl_event_* call tree the brief asks this
 *        header to map) but midi.c does NOT define them this task — their no-op
 *        bodies stay in game_stubs.c until a Phase-E task ports real bodies here and
 *        removes the game_stubs.c stubs (the same stub-then-port lifecycle sound.c's
 *        own L1-L5 sound functions followed).
 *
 *  Source of truth: Ghidra BumpyDecomp decompile + raw disassembly (MCP), address-
 *  verified via get_xrefs_to on every cited word.  See midi.h for the per-symbol
 *  citations and the engine call-graph summary.
 * ──────────────────────────────────────────────────────────────────────────── */
#include "midi.h"
#include "sound.h"   /* OPL driver (opl_write_reg, snddrv_dispatch_a/b/c/d) + the
                        shared register-entry/runtime-table externs (see the
                        ownership block above) — pulled in, not re-declared. */

/* ── song/aux far pointers midi_load_sequence (1000:87cd) stages ─────────────── */
u16 midi_song_data_off;   /* DGROUP 0x5580 */
u16 midi_song_data_seg;   /* DGROUP 0x5582 */
u16 midi_aux_ptr_off;     /* CODE   0x8485 */
u16 midi_aux_ptr_seg;     /* CODE   0x8487 */
u16 midi_data_seg;        /* CODE   0x8483 — see midi.h's NAME CAVEAT */

/* ── MThd/tempo header fields ─────────────────────────────────────────────────── */
u16 midi_division;        /* CODE 0x85a3 */
u16 midi_tempo_lo;        /* CODE 0x85a5 */
u8  midi_tempo_hi;        /* CODE 0x85a7 */

/* ── per-track pointer/time tables (zero-initialised; populated by the future
 *  Phase-D midi_parse_file / midi_init_track_table ports) ──────────────────────── */
u16 midi_track_ptr_table[MIDI_MAX_TRACKS][2];    /* CODE 0x81cc..0x820c */
u16 midi_track_time_table[MIDI_MAX_TRACKS][2];   /* CODE 0x820c..0x824c */
