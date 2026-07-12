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
        if (p[0xb] != 0) {
            dh = (u8)(dh | 0x80);
        }
        if (p[0xc] != 0) {
            dh = (u8)(dh | 0x40);
        }
        if (p[7]  != 0) {
            dh = (u8)(dh | 0x20);
        }
        if (p[0xd] != 0) {
            dh = (u8)(dh | 0x10);
        }
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
        if (p[0x18] != 0) {
            dh = (u8)(dh | 0x80);
        }
        if (p[0x19] != 0) {
            dh = (u8)(dh | 0x40);
        }
        if (p[0x14] != 0) {
            dh = (u8)(dh | 0x20);
        }
        if (p[0x1a] != 0) {
            dh = (u8)(dh | 0x10);
        }
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
        if (p[0xb] != 0) {
            dh = (u8)(dh | 0x80);
        }
        if (p[0xc] != 0) {
            dh = (u8)(dh | 0x40);
        }
        if (p[7]  != 0) {
            dh = (u8)(dh | 0x20);
        }
        if (p[0xd] != 0) {
            dh = (u8)(dh | 0x10);
        }
        opl_write_reg(dl, dh);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  SMF parser (Task E1) — midi_read_varlen (8891) / seq_normalize_far_ptr (8a23) /
 *  midi_get_track_count (8999) / midi_init_track_table (87a2) /
 *  midi_parse_file (8809).
 *
 *  ── Ordering / the Task E2 forward dependency ───────────────────────────────────
 *  midi_read_varlen, seq_normalize_far_ptr, and midi_get_track_count are
 *  SELF-CONTAINED leaves (verified via disassemble_function: no CALL instructions
 *  in midi_read_varlen or seq_normalize_far_ptr; midi_get_track_count is a pure
 *  2-global getter) — all three are registered in tools/midi_ctest.c's PORTED[]
 *  this task and validate.
 *
 *  midi_init_track_table's real asm (1000:87ad..87b2) CALLS midi_read_varlen, and
 *  — when the decoded delta is exactly 0 (the real `OR BX,DX` / `JNZ` ZF test) —
 *  ALSO CALLS midi_process_event (1000:8809's own sibling at 873c, Task E2,
 *  genuinely not reconstructed yet: game_stubs.c/tools/midi_ctest.c both carry a
 *  faithful-signature carve-out stub for it so this task's real call site still
 *  links).  midi_parse_file (8809) calls midi_init_track_table once all tracks are
 *  walked, so it inherits the same forward dependency transitively.  Per the task
 *  brief: BOTH bodies are reconstructed 1:1 here (including the real conditional
 *  call), but NEITHER is registered in tools/midi_ctest.c's PORTED[] this task —
 *  they stay UNPORTED (not a hard failure) until Task E2 lands midi_process_event
 *  and can wire a real differential for them.
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── little helpers the parser uses to mirror the asm's own raw-vs-swapped memory
 *  reads (MIDI multi-byte header fields are big-endian; x86 LODSW/word-compare is
 *  little-endian) ─────────────────────────────────────────────────────────────── */
static u16 midi_rd16_raw(const u8 *p)
{
    /* the asm's own LODSW / `CMP word ptr [SI],imm` — a plain little-endian word
       read, NO byte-swap (matches raw memory order). */
    return (u16)(p[0] | (p[1] << 8));
}

static u16 midi_bswap16(u16 v)
{
    /* the asm's own `XCHG AL,AH` — swaps the two bytes of a just-LODSW'd word,
       turning the file's big-endian 16-bit field into its numeric value. */
    return (u16)((v >> 8) | (v << 8));
}

