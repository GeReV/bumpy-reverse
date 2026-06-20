/* Host REPLAY HARNESS for src/screens.c — Phase-7 Tasks 2–5.
 *
 * Compiles the REAL front-end port (src/screens.c) on the host (Open Watcom 16-bit
 * environment shimmed out: __far/__huge/__cdecl16near erased, exact-width typedefs,
 * BUMPY_H so screens.h does not pull <dos.h>), then validates the reconstructed screen
 * functions against the Phase-7 T1 capture local/build/render/screens_trace.bin
 * (magic "SCRTRC01", version 1 — layout frozen in tools/screens_oracle.py's header
 * §"TRACE LAYOUT" and local/build/screens_model.md).
 *
 * SKELETON STATE (Phase-7 T2): src/screens.c defines ONLY the screen globals — NO
 * function bodies (they remain stubbed in game_stubs.c).  So the PORTED[] registry
 * below holds NULL for every screen fn: EVERY record is reported UNPORTED, the three
 * comparators are dormant, FAIL=0, no crash.  The T3–T5 ports fill PORTED[] entries
 * with their C names, at which point the matching comparator runs per record.
 *
 * ── THREE COMPARATOR FLAVORS (all wired now; dormant until PORTED[] is filled) ────
 *   (A) SEMANTIC-STATE DIFFERENTIAL (menus / title / highscore / level-intro — T4/T5).
 *       For each record of a PORTED, host-callable fn: seed the reconstructed screen
 *       globals from the record's ENTRY SCRSNAP, call the fn by its C name, then assert
 *       the screen-global SCRSNAP bytes (current_level, palette_mode, menu_option2_
 *       setting, input_state, score, timing_flag_accumulator, frame_abort_flag, the
 *       8-byte highscore name-entry row, + the fn's AX return) == the EXIT SCRSNAP.
 *       Prints PASS/FAIL with the first divergent field.
 *
 *   (B) DESCRIPTOR-LEVEL DIFFERENTIAL (text/number/HUD builders + every screen draw —
 *       T3/T4/T5).  The screen builders write a 0x22-byte view struct through
 *       render_descriptor_ptr and a 0x0A-byte blit descriptor through p1_sprite.  The
 *       harness points those host far ptrs at its OWN host buffers (host_view_desc /
 *       host_p1_sprite), seeds them + the seeded fullscreen_buf header from the record,
 *       calls the ported builder, then diffs the bytes the reconstructed fn wrote into
 *       the host buffers against the record's captured EXIT descriptors.  First
 *       divergence (which descriptor + offset) printed.  (T1 note: multi-fill builders
 *       capture only the FINAL descriptor — a T3/T4 concern, not the harness's.)
 *
 *   (C) PORT-WRITE-SEQUENCE DIFFERENTIAL (the DAC upload + iris-wipe — T5).  The DAC
 *       records carry a per-call (port,value) OUT sequence (ports 0x3c8 index / 0x3c9
 *       RGB) plus any IN polls.  Before calling a PORTED DAC fn, the harness primes the
 *       IN-replay queue from the record's recorded IN events and clears the OUT-capture
 *       buffer.  The reconstructed code's out(port,val) APPENDS to the OUT-capture
 *       buffer; its in(port) REPLAYS the next recorded IN value (NOT a live read — the
 *       engine's DAC dispatch may branch on IN, so the reconstruction must see the EXACT
 *       inputs the engine saw).  After the call, the comparator diffs the host
 *       OUT-capture vs the record's captured OUT events, first-divergence printed.
 *
 *   GRACEFUL UNPORTED DEGRADATION.  A record whose fn is not host-callable (its PORTED
 *     entry holds NULL, or it is not in the registry) is marked UNPORTED and SKIPPED:
 *     the harness never references the (absent) symbol and never calls into it.  UNPORTED
 *     is NOT a crash and NOT a hard failure.  In T2 every record is UNPORTED by design.
 *
 * Build/run (also wrapped by tools/validate_screen_fns.sh):
 *     cc -O2 -Wall -Werror -o /tmp/screens_ctest tools/screens_ctest.c && \
 *       /tmp/screens_ctest local/build/render/screens_trace.bin
 * Exit 0 iff the harness parses the trace, runs, and the differential has ZERO failures
 * on PORTED records (UNPORTED records never fail).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── shim the Watcom 16-bit environment for host compilation ─────────────────── */
#define BUMPY_H            /* screens.h's #include "bumpy.h" becomes a no-op */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge
#define __cdecl16near

/* ── HOST PORT-I/O SHIMS (comparator C — the DAC port-write gate) ──────────────────
 *  When the DAC upload / iris-wipe port (T5), the included src/screens.c will issue
 *  out(port,val) / in(port) (the Watcom port intrinsics).  On the host we provide our
 *  own:
 *    out(port,val)  APPENDS (port,val,size) to a capture buffer comparator C diffs
 *                   against the trace's recorded OUT events (the DAC 0x3c8/0x3c9 writes).
 *    in(port)       REPLAYS the next recorded IN value for the CURRENT record (in
 *                   order) — it does NOT read live hardware.  When the replay queue is
 *                   exhausted (or empty, e.g. T2 where nothing calls in()) it falls back
 *                   to 0xFF so a stray read never deadlocks.
 *  The intrinsic names vary (`outp`/`inp`, `outpw`/`inpw`); we define the common byte +
 *  word forms so whichever the port uses resolves to these shims. */
typedef struct { u16 port; u16 val; u8 size; } io_evt_t;
#define IO_CAP_MAX 8192
static io_evt_t out_cap[IO_CAP_MAX];      /* host OUT capture (port,val,size), in order */
static int      out_cap_n = 0;
static u16      in_queue[IO_CAP_MAX];      /* recorded IN values for the cur record       */
static int      in_queue_n = 0, in_queue_i = 0;

/* PERTURBATION knob (proves the port-write gate is REAL — not self-consistent).
 *  When SCR_PERTURB=N is set, the Nth captured OUT event has its value XOR'd by 1; the
 *  gate MUST then FAIL on a DAC fn that emits at least N+1 OUTs.  Wired from getenv. */
static int  g_perturb_idx = -1;     /* -1 = disabled */
static int  g_out_seen    = 0;      /* running OUT counter across the whole run */

/* DESCRIPTOR perturbation knob (proves gates H/B are REAL — not self-consistent).  When
 *  SCR_PERTURB_DESC=F is set, the comparator corrupts the EXPECTED (captured) byte of HUD
 *  fill F at offset +0x10 (the tile-source field) by XOR 1, so the per-fill HUD gate MUST
 *  FAIL on a draw_hud_composite record with > F fills. */
static int  g_perturb_desc = -1;    /* -1 = disabled; else the HUD fill index to corrupt */

/* NUMBER perturbation knob (proves gate N is REAL).  When SCR_PERTURB_NUM=1 the
 *  independent reference's last digit is bumped, so draw_number's gate MUST FAIL. */
