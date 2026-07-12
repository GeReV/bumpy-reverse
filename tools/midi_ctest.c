/* Host REPLAY HARNESS for src/midi.c (+ its OPL2 backend, src/sound.c) — Task C3.
 *
 * Forks tools/sound_ctest.c's scaffold: shims the Watcom 16-bit environment out
 * (BUMPY_H, exact-width typedefs, __far/__huge/__cdecl16near erased, the outp/inp
 * port-capture shims, a minimal <dos.h> union REGS/int86x stand-in), then compiles
 * the REAL reconstructed C on the host and validates it against the Task C2 capture
 * local/build/render/midi_trace.bin (magic "MIDTRC01", version 1 — layout frozen in
 * tools/midi_oracle.py's header and .superpowers/sdd/task-C2-report.md's "MIDI_SNAP
 * layout" section).
 *
 * ── THE INCLUDE STRUCTURE (the crux of this task) ────────────────────────────────
 * The MIDI engine's future bodies (midi_process_event's dispatch, the voice-message
 * funnel midi_emit_voice_msg_w1/w2/w3 -> emit_midi_voice_message, ...) call the OPL2
 * driver that lives in src/sound.c (opl_write_reg, opl_play_note, opl2_all_notes_off)
 * and share globals with it (snd_seq_cursor / snd_seq_event_al / snd_seq_default_chan,
 * the opl_fnum_* / opl_chan_* runtime tables, snddrv_dispatch_a/b/c/d). So this ONE
 * translation unit `#include`s BOTH src/sound.c AND src/midi.c, in that order —
 * mirroring sound_ctest.c's own "#include the real module, shim only what a host
 * build needs" convention, just with two modules instead of one.
 *
 * This works as a single clean dual-include with NO workaround needed:
 *   - src/sound.c's own `#ifndef BUMPY_H` guard (around its <conio.h>/<dos.h>
 *     includes) already skips those system headers once BUMPY_H is pre-#defined —
 *     the exact mechanism sound_ctest.c relies on; unaffected by adding midi.c.
 *   - src/midi.c is a GLOBALS-ONLY skeleton (Task C1: no function bodies yet) that
 *     `#include`s "midi.h" then "sound.h" (guarded by `#ifndef SOUND_H`, already
 *     pulled in by sound.c) — so its second `#include "sound.h"` is a no-op re-guard,
 *     not a redefinition. midi.c DEFINES a small set of module globals (song/aux far-
 *     ptr halves, tempo/division fields, the two per-track tables) that appear
 *     nowhere else in src/ (grep-verified by Task C1) — zero duplicate symbols against
 *     sound.c.
 *   - The only symbols BOTH TUs' compiled bodies reference that this harness must
 *     itself provide are exactly the ones sound_ctest.c already provides for sound.c
 *     alone (the player.c cross-module globals + the game_stubs.c carve-out no-ops):
 *     midi.c contributes NO function bodies, so it adds NO new host-shim
 *     requirement beyond what sound_ctest.c already needed. Concretely: sound.c's
 *     compiled bodies reference `seq_set_channel_param` / `midi_emit_voice_msg_w3` /
 *     `opl_event_note_on` as carve-out leaves (normally game_stubs.c's no-op stubs,
 *     NOT linked here); midi.h ALSO prototypes those same three (part of its call
 *     tree map, matching signatures) but midi.c does not DEFINE them (Task C1 report)
 *     — so this harness's own host no-op bodies (below, verbatim from sound_ctest.c)
 *     are the only bodies satisfying both TUs, with no clash.
 *   - out()/in() (and outp/inp/outpw/inpw) are the SHARED port-I/O shims — defined
 *     exactly once, before either #include, precisely as sound_ctest.c does; both
 *     TUs' compiled bodies that eventually reach the OPL2/MPU/PIT ports (currently
 *     only sound.c's already-real L4 drivers — midi.c has no bodies yet) resolve to
 *     these same definitions.
 * No alternative (separate .obj + link) was needed.
 *
 * ── BASELINE STATE (this task) ───────────────────────────────────────────────────
 * src/midi.c defines ONLY the MIDI module's globals — NO midi_* / seq_* / opl_event_*
 * function bodies exist yet (Phase D/E, future tasks). So the PORTED[] registry below
 * is EMPTY: every record in the trace resolves UNPORTED (never a hard failure), and
 * the comparators (cmp_semantic / cmp_ports) are wired but dormant — exactly
 * sound_ctest.c's own T2-skeleton baseline, just with a registry that starts at zero
 * entries instead of all-NULL (there is nothing to reference by name yet: the
 * midi_* / opl_event_* symbols this task's future entries would name do not exist as
 * function bodies, so referencing them now would fail to link).
 *
 * Build/run (also wrapped by tools/validate_midi.sh):
 *     cc -O2 -Wall -Werror -o /tmp/midi_ctest tools/midi_ctest.c && \
 *       /tmp/midi_ctest local/build/render/midi_trace.bin
 * Exit 0 iff the harness parses the trace, runs, and the differential has ZERO
 * failures on PORTED records (UNPORTED records never fail).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── shim the Watcom 16-bit environment for host compilation (verbatim from
 *  tools/sound_ctest.c — both included modules were written against this exact
 *  host typedef set) ─────────────────────────────────────────────────────────── */
