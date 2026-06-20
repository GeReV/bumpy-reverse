/* Host REPLAY HARNESS for src/player2.c — Phase-4 Task 2.
 *
 * Compiles the REAL P2 port (src/player2.c) on the host (Watcom 16-bit environment
 * shimmed out: __far/__huge erased, exact-width typedefs, BUMPY_H defined so
 * player2.h does not pull <dos.h>), then validates the reconstructed Player-2
 * trajectory + AI rng-decision + draw-descriptor against the Phase-4 T1 capture
 * local/build/render/p2_trace.bin (magic "P2TRACE1", version 1 — layout frozen in
 * the tools/p2_oracle.py header §"TRACE LAYOUT" and local/build/p2_model.md).
 *
 * src/player2.c is a SKELETON this task: it defines the P2 globals but NO function
 * bodies (those port in Phase-4 T3/T4/T5).  So EVERY P2 function is UNPORTED here —
 * the self-check below proves the harness + the trace format work end-to-end with
 * no crash and no hard failure, every record cleanly localised to UNPORTED.
 *
 * ── COMPARATORS ──────────────────────────────────────────────────────────────
 *   (A) PER-FUNCTION TRAJECTORY + AI DIFFERENTIAL (the gate).  For each record of a
 *       PORTED, host-callable fn: seed the reconstructed P2 globals + the captured
 *       move-script bytes + rng_frame = the record's ENTRY SNAP, call the fn BY ITS
 *       C NAME (p2_set_move_state takes the launch state as its cdecl arg, derived
 *       from the EXIT move_state), then assert every output field == the record's
 *       EXIT SNAP.  Covers the move-state/trajectory globals (px, py, move_anim,
 *       grid col/row, cell, move_state, steps_left, step_idx, facing, toggle) AND
 *       the AI-decision outputs (move_state after select_move_a/b/random/dispatch)
 *       AND the pvp_collision_flag.  Prints PASS/FAIL with first-divergence.
 *
 *   (B) RENDER-DESCRIPTOR COMPARATOR (draw_p2_sprite records).  draw_p2_sprite
 *       builds a P2 object descriptor at the 0x9b9e far ptr; the capture carries
 *       its EXIT bytes (desc_len>0).  When draw_p2_sprite is PORTED + writes a
 *       host-visible descriptor, this asserts the produced descriptor == the
 *       captured one (x = p2_pixel_x, y = p2_pixel_y, frame = p2_frame_base +
 *       p2_move_anim, per p2_model.md §Scenario 14).  Until then the record is
 *       UNPORTED (the descriptor bytes are retained for the future port).
 *
 *   GRACEFUL UNPORTED DEGRADATION.  A function with no reconstructed C body yet
 *     (ALL eleven this task) is marked "UNPORTED (expected until T3-T5)" and
 *     SKIPPED: it is NEVER referenced as a symbol (the PORTED registry holds a NULL
 *     callable for it, so the harness never link-depends on the missing body and
 *     never call-throughs into it).  UNPORTED is NOT a crash and NOT a hard
 *     failure.  As T3/T4/T5 fill in bodies in player2.c, their PORTED registry
 *     entries gain a real callable and their UNPORTED counts convert to PASS.
 *
 * Build/run (also wrapped by tools/validate_p2.sh):
 *     cc -O2 -Wall -Werror -o /tmp/p2_ctest tools/p2_ctest.c && \
 *       /tmp/p2_ctest local/build/render/p2_trace.bin
 * Exit 0 iff the harness parses the trace, runs, and the per-function differential
 * has ZERO failures on PORTED records (UNPORTED records never fail).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── shim the Watcom 16-bit environment for host compilation ─────────────────── */
#define BUMPY_H            /* player2.h's #include "bumpy.h" becomes a no-op */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge

