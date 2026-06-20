/* Host REPLAY HARNESS for src/sound.c — Phase-6 Tasks 2–6.
 *
 * Compiles the REAL sound port (src/sound.c) on the host (Open Watcom 16-bit
 * environment shimmed out: __far/__huge/__cdecl16near erased, exact-width typedefs,
 * BUMPY_H so sound.h does not pull <dos.h>), then validates the reconstructed sound
 * functions against the Phase-6 T1 capture local/build/render/sound_trace.bin
 * (magic "SNDTRC01", version 1 — layout frozen in tools/sound_oracle.py's header
 * §"TRACE LAYOUT" and local/build/sound_model.md).
 *
 * SKELETON STATE (Phase-6 T2): src/sound.c defines ONLY the sound globals — NO
 * function bodies (they remain stubbed in game_stubs.c).  So the PORTED[] registry
 * below holds NULL for every sound fn: EVERY record is reported UNPORTED, the
 * comparators are dormant, FAIL=0, no crash.  The L1–L4 ports (T3–T6) fill PORTED[]
 * entries with their C names, at which point the matching comparator runs per record.
 *
 * ── TWO COMPARATOR FLAVORS (both wired now; dormant until PORTED[] is filled) ─────
 *   (A) SEMANTIC-STATE DIFFERENTIAL (L1 dispatch / L2 device / L3 tone — T3–T5).
 *       For each record of a PORTED, host-callable fn: seed the reconstructed sound
 *       globals from the record's ENTRY SND_SNAP, call the fn by its C name, then
 *       assert the sound-global SNAP bytes (sound_device_state, snddrv_mode,
 *       sound_active_device_mask, sound_init_state, sound_mode, the LUT-index inputs,
 *       the 10-word tone param frame, the timer-cb far ptr) == the EXIT SNAP.  Prints
 *       PASS/FAIL with first-divergence (which SNAP field).
 *
 *   (B) PORT-WRITE-SEQUENCE DIFFERENTIAL (L4 hardware drivers — T6).  L4 driver
 *       records carry a per-call I/O sequence (dir/port/size/value, in execution
 *       order — OUT events AND the engine's IN polls).  Before calling a PORTED L4
 *       fn, the harness primes the IN-replay queue from the record's recorded IN
 *       events and clears the OUT-capture buffer.  The reconstructed driver's
 *       out(port,val) APPENDS to the OUT-capture buffer; its in(port) REPLAYS the
 *       next recorded IN value (NOT a live hardware read — the engine's drivers branch
 *       on IN, e.g. polling 0x331 DSR bit 0x40, so the reconstruction must see the
 *       EXACT inputs the engine saw).  After the call, the comparator diffs the host
 *       OUT-capture vs the record's captured OUT events, first-divergence printed.
 *
 *   GRACEFUL UNPORTED DEGRADATION.  A record whose fn is not host-callable (its
 *     PORTED entry holds NULL, or it is not in the registry) is marked UNPORTED and
 *     SKIPPED: the harness never references the (absent) symbol and never calls into
 *     it.  UNPORTED is NOT a crash and NOT a hard failure.  In T2 every record is
 *     UNPORTED by design.
 *
 * Build/run (also wrapped by tools/validate_sound.sh):
 *     cc -O2 -Wall -Werror -o /tmp/sound_ctest tools/sound_ctest.c && \
 *       /tmp/sound_ctest local/build/render/sound_trace.bin
 * Exit 0 iff the harness parses the trace, runs, and the differential has ZERO
 * failures on PORTED records (UNPORTED records never fail).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── shim the Watcom 16-bit environment for host compilation ─────────────────── */
#define BUMPY_H            /* sound.h's #include "bumpy.h" becomes a no-op */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge
#define __cdecl16near

/* ── HOST PORT-I/O SHIMS (the genuinely new Phase-6 harness piece) ────────────────
 *  When the L4 drivers port (T6), the included src/sound.c will issue out(port,val)
 *  and in(port) (the Watcom port intrinsics).  On the host we provide our own:
 *    out(port,val)  APPENDS (port,val) to a capture buffer the L4 comparator diffs
 *                   against the trace's recorded OUT events.
 *    in(port)       REPLAYS the next recorded IN value for the CURRENT record (in
 *                   order) — it does NOT read live hardware.  The engine's drivers
 *                   branch on IN (e.g. the MPU-401 0x331 DSR poll), so the
 *                   reconstructed driver must see the EXACT inputs the engine saw,
 *                   captured in the trace's I/O sequence.  When the replay queue is
 *                   exhausted (or empty, e.g. T2 where nothing calls in()), it falls
 *                   back to 0xFF so a stray read never deadlocks.
 *  The intrinsic names vary (`outp`/`inp`, `outpw`/`inpw`); we define the common
 *  byte+word forms so whichever the port uses resolves to these shims. */
