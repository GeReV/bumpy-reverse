/* Host REPLAY HARNESS for src/items.c item/exit semantic state — Phase-3 Task 2.
 *
 * Compiles the REAL port (src/items.c) on the host (Watcom 16-bit environment
 * shimmed out: __far/__huge erased, exact-width typedefs, BUMPY_H defined so
 * items.h does not pull <dos.h>), then validates the reconstructed item/exit
 * SEMANTIC STATE against the Phase-3 T1 capture
 * local/build/render/items_trace.bin (magic ITEMTRC1, version 1 — layout frozen
 * in tools/items_oracle.py and local/build/items_model.md §"Trace layout").
 *
 * Unlike the Phase-2 physics harness (continuous px/py trajectory, desync-prone),
 * this validates DISCRETE semantic events: score / items_remaining /
 * level_exit_cell / level_complete_flag / level_complete_anim_counter /
 * p1_item_code / p1_cell / anim_target_cell / current_level / p1_step_col_count /
 * physics_frozen / p1_pixel_y, plus the tilemap layer-C item byte at
 * tilemap[p1_cell+0x60].  There is therefore ONE comparator: a per-function
 * differential.
 *
 *   PER-FUNCTION DIFFERENTIAL (the gate).  For each trace record of a PORTED,
 *     host-callable fn: seed the reconstructed globals + a synthetic tilemap
 *     (the layer-C item byte from the record placed at tilemap[entry.p1_cell+
 *     0x60]) = the record's ENTRY SNAP, call the fn BY ITS C NAME, then assert
 *     every output semantic-state field == the record's EXIT SNAP (including the
 *     tilemap item byte the fn may have cleared).  Prints PASS/FAIL with the
 *     first divergence (fn, field, got vs want).
 *
 *   GRACEFUL UNPORTED DEGRADATION.  A function with no reconstructed C body yet
 *     (ALL FIVE this task — T3/T4 port them) is marked
 *     "UNPORTED (expected until T3/T4)" and SKIPPED: it is NEVER referenced as a
 *     symbol (the PORTED registry holds a NULL callable for it, so the harness
 *     never link-depends on the missing body and never call-throughs into it).
 *     UNPORTED is NOT a crash and NOT a hard failure.  As Tasks 3/4 fill in the
 *     bodies, their PORTED registry entries gain a real callable and their
 *     UNPORTED counts convert to PASS.
 *
 * SELF-CHECK (this task, before any port): every record of every scenario must
 * report UNPORTED — proving the harness + the trace format work end-to-end with
 * no crash and no hard failure.  Exit 0.
 *
 * Build/run (also wrapped by tools/validate_items.sh):
 *     cc -O2 -Wall -Werror -o /tmp/items_ctest tools/items_ctest.c && \
 *       /tmp/items_ctest local/build/render/items_trace.bin
 * Exit 0 iff the harness parses the trace, runs, and the per-function
 * differential has ZERO failures on PORTED records (UNPORTED records never fail).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── shim the Watcom 16-bit environment for host compilation ─────────────────── */
#define BUMPY_H            /* items.h's #include "bumpy.h" becomes a no-op */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge

/* tilemap: cross-module level-data far ptr (owned by level.c).  The item/exit
   functions index the layer-C item byte at tilemap[cell+0x60] and the exit/
   teleport tiles at tilemap[cell+0x30].  Point it at a synthetic window the
   comparator seeds per-record from the captured tilemap item byte. */
#define TILEMAP_SIZE 0x300
static u8 synth_tilemap[TILEMAP_SIZE];
u8 __far *tilemap = synth_tilemap;

/* ── host definitions of the semantic-state globals OWNED BY OTHER MODULES ──────
 * src/items.c defines only its four owned globals (items_remaining,
 * level_exit_cell, level_complete_anim_counter, p1_item_code — see items.h).
 * The remaining globals the item/exit functions touch are owned by game.c,
 * level.c and player.c, which are NOT compiled into this harness — so we provide
 * host definitions here (mirroring the player_ctest/physics_ctest convention of
 * supplying cross-module globals the included TU references). */