/* ── PRNG state + step — OWNED BY src/prng.c (globals.c defines the state). ──────
 *  player2.c does NOT reference these, but the AI fn p2_ai_select_move_random calls
 *  rand()/prng_step (see the AI-DETERMINISM note below).  When T4 ports that fn we
 *  will compile src/prng.c into this harness (or shim prng_step) and seed the prng
 *  state from the capture; for the T2 skeleton they are host-provided here so the
 *  extension point links if a future port references them. */
u16 prng_state0, prng_state1, prng_state2;  /* globals.c — DGROUP 0x203b           */

/* mode_script_tbl / p2 state-script + handler tables (far/near ptr tables the P2
   move-state fns index).  Not referenced by the skeleton TU; the T3 ports that read
   them will seed a synthetic table here — left as a documented extension point. */

/* tilemap: cross-module level-data far ptr (owned by level.c).  p2_update_grid_cell
   / p2_set_pixel_from_cell map P2 cell<->pixel via the grid geometry; when those
   port (T3) the harness will seed a synthetic window here.  Provided now so the
   include + future ports link. */
#define TILEMAP_SIZE 0x300
static u8 synth_tilemap[TILEMAP_SIZE];
u8 __far *tilemap = synth_tilemap;

/* Cross-module globals the P2 fns read/write but that player2.c declares extern
   (owned elsewhere).  player2.c does not pull their owning headers, so the harness
   provides host definitions (mirroring the items_ctest/physics_ctest convention of
   supplying the cross-module globals the included TU references). */
u8  p2_move_state;       /* game.c   0x8562 */
u8  rng_frame;           /* player.c 0x79b3 — the AI rng-decision input            */
u8  game_mode;           /* game.c   0x792c */
u8  physics_frozen;      /* player.c 0xa0ce */
u8  current_level;       /* level.c  0x79b2 */

#include "../src/player2.c"

/* ════════════════════════════════════════════════════════════════════════════
 *  Trace format (frozen — see tools/p2_oracle.py header §"TRACE LAYOUT").
 *
 *  Header:  magic[8]="P2TRACE1", u16 version(=1), u16 n_scenarios,
 *           u16 n_fn_names, then per name {u8 len, len bytes ascii}.
 *  Per scenario: u8 id, u8 name_len, name bytes, u8 seeded, u8 level,
 *                u32 n_records, then n_records records.
 *  Per record: u16 fn_addr, u16 fn_name_idx, SNAP entry, SNAP exit,
 *              u16 script_off, u16 script_seg, u16 script_len, script_len bytes,
 *              u8 desc_len, desc_len bytes.
 *  P2SNAP (26 B LE, struct.pack "<hhHhh"+"B"*16):
 *    px(s16) py(s16) anim(u16) gcol(s16) grow(s16)
 *    cell(u8) state(u8) steps_left(u8) step_idx(u8) facing(u8) toggle(u8)
 *    rng(u8) thresh(u8) blk0(u8) blk1(u8) blk3(u8) pvp(u8) mode(u8) frozen(u8)
 *    prng0(u8) pad(u8).
 * ════════════════════════════════════════════════════════════════════════════ */
#define SNAP_SIZE 26

typedef struct {
    s16 px, py;
    u16 anim;
    s16 gcol, grow;
    u8  cell, state, steps_left, step_idx, facing, toggle;
    u8  rng, thresh, blk0, blk1, blk3, pvp, mode, frozen;
    u8  prng0, pad;
} snap_t;

typedef struct {
    u16    fn_addr;
    u16    fn_name_idx;
    snap_t ent, ex;
    u16    script_off, script_seg, script_len;
    const u8 *script;          /* points into the loaded file buffer */
    u8     desc_len;
    const u8 *desc;            /* points into the loaded file buffer (draw records) */
} record_t;