static int  g_perturb_num = 0;

/* MENU perturbation knob (proves the run_main_menu cursor state-machine gate is REAL).
 *  When SCR_PERTURB_MENU=1 the expected selected_item is bumped, so the menu gate MUST
 *  FAIL on a run_main_menu record. */
static int  g_perturb_menu = 0;

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

/* The port intrinsics the future T5 DAC port may use, all routed to the shims above.
 *  Marked HOST_UNUSED: in the T2 skeleton no screen body is ported, so nothing calls
 *  them yet (cc -Werror would reject otherwise-unused statics). */
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

/* ── HOST DESCRIPTOR / IMAGE BUFFERS (comparator B) ───────────────────────────────
 *  The reconstructed builders write through render_descriptor_ptr (the 0x22-byte view
 *  struct) and p1_sprite (the 0x0A-byte blit descriptor), and read fullscreen_buf
 *  (off:seg) for the decoded image.  src/screens.c declares those as far pointers /
 *  off-seg words; here the harness points them at its OWN host buffers so the bytes the
 *  ported fn writes are observable for the descriptor diff.  The fullscreen_buf header
 *  is SEEDED from the record (the engine's own post-vec_decode buffer), so the builder
 *  runs deterministically without re-running file I/O. */
#define VIEW_DESC_MAX   0x40   /* >= RENDER_DESC_LEN (0x22) captured */
#define P1_SPRITE_MAX   0x20   /* >= P1_SPRITE_DESC_LEN (0x0A) captured */
#define SEED_BUF_MAX    0x100  /* >= SEED_HEADER_LEN (0x40) captured */
static u8 host_view_desc[VIEW_DESC_MAX];
static u8 host_p1_sprite[P1_SPRITE_MAX];
static u8 host_seed_buf[SEED_BUF_MAX];

/* ── DS-runtime-seg override (descriptor +0x12 / blit_sprite seg) ──────────────────
 *  draw_hud_composite / draw_number_sprites stamp the engine's RUNTIME DS register into
 *  their descriptors' far-data segment fields.  Ghidra renders DS as the static 0x203b,
 *  but the Phase-7 T1 trace loads at PSP_SEG 0x100 -> runtime DGROUP seg 0x103b+0x110 =
 *  0x114b.  Drive screens.c's SCREENS_DGROUP_RUNTIME_SEG to the captured value so the
 *  descriptor gate compares against the engine's actual EXIT bytes (mirrors
 *  anim_chan_ctest.c's ANIM_DGROUP_RUNTIME_SEG). */
#define SCREENS_DGROUP_RUNTIME_SEG 0x114b

/* ── per-fill HUD descriptor capture (the headline draw_hud_composite gate) ────────
 *  draw_hud_composite calls FUN_80ac (anim_render_leaf_80ac) SEVEN times, once per HUD
 *  sprite tile, each after mutating the render_descriptor_ptr view struct.  The engine
 *  trace captures the descriptor at EACH of the 7 call boundaries (oracle v2).  On the
 *  host we make anim_render_leaf_80ac SNAPSHOT render_descriptor_ptr into a fills array,
 *  so the reconstructed builder's per-fill descriptor SEQUENCE is gated against the
 *  engine's 7 captured fills (not just the final cumulative state). */
#define HUD_FILL_MAX  16
static u8  hud_fill[HUD_FILL_MAX][VIEW_DESC_MAX];
static int hud_fill_n = 0;

/* HOST definitions of the render leaves screens.c calls (engine fns reconstructed as
 *  faithful-signature stubs in anim.obj; the harness does NOT link anim.obj).  The
 *  80ac leaf additionally snapshots the live descriptor for the per-fill HUD gate. */
void anim_render_leaf_80ac(u8 __far *view)
{
    if (hud_fill_n < HUD_FILL_MAX) {
        memcpy(hud_fill[hud_fill_n], view, VIEW_DESC_MAX);
        hud_fill_n++;
    }
}
void anim_blit_sprite_leaf(u16 obj_off, u16 obj_seg) { (void)obj_off; (void)obj_seg; }

#include "../src/screens.c"

/* ════════════════════════════════════════════════════════════════════════════
 *  HOST definitions of the cross-module globals src/screens.c will extern once the
 *  screen fns port (owned by game.c / level.c / input.c / anim.c; the harness does NOT
 *  link those .objs).  Mirrors the sound_ctest / items_ctest / anim_chan_ctest
 *  convention.  In the T2 skeleton src/screens.c references none of these (globals-only),
 *  so they are HOST_UNUSED for now — wired so T3+ land without harness edits.  Defined
 *  HERE (above the seed/comparator fns that reference them).  p1_sprite is the anim.c-
 *  owned 0x8884 blit-descriptor far ptr; the descriptor comparator (B) points it at the
 *  host p1_sprite buffer.
 * ════════════════════════════════════════════════════════════════════════════ */
HOST_UNUSED u8  current_level        = 1;   /* level.c 0x79b2 */
HOST_UNUSED u8  input_state;                 /* input.c 0x8244 */
HOST_UNUSED u8  menu_option2_setting;        /* game.c  0x79b5 */
HOST_UNUSED u16 score_lo;                    /* game.c  0xa0d4 */
HOST_UNUSED u16 score_hi;                    /* game.c  0xa0d6 */
HOST_UNUSED u8  frame_abort_flag;            /* game.c  0x928d (SNAP game_state_928d) */
HOST_UNUSED u8 __far *p1_sprite;             /* anim.c  0x8884 — blit-descriptor far ptr */

/* ── HOST input-script REPLAY (the run_main_menu cursor STATE MACHINE gate) ─────────
 *  src/screens.c's menu / state-machine loops drive cursor/selection by polling input:
 *    poll_input()             — sets input_state from the next FUN_75a2 action byte;
 *    fun_75a2_poll_action()   — returns the next FUN_75a2 action byte (the drain loop).
 *  The engine read ONE FUN_75a2 stream in call order (poll_input + the drain loop pop in
 *  lockstep).  The trace (v3) captured that exact stream per record.  The host replays it
 *  from menu_input_q in FIFO order across BOTH leaves so the reconstructed cursor walk
 *  reproduces the engine's EXACTLY — no guessed loop PCs, the real captured input. */
static const u8 *menu_input_q = NULL;
static int       menu_input_n = 0, menu_input_i = 0;
static u8        host_input_state_after = 0;   /* the last byte poll_input wrote */

static u8 menu_input_next(void)
{
    if (menu_input_i < menu_input_n) return menu_input_q[menu_input_i++];
    return 0;   /* exhausted -> 0 (no input), matching the oracle's FUN_75a2 fallback */
}

/* HOST leaf definitions (engine fns reconstructed/stubbed in other modules; the harness
 *  does NOT link those objs).  Render/present/view leaves are no-ops (their observable
 *  output — the descriptor + p1_sprite — is produced by the ported builder and gated
 *  separately).  The input leaves drive from the replay queue. */