typedef struct { u16 port; u16 val; } io_evt_t;
#define IO_CAP_MAX 4096
static io_evt_t out_cap[IO_CAP_MAX];     /* host OUT capture (port,val), in order   */
static int      out_cap_n = 0;
static u16      in_queue[IO_CAP_MAX];     /* recorded IN values for the cur record   */
static int      in_queue_n = 0, in_queue_i = 0;

static void host_out(u16 port, u16 val)
{
    if (out_cap_n < IO_CAP_MAX) {
        out_cap[out_cap_n].port = port;
        out_cap[out_cap_n].val  = val;
        out_cap_n++;
    }
}
static u16 host_in(u16 port)
{
    (void)port;
    if (in_queue_i < in_queue_n) return in_queue[in_queue_i++];
    return 0xFF;   /* replay exhausted/empty — deterministic fallback */
}

/* The port intrinsics the future L4 port may use, all routed to the shims above.
 *  Marked HOST_UNUSED: in the T2 skeleton no sound body is ported, so nothing calls
 *  them yet (cc -Werror would reject the otherwise-unused statics).  The attribute is
 *  benign once the L4 port (T6) starts issuing out()/in() from src/sound.c. */
#ifdef __GNUC__
#  define HOST_UNUSED __attribute__((unused))
#else
#  define HOST_UNUSED
#endif
static HOST_UNUSED int      outp (u16 port, int val) { host_out(port, (u16)val); return val; }
static HOST_UNUSED unsigned inp  (u16 port)          { return host_in(port); }
static HOST_UNUSED int      outpw(u16 port, int val) { host_out(port, (u16)val); return val; }
static HOST_UNUSED unsigned inpw (u16 port)          { return host_in(port); }
static HOST_UNUSED void     out  (u16 port, u16 val) { host_out(port, val); }
static HOST_UNUSED u16      in   (u16 port)          { return host_in(port); }

#include "../src/sound.c"

/* ════════════════════════════════════════════════════════════════════════════
 *  Trace format (frozen — see tools/sound_oracle.py header §"TRACE LAYOUT").
 *
 *  Header:  magic[8]="SNDTRC01", u16 version(=1), u16 n_scenarios,
 *           u16 n_fn_names, then per name {u8 len, len bytes ascii}.
 *  Per scenario: u8 id, u8 name_len, name bytes, u8 seed_device, u32 n_records,
 *                then n_records records.
 *  Per record: u16 fn_off, u16 fn_name_idx, SND_SNAP entry, SND_SNAP exit,
 *              u16 n_io, then n_io * { u8 dir(0=OUT,1=IN), u16 port, u8 size, u16 value }.
 *  SND_SNAP (struct "<hHHHHBBBB" + "H"*10 + "HH"):
 *    s16 sound_device_state, u16 snddrv_mode, u16 sound_active_mask,
 *    u16 sound_init_state, u16 sound_mode,
 *    u8 p1_pending_action, u8 prev_game_mode, u8 p1_contact_code, u8 pad,
 *    u16 param_frame[10], u16 timer_cb_off, u16 timer_cb_seg.
 * ════════════════════════════════════════════════════════════════════════════ */
#define PF_WORDS   10
/* SND_SNAP byte size: 5*2 + 4*1 + 10*2 + 2*2 = 10 + 4 + 20 + 4 = 38. */
#define SNAP_SIZE  (5 * 2 + 4 + PF_WORDS * 2 + 2 * 2)