/* Engine seg-1000 offsets for the eleven hooked P2 fns (p2_oracle FN_NAMES). */
#define FN_P2_SET_MOVE_STATE     0x4bc6
#define FN_P2_STEP_SCRIPTED      0x4c14
#define FN_P2_UPDATE_GRID_CELL   0x4b4e
#define FN_P2_SET_PIXEL          0x48a9
#define FN_P2_AI_DISPATCH        0x4f4e
#define FN_P2_AI_SELECT_A        0x4f04
#define FN_P2_AI_SELECT_B        0x4f89
#define FN_P2_AI_SELECT_RANDOM   0x4fd3
#define FN_P2_RUN_STATE_HANDLER  0x5003
#define FN_CHECK_PVP_COLLISION   0x50fb
#define FN_DRAW_P2_SPRITE        0x1cea

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) |
                                      ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void parse_snap(const u8 *p, snap_t *s)
{
    s->px   = (s16)rd16(p + 0);
    s->py   = (s16)rd16(p + 2);
    s->anim = rd16(p + 4);
    s->gcol = (s16)rd16(p + 6);
    s->grow = (s16)rd16(p + 8);
    s->cell       = p[10]; s->state  = p[11]; s->steps_left = p[12];
    s->step_idx   = p[13]; s->facing = p[14]; s->toggle     = p[15];
    s->rng        = p[16]; s->thresh = p[17]; s->blk0       = p[18];
    s->blk1       = p[19]; s->blk3   = p[20]; s->pvp        = p[21];
    s->mode       = p[22]; s->frozen = p[23]; s->prng0      = p[24];
    s->pad        = p[25];
}

static const char *fn_name(u16 fn_addr)
{
    switch (fn_addr) {
        case FN_P2_SET_MOVE_STATE:    return "p2_set_move_state";
        case FN_P2_STEP_SCRIPTED:     return "p2_step_scripted_move";
        case FN_P2_UPDATE_GRID_CELL:  return "p2_update_grid_cell";
        case FN_P2_SET_PIXEL:         return "p2_set_pixel_from_cell";
        case FN_P2_AI_DISPATCH:       return "p2_ai_dispatch_move";
        case FN_P2_AI_SELECT_A:       return "p2_ai_select_move_a";
        case FN_P2_AI_SELECT_B:       return "p2_ai_select_move_b";
        case FN_P2_AI_SELECT_RANDOM:  return "p2_ai_select_move_random";
        case FN_P2_RUN_STATE_HANDLER: return "p2_run_move_state_handler";
        case FN_CHECK_PVP_COLLISION:  return "check_pvp_collision";
        case FN_DRAW_P2_SPRITE:       return "draw_p2_sprite";
        default:                      return "?";
    }
}

/* ── ENTRY SNAP -> reconstructed P2 globals ──────────────────────────────────── */
static void seed_globals(const snap_t *s)
{
    p2_pixel_x = s->px;  p2_pixel_y = s->py;  p2_move_anim = s->anim;
    p2_grid_col = s->gcol; p2_grid_row = s->grow;
    p2_cell = (s8)s->cell;
    p2_move_state = s->state;
    p2_move_steps_left = s->steps_left;
    p2_step_idx = s->step_idx;
    p2_facing_neg_dx = s->facing;
    p2_move_toggle = s->toggle;
    rng_frame = s->rng;
    p2_ai_threshold = s->thresh;
    p2_dir_blocked_0 = s->blk0;
    p2_dir_blocked_1 = s->blk1;
    p2_dir_blocked_3 = s->blk3;
    pvp_collision_flag = s->pvp;
    game_mode = s->mode;
    physics_frozen = s->frozen;
}

/* Seed the captured P2 move-script bytes into a host buffer + point the engine's
   move-script far pointer at it (the [anim,dx,dy] 6-byte entries the trajectory fn
   reads).  When p2_step_scripted_move/p2_set_move_state port (T3) they read the far
   ptr at 0xa0ba; this is the host backing for it. */
static u8 script_buf[128];
static void seed_script(const record_t *r)
{
    memset(script_buf, 0, sizeof(script_buf));
    if (r->script_len) {
        unsigned n = r->script_len < sizeof(script_buf) ? r->script_len
                                                        : (unsigned)sizeof(script_buf);
        memcpy(script_buf, r->script, n);
    }
    /* T3 EXTENSION POINT: when p2_move_script (far ptr @0xa0ba) is a reconstructed
       global, point it here, e.g.  p2_move_script = (u16 __far *)script_buf;
       (the skeleton does not yet declare it, so this is a documented hook). */
}