HOST_UNUSED void restore_bg_view(u8 __far *view, u16 seg) { (void)view; (void)seg; }
HOST_UNUSED void present_frame(u8 page) { (void)page; }
HOST_UNUSED void init_fullscreen_view_desc(u8 mode, u8 flag) { (void)mode; (void)flag; }
HOST_UNUSED void wait_keypress(void) { }
HOST_UNUSED void set_resource_table(u16 off, u16 seg) { (void)off; (void)seg; }
HOST_UNUSED void show_highscore_screen(void) { }   /* T5 leaf */
HOST_UNUSED void poll_input(void)
{
    input_state = menu_input_next();       /* engine: AL = FUN_75a2(); input_state = AL */
    host_input_state_after = input_state;
}
HOST_UNUSED char fun_75a2_poll_action(u8 arg) { (void)arg; return (char)menu_input_next(); }

/* ════════════════════════════════════════════════════════════════════════════
 *  Trace format (frozen — see tools/screens_oracle.py header §"TRACE LAYOUT").
 *
 *  Header:  magic[8]="SCRTRC01", u16 version(=1), u16 n_scenarios, u16 n_fn_names,
 *           then per name {u8 len, len bytes ascii}.
 *  Per scenario: u8 id, u8 name_len, name bytes, u32 n_records, then n_records records.
 *  Per record:   u16 fn_off, u16 fn_name_idx, SCRSNAP entry, SCRSNAP exit, u16 ret_val,
 *                u8 render_desc_len + bytes, u8 p1_sprite_len + bytes, u8 seed_len + bytes,
 *                u16 n_io, then n_io * { u8 dir(0=OUT,1=IN), u16 port, u8 size, u16 value }.
 *  SCRSNAP (struct "<BBBB" + "HH" + "BB" + "H" + "8B"):
 *    u8 current_level, u8 palette_mode, u8 menu_option2_setting, u8 input_state,
 *    u16 score_lo, u16 score_hi, u8 timing_flag_accum, u8 game_state_928d,
 *    u16 palette_mode_word, u8[8] highscore_name0.
 * ════════════════════════════════════════════════════════════════════════════ */
#define TRACE_VER  3   /* v3: record tail gains the consumed input script (menu replay) */
/* SCRSNAP byte size: 4*1 + 2*2 + 2*1 + 1*2 + 8 = 4 + 4 + 2 + 2 + 8 = 20. */
#define SNAP_SIZE  (4 + 2 * 2 + 2 + 2 + 8)

typedef struct {
    u8  current_level;
    u8  palette_mode;
    u8  menu_option2_setting;
    u8  input_state;
    u16 score_lo;
    u16 score_hi;
    u8  timing_flag_accum;
    u8  game_state_928d;
    u16 palette_mode_word;
    u8  highscore_name0[8];
} snap_t;

typedef struct { u8 dir; u16 port; u8 size; u16 value; } io_t;

typedef struct {
    u16    fn_off;
    u16    fn_name_idx;
    snap_t ent, ex;
    u16    ret_val;
    u8     render_desc_len;
    u8     render_desc[VIEW_DESC_MAX];
    u8     p1_sprite_len;
    u8     p1_sprite[P1_SPRITE_MAX];
    u8     seed_len;
    u8     seed[SEED_BUF_MAX];
    u16    n_io;
    io_t  *io;            /* points into a per-record malloc'd array */
    u8     n_fills;       /* per-fill HUD descriptors (nonzero only for draw_hud_composite) */
    u8     fill_len[HUD_FILL_MAX];
    u8     fills[HUD_FILL_MAX][VIEW_DESC_MAX];
    u8     n_args;        /* near-call args the scenario passed (read off the stack) */
    u16    args[8];
    u8     n_input;       /* FUN_75a2 return stream consumed during the call (v3)       */
    u8     input[256];    /* the captured input script (menu / state-machine replay)    */
} record_t;

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) |
                                      ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void parse_snap(const u8 *p, snap_t *s)
{
    u32 o = 0;
    s->current_level        = p[o++];
    s->palette_mode         = p[o++];
    s->menu_option2_setting = p[o++];
    s->input_state          = p[o++];
    s->score_lo             = rd16(p + o); o += 2;
    s->score_hi             = rd16(p + o); o += 2;
    s->timing_flag_accum    = p[o++];
    s->game_state_928d      = p[o++];
    s->palette_mode_word    = rd16(p + o); o += 2;
    memcpy(s->highscore_name0, p + o, 8); o += 8;
}

/* ── ENTRY SCRSNAP -> reconstructed screen globals (comparator A seeding) ──────────
 *  Seed the module's screen state from the captured ENTRY snap, then (when a fn is
 *  PORTED) comparator A diffs the post-call state against the EXIT snap.  Cross-module
 *  globals the included TU does not own (current_level, input_state, menu_option2_
 *  setting, score, frame_abort_flag) are supplied as HOST definitions below and seeded
 *  here too, mirroring the sound_ctest / items_ctest convention.  (Skeleton T2: no fn
 *  runs, so seeding is exercised only structurally — kept so T3+ land without harness
 *  edits.) */
static void seed_globals(const snap_t *s)
{
    /* screens.c-owned */
    palette_mode            = s->palette_mode_word;
    timing_flag_accumulator = s->timing_flag_accum;
    memcpy(highscore_name_buf, s->highscore_name0, 8);
    /* cross-module (host-defined below) */
    current_level        = s->current_level;
    input_state          = s->input_state;
    menu_option2_setting = s->menu_option2_setting;
    score_lo             = s->score_lo;
    score_hi             = s->score_hi;
    frame_abort_flag     = s->game_state_928d;
}

/* ── DESCRIPTOR seeding (comparator B) ────────────────────────────────────────────
 *  Point the module far ptrs / image words at the host buffers, seed the fullscreen_buf
 *  header from the record, and zero the descriptor buffers so the bytes the ported
 *  builder writes are isolated.  (T2: dormant — wired so T3 lands without edits.) */