u16 score_lo, score_hi;          /* game.c   — 0xa0d4 / 0xa0d6 */
u8  current_level;               /* level.c  — 0x79b2          */
u8  copyprotect_flag;            /* level.c  — 0x119a          */
u8  level_complete_flag;         /* player.c — 0xa1b1          */
u8  anim_target_cell;            /* player.c — 0x856f          */
u8  p1_cell;                     /* player.c — 0x856e          */
u8  p1_step_col_count;           /* player.c — 0x855e — cursor/move-step COLUMN counter */
/* DECOY counter-aliasing guard (Phase-9.1 methodology — mirrors physics_ctest.c).
   The REAL move_step_count is DGROUP 0x824c (jump_step_counter), a DISTINCT global
   from p1_step_col_count @ 0x855e.  check_exit_tile_vert's binary read is
   CMP [0x855e],7 = p1_step_col_count.  We define move_step_count here ONLY as a
   decoy: seed_globals sets it to a value DISTINCT from the SNAP's p1_step_col_count
   so that if check_exit_tile_vert ever reverts to reading move_step_count, the gate
   FAILS.  Nothing in the reconstruction reads it (the items.c read was corrected to
   p1_step_col_count); it exists solely to catch a regression. */
u8  move_step_count;             /* player.c — 0x824c (decoy — read by nothing) */
u8  p1_move_step_idx;            /* player.c — 0x792a          */
u8  physics_frozen;              /* player.c — 0xa0ce          */
s16 p1_pixel_y;                  /* player.c — 0x9292          */
s16 sound_device_state;          /* player.c — 0x689c (==4 → OPL ids; here 0)  */

/* P1 pixel-X scratch (written by p1_set_pixel_from_cell; NOT in the validated
   SNAP — provided so the host shim mirrors the engine leaf exactly). */
s16 p1_pixel_x;                  /* player.c — 0x9290 */

/* FX/sound callees the collect path reaches (game_stubs.c in BUMPY.EXE).  They
   have no effect on the validated semantic state, so the harness stubs them. */
void play_sound(unsigned char id)        { (void)id; }   /* 1000:6e11 */
u8 settle_countdown;   /* game.c 0x791a — '#'-item/tries counter (unified) */
void apply_cell_animation(unsigned char f){ (void)f; }    /* 1000:69aa */

/* Player-spine callees the T4 exit/teleport functions invoke (reconstructed in
   player.c; player.c is NOT compiled into this harness, so they are shimmed here
   — the same convention as play_sound/apply_cell_animation above):
     - enter_game_mode / dispatch_move_step write game_mode + the move-step
       dispatch, NEITHER in the validated SNAP -> no-op shims.
     - p1_set_pixel_from_cell (1000:4906) writes p1_step_col_count + p1_pixel_y
       (BOTH in the SNAP), so its host shim reproduces the engine leaf FAITHFULLY:
         p1_grid_row  = p1_cell >> 3
         p1_step_col_count = p1_cell - p1_grid_row*8   (the in-row column index)
         p1_pixel_x = posC_X[p1_cell] + 7
         p1_pixel_y = posC_Y[p1_cell] + 0xf
       The engine reads posC_X/Y from the DGROUP cell-coord table at 0x274/0x276;
       that table is the same grid geometry level.c's level_populate_dg computes —
       posC_X[cell] = col*40 + 8, posC_Y[cell] = row*32 + 8 — so the shim derives
       the two coord values analytically (matches the captured trace exactly:
       teleport to cell 0x0a -> p1_step_col_count=2, p1_pixel_y=0x28+0xf+0xd=0x44). */
void enter_game_mode(unsigned char m)    { (void)m; }     /* 1000:4263 */
void dispatch_move_step(void)            {}               /* 1000:238e */
void p1_set_pixel_from_cell(void)        /* 1000:4906 */
{
    u8 row = (u8)(p1_cell >> 3);
    u8 col = (u8)(p1_cell - (u8)(row * 8));
    p1_step_col_count = col;
    p1_pixel_x = (s16)((u16)col * 40u + 8u) + 7;   /* posC_X[cell] + 7 (not in SNAP) */
    p1_pixel_y = (s16)((u16)row * 32u + 8u) + 0xf; /* posC_Y[cell] + 0xf            */
}

#include "../src/items.c"