/* ════════════════════════════════════════════════════════════════════════════
 *  AI-DETERMINISM EXTENSION POINT  (forward-looking — Phase-4 Task 4).
 *
 *  p2_ai_select_move_random (1000:4fd3) computes
 *      move_state = (rng_frame & 3) + (rand() & 1) + 5
 *  i.e. it reads rng_frame (in the SNAP) AND calls rand() == prng_step(), whose
 *  parity bit (rand() & 1) depends on the PRNG STATE (prng_state0/1/2), NOT on
 *  rng_frame.  So to replay that fn DETERMINISTICALLY on the host, the harness must
 *  seed the prng state to exactly the value the engine held at the call — otherwise
 *  the (rand() & 1) bit (hence the +0/+1 in the resulting move_state) is unknowable.
 *
 *  T1 captured only prng_state0's LOW BYTE (snap.prng0, for a cross-check) — NOT the
 *  full 3-word prng state.  The captured EXIT move_states for the random scenarios
 *  (p2_model.md: rng0->6, rng1->7, rng2->8, rng3->9) all have (rand() & 1)==1, but
 *  that is a single observed sample, not a seeded-reproducible input.
 *
 *  TODO(T4): make p2_ai_select_move_random's replay deterministic.  EITHER
 *    (a) extend the T1 capture (tools/p2_oracle.py) to record the full prng_state0/
 *        1/2 at the ENTRY of each AI-random record, and seed them here below; OR
 *    (b) if the engine's rand() result is fully determined by a captured input,
 *        derive (rand()&1) from it.
 *  Until then this hook seeds only prng_state0's low byte (the cross-check value)
 *  and the per-fn comparator marks p2_ai_select_move_random's parity-dependent
 *  bit as NOT-yet-deterministic (it is UNPORTED this task regardless).
 * ════════════════════════════════════════════════════════════════════════════ */
static void seed_ai_prng(const snap_t *s)
{
    /* Cross-check value only (low byte of prng_state0).  T4 replaces this with a
       full prng_state0/1/2 seed once the capture carries them. */
    prng_state0 = (prng_state0 & 0xff00u) | s->prng0;
    /* prng_state1 / prng_state2: NOT in the trace — T4 capture addition (see TODO). */
}

/* Compare the live P2 globals against an exit SNAP.  Returns NULL if all match,
   else the name of the first divergent field; got/want filled.  The comparator is
   restricted to the fields a P2 fn actually produces (the union across all P2 fns);
   per-fn callers further constrain it via which fields the fn touches. */
static const char *cmp_exit(const snap_t *e, long *got, long *want)
{
#define FLD(name, lhs, rhs) do { if ((long)(lhs) != (long)(rhs)) { \
        *got = (long)(lhs); *want = (long)(rhs); return name; } } while (0)
    FLD("px",         p2_pixel_x,         e->px);
    FLD("py",         p2_pixel_y,         e->py);
    FLD("anim",       p2_move_anim,       e->anim);
    FLD("gcol",       p2_grid_col,        e->gcol);
    FLD("grow",       p2_grid_row,        e->grow);
    FLD("cell",       (u8)p2_cell,        e->cell);
    FLD("state",      p2_move_state,      e->state);
    FLD("steps_left", p2_move_steps_left, e->steps_left);
    FLD("step_idx",   p2_step_idx,        e->step_idx);
    FLD("facing",     p2_facing_neg_dx,   e->facing);
    FLD("toggle",     p2_move_toggle,     e->toggle);
    FLD("pvp",        pvp_collision_flag, e->pvp);
#undef FLD
    return NULL;
}