typedef struct {
    s16 sound_device_state;
    u16 snddrv_mode;
    u16 sound_active_mask;
    u16 sound_init_state;
    u16 sound_mode;
    u8  p1_pending_action;
    u8  prev_game_mode;
    u8  p1_contact_code;
    u8  pad;
    u16 param_frame[PF_WORDS];
    u16 timer_cb_off;
    u16 timer_cb_seg;
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
    unsigned i;
    u32 o = 0;
    s->sound_device_state = rds16(p + o); o += 2;
    s->snddrv_mode        = rd16(p + o);  o += 2;
    s->sound_active_mask  = rd16(p + o);  o += 2;
    s->sound_init_state   = rd16(p + o);  o += 2;
    s->sound_mode         = rd16(p + o);  o += 2;
    s->p1_pending_action  = p[o++];
    s->prev_game_mode     = p[o++];
    s->p1_contact_code    = p[o++];
    s->pad                = p[o++];
    for (i = 0; i < PF_WORDS; i++) { s->param_frame[i] = rd16(p + o); o += 2; }
    s->timer_cb_off = rd16(p + o); o += 2;
    s->timer_cb_seg = rd16(p + o); o += 2;
}

/* ── ENTRY SND_SNAP -> reconstructed sound globals ──────────────────────────────
 *  Seed the module's sound state from the captured ENTRY snap, then (when a fn is
 *  PORTED) the comparator diffs the post-call state against the EXIT snap.  The
 *  cross-module LUT-index inputs (sound_device_state + p1_pending_action /
 *  p1_contact_code / tile_below_player / prev_game_mode) are owned by player.c; the
 *  harness supplies its own host definitions of them (see below) and seeds those too,
 *  mirroring the items_ctest / anim_chan_ctest convention for cross-module globals
 *  the included TU references.  (Skeleton T2: no fn runs, so seeding is exercised only
 *  structurally — kept here so T3+ land without harness edits.) */
static void seed_globals(const snap_t *s)
{
    unsigned i;
    sound_device_state       = s->sound_device_state;
    snddrv_mode              = s->snddrv_mode;
    sound_active_device_mask = s->sound_active_mask;
    sound_init_state         = s->sound_init_state;
    sound_mode               = s->sound_mode;
    p1_pending_action        = s->p1_pending_action;
    prev_game_mode           = s->prev_game_mode;
    p1_contact_code          = s->p1_contact_code;
    for (i = 0; i < PF_WORDS; i++) snd_param_frame[i] = s->param_frame[i];
    snd_timer_cb_off = s->timer_cb_off;
    snd_timer_cb_seg = s->timer_cb_seg;
}

/* ── CODE-segment load-base relocation (the project's deliberate _seg convention) ──
 *  The far timer-callback SEGMENT is a relocated word: src/sound.c stores the FAITHFUL
 *  Ghidra image-base literal 0x1000 (decomp `timer_callback_seg = 0x1000`), but the MZ
 *  relocation fixes that segment immediate to the RUNTIME CODE base at load.  The Phase-6
 *  T1 oracle loaded the image at base = PSP_SEG+0x10 = 0x110 (tools/sound_oracle.py
 *  CODE_LIN), so the trace captured timer_cb_seg = 0x110.  Both denote "the program's
 *  CODE segment"; the comparator maps the host's image-base literal to the trace's
 *  runtime base.  The OFFSET is NOT relocated (faithful literal 0x9631/0x96c4/0x95b5
 *  matches the trace verbatim). */
#define CODE_IMAGE_SEG   0x1000   /* Ghidra seg-1000 image base (the faithful literal) */
#define CODE_RUNTIME_SEG 0x110    /* oracle runtime CODE base (what the trace captured) */
static u16 seg_to_trace(u16 seg)
{
    return (seg == CODE_IMAGE_SEG) ? CODE_RUNTIME_SEG : seg;
}

/* ── (A) SEMANTIC-STATE COMPARATOR — read live sound globals vs the EXIT snap ─────
 *  Returns NULL if every captured sound-global field matches the EXIT snap, else a
 *  short field name of the first divergence; got/want filled. */