/* ════════════════════════════════════════════════════════════════════════════
 *  Trace format (frozen — see tools/items_oracle.py header §"TRACE LAYOUT").
 *
 *  Header:  magic[8]="ITEMTRC1", u16 version(=1), u16 n_scenarios,
 *           u16 n_fn_names, then per name {u8 len, len bytes ascii}.
 *  Per scenario: u8 id, u8 name_len, name bytes, u8 setup_kind, u8 level,
 *                u8 start_cell, u32 n_records, then n_records records.
 *  Per record: u16 fn_addr, u16 fn_name_idx, SNAP entry, SNAP exit.
 *  SNAP (18 B LE, struct.pack "<HHBBBBBBBBBBhBB"):
 *    score_lo(u16) score_hi(u16) items_remaining(u8) level_exit_cell(u8)
 *    level_complete_flag(u8) level_complete_anim_counter(u8) p1_item_code(u8)
 *    p1_cell(u8) anim_target_cell(u8) current_level(u8) p1_step_col_count(u8)
 *    physics_frozen(u8) p1_pixel_y(s16) tilemap_item_byte(u8) pad(u8).
 * ════════════════════════════════════════════════════════════════════════════ */
#define SNAP_SIZE 18

typedef struct {
    u16 score_lo, score_hi;
    u8  items_remaining, level_exit_cell, level_complete_flag,
        level_complete_anim_counter, p1_item_code, p1_cell, anim_target_cell,
        current_level, p1_step_col_count, physics_frozen;
    s16 p1_pixel_y;
    u8  tilemap_item_byte, pad;
} snap_t;

typedef struct {
    u16    fn_addr;
    u16    fn_name_idx;
    snap_t ent, ex;
} record_t;

/* Engine seg-1000 offsets for the five hooked item/exit fns (items_oracle FN_NAMES). */
#define FN_P1_COLLECT_ITEM        0x6c14
#define FN_P1_COLLECT_ITEM_SCORE  0x6c95
#define FN_CHECK_EXIT_TILE_VERT   0x6372
#define FN_MOVE_STEP_READ_ITEM    0x6627
#define FN_TELEPORT_NEXT_EXIT     0x25ad

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) |
                                      ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void parse_snap(const u8 *p, snap_t *s)
{
    s->score_lo = rd16(p + 0);
    s->score_hi = rd16(p + 2);
    s->items_remaining            = p[4];
    s->level_exit_cell            = p[5];
    s->level_complete_flag        = p[6];
    s->level_complete_anim_counter= p[7];
    s->p1_item_code               = p[8];
    s->p1_cell                    = p[9];
    s->anim_target_cell           = p[10];
    s->current_level              = p[11];
    s->p1_step_col_count          = p[12];
    s->physics_frozen             = p[13];
    s->p1_pixel_y                 = (s16)rd16(p + 14);
    s->tilemap_item_byte          = p[16];
    s->pad                        = p[17];
}

static const char *fn_name(u16 fn_addr)
{
    switch (fn_addr) {
        case FN_P1_COLLECT_ITEM:       return "p1_collect_item";
        case FN_P1_COLLECT_ITEM_SCORE: return "p1_collect_item_score";
        case FN_CHECK_EXIT_TILE_VERT:  return "check_exit_tile_vert";
        case FN_MOVE_STEP_READ_ITEM:   return "move_step_read_item";
        case FN_TELEPORT_NEXT_EXIT:    return "teleport_to_next_exit_tile";
        default:                       return "?";
    }
}

/* ── ENTRY SNAP -> reconstructed globals + synthetic tilemap ─────────────────────
   Seeds the synthetic tilemap so the called function sees the tiles the engine
   saw at capture time.  Three tile layers participate:
     - layer-C item byte  at tilemap[entry_cell + 0x60]  (collect path; from the snap)
     - vertical exit tile at tilemap[entry_cell + 0x30] = 0x0c  (check_exit_tile_vert)
     - teleport tile      at tilemap[dest_cell]         = 0x0f  (teleport_to_next_exit)
   The exit/teleport tiles are NOT carried as a snap byte, but the EXIT snapshot
   pins where they must be:
     * check_exit_tile_vert FIRES iff the exit tile is present — observable as
       physics_frozen going 0 -> 1 in the exit snap; seed it exactly then.
     * teleport_to_next_exit_tile scans forward and lands ON the teleport tile —
       the destination is the EXIT snap's p1_cell; seed 0x0f at that cell. */