#define BUMPY_H            /* sound.h's/midi.h's #include "bumpy.h" becomes a no-op */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge
#define __cdecl16near

/* ── HOST PORT-I/O SHIMS (shared by BOTH included TUs — defined once) ─────────────
 *  Only src/sound.c's already-real L4 drivers (opl_write_reg, pc_speaker_silence,
 *  mpu401_write_data_polled, ...) issue out()/in() today; src/midi.c has no bodies
 *  yet (baseline). Once Phase D/E lands MIDI bodies that reach those same drivers,
 *  they resolve to these identical shims — no separate MIDI-side port shim needed. */
typedef struct { u16 port; u16 val; u8 size; } io_evt_t;
#define IO_CAP_MAX 4096
static io_evt_t out_cap[IO_CAP_MAX];     /* host OUT capture (port,val,size), in order */
static int      out_cap_n = 0;
static u16      in_queue[IO_CAP_MAX];     /* recorded IN values for the cur record   */
static int      in_queue_n = 0, in_queue_i = 0;

/* PERTURBATION knob (proves the port-write gate is REAL — not self-consistent, once
 *  an L4 MIDI/OPL entry is PORTED). MIDI_PERTURB=N XORs the Nth captured OUT event's
 *  value by 1; dormant in this baseline (no PORTED L4 entries yet to perturb). */
static int  g_perturb_idx = -1;     /* -1 = disabled */
static int  g_out_seen    = 0;      /* running OUT counter across the whole run */

static void host_out_sz(u16 port, u16 val, u8 size)
{
    if (g_perturb_idx >= 0 && g_out_seen == g_perturb_idx) {
        val ^= 1;                   /* corrupt exactly one emitted value */
    }
    g_out_seen++;
    if (out_cap_n < IO_CAP_MAX) {
        out_cap[out_cap_n].port = port;
        out_cap[out_cap_n].val  = val;
        out_cap[out_cap_n].size = size;
        out_cap_n++;
    }
}
static u16 host_in(u16 port)
{
    (void)port;
    if (in_queue_i < in_queue_n) return in_queue[in_queue_i++];
    return 0xFF;   /* replay exhausted/empty — deterministic fallback */
}

#ifdef __GNUC__
#  define HOST_UNUSED __attribute__((unused))
#else
#  define HOST_UNUSED
#endif
static HOST_UNUSED int      outp (u16 port, int val) { host_out_sz(port, (u16)val, 1); return val; }
static HOST_UNUSED unsigned inp  (u16 port)          { return host_in(port); }
static HOST_UNUSED int      outpw(u16 port, int val) { host_out_sz(port, (u16)val, 2); return val; }
static HOST_UNUSED unsigned inpw (u16 port)          { return host_in(port); }
static HOST_UNUSED void     out  (u16 port, u16 val) { host_out_sz(port, val, 1); }
static HOST_UNUSED u16      in   (u16 port)          { return host_in(port); }

/* ── Minimal host shim for <dos.h>'s union REGS / struct SREGS / int86x (verbatim
 *  from tools/sound_ctest.c) ───────────────────────────────────────────────────────
 *  src/sound.c's timer_teardown_restore (Task A3) issues 3 real DOS INT 21h calls
 *  guarded by isr_installed_flag, which stays 0 on every replay scenario (both the
 *  sound trace and this MIDI trace) — this shim exists purely so that (dead-on-this-
 *  harness) call site type-checks on the host build. Must precede the #includes below
 *  (sound.c references these types/this function textually). */
union REGS { struct { unsigned char al,ah,bl,bh,cl,ch,dl,dh; } h;
             struct { unsigned int  ax,bx,cx,dx,si,di; } x; };
