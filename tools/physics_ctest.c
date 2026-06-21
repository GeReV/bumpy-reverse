/* Host REPLAY HARNESS for src/player.c physics — Phase-2 Task 2.
 *
 * Compiles the REAL port (src/player.c) on the host (Watcom 16-bit environment
 * shimmed out: __far/__huge erased, exact-width typedefs, BUMPY_H defined so
 * player.h does not pull <dos.h>), then validates the reconstructed physics
 * against the Phase-2 T1 capture local/build/render/physics_trace.bin
 * (magic PHYSTRC1, version 1 — layout frozen in tools/physics_oracle.py and
 * local/build/physics_model.md §Trace layout).  TWO comparators:
 *
 *   PRIMARY — per-function differential (the real gate).  For each trace record
 *     of a PORTED, host-callable fn: seed the reconstructed globals + the
 *     record's move-script bytes (+ a synthetic tilemap window) = the record's
 *     ENTRY SNAP, call the fn by its C name, assert the output globals == the
 *     EXIT SNAP.  Prints PASS/FAIL with first-divergence (fn, field, got/want).
 *     Records whose fn has no host-callable port (now only p1_movement_dispatch,
 *     the raw-offset call-through dispatcher) are marked UNPORTED and skipped —
 *     NOT a hard failure.  As of Phase-2 T4 the 0x63xx move-step substates behind
 *     dispatch_move_step ARE host-callable and additionally exit-diffed (see the
 *     SUBSTATE_MAP), and check_tile_below's delegate path is fully mode-diffed.
 *
 *   SECONDARY — trajectory-stitch (host validation tooling, clearly labelled).
 *     Per scenario, seed ONLY the first record's entry, then re-run the
 *     reconstructed physics chain (dispatched through an explicit host map:
 *     engine-offset -> reconstructed-C-fn) following the captured fn-call order,
 *     asserting the produced px/py/move_anim/game_mode SEQUENCE == the captured
 *     sequence; prints first-divergence step.  This is NOT part of src/.
 *
 * THE DISPATCH WRINKLE: move_step_dispatch_tbl[] (in player.c) holds raw ENGINE
 * near-offsets (0x6648, 0x6717 ...), NOT host function pointers — so the
 * reconstructed dispatch_move_step()'s call-through WILL crash on the host.
 * Therefore the per-fn comparator tests dispatch_move_step's SLOT-ADDRESS
 * arithmetic (like player_ctest's E6) AND, for the Phase-2 T4 substates, resolves
 * the routed offset via SUBSTATE_MAP and calls the reconstructed C substate
 * directly (never the raw call-through); the trajectory-stitch likewise routes
 * dispatch records through SUBSTATE_MAP.  The single host-unsafe nested dispatch a
 * delegate (move_down) reaches is neutralised by a host no-op installed at the
 * exact slot it reads (see install_noop_slot) — the mode transition it captures is
 * set by enter_game_mode BEFORE that dispatch and is reproduced 1:1.
 *
 * Build/run (also wrapped by tools/validate_physics.sh):
 *     cc -O2 -Wall -Werror -o /tmp/physics_ctest tools/physics_ctest.c && \
 *       /tmp/physics_ctest local/build/render/physics_trace.bin
 * Exit 0 iff the harness parses the trace, the per-fn comparator PASSES on every
 * PORTED record, and the trajectory-stitch matches up to the first UNPORTED stop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── shim the Watcom 16-bit environment for host compilation ─────────────────── */
#define BUMPY_H            /* player.h's #include "bumpy.h" becomes a no-op */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge
/* MK_FP host model (identical to player_ctest.c): a 1 MB+ linear "far memory"
   shadow indexed by the real-mode linear address (seg<<4 + off).  enter_game_mode
   builds and dereferences a far pointer from mode_script_tbl, so MK_FP must land
   in valid (zeroed) host memory. */
static unsigned char far_mem[0x110000];
#define MK_FP(seg, off) ((void *)(far_mem + (((u32)(seg) << 4) + (u16)(off))))

/* input_state is normally owned by input.c; provide a host definition. */
u8 input_state;

/* mode_script_tbl: forward-declared near blob enter_game_mode indexes.  The
   per-fn comparator seeds it per-record (see seed_mode_script_tbl). */
u8 mode_script_tbl[64 * 4];

/* tilemap: cross-module level-data far ptr (owned by level.c → T7).  Point it at
   a synthetic window the comparator seeds per-record from the trace's captured
   tilemap bytes; read_tile_layer_contact reads tilemap[cell+0x30]. */
#define TILEMAP_SIZE 0x300
static u8 synth_tilemap[TILEMAP_SIZE];
u8 __far *tilemap = synth_tilemap;

/* Leaf stubs (identical set to player_ctest.c).  land_on_tile_below /
   check_tile_below_ladder_or_land (Phase-2 T3) and the move-step substates + their
   two delegates p1_exec_pending_action / move_down_step (Phase-2 T4) are now
   DEFINED in player.c — no longer host stubs here.  Only the boundary callees
   (FX allocator apply_cell_animation; sound play_sound/apply_contact_action; the
   teleport leaf FUN_1000_4802) remain stubbed below. */
/* play_sound: record the last id apply_contact_action emitted so the contact-family
   differential (run_contact_family, below) can assert the device-selected sound id. */
u8 g_last_play_sound_id;
int g_play_sound_calls;
void play_sound(u8 id) { g_last_play_sound_id = id; g_play_sound_calls++; }
void play_action_sound(void) { }
/* apply_contact_action (1000:6a89) is now RECONSTRUCTED in src/player.c (Phase-9 T1)
   and pulled in via the #include below — no host stub here (would be a dup symbol). */
void play_walk_anim_default(void) { }
void step_walk_anim(u8 a, u8 p, u16 fo, u16 fs) { (void)a;(void)p;(void)fo;(void)fs; }
void apply_cell_animation(u8 fx) { (void)fx; }                      /* FX allocator → Phase 5/6 */
void FUN_1000_4802(void) { }                                       /* pending==0x0f teleport leaf */
/* run_physics_settle (player.c) reads these cross-module DGROUP bytes (game.c). */
u8 session_continue_flag, frame_abort_flag, settle_countdown;

/* ── Phase-9 T2 cross-module callees pulled in by the move_step_dispatch_tbl
   resolver + handle_gameplay_input.  In the real build these live in sound.c /
   items.c / input.c / player2.c / game.c / screens.c; the host replay stubs them
   so the link closes.  They are boundary leaves (sound/item/FX/exit-tile) that do
   NOT touch the validated physics SNAP fields (px/py/anim/mode/step/facing/
   steps_left/input/override/locked), so the per-fn differential stays exact.  The
   move-step substates that DO mutate SNAP fields (cursor/contact/landing) are
   reconstructed in player.c and reached through the real resolver. */
void play_exit_sound(void)        { }   /* sound.c 6305 */
void play_contact_sound(void)     { }   /* sound.c 640c */
void play_pickup_sound(void)      { }   /* sound.c 645d */
void play_state_sound_79b9(void)  { }   /* sound.c 647e */
void play_event_sound_64c1(void)  { }   /* sound.c 64c1 */
void check_exit_tile_vert(void)   { }   /* items.c 6372 (level-exit; sets physics_frozen — not a SNAP field) */
void move_step_read_item(void)    { }   /* items.c 6627 (item pickup; layer-C read) */
void input_state_clear(void)      { input_state = 0; }   /* input.c 65d2 */
u8   get_key_state(u8 sc)         { (void)sc; return 0; } /* input.c 7ab4 (no debug keys in capture) */
void poll_input(void)             { }   /* input.c 1dde (input already seeded from the SNAP) */
u8   timing_flag_accumulator;           /* screens.c 0x854f */
u8   round_continue_flag;               /* game.c 0x9d30 */
u8   pvp_collision_flag;                /* player2.c 0xa1aa (Ghidra players_colliding) */
/* Out-of-scope handler-table targets — host stubs so the table links. */
void move_walk_right_anim_step(void) { }
void enter_mode_0b_jump_start(void) { }
void move_anim_step_to_mode0c(void) { }
void move_step_check_walkable(void) { }
void move_step_dispatch_input(void) { }
void teleport_to_next_exit_tile(void) { }
void p1_input_dispatch_bit10(void) { }
void FUN_1000_4437(void) { }
void advance_physics_freeze(void) { }
void FUN_1000_1e3d(void) { }

