/* ────────────────────────────────────────────────────────────────────────────
 *  midi.c — MIDI/SMF sequencer + MIDI-to-OPL voice dispatch (Phase-D/E reconstruction).
 *
 *  SKELETON (Task C1) + Task D2 body landing: this TU originally defined ONLY the MIDI
 *  module's GLOBALS — NO function bodies.  Task D2 lands the FIRST 6 function bodies —
 *  the MIDI-to-OPL2 voice-message bridge: opl_event_note_on (8ea3), midi_emit_voice_
 *  msg_w1/w2/w3 (8b81/8b6b/8e93), emit_midi_voice_message (8bc8), and
 *  seq_set_channel_param (922c).  The rest of the midi_* / seq_* call tree (full map +
 *  addresses in midi.h) remains simply undefined pending Phase D (load/parse/
 *  track-table pipeline) and Phase E (event-stream cursor).
 *
 *  Task D2 REMOVES the game_stubs.c carve-out stubs for 4 of these 6 (seq_set_channel_
 *  param / midi_emit_voice_msg_w1 / midi_emit_voice_msg_w3 / opl_event_note_on — a prior
 *  task's documented boundary) since midi.obj now supplies their REAL bodies (keeping
 *  the old stubs would be duplicate symbols).  midi_emit_voice_msg_w2 and
 *  emit_midi_voice_message were NEVER stubbed anywhere (unreferenced by any
 *  already-reconstructed caller until this task's own w1/w3 bodies call them) — no
 *  stub to remove for those two.  tools/sound_ctest.c KEEPS its own 4 host no-op stubs
 *  unchanged (it #includes ONLY src/sound.c, whose compiled body still references all 4
 *  directly — seq_set_channel_param / midi_emit_voice_msg_w3 / opl_event_note_on from
 *  the snddrv_dispatch_d_mode0/mode1 backends, midi_emit_voice_msg_w1 from
 *  opl_set_note_params); tools/midi_ctest.c (which #includes BOTH sound.c AND midi.c)
 *  REMOVES its matching 4 stubs (now duplicate symbols against midi.obj's bodies) and
 *  gains PORTED[] entries for all 6 — see midi_ctest.c for the full wiring.
 *
 *  Prior to this task, midi.obj linked cleanly alongside the game_stubs.c MIDI
 *  carve-out stubs with ZERO duplicate symbols — the same globals-only skeleton
 *  pattern Phase-5 T2 (anim.obj) / Phase-3 T2 (items.obj) / Phase-6 T2 (sound.obj)
 *  used; this task is the first to un-stub any of the 6.
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
 *      chan_param_table[16]       CODE   0x8473..0x8483  (Task D2 — seq_set_channel_
 *                                 param's per-channel byte table; previously only a
 *                                 tools/midi_ctest.c harness-side shadow, per the C2/C3
 *                                 notes — grep-verified no other src/ TU touches 0x8473)
 *      midi_voice_chan / midi_voice_note_byte / midi_emit_al / midi_emit_ptr  (Task D2
 *                                 — register-entry ambient standins the
 *                                 midi_emit_voice_msg_w1/w2/w3 -> emit_midi_voice_message
 *                                 chain uses; new, no existing sound.h global models
 *                                 these registers)
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
 *      (Task D2 UPDATE: seq_set_channel_param / midi_emit_voice_msg_w1 / _w3 /
 *        opl_event_note_on were formerly game_stubs.c carve-out stubs — ALL SIX of
 *        opl_event_note_on / midi_emit_voice_msg_w1/w2/w3 / emit_midi_voice_message /
 *        seq_set_channel_param are now DEFINED HERE (real bodies); the 4 that had
 *        game_stubs.c stubs have had those stubs REMOVED — see the ownership note at
 *        the top of this file and game_stubs.c's own updated comment block.)
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

/* ── seq_set_channel_param's per-channel byte table (Task D2; see midi.h) ────────── */
u8 chan_param_table[MIDI_CHAN_PARAM_LEN];        /* CODE 0x8473..0x8483 */

/* ── register-entry ambient standins for the w1/w2/w3 -> emit_midi_voice_message
 *  chain (Task D2; see midi.h for the full convention note). ──────────────────────── */
u16 midi_voice_chan;        /* engine BX at midi_emit_voice_msg_w1 entry */
u8  midi_voice_note_byte;   /* engine AH at midi_emit_voice_msg_w1 entry */
u8  midi_emit_al;           /* engine AL at emit_midi_voice_message entry */
u8 __far *midi_emit_ptr;    /* engine DS:(BX+DI) at emit_midi_voice_message entry */