/* ── seq_normalize_far_ptr (1000:8a23) — DS:SI far-pointer renormalization ───────
 *  Register-entry, no stack args, no globals.  Real asm (1000:8a23..8a3a; AX/BX
 *  PUSH'd/POP'd around the body, so untouched by the caller's view):
 *      MOV AX,SI ; AND SI,0xf ; SHR AX,1 (x4) ; MOV BX,DS ; ADD AX,BX ; MOV DS,AX
 *  i.e. new_DS := old_DS + (old_SI >> 4), new_SI := old_SI & 0xF — this ROLLS SI's
 *  excess offset (everything above the low nibble) into DS.
 *
 *  RECONSTRUCTION FIDELITY (correcting an earlier working note): tools/midi_oracle.py's
 *  own header comment ("a no-op stub in THIS binary (verified: RET)") and this task's
 *  brief both mischaracterize this function as bare `RET`-only.  disassemble_function
 *  1000:8a23 shows 11 real instructions, not a lone RET — confirmed also via
 *  get_xrefs_to(1000:8a23): 2 real callers (midi_parse_file 885e; midi_process_event
 *  879d, Task E2).  The asm IS real register surgery.
 *
 *  It is, however, a PROVABLE VALUE-PRESERVING IDENTITY: normalizing a segment:offset
 *  pair by definition preserves the LINEAR ADDRESS it names
 *  (new_DS*16+new_SI == old_DS*16+old_SI, since (old_SI>>4)*16 + (old_SI&0xF) ==
 *  old_SI exactly).  This codebase's register-entry DS:SI standin for the MIDI event
 *  cursor (`snd_seq_cursor`, sound.h) is modelled as ONE merged pointer, not a split
 *  seg/off pair the way midi_song_data_off/_seg are (every already-ported register-
 *  entry MIDI leaf — opl_event_note_on, midi_emit_voice_msg_w1/2/3, ... — shares that
 *  same convention); no consumer anywhere in the reconstructed call graph inspects DS
 *  on its own.  Splitting that shared cursor abstraction for this one call site would
 *  ripple into every already-committed register-entry MIDI leaf for a change that is,
 *  by the math above, unobservable to any of them.  A literal FP_SEG/FP_OFF/MK_FP
 *  round-trip was considered and rejected: on the host replay harness `snd_seq_cursor`
 *  points into a small dedicated `si_window` buffer, not the harness's `far_mem` arena
 *  MK_FP indexes into, so re-deriving a "segment" from it and rebuilding a pointer via
 *  MK_FP would silently relocate the cursor into unrelated `far_mem` bytes — corrupting
 *  shared harness state for no representable benefit (the identity above already proves
 *  nothing consumer-observable changes).  Reconstructed as a true no-op body against
 *  this model; see docs/reconstruction-fidelity.md. */
void seq_normalize_far_ptr(void)
{
    /* Faithful no-op against this codebase's merged DS:SI cursor model — see the
       RECONSTRUCTION FIDELITY note above for why this is a value-preserving identity,
       not an invented simplification. */
}

/* ── midi_read_varlen (1000:8891) — decode a 7-bits/byte SMF variable-length
 *  quantity at the register-entry DS:SI cursor (snd_seq_cursor) ─────────────────
 *  Reconstructed against BOTH decompile_function_by_address AND
 *  disassemble_function (the decompile's CONCAT/bit-twiddling is an accurate but
 *  opaque rendering of the same instructions — cross-checked, not overridden).
 *  Standard SMF VLQ, MSB-first 7-bit groups, up to 4 bytes:
 *    1 byte  (b1 continuation clear):            value = b1
 *    2 bytes (b1 set, b2 clear):                 value = (b1&0x7f)<<7  | (b2&0x7f)
 *    3 bytes (b1,b2 set, b3 clear):               value = (b1&0x7f)<<14 | (b2&0x7f)<<7 | b3
 *    4 bytes (b1,b2,b3 set; b4 ALWAYS consumed — the asm never tests its own
 *             continuation bit): a genuine 4th-byte quirk — see below.
 *
 *  RECONSTRUCTION FIDELITY (register-entry + packed return — CORRECTING the task
 *  brief's own characterization): the asm returns via DX:AX (Watcom's native 32-bit
 *  return convention), packed here as `CONCAT22(dx,ax)`.  The brief describes this as
 *  "decoded value in the low word, byte-count in the high word" — that does NOT match
 *  the asm/decompile: DX is always 0 for the 1- and 2-byte branches (verified: the
 *  brief's own worked example, bytes `8a 70` -> 1392, decodes with DX=0, AX=1392 here
 *  — a "byte count" would read 2, not 0), and for the 3-byte branch DX is EXACTLY
 *  `(b1&0x7f)>>2` (the VLQ's bits 16-20) — i.e. DX:AX is the FULL decoded (up to
 *  28-bit) numeric value, split high:low, not value-plus-metadata.  This also matches
 *  midi_track_time_table's own pre-existing doc comment ("32-bit next-event clock") —
 *  midi_init_track_table (Task E1, below) stores this return verbatim into that table.
 *  Every branch below is a literal instruction-by-instruction transliteration of the
 *  raw disasm (not the closed-form formula above) so the SAME x86 8/16-bit
 *  truncation and CF-chained RCR behavior reproduces exactly, including the 4-byte
 *  branch's anomaly noted next.
 *
 *  RECONSTRUCTION FIDELITY (4-byte branch bit-layout anomaly, DISCOVERED this task):
 *  the 1/2/3-byte branches are clean, standard MSB-first VLQ decodes (verified
 *  numerically against 3 independent hand test-vectors, cross-checked two ways:
 *  literal instruction transliteration vs. the decompile's CONCAT expressions).  The
 *  4-byte branch does NOT extend that same clean pattern — e.g. bytes `81 80 80 7F`
 *  (a textbook 4-byte VLQ encoding 0x20007F per the standard MSB-first formula)
 *  decodes here to DX:AX = `0x001F:0xC0C0`, NOT `0x0002:0x007F`.  This was verified
 *  TWICE independently (literal per-instruction CF/RCR simulation, and the
 *  decompile's own CONCAT11 formula, both by hand) and both agree with EACH OTHER
 *  while disagreeing with the "obvious" 28-bit extension — strong evidence this is a
 *  genuine quirk (plausibly an unexercised latent bug) in the ORIGINAL 1992 binary's
 *  4-byte path, not a transcription error here.  4-byte SMF deltas are extremely rare
 *  in practice (>2 million ticks between events) and the Task C2 oracle capture (54
 *  midi_read_varlen records off the REAL Bumpy.mid) may not exercise this branch at
 *  all — reproduced faithfully as-is either way, per "adhere to the binary, never
 *  invent". See docs/reconstruction-fidelity.md. */