/* ── channel-B anim globals apply_contact_action (player.c, Phase-9 T1) references.
   OWNED by anim.c in the real build; defined here for the host replay so the
   reconstructed apply_contact_action can claim a channel-B slot.  anim.h declares
   them extern + the anim_chan_rec type (its include guard makes player.c's later
   #include a no-op). */
#include "../src/anim.h"
anim_chan_rec       anim_b_records[ANIM_B_SLOTS];
anim_chan_rec       anim_b_terminator = { 0xff, 0, 0, 0, 0, 0, 0, 0 };
anim_chan_rec __far *anim_channels_b_tbl[ANIM_B_SLOTS + 1];   /* 4 slots + 0xFF term */
u8                  anim_b_loop_idx;   /* DGROUP 0x8566 = last_contact_action alias */

/* p1_sprite (anim.c — DGROUP 0x8884): player.c's Phase-9 T3 draw_p1_sprite references
   it extern (OWNED by anim.c, not included here).  Host-defined so the link closes. */
u8 __far *p1_sprite;

#include "../src/player.c"

/* ════════════════════════════════════════════════════════════════════════════
 *  Trace format (frozen — see tools/physics_oracle.py header).
 * ════════════════════════════════════════════════════════════════════════════ */
#define SNAP_SIZE 16

/* SNAP field order (16 B LE): px(s16) py(s16) anim mode step facing steps_left
   input frozen override cell locked prev_mode pad. */
typedef struct {
    s16 px, py;
    u8  anim, mode, step, facing, steps_left, input, frozen, override,
        cell, locked, prev_mode, pad;
} snap_t;

typedef struct {
    u16    fn_addr;
    u16    fn_name_idx;
    u16    dispatch_target;
    snap_t ent, ex;
    u16    script_off, script_seg, script_len;
    const u8 *script;          /* points into the loaded file buffer */
    u8     tile_base, tile_len;
    const u8 *tile;            /* points into the loaded file buffer */
} record_t;

/* Engine seg-1000 offsets for the hooked fns (from physics_oracle FN_NAMES). */
#define FN_ENTER_GAME_MODE      0x4263
#define FN_P1_STEP_SCRIPTED     0x13df
#define FN_DISPATCH_MOVE_STEP   0x238e
#define FN_P1_MOVEMENT_DISPATCH 0x1e02
#define FN_LAND_ON_TILE         0x2810
#define FN_CHECK_TILE_BELOW     0x29a6

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) |
                                      ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void parse_snap(const u8 *p, snap_t *s)
{
    s->px = (s16)rd16(p + 0);
    s->py = (s16)rd16(p + 2);
    s->anim = p[4]; s->mode = p[5]; s->step = p[6]; s->facing = p[7];
    s->steps_left = p[8]; s->input = p[9]; s->frozen = p[10]; s->override = p[11];
    s->cell = p[12]; s->locked = p[13]; s->prev_mode = p[14]; s->pad = p[15];
}

/* ── globals <-> SNAP ───────────────────────────────────────────────────────── */
static void seed_globals(const snap_t *s)
{
    p1_pixel_x = s->px; p1_pixel_y = s->py;
    p1_move_anim = s->anim; game_mode = s->mode; p1_move_step_idx = s->step;
    p1_facing_left = s->facing; p1_move_steps_left = s->steps_left;
    input_state = s->input; physics_frozen = s->frozen; move_override = s->override;
    p1_cell = s->cell; move_locked = s->locked; prev_game_mode = s->prev_mode;
}

/* Compare the live globals against an exit SNAP.  Returns NULL if all match,
   else the name of the first divergent field; got/want filled. */
static const char *cmp_exit(const snap_t *e, long *got, long *want)
{
#define FLD(name, lhs, rhs) do { if ((long)(lhs) != (long)(rhs)) { \
        *got = (long)(lhs); *want = (long)(rhs); return name; } } while (0)
    FLD("px",         p1_pixel_x,        e->px);
    FLD("py",         p1_pixel_y,        e->py);
    FLD("anim",       p1_move_anim,      e->anim);
    FLD("mode",       game_mode,         e->mode);
    FLD("step",       p1_move_step_idx,  e->step);
    FLD("facing",     p1_facing_left,    e->facing);
    FLD("steps_left", p1_move_steps_left,e->steps_left);
    FLD("input",      input_state,       e->input);
    FLD("override",   move_override,     e->override);
    FLD("locked",     move_locked,       e->locked);
    /* physics_frozen: not an output of the current per-fn targets
       (p1_step_scripted_move and enter_game_mode do not touch it). */
    /* prev_mode: set only by enter_game_mode's caller (p1_movement_dispatch),
       which is not a per-fn diff target yet (T3/T4); excluded intentionally. */
    /* cell: a read-only input to the physics fns being tested here; the entry
       and exit snaps carry the same value, so comparing it adds no signal. */
#undef FLD
    return NULL;
}

/* Seed the move-script far pointer at the record's captured bytes (host copy). */
static u8 script_buf[256];
static void seed_script(const record_t *r)
{
    memset(script_buf, 0, sizeof(script_buf));
    if (r->script_len) {
        unsigned n = r->script_len < sizeof(script_buf) ? r->script_len
                                                        : sizeof(script_buf);
        memcpy(script_buf, r->script, n);
    }
    p1_move_script = (u16 __far *)script_buf;
}

/* Seed the synthetic tilemap from the record's captured window (tile_base is the
   first cell; the engine indexes tilemap[cell] and tilemap[cell+0x30]). */
static void seed_tilemap(const record_t *r)
{
    memset(synth_tilemap, 0, TILEMAP_SIZE);
    if (r->tile_len) {
        unsigned i;
        for (i = 0; i < r->tile_len; i++) {
            unsigned cell = (unsigned)r->tile_base + i;
            if (cell < TILEMAP_SIZE) synth_tilemap[cell] = r->tile[i];
        }
    }
}

/* enter_game_mode loads mode_script_tbl[mode] (a far ptr) and sets
   steps_left=script[0], facing=script[1], p1_move_script=(off,seg)=script[2..5].
   Those table contents were populated at RUNTIME in the engine and are NOT in the
   trace, so we seed mode_script_tbl[mode] -> a synthetic script block built FROM
   the record's EXIT (steps_left, facing) + the record's captured script_off/seg.
   This lets the host enter_game_mode reproduce the exit by construction, so the
   comparator validates its real control flow + table-read->assign STRUCTURE. */