/* ════════════════════════════════════════════════════════════════════════════
 *  MIDI-to-OPL2 voice-message bridge (Task D2) — opl_event_note_on / seq_set_
 *  channel_param / midi_emit_voice_msg_w1/w2/w3 / emit_midi_voice_message.
 *
 *  Reached from snddrv_dispatch_d_mode0/mode1 (sound.c, already PORTED): mode0's
 *  0xC0 (program change) -> seq_set_channel_param; mode1's 0xC0 -> midi_emit_voice_
 *  msg_w3 -> _w2 -> _w1 -> emit_midi_voice_message (CALL-CHAIN CONFIRMED via raw
 *  disasm: w3 CALLs w2 at 1000:8e99, w2 CALLs w1 at 1000:8b76, w1 CALLs
 *  emit_midi_voice_message at 1000:8bc0 — the REVERSE of a superficial name-order
 *  reading); mode1's 0x90 (note-on) -> opl_event_note_on -> opl_play_note (already
 *  PORTED, sound.c).  opl_set_note_params (9241, already PORTED, sound.c) ALSO calls
 *  midi_emit_voice_msg_w1 directly (BX=chan, AH=1) and opl_event_note_on directly
 *  (AL=1, DS:SI->its own 0x9272/0x9273 scratch bytes) — that call site is UNCHANGED by
 *  this task (opl_set_note_params has no xrefs — provably unreachable in the current
 *  call graph — so its register wiring gap, documented at its own definition in
 *  sound.c, remains an accepted, harmless gap).
 *
 *  ── RECONSTRUCTION FIDELITY (register-entry, all six) ────────────────────────────
 *  None of the six takes stack args in the real asm — every input arrives in a CPU
 *  register (or via DS:SI) with no true reconstructed caller supplying it as a normal
 *  C argument (the real callers are either register-entry themselves, forwarding
 *  registers untouched, or are register-entry stand-ins the harness seeds directly).
 *  Modelled with the SAME "file-scope global stands in for a caller-supplied register"
 *  convention already established in this codebase: snd_seq_event_al / snd_seq_cursor
 *  / snd_seq_default_chan (sound.h, the 9 snddrv_dispatch_b/c/d backends) and
 *  snd_busy_delay's own register-args precedent (sound.c, "RECONSTRUCTION FIDELITY:
 *  register-args asm routine"). opl_event_note_on and seq_set_channel_param reuse the
 *  EXISTING snd_seq_event_al (AL) / snd_seq_cursor (DS:SI) globals directly (their
 *  register roles line up exactly with what those globals already model for the sound.c
 *  dispatch backends that call them). midi_emit_voice_msg_w1's BX/AH and
 *  emit_midi_voice_message's AL/DS:(BX+DI) have no existing sound.h stand-in, so Task D2
 *  adds midi_voice_chan / midi_voice_note_byte / midi_emit_al / midi_emit_ptr (all
 *  midi.h-declared, defined above).
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── opl_event_note_on (1000:8ea3) — MIDI note-on -> OPL trigger ─────────────────
 *  Register-entry: AL = the channel nibble (snd_seq_event_al & 0xf); DS:SI =
 *  snd_seq_cursor, the live MIDI-track cursor. Reads two stream bytes via LODSB (note,
 *  velocity), computes key_on_flag = velocity ? 0x20 : 0, and tail-calls
 *  opl_play_note(key_on_flag, velocity, note, chan) — the exact push/CALL/ADD-SP-8
 *  order confirms this argument order (cdecl: last-pushed = first C arg). asm
 *  1000:8ea3 verbatim: AND AL,0xf; XOR AH,AH; PUSH AX(chan); LODSB; PUSH AX(note);
 *  LODSB; PUSH AX(velocity); AND AX,AX; JZ +; MOV AX,0x20; PUSH AX(key_on_flag);
 *  CALL 905d; ADD SP,8. */
void opl_event_note_on(void)
{
    u8 chan, note, vel, key_on_flag;

    chan = (u8)(snd_seq_event_al & 0xf);
    note = *snd_seq_cursor; snd_seq_cursor++;          /* LODSB — note number */
    vel  = *snd_seq_cursor; snd_seq_cursor++;          /* LODSB — velocity    */
    key_on_flag = (u8)((vel != 0) ? 0x20 : 0);

    opl_play_note(key_on_flag, vel, note, chan);
}