u32 midi_read_varlen(void)
{
    u16 ax, dx, t16;
    u8  b1, b2, b3, b4, cf, t8, dl, dh;

    ax = 0;
    dx = 0;

    b1 = *snd_seq_cursor;  snd_seq_cursor++;                     /* 8895 LODSB */
    if ((b1 & 0x80) == 0) {                                      /* 8896/8898 */
        /* ── 1-byte VLQ (0..0x7f) ── */
        ax = b1;
        return ((u32)dx << 16) | ax;
    }

    b2 = *snd_seq_cursor;  snd_seq_cursor++;                     /* 889c LODSB */
    if ((b2 & 0x80) == 0) {                                      /* 889d/889f */
        /* ── 2-byte VLQ ── */
        ax = (u16)(((u16)(b1 & 0x7f) << 8) | (b2 & 0x7f));       /* 88a1 AND AX,0x7f7f */
        t8 = (u8)(ax & 0xff);
        t8 = (u8)(t8 << 1);                                      /* 88a4 SHL AL,1 */
        ax = (u16)((ax & 0xff00) | t8);
        ax = (u16)(ax >> 1);                                     /* 88a6 SHR AX,1 */
        return ((u32)dx << 16) | ax;
    }

    /* ── 3-or-4-byte VLQ: 88aa XCHG AX,DX — DX := {AH=b1,AL=b2} raw, AX := 0 ── */
    dx = (u16)(((u16)b1 << 8) | b2);
    ax = 0;

    b3 = *snd_seq_cursor;  snd_seq_cursor++;                     /* 88ab LODSB */
    if ((b3 & 0x80) == 0) {                                      /* 88ac/88ae */
        /* ── 3-byte VLQ ── */
        ax = (u16)((ax & 0xff00) | b3);                          /* AL := b3 */

        t8 = (u8)(ax >> 8);                                      /* 88b0 XCHG AH,DL */
        dl = (u8)(dx & 0xff);
        ax = (u16)(((u16)dl << 8) | (ax & 0xff));
        dx = (u16)((dx & 0xff00) | t8);

        dl = (u8)(dx & 0xff);                                    /* 88b2 XCHG DL,DH */
        dh = (u8)(dx >> 8);
        dx = (u16)(((u16)dl << 8) | dh);

        dx = (u16)(dx & 0x7f7f);                                 /* 88b4 AND DX,0x7f7f */

        ax = (u16)(ax << 1);                                     /* 88b8 SHL AX,1 (CF discarded — overwritten next) */

        t8 = (u8)(ax & 0xff);                                    /* 88ba SHL AL,1 */
        cf = (u8)((t8 >> 7) & 1);
        t8 = (u8)(t8 << 1);
        ax = (u16)((ax & 0xff00) | t8);

        t8 = (u8)(dx & 0xff);                                    /* 88bc SHR DL,1 */
        cf = (u8)(t8 & 1);
        t8 = (u8)(t8 >> 1);
        dx = (u16)((dx & 0xff00) | t8);

        t16 = (u16)(ax & 1);                                     /* 88be RCR AX,1 */
        ax = (u16)((ax >> 1) | ((u16)cf << 15));
        cf = (u8)t16;

        t8 = (u8)(dx & 0xff);                                    /* 88c0 SHR DL,1 */
        cf = (u8)(t8 & 1);
        t8 = (u8)(t8 >> 1);
        dx = (u16)((dx & 0xff00) | t8);

        ax = (u16)((ax >> 1) | ((u16)cf << 15));                 /* 88c2 RCR AX,1 */

        return ((u32)dx << 16) | ax;
    }

    /* ── 4-byte VLQ (b4 consumed unconditionally — the asm never tests its own
     *  continuation bit; see the RECONSTRUCTION FIDELITY note above) ── */
    b4 = *snd_seq_cursor;  snd_seq_cursor++;                     /* 88c8 LODSB */
    ax = (u16)(((u16)b3 << 8) | b4);                             /* 88c6 MOV AH,AL; 88c8 LODSB -> AX={b3,b4} */

    t16 = ax; ax = dx; dx = t16;                                 /* 88c9 XCHG AX,DX */

    dx = (u16)(dx & 0x7f7f);                                     /* 88ca AND DX,0x7f7f */

    t8 = (u8)(dx & 0xff);                                        /* 88ce SHL DL,1 */
    t8 = (u8)(t8 << 1);
    dx = (u16)((dx & 0xff00) | t8);

    dx = (u16)(dx >> 1);                                         /* 88d0 SHR DX,1 */

    ax = (u16)(ax << 1);                                         /* 88d2 SHL AX,1 (CF discarded) */

    t8 = (u8)(ax & 0xff);                                        /* 88d4 SHL AL,1 */
    cf = (u8)((t8 >> 7) & 1);
    t8 = (u8)(t8 << 1);
    ax = (u16)((ax & 0xff00) | t8);

    t8 = (u8)(dx & 0xff);                                        /* 88d6 SHR DL,1 */
    cf = (u8)(t8 & 1);
    t8 = (u8)(t8 >> 1);
    dx = (u16)((dx & 0xff00) | t8);

    t16 = (u16)(ax & 1);                                         /* 88d8 RCR AX,1 */
    ax = (u16)((ax >> 1) | ((u16)cf << 15));
    cf = (u8)t16;

    t8 = (u8)(dx & 0xff);                                        /* 88da SHR DL,1 */
    cf = (u8)(t8 & 1);
    t8 = (u8)(t8 >> 1);
    dx = (u16)((dx & 0xff00) | t8);

    ax = (u16)((ax >> 1) | ((u16)cf << 15));                     /* 88dc RCR AX,1 */

    return ((u32)dx << 16) | ax;
}