static const char *cmp_semantic(const snap_t *ex, long *got, long *want)
{
    static char buf[32];
    unsigned i;
    #define CHK(field, live) do { if ((long)(live) != (long)(ex->field)) { \
        *got = (long)(live); *want = (long)(ex->field); return #field; } } while (0)
    CHK(sound_device_state,  sound_device_state);
    CHK(snddrv_mode,         snddrv_mode);
    CHK(sound_active_mask,   sound_active_device_mask);
    CHK(sound_init_state,    sound_init_state);
    CHK(sound_mode,          sound_mode);
    CHK(p1_pending_action,   p1_pending_action);
    CHK(prev_game_mode,      prev_game_mode);
    CHK(p1_contact_code,     p1_contact_code);
    CHK(timer_cb_off,        snd_timer_cb_off);
    /* timer_cb_seg: src/sound.c stores the faithful image-base literal (0x1000); the
     *  trace captured the relocated runtime CODE base (0x110).  Map before comparing
     *  (the project's _seg load-base relocation — see seg_to_trace). */
    CHK(timer_cb_seg,        seg_to_trace(snd_timer_cb_seg));
    #undef CHK
    for (i = 0; i < PF_WORDS; i++) {
        if (snd_param_frame[i] != ex->param_frame[i]) {
            *got = snd_param_frame[i]; *want = ex->param_frame[i];
            snprintf(buf, sizeof(buf), "param_frame[%u]", i);
            return buf;
        }
    }
    return NULL;
}

/* ── (B) PORT-WRITE-SEQUENCE COMPARATOR — host OUT-capture vs the record's OUTs ───
 *  Diffs the host out() capture buffer against the record's captured OUT events (in
 *  order).  IN events are not compared here — they are the inputs the host in() shim
 *  replays; the validated output is the OUT sequence the driver produces given those
 *  inputs.  Returns NULL on match, else a short tag of the first divergence. */