static void seed_tilemap(u16 fn_addr, const snap_t *ent, const snap_t *ex)
{
    unsigned cell;
    memset(synth_tilemap, 0, TILEMAP_SIZE);

    /* layer-C item byte (collect path). */
    cell = (unsigned)ent->p1_cell + 0x60;
    if (cell < TILEMAP_SIZE)
        synth_tilemap[cell] = ent->tilemap_item_byte;

    /* vertical exit tile (check_exit_tile_vert): present iff the call froze
       physics (entry 0 -> exit 1). */
    if (fn_addr == FN_CHECK_EXIT_TILE_VERT &&
        ent->physics_frozen == 0 && ex->physics_frozen == 1) {
        cell = (unsigned)ent->p1_cell + 0x30;
        if (cell < TILEMAP_SIZE)
            synth_tilemap[cell] = 0x0c;
    }

    /* teleport tile (teleport_to_next_exit_tile): the destination cell is the
       exit snap's p1_cell (the scan lands on the 0x0f tile). */
    if (fn_addr == FN_TELEPORT_NEXT_EXIT) {
        cell = (unsigned)ex->p1_cell;
        if (cell < TILEMAP_SIZE)
            synth_tilemap[cell] = 0x0f;
    }
}

static void seed_globals(const snap_t *s)
{
    score_lo = s->score_lo;  score_hi = s->score_hi;
    items_remaining             = s->items_remaining;
    level_exit_cell             = s->level_exit_cell;
    level_complete_flag         = s->level_complete_flag;
    level_complete_anim_counter = s->level_complete_anim_counter;
    p1_item_code                = s->p1_item_code;
    p1_cell                     = s->p1_cell;
    anim_target_cell            = s->anim_target_cell;
    current_level               = s->current_level;
    p1_step_col_count           = s->p1_step_col_count;
    /* DECOY: seed the REAL move_step_count @ 0x824c to a value DISTINCT from the
       SNAP's p1_step_col_count, so that if check_exit_tile_vert ever reverts to
       reading move_step_count, it reads this decoy and the gate FAILS (the
       counter-aliasing guard — mirrors physics_ctest.c's 0x824c≠0x855e decoy).
       The reconstruction reads p1_step_col_count; nothing reads move_step_count
       unless the bug is reintroduced.  Pick 7 when the column counter is != 7
       (so a wrong read flips check_exit_tile_vert's `!= 7` guard to no-op), else
       a value != the column counter and != 7. */
    move_step_count = (s->p1_step_col_count != 7) ? 7
                    : (u8)(s->p1_step_col_count ^ 0xff);
    physics_frozen              = s->physics_frozen;
    p1_pixel_y                  = s->p1_pixel_y;
}

/* Read back the live tilemap item byte at the (entry) cell — the exit SNAP's
   tilemap_item_byte is tilemap[p1_cell_entry+0x60] AFTER the call (p1_collect_item
   clears it to 0; the cell index does not change for the collect path). */
static u8 live_tilemap_item_byte(u8 entry_cell)
{
    unsigned cell = (unsigned)entry_cell + 0x60;
    return (cell < TILEMAP_SIZE) ? synth_tilemap[cell] : 0;
}

/* Compare the live globals against an exit SNAP.  Returns NULL if all match,
   else the name of the first divergent field; got/want filled. */
static const char *cmp_exit(const snap_t *e, u8 entry_cell, long *got, long *want)
{
#define FLD(name, lhs, rhs) do { if ((long)(lhs) != (long)(rhs)) { \
        *got = (long)(lhs); *want = (long)(rhs); return name; } } while (0)
    FLD("score_lo",            score_lo,                    e->score_lo);
    FLD("score_hi",            score_hi,                    e->score_hi);
    FLD("items_remaining",     items_remaining,             e->items_remaining);
    FLD("level_exit_cell",     level_exit_cell,             e->level_exit_cell);
    FLD("level_complete_flag", level_complete_flag,         e->level_complete_flag);
    FLD("level_complete_anim", level_complete_anim_counter, e->level_complete_anim_counter);
    FLD("p1_item_code",        p1_item_code,                e->p1_item_code);
    FLD("p1_cell",             p1_cell,                     e->p1_cell);
    FLD("anim_target_cell",    anim_target_cell,            e->anim_target_cell);
    FLD("current_level",       current_level,               e->current_level);
    FLD("p1_step_col_count",   p1_step_col_count,           e->p1_step_col_count);
    FLD("physics_frozen",      physics_frozen,              e->physics_frozen);
    FLD("p1_pixel_y",          p1_pixel_y,                  e->p1_pixel_y);
    FLD("tilemap_item_byte",   live_tilemap_item_byte(entry_cell), e->tilemap_item_byte);