/* ── midi_get_track_count (1000:8999) — SMF track-count getter ───────────────────
 *  0 args, pure getter.  asm 1000:8999 verbatim: MOV AX,CS:[0x85a1]; AND AX,AX;
 *  JNZ 89a7; MOV AX,CS:[0x8483]; AND AX,AX; RET.  Returns `midi_track_count`
 *  (CODE 0x85a1) unless it's 0, in which case it falls back to `midi_data_seg`
 *  (CODE 0x8483).
 *
 *  RECONSTRUCTION FIDELITY (0x85a1 name-clash — VERIFIED this task, per the task's
 *  own ask): CODE 0x85a1 is the SAME physical cell sound.c's `midi_track_count`
 *  (the MPU-401 poll-timeout residual write in mpu401_write_data_polled) writes.
 *  get_xrefs_to(1000:85a1) confirms BOTH readers of this getter (midi_get_track_count
 *  here; midi_init_track_table's loop-count read at 87a2, below) and BOTH SMF writers
 *  (midi_parse_file's `MOV CS:[0x85a1],AX` at 8846, below; midi_process_event's
 *  end-of-track decrement at 877d, Task E2) target the exact same address as
 *  mpu401_write_data_polled's residual store — genuinely the SAME cell, reused by two
 *  logically-unrelated engine subsystems (not a coincidental name), exactly as
 *  midi.h's pre-existing ownership note already documented.  midi.h's existing
 *  extern of sound.c's `midi_track_count` is reused unchanged; no new global. */
s16 midi_get_track_count(void)
{
    s16 track_count = midi_track_count;
    if (midi_track_count == 0) {
        track_count = (s16)midi_data_seg;
    }
    return track_count;
}