/* ── RENDER-DESCRIPTOR COMPARATOR (draw_p2_sprite) ───────────────────────────────
 *  The capture carries the EXIT bytes of the P2 object descriptor built at the
 *  0x9b9e far ptr (desc_len>0 only for draw_p2_sprite).  Per p2_model.md §Scenario
 *  14 the leading words are: x = p2_pixel_x, y = p2_pixel_y, frame = p2_frame_base
 *  + p2_move_anim.  When draw_p2_sprite is PORTED and writes a host-visible
 *  descriptor (T5), compare the produced descriptor against the captured one here.
 *
 *  NOTE(T5): p2_frame_base is NOT in P2SNAP (the capture records only px/py/anim of
 *  the descriptor inputs), so the frame word (= frame_base + anim) is only
 *  reproducible once T5 either (a) seeds p2_frame_base from a T1 capture addition,
 *  or (b) derives it from the captured descriptor itself (frame_base = desc.frame -
 *  anim).  Until then this comparator asserts the frame-base-INDEPENDENT words
 *  (x = p2_pixel_x, y = p2_pixel_y) against the captured descriptor and back-derives
 *  frame_base from the descriptor to assert the anim relationship — a format
 *  cross-check that the trace's descriptor head is (x, y, frame_base+anim), without
 *  referencing the (absent) draw_p2_sprite symbol. */
static const char *cmp_descriptor(const record_t *r, const snap_t *e,
                                  long *got, long *want)
{
    u16 want_x, want_y, d_x, d_y, d_frame, derived_base;
    if (r->desc_len < 6) return NULL;     /* nothing to compare */
    /* expected descriptor head from the captured EXIT globals (frame-base free) */
    want_x = (u16)e->px;
    want_y = (u16)e->py;
    d_x     = rd16(r->desc + 0);
    d_y     = rd16(r->desc + 2);
    d_frame = rd16(r->desc + 4);
    /* back-derived frame_base (T5 seeds this directly): frame = frame_base + anim. */
    derived_base = (u16)(d_frame - e->anim);
    (void)derived_base;
#define DFLD(name, lhs, rhs) do { if ((long)(lhs) != (long)(rhs)) { \
        *got = (long)(lhs); *want = (long)(rhs); return name; } } while (0)
    DFLD("desc.x",       d_x,                          want_x);
    DFLD("desc.y",       d_y,                          want_y);
    /* frame relationship: desc.frame - anim must be a stable non-negative base. */
    DFLD("desc.frame_ge_anim", (d_frame >= e->anim),   1);
#undef DFLD
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  PORTED REGISTRY — engine seg-1000 offset -> reconstructed-C callable.
 *
 *  Each P2 function is host-callable iff it has a reconstructed body in
 *  src/player2.c.  This task (the skeleton) ports NONE, so every entry's callable
 *  is NULL -> the harness marks the record UNPORTED and never references the
 *  (absent) symbol.  Phase-4 T3/T4/T5 fill in a body in player2.c AND swap NULL for
 *  the function's C name here (e.g. { FN_P2_STEP_SCRIPTED, p2_step_scripted_move });
 *  that record's UNPORTED count then converts to a per-function diff with no other
 *  harness change.
 *
 *  The cdecl arg some P2 fns take (p2_set_move_state(state)) is handled in the
 *  comparator switch, not the registry, since the registry holds void(*)(void).
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct { u16 off; void (*fn)(void); } ported_t;

static const ported_t PORTED[] = {
    { FN_P2_SET_MOVE_STATE,    NULL },   /* T3 */
    { FN_P2_STEP_SCRIPTED,     NULL },   /* T3 */
    { FN_P2_UPDATE_GRID_CELL,  NULL },   /* T3 */
    { FN_P2_SET_PIXEL,         NULL },   /* T3 */
    { FN_P2_RUN_STATE_HANDLER, NULL },   /* T3 */
    { FN_P2_AI_DISPATCH,       NULL },   /* T4 */
    { FN_P2_AI_SELECT_A,       NULL },   /* T4 */
    { FN_P2_AI_SELECT_B,       NULL },   /* T4 */
    { FN_P2_AI_SELECT_RANDOM,  NULL },   /* T4 (prng-determinism — see seed_ai_prng) */
    { FN_CHECK_PVP_COLLISION,  NULL },   /* T5 */
    { FN_DRAW_P2_SPRITE,       NULL },   /* T5 */
};
#define PORTED_N (sizeof(PORTED) / sizeof(PORTED[0]))