/* ── seq_set_channel_param (1000:922c) — OPL/PC-speaker program-change store ─────
 *  Register-entry: AL = channel nibble, DS:SI = 1 byte to store. Writes
 *  chan_param_table[chan] = *SI, advances SI. asm 1000:922c verbatim: AND AL,0xf;
 *  MOV AH,AL; MOV AL,[SI]; MOV BX,0x8473; ADD BL,AH; ADC BH,0; MOV CS:[BX],AL;
 *  INC SI. */
void seq_set_channel_param(void)
{
    u8 chan = (u8)(snd_seq_event_al & 0xf);
    u8 val  = *snd_seq_cursor;
    snd_seq_cursor++;                                  /* the asm's own INC SI */

    chan_param_table[chan] = val;
}

/* ── midi_emit_voice_msg_w3 (1000:8e93) — channel-msg program-change entry ───────
 *  Register-entry: AL = the raw event byte (channel low nibble; snd_seq_event_al),
 *  DS:SI = snd_seq_cursor (1 stream byte, read but NOT auto-incremented by the read
 *  itself — the asm's INC SI happens AFTER the nested call returns). Forwards
 *  chan into AH (-> w2's passthrough -> w1) and the stream byte into AL (-> w2's own
 *  BX-source input), then calls w2. asm 1000:8e93 verbatim: AND AL,0xf; MOV AH,AL;
 *  MOV AL,[SI]; CALL 8b6b; INC SI. */
void midi_emit_voice_msg_w3(void)
{
    u8 chan = (u8)(snd_seq_event_al & 0xf);            /* AND AL,0xf            */
    u8 val  = *snd_seq_cursor;                          /* MOV AL,[SI] (no incr yet) */

    midi_voice_note_byte = chan;                        /* MOV AH,AL — forwarded thru w2 unchanged into w1 */
    snd_seq_event_al = val;                              /* AL := stream byte, w2's own input */
    midi_emit_voice_msg_w2();                            /* CALL 0x8b6b */
    snd_seq_cursor++;                                     /* INC SI (post-call, per asm order) */
}

/* ── midi_emit_voice_msg_w2 (1000:8b6b) — channel/index -> BX relay ──────────────
 *  Register-entry: AL = the value that becomes w1's BX (zero-extended); AH passes
 *  through UNTOUCHED (w1 reads it via midi_voice_note_byte, which THIS fn does not
 *  modify). asm 1000:8b6b verbatim: MOV BL,AL; XOR BH,BH; CALL 8b81. */
void midi_emit_voice_msg_w2(void)
{
    midi_voice_chan = (u16)(u8)snd_seq_event_al;        /* MOV BL,AL; XOR BH,BH */
    midi_emit_voice_msg_w1();                            /* CALL 0x8b81 (AH passthrough, untouched) */
}

/* ── midi_emit_voice_msg_w1 (1000:8b81) — per-channel patch-descriptor lookup ─────
 *  Register-entry: BX = midi_voice_chan (channel/index, *12-scaled below); AH =
 *  midi_voice_note_byte (forwarded as AL into emit_midi_voice_message). Walks the
 *  song-data blob (DS:SI = midi_song_data_seg:midi_song_data_off, loaded via LDS from
 *  DGROUP 0x5580/0x5582): DI = SI + word@(SI+0xc) + chan*12 indexes a per-channel
 *  program-slot table, whose word value (idx) is then scaled *30 and added to
 *  SI + word@(SI+0x10) to reach the channel's 30-byte OPL patch descriptor. BX is
 *  zeroed before the tail call (folded into midi_emit_ptr here). asm 1000:8b81
 *  verbatim: MOV SI,0x203b; MOV DS,SI; LDS SI,[0x5580]; MOV DI,[SI+0xc]; ADD DI,SI;
 *  <BX*=12>; ADD DI,BX; MOV BX,[DI]; <BX*=30>; MOV DI,[SI+0x10]; ADD DI,SI; ADD DI,BX;
 *  XOR BX,BX; MOV AL,AH; XOR AH,AH; CALL 8bc8.
 *
 *  RECONSTRUCTION FIDELITY (constant-multiply simplification): the asm computes
 *  chan*12 and idx*30 via ADD/SHL/SUB sequences on a 16-bit register (no MUL); this is
 *  transcribed as direct `* 12` / `* 30` — an exact, standard equivalent (16-bit
 *  wraparound truncated once here matches the same truncation applied at each asm
 *  step, by the distributivity of modular arithmetic over doubling/subtraction), not a
 *  behavior change — the same simplification opl_play_note's own reconstruction
 *  already uses for its shift-derived table indices (sound.c). */