struct SREGS { unsigned int es,cs,ss,ds; };
static int int86x(int intno, union REGS *inr, union REGS *outr, struct SREGS *segr)
{
    (void)intno; (void)inr;
    if (outr) { outr->x.ax = 0; outr->x.bx = 0; outr->x.cx = 0;
                outr->x.dx = 0; outr->x.si = 0; outr->x.di = 0; }
    if (segr) { segr->es = 0; segr->cs = 0; segr->ss = 0; segr->ds = 0; }
    return 0;
}

/* ── the dual-include: the real OPL2 driver + the MIDI module sharing its globals ── */
#include "../src/sound.c"
#include "../src/midi.c"

/* ════════════════════════════════════════════════════════════════════════════
 *  Trace format (frozen — see tools/midi_oracle.py's header and
 *  .superpowers/sdd/task-C2-report.md "MIDI_SNAP layout" / "Per-function record
 *  counts" sections).
 *
 *  Header:  magic[8]="MIDTRC01", u16 version(=1), u16 n_scenarios,
 *           u16 n_fn_names, then per name {u8 len, len bytes ascii}.
 *  Per scenario: u8 id, u8 name_len, name bytes, u8 seed_byte (always 0xFF — MIDI
 *                has no single per-scenario "seed device" the way sound does),
 *                u32 n_records, then n_records records.
 *  Per record: u16 fn_off, u16 fn_name_idx, MIDI_SNAP entry, MIDI_SNAP exit,
 *              u16 n_io, then n_io * { u8 dir(0=OUT,1=IN), u16 port, u8 size, u16 value }.
 *
 *  MIDI_SNAP (struct "<8H" + "HHHHHHHHBBh" + "32s" + "192s" + "16s", 276 B, LE):
 *    u16 ax,bx,cx,dx,si,di,ds,es       — full register file at snapshot time
 *    u16 midi_song_data_off           DGROUP 0x5580
 *    u16 midi_song_data_seg           DGROUP 0x5582
 *    u16 midi_seq_step_active         DGROUP 0x557e
 *    u16 midi_aux_ptr_off             CODE   0x8485
 *    u16 midi_aux_ptr_seg             CODE   0x8487
 *    u16 midi_data_seg (a.k.a. midi_load_flag, same cell)  CODE 0x8483
 *    u16 midi_division                CODE   0x85a3
 *    u16 midi_tempo_lo                CODE   0x85a5
 *    u8  midi_tempo_hi                CODE   0x85a7
 *    u8  _pad
 *    s16 midi_track_count             CODE   0x85a1  (sound.c-owned, same cell)
 *    32B si_window                    32 bytes read at (DS:SI) at snapshot time
 *    192B track_tables                CODE 0x81cc..0x828c: midi_track_ptr_table[16][2]
 *                                      (0x81cc, 64B) + midi_track_time_table[16][2]
 *                                      (0x820c, 64B) + a per-track "default channel"
 *                                      continuation table (0x824c, 64B — one byte per
 *                                      track at a 4-byte stride; discovered by Task C2,
 *                                      NOT YET a src/midi.c global — see seed_globals).
 *    16B chan_param_table             CODE 0x8473..0x8483 — seq_set_channel_param's
 *                                      per-channel byte table (NOT YET a src/midi.c
 *                                      global either — see seed_globals).
 * ════════════════════════════════════════════════════════════════════════════ */
#define SI_WINDOW_LEN    32
#define TRACK_TABLES_LEN 192   /* CODE 0x81cc..0x828c */
#define CHAN_PARAM_LEN   16    /* CODE 0x8473..0x8483 */
/* MIDI_SNAP byte size: 8*2 (regs) + 8*2+1+1+2 (the 11 sequencer-state fields) +
 * 32 + 192 + 16 = 16 + 20 + 240 = 276. */
#define SNAP_SIZE  (8 * 2 + 8 * 2 + 1 + 1 + 2 + SI_WINDOW_LEN + TRACK_TABLES_LEN + CHAN_PARAM_LEN)
#define TRACE_VER  1

typedef struct {
    u16 ax, bx, cx, dx, si, di, ds, es;
    u16 midi_song_data_off_v;
    u16 midi_song_data_seg_v;
    u16 midi_seq_step_active_v;
    u16 midi_aux_ptr_off_v;
    u16 midi_aux_ptr_seg_v;
    u16 midi_data_seg_v;
    u16 midi_division_v;
    u16 midi_tempo_lo_v;
    u8  midi_tempo_hi_v;
    u8  _pad;
    s16 midi_track_count_v;
    u8  si_window[SI_WINDOW_LEN];
    u8  track_tables[TRACK_TABLES_LEN];
    u8  chan_param_table[CHAN_PARAM_LEN];
} snap_t;