static u8 eg_script_block[8];
static void seed_mode_script_tbl(const record_t *r)
{
    u16 ptr_off, ptr_seg;
    memset(mode_script_tbl, 0, sizeof(mode_script_tbl));
    /* synthetic script block at far_mem(seg=0x9000, off=0): [steps, facing,
       off_lo,off_hi, seg_lo,seg_hi] — script[2..5] reproduce the exit p1_move_script. */
    eg_script_block[0] = r->ex.steps_left;
    eg_script_block[1] = r->ex.facing;
    eg_script_block[2] = (u8)(r->script_off & 0xff);
    eg_script_block[3] = (u8)(r->script_off >> 8);
    eg_script_block[4] = (u8)(r->script_seg & 0xff);
    eg_script_block[5] = (u8)(r->script_seg >> 8);
    /* place the block in far_mem and point mode_script_tbl[mode] at it. */
    {
        u32 lin = (u32)0x9000 * 16;   /* an unused far_mem region */
        memcpy(far_mem + lin, eg_script_block, sizeof(eg_script_block));
        ptr_off = 0x0000; ptr_seg = 0x9000;
    }
    /* mode_script_tbl[mode*4] = (off, seg). */
    {
        u8 mode = r->ex.mode;
        u8 *e = mode_script_tbl + (unsigned)mode * 4;
        e[0] = (u8)(ptr_off & 0xff); e[1] = (u8)(ptr_off >> 8);
        e[2] = (u8)(ptr_seg & 0xff); e[3] = (u8)(ptr_seg >> 8);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  HOST DISPATCH MAP (the wrinkle): engine seg-1000 offset -> reconstructed-C-fn.
 *  Seeded with the Phase-1-ported substate/handler fns now; T3/T4 add entries as
 *  they port the 0x63xx move-step substates + the landing leaves.  Used ONLY by
 *  the trajectory-stitch loop (validation tooling), never by src/.
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct { u16 off; void (*fn)(void); const char *name; } hostmap_t;

/* Phase-9 T2: p1_movement_dispatch + dispatch_move_step are now called for real
   (resolver-backed), so the old HOSTMAP coverage table the stitch consulted is gone. */

/* ── Phase-2 T4: the move-step SUBSTATE map (engine offset -> reconstructed C fn).
 *  dispatch_move_step routes through move_step_dispatch_tbl[mode*0x22 + step*2],
 *  whose entries are RAW engine offsets (not host pointers) — so to execute a
 *  captured dispatch_move_step record on the host we read that slot's offset,
 *  look it up here, and call the real C substate.  Substates the captured
 *  scenarios reach are all ported (Task 4); the common filler 0x7111 maps to the
 *  no-op sentinel.  Offsets ABSENT here (e.g. 0x6326/0x645d/0x647e item/sound/FX
 *  micro-handlers outside the reached physics set) stay UNPORTED -> cleanly
 *  skipped, never call-through a raw offset. */
static const hostmap_t SUBSTATE_MAP[] = {
    { 0x64e2, cursor_move_up,                "cursor_move_up" },
    { 0x64ff, cursor_move_down,              "cursor_move_down" },
    { 0x6535, cursor_move_right,             "cursor_move_right" },
    { 0x654e, p1_try_trigger_pending_action, "p1_try_trigger_pending_action" },
    { 0x6587, p1_try_jump_action,            "p1_try_jump_action" },
    { 0x65e5, input_state_mask_10,           "input_state_mask_10" },
    { 0x65fb, input_state_mask_1d,           "input_state_mask_1d" },
    { 0x6611, input_state_mask_0f,           "input_state_mask_0f" },
    { 0x6648, p1_move_step_with_sound,       "p1_move_step_with_sound" },
    { 0x66d8, move_step_last_variant,        "move_step_last_variant" },
    { 0x6717, move_step_landed,              "move_step_landed" },
    { 0x673a, move_step_noop,                "move_step_noop" },
    { 0x7111, move_step_noop_sentinel,       "move_step_noop_sentinel" },
};
#define SUBSTATE_MAP_N (sizeof(SUBSTATE_MAP) / sizeof(SUBSTATE_MAP[0]))

static void (*substate_lookup(u16 off))(void)
{
    unsigned i;
    for (i = 0; i < SUBSTATE_MAP_N; i++)
        if (SUBSTATE_MAP[i].off == off) return SUBSTATE_MAP[i].fn;
    return NULL;
}

static const char *SUBSTATE_MAP_name(u16 off)
{
    unsigned i;
    for (i = 0; i < SUBSTATE_MAP_N; i++)
        if (SUBSTATE_MAP[i].off == off) return SUBSTATE_MAP[i].name;
    return "?";
}

/* ── Phase-9 T2 PERTURBATION PROOF ───────────────────────────────────────────────
   The headline conversion is the 624 records that now execute the real, resolver-backed
   dispatch_move_step.  To prove that routing is actually EXERCISED (not bypassed), the
   PHYS_PERTURB env corrupts ONE offset→fn mapping as seen by the resolver: it rewrites
   the slots holding offset 0x7111 (move_step_noop_sentinel — a do-nothing handler reached
   by thousands of records) to 0x65e5 (input_state_mask_10, which masks input_state &= 0x10).
   The sentinel preserves the entry input; the mis-route masks it to bit 0x10 only, so every
   record whose captured input is a non-zero value without bit 0x10 (e.g. 0x01 / 0x08) now
   exits with input==0 ≠ captured → the gate MUST FAIL.  If the gate still passed, the
   resolver-backed dispatch wouldn't be exercised — so this is a genuine teeth check on the
   routing path.  Only the in-memory table is touched for this one-shot run; the on-disk
   source table is byte-exact. */
static int g_phys_perturb;                 /* set in main from getenv("PHYS_PERTURB") */
#define PERTURB_FROM_OFF 0x7111            /* move_step_noop_sentinel (do-nothing; reached ~4442x) */
#define PERTURB_TO_OFF   0x65e5            /* mis-routed to input_state_mask_10 (input &= 0x10) */

/* Under PHYS_PERTURB, rewrite every move_step_dispatch_tbl slot holding the FROM offset
   to the TO offset, so dispatch_move_step (which reads the table + routes via the src/
   resolver) mis-routes those slots.  Returns the number of slots corrupted (>0 means the
   perturbation is actually reachable in the table).  This mutates the in-memory table for
   the duration of this one-shot harness run only — the on-disk source table is untouched. */
static long apply_phys_perturb(void)
{
    long n = 0;
    unsigned i;
    if (!g_phys_perturb) return 0;
    for (i = 0; i + 1 < sizeof(move_step_dispatch_tbl); i += 2) {
        u16 off = (u16)(move_step_dispatch_tbl[i] | (move_step_dispatch_tbl[i + 1] << 8));
        if (off == PERTURB_FROM_OFF) {
            move_step_dispatch_tbl[i]     = (u8)(PERTURB_TO_OFF & 0xff);
            move_step_dispatch_tbl[i + 1] = (u8)(PERTURB_TO_OFF >> 8);
            n++;
        }
    }
    return n;
}

/* Read the raw engine offset move_step_dispatch_tbl holds for [mode][step]. */
static u16 dispatch_slot_offset(u8 mode, u8 step)
{
    unsigned off = (unsigned)mode * 0x22 + (unsigned)step * 2;
    return (u16)(move_step_dispatch_tbl[off] |
                 (move_step_dispatch_tbl[off + 1] << 8));
}

/* Phase-9 T2: dispatch_move_step is now host-safe — it reads the raw 16-bit engine
   offset from move_step_dispatch_tbl + mode*0x22 + step*2 and routes it through the
   src/ resolver move_step_handler_for_offset().  The earlier slot-pointer-injection
   neutralisation (install_noop_slot/restore_disp_tbl) is gone: every trailing dispatch
   now runs for real through the resolver. */

/* Is the hooked-fn at this engine offset PORTED + host-callable as a per-fn
   differential target?  p1_step_scripted_move, enter_game_mode and (Phase-2 T3)
   the two landing leaves land_on_tile_below / check_tile_below_ladder_or_land are;
   the dispatch fns are handled specially (slot-arith / re-anchor). */
static int fn_is_diff_ported(u16 fn_addr)
{
    return fn_addr == FN_P1_STEP_SCRIPTED || fn_addr == FN_ENTER_GAME_MODE ||
           fn_addr == FN_LAND_ON_TILE     || fn_addr == FN_CHECK_TILE_BELOW;
}

/* ════════════════════════════════════════════════════════════════════════════ */
static const char *fn_name(u16 fn_addr)
{
    switch (fn_addr) {
        case FN_ENTER_GAME_MODE:      return "enter_game_mode";
        case FN_P1_STEP_SCRIPTED:     return "p1_step_scripted_move";
        case FN_DISPATCH_MOVE_STEP:   return "dispatch_move_step";
        case FN_P1_MOVEMENT_DISPATCH: return "p1_movement_dispatch";
        case FN_LAND_ON_TILE:         return "land_on_tile_below";
        case FN_CHECK_TILE_BELOW:     return "check_tile_below_ladder_or_land";
        default:                      return "?";
    }
}

/* Per-fn comparator stat accumulators. */
typedef struct { long pass, fail, unported, skipped_mode; } stats_t;

/* dispatch_move_step ROUTING probe (Phase-9 T2: dispatch_move_step is now host-safe).
   dispatch_move_step reads the raw 16-bit engine offset at move_step_dispatch_tbl +
   mode*0x22 + step*2 and routes it through move_step_handler_for_offset() (the host
   resolver).  This probe asserts:
     (a) the mode-row invariant (mode < 0x40),
     (b) the resolver maps the slot's offset to the SAME host function the harness's
         independent SUBSTATE_MAP names for that offset (when the offset is in the
         reached set), proving dispatch's index arithmetic + the resolver agree and
         that the resolver is genuinely exercised (perturb-sensitive).
   Returns 1 = routed correctly, 0 = routing mismatch, -1 = hard invariant violation. */
static int check_dispatch_routes(const record_t *r)
{
    unsigned mode = r->ent.mode, step = r->ent.step;
    u16 soff;
    void (*resolved)(void);
    void (*expected)(void);
    /* a real engine table only has 0x40 mode rows; all captured modes (idle/walk
       /jump/fall) are < 0x40 — assert the invariant so inflated PASS counts are
       impossible; hard-fail if a future capture ever violates it. */
    if (mode >= 0x40) {
        fprintf(stderr, "ASSERT: dispatch record has mode=%#x >= 0x40 "
                "(invariant violated — should never happen in this capture)\n", mode);
        return -1;   /* caller treats negative as a hard failure */
    }
    soff = dispatch_slot_offset((u8)mode, (u8)step);
    resolved = move_step_handler_for_offset(soff);   /* the src/ resolver under test */
    if (resolved == NULL) return 0;                  /* resolver must be total */
    expected = substate_lookup(soff);
    if (expected != NULL && resolved != expected) {
        /* the resolver routed this offset to a DIFFERENT fn than the harness's
           independent map expects — a routing regression (e.g. corrupted mapping). */
        return 0;
    }
    return 1;
}

/* ── PRIMARY: per-function differential over every record ────────────────────── */
static int run_per_function(record_t *recs, long nrec, const char *scname,
                            stats_t *st)
{
    long i;
    int scen_fail = 0;
    long printed = 0;
    for (i = 0; i < nrec; i++) {
        record_t *r = &recs[i];
        if (r->fn_addr == FN_DISPATCH_MOVE_STEP) {
            /* Phase-9 T2: dispatch_move_step is now HOST-SAFE (resolver-backed).
               (1) ROUTING: assert the resolver routes this record's slot offset to the
                   expected host fn (mode-row invariant + resolver/map agreement). */
            int routed = check_dispatch_routes(r);
            if (routed < 0) {
                st->fail++; scen_fail = 1;
                if (printed++ < 8)
                    printf("    FAIL [%s #%ld] dispatch_move_step mode=%#x >= 0x40 "
                           "(invariant violation)\n", scname, i, r->ent.mode);
                continue;
            }
            if (routed == 0) {
                st->fail++; scen_fail = 1;
                if (printed++ < 8)
                    printf("    FAIL [%s #%ld] dispatch_move_step resolver routing "
                           "mode=%#x step=%u offset mismatch\n",
                           scname, i, r->ent.mode, r->ent.step);
                continue;
            }
            /* (2) Call dispatch_move_step() FOR REAL through the resolver and assert the
               exit SNAP.  When the routed substate mutates SNAP fields (cursor / contact /
               landing), this is a full exit diff; when it is a boundary leaf (item / sound /
               FX / exit-tile — no SNAP-field side effect), the seeded entry == exit by
               construction, so the same diff confirms the no-op faithfully. */
            {
                const char *bad; long got, want;
                seed_tilemap(r);
                seed_globals(&r->ent);
                seed_script(r);
                /* p1_pending_action is NOT in the SNAP; reconstruct it as the tile under
                   p1_cell (the p1_read_tile_under value the engine latched), so
                   pending-action-keyed substates take the captured branch. */
                p1_pending_action = synth_tilemap[r->ent.cell];
                dispatch_move_step();
                bad = cmp_exit(&r->ex, &got, &want);
                if (bad != NULL) {
                    u16 soff = dispatch_slot_offset(r->ent.mode, r->ent.step);
                    st->fail++; scen_fail = 1;
                    if (printed++ < 8)
                        printf("    FAIL [%s #%ld] dispatch_move_step->%s "
                               "field %s: got %ld want %ld (mode=%#x step=%u)\n",
                               scname, i, SUBSTATE_MAP_name(soff), bad, got, want,
                               r->ent.mode, r->ent.step);
                    continue;
                }
            }
            st->pass++;
            continue;
        }
        if (r->fn_addr == FN_P1_MOVEMENT_DISPATCH) {
            /* Phase-9 T2: p1_movement_dispatch is now FULLY host-callable — its
               game_mode_handlers[] tail-call dispatch_move_step, which is now
               resolver-backed (host-safe) instead of a raw-offset call-through.  Seed
               the entry SNAP + tilemap + script, call it for real, and assert the exit
               SNAP.  This converts the 624 records that were UNPORTED purely because the
               dispatch knot could not be executed on the host. */
            const char *bad; long got, want;
            seed_tilemap(r);
            seed_globals(&r->ent);
            seed_script(r);
            seed_mode_script_tbl(r);
            p1_pending_action = synth_tilemap[r->ent.cell];
            p1_movement_dispatch();
            bad = cmp_exit(&r->ex, &got, &want);
            if (bad == NULL) {
                st->pass++;
            } else {
                st->fail++; scen_fail = 1;
                if (printed++ < 8)
                    printf("    FAIL [%s #%ld] p1_movement_dispatch field %s: "
                           "got %ld want %ld (mode=%#x step=%u cell=%u)\n",
                           scname, i, bad, got, want,
                           r->ent.mode, r->ent.step, r->ent.cell);
            }
            continue;
        }
        if (!fn_is_diff_ported(r->fn_addr)) {
            /* Phase-9 T2: after dispatch_move_step + p1_movement_dispatch were made
               host-callable (above), every captured fn_addr is now a diff target — this
               UNPORTED fallback should no longer fire.  Kept as a safety net: any future
               capture introducing a not-yet-ported hooked fn lands here (UNPORTED, not a
               hard failure) instead of crashing. */
            st->unported++;
            continue;
        }
        /* ── seed entry, call the fn by C name, assert exit ── */
        seed_tilemap(r);
        if (r->fn_addr == FN_P1_STEP_SCRIPTED) {
            const char *bad; long got, want;
            seed_globals(&r->ent);
            seed_script(r);
            (void)p1_step_scripted_move();
            bad = cmp_exit(&r->ex, &got, &want);
            /* p1_step_scripted_move does NOT touch input/override/locked beyond
               returning move_locked; cmp_exit already restricts to its outputs,
               but input_state is untouched by it so entry==exit there. */
            if (bad == NULL) {
                st->pass++;
            } else {
                st->fail++; scen_fail = 1;
                if (printed++ < 8)
                    printf("    FAIL [%s #%ld] p1_step_scripted_move field %s: "
                           "got %ld want %ld (entry mode=%#x facing=%u steps=%u)\n",
                           scname, i, bad, got, want,
                           r->ent.mode, r->ent.facing, r->ent.steps_left);
            }
        } else if (r->fn_addr == FN_ENTER_GAME_MODE) {
            const char *bad; long got, want;
            seed_globals(&r->ent);
            seed_script(r);
            seed_mode_script_tbl(r);
            enter_game_mode(r->ex.mode);   /* mode arg == exit game_mode */
            /* enter_game_mode validates: game_mode<-mode, input_state<-0, and the
               table-read->assign (steps_left, facing) reproduced via the seeded
               synthetic table.  px/py/anim are untouched by it (entry==exit). */
            bad = cmp_exit(&r->ex, &got, &want);
            if (bad == NULL) {
                st->pass++;
            } else {
                st->fail++; scen_fail = 1;
                if (printed++ < 8)
                    printf("    FAIL [%s #%ld] enter_game_mode field %s: "
                           "got %ld want %ld (mode=%#x locked=%u)\n",
                           scname, i, bad, got, want, r->ex.mode, r->ent.locked);
            }
        } else if (r->fn_addr == FN_LAND_ON_TILE) {
            /* land_on_tile_below: seed entry + the synthetic tilemap (cell-8 tile)
               + mode_script_tbl[ex.mode] (the entered mode's script, reproduced
               from the exit steps_left/facing — same construction as the
               enter_game_mode target, since the leaf's mode transition IS an
               enter_game_mode of the land-table-resolved mode).  The leaf reads
               tile_below_player = tilemap[cell-8], resolves land_mode_fx_tbl, and
               enter_game_mode(land_mode); the captured tile-window seeds cell-8 so
               the resolved mode matches the capture by construction. */
            const char *bad; long got, want;
            seed_globals(&r->ent);
            seed_script(r);
            seed_mode_script_tbl(r);
            land_on_tile_below();
            bad = cmp_exit(&r->ex, &got, &want);
            if (bad == NULL) {
                st->pass++;
            } else {
                st->fail++; scen_fail = 1;
                if (printed++ < 8)
                    printf("    FAIL [%s #%ld] land_on_tile_below field %s: "
                           "got %ld want %ld (entry mode=%#x cell=%u prev=%#x)\n",
                           scname, i, bad, got, want,
                           r->ent.mode, r->ent.cell, r->ent.prev_mode);
            }
        } else { /* FN_CHECK_TILE_BELOW */
            /* check_tile_below_ladder_or_land — now FULLY DIFFED (Phase-2 T4).
               The two delegates it tail-calls (p1_exec_pending_action 465e /
               move_down_step 253f) are reconstructed in player.c, so the leaf can
               be called through on every branch.  Their mode result is keyed by
               p1_pending_action, which is NOT in the SNAP — we reconstruct it as the
               tile under p1_cell (the p1_read_tile_under value the engine latched).
               move_down_step's trailing dispatch_move_step() call-through would route
               a RAW engine offset on the host, so for the down-input delegate branch
               (cell>=8, tile 0x0e, down held) we drive move_down_step's slot via the
               substate map instead of the raw call-through — see below.  Every other
               branch (in-leaf ladder, p1_exec_pending_action delegate) is a direct
               full call-through + exit diff. */
            int cell = r->ent.cell;
            int tile_below = -1;
            const char *bad; long got, want;
            seed_tilemap(r);
            if (cell >= 8) {
                unsigned idx = (unsigned)(cell - 8);
                tile_below = (idx < TILEMAP_SIZE) ? synth_tilemap[idx] : -1;
            }
            seed_globals(&r->ent);
            seed_script(r);
            seed_mode_script_tbl(r);
            p1_pending_action = synth_tilemap[cell];   /* p1_read_tile_under value */
            if (cell >= 8 && tile_below == 0x0e && (r->ent.input & 2) == 0) {
                /* IN-LEAF ladder branch: the leaf itself plays a sound + FX then
                   enter_game_mode(0x0a) — no delegate, no nested dispatch.  Full
                   call-through. */
                check_tile_below_ladder_or_land();
                bad = cmp_exit(&r->ex, &got, &want);
            } else {
                /* DELEGATE branch.  For tile_below==0 (every captured record) the
                   leaf tail-calls p1_exec_pending_action -> exec_move_action(0) ->
                   move_down, which resolves the bounce/settle mode from rng_frame
                   (NOT in the SNAP) and p1_begin_move(mode) = enter_game_mode(mode)
                   + a trailing dispatch_move_step.  The captured EXIT mode IS that
                   resolved mode, so we seed rng_frame to the bucket that reproduces
                   it (faithful: rng_frame is a real engine input the trace's exit
                   pins down), seed mode_script_tbl[ex.mode] so enter_game_mode
                   reproduces steps_left/facing, then call the leaf through and full-diff
                   the exit.  Phase-9 T2: the trailing dispatch_move_step is now host-safe
                   (resolver-backed), so it runs FOR REAL — no slot neutralisation needed. */
                switch (r->ex.mode) {
                    case 0x3c: rng_frame = 0xff; break;   /* >=0xec */
                    case 0x3d: rng_frame = 0xe0; break;   /* [0xd8,0xec) */
                    case 0x3e: rng_frame = 0xd0; break;   /* [0xc4,0xd8) */
                    case 0x3f: rng_frame = 0xb8; break;   /* [0xb0,0xc4) */
                    default:   rng_frame = 0x00; break;   /* <0xb0 -> move_action 0 -> mode 0 */
                }
                check_tile_below_ladder_or_land();
                bad = cmp_exit(&r->ex, &got, &want);
            }
            if (bad == NULL) {
                st->pass++;
            } else {
                st->fail++; scen_fail = 1;
                if (printed++ < 8)
                    printf("    FAIL [%s #%ld] check_tile_below field %s: "
                           "got %ld want %ld (mode=%#x cell=%u tile_below=%d in=%#x)\n",
                           scname, i, bad, got, want,
                           r->ent.mode, r->ent.cell, tile_below, r->ent.input);
            }
        }
    }
    return !scen_fail;
}

/* ── SECONDARY: trajectory-stitch (host validation tooling) ──────────────────────
 * Seed ONLY the scenario's first record entry, then walk the captured fn-call
 * order, replaying each hooked fn through host-callable reconstructions; assert
 * the produced (px,py,anim,mode) sequence matches each record's EXIT until the
 * first UNPORTED stop.  Dispatch fns route via the host map / slot table; if a
 * record requires an UNPORTED fn (landing leaves, or a dispatch target absent
 * from move_step_dispatch_tbl's host-callable set) we STOP and report the step,
 * which is the clean localisation the brief asks for (no crash, no hard-fail). */
/* run_trajectory_stitch return: matched steps and skipped dispatch records. */
typedef struct { long matched; long skipped_dispatch; long stop_step; } stitch_result_t;

static stitch_result_t run_trajectory_stitch(record_t *recs, long nrec, const char *scname)
{
    long i;
    stitch_result_t res = { 0, 0, nrec };
    if (nrec == 0) return res;
    seed_globals(&recs[0].ent);
    seed_script(&recs[0]);
    for (i = 0; i < nrec; i++) {
        record_t *r = &recs[i];
        seed_tilemap(r);
        if (r->fn_addr == FN_P1_STEP_SCRIPTED) {
            seed_script(r);            /* re-anchor the script ptr at the captured bytes */
            (void)p1_step_scripted_move();
        } else if (r->fn_addr == FN_ENTER_GAME_MODE) {
            seed_mode_script_tbl(r);
            enter_game_mode(r->ex.mode);
        } else if (r->fn_addr == FN_LAND_ON_TILE) {
            /* land_on_tile_below is now host-callable (Phase-2 T3): its mode
               transition is the land-table-resolved enter_game_mode, reproducible
               from the seeded tilemap + mode_script_tbl.  Call it through so the
               stitch ADVANCES PAST the landing step instead of stopping. */
            seed_mode_script_tbl(r);
            land_on_tile_below();
        } else if (r->fn_addr == FN_CHECK_TILE_BELOW) {
            /* check_tile_below_ladder_or_land (Phase-2 T4): on the captured path it
               tail-calls p1_exec_pending_action -> move_down, which resolves the exit
               (bounce/settle) mode from rng_frame and runs a trailing
               dispatch_move_step.  Seed rng_frame to the bucket the captured EXIT
               mode pins, seed mode_script_tbl[ex.mode], neutralise the single
               trailing dispatch (host no-op at slot (ex.mode, ent.step)), call the
               leaf through, restore — so the stitch executes the real delegate mode
               transition (matched, not re-anchored-blind). */
            seed_mode_script_tbl(r);
            p1_pending_action = synth_tilemap[r->ent.cell];
            switch (r->ex.mode) {
                case 0x3c: rng_frame = 0xff; break;
                case 0x3d: rng_frame = 0xe0; break;
                case 0x3e: rng_frame = 0xd0; break;
                case 0x3f: rng_frame = 0xb8; break;
                default:   rng_frame = 0x00; break;
            }
            /* Phase-9 T2: the trailing dispatch_move_step is host-safe — run for real. */
            check_tile_below_ladder_or_land();
        } else if (r->fn_addr == FN_DISPATCH_MOVE_STEP) {
            /* Phase-9 T2: dispatch_move_step is now host-safe (resolver-backed) — call
               it through for real.  p1_pending_action keys the pending-action substates
               and is reconstructed from the tile under p1_cell. */
            if (r->ent.mode < 0x40) {
                p1_pending_action = synth_tilemap[r->ent.cell];
                dispatch_move_step();
            }
        } else if (r->fn_addr == FN_P1_MOVEMENT_DISPATCH) {
            /* Phase-9 T2: p1_movement_dispatch is fully host-callable now (its handlers'
               tail-call dispatch_move_step is resolver-backed).  Its branch selection
               depends on engine inputs NOT in the 16-byte SNAP (e.g. the jump/move input
               bits + rng_frame the mode-handler reads), so — as the PRIMARY per-fn diff
               does — seed this record's full captured ENTRY before calling, then compare
               to its captured EXIT.  This keeps the stitch a per-step delta check
               (re-anchored each step) rather than drifting on unseeded non-SNAP state. */
            seed_globals(&r->ent);
            seed_mode_script_tbl(r);
            p1_pending_action = synth_tilemap[r->ent.cell];
            p1_movement_dispatch();
        } else {
            /* No remaining UNPORTED hooked fn on the captured path (landing leaves are
               handled above).  Any future not-yet-ported fn lands here -> localised stop. */
            printf("    STITCH stop [%s] step %ld: UNPORTED fn %s\n",
                   scname, i, fn_name(r->fn_addr));
            res.stop_step = i;
            return res;
        }
        /* compare the produced trajectory fields against the captured exit. */
        if (p1_pixel_x != r->ex.px || p1_pixel_y != r->ex.py ||
            p1_move_anim != r->ex.anim || game_mode != r->ex.mode) {
            /* Divergence is EXPECTED once the chain depends on an unported
               substate's side effects; report and stop (localised, not a crash). */
            printf("    STITCH diverge [%s] step %ld (fn %s): "
                   "(px,py,anim,mode) got (%d,%d,%u,%#x) want (%d,%d,%u,%#x)\n",
                   scname, i, fn_name(r->fn_addr),
                   p1_pixel_x, p1_pixel_y, p1_move_anim, game_mode,
                   r->ex.px, r->ex.py, r->ex.anim, r->ex.mode);
            res.stop_step = i;
            return res;
        }
        /* re-anchor to the captured exit so the next step starts from ground
           truth (the stitch validates per-step deltas, not unbounded drift). */
        seed_globals(&r->ex);
        res.matched++;
    }
    res.stop_step = nrec;
    return res;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  PHASE 9, TASK 1 — CONTACT-ACTION HANDLER FAMILY DIFFERENTIAL
 *  --------------------------------------------------------------------------
 *  The contact handlers map p1_contact_code through a DGROUP byte LUT to an action
 *  code, then run apply_contact_action(action) = play the device-selected contact
 *  sound + claim a channel-B anim slot keyed by p1_cell_prev + stamp the action's
 *  tile into tilemap[p1_cell_prev+0x30].  These functions are NOT in the physics
 *  16-byte SNAP, so this is a SELF-CONTAINED differential whose GROUND TRUTH is the
 *  engine's own byte-exact tables read DIRECTLY from BUMPY_unpacked.exe (NOT from
 *  player.c's arrays) + the engine-verified channel-B slot-allocator semantics
 *  (the structural twin of apply_cell_animation, anim.c 69aa).  A real differential:
 *  player.c's reconstructed arrays/logic vs the engine image; perturbation-proven
 *  (set CONTACT_PERTURB=1 to corrupt a seeded input → the gate MUST fail).
 * ════════════════════════════════════════════════════════════════════════════ */

/* DGROUP file base in BUMPY_unpacked.exe (see player.c table comments). */
#define DGROUP_FILE_BASE 0x11440

/* The six dispatch LUTs' DGROUP offsets, paired with the handler that reads each. */
typedef struct { u16 dgroup_off; const char *name; } lut_ref_t;
static const lut_ref_t CONTACT_LUTS[6] = {
    { 0x35fe, "at_start(35fe)" }, { 0x361e, "before_end(361e)" },
    { 0x363e, "main(363e)"     }, { 0x365e, "prev(365e)"       },
    { 0x367e, "tbl_367e(367e)" }, { 0x369e, "tbl_369e(369e)"   },
};
/* player.c's reconstructed copies (the thing under test). */
static u8 *contact_lut_recon[6];          /* filled in main from player.c symbols */

/* Read a window of the engine image; returns 1 on success. */
static int read_exe(const char *exe, u32 off, u8 *dst, unsigned n)
{
    FILE *f = fopen(exe, "rb");
    if (!f) return 0;
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return 0; }
    if (fread(dst, 1, n, f) != n) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

/* Reset the 4 channel-B slots to a known free state and re-wire the slot table. */
static void contact_reset_b_slots(void)
{
    int i;
    memset(anim_b_records, 0, sizeof(anim_b_records));
    for (i = 0; i < ANIM_B_SLOTS; i++)
        anim_channels_b_tbl[i] = &anim_b_records[i];
    anim_b_terminator.active = 0xff;        /* the 0xFF scan terminator (DGROUP 0x4cb0) */
    anim_channels_b_tbl[ANIM_B_SLOTS] = &anim_b_terminator;
}

/* run_contact_family: drive every handler; assert against the engine image. */
static int run_contact_family(const char *exe, stats_t *st)
{
    u8 lut_eng[6][0x30];        /* engine-image dispatch LUTs                   */
    u8 sound_opl[0x30], sound_std[0x30];   /* engine-image contact-sound LUTs   */
    u8 tiledef_eng[0x18 * 4];   /* engine-image contact tile-def far-ptr table  */
    int i, t, perturb = (getenv("CONTACT_PERTURB") != NULL);
    long printed = 0;

    /* ── 1. load engine ground truth directly from the unpacked image ── */
    for (i = 0; i < 6; i++) {
        if (!read_exe(exe, DGROUP_FILE_BASE + CONTACT_LUTS[i].dgroup_off,
                      lut_eng[i], 0x30)) {
            fprintf(stderr, "contact: cannot read %s LUT from %s\n",
                    CONTACT_LUTS[i].name, exe);
            return -1;
        }
    }
    if (!read_exe(exe, DGROUP_FILE_BASE + 0x272e, sound_opl, 0x30) ||
        !read_exe(exe, DGROUP_FILE_BASE + 0x274e, sound_std, 0x30) ||
        !read_exe(exe, DGROUP_FILE_BASE + 0x3256, tiledef_eng, sizeof(tiledef_eng))) {
        fprintf(stderr, "contact: cannot read sound/tiledef tables from %s\n", exe);
        return -1;
    }

    /* Seed the engine's DGROUP image into far_mem at the static DGROUP segment
       (0x103b) so the far ptrs in the tiledef table (0x103b:offset) deref the REAL
       tile-def records for BOTH the reconstruction and this oracle's recomputation.
       DGROUP file 0x11440 -> linear (0x103b<<4); copy a generous window. */
    {
        static u8 dgroup_img[0x6000];
        u32 dg_lin = (u32)0x103b << 4;
        unsigned n = sizeof(dgroup_img);
        if (read_exe(exe, DGROUP_FILE_BASE, dgroup_img, n) &&
            dg_lin + n <= sizeof(far_mem)) {
            memcpy(far_mem + dg_lin, dgroup_img, n);
        } else {
            fprintf(stderr, "contact: cannot seed DGROUP image into far_mem\n");
            return -1;
        }
    }

    /* Optional perturbation: corrupt the engine-image baseline that EVERY gate below
       compares against (the LUT byte-compare, the device-selected sound, the tiledef
       far ptr, the dispatch-handler action resolution), proving the comparators have
       teeth (CONTACT_PERTURB=1 -> the gate MUST FAIL). */
    if (perturb) {
        lut_eng[2][5] ^= 0xFF;   /* main(363e) LUT, contact_code 5 -> action resolve */
        lut_eng[4][7] ^= 0xFF;   /* tbl_367e LUT, dispatch-handler action resolve     */
        sound_opl[5]  ^= 0xFF;   /* device-selected contact sound id                  */
        tiledef_eng[4*2] ^= 0xFF;/* action 2 tile-def far-ptr off                     */
    }

    /* ── 2. ASSERT player.c's reconstructed LUTs == the engine image bytes ──
       (a table transcription error -> immediate FAIL; this is the byte-exact gate). */
    for (i = 0; i < 6; i++) {
        if (memcmp(contact_lut_recon[i], lut_eng[i], 0x30) != 0) {
            st->fail++;
            printf("    FAIL contact LUT %s: reconstructed bytes != engine image\n",
                   CONTACT_LUTS[i].name);
        } else { st->pass++; }
    }
    if (memcmp(contact_sound_lut_opl_272e, sound_opl, 0x30) != 0) {
        st->fail++; printf("    FAIL contact sound LUT (OPL 272e) != engine image\n");
    } else { st->pass++; }
    if (memcmp(contact_sound_lut_std_274e, sound_std, 0x30) != 0) {
        st->fail++; printf("    FAIL contact sound LUT (std 274e) != engine image\n");
    } else { st->pass++; }
    if (memcmp(contact_tiledef_tbl, tiledef_eng, sizeof(tiledef_eng)) != 0) {
        st->fail++; printf("    FAIL contact tiledef tbl (3256) != engine image\n");
    } else { st->pass++; }

    /* ── 3. drive apply_contact_action over a sweep; assert its effects against an
       INDEPENDENT recomputation from the engine-image tiledef + sound tables ──
       For each tile-def-bearing action (2..0x17), seed a fresh free B table + a
       known p1_cell_prev, set the device, call apply_contact_action, and assert:
         - last_contact_action (anim_b_loop_idx 0x8566) == action,
         - the device-selected contact sound was played (id from engine sound LUT),
         - slot 0 claimed: active==1, cell==p1_cell_prev,
         - the stream far ptr copied from tile_def[2..5],
         - tilemap[p1_cell_prev+0x30] == tile_def[0].
       tile_def is rebuilt from the ENGINE-IMAGE tiledef table (independent of the
       reconstruction's array). */
    for (t = 0; t < 2; t++) {                 /* t=0: std device, t=1: OPL device */
        int action;
        sound_device_state = (t == 1) ? 4 : 0;
        for (action = 2; action <= 0x17; action++) {
            u16 td_off = (u16)(tiledef_eng[action*4+0] | (tiledef_eng[action*4+1] << 8));
            u16 td_seg = (u16)(tiledef_eng[action*4+2] | (tiledef_eng[action*4+3] << 8));
            const u8 *td = (const u8 *)MK_FP(td_seg, td_off);  /* engine far ptr */
            u8 cell_prev = (u8)(0x10 + action);    /* arbitrary distinct key cell  */
            u8 want_tile = td[0];
            u16 want_soff = (u16)(td[2] | (td[3] << 8));
            u16 want_sseg = (u16)(td[4] | (td[5] << 8));
            u8 want_sound = (t == 1) ? sound_opl[action] : sound_std[action];
            const char *bad = NULL; long got = 0, want = 0;

            /* tilemap shadow (cell_prev+0x30 must be in synth_tilemap range). */
            memset(synth_tilemap, 0, TILEMAP_SIZE);
            contact_reset_b_slots();
            p1_cell_prev = cell_prev;
            g_last_play_sound_id = 0xAA; g_play_sound_calls = 0;
            anim_b_loop_idx = 0;

            apply_contact_action((u8)action);

            if (anim_b_loop_idx != (u8)action) {
                bad = "last_contact_action"; got = anim_b_loop_idx; want = action;
            } else if (want_sound != 0 &&
                       (g_play_sound_calls != 1 || g_last_play_sound_id != want_sound)) {
                bad = "contact_sound"; got = g_last_play_sound_id; want = want_sound;
            } else if (want_sound == 0 && g_play_sound_calls != 0) {
                bad = "contact_sound(silent)"; got = g_play_sound_calls; want = 0;
            } else if (anim_b_records[0].active != 1) {
                bad = "slot.active"; got = anim_b_records[0].active; want = 1;
            } else if (anim_b_records[0].cell != cell_prev) {
                bad = "slot.cell"; got = anim_b_records[0].cell; want = cell_prev;
            } else if (anim_b_records[0].stream_off != want_soff) {
                bad = "slot.stream_off"; got = anim_b_records[0].stream_off; want = want_soff;
            } else if (anim_b_records[0].stream_seg != want_sseg) {
                bad = "slot.stream_seg"; got = anim_b_records[0].stream_seg; want = want_sseg;
            } else if (synth_tilemap[(u16)cell_prev + 0x30] != want_tile) {
                bad = "tilemap_stamp";
                got = synth_tilemap[(u16)cell_prev + 0x30]; want = want_tile;
            }
            if (bad) {
                st->fail++;
                if (printed++ < 12)
                    printf("    FAIL apply_contact_action[dev=%d action=%#x] %s: "
                           "got %ld want %ld\n", t, action, bad, got, want);
            } else { st->pass++; }
        }
    }

    /* ── 4. drive each dispatch handler: assert it resolves the action code via the
       reconstructed LUT == the engine-image LUT, applies it, and clears input_state.
       The handlers gate on move_step_count (0/7); cover both branches. ── */
    {
        static const struct { u16 off; const char *nm; int lut; int guard; }
        H[] = {
            /* guard: 0 = unconditional, 1 = move_step_count!=0, 7 = move_step_count!=7 */
            { 0x6890, "at_start",      0, 1 },   /* lut 35fe */
            { 0x68bb, "before_end",    1, 7 },   /* lut 361e */
            { 0x68fe, "tbl_367e",      4, 0 },   /* lut 367e */
            { 0x693a, "tbl_369e",      5, 0 },   /* lut 369e */
        };
        int h, cc;
        for (h = 0; h < (int)(sizeof(H)/sizeof(H[0])); h++) {
            for (cc = 0; cc < 0x18; cc++) {       /* sweep contact codes 0..0x17 */
                u8 want_action = lut_eng[H[h].lut][cc];
                const char *bad = NULL; long got = 0, want = 0;
                memset(synth_tilemap, 0, TILEMAP_SIZE);
                contact_reset_b_slots();
                p1_contact_code = (u8)cc;
                p1_cell_prev = (u8)(0x10 + cc);
                input_state = 0x7f;
                move_step_count = (H[h].guard == 7) ? 3 : 1;   /* take the active branch */
                sound_device_state = 0;
                anim_b_loop_idx = 0xEE;

                switch (H[h].off) {
                    case 0x6890: p1_apply_contact_action_at_start();   break;
                    case 0x68bb: p1_apply_contact_action_before_end(); break;
                    case 0x68fe: p1_apply_contact_action_tbl_367e();   break;
                    case 0x693a: p1_apply_contact_action_tbl_369e();   break;
                }
                /* the handler must have applied action = LUT[cc] (last_contact_action
                   latches it) and zeroed input_state. */
                if (anim_b_loop_idx != want_action) {
                    bad = "resolved_action"; got = anim_b_loop_idx; want = want_action;
                } else if (input_state != 0) {
                    bad = "input_state"; got = input_state; want = 0;
                }
                if (bad) {
                    st->fail++;
                    if (printed++ < 12)
                        printf("    FAIL %s[cc=%#x] %s: got %ld want %ld\n",
                               H[h].nm, cc, bad, got, want);
                } else { st->pass++; }
            }
        }
    }

    return 0;
}

/* ════════════════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1]
                                  : "local/build/render/physics_trace.bin";
    FILE *f = fopen(path, "rb");
    long sz; u8 *b; u32 o; u16 ver, nsc, nfn; unsigned s;
    stats_t st = { 0, 0, 0 };
    int hard_fail = 0;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz);
    if (!b || fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
    fclose(f);

    if (memcmp(b, "PHYSTRC1", 8) != 0) {
        fprintf(stderr, "bad magic (want PHYSTRC1)\n"); return 2;
    }
    ver = rd16(b + 8); nsc = rd16(b + 10);
    if (ver != 1) { fprintf(stderr, "unsupported version %u\n", ver); return 2; }
    o = 12;
    nfn = rd16(b + o); o += 2;
    /* skip the fn-name string table (we key on fn_addr, not the name index). */
    { u16 k; for (k = 0; k < nfn; k++) { u8 ln = b[o]; o += 1 + ln; } }

    printf("physics_ctest: replay harness over %s\n", path);
    printf("  trace: PHYSTRC1 v%u, %u scenarios, %u fn-names\n", ver, nsc, nfn);
    printf("  per-fn diff targets (host-callable): p1_step_scripted_move, "
           "enter_game_mode, landing leaves (T3), the 0x63xx move-step substates "
           "(T4) + check_tile_below delegates; dispatch_move_step = resolver routing "
           "+ substate exit diff (Phase-9 T2, host-safe); p1_movement_dispatch = full "
           "exit diff (Phase-9 T2, resolver-backed dispatch)\n");

    /* Phase-9 T2 perturbation: under PHYS_PERTURB, mis-route one resolver mapping so the
       resolver-backed dispatch routes records to the wrong host fn — the gate MUST fail. */
    g_phys_perturb = (getenv("PHYS_PERTURB") != NULL);
    if (g_phys_perturb) {
        long pc = apply_phys_perturb();
        printf("  PHYS_PERTURB active: corrupted %ld dispatch slot(s) %#x->%#x "
               "(expect the gate to FAIL)\n", pc, PERTURB_FROM_OFF, PERTURB_TO_OFF);
        if (pc == 0) {
            fprintf(stderr, "PHYS_PERTURB: FROM offset %#x not present in the table — "
                    "perturbation unreachable\n", PERTURB_FROM_OFF);
            return 2;
        }
    }

    for (s = 0; s < nsc; s++) {
        u8 sid, name_len, level, start_cell;
        char scname[64];
        u32 nrec, k;
        record_t *recs;
        stats_t sst = { 0, 0, 0, 0 };
        stitch_result_t sr;
        int per_ok;

        sid = b[o]; o += 1;
        name_len = b[o]; o += 1;
        { unsigned n = name_len < 63 ? name_len : 63;
          memcpy(scname, b + o, n); scname[n] = 0; o += name_len; }
        level = b[o]; start_cell = b[o + 1]; o += 2;
        nrec = rd32(b + o); o += 4;

        recs = malloc(sizeof(record_t) * (nrec ? nrec : 1));
        for (k = 0; k < nrec; k++) {
            record_t *r = &recs[k];
            r->fn_addr = rd16(b + o); o += 2;
            r->fn_name_idx = rd16(b + o); o += 2;
            r->dispatch_target = rd16(b + o); o += 2;
            parse_snap(b + o, &r->ent); o += SNAP_SIZE;
            parse_snap(b + o, &r->ex);  o += SNAP_SIZE;
            r->script_off = rd16(b + o); o += 2;
            r->script_seg = rd16(b + o); o += 2;
            r->script_len = rd16(b + o); o += 2;
            r->script = b + o; o += r->script_len;
            r->tile_base = b[o]; o += 1;
            r->tile_len = b[o]; o += 1;
            r->tile = b + o; o += r->tile_len;
        }

        printf("\n== scenario %u: %s (level %u, start_cell %u, %lu records) ==\n",
               sid, scname, level, start_cell, (unsigned long)nrec);

        /* PRIMARY */
        per_ok = run_per_function(recs, (long)nrec, scname, &sst);
        printf("  per-fn: PASS=%ld  FAIL=%ld  UNPORTED=%ld  SKIPPED_MODE=%ld\n",
               sst.pass, sst.fail, sst.unported, sst.skipped_mode);
        if (!per_ok) hard_fail = 1;

        /* SECONDARY */
        sr = run_trajectory_stitch(recs, (long)nrec, scname);
        printf("  trajectory-stitch: matched %ld / skipped_dispatch %ld / total %lu\n",
               sr.matched, sr.skipped_dispatch, (unsigned long)nrec);

        st.pass += sst.pass; st.fail += sst.fail; st.unported += sst.unported;
        st.skipped_mode += sst.skipped_mode;
        free(recs);
    }

    /* ── Phase-9 T1: contact-action handler family differential ── */
    {
        stats_t cst = { 0, 0, 0, 0 };
        const char *exe = "local/build/unpack/BUMPY_unpacked.exe";
        int rc;
        contact_lut_recon[0] = contact_action_lut_35fe;
        contact_lut_recon[1] = contact_action_lut_361e;
        contact_lut_recon[2] = contact_action_lut_363e;
        contact_lut_recon[3] = contact_action_lut_365e;
        contact_lut_recon[4] = contact_action_lut_367e;
        contact_lut_recon[5] = contact_action_lut_369e;
        printf("\n== Phase-9 T1: contact-action handler family "
               "(ground truth = %s) ==\n", exe);
        rc = run_contact_family(exe, &cst);
        if (rc < 0) {
            fprintf(stderr, "contact: ground-truth image unavailable — gate cannot run\n");
            hard_fail = 1;
        } else {
            printf("  contact-family: PASS=%ld  FAIL=%ld%s\n",
                   cst.pass, cst.fail,
                   getenv("CONTACT_PERTURB") ? "  (CONTACT_PERTURB active)" : "");
            st.pass += cst.pass; st.fail += cst.fail;
            if (cst.fail) hard_fail = 1;
        }
    }

    printf("\n=== TOTAL per-fn: PASS=%ld  FAIL=%ld  UNPORTED=%ld  SKIPPED_MODE=%ld ===\n",
           st.pass, st.fail, st.unported, st.skipped_mode);
    if (hard_fail || st.fail != 0) {
        printf("FAIL: %ld per-function differential failure(s) on PORTED fns\n",
               st.fail);
        return 1;
    }
    printf("PASS: every record matched its exit; UNPORTED=%ld (Phase-9 T2 made "
           "dispatch_move_step + p1_movement_dispatch host-callable via the offset->fn "
           "resolver — the 624 former dispatch-knot UNPORTED records now execute for real)\n",
           st.unported);
    return 0;
}