void midi_emit_voice_msg_w1(void)
{
    u16 chan = midi_voice_chan;                          /* incoming BX */
    u16 tbl_off;
    u16 di;
    u16 idx;

    tbl_off = *(u16 __far *)MK_FP(midi_song_data_seg,
                                  (u16)(midi_song_data_off + 0x0c));
    di  = (u16)(midi_song_data_off + tbl_off + chan * 12);    /* per-channel program slot */
    idx = *(u16 __far *)MK_FP(midi_song_data_seg, di);        /* that channel's patch/voice index */

    tbl_off = *(u16 __far *)MK_FP(midi_song_data_seg,
                                  (u16)(midi_song_data_off + 0x10));
    di  = (u16)(midi_song_data_off + tbl_off + idx * 30);     /* the patch's 30-byte descriptor */

    midi_emit_al  = midi_voice_note_byte;                     /* MOV AL,AH */
    midi_emit_ptr = (u8 __far *)MK_FP(midi_song_data_seg, di); /* XOR BX,BX folded in (BX==0) */

    emit_midi_voice_message();                                /* CALL 0x8bc8 */
}

/* ── emit_midi_voice_message (1000:8bc8) — shared OPL2 patch/note register writer ──
 *  Register-entry: AL = midi_emit_al (channel/operator-slot selector byte); DS:
 *  (BX+DI) = midi_emit_ptr (the 30-byte per-channel OPL patch descriptor; BX is always
 *  0 at this call boundary — every reconstructed caller XORs BX,BX (w1) or seeds BX=0
 *  directly — so DI/midi_emit_ptr already carries the full offset).
 *
 *  Always writes reg 0x08=0x00 (CSW/note-select clear) first, then branches on the
 *  descriptor's flag byte p[0]:
 *    ZERO    -> the 12-register "2-operator" program: 0xC0+al (feedback/connection,
 *               using the ORIGINAL al), then re-bases al to a per-channel "slot" value
 *               looked up from opl_fnum_lo_5593[al] and writes the 0x40/0x60/0x80/0x20
 *               (operator 1) and 0x43/0x63/0x83/0x23 (operator 2) register quads plus
 *               the 0xE0/0xE3 waveform-select pair, all offset by +slot.
 *    NONZERO -> a reduced 4-register program hardcoded to operator slot 1 (0x41/0x61/
 *               0x81/0x21) — the incoming al is DISCARDED (asm 8d86 "MOV AL,1").
 *  asm 1000:8bc8 verbatim (see the per-write comments below for the exact bit-field
 *  source of each register value).
 *
 *  RECONSTRUCTION FIDELITY: CX/DX are PUSHed/POPed (preserved) by the real asm but
 *  never read as inputs anywhere in the body (confirmed via the raw disasm — no
 *  `[..+CX..]`/`[..+DX..]`-relative read, no arithmetic on the incoming CX/DX values) —
 *  not modelled; their caller-seeded values are immaterial to the outcome (see
 *  tools/midi_ctest.c's call wrapper note). */