typedef struct { u8 dir; u16 port; u8 size; u16 value; } io_t;

typedef struct {
    u16    fn_off;
    u16    fn_name_idx;
    snap_t ent, ex;
    u16    n_io;
    io_t  *io;            /* points into a per-record malloc'd array */
} record_t;

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static s16 rds16(const u8 *p) { return (s16)(u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) |
                                      ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void parse_snap(const u8 *p, snap_t *s)
{
    u32 o = 0;
    s->ax = rd16(p + o); o += 2;
    s->bx = rd16(p + o); o += 2;
    s->cx = rd16(p + o); o += 2;
    s->dx = rd16(p + o); o += 2;
    s->si = rd16(p + o); o += 2;
    s->di = rd16(p + o); o += 2;
    s->ds = rd16(p + o); o += 2;
    s->es = rd16(p + o); o += 2;
    s->midi_song_data_off_v    = rd16(p + o); o += 2;
    s->midi_song_data_seg_v    = rd16(p + o); o += 2;
    s->midi_seq_step_active_v  = rd16(p + o); o += 2;
    s->midi_aux_ptr_off_v      = rd16(p + o); o += 2;
    s->midi_aux_ptr_seg_v      = rd16(p + o); o += 2;
    s->midi_data_seg_v         = rd16(p + o); o += 2;
    s->midi_division_v         = rd16(p + o); o += 2;
    s->midi_tempo_lo_v         = rd16(p + o); o += 2;
    s->midi_tempo_hi_v         = p[o++];
    s->_pad                    = p[o++];
    s->midi_track_count_v      = rds16(p + o); o += 2;
    memcpy(s->si_window, p + o, SI_WINDOW_LEN);       o += SI_WINDOW_LEN;
    memcpy(s->track_tables, p + o, TRACK_TABLES_LEN); o += TRACK_TABLES_LEN;
    memcpy(s->chan_param_table, p + o, CHAN_PARAM_LEN); o += CHAN_PARAM_LEN;
    (void)o;   /* == SNAP_SIZE, by construction */
}

/* ── the current record, published for future PORTED-wrapper arg recovery ────────
 *  Mirrors tools/sound_ctest.c's g_cur_rec convention: several of the 21 targets are
 *  register-entry (channel nibble in AL, or in BX; a byte at DS:SI) that the fixed
 *  MIDI_SNAP shape does not name per-function — Task D/E's zero-arg registry wrappers
 *  recover those directly from g_cur_rec->ent (raw registers + si_window), the same
 *  pattern sound_ctest.c's call_seq_set_channel_param-style wrappers would use here. */
static const record_t *g_cur_rec = NULL;

/* ── ENTRY MIDI_SNAP -> reconstructed MIDI/sound globals ──────────────────────────
 *  Seed the sequencer state from the captured ENTRY snap, then (once a fn is PORTED)
 *  the comparator diffs the post-call state against the EXIT snap.
 *
 *  NOT YET SEEDABLE INTO A REAL MODULE GLOBAL (documented gap, not invented): Task C2
 *  discovered two of the four sub-tables the 192B/16B blocks cover have no backing
 *  src/midi.c global yet —
 *    (a) track_tables[128..191] — the per-track "default channel" continuation table
 *        (CODE 0x824c..0x828c), found via disassembly of midi_process_event's marker-
 *        event handler, postdating Task C1's enumeration;
 *    (b) chan_param_table[0..15] — seq_set_channel_param's per-channel byte table
 *        (CODE 0x8473..0x8483), never enumerated as a global at all.
 *  This task's file list is tools/midi_ctest.c + tools/validate_midi.sh only (see the
 *  brief's staging constraint) — it does not add globals to src/midi.h/midi.c. Both
 *  sub-tables are kept as HARNESS-SIDE shadow buffers below (seeded from the ENTRY
 *  snap so the struct shape is fully exercised), clearly flagged so a Phase-D/E task
 *  that defines the real globals can point these at them (or delete the shadow) —
 *  not a silent gap. */
static u8 g_si_window_buf[SI_WINDOW_LEN];
static u8 g_track_default_chan_shadow[64];     /* track_tables[128..191] — see note above */
static u8 g_chan_param_table_shadow[CHAN_PARAM_LEN];   /* see note above */

static void seed_globals(const snap_t *s)
{
    /* si_window -> a host buffer, with snd_seq_cursor (sound.c-owned, DS:SI stand-in
     * for the 9 already-ported snddrv_dispatch_b/c/d MIDI backends AND the future
     * midi_process_event/midi_read_varlen cursor) pointed at it — the brief's exact
     * ask ("fill a host buffer with the 32B si_window and point snd_seq_cursor at
     * it"). */
    memcpy(g_si_window_buf, s->si_window, SI_WINDOW_LEN);
    snd_seq_cursor = g_si_window_buf;

    /* snd_seq_event_al: AL is the MIDI event status/data byte for every register-
     * entry target in this call tree (seq_set_channel_param, opl_event_note_on,
     * midi_emit_voice_msg_w2/w3 all read AL directly per the C2 report's ABI
     * summary) — seeded from the entry register file's AX low byte. */
    snd_seq_event_al = (u8)(s->ax & 0xFF);

    /* the sequencer-state globals (all real src/midi.c or sound.c-owned globals). */
    midi_song_data_off   = s->midi_song_data_off_v;
    midi_song_data_seg   = s->midi_song_data_seg_v;
    midi_seq_step_active = s->midi_seq_step_active_v;
    midi_aux_ptr_off     = s->midi_aux_ptr_off_v;
    midi_aux_ptr_seg     = s->midi_aux_ptr_seg_v;
    midi_data_seg        = s->midi_data_seg_v;
    midi_division        = s->midi_division_v;
    midi_tempo_lo        = s->midi_tempo_lo_v;
    midi_tempo_hi        = s->midi_tempo_hi_v;
    midi_track_count     = s->midi_track_count_v;

    /* track_tables[0..63] -> midi_track_ptr_table, [64..127] -> midi_track_time_table
     * (both real src/midi.c globals, each a u16[16][2] = 64 B — memcpy'd as raw bytes,
     * same little-endian layout the trace captured). [128..191] has no real global
     * yet (see the note above) -> the shadow buffer only. */
    memcpy(midi_track_ptr_table,  s->track_tables + 0,  64);
    memcpy(midi_track_time_table, s->track_tables + 64, 64);
    memcpy(g_track_default_chan_shadow, s->track_tables + 128, 64);

    /* chan_param_table -> shadow only (no real global yet, see the note above). */
    memcpy(g_chan_param_table_shadow, s->chan_param_table, CHAN_PARAM_LEN);

    /* snd_seq_default_chan ("the channel"): models CS:[BX+0x80], i.e. track 0's byte
     * in the (not-yet-a-real-global) default-channel table, as a baseline default —
     * a specific per-record track index is a per-function arg-recovery detail (which
     * track's BX applies depends on the caller, out of a generic seed's scope), left
     * to Task D/E's registry wrappers via g_cur_rec, same as sound_ctest.c's
     * find_changed_*_channel default-to-0 convention. */
    snd_seq_default_chan = g_track_default_chan_shadow[0];
}

/* ── (A) SEMANTIC-STATE COMPARATOR — read live MIDI/sound globals vs the EXIT snap ─
 *  Returns NULL if every captured field matches the EXIT snap, else a short field
 *  name of the first divergence; got/want filled.
 *
 *  check_tbl gates the midi_track_ptr_table/midi_track_time_table (128 B) compare —
 *  reserved for the future table-filling fns (midi_parse_file / midi_init_track_table)
 *  whose CONTRACT is the table install, mirroring sound_ctest.c's check_tbl convention.
 *  The track_tables[128..191] tail and chan_param_table are NOT compared here (no real
 *  module global backs them yet — see seed_globals's note); a Phase-D/E task that adds
 *  those globals to src/midi.c must extend this comparator to cover them too. */
static const char *cmp_semantic(const snap_t *ex, long *got, long *want, int check_tbl)
{
    #define CHK(field, live) do { if ((long)(live) != (long)(ex->field)) { \
        *got = (long)(live); *want = (long)(ex->field); return #field; } } while (0)
    CHK(midi_song_data_off_v,   midi_song_data_off);
    CHK(midi_song_data_seg_v,   midi_song_data_seg);
    CHK(midi_seq_step_active_v, midi_seq_step_active);
    CHK(midi_aux_ptr_off_v,     midi_aux_ptr_off);
    CHK(midi_aux_ptr_seg_v,     midi_aux_ptr_seg);
    CHK(midi_data_seg_v,        midi_data_seg);
    CHK(midi_division_v,        midi_division);
    CHK(midi_tempo_lo_v,        midi_tempo_lo);
    CHK(midi_tempo_hi_v,        midi_tempo_hi);
    CHK(midi_track_count_v,     midi_track_count);
    #undef CHK
    if (!check_tbl) return NULL;
    if (memcmp(midi_track_ptr_table, ex->track_tables + 0, 64) != 0) {
        *got = 0; *want = 1;   /* table mismatch — see divergence via the fn name printed */
        return "midi_track_ptr_table";
    }
    if (memcmp(midi_track_time_table, ex->track_tables + 64, 64) != 0) {
        *got = 0; *want = 1;
        return "midi_track_time_table";
    }
    return NULL;
}

/* ── (B) PORT-WRITE-SEQUENCE COMPARATOR — host OUT-capture vs the record's OUTs ───
 *  Identical mechanism to tools/sound_ctest.c's comparator B (generic — no MIDI-
 *  specific fields involved): diffs the host out() capture buffer against the
 *  record's captured OUT events, in order. IN events are not compared here — they
 *  are the inputs the host in() shim replays. */
static const char *cmp_ports(const record_t *r, long *got, long *want)
{
    static char buf[48];
    int wi = 0;
    int hi;
    int n_out = 0, k;
    for (k = 0; k < (int)r->n_io; k++)
        if (r->io[k].dir == 0) n_out++;
    if (out_cap_n != n_out) {
        *got = out_cap_n; *want = n_out;
        return "out_count";
    }
    for (hi = 0; hi < out_cap_n; hi++) {
        while (wi < (int)r->n_io && r->io[wi].dir != 0) wi++;
        if (wi >= (int)r->n_io) { *got = hi; *want = n_out; return "out_overrun"; }
        if (out_cap[hi].port != r->io[wi].port) {
            *got = out_cap[hi].port; *want = r->io[wi].port;
            snprintf(buf, sizeof(buf), "out[%d].port", hi);
            return buf;
        }
        if (out_cap[hi].val != r->io[wi].value) {
            *got = out_cap[hi].val; *want = r->io[wi].value;
            snprintf(buf, sizeof(buf), "out[%d].val@0x%03x", hi, r->io[wi].port);
            return buf;
        }
        if (out_cap[hi].size != r->io[wi].size) {
            *got = out_cap[hi].size; *want = r->io[wi].size;
            snprintf(buf, sizeof(buf), "out[%d].size@0x%03x", hi, r->io[wi].port);
            return buf;
        }
        wi++;
    }
    return NULL;
}

/* prime the in() replay queue + clear the out() capture for an L4 record. */
static void prime_ports(const record_t *r)
{
    int k;
    out_cap_n = 0;
    in_queue_n = 0; in_queue_i = 0;
    for (k = 0; k < (int)r->n_io; k++) {
        if (r->io[k].dir == 1 && in_queue_n < IO_CAP_MAX)
            in_queue[in_queue_n++] = r->io[k].value;
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  HOST definitions of the cross-module globals src/sound.c externs (owned by
 *  player.c; the harness does NOT link player.obj) — verbatim from
 *  tools/sound_ctest.c. midi.c introduces no ADDITIONAL cross-module requirement
 *  of its own (it is a globals-only skeleton with no function bodies).
 * ════════════════════════════════════════════════════════════════════════════ */
s16 sound_device_state;     /* player.c 0x689c */
u8  p1_pending_action;      /* player.c 0x7924 */
u8  p1_contact_code;        /* player.c 0x8551 */
u8  tile_below_player;      /* player.c 0x79b9 */
u8  prev_game_mode;         /* player.c 0x8552 */

/* ── HOST stubs for the game_stubs.c carve-outs both TUs' compiled bodies
 *  reference — verbatim from tools/sound_ctest.c ────────────────────────────────
 *  seq_set_channel_param / midi_emit_voice_msg_w3 / opl_event_note_on are ALSO
 *  prototyped in src/midi.h (same call-tree map) but midi.c does not define them
 *  (Task C1 report) — these are the only bodies satisfying either TU's reference,
 *  no clash. */
void seq_set_channel_param(void)  {}
void midi_emit_voice_msg_w3(void) {}
void opl_event_note_on(void)      {}
void FUN_1000_6183(void)         {}
void maybe_opl2_detect_chip(void) {}
void opl2_reset_all_regs(void)    {}
void pit_set_counter0(void)       {}
void p1_try_trigger_pending_action(void) {}   /* 1000:654e — player.c (host no-op) */

/* ════════════════════════════════════════════════════════════════════════════
 *  PORTED REGISTRY — engine seg-1000 offset -> reconstructed-C callable.
 *
 *  Task C3 BASELINE: EMPTY. No entry references any midi_* / seq_* / opl_event_* body
 *  yet — none exist (src/midi.c is a globals-only skeleton; Phase D/E ports them).
 *  Referencing an unported function's C name here would fail to LINK, not just
 *  fail a comparator — so the registry starts genuinely empty, not all-NULL.
 *
 *  ─────────────────────────────────────────────────────────────────────────────
 *  >>> Task D/E: ADD ONE ENTRY HERE PER FUNCTION AS ITS BODY LANDS IN src/midi.c <<<
 *  Each entry is { off, name, is_l4, fn, check_tbl }:
 *    off        — the seg-1000 engine offset (matches a MIDI_SNAP record's fn_off;
 *                 see the 21-entry FN_NAMES table in tools/midi_oracle.py / the C2
 *                 report's "Per-function record counts" table).
 *    name       — the reconstructed C function's name (for FAIL log lines).
 *    is_l4      — 1 for the 10 L4 emitter/driver fns (selects comparator B,
 *                 cmp_ports; requires prime_ports() before the call) — the 9 OPL2/
 *                 emission fns + midi_install_tempo_timer; 0 for the 12 parser/
 *                 sequencer fns (selects comparator A, cmp_semantic).
 *    fn         — a zero-arg wrapper (mirrors tools/sound_ctest.c's call_* helpers)
 *                 that recovers any needed call arguments from g_cur_rec (register-
 *                 entry targets: AL/BX/SI per the ABI notes in the C2 report) and
 *                 invokes the real reconstructed function.
 *    check_tbl  — 1 ONLY for the table-filling fns (midi_parse_file /
 *                 midi_init_track_table) whose CONTRACT is the ptr/time table
 *                 install; 0 otherwise (see cmp_semantic's doc comment above).
 *  Example (once midi_read_varlen is ported):
 *    { 0x8891, "midi_read_varlen", 0, call_midi_read_varlen, 0 },
 *  ───────────────────────────────────────────────────────────────────────────── */
typedef struct { u16 off; const char *name; int is_l4; void (*fn)(void);
                 int check_tbl; } ported_t;

static const ported_t PORTED[] = {
    /* empty — see the baseline note above */
};
#define PORTED_N (sizeof(PORTED) / sizeof(PORTED[0]))

static const ported_t *ported_lookup(u16 off)
{
    unsigned i;
    for (i = 0; i < PORTED_N; i++)
        if (PORTED[i].off == off) return &PORTED[i];
    return NULL;
}

typedef struct { long pass, fail, unported, port_checked; } stats_t;

/* ── PER-FUNCTION semantic-state + port-write-sequence differential ────────────── */
static int run_per_function(record_t *recs, long nrec, const char *scname,
                            stats_t *st)
{
    long i;
    int  scen_fail = 0;
    long printed = 0;
    (void)scname;
    for (i = 0; i < nrec; i++) {
        record_t *r = &recs[i];
        const ported_t *p = ported_lookup(r->fn_off);

        if (p == NULL || p->fn == NULL) {
            /* No reconstructed body yet (baseline: PORTED[] is empty, so this is
               ALWAYS taken). Never references the (nonexistent) symbol; UNPORTED is
               not a crash, not a hard failure. */
            st->unported++;
            continue;
        }

        /* ── PORTED: seed entry, (prime ports if L4), call, assert ──────────────── */
        {
            const char *bad = NULL; long got = 0, want = 0;
            g_cur_rec = r;            /* publish current record for arg-recovery wrappers */
            seed_globals(&r->ent);
            if (p->is_l4) {
                prime_ports(r);
                p->fn();
                bad = cmp_ports(r, &got, &want);
                if (bad == NULL) st->port_checked++;
            } else {
                p->fn();
                bad = cmp_semantic(&r->ex, &got, &want, p->check_tbl);
            }
            if (bad == NULL) {
                st->pass++;
            } else {
                st->fail++; scen_fail = 1;
                if (printed++ < 8)
                    printf("    FAIL [%s #%ld] %s field %s: got %#lx want %#lx\n",
                           scname, i, p->name, bad, got, want);
            }
        }
    }
    return !scen_fail;
}

/* ════════════════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1]
                                  : "local/build/render/midi_trace.bin";
    FILE *f = fopen(path, "rb");
    long sz; u8 *b; u32 o; u16 ver, nsc, nfn; unsigned s;
    stats_t st = { 0, 0, 0, 0 };
    long n_records = 0, n_io_total = 0;
    int hard_fail = 0;

    {   /* perturbation knob: MIDI_PERTURB=N corrupts the Nth emitted OUT (dormant
         * until an L4 PORTED entry exists to perturb — see the PORTED[] doc comment). */
        const char *pe = getenv("MIDI_PERTURB");
        if (pe && *pe) {
            g_perturb_idx = atoi(pe);
            printf("midi_ctest: PERTURBATION active — corrupting emitted OUT #%d "
                   "(the port-write gate MUST report a FAIL once an L4 fn is PORTED)\n",
                   g_perturb_idx);
        }
    }
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz);
    if (!b || fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
    fclose(f);

    if (sz < 14 || memcmp(b, "MIDTRC01", 8) != 0) {
        fprintf(stderr, "bad magic (want MIDTRC01)\n"); return 2;
    }
    ver = rd16(b + 8); nsc = rd16(b + 10);
    if (ver != TRACE_VER) {
        fprintf(stderr, "unsupported version %u (want %u)\n", ver, TRACE_VER); return 2;
    }
    o = 12;
    nfn = rd16(b + o); o += 2;
    /* skip the fn-name string table (we key on fn_off, not the name index). */
    { u16 k; for (k = 0; k < nfn; k++) { u8 ln = b[o]; o += 1 + ln; } }

    printf("midi_ctest: replay harness over %s\n", path);
    printf("  trace: MIDTRC01 v%u, %u scenarios, %u fn-names (SNAP=%d B)\n",
           ver, nsc, nfn, SNAP_SIZE);
    printf("  src/midi.c: BASELINE (globals-only skeleton, Task C1) — PORTED[] is "
           "empty; every record reports UNPORTED. src/sound.c's OPL2/MPU/PIT drivers "
           "are linked in (dual-#include) but not yet exercised through any MIDI-side "
           "entry. Expected FAIL=0.\n");

    for (s = 0; s < nsc; s++) {
        u8 sid, name_len;
        char scname[64];
        u32 nrec, k;
        record_t *recs;
        stats_t sst = { 0, 0, 0, 0 };
        int per_ok;

        sid = b[o]; o += 1;
        name_len = b[o]; o += 1;
        { unsigned n = name_len < 63 ? name_len : 63;
          memcpy(scname, b + o, n); scname[n] = 0; o += name_len; }
        o += 1;   /* per-scenario seed byte — always 0xFF for MIDI, unused */
        nrec = rd32(b + o); o += 4;

        recs = malloc(sizeof(record_t) * (nrec ? nrec : 1));
        for (k = 0; k < nrec; k++) {
            record_t *r = &recs[k];
            unsigned j;
            r->fn_off = rd16(b + o); o += 2;
            r->fn_name_idx = rd16(b + o); o += 2;
            parse_snap(b + o, &r->ent); o += SNAP_SIZE;
            parse_snap(b + o, &r->ex);  o += SNAP_SIZE;
            r->n_io = rd16(b + o); o += 2;
            r->io = r->n_io ? malloc(sizeof(io_t) * r->n_io) : NULL;
            for (j = 0; j < r->n_io; j++) {
                r->io[j].dir   = b[o];        o += 1;
                r->io[j].port  = rd16(b + o); o += 2;
                r->io[j].size  = b[o];        o += 1;
                r->io[j].value = rd16(b + o); o += 2;
            }
            n_io_total += r->n_io;
            n_records++;
        }

        printf("\n== scenario %u: %s (%lu records) ==\n",
               sid, scname, (unsigned long)nrec);

        per_ok = run_per_function(recs, (long)nrec, scname, &sst);
        printf("  per-fn: PASS=%ld  FAIL=%ld  UNPORTED=%ld  PORT_CHECKED=%ld\n",
               sst.pass, sst.fail, sst.unported, sst.port_checked);
        if (!per_ok) hard_fail = 1;

        st.pass += sst.pass; st.fail += sst.fail; st.unported += sst.unported;
        st.port_checked += sst.port_checked;
        for (k = 0; k < nrec; k++) free(recs[k].io);
        free(recs);
    }

    if (o != (u32)sz) {
        fprintf(stderr, "WARNING: parsed %lu of %ld bytes (trailing data)\n",
                (unsigned long)o, sz);
    }

    printf("\n=== TOTAL per-fn: PASS=%ld  FAIL=%ld  UNPORTED=%ld  PORT_CHECKED=%ld "
           "(records=%ld, port-I/O events=%ld) ===\n",
           st.pass, st.fail, st.unported, st.port_checked, n_records, n_io_total);
    if (hard_fail || st.fail != 0) {
        printf("FAIL: %ld per-function differential failure(s) on PORTED fns\n",
               st.fail);
        free(b);
        return 1;
    }
    printf("PASS: FAIL=0.  %ld records UNPORTED (baseline: no midi_*/seq_*/opl_event_* "
           "body ported yet); %ld PORTED records matched (%ld via the port-write-"
           "sequence gate).\n",
           st.unported, st.pass, st.port_checked);
    free(b);
    return 0;
}