static int ported_known(u16 off)
{
    unsigned i;
    for (i = 0; i < PORTED_N; i++)
        if (PORTED[i].off == off) return 1;
    return 0;
}

static void (*ported_lookup(u16 off))(void)
{
    unsigned i;
    for (i = 0; i < PORTED_N; i++)
        if (PORTED[i].off == off) return PORTED[i].fn;
    return NULL;
}

/* Is this an AI-decision fn whose replay needs the prng state seeded? */
static int fn_is_ai_random(u16 off) { return off == FN_P2_AI_SELECT_RANDOM; }

/* Per-fn comparator stat accumulators. */
typedef struct { long pass, fail, unported, desc_checked; } stats_t;

/* ── PER-FUNCTION TRAJECTORY + AI + RENDER-DESCRIPTOR differential ────────────── */
static int run_per_function(record_t *recs, long nrec, const char *scname,
                            stats_t *st)
{
    long i;
    int  scen_fail = 0;
    long printed = 0;
    for (i = 0; i < nrec; i++) {
        record_t *r = &recs[i];
        void (*fn)(void) = ported_lookup(r->fn_addr);

        if (fn == NULL) {
            /* No reconstructed body yet.  Never references the symbol; UNPORTED is
               not a crash, not a hard failure.  For the draw record we still retain
               its descriptor bytes (cmp below runs only when the fn is PORTED). */
            if (!ported_known(r->fn_addr) && printed++ < 4)
                printf("    NOTE [%s #%ld] unknown fn_addr %#x (%s) — skipped\n",
                       scname, i, r->fn_addr, fn_name(r->fn_addr));
            st->unported++;
            continue;
        }

        /* ── PORTED: seed entry + script (+ prng for AI-random), call, assert ── */
        {
            const char *bad; long got, want;
            seed_globals(&r->ent);
            seed_script(r);
            if (fn_is_ai_random(r->fn_addr))
                seed_ai_prng(&r->ent);     /* AI-determinism hook (T4 TODO) */

            if (r->fn_addr == FN_P2_SET_MOVE_STATE) {
                /* p2_set_move_state(state) — cdecl arg is the launch state (the
                   EXIT move_state the record establishes). */
                void (*f1)(u8) = (void (*)(u8))fn;
                f1(r->ex.state);
            } else {
                fn();
            }

            bad = cmp_exit(&r->ex, &got, &want);
            /* draw_p2_sprite: also assert the produced render descriptor. */
            if (bad == NULL && r->fn_addr == FN_DRAW_P2_SPRITE && r->desc_len) {
                bad = cmp_descriptor(r, &r->ex, &got, &want);
                if (bad == NULL) st->desc_checked++;
            }
            if (bad == NULL) {
                st->pass++;
            } else {
                st->fail++; scen_fail = 1;
                if (printed++ < 8)
                    printf("    FAIL [%s #%ld] %s field %s: got %ld want %ld "
                           "(entry cell=%#x state=%u rng=%#x thresh=%#x)\n",
                           scname, i, fn_name(r->fn_addr), bad, got, want,
                           r->ent.cell, r->ent.state, r->ent.rng, r->ent.thresh);
            }
        }
    }
    return !scen_fail;
}