/* ── midi_init_track_table (1000:87a2) — per-track state-table seed ──────────────
 *  0 args.  Loops `midi_track_count` times (asm: `MOV CX,CS:[0x85a1]; ... LOOP 87aa`
 *  — a real x86 LOOP, so CX==0 would wrap and loop 65536 times; midi_parse_file's
 *  own 0<count<=0x10 guard is what keeps this from ever happening on the real path).
 *  Per track: LDS the track's CURRENT {off,seg} from midi_track_ptr_table into the
 *  DS:SI cursor, decode its first delta-time via midi_read_varlen; if that delta is
 *  exactly 0 (the real `OR BX,DX` / `JNZ` ZF test — the event is due immediately),
 *  dispatch it via midi_process_event and store WHATEVER it returns instead (Task
 *  E2 — see the RECONSTRUCTION FIDELITY / forward-dependency note at the top of this
 *  section); either way, store the resulting 32-bit value into
 *  midi_track_time_table[track] and write the ADVANCED cursor back into
 *  midi_track_ptr_table[track].  asm 1000:87a2 verbatim: MOV CX,CS:[0x85a1]; MOV
 *  BX,0x81cc; LDS SI,CS:[BX]; CALL 8891; JNZ 87b5; CALL 873c; MOV CS:[BX+0x40],AX;
 *  MOV CS:[BX+0x42],DX; MOV CS:[BX],SI; MOV AX,DS; MOV CS:[BX+2],AX; INC BX (x4);
 *  LOOP 87aa. */
void midi_init_track_table(void)
{
    u16 cx;
    u16 idx;
    u32 val;

    cx  = (u16)midi_track_count;                                  /* CX (LOOP counter) */
    idx = 0;                                                       /* BX/4 — track index */

    do {
        /* 87aa LDS SI,CS:[BX] — point the cursor at this track's current {off,seg} */
        snd_seq_cursor = (u8 *)MK_FP(midi_track_ptr_table[idx][1],
                                     midi_track_ptr_table[idx][0]);

        val = midi_read_varlen();                                   /* 87ad CALL 8891 */
        if (val == 0) {                                              /* 87b0 JNZ (ZF set -> fall through) */
            val = midi_process_event();                               /* 87b2 CALL 873c (Task E2) */
        }

        midi_track_time_table[idx][0] = (u16)(val & 0xffffu);        /* 87b5 */
        midi_track_time_table[idx][1] = (u16)(val >> 16);            /* 87b9 */

        midi_track_ptr_table[idx][0] = FP_OFF(snd_seq_cursor);        /* 87bd MOV CS:[BX],SI */
        midi_track_ptr_table[idx][1] = FP_SEG(snd_seq_cursor);        /* 87c0/87c2 MOV AX,DS; MOV CS:[BX+2],AX */

        idx++;                                                        /* 87c6..87c9 INC BX x4 */
        cx--;                                                          /* LOOP's own decrement */
    } while (cx != 0);
}

/* ── midi_parse_file (1000:8809) — validate MThd, walk MTrk chunks ───────────────
 *  Register-entry: DS:SI = the file image (midi_load_sequence's fall-through
 *  arg — see midi.h's ownership note).  Validates the 14-byte MThd header
 *  ("MThd", length==6, format!=2, 0<ntrks<=0x10, division's SMPTE bit clear),
 *  stores midi_track_count/midi_division, then per track: normalizes the cursor
 *  (seq_normalize_far_ptr), validates the 8-byte MTrk header ("MTrk", a 16-bit
 *  length), records the track's event-data start into midi_track_ptr_table, and
 *  advances the cursor past the track's data (bailing on a 16-bit offset overflow
 *  — the asm's own `JC`).  On success (all tracks consumed) calls
 *  midi_init_track_table and returns -1 (0xFFFF); any validation failure returns 0.
 *  Multi-byte header fields are big-endian in the file; the asm's own
 *  LODSW+`XCHG AL,AH` byte-swap idiom is mirrored via midi_bswap16, while the FIXED
 *  MThd-length/format-reject checks compare RAW (un-swapped) memory the same way
 *  the asm's literal `CMP word ptr [SI],imm` does (see midi_rd16_raw's own note).
 *  asm 1000:8809 verbatim: see the per-line comments below (byte-for-byte matched
 *  against disassemble_function 1000:8809..8890).
 *
 *  RECONSTRUCTION FIDELITY (16-bit offset overflow via FP_OFF): the asm's `ADD
 *  SI,AX; JC 888e` bails when adding a track's byte length overflows SI's 16-bit
 *  offset.  This reconstruction's DS:SI cursor is a genuine far pointer only on the
 *  real Watcom -ml build (where FP_OFF(snd_seq_cursor) IS the live SI register, so
 *  the check below is exact); the host replay harness's FP_OFF is a `far_mem`-arena-
 *  relative approximation (this function is UNPORTED this task — see the top-of-
 *  section note — so that approximation is never exercised by the differential). */