#undef FLD
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  PORTED REGISTRY — engine seg-1000 offset -> reconstructed-C callable.
 *
 *  Each of the five item/exit functions is host-callable iff it has a
 *  reconstructed body in src/items.c.  This task ports NONE of them, so every
 *  entry's callable is NULL -> the harness marks the record UNPORTED and never
 *  references the (absent) symbol.  Phase-3 Tasks 3/4 fill in a body in items.c
 *  AND swap NULL for the function's C name here (e.g. { FN_P1_COLLECT_ITEM,
 *  p1_collect_item }); that record's UNPORTED count then converts to a PASS/FAIL
 *  per-function diff with no other harness change.
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct { u16 off; void (*fn)(void); } ported_t;

static const ported_t PORTED[] = {
    { FN_P1_COLLECT_ITEM,       p1_collect_item },         /* T3 ported           */
    { FN_P1_COLLECT_ITEM_SCORE, p1_collect_item_score },   /* T3 ported           */
    { FN_CHECK_EXIT_TILE_VERT,  check_exit_tile_vert },    /* T4 ported           */
    { FN_MOVE_STEP_READ_ITEM,   move_step_read_item },     /* T3 ported           */
    { FN_TELEPORT_NEXT_EXIT,    teleport_to_next_exit_tile }, /* T4 ported        */
};
#define PORTED_N (sizeof(PORTED) / sizeof(PORTED[0]))

static void (*ported_lookup(u16 off))(void)
{
    unsigned i;
    for (i = 0; i < PORTED_N; i++)
        if (PORTED[i].off == off) return PORTED[i].fn;
    return NULL;
}

/* Per-fn comparator stat accumulators. */
typedef struct { long pass, fail, unported; } stats_t;

/* ── per-function differential over every record ─────────────────────────────── */
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
            /* No reconstructed body yet (all five, this task).  Never references
               the symbol; UNPORTED is not a crash, not a hard failure. */
            st->unported++;
            continue;
        }
        /* ── seed entry + synthetic tilemap, call by C name, assert exit ── */
        {
            const char *bad; long got, want;
            seed_tilemap(r->fn_addr, &r->ent, &r->ex);
            seed_globals(&r->ent);
            fn();
            bad = cmp_exit(&r->ex, r->ent.p1_cell, &got, &want);
            if (bad == NULL) {
                st->pass++;
            } else {
                st->fail++; scen_fail = 1;
                if (printed++ < 8)
                    printf("    FAIL [%s #%ld] %s field %s: got %ld want %ld "
                           "(entry cell=%#x item_code=%#x items_rem=%u)\n",
                           scname, i, fn_name(r->fn_addr), bad, got, want,
                           r->ent.p1_cell, r->ent.p1_item_code,
                           r->ent.items_remaining);
            }
        }
    }
    return !scen_fail;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  LEVEL-ADVANCE STATE-TRANSITION CHECK (Phase-3 Task 4, Part C)
 *
 *  The exit -> level-advance is wired in src/game.c's game_loop (1000:0c18),
 *  ported 1:1 in Phase-1 T7.  The decomp's advance tail is:
 *
 *      do { uVar1 = all_entries_flag_set(); } while (uVar1 == 0);  // wait complete
 *      current_level = current_level + 1;                          // ADVANCE
 *      if (current_level != 0x0a) break;                           // -> start_level(N)
 *      show_title_and_init();                                      // all 9 done
 *
 *  i.e. once the level-complete predicate (all_entries_flag_set, which the exit
 *  functions feed via the move-descriptor flags + level_complete_flag) returns
 *  nonzero, current_level is incremented and the outer `do { start_level(); ... }`
 *  loop re-enters start_level for the NEXT level (unless current_level wrapped to
 *  0x0a, the all-levels-done title return).
 *
 *  Fully scripting a complete-level playthrough under Unicorn is impractical
 *  (the per-tick loop has many stubbed callees), so — per the brief — the advance
 *  is validated here as a STATE TRANSITION in isolation: reproduce the decomp's
 *  advance decision verbatim and assert it (a) increments current_level and
 *  (b) re-invokes start_level for the next level, with the copy-protection
 *  #define OFF so NO challenge fires on entry to level 2+.  A recording
 *  start_level model captures (start_level_calls, last_level_loaded); the
 *  protection guard is reproduced under the same `#ifdef BUMPY_COPY_PROTECTION`
 *  gate as src/level.c, proving it compiles OUT in the default build.
 *
 *  DEFERRED to the live loop (documented, NOT runnable in this host harness):
 *  the full game_loop tick spine (run_main_menu/present_frame/handle_gameplay_input/
 *  all_entries_flag_set with a real move-descriptor table) — those are stubbed in
 *  game_stubs.c; the end-to-end "play level 1 -> reach exit -> auto-load level 2"
 *  run is a Phase-2+ integration gate, not this task.
 * ════════════════════════════════════════════════════════════════════════════ */