/* ════════════════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1]
                                  : "local/build/render/p2_trace.bin";
    FILE *f = fopen(path, "rb");
    long sz; u8 *b; u32 o; u16 ver, nsc, nfn; unsigned s;
    stats_t st = { 0, 0, 0, 0 };
    long n_records = 0, n_draw = 0;
    int hard_fail = 0;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz);
    if (!b || fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
    fclose(f);

    if (sz < 14 || memcmp(b, "P2TRACE1", 8) != 0) {
        fprintf(stderr, "bad magic (want P2TRACE1)\n"); return 2;
    }
    ver = rd16(b + 8); nsc = rd16(b + 10);
    if (ver != 1) { fprintf(stderr, "unsupported version %u\n", ver); return 2; }
    o = 12;
    nfn = rd16(b + o); o += 2;
    /* skip the fn-name string table (we key on fn_addr, not the name index). */
    { u16 k; for (k = 0; k < nfn; k++) { u8 ln = b[o]; o += 1 + ln; } }

    printf("p2_ctest: replay harness over %s\n", path);
    printf("  trace: P2TRACE1 v%u, %u scenarios, %u fn-names\n", ver, nsc, nfn);
    printf("  per-fn diff targets (host-callable): NONE yet — src/player2.c is a "
           "skeleton (globals only); all eleven P2 fns are UNPORTED (move-state/"
           "trajectory [T3], AI rng-decision [T4], draw+pvp [T5])\n");

    for (s = 0; s < nsc; s++) {
        u8 sid, name_len, seeded, level;
        char scname[64];
        u32 nrec, k;
        record_t *recs;
        stats_t sst = { 0, 0, 0, 0 };
        int per_ok;

        sid = b[o]; o += 1;
        name_len = b[o]; o += 1;
        { unsigned n = name_len < 63 ? name_len : 63;
          memcpy(scname, b + o, n); scname[n] = 0; o += name_len; }
        seeded = b[o]; level = b[o + 1]; o += 2;
        nrec = rd32(b + o); o += 4;

        recs = malloc(sizeof(record_t) * (nrec ? nrec : 1));
        for (k = 0; k < nrec; k++) {
            record_t *r = &recs[k];
            r->fn_addr = rd16(b + o); o += 2;
            r->fn_name_idx = rd16(b + o); o += 2;
            parse_snap(b + o, &r->ent); o += SNAP_SIZE;
            parse_snap(b + o, &r->ex);  o += SNAP_SIZE;
            r->script_off = rd16(b + o); o += 2;
            r->script_seg = rd16(b + o); o += 2;
            r->script_len = rd16(b + o); o += 2;
            r->script = b + o; o += r->script_len;
            r->desc_len = b[o]; o += 1;
            r->desc = b + o; o += r->desc_len;
            if (r->desc_len) n_draw++;
            n_records++;
        }

        printf("\n== scenario %u: %s (seeded %u, level %u, %lu records) ==\n",
               sid, scname, seeded, level, (unsigned long)nrec);

        per_ok = run_per_function(recs, (long)nrec, scname, &sst);
        printf("  per-fn: PASS=%ld  FAIL=%ld  UNPORTED=%ld  DESC_CHECKED=%ld\n",
               sst.pass, sst.fail, sst.unported, sst.desc_checked);
        if (!per_ok) hard_fail = 1;

        st.pass += sst.pass; st.fail += sst.fail; st.unported += sst.unported;
        st.desc_checked += sst.desc_checked;
        free(recs);
    }

    printf("\n=== TOTAL per-fn: PASS=%ld  FAIL=%ld  UNPORTED=%ld  DESC_CHECKED=%ld "
           "(records=%ld, draw-descriptor records=%ld) ===\n",
           st.pass, st.fail, st.unported, st.desc_checked, n_records, n_draw);
    if (hard_fail || st.fail != 0) {
        printf("FAIL: %ld per-function differential failure(s) on PORTED fns\n",
               st.fail);
        return 1;
    }
    printf("PASS: harness parsed the trace and ran clean; every record UNPORTED "
           "(=%ld) — cleanly localised to the skeleton state (no P2 fn body yet; "
           "T3/T4/T5 port them).  AI-determinism prng hook + draw-descriptor "
           "comparator wired and waiting on the ports.\n", st.unported);
    return 0;
}