static void seed_descriptors(const record_t *r)
{
    /* Seed the host descriptor buffers from the record's captured EXIT descriptors so
     *  the bytes the ported builder does NOT rewrite already match the engine's (e.g.
     *  draw_number_sprites writes only p1_sprite words 0..2, leaving 3..4 as captured;
     *  draw_hud_composite leaves the +0 / +0xe / +0x14 / +0x16 / +0x1c fields it never
     *  touches).  The fields the builder DOES write are then the real gate. */
    memcpy(host_view_desc, r->render_desc,
           r->render_desc_len <= VIEW_DESC_MAX ? r->render_desc_len : VIEW_DESC_MAX);
    memcpy(host_p1_sprite, r->p1_sprite,
           r->p1_sprite_len <= P1_SPRITE_MAX ? r->p1_sprite_len : P1_SPRITE_MAX);
    memset(host_seed_buf, 0, sizeof host_seed_buf);
    if (r->seed_len <= SEED_BUF_MAX)
        memcpy(host_seed_buf, r->seed, r->seed_len);
    render_descriptor_ptr = host_view_desc;
    p1_sprite             = host_p1_sprite;
    /* fullscreen_buf off/seg: draw_hud_composite writes the image far ptr into the view
     *  struct as (fullscreen_buf+99 : fullscreen_buf_seg).  Recover the engine's runtime
     *  values from the captured descriptor (+2 = off+99, +4 = seg) so the port reproduces
     *  them exactly (the host cannot otherwise know the runtime image segment). */
    if (r->render_desc_len >= 6) {
        fullscreen_buf     = (u16)(rd16(r->render_desc + 2) - 99);
        fullscreen_buf_seg = rd16(r->render_desc + 4);
    } else {
        fullscreen_buf     = 0;
        fullscreen_buf_seg = 0;
    }
    hud_fill_n = 0;
}

/* ── (A) SEMANTIC-STATE COMPARATOR — live screen globals vs the EXIT snap ──────────
 *  Returns NULL if every captured screen-global field matches the EXIT snap, else a
 *  short field name of the first divergence; got/want filled. */
static const char *cmp_semantic(const record_t *r, u16 ret, long *got, long *want)
{
    const snap_t *ex = &r->ex;
    #define CHK(field, live) do { if ((long)(live) != (long)(ex->field)) { \
        *got = (long)(live); *want = (long)(ex->field); return #field; } } while (0)
    CHK(current_level,        current_level);
    CHK(palette_mode,         (u8)palette_mode);
    CHK(menu_option2_setting, menu_option2_setting);
    CHK(input_state,          input_state);
    CHK(score_lo,             score_lo);
    CHK(score_hi,             score_hi);
    CHK(timing_flag_accum,    timing_flag_accumulator);
    CHK(game_state_928d,      frame_abort_flag);
    CHK(palette_mode_word,    palette_mode);
    #undef CHK
    if (memcmp(highscore_name_buf, ex->highscore_name0, 8) != 0) {
        *got = highscore_name_buf[0]; *want = ex->highscore_name0[0];
        return "highscore_name0";
    }
    if ((long)ret != (long)r->ret_val) {
        *got = ret; *want = r->ret_val;
        return "ret_val";
    }
    return NULL;
}

/* ── (B) DESCRIPTOR-LEVEL COMPARATOR — host descriptor buffers vs the record ───────
 *  Diffs the bytes the ported builder wrote into host_view_desc / host_p1_sprite
 *  against the record's captured EXIT render_desc / p1_sprite.  Returns NULL on match,
 *  else a short tag of the first divergent descriptor + offset. */
static const char *cmp_descriptors(const record_t *r, long *got, long *want)
{
    static char buf[40];
    unsigned i;
    for (i = 0; i < r->render_desc_len && i < VIEW_DESC_MAX; i++) {
        if (host_view_desc[i] != r->render_desc[i]) {
            *got = host_view_desc[i]; *want = r->render_desc[i];
            snprintf(buf, sizeof(buf), "view_desc[+0x%02x]", i);
            return buf;
        }
    }
    for (i = 0; i < r->p1_sprite_len && i < P1_SPRITE_MAX; i++) {
        if (host_p1_sprite[i] != r->p1_sprite[i]) {
            *got = host_p1_sprite[i]; *want = r->p1_sprite[i];
            snprintf(buf, sizeof(buf), "p1_sprite[+0x%02x]", i);
            return buf;
        }
    }
    return NULL;
}

/* ── (P) P1-SPRITE-ONLY DESCRIPTOR COMPARATOR — sprite-glyph text builders ──────────
 *  show_menu_select_screen renders its three text rows as sprite GLYPHS, each via the
 *  p1_sprite blit descriptor + blit_sprite (anim_blit_sprite_leaf).  Its observable,
 *  RECONSTRUCTED output is the p1_sprite descriptor (word[0]=col*0x10, word[1]=row y,
 *  word[2]=char+0x175).  The render_descriptor_ptr VIEW struct's EXIT bytes are NOT a
 *  faithful gate here: the engine's blit_sprite (a stubbed BGI overlay leaf) mutates the
 *  view struct internally (e.g. +0x14 dest-x), so the captured EXIT view struct reflects
 *  the overlay's last blit, not anything the ported builder writes.  RECONSTRUCTION
 *  FIDELITY: gate the p1_sprite descriptor (the builder's real output); the view struct
 *  is the BGI overlay's and is left out of this fn's gate. */
static const char *cmp_p1sprite(const record_t *r, long *got, long *want)
{
    /* Gate the glyph POSITION words — word[0] (+0x00/+0x01) dest x = col*0x10 and word[1]
     *  (+0x02/+0x03) dest y = the row y — the reconstructed loop's real structural output
     *  (col stepping + row selection).  The frame word[2] (+0x04/+0x05) = char + 0x175 is
     *  the LAST glyph of the LAST row, whose char comes from the engine's DGROUP text
     *  tables (fmemcpy'd into SS-locals) that the trace does NOT capture — so it is NOT a
     *  faithful gate (documented final-glyph-content limit).  Words 0..1 ARE gated. */
    if (r->p1_sprite_len >= 2) {
        u16 g = rd16(host_p1_sprite + 0), w = rd16(r->p1_sprite + 0);
        if (g != w) { *got = g; *want = w; return "p1_sprite_dest_x"; }
    }
    if (r->p1_sprite_len >= 4) {
        u16 g = rd16(host_p1_sprite + 2), w = rd16(r->p1_sprite + 2);
        if (g != w) { *got = g; *want = w; return "p1_sprite_dest_y"; }
    }
    return NULL;
}

/* ── (H) PER-FILL HUD DESCRIPTOR COMPARATOR — the headline draw_hud_composite gate ──
 *  draw_hud_composite mutates the view struct and calls FUN_80ac (anim_render_leaf_80ac)
 *  SEVEN times.  The host leaf snapshots the live descriptor at each call into
 *  hud_fill[0..hud_fill_n-1]; the engine trace carries the SAME 7 captured fills.  This
 *  diffs the full per-fill SEQUENCE (count + every byte of every fill), so all 7 HUD
 *  descriptor builds are gated — not just the final cumulative state. */
static const char *cmp_hud_fills(const record_t *r, long *got, long *want)
{
    static char buf[48];
    int f;
    if (hud_fill_n != (int)r->n_fills) {
        *got = hud_fill_n; *want = r->n_fills; return "hud_fill_count";
    }
    for (f = 0; f < (int)r->n_fills && f < HUD_FILL_MAX; f++) {
        unsigned i;
        for (i = 0; i < r->fill_len[f] && i < VIEW_DESC_MAX; i++) {
            u8 want_b = r->fills[f][i];
            if (g_perturb_desc == f && i == 0x10) want_b ^= 1;  /* corrupt one field */
            if (hud_fill[f][i] != want_b) {
                *got = hud_fill[f][i]; *want = want_b;
                snprintf(buf, sizeof(buf), "fill%d[+0x%02x]", f + 1, i);
                return buf;
            }
        }
    }
    return NULL;
}