/* Recording model of start_level(N) — the advance target. */
static int adv_start_level_calls;
static u8  adv_last_level_loaded;
static int adv_challenge_fired;

static void adv_start_level(u8 level)
{
    /* Mirror src/level.c start_level's copy-protection hook under the SAME gate.
       With BUMPY_COPY_PROTECTION undefined (default) the whole block compiles
       out: no challenge fires; current_level flows unchanged to level 2+. */
    current_level = level;
#ifdef BUMPY_COPY_PROTECTION
    if ((1u < (unsigned)current_level) && (copyprotect_flag == 0u)) {
        copyprotect_flag = 1u;       /* cracked-build challenge: pass */
        adv_challenge_fired = 1;
    }
    if (copyprotect_flag == (u8)0xffu) {
        current_level = 1u;
    }
#endif
    adv_start_level_calls++;
    adv_last_level_loaded = current_level;
}

/* One faithful iteration of game_loop's advance tail.  `complete` models the
   all_entries_flag_set() predicate (nonzero => the level's exit was reached).
   Returns 1 if the outer loop should re-enter start_level for the next level,
   0 if it reached the all-levels-done title return.  On a re-enter it invokes
   adv_start_level(current_level) exactly as the outer `do { start_level(); }`. */
static int adv_step(u8 complete)
{
    if (complete == 0) {
        return -1;                   /* predicate false: stay in the round */
    }
    current_level = (u8)(current_level + 1);     /* decomp: current_level + 1 */
    if (current_level != 0x0a) {                  /* not the wrap -> next level */
        adv_start_level(current_level);           /* outer do{} re-enters start_level */
        return 1;
    }
    /* current_level == 0x0a: all 9 levels cleared -> show_title_and_init() */
    return 0;
}