static const char *cmp_ports(const record_t *r, long *got, long *want)
{
    static char buf[48];
    int wi = 0;   /* index into the record's OUT events */
    int hi;       /* index into the host OUT capture     */
    /* count recorded OUT events */
    int n_out = 0, k;
    for (k = 0; k < (int)r->n_io; k++)
        if (r->io[k].dir == 0) n_out++;
    if (out_cap_n != n_out) {
        *got = out_cap_n; *want = n_out;
        return "out_count";
    }
    for (hi = 0; hi < out_cap_n; hi++) {
        /* advance wi to the next recorded OUT */
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
 *  player.c; the harness does NOT link player.obj).  Mirrors the items_ctest /
 *  anim_chan_ctest convention of supplying the cross-module globals the included TU
 *  references.  sound.c reads these as the L1 dispatch LUT-index inputs.
 * ════════════════════════════════════════════════════════════════════════════ */
s16 sound_device_state;     /* player.c 0x689c */
u8  p1_pending_action;      /* player.c 0x7924 */
u8  p1_contact_code;        /* player.c 0x8551 */
u8  tile_below_player;      /* player.c 0x79b9 */
u8  prev_game_mode;         /* player.c 0x8552 */

/* ── HOST stubs for the T3 still-stubbed sound callees src/sound.c references ──────
 *  In BUMPY.EXE these resolve to the faithful-signature stubs in game_stubs.c; the
 *  harness does NOT link game_stubs.c (it #includes only src/sound.c), so it supplies
 *  its own no-op host stubs — the same convention items_ctest uses for play_sound etc.
 *  None affects the validated semantic state (device-guarded dispatch + tone frame). */
void record_min_status_code(u16 status)        { (void)status; }   /* 1000:945b */
void FUN_1000_7df9(void)                        {}                  /* 1000:7df9 */
void speaker_gate_reset(void)                   {}                  /* 1000:9440 */
void FUN_1000_8a07(u8 lo, u8 hi)               { (void)lo; (void)hi; }  /* 1000:8a07 */

/* ════════════════════════════════════════════════════════════════════════════
 *  Phase-6 T3 — CALL-ARGUMENT RECOVERY for the effect→frame pipeline.
 *
 *  WHY recovery is needed.  The trace records a sound-fn CALL as fn_off + ENTRY snap
 *  + EXIT snap + port-I/O — it does NOT serialize the call's ARGUMENTS (the eid handed
 *  to play_sound, or the tone-param tuple handed to schedule_timer_callback_*).  But to
 *  re-run a PORTED pipeline fn on the host and diff its EXIT frame, the harness must
 *  call it with the SAME arguments the engine used.  We recover them from the record:
 *
 *   (1) schedule_timer_callback_a/b/c — the fn OVERWRITES the param frame from its args
 *       via a fixed slot map (decompiled at 1000:9488/9502/956d).  So the args are read
 *       back from the EXIT frame's CANONICAL slots (the inverse of the fill map), then
 *       the reconstructed fn is re-run from the seeded ENTRY frame and ALL 10 slots +
 *       the cb ptr are asserted == the EXIT snap.  This is a genuine differential, NOT
 *       a fit: recovery reads the canonical source slots ([0],[1],[2|7],[3],[4],[5|8],
 *       [6]); the comparator then checks EVERY slot, so a wrong-slot / missing dual-write
 *       / wrong-cb bug in the port still diverges (the port writes the live frame, the
 *       harness recovery + comparator are independent of the port).
 *
 *   (2) play_sound / play_sound_effect — recover the effect id by SIMULATING the 21-case
 *       switch SPEC (an independent data table here, EFFECT_SPEC[]) against the record's
 *       ENTRY frame for every candidate eid and picking the eid whose simulated EXIT
 *       frame == the record's EXIT frame.  The real reconstructed play_sound(eid) is then
 *       called and its live frame diffed vs the EXIT snap — so the port must reproduce
 *       what the independent spec predicts (a real differential of the switch + the
 *       device guard + the scheduler chain).  Device-guard paths where the frame does not
 *       change (mute: device==-0x8000; OPL sample: device==4) are detected as exit==entry
 *       and validated as the no-op the engine captured (any eid; the port leaves the frame
 *       untouched, matching).
 *
 *  The current record is published in g_cur_rec so the zero-arg wrapper the registry
 *  needs can read back the recovered args.  Recovery + spec live ONLY in the harness;
 *  src/sound.c is never consulted for them. */
static const record_t *g_cur_rec = NULL;

/* EFFECT_SPEC[eid] — the play_sound_effect switch as DATA (independent of src/sound.c).
 *  submit: 'a' => schedule_timer_callback_a(2,arg2,arg3,1,uVar1,uVar2,uVar3,uVar4),
 *          'b' => schedule_timer_callback_b(2,p2,p3,p4,p5,p6).
 *  For 'a': frame=[uVar2',arg3,arg4(=1),...] per the 9488 fill; encoded as the literal
 *  arg tuple the decomp passes.  eid 0 + any eid not below => no submit (frame unchanged). */
typedef struct {
    char submit;            /* 'a', 'b', or 0 (no submit) */
    u16  a2, a3, a4, a5, a6, a7, a8;  /* schedule_timer_callback_a(2,a2,a3,a4,a5,a6,a7,a8) */
    u16  b2, b3, b4, b5, b6;          /* schedule_timer_callback_b(2,b2,b3,b4,b5,b6)        */
} effect_spec_t;

/* From play_sound_effect (1000:6e30).  'a' tuple = (tone_arg2, tone_arg3, 1, uVar1,
 *  uVar2, uVar3, uVar4); cases 1/9/0xc/0x10/0x15 take the LAB_70d6 tail (uVar3=1,
 *  tone_arg2=0x1e). 'b' cases 0x0b/0x11/0x12 submit via schedule_timer_callback_b. */
static const effect_spec_t EFFECT_SPEC[0x16] = {
    /* 0x00 */ { 0 },
    /* 0x01 */ { 'a', 0x1e, 1000,  1, 10,     0x1c2, 1, 1,        0,0,0,0,0 },
    /* 0x02 */ { 'a', 0x28, 800,   1, 0xfff6, 0x1c2, 1, 1,        0,0,0,0,0 },
    /* 0x03 */ { 'a', 400,  0x1b8, 1, 0xffff, 499,   4, 0xffff,   0,0,0,0,0 },
    /* 0x04 */ { 'a', 0x5a, 0xdc,  1, 0xffff, 100,   1, 4,        0,0,0,0,0 },
    /* 0x05 */ { 'a', 0x19, 1000,  1, 10,     0x1b8, 1, 2,        0,0,0,0,0 },
    /* 0x06 */ { 'a', 0x14, 0x44c, 1, 10,     0x1b8, 2, 5,        0,0,0,0,0 },
    /* 0x07 */ { 'a', 0xf,  0x4b0, 1, 10,     0x1b8, 1, 3,        0,0,0,0,0 },
    /* 0x08 */ { 'a', 0x28, 0xdc,  1, 0xfffb, 100,   1, 5,        0,0,0,0,0 },
    /* 0x09 */ { 'a', 0x1e, 0x32,  1, 0x14,   0x1c2, 1, 1,        0,0,0,0,0 },
    /* 0x0a */ { 'a', 0xf,  200,   1, 0x32,   0x15d, 1, 10,       0,0,0,0,0 },
    /* 0x0b */ { 'b', 0,0,0,0,0,0,0,                              0x28, 0x14, 499, 1, 0xfffc },
    /* 0x0c */ { 'a', 0x1e, 0x4b0, 1, 10,     0x1a4, 1, 2,        0,0,0,0,0 },
    /* 0x0d */ { 'a', 0x14, 200,   1, 0x32,   0x15d, 2, 0xf,      0,0,0,0,0 },
    /* 0x0e */ { 'a', 0x32, 10,    1, 4,      200,   10,0,        0,0,0,0,0 },
    /* 0x0f */ { 'a', 400,  300,   1, 2,      100,   2, 1,        0,0,0,0,0 },
    /* 0x10 */ { 'a', 0x1e, 0x4b0, 1, 10,     0x1a4, 1, 2,        0,0,0,0,0 },
    /* 0x11 */ { 'b', 0,0,0,0,0,0,0,                              0x28, 0x14, 499, 1, 0xfffc },
    /* 0x12 */ { 'b', 0,0,0,0,0,0,0,                              0x50, 0x1e, 499, 2, 0xfffc },
    /* 0x13 */ { 'a', 800,  300,   1, 1,      100,   2, 1,        0,0,0,0,0 },
    /* 0x14 */ { 'a', 0x32, 10,    1, 4,      200,   10,0,        0,0,0,0,0 },
    /* 0x15 */ { 'a', 0x1e, 600,   1, 10,     0x1c2, 1, 1,        0,0,0,0,0 },
};

/* Apply schedule_timer_callback_a's frame fill (1000:9488 map) to f[10]. */
static void spec_fill_a(u16 f[PF_WORDS], u16 a2, u16 a3, u16 a4, u16 a5,
                        u16 a6, u16 a7, u16 a8, u16 *cb_off, u16 *cb_seg)
{
    f[0] = a2; f[1] = a3; f[7] = a4; f[2] = a4; f[3] = a5;
    f[4] = a6; f[5] = a7; f[8] = a7; f[6] = a8; f[9] = 0xf;
    *cb_off = 0x9631; *cb_seg = CODE_RUNTIME_SEG;   /* trace-base (relocated) for matching */
}
/* schedule_timer_callback_b fill (1000:9502 map). */
static void spec_fill_b(u16 f[PF_WORDS], u16 b2, u16 b3, u16 b4, u16 b5, u16 b6,
                        u16 *cb_off, u16 *cb_seg)
{
    f[0] = b2; f[1] = b3; f[4] = b4; f[5] = b5; f[8] = b5; f[6] = b6; f[9] = 0xf;
    *cb_off = 0x96c4; *cb_seg = CODE_RUNTIME_SEG;   /* trace-base (relocated) for matching */
}

/* Simulate play_sound_effect(eid) on entry frame -> exit frame + cb (spec, host-side). */
static void spec_effect(u8 eid, s16 device, const u16 ein[PF_WORDS],
                        u16 efoff, u16 efseg,
                        u16 out[PF_WORDS], u16 *cb_off, u16 *cb_seg)
{
    int i;
    for (i = 0; i < PF_WORDS; i++) out[i] = ein[i];
    *cb_off = efoff; *cb_seg = efseg;
    if (device == 4) return;                 /* OPL sample path: frame unchanged */
    if (eid >= 0x16) return;                 /* default: no submit */
    {
        const effect_spec_t *s = &EFFECT_SPEC[eid];
        if (s->submit == 'a')
            spec_fill_a(out, s->a2, s->a3, s->a4, s->a5, s->a6, s->a7, s->a8, cb_off, cb_seg);
        else if (s->submit == 'b')
            spec_fill_b(out, s->b2, s->b3, s->b4, s->b5, s->b6, cb_off, cb_seg);
        /* submit==0: frame unchanged */
    }
}

/* Recover the effect id whose spec EXIT frame matches the record's EXIT snap. */
static int recover_eid(const record_t *r)
{
    u16 f[PF_WORDS], co, cs; int eid;
    for (eid = 0; eid < 0x16; eid++) {
        spec_effect((u8)eid, r->ent.sound_device_state, r->ent.param_frame,
                    r->ent.timer_cb_off, r->ent.timer_cb_seg, f, &co, &cs);
        if (memcmp(f, r->ex.param_frame, sizeof f) == 0 &&
            co == r->ex.timer_cb_off && cs == r->ex.timer_cb_seg)
            return eid;
    }
    return 0;   /* no spec eid matched (e.g. frame unchanged) — no-op call is correct */
}

/* ── zero-arg registry wrappers: recover args from g_cur_rec, call the real port ──── */
static void call_play_sound(void)
{
    play_sound((u8)recover_eid(g_cur_rec));
}
static void call_play_sound_effect(void)
{
    play_sound_effect((u8)recover_eid(g_cur_rec));
}
static void call_schedule_timer_callback_a(void)
{
    /* args from the EXIT frame's canonical source slots (inverse of the 9488 fill). */
    const u16 *e = g_cur_rec->ex.param_frame;
    schedule_timer_callback_a(2, e[0], e[1], e[2], e[3], e[4], e[5], e[6]);
}
static void call_schedule_timer_callback_b(void)
{
    const u16 *e = g_cur_rec->ex.param_frame;
    schedule_timer_callback_b(2, e[0], e[1], e[4], e[5], e[6]);
}
static void call_schedule_timer_callback_c(void)
{
    /* _c writes only slot [1] (+ the 0x0f marker + cb); recover param_2 from [1]. */
    const u16 *e = g_cur_rec->ex.param_frame;
    schedule_timer_callback_c(2, e[1]);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  PORTED REGISTRY — engine seg-1000 offset -> reconstructed-C callable.
 *
 *  Phase-6 T3: the effect→frame pipeline (play_sound / play_sound_effect +
 *  schedule_timer_callback_a/b/c) is PORTED in src/sound.c and wired below via the
 *  arg-recovery wrappers above.  The remaining sound fns stay NULL (UNPORTED) until
 *  their L2/L4/L5 ports (T4–T6) fill them.  The run loop dispatches each PORTED record
 *  through comparator (A) semantic-state (L1–L3) or (B) port-write-sequence (L4).
 *
 *  `is_l4` selects the port-write-sequence comparator (B); else semantic-state (A).
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct { u16 off; const char *name; int is_l4; void (*fn)(void); } ported_t;

static const ported_t PORTED[] = {
    /* L1 dispatch (semantic-state, T3) — PORTED in src/sound.c. */
    { 0x6e11, "play_sound",            0, call_play_sound },
    { 0x6e30, "play_sound_effect",     0, call_play_sound_effect },
    { 0x63be, "play_action_sound",     0, NULL },
    { 0x640c, "play_contact_sound",    0, NULL },
    { 0x6305, "play_exit_sound",       0, NULL },
    { 0x645d, "play_pickup_sound",     0, NULL },
    { 0x64c1, "play_event_sound_64c1", 0, NULL },
    { 0x647e, "play_state_sound_79b9", 0, NULL },
    /* L2 device (semantic-state, T4). */
    { 0x6de3, "sound_select_device",          0, NULL },
    { 0x88e5, "snddrv_init",                  0, NULL },
    { 0x891e, "select_sound_device_from_mask",0, NULL },
    { 0x85b5, "snddrv_dispatch_a",            0, NULL },
    { 0x85db, "snddrv_dispatch_b",            0, NULL },
    { 0x8600, "snddrv_dispatch_c",            0, NULL },
    { 0x8626, "snddrv_dispatch_d",            0, NULL },
    { 0x872e, "snd_busy_delay",               0, NULL },
    /* L3 tone (semantic-state) — schedule_timer_callback_a/b/c PORTED (T3). */
    { 0x9488, "schedule_timer_callback_a",    0, call_schedule_timer_callback_a },
    { 0x9502, "schedule_timer_callback_b",    0, call_schedule_timer_callback_b },
    { 0x956d, "schedule_timer_callback_c",    0, call_schedule_timer_callback_c },
    { 0x7f2b, "arm_timer_callback",           0, NULL },
    { 0x7de8, "set_timer_slot",               0, NULL },
    { 0x7f65, "disable_timer_callback",       0, NULL },
    { 0x7e3d, "get_timer_slot_field",         0, NULL },
    { 0x7fde, "timer_restore",                0, NULL },
    /* L4 hardware (port-write-sequence, T6). */
    { 0x9115, "pc_speaker_silence",               1, NULL },
    { 0x9440, "speaker_gate_reset",               1, NULL },
    { 0x9451, "speaker_gate_strobe",              1, NULL },
    { 0x946e, "record_status_and_strobe_speaker", 1, NULL },
    { 0x8a07, "FUN_8a07_mpu_sample",              1, NULL },
    { 0x8ad0, "FUN_8ad0_mpu_settle",              1, NULL },
    { 0x8e2f, "FUN_8e2f_opl_allnotesoff",         1, NULL },
    { 0x89e2, "FUN_89e2_mpu_io",                  1, NULL },
    { 0x9007, "opl_write_reg",                    1, NULL },
    { 0x905d, "opl_play_note",                    1, NULL },
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
    for (i = 0; i < nrec; i++) {
        record_t *r = &recs[i];
        const ported_t *p = ported_lookup(r->fn_off);

        if (p == NULL || p->fn == NULL) {
            /* No reconstructed body yet (T2: all NULL).  Never references the symbol;
               UNPORTED is not a crash, not a hard failure. */
            st->unported++;
            continue;
        }

        /* ── PORTED: seed entry, (prime ports if L4), call, assert ──────────────── */
        {
            const char *bad = NULL; long got = 0, want = 0;
            g_cur_rec = r;            /* publish current record for the arg-recovery wrappers */
            seed_globals(&r->ent);
            if (p->is_l4) {
                prime_ports(r);
                p->fn();
                bad = cmp_ports(r, &got, &want);
                if (bad == NULL) st->port_checked++;
            } else {
                p->fn();
                bad = cmp_semantic(&r->ex, &got, &want);
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
                                  : "local/build/render/sound_trace.bin";
    FILE *f = fopen(path, "rb");
    long sz; u8 *b; u32 o; u16 ver, nsc, nfn; unsigned s;
    stats_t st = { 0, 0, 0, 0 };
    long n_records = 0, n_io_total = 0;
    int hard_fail = 0;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz);
    if (!b || fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
    fclose(f);

    if (sz < 14 || memcmp(b, "SNDTRC01", 8) != 0) {
        fprintf(stderr, "bad magic (want SNDTRC01)\n"); return 2;
    }
    ver = rd16(b + 8); nsc = rd16(b + 10);
    if (ver != 1) { fprintf(stderr, "unsupported version %u (want 1)\n", ver); return 2; }
    o = 12;
    nfn = rd16(b + o); o += 2;
    /* skip the fn-name string table (we key on fn_off, not the name index). */
    { u16 k; for (k = 0; k < nfn; k++) { u8 ln = b[o]; o += 1 + ln; } }

    printf("sound_ctest: replay harness over %s\n", path);
    printf("  trace: SNDTRC01 v%u, %u scenarios, %u fn-names (SNAP=%d B)\n",
           ver, nsc, nfn, SNAP_SIZE);
    printf("  src/sound.c is the Phase-6 T2 GLOBALS-ONLY skeleton (no sound bodies "
           "ported; bodies still stubbed in game_stubs.c).  PORTED[] all NULL -> every "
           "record UNPORTED; comparators dormant; expected FAIL=0.\n");

    for (s = 0; s < nsc; s++) {
        u8 sid, name_len, seed_dev;
        char scname[64];
        u32 nrec, k;
        record_t *recs;
        stats_t sst = { 0, 0, 0, 0 };
        int per_ok;

        sid = b[o]; o += 1;
        name_len = b[o]; o += 1;
        { unsigned n = name_len < 63 ? name_len : 63;
          memcpy(scname, b + o, n); scname[n] = 0; o += name_len; }
        seed_dev = b[o]; o += 1;
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

        printf("\n== scenario %u: %s (seed_device 0x%02x, %lu records) ==\n",
               sid, scname, seed_dev, (unsigned long)nrec);

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
    printf("PASS: FAIL=0.  %ld records UNPORTED (Phase-6 T2 skeleton — no sound bodies "
           "ported yet; comparators wired + dormant); %ld PORTED records matched.\n",
           st.unported, st.pass);
    free(b);
    return 0;
}