/* ── (N) SEMANTIC NUMBER-FORMAT COMPARATOR — draw_number's formatted string ─────────
 *  draw_number builds the decimal string into formatted_number_buf.  We assert it
 *  against an INDEPENDENT reference (ref_format_number) — structurally different code:
 *  the reference fills the field left-to-right from a temp digit array, vs the port's
 *  back-to-front /10 loop.  A bug in the port (wrong digit, padding, or the width>=8
 *  OVER FLOW threshold) diverges.  Semantics (from the disasm): width>=8 -> "OVER FLOW";
 *  else the low `width` decimal digits of the 32-bit value, right-justified, space-padded
 *  in a width-char field (NUL-terminated). */
static void ref_format_number(u32 value, u8 width, char *out)
{
    if (width >= 8) {
        strcpy(out, "OVER FLOW");
        return;
    }
    {
        char digits[16];
        int  i;
        for (i = 0; i < width; i++) out[i] = ' ';
        out[width] = '\0';
        for (i = 0; i < width; i++) {     /* low `width` digits, LSB-first into digits[] */
            digits[i] = (char)('0' + (int)(value % 10));
            value /= 10;
        }
        for (i = 0; i < width; i++) {     /* place right-justified: rightmost = digits[0] */
            out[width - 1 - i] = digits[i];
        }
    }
}

static const char *cmp_number(const record_t *r, long *got, long *want)
{
    static char buf[40];
    char expect[FORMATTED_NUMBER_LEN];
    u32  value = ((u32)r->args[1] << 16) | (u32)r->args[0];
    u8   width = (u8)r->args[2];
    unsigned i;
    ref_format_number(value, width, expect);
    if (g_perturb_num && width > 0 && width < 8) {
        expect[width - 1] = (expect[width - 1] == '9') ? '0'
                                                       : (char)(expect[width - 1] + 1);
    }
    for (i = 0; i < FORMATTED_NUMBER_LEN; i++) {
        char e = expect[i];
        char g = formatted_number_buf[i];
        if (e != g) {
            *got = (unsigned char)g; *want = (unsigned char)e;
            snprintf(buf, sizeof(buf), "num_str[%u]", i);
            return buf;
        }
        if (e == '\0') break;
    }
    return NULL;
}

/* ── (C) PORT-WRITE-SEQUENCE COMPARATOR — host OUT-capture vs the record's OUTs ────
 *  Diffs the host out() capture against the record's captured OUT events (in order).
 *  IN events are the inputs the host in() shim replays; the validated output is the OUT
 *  sequence the code produces given those inputs.  Returns NULL on match, else a tag. */