int midi_parse_file(void)
{
    u16 track_count, division, fmt_raw;
    u16 len_hi, len_lo, track_len;
    u16 cx, idx;
    u16 off_before, off_after;

    if (midi_rd16_raw(snd_seq_cursor) != 0x544d) {                  /* 8809 "MT" */
        return 0;
    }
    snd_seq_cursor += 2;
    if (midi_rd16_raw(snd_seq_cursor) != 0x6468) {                  /* 8814 "hd" */
        return 0;
    }
    snd_seq_cursor += 2;
    if (midi_rd16_raw(snd_seq_cursor) != 0x0000) {                  /* 881f length hi word == 0 */
        return 0;
    }
    snd_seq_cursor += 2;
    if (midi_rd16_raw(snd_seq_cursor) != 0x0600) {                  /* 8829 length lo word == 6 (raw, per asm) */
        return 0;
    }
    snd_seq_cursor += 2;

    fmt_raw = midi_rd16_raw(snd_seq_cursor);
    if (fmt_raw == 0x0200) {                                        /* 8831/8835 reject format==2 (checked pre-swap) */
        return 0;
    }
    snd_seq_cursor += 2;                                             /* 8837 — format value itself never read/stored */

    track_count = midi_bswap16(midi_rd16_raw(snd_seq_cursor));       /* 8839/883a LODSW; XCHG AL,AH */
    snd_seq_cursor += 2;
    if (track_count == 0 || track_count > 0x10) {                    /* 883c..8844 */
        return 0;
    }
    midi_track_count = (s16)track_count;                                /* 8846 */

    division = midi_bswap16(midi_rd16_raw(snd_seq_cursor));             /* 884a/884b */
    snd_seq_cursor += 2;
    if ((division & 0x8000) != 0) {                                   /* 884d/8850 reject SMPTE division */
        return 0;
    }
    midi_division = division;                                             /* 8852 */

    cx  = track_count;                                                     /* 8856 MOV CX,... */
    idx = 0;                                                                /* 885b MOV BX,0x81cc */

    for (;;) {
        seq_normalize_far_ptr();                                             /* 885e CALL 8a23 */

        if (midi_rd16_raw(snd_seq_cursor) != 0x544d) {                        /* 8861/8862 "MT" */
            return 0;
        }
        snd_seq_cursor += 2;
        if (midi_rd16_raw(snd_seq_cursor) != 0x6b72) {                         /* 8867/8868 "rk" */
            return 0;
        }
        snd_seq_cursor += 2;

        len_hi = midi_rd16_raw(snd_seq_cursor);                                 /* 886d */
        snd_seq_cursor += 2;
        if (len_hi != 0) {                                                       /* 886e/8871 */
            return 0;
        }

        len_lo = midi_rd16_raw(snd_seq_cursor);                                   /* 8873 (raw, pre-swap) */
        snd_seq_cursor += 2;

        midi_track_ptr_table[idx][0] = FP_OFF(snd_seq_cursor);                     /* 8874 MOV CS:[BX],SI */
        midi_track_ptr_table[idx][1] = FP_SEG(snd_seq_cursor);                     /* 8879 MOV CS:[BX+2],DS */
        idx++;                                                                      /* 8877/887c INC BX x4 */

        track_len  = midi_bswap16(len_lo);                                           /* 887e XCHG AL,AH */
        off_before = FP_OFF(snd_seq_cursor);
        off_after  = (u16)(off_before + track_len);
        if (off_after < off_before) {                                                 /* 8882 JC — 16-bit offset overflow */
            return 0;
        }
        snd_seq_cursor += track_len;                                                   /* 8880 ADD SI,AX */

        cx--;                                                                           /* 8884 LOOP's own decrement */
        if (cx == 0) {
            break;
        }
    }

    midi_init_track_table();                                                            /* 8886 CALL 87a2 */
    return -1;                                                                            /* 8889 MOV AX,0xFFFF */
}