void emit_midi_voice_message(void)
{
    u8 al = midi_emit_al;
    u8 __far *p = midi_emit_ptr;
    u8 dl, dh, ah;

    opl_write_reg(0x08, 0x00);                          /* CSW/note-select clear (always) */

    if (p[0] == 0) {
        /* ── 12-register "2-operator" program ── */
        dl = (u8)(al + 0xc0);                           /* reg 0xC0+al: feedback/connection */
        dh = (u8)(p[4] & 0x7);
        dh = (u8)(dh << 1);
        ah = (u8)((p[0xe] & 1) ^ 1);
        dh = (u8)(dh | ah);
        opl_write_reg(dl, dh);

        al = opl_fnum_lo_5593[al];                       /* re-base: al := per-chan "slot" (0x203b:0x5593+al) */

        dl = (u8)(al + 0x40);                            /* reg 0x40+slot: level/KSL, operator 1 */
        dh = (u8)(p[0xa] & 0x3f);
        ah = (u8)((p[2] >> 2) & 0xc0);
        dh = (u8)(dh | ah);
        opl_write_reg(dl, dh);

        dl = (u8)(al + 0x60);                            /* reg 0x60+slot: attack/decay, operator 1 */
        dh = (u8)((p[5] & 0xf) << 4);
        ah = (u8)(p[8] & 0xf);
        dh = (u8)(dh | ah);
        opl_write_reg(dl, dh);

        dl = (u8)(al + 0x80);                            /* reg 0x80+slot: sustain/release, operator 1 */
        dh = (u8)((p[6] & 0xf) << 4);
        ah = (u8)(p[9] & 0xf);
        dh = (u8)(dh | ah);
        opl_write_reg(dl, dh);

        dl = (u8)(al + 0x20);                            /* reg 0x20+slot: AM/VIB/EG/KSR/mult, operator 1 */
        dh = (u8)(p[3] & 0xf);
        if (p[0xb] != 0) dh = (u8)(dh | 0x80);
        if (p[0xc] != 0) dh = (u8)(dh | 0x40);
        if (p[7]  != 0) dh = (u8)(dh | 0x20);
        if (p[0xd] != 0) dh = (u8)(dh | 0x10);
        opl_write_reg(dl, dh);

        dl = (u8)(al + 0x43);                            /* reg 0x43+slot: level/KSL, operator 2 */
        dh = (u8)(p[0x17] & 0x3f);
        ah = (u8)((p[0xf] >> 2) & 0xc0);
        dh = (u8)(dh | ah);
        opl_write_reg(dl, dh);

        dl = (u8)(al + 0x63);                            /* reg 0x63+slot: attack/decay, operator 2 */
        dh = (u8)((p[0x12] & 0xf) << 4);
        ah = (u8)(p[0x15] & 0xf);
        dh = (u8)(dh | ah);
        opl_write_reg(dl, dh);

        dl = (u8)(al + 0x83);                            /* reg 0x83+slot: sustain/release, operator 2 */
        dh = (u8)((p[0x13] & 0xf) << 4);
        ah = (u8)(p[0x16] & 0xf);
        dh = (u8)(dh | ah);
        opl_write_reg(dl, dh);

        dl = (u8)(al + 0x23);                            /* reg 0x23+slot: AM/VIB/EG/KSR/mult, operator 2 */
        dh = (u8)(p[0x10] & 0xf);
        if (p[0x18] != 0) dh = (u8)(dh | 0x80);
        if (p[0x19] != 0) dh = (u8)(dh | 0x40);
        if (p[0x14] != 0) dh = (u8)(dh | 0x20);
        if (p[0x1a] != 0) dh = (u8)(dh | 0x10);
        opl_write_reg(dl, dh);

        dl = (u8)(al + 0xe0);                            /* reg 0xE0+slot: waveform, operator 1 */
        dh = (u8)(p[0x1c] & 0x3);
        opl_write_reg(dl, dh);

        dl = (u8)(al + 0xe3);                            /* reg 0xE3+slot: waveform, operator 2 */
        dh = (u8)(p[0x1d] & 0x3);
        opl_write_reg(dl, dh);
    } else {
        /* ── reduced 4-register program, hardcoded to operator slot 1 (the incoming
         *  al/chan is DISCARDED here — asm 8d86 "MOV AL,1" verbatim) ── */
        al = 1;

        dl = (u8)(al + 0x40);                            /* reg 0x41: level/KSL */
        dh = (u8)(p[0xa] & 0x3f);
        ah = (u8)((p[2] >> 2) & 0xc0);
        dh = (u8)(dh | ah);
        opl_write_reg(dl, dh);

        dl = (u8)(al + 0x60);                            /* reg 0x61: attack/decay */
        dh = (u8)((p[5] & 0xf) << 4);
        ah = (u8)(p[8] & 0xf);
        dh = (u8)(dh | ah);
        opl_write_reg(dl, dh);

        dl = (u8)(al + 0x80);                            /* reg 0x81: sustain/release */
        dh = (u8)((p[6] & 0xf) << 4);
        ah = (u8)(p[9] & 0xf);
        dh = (u8)(dh | ah);
        opl_write_reg(dl, dh);

        dl = (u8)(al + 0x20);                            /* reg 0x21: AM/VIB/EG/KSR/mult */
        dh = (u8)(p[3] & 0xf);
        if (p[0xb] != 0) dh = (u8)(dh | 0x80);
        if (p[0xc] != 0) dh = (u8)(dh | 0x40);
        if (p[7]  != 0) dh = (u8)(dh | 0x20);
        if (p[0xd] != 0) dh = (u8)(dh | 0x10);
        opl_write_reg(dl, dh);
    }
}