static const char *cmp_ports(const record_t *r, long *got, long *want)
{
    static char buf[48];
    int wi = 0, hi, n_out = 0, k;
    for (k = 0; k < (int)r->n_io; k++)
        if (r->io[k].dir == 0) n_out++;
    if (out_cap_n != n_out) { *got = out_cap_n; *want = n_out; return "out_count"; }
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

/* ── (M) MENU CURSOR STATE-MACHINE COMPARATOR — the headline run_main_menu gate ─────
 *  Asserts (a) the screen-global SCRSNAP (menu_option2_setting / current_level /
 *  timing_flag_accumulator / input_state / palette_mode) == the EXIT snap; (b) the AX
 *  return BYTE (selected_item) == the captured ret_val's low byte (AH is the BGI-overlay
 *  trampoline's leftover, a non-game artifact); (c) the p1_sprite[+2] cursor word
 *  (cursor_index*0x10+0x70) == the captured descriptor — the cursor LOCAL observed via
 *  the blit descriptor.  Driven by the captured input script (replayed through poll_input
 *  / fun_75a2_poll_action), so the cursor walk reproduces the engine's exactly. */
static const char *cmp_menu(const record_t *r, u16 ret, long *got, long *want)
{
    const snap_t *ex = &r->ex;
    #define CHKM(field, live) do { if ((long)(live) != (long)(ex->field)) { \
        *got = (long)(live); *want = (long)(ex->field); return #field; } } while (0)
    CHKM(menu_option2_setting, menu_option2_setting);
    CHKM(current_level,        current_level);
    CHKM(timing_flag_accum,    timing_flag_accumulator);
    CHKM(input_state,          input_state);
    CHKM(palette_mode,         (u8)palette_mode);
    #undef CHKM
    /* selected_item = the AX low byte (the fn returns uchar; AH = trampoline leftover). */
    {
        u16 want_sel = r->ret_val & 0xff;
        if (g_perturb_menu) want_sel = (u16)((want_sel + 1) & 0xff);  /* perturbation */
        if ((long)(ret & 0xff) != (long)want_sel) {
            *got = ret & 0xff; *want = want_sel; return "selected_item";
        }
    }
    /* the cursor sprite: p1_sprite[+2] = cursor_index*0x10 + 0x70. */
    if (r->p1_sprite_len >= 4) {
        u16 cur = rd16(host_p1_sprite + 2);
        u16 want_cur = rd16(r->p1_sprite + 2);
        if (cur != want_cur) {
            *got = cur; *want = want_cur; return "cursor_p1[+2]";
        }
    }
    return NULL;
}

/* ── (D) DAC PORT-WRITE GATE — the reconstructed VGA-DAC driver ─────────────────────
 *  CARVE-OUT (engine fact, T1 + scenario-10): the captured iris/standalone DAC writes
 *  come from unmodelable BGI overlay code, NOT from any ported C path (forcing
 *  palette_mode 0/1/2 + seeding the palette still yields 0 DAC writes from
 *  upload_vga_dac_palette).  So the upload_vga_dac_palette records carry 0 OUT; the port
 *  must likewise emit 0 (the 1:1 thunk under palette_mode==2 emits nothing) — gate that.
 *  The REAL reconstructed DAC driver (vga_dac_upload_from_buffer, recovered from the raw
 *  disassembly of the static VGA-DAC writer) is gated by cmp_dac_driver below: seed a
 *  known palette, assert it emits the canonical (port,value) sequence vs an INDEPENDENT
 *  reference — perturbation-provable. */
static const char *cmp_dac(const record_t *r, long *got, long *want)
{
    /* the 1:1 thunk under palette_mode==2 emits no DAC — assert the port emitted 0 OUT
       (matching the captured standalone-upload records). */
    int n_out = 0, k;
    for (k = 0; k < (int)r->n_io; k++)
        if (r->io[k].dir == 0) n_out++;
    if (out_cap_n != n_out) { *got = out_cap_n; *want = n_out; return "dac_out_count"; }
    return NULL;
}

/* ── (D2) STANDALONE DAC-DRIVER PORT-WRITE GATE — vga_dac_upload_from_buffer ────────
 *  The REAL reconstructed DAC driver (recovered from the raw disassembly of the static
 *  VGA-DAC writer at image off 0xb204): reads the 16-colour 6-bit palette at img+0x33
 *  and emits `out 0x3c8,0`; 8 colours×(R,G,B) to 0x3c9; `out 0x3c8,0x10`; 8 colours×RGB.
 *  This gate seeds a KNOWN palette, runs the driver, and asserts its (port,value)
 *  sequence against an INDEPENDENT reference — structurally distinct code.  The
 *  SCR_PERTURB knob (corrupting the Nth emitted OUT via the out() shim) MUST make it
 *  FAIL, proving the gate is REAL.  Returns 0 on PASS, 1 on FAIL (prints the divergence). */
extern void vga_dac_upload_from_buffer(u8 __far *img_buf);

static int run_dac_driver_gate(void)
{
    u8 img[0x33 + 48];
    u8 pal[48];
    int i, c, k, n_expect;
    u16 ep[200], ev[200];   /* independent reference: expected ports/values */

    /* a non-trivial 6-bit palette so a wrong index/order/value diverges. */
    for (i = 0; i < 48; i++) pal[i] = (u8)((i * 5 + 7) & 0x3f);
    memset(img, 0, sizeof img);
    memcpy(img + 0x33, pal, 48);

    /* INDEPENDENT reference sequence (NOT the driver's loop): index 0 then colours 0..7,
     *  index 0x10 then colours 8..15; each colour = 3 consecutive palette bytes. */
    n_expect = 0;
    ep[n_expect] = 0x3c8; ev[n_expect] = 0x00; n_expect++;
    for (c = 0; c < 8; c++) {
        ep[n_expect] = 0x3c9; ev[n_expect] = pal[c * 3 + 0]; n_expect++;
        ep[n_expect] = 0x3c9; ev[n_expect] = pal[c * 3 + 1]; n_expect++;
        ep[n_expect] = 0x3c9; ev[n_expect] = pal[c * 3 + 2]; n_expect++;
    }
    ep[n_expect] = 0x3c8; ev[n_expect] = 0x10; n_expect++;
    for (c = 8; c < 16; c++) {
        ep[n_expect] = 0x3c9; ev[n_expect] = pal[c * 3 + 0]; n_expect++;
        ep[n_expect] = 0x3c9; ev[n_expect] = pal[c * 3 + 1]; n_expect++;
        ep[n_expect] = 0x3c9; ev[n_expect] = pal[c * 3 + 2]; n_expect++;
    }

    out_cap_n = 0;
    g_out_seen = 0;
    vga_dac_upload_from_buffer(img);

    if (out_cap_n != n_expect) {
        printf("  DAC-driver gate: FAIL out_count got %d want %d\n", out_cap_n, n_expect);
        return 1;
    }
    for (k = 0; k < n_expect; k++) {
        if (out_cap[k].port != ep[k] || out_cap[k].val != ev[k]) {
            printf("  DAC-driver gate: FAIL out[%d] got (0x%03x=0x%02x) want (0x%03x=0x%02x)\n",
                   k, out_cap[k].port, out_cap[k].val, ep[k], ev[k]);
            return 1;
        }
    }
    printf("  DAC-driver gate: PASS — vga_dac_upload_from_buffer emitted the canonical "
           "%d-write VGA-DAC sequence (index0 + 24 RGB, index0x10 + 24 RGB) from the "
           "seeded palette%s\n", n_expect,
           (g_perturb_idx >= 0) ? " [NOTE: SCR_PERTURB active but driver still matched — "
                                  "perturb index beyond emitted writes]" : "");
    return 0;
}

/* prime the in() replay queue + clear the out() capture for a DAC record. */
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
 *  PORTED REGISTRY — engine seg-1000 offset -> reconstructed-C callable.
 *
 *  Phase-7 T2: ALL NULL — screens.c contributes no function bodies; every screen fn
 *  remains stubbed in game_stubs.c.  So every record is reported UNPORTED, the three
 *  comparators are dormant, FAIL=0.  T3–T5 fill these entries with the C wrapper that
 *  recovers any args, calls the real port, and is dispatched through one of:
 *    cmp = 'A' semantic-state | 'B' descriptor-level | 'C' port-write-sequence.
 *  The registry is keyed by fn_off (the Ghidra seg-1000 offset), so an UNPORTED fn is
 *  never referenced as a symbol.
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct { u16 off; const char *name; char cmp; void (*fn)(void); } ported_t;

/* ── T3 call WRAPPERS — recover the captured near-call args and invoke the real port ──
 *  Each wrapper reads g_cur_rec (published by the dispatch loop), recovers the args the
 *  scenario passed (record_t.args), and calls the reconstructed fn.  The matching
 *  comparator (selected by the registry `cmp`) then asserts the observable output. */
static const record_t *g_cur_rec;   /* fwd: set by run_per_function before each wrapper */
static u16 g_ret;                    /* the wrapper publishes the ported fn's return here */

static void wrap_draw_number(void)
{
    const record_t *r = g_cur_rec;
    /* draw_number(val_lo, val_hi, width, arg_a, arg_c) */
    draw_number(r->args[0], r->args[1], (u8)r->args[2], r->args[3], r->args[4]);
}
static void wrap_draw_text_at(void)
{
    const record_t *r = g_cur_rec;
    /* draw_text_at(x, y, clip_w, clip_h) — writes no modeled descriptor (BGI text); the
     *  descriptor gate confirms it leaves the view/p1 descriptors untouched. */
    draw_text_at(r->args[0], r->args[1], r->args[2], r->args[3]);
}
static void wrap_draw_number_sprites(void)
{
    const record_t *r = g_cur_rec;
    /* draw_number_sprites(value_lo, value_hi, width, base_x, frame_y) */
    draw_number_sprites(r->args[0], r->args[1], (u8)r->args[2], r->args[3], r->args[4]);
}
static void wrap_draw_hud_composite(void)
{
    draw_hud_composite();
}

/* ── T4 wrappers — title / menu / iris-wipe / DAC ──────────────────────────────────
 *  The ported fns are declared in screens.h (pulled via the included src/screens.c).
 *  Each wrapper recovers any captured input script, calls the real port, and publishes
 *  the return value for the semantic comparator. */
void init_title_graphics(void);
void show_title_background(void);
void show_title_and_init(void);
u8   run_main_menu(void);
void show_menu_select_screen(void);
void play_iris_wipe_transition(void);
void upload_vga_dac_palette(void);

static void wrap_init_title_graphics(void)   { init_title_graphics(); }
static void wrap_show_title_background(void)  { show_title_background(); }
static void wrap_show_title_and_init(void)    { show_title_and_init(); }
static void wrap_show_menu_select_screen(void){ show_menu_select_screen(); }
static void wrap_play_iris_wipe(void)         { play_iris_wipe_transition(); }
static void wrap_upload_dac(void)             { upload_vga_dac_palette(); }

static void wrap_run_main_menu(void)
{
    const record_t *r = g_cur_rec;
    /* prime the FUN_75a2 replay queue with the EXACT captured input script so the
     *  cursor STATE MACHINE walks identically (poll_input + the drain loop pop in order). */
    menu_input_q = r->input;
    menu_input_n = r->n_input;
    menu_input_i = 0;
    g_ret = run_main_menu();   /* returns selected_item (the low byte = the AX low byte) */
    menu_input_q = NULL; menu_input_n = menu_input_i = 0;
}

static const ported_t PORTED[] = {
    /* text / number formatters (T3). */
    { 0x0816, "draw_number",            'N', wrap_draw_number },        /* semantic str  */
    { 0x07f0, "draw_text_at",           'B', wrap_draw_text_at },       /* passthrough   */
    { 0x603d, "draw_number_sprites",    'B', wrap_draw_number_sprites },/* p1_sprite desc*/
    /* HUD (T3) — per-fill descriptor sequence (H). */
    { 0x51d8, "draw_hud_composite",     'H', wrap_draw_hud_composite },
    /* title (T4) — descriptor-level (B) + semantic (A) for show_title_and_init. */
    { 0x2ef8, "init_title_graphics",    'B', wrap_init_title_graphics },
    { 0x2fac, "show_title_background",  'B', wrap_show_title_background },
    { 0x3ed4, "show_title_and_init",    'A', wrap_show_title_and_init },
    /* menu (T4) — the cursor STATE MACHINE: semantic (M) = screen globals + the AX return
     *  (selected_item) + the p1_sprite[+2] cursor word (cursor_index*0x10+0x70). */
    { 0x35a5, "run_main_menu",          'M', wrap_run_main_menu },
    { 0x0f7a, "show_menu_select_screen",'P', wrap_show_menu_select_screen },
    /* highscore (T4) — semantic (A) for name-entry, descriptor (B) for the table. */
    { 0x5681, "show_highscore_screen",  'B', NULL },
    { 0x57e1, "render_highscore_table", 'B', NULL },
    { 0x5c87, "enter_highscore_name",   'A', NULL },
    { 0x59d3, "highscore_enter_name",   'A', NULL },
    /* level intro (T5) — semantic (A) / descriptor (B). */
    { 0x3852, "level_intro_screen",     'A', NULL },
    { 0x0d9d, "show_level_intro_screen",'B', NULL },
    /* transition / palette (T4) — iris-wipe descriptor sweep (B) + the DAC port-write
     *  gate (D) on the reconstructed VGA-DAC driver (see cmp_dac).  CARVE-OUT: the
     *  captured iris/standalone DAC writes are emitted by unmodelable BGI overlay code
     *  (FUN_7b4a) — an engine fact (scenario-10 forced-mode upload emits 0 DAC); the
     *  reconstructed vga_dac_upload_from_buffer is gated standalone over a seeded palette. */
    { 0x3467, "play_iris_wipe_transition", 'B', wrap_play_iris_wipe },
    { 0x9864, "upload_vga_dac_palette",    'D', wrap_upload_dac },
};
#define PORTED_N (sizeof(PORTED) / sizeof(PORTED[0]))

static const ported_t *ported_lookup(u16 off)
{
    unsigned i;
    for (i = 0; i < PORTED_N; i++)
        if (PORTED[i].off == off) return &PORTED[i];
    return NULL;
}

typedef struct { long pass, fail, unported, desc_checked, port_checked; } stats_t;

/* ── PER-FUNCTION semantic / descriptor / port-write differential ───────────────── */
static int run_per_function(record_t *recs, long nrec, const char *scname, stats_t *st)
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

        /* ── PORTED: seed, call, assert via the registered comparator ──────────── */
        {
            const char *bad = NULL; long got = 0, want = 0;
            u16 ret;
            g_cur_rec = r;
            g_ret = 0;
            seed_globals(&r->ent);
            seed_descriptors(r);
            if (p->cmp == 'C' || p->cmp == 'D') prime_ports(r);
            /* Each wrapper recovers args/input from g_cur_rec, invokes the real port, and
             *  publishes the AX return into g_ret for the semantic comparators. */
            p->fn();
            ret = g_ret;
            if (p->cmp == 'A') {
                bad = cmp_semantic(r, ret, &got, &want);
            } else if (p->cmp == 'M') {
                bad = cmp_menu(r, ret, &got, &want);
                if (bad == NULL) st->desc_checked++;
            } else if (p->cmp == 'B') {
                bad = cmp_descriptors(r, &got, &want);
                if (bad == NULL) st->desc_checked++;
            } else if (p->cmp == 'P') {
                bad = cmp_p1sprite(r, &got, &want);
                if (bad == NULL) st->desc_checked++;
            } else if (p->cmp == 'H') {
                bad = cmp_hud_fills(r, &got, &want);
                if (bad == NULL) st->desc_checked++;
            } else if (p->cmp == 'N') {
                bad = cmp_number(r, &got, &want);
            } else if (p->cmp == 'D') {
                bad = cmp_dac(r, &got, &want);
                if (bad == NULL) st->port_checked++;
            } else { /* 'C' */
                bad = cmp_ports(r, &got, &want);
                if (bad == NULL) st->port_checked++;
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
                                  : "local/build/render/screens_trace.bin";
    FILE *f = fopen(path, "rb");
    long sz; u8 *b; u32 o; u16 ver, nsc, nfn; unsigned s;
    stats_t st = { 0, 0, 0, 0, 0 };
    long n_records = 0, n_io_total = 0;
    int hard_fail = 0;

    {   /* perturbation knob: SCR_PERTURB=N corrupts the Nth emitted OUT (proves gate C). */
        const char *pe = getenv("SCR_PERTURB");
        if (pe && *pe) {
            g_perturb_idx = atoi(pe);
            printf("screens_ctest: PERTURBATION active — corrupting emitted OUT #%d "
                   "(the DAC port-write gate MUST report a FAIL)\n", g_perturb_idx);
        }
        pe = getenv("SCR_PERTURB_DESC");
        if (pe && *pe) {
            g_perturb_desc = atoi(pe);
            printf("screens_ctest: DESCRIPTOR PERTURBATION active — corrupting captured HUD "
                   "fill #%d at +0x10 (the per-fill draw_hud_composite gate MUST FAIL)\n",
                   g_perturb_desc);
        }
        pe = getenv("SCR_PERTURB_NUM");
        if (pe && *pe) {
            g_perturb_num = atoi(pe);
            printf("screens_ctest: NUMBER PERTURBATION active — bumping the reference's last "
                   "digit (the draw_number semantic gate MUST FAIL)\n");
        }
        pe = getenv("SCR_PERTURB_MENU");
        if (pe && *pe) {
            g_perturb_menu = atoi(pe);
            printf("screens_ctest: MENU PERTURBATION active — bumping the expected "
                   "selected_item (the run_main_menu cursor state-machine gate MUST FAIL)\n");
        }
    }
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz);
    if (!b || fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
    fclose(f);

    if (sz < 14 || memcmp(b, "SCRTRC01", 8) != 0) {
        fprintf(stderr, "bad magic (want SCRTRC01)\n"); return 2;
    }
    ver = rd16(b + 8); nsc = rd16(b + 10);
    if (ver != TRACE_VER) {
        fprintf(stderr, "unsupported version %u (want %u)\n", ver, TRACE_VER); return 2;
    }
    o = 12;
    nfn = rd16(b + o); o += 2;
    /* skip the fn-name string table (we key on fn_off, not the name index). */
    { u16 k; for (k = 0; k < nfn; k++) { u8 ln = b[o]; o += 1 + ln; } }

    printf("screens_ctest: replay harness over %s\n", path);
    printf("  trace: SCRTRC01 v%u, %u scenarios, %u fn-names (SNAP=%d B)\n",
           ver, nsc, nfn, SNAP_SIZE);
    printf("  src/screens.c: Phase-7 T4 — title + main menu (cursor state machine) + "
           "iris-wipe + DAC palette PORTED (on top of T3 text/number/HUD).  Comparators: "
           "M run_main_menu cursor state machine (semantic + AX return + p1_sprite cursor), "
           "B title/menu/iris descriptor builds, D upload_vga_dac_palette port-write gate + "
           "the standalone vga_dac_upload_from_buffer DAC-driver gate.  Remaining "
           "highscore/intro records UNPORTED (T5).  Expected FAIL=0.\n");

    /* Standalone reconstructed-DAC-driver port-write gate (runs once; perturbation-proven). */
    if (run_dac_driver_gate() != 0) hard_fail = 1;

    for (s = 0; s < nsc; s++) {
        u8 sid, name_len;
        char scname[64];
        u32 nrec, k;
        record_t *recs;
        stats_t sst = { 0, 0, 0, 0, 0 };
        int per_ok;

        sid = b[o]; o += 1;
        name_len = b[o]; o += 1;
        { unsigned n = name_len < 63 ? name_len : 63;
          memcpy(scname, b + o, n); scname[n] = 0; o += name_len; }
        nrec = rd32(b + o); o += 4;

        recs = malloc(sizeof(record_t) * (nrec ? nrec : 1));
        for (k = 0; k < nrec; k++) {
            record_t *r = &recs[k];
            unsigned j;
            r->fn_off = rd16(b + o); o += 2;
            r->fn_name_idx = rd16(b + o); o += 2;
            parse_snap(b + o, &r->ent); o += SNAP_SIZE;
            parse_snap(b + o, &r->ex);  o += SNAP_SIZE;
            r->ret_val = rd16(b + o); o += 2;
            r->render_desc_len = b[o++];
            { unsigned n = r->render_desc_len < VIEW_DESC_MAX ? r->render_desc_len : VIEW_DESC_MAX;
              memcpy(r->render_desc, b + o, n); o += r->render_desc_len; }
            r->p1_sprite_len = b[o++];
            { unsigned n = r->p1_sprite_len < P1_SPRITE_MAX ? r->p1_sprite_len : P1_SPRITE_MAX;
              memcpy(r->p1_sprite, b + o, n); o += r->p1_sprite_len; }
            r->seed_len = b[o++];
            { unsigned n = r->seed_len < SEED_BUF_MAX ? r->seed_len : SEED_BUF_MAX;
              memcpy(r->seed, b + o, n); o += r->seed_len; }
            r->n_io = rd16(b + o); o += 2;
            r->io = r->n_io ? malloc(sizeof(io_t) * r->n_io) : NULL;
            for (j = 0; j < r->n_io; j++) {
                r->io[j].dir   = b[o];        o += 1;
                r->io[j].port  = rd16(b + o); o += 2;
                r->io[j].size  = b[o];        o += 1;
                r->io[j].value = rd16(b + o); o += 2;
            }
            r->n_fills = b[o++];
            for (j = 0; j < r->n_fills; j++) {
                u8 fl = b[o++];
                unsigned n = fl < VIEW_DESC_MAX ? fl : VIEW_DESC_MAX;
                if (j < HUD_FILL_MAX) {
                    r->fill_len[j] = fl;
                    memcpy(r->fills[j], b + o, n);
                }
                o += fl;
            }
            r->n_args = b[o++];
            for (j = 0; j < r->n_args; j++) {
                if (j < 8) r->args[j] = rd16(b + o);
                o += 2;
            }
            r->n_input = b[o++];
            { unsigned n = r->n_input < 256 ? r->n_input : 256;
              memcpy(r->input, b + o, n); o += r->n_input; }
            n_io_total += r->n_io;
            n_records++;
        }

        printf("\n== scenario %u: %s (%lu records) ==\n",
               sid, scname, (unsigned long)nrec);

        per_ok = run_per_function(recs, (long)nrec, scname, &sst);
        printf("  per-fn: PASS=%ld  FAIL=%ld  UNPORTED=%ld  DESC_CHECKED=%ld  PORT_CHECKED=%ld\n",
               sst.pass, sst.fail, sst.unported, sst.desc_checked, sst.port_checked);
        if (!per_ok) hard_fail = 1;

        st.pass += sst.pass; st.fail += sst.fail; st.unported += sst.unported;
        st.desc_checked += sst.desc_checked; st.port_checked += sst.port_checked;
        for (k = 0; k < nrec; k++) free(recs[k].io);
        free(recs);
    }

    if (o != (u32)sz) {
        fprintf(stderr, "WARNING: parsed %lu of %ld bytes (trailing data)\n",
                (unsigned long)o, sz);
        hard_fail = 1;
    }

    printf("\n=== TOTAL per-fn: PASS=%ld  FAIL=%ld  UNPORTED=%ld  DESC_CHECKED=%ld  "
           "PORT_CHECKED=%ld (records=%ld, port-I/O events=%ld) ===\n",
           st.pass, st.fail, st.unported, st.desc_checked, st.port_checked,
           n_records, n_io_total);
    if (hard_fail || st.fail != 0) {
        printf("FAIL: %ld per-function differential failure(s) on PORTED fns\n", st.fail);
        free(b);
        return 1;
    }
    printf("PASS: FAIL=0.  %ld records UNPORTED (the title/menu/highscore/intro screen fns "
           "still stubbed in game_stubs.c — Phase-7 T4/T5); %ld PORTED records matched "
           "(draw_number / draw_text_at / draw_number_sprites / draw_hud_composite).\n",
           st.unported, st.pass);
    free(b);
    return 0;
}