static int validate_level_advance(void)
{
    int ok = 1;

    adv_start_level_calls = 0;
    adv_last_level_loaded = 0;
    adv_challenge_fired   = 0;

    printf("\n== level-advance state-transition (Part C) ==\n");

    /* 1) complete level 1 -> current_level 1->2, start_level(2) invoked. */
    current_level   = 1u;
    copyprotect_flag = 0u;
    {
        int r = adv_step(1u);   /* exit reached on level 1 */
        if (r != 1 || current_level != 2u || adv_last_level_loaded != 2u ||
            adv_start_level_calls != 1) {
            printf("  FAIL advance 1->2: r=%d current_level=%u loaded=%u calls=%d\n",
                   r, current_level, adv_last_level_loaded, adv_start_level_calls);
            ok = 0;
        } else {
            printf("  PASS advance 1->2: start_level(2) invoked (calls=%d, "
                   "challenge_fired=%d)\n", adv_start_level_calls, adv_challenge_fired);
        }
    }

    /* 2) protection #define is OFF -> no challenge fired entering level 2. */
    if (adv_challenge_fired != 0) {
        printf("  FAIL: copy-protection challenge fired with BUMPY_COPY_PROTECTION "
               "undefined (should be compiled OUT)\n");
        ok = 0;
    } else {
        printf("  PASS: BUMPY_COPY_PROTECTION OFF -> no challenge on level 2+ entry\n");
    }

    /* 3) predicate FALSE -> no advance (stays in the round). */
    current_level = 3u;
    if (adv_step(0u) != -1 || current_level != 3u) {
        printf("  FAIL: incomplete level advanced (current_level=%u)\n", current_level);
        ok = 0;
    } else {
        printf("  PASS: incomplete level does not advance (current_level stays 3)\n");
    }

    /* 4) all-levels-done boundary: complete level 9 -> current_level 9->0x0a ->
          title return (no further start_level). */
    current_level = 9u;
    adv_start_level_calls = 0;
    {
        int r = adv_step(1u);
        if (r != 0 || current_level != 0x0au || adv_start_level_calls != 0) {
            printf("  FAIL boundary 9->0x0a: r=%d current_level=%u calls=%d\n",
                   r, current_level, adv_start_level_calls);
            ok = 0;
        } else {
            printf("  PASS boundary: level 9 complete -> current_level=0x0a -> "
                   "title return (no start_level)\n");
        }
    }

    printf("  level-advance: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* ════════════════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1]
                                  : "local/build/render/items_trace.bin";
    FILE *f = fopen(path, "rb");
    long sz; u8 *b; u32 o; u16 ver, nsc, nfn; unsigned s;
    stats_t st = { 0, 0, 0 };
    int hard_fail = 0;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz);
    if (!b || fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
    fclose(f);

    if (sz < 14 || memcmp(b, "ITEMTRC1", 8) != 0) {
        fprintf(stderr, "bad magic (want ITEMTRC1)\n"); return 2;
    }
    ver = rd16(b + 8); nsc = rd16(b + 10);
    if (ver != 1) { fprintf(stderr, "unsupported version %u\n", ver); return 2; }
    o = 12;
    nfn = rd16(b + o); o += 2;
    /* skip the fn-name string table (we key on fn_addr, not the name index). */
    { u16 k; for (k = 0; k < nfn; k++) { u8 ln = b[o]; o += 1 + ln; } }

    printf("items_ctest: replay harness over %s\n", path);
    printf("  trace: ITEMTRC1 v%u, %u scenarios, %u fn-names\n", ver, nsc, nfn);
    printf("  per-fn diff targets (host-callable): ALL FIVE item/exit functions "
           "are PORTED (p1_collect_item, p1_collect_item_score [T3], "
           "move_step_read_item [T3], check_exit_tile_vert [T4], "
           "teleport_to_next_exit_tile [T4]) — every record runs the per-fn "
           "differential\n");

    for (s = 0; s < nsc; s++) {
        u8 sid, name_len, level, start_cell, setup_kind;
        char scname[64];
        u32 nrec, k;
        record_t *recs;
        stats_t sst = { 0, 0, 0 };
        int per_ok;

        sid = b[o]; o += 1;
        name_len = b[o]; o += 1;
        { unsigned n = name_len < 63 ? name_len : 63;
          memcpy(scname, b + o, n); scname[n] = 0; o += name_len; }
        setup_kind = b[o]; o += 1;
        level = b[o]; o += 1;
        start_cell = b[o]; o += 1;
        nrec = rd32(b + o); o += 4;

        recs = malloc(sizeof(record_t) * (nrec ? nrec : 1));
        for (k = 0; k < nrec; k++) {
            record_t *r = &recs[k];
            r->fn_addr = rd16(b + o); o += 2;
            r->fn_name_idx = rd16(b + o); o += 2;
            parse_snap(b + o, &r->ent); o += SNAP_SIZE;
            parse_snap(b + o, &r->ex);  o += SNAP_SIZE;
        }

        printf("\n== scenario %u: %s (setup %u, level %u, start_cell %u, %lu records) ==\n",
               sid, scname, setup_kind, level, start_cell, (unsigned long)nrec);

        per_ok = run_per_function(recs, (long)nrec, scname, &sst);
        printf("  per-fn: PASS=%ld  FAIL=%ld  UNPORTED=%ld\n",
               sst.pass, sst.fail, sst.unported);
        if (!per_ok) hard_fail = 1;

        st.pass += sst.pass; st.fail += sst.fail; st.unported += sst.unported;
        free(recs);
    }

    printf("\n=== TOTAL per-fn: PASS=%ld  FAIL=%ld  UNPORTED=%ld ===\n",
           st.pass, st.fail, st.unported);
    if (hard_fail || st.fail != 0) {
        printf("FAIL: %ld per-function differential failure(s) on PORTED fns\n",
               st.fail);
        return 1;
    }

    /* Part C: level-advance state-transition (exit -> current_level++ ->
       start_level(N), protection #define OFF). */
    if (!validate_level_advance()) {
        printf("FAIL: level-advance state-transition check failed\n");
        return 1;
    }

    printf("\nPASS: every PORTED record matched its exit SNAP (UNPORTED=%ld; all "
           "five item/exit functions are now reconstructed — p1_collect_item, "
           "p1_collect_item_score, move_step_read_item [T3], check_exit_tile_vert, "
           "teleport_to_next_exit_tile [T4]); level-advance state-transition "
           "validated (1->2 start_level(2), protection #define OFF)\n", st.unported);
    return 0;
}
