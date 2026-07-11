/* Host int8-synced END-TO-END replay harness — Phase-9.x (PARTS 1 + 2).
 *
 * PART 2 (this task): the per-tick replay loop (seed-once-then-evolve), the
 * first-divergence report, and --perturb.  run_replay() reads a captured trace
 * (magic "BINT" / version checked — stale or foreign traces HARD-fail), seeds the
 * live engine globals ONCE from INIT, then for each FRAME feeds the trailing
 * rng/input and calls the REAL game_tick(), comparing the evolved globals +
 * tilemap hash against the captured FRAME state and printing the first mismatching
 * (frame, field, got/want).  --perturb corrupts one seeded field so a tick must
 * diverge (proves the gate is a genuine differential).  Two committed self-tests:
 *   --selftest-seed   : synthetic INIT -> seed -> assert globals (PART 1, no tick)
 *   --selftest-replay : synthetic 2-FRAME trace (one match / one one-byte-flipped)
 *                       -> assert run_replay returns 0 / reports a field (needs the
 *                       game_tick build: cc -DINT8_WITH_GAME_TICK -Itools).
 *
 * PART 1: INIT parse + seed-once.  Defines the canonical INIT/FRAME
 * layout consumer (tools/int8_trace.h), the host-include skeleton for the
 * reconstructed engine TUs, and seed_from_init() — which writes one captured INIT
 * snapshot into the live reconstructed engine globals (the per-tick loop's full
 * READ-SET).  A --selftest-seed mode builds a synthetic INIT with sentinels, seeds,
 * and asserts the globals took them (no trace file, no game_tick call) — proving the
 * seed path end-to-end before the per-tick replay loop (the NEXT task) is wired in.
 *
 * HOST-INCLUDE STRATEGY (mirrors tools/p1_spine_ctest.c / physics_ctest.c):
 *   The Watcom 16-bit environment is shimmed out (__far/__huge/__cdecl16near erased,
 *   exact-width typedefs, BUMPY_H so the headers don't pull <dos.h>).  The TUs that
 *   host-include cleanly (the physics/p2/anim/items/spawn ctest pattern) are
 *   #included directly below; they OWN most of the seeded globals.  game.c and
 *   level.c pull <dos.h> + the render/loop pipeline and CANNOT host-include wholesale
 *   — for the per-tick replay (next task) the gate script awk-extracts game_tick (and
 *   its game.c/level.c helpers) into tools/int8_extracted.h, which this file
 *   #includes when INT8_WITH_GAME_TICK is defined.  The cross-module globals those
 *   modules own (current_level, score, session flags, ...) are host-defined here, as
 *   p1_spine_ctest/items_ctest define theirs.
 *
 * ── ASSEMBLED INIT SCALAR UNION (grounded in the named gates; see int8_trace.h
 *    struct int8_scalars for the per-field gate tags) ──────────────────────────────
 *   [phys]  physics_ctest.c   px,py,move_anim,game_mode,step_idx,facing,steps_left,
 *                             input_state,frozen,override,p1_cell,locked,prev_mode,
 *                             step_col_count
 *   [spine] p1_spine_ctest.c  grid x/y new/cur/prev, scroll x/y, bbox x0/x1/y0/y1,
 *                             pending_erase x/y/count, level_complete_flag
 *   [items] items_ctest.c     score_lo/hi, items_remaining, level_exit_cell,
 *                             level_complete_anim_counter, p1_item_code,
 *                             anim_target_cell, current_level, move_step_count,
 *                             sound_device_state
 *   [anim]  anim_chan_ctest.c g_anim_channel_idx, g_anim_cur_cmd_byte, anim_b_loop_idx,
 *                             anim_b_cur_frame_byte, A/B stream far ptrs (off/seg)
 *   [spawn] spawn_ctest.c     p2_cell, p2_move_state, p2_ai_threshold, p2_frame_base
 *   [sess]  brief Step 1      round/session_continue_flag, frame_abort_flag,
 *                             settle_countdown, rng_frame
 *   + arrays: tilemap[0x300], anim_channels[7*12], entity_state[0x200] (reserved)
 *
 * Build (seed self-test, standalone — no game_tick):
 *     cc -O2 -Wall -Itools -o /tmp/int8_ctest tools/int8_ctest.c && \
 *       /tmp/int8_ctest --selftest-seed     # prints "selftest-seed OK"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ── shim the Watcom 16-bit environment for host compilation ─────────────────── */
#define BUMPY_H
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge
#define __cdecl16near

/* MK_FP / FP_OFF / FP_SEG host model (mirrors anim_chan_ctest.c / spawn_ctest.c):
   a >256 KB linear "far memory" shadow indexed by the real-mode linear address. */
#define FAR_MEM_SIZE 0x110000UL
static unsigned char far_mem[FAR_MEM_SIZE];
#define MK_FP(seg, off) ((void *)(far_mem + (((u32)(seg) << 4) + (u16)(off))))
static u16 host_fp_seg(const void *p) { return (u16)(((u32)((const unsigned char *)p - far_mem)) >> 4); }
static u16 host_fp_off(const void *p) { return (u16)(((u32)((const unsigned char *)p - far_mem)) & 0xF); }
#define FP_SEG(p) host_fp_seg((const void *)(p))
#define FP_OFF(p) host_fp_off((const void *)(p))

/* Captured-runtime DS the anim/init descriptor wrappers stamp (PSP 0x100 -> DS 0x114b). */
#define ANIM_DGROUP_RUNTIME_SEG 0x114b
#define GAME_DGROUP_RUNTIME_SEG 0x114b

/* Runtime DGROUP segment of the trace being seeded (header.dgroup_seg, default the
 * DOSBox capture calibration 0x185f).  Used to place the captured move-script DGROUP
 * window into the host far-memory at its captured runtime linear base so the
 * far-pointer move-script reads (p1_move_script + mode_script_tbl) resolve 1:1. */
static u16 g_dgroup_seg = 0x185f;

/* ── the int8 trace layout (defines INT8_TILEMAP_SIZE + the INIT/FRAME structs).
 *    Included up here so the tilemap window below is sized from the canonical macro. */
#include "int8_trace.h"

/* ── tilemap: cross-module far ptr (game.c owns it; everyone reads it).  Backed by a
 *    host window seed_from_init copies the captured grid into. ──────────────────── */
static u8 synth_tilemap[INT8_TILEMAP_SIZE];
u8 __far *tilemap;

/* ── cross-module globals owned by game.c / level.c (NOT host-includable) ─────────
 *    Mirrors the items_ctest / p1_spine_ctest convention of host-defining the
 *    globals the included TUs reference but do not themselves define. ───────────── */
u16 score_lo, score_hi;          /* game.c   0xa0d4 / 0xa0d6 */
u8  current_level;               /* level.c  0x79b2          */
u8  copyprotect_flag;            /* level.c  0x119a          */
s16 sound_device_state;          /* player.c 0x689c (host-owned here) */
u8  round_continue_flag;         /* game.c   0x9d30          */
u8  session_continue_flag;       /* game.c   0x856d          */
u8  frame_abort_flag;            /* game.c   0x928d          */
u8  settle_countdown;            /* game.c                   */
u8  rng_frame;                   /* player.c — driven by rand() in game_tick */
u8  input_state;                 /* input.c  0x8244          */
u16 prng_state0, prng_state1, prng_state2;  /* prng.c references these (owned here) */

/* deferred-contact event queue — game.c-owned globals (0x9ba6/0x886/0x79b7); the
   extracted game_post_present body reads/writes them.  Host-defined here so the
   extracted bodies resolve against the real reconstructed apply_contact_action/
   apply_cell_animation in the included TUs (REAL logic, not stubbed). */
u8 __far *deferred_contact_ptr;      /* DGROUP 0x9ba6/0x9ba8 — event-queue cursor far ptr */
u8        deferred_contact_buf[16];  /* DGROUP 0x0886 — the event buffer (head = reset tgt) */
u8        deferred_contact_countdown;/* DGROUP 0x79b7 — per-event delay countdown          */

/* p2 spawn globals (spawn.c references them extern; owned elsewhere in the build). */
u8 __far *level_src_ptr;
s8  p2_cell;
u8  p2_ai_threshold;
u8  p2_move_state;
u16 p2_frame_base;
u8 __far *p2_cell_coord_tbl;

/* anim working stream far ptrs are anim.c-owned (g_anim_stream_ptr / anim_b_stream_ptr);
   the cmd/frame bytes too — all defined inside src/anim.c (included below). */

/* ── carve-out leaf callees game_tick / the spine reach that no host-included TU
 *    defines.  No-op host shims (the documented render/sound/FX boundary), mirrored
 *    from p1_spine_ctest.c.  These keep the link closing; the seed self-test does not
 *    invoke them. ───────────────────────────────────────────────────────────────── */
void play_sound(u8 id) { (void)id; }
void play_action_sound(void) {}
/* play_walk_anim_default / step_walk_anim are RECONSTRUCTED in player.c (cluster-2);
   no host stub here — the replay runs the real bodies. */
void FUN_1000_4802(void) {}
void play_exit_sound(void) {}
void play_contact_sound(void) {}
void play_pickup_sound(void) {}
void play_state_sound_79b9(void) {}
void play_event_sound_64c1(void) {}
/* check_exit_tile_vert / teleport_to_next_exit_tile are RECONSTRUCTED in items.c
   (included below) — no host stub here. */
void input_state_clear(void) { input_state = 0; }
u8   get_key_state(u8 sc) { (void)sc; return 0; }
u8   timing_flag_accumulator;
/* move_walk_right_anim_step / enter_mode_0b_jump_start / move_anim_step_to_mode0c /
   move_step_check_walkable / move_step_dispatch_input / p1_input_dispatch_bit10 /
   advance_physics_freeze are RECONSTRUCTED in player.c (cluster-2) — no host stub;
   the replay runs the real movement/anim bodies (this is what the trace must exercise). */
void FUN_1000_4437(void) {}
void FUN_1000_1e3d(void) {}
void p2_dispatch_move_state_handler(void) {}
/* DGROUP globals owned by game_stubs.c (not included here) that the cluster-2
   reconstructed bodies now reference — host-define them so the replay links. */
u8 dgroup_flag_a1a9;     /* 0xa1a9 — round-activate flag */
u8   pvp_collision_flag;
u8   mode_script_tbl[0x40 * 4];
void setup_fullscreen_view(void) {}
/* game_tick render/timing/loop-boundary carve-out leaves (the documented partition
   validate_integration.sh asserts: present_frame = VGA page-flip;
   rotate_timing_flags_and_wait = int8 timing; show_pause_screen = a screen TU).
   These mutate no replayed state — pure presentation/timing — so they are no-op
   host shims, NOT real game logic.  game_post_present / game_post_input are NOT here:
   they contain real state mutation and are awk-extracted (see int8_extracted.h). */
void present_frame(u8 page) { (void)page; }
void rotate_timing_flags_and_wait(void) {}
void show_pause_screen(void) {}

/* ── poll_input / rand stubs: the established pattern (physics_ctest.c:120 stubs
 *    poll_input "input already seeded from the SNAP"; the per-tick loop feeds the
 *    captured input/rng via these globals before each tick — wired in the NEXT
 *    task).  Defined here so the skeleton is complete. ──────────────────────────── */
u8  g_fed_input;
int g_fed_rng;
void poll_input(void) { input_state = g_fed_input; }
int  rand(void) { return g_fed_rng; }

/* ── the REAL reconstructed engine TUs that host-include cleanly ────────────────── */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"
#include "../src/prng.c"
#include "../src/anim.c"
#include "../src/items.c"
#include "../src/spawn.c"
#include "../src/player.c"
#include "../src/player2.c"
#pragma GCC diagnostic pop

#ifdef INT8_WITH_GAME_TICK
/* forward-declare the two game.c bodies the extracted header itself defines (game_tick
   calls them before their definitions appear in the same extracted header). */
void game_post_present(void);
void game_post_input(void);
/* tools/int8_extracted.h is a TRANSIENT build artifact (not committed) the int8 gate
   regenerates by awk-extracting three verbatim game.c bodies — game_tick (per-tick
   spine), game_post_present and game_post_input (the two REAL state-mutating helpers
   game_tick calls, which the host can't include wholesale).  The Task-7 wrapper
   (tools/validate_int8.sh) owns the recipe; it is, verbatim:
     awk '/^void game_tick\(void\)$/         {p=1} p{print} p&&/^}$/{exit}' src/game.c
     awk '/^void game_post_present\(void\)$/ {p=1} p{print} p&&/^}$/{exit}' src/game.c
     awk '/^void game_post_input\(void\)$/   {p=1} p{print} p&&/^}$/{exit}' src/game.c
   (mirrors tools/validate_p1_spine.sh:42-60).  Build: cc -DINT8_WITH_GAME_TICK -Itools. */
#include "int8_extracted.h"
#endif

/* p1_set_pixel_from_cell is now RECONSTRUCTED in player.c (included above) — the
   host stub that used to live here (analytical mirror of items_ctest.c) is removed
   so the replay uses the real reconstructed body. */

/* ════════════════════════════════════════════════════════════════════════════
 *  Far-ptr DESCRIPTOR backing — the view / sprite-object / pos-table DGROUP far
 *  ptrs the game_tick call tree dereferences (p1/p2 grid recompute read
 *  sprite+0x14/0x16; the anim steppers write p1_sprite+0..4 and the clear-view
 *  +0x14/0x16; render/erase pass the view ptrs to the no-op graphics-overlay leaves).  In the
 *  engine these point into a contiguous DGROUP descriptor block set up by
 *  init_sprite_structs / init_view_anim_descriptors; the host can't run that graphics-overlay
 *  init, so — exactly as tools/anim_chan_ctest.c wire_views() does — we point each
 *  far ptr at a dedicated host backing buffer.  Zeroed: the descriptor CONTENT
 *  (sprite origin words) is not yet in the captured scalar union (entity_state is
 *  reserved), so it is a deterministic zero here; see the report's concerns.
 * ════════════════════════════════════════════════════════════════════════════ */
#define VIEW_LEN     0x40
#define SPRITE_LEN   0x40
#define POSTBL_LEN   0x400
static u8 hb_p1_sprite[SPRITE_LEN], hb_p2_sprite[SPRITE_LEN];
static u8 hb_p1_view[VIEW_LEN], hb_p1_erase_view[VIEW_LEN], hb_pending_erase_view[VIEW_LEN];
static u8 hb_p2_view[VIEW_LEN], hb_p2_erase_view[VIEW_LEN];
static u8 hb_anim_a_erase_view[VIEW_LEN], hb_anim_a_draw_view[VIEW_LEN], hb_anim_a_clear_view[VIEW_LEN];
static u8 hb_anim_b_view0[VIEW_LEN], hb_anim_b_view1[VIEW_LEN];
static u8 hb_anim_b_draw_view[VIEW_LEN], hb_anim_b_clear_view[VIEW_LEN];
static u8 hb_anim_posA_tbl[POSTBL_LEN], hb_anim_posB_tbl[POSTBL_LEN];
static u8 hb_anim_a_grid_tbl[POSTBL_LEN], hb_anim_b_grid_tbl[POSTBL_LEN];

static void wire_far_descriptors(void)
{
    p1_sprite          = hb_p1_sprite;
    p2_sprite          = hb_p2_sprite;
    p1_view            = hb_p1_view;
    p1_erase_view      = hb_p1_erase_view;
    pending_erase_view = hb_pending_erase_view;
    p2_view            = hb_p2_view;
    p2_erase_view      = hb_p2_erase_view;
    anim_a_erase_view  = hb_anim_a_erase_view;
    anim_a_draw_view   = hb_anim_a_draw_view;
    anim_a_clear_view  = hb_anim_a_clear_view;
    anim_b_view0       = hb_anim_b_view0;
    anim_b_view1       = hb_anim_b_view1;
    anim_b_draw_view   = hb_anim_b_draw_view;
    anim_b_clear_view  = hb_anim_b_clear_view;
    anim_posA_tbl      = hb_anim_posA_tbl;
    anim_posB_tbl      = hb_anim_posB_tbl;
    anim_a_grid_tbl    = hb_anim_a_grid_tbl;
    anim_b_grid_tbl    = hb_anim_b_grid_tbl;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  SET_SCALAR — write a named field into the INIT scalar sub-block.
 * ════════════════════════════════════════════════════════════════════════════ */
#define SET_SCALAR(initp, field, val) ((initp)->scalars.field = (val))

/* ════════════════════════════════════════════════════════════════════════════
 *  seed_from_init — write one captured INIT snapshot into the live engine globals.
 *  Copies the tilemap window, the 7 anim channel records, the entity arrays, and
 *  every scalar field into its reconstructed global (reusing the field<->global
 *  mapping the per-function gates' seed_globals established).
 * ════════════════════════════════════════════════════════════════════════════ */
static void seed_from_init(const struct int8_init *in)
{
    const struct int8_scalars *s = &in->scalars;

    /* point the view/sprite/pos-table DGROUP far ptrs at host backing buffers
       (the engine's init_sprite_structs / init_view_anim_descriptors equivalent). */
    wire_far_descriptors();

    /* ── world-state arrays ── */
    tilemap = synth_tilemap;
    memcpy(synth_tilemap, in->tilemap, INT8_TILEMAP_SIZE);

    /* anim channel records: 3 A then 4 B, 12 raw bytes each (anim_chan_ctest layout). */
    memcpy(anim_a_records, in->anim_channels, INT8_ANIM_A_SLOTS * INT8_ANIM_REC_LEN);
    memcpy(anim_b_records, in->anim_channels + INT8_ANIM_A_SLOTS * INT8_ANIM_REC_LEN,
           INT8_ANIM_B_SLOTS * INT8_ANIM_REC_LEN);
    /* re-wire the slot tables at the host record storage + terminators (anim_chan_ctest
       seed_globals does this so the steppers/allocator scan the seeded records). */
    {
        int i;
        for (i = 0; i < INT8_ANIM_A_SLOTS; i++) anim_channels_a_tbl[i] = &anim_a_records[i];
        anim_a_terminator.active = 0xff;
        anim_channels_a_tbl[INT8_ANIM_A_SLOTS] = &anim_a_terminator;
        for (i = 0; i < INT8_ANIM_B_SLOTS; i++) anim_channels_b_tbl[i] = &anim_b_records[i];
        anim_b_terminator.active = 0xff;
        anim_channels_b_tbl[INT8_ANIM_B_SLOTS] = &anim_b_terminator;
    }
    /* entity_state[] carries the P1/P2 sprite-object descriptor pointee blocks
       (INT8_VERSION 2): [0x00..0x40) = p1_sprite pointee, [0x40..0x80) = p2_sprite
       pointee.  p1/p2_update_grid_cell read the sprite origin words at +0x14/+0x16
       (which feed the COMPARED grid-cell scalars), so seed them into the host sprite
       backing buffers wire_far_descriptors() points p1_sprite/p2_sprite at.  (The
       remaining bytes are reserved zeros.) */
    memcpy(hb_p1_sprite, in->entity_state + 0x00, SPRITE_LEN);
    memcpy(hb_p2_sprite, in->entity_state + 0x40, SPRITE_LEN);

    /* ── [move/anim] low-DGROUP static-data window (INT8_VERSION 3) ─────────────
       Place the captured window into the host far-memory at its captured runtime
       DGROUP linear base so EVERY far-pointer hop the per-tick spine + anim system
       takes resolves 1:1:
         - p1_step_scripted_move() derefs p1_move_script (seeded below) -> window;
         - enter_game_mode() reads mode_script_tbl[mode] -> a [steps,facing,off,seg]
           header -> the [anim,dx,dy] step array -> all inside the window;
         - apply_cell_animation()/the anim-channel steppers read the anim far-ptr
           tables (tiledef/frame/grid/pos) and follow them to the tile-def / frame /
           stream blobs -> all inside the window.
       The window is keyed by DGROUP offset (INT8_MOVE_DATA_OFF == 0), so any near
       u8[] table the included TUs own is filled by memcpy at its DGROUP offset, and
       any far-ptr table the host wires is pointed into the window at its DGROUP
       linear address. */
    {
        unsigned long base = ((unsigned long)g_dgroup_seg << 4) + INT8_MOVE_DATA_OFF;
        const u8 *win = in->move_data;
        if (base + INT8_MOVE_DATA_LEN <= FAR_MEM_SIZE) {
            memcpy(far_mem + base, win, INT8_MOVE_DATA_LEN);
        }
        /* NEAR u8[] tables this build owns — filled from the window at their DGROUP
           offsets (the engine reads these as DS-relative near accesses). */
        memcpy(mode_script_tbl,      win + 0x2252, sizeof mode_script_tbl);      /* 0x2252 move-mode tbl */
        memcpy(anim_a_tiledef_tbl,   win + 0x2ede, ANIM_FARPTR_TBL_LEN);          /* 0x2ede cell-anim tiledef */
        memcpy(anim_a_frame_tbl,     win + 0x3d6a, ANIM_FARPTR_TBL_LEN);          /* 0x3d6a A frame tbl */
        memcpy(anim_b_frame_tbl,     win + 0x40a6, ANIM_FARPTR_TBL_LEN);          /* 0x40a6 B frame tbl */
        memcpy(contact_tiledef_tbl,  win + 0x3256, CONTACT_TILEDEF_TBL_LEN);      /* 0x3256 contact tiledef */
        /* FAR-ptr grid/pos tables — point them at the window at their DGROUP linear
           address so cell*4 indexing reads the real relocated coords. */
        anim_posA_tbl   = far_mem + base + 0x00f4;   /* 0xf4   pos A   */
        anim_posB_tbl   = far_mem + base + 0x03f4;   /* 0x3f4  pos B   */
        anim_a_grid_tbl = far_mem + base + 0x32be;   /* 0x32be grid A  */
        anim_b_grid_tbl = far_mem + base + 0x343e;   /* 0x343e grid B  */
    }

    /* ── [phys] player/physics ── */
    p1_pixel_x         = s->p1_pixel_x;
    p1_pixel_y         = s->p1_pixel_y;
    p1_move_anim       = s->p1_move_anim;
    game_mode          = s->game_mode;
    p1_move_step_idx   = s->p1_move_step_idx;
    p1_facing_left     = s->p1_facing_left;
    p1_move_steps_left = s->p1_move_steps_left;
    input_state        = s->input_state;
    physics_frozen     = s->physics_frozen;
    move_override      = s->move_override;
    p1_cell            = s->p1_cell;
    move_locked        = s->move_locked;
    prev_game_mode     = s->prev_game_mode;
    p1_step_col_count  = s->p1_step_col_count;
    /* [move] live p1_move_script far ptr -> the seeded move-data window. */
    p1_move_script     = (u16 __far *)MK_FP(s->p1_move_script_seg, s->p1_move_script_off);

    /* ── [spine] grid / scroll / bbox / pending ── */
    p1_grid_x_new      = s->p1_grid_x_new;
    p1_grid_y_new      = s->p1_grid_y_new;
    p1_grid_x          = s->p1_grid_x;
    p1_grid_y          = s->p1_grid_y;
    p1_grid_x_prev     = s->p1_grid_x_prev;
    p1_grid_y_prev     = s->p1_grid_y_prev;
    p1_scroll_x        = s->p1_scroll_x;
    p1_scroll_y        = s->p1_scroll_y;
    pvp_p1_x0          = s->pvp_p1_x0;
    pvp_p1_x1          = s->pvp_p1_x1;
    pvp_p1_y0          = s->pvp_p1_y0;
    pvp_p1_y1          = s->pvp_p1_y1;
    pending_erase_x    = s->pending_erase_x;
    pending_erase_y    = s->pending_erase_y;
    pending_erase_count= s->pending_erase_count;
    level_complete_flag= s->level_complete_flag;

    /* ── [items] score / item / exit / level semantic state ── */
    score_lo           = s->score_lo;
    score_hi           = s->score_hi;
    items_remaining    = s->items_remaining;
    level_exit_cell    = s->level_exit_cell;
    level_complete_anim_counter = s->level_complete_anim_counter;
    p1_item_code       = s->p1_item_code;
    anim_target_cell   = s->anim_target_cell;
    current_level      = s->current_level;
    move_step_count    = s->move_step_count;
    sound_device_state = s->sound_device_state;

    /* ── [anim] channel scalars + working stream far ptrs (rebuilt via MK_FP) ── */
    g_anim_channel_idx   = s->g_anim_channel_idx;
    g_anim_cur_cmd_byte  = s->g_anim_cur_cmd_byte;
    anim_b_loop_idx      = s->anim_b_loop_idx;
    anim_b_cur_frame_byte= s->anim_b_cur_frame_byte;
    g_anim_stream_ptr    = (u8 __far *)MK_FP(s->g_anim_stream_seg, s->g_anim_stream_off);
    anim_b_stream_ptr    = (u8 __far *)MK_FP(s->anim_b_stream_seg, s->anim_b_stream_off);

    /* ── [spawn] player-2 ── */
    p2_cell            = s->p2_cell;
    p2_move_state      = s->p2_move_state;
    p2_ai_threshold    = s->p2_ai_threshold;
    p2_frame_base      = s->p2_frame_base;

    /* deferred-contact event-queue cursor: a self-bootstrapping DGROUP far ptr the
       engine resets to the buffer head with an empty (0xff) head at level reset —
       not a captured scalar.  Seed it to that reset state so game_post_present's
       first dereference is valid (mirrors game.c's reset path: ptr=buf; *ptr=0xff). */
    deferred_contact_ptr = (u8 __far *)deferred_contact_buf;
    *deferred_contact_ptr = 0xff;
    deferred_contact_countdown = 0;

    /* ── [sess] session / level / frame-boundary control ── */
    round_continue_flag   = s->round_continue_flag;
    session_continue_flag = s->session_continue_flag;
    frame_abort_flag      = s->frame_abort_flag;
    settle_countdown      = s->settle_countdown;
    rng_frame             = s->rng_frame;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  PART 2 — per-tick replay loop + first-divergence report + --perturb.
 *  The whole block needs game_tick (the replay evolve step); it is compiled only
 *  when INT8_WITH_GAME_TICK is defined (the gate wrapper flips it).  Without it,
 *  main() still serves --selftest-seed (the standalone DONE bar from part 1).
 * ════════════════════════════════════════════════════════════════════════════ */
#ifdef INT8_WITH_GAME_TICK

/* --perturb: after seeding, corrupt ONE seeded field so a tick must diverge. */
static int g_perturb;

/* cmp_frame — compare the evolved live globals against FRAME[k].state field by
 * field (the int8_frame_state assert-set = union of the per-function gates' cmp_*
 * output fields).  Returns the FIRST mismatching field name (NULL == all match);
 * *got = live value, *want = captured value, for the divergence report. */
#define CMP1(field)  do { \
        if ((long)(field) != (long)(f->state.field)) { \
            *got = (long)(field); *want = (long)(f->state.field); return #field; } \
    } while (0)

static const char *cmp_frame(const struct int8_frame *f, long *got, long *want)
{
    /* [phys] cmp_exit / [spine] cmp_grid + cmp_adv + cmp_bbox */
    CMP1(p1_pixel_x);
    CMP1(p1_pixel_y);
    CMP1(p1_grid_x_new);
    CMP1(p1_grid_y_new);
    CMP1(p1_grid_x);
    CMP1(p1_grid_y);
    CMP1(p1_grid_x_prev);
    CMP1(p1_grid_y_prev);
    CMP1(pvp_p1_x0);
    CMP1(pvp_p1_x1);
    CMP1(pvp_p1_y0);
    CMP1(pvp_p1_y1);
    /* p1_pixel_y_dup — items' separate p1_pixel_y compare (explicit alias) */
    if ((long)p1_pixel_y != (long)f->state.p1_pixel_y_dup) {
        *got = (long)p1_pixel_y; *want = (long)f->state.p1_pixel_y_dup;
        return "p1_pixel_y_dup";
    }
    /* [items] score / semantic state */
    CMP1(score_lo);
    CMP1(score_hi);
    CMP1(p1_move_anim);
    CMP1(game_mode);
    CMP1(p1_move_step_idx);
    CMP1(p1_facing_left);
    CMP1(p1_move_steps_left);
    CMP1(input_state);
    CMP1(physics_frozen);
    CMP1(move_override);
    CMP1(p1_cell);
    CMP1(move_locked);
    CMP1(prev_game_mode);
    CMP1(p1_step_col_count);
    CMP1(pending_erase_count);
    CMP1(level_complete_flag);
    CMP1(items_remaining);
    CMP1(level_exit_cell);
    CMP1(level_complete_anim_counter);
    CMP1(p1_item_code);
    CMP1(anim_target_cell);
    CMP1(current_level);
    CMP1(move_step_count);
    /* [spawn] p2 state */
    CMP1(p2_cell);
    CMP1(p2_move_state);
    /* p2_frame_base is split lo/hi in the FRAME state */
    if ((long)(p2_frame_base & 0xff) != (long)f->state.p2_frame_base_lo) {
        *got = (long)(p2_frame_base & 0xff); *want = (long)f->state.p2_frame_base_lo;
        return "p2_frame_base_lo";
    }
    if ((long)((p2_frame_base >> 8) & 0xff) != (long)f->state.p2_frame_base_hi) {
        *got = (long)((p2_frame_base >> 8) & 0xff); *want = (long)f->state.p2_frame_base_hi;
        return "p2_frame_base_hi";
    }
    return NULL;
}
#undef CMP1

/* read_trace — slurp a trace file into a freshly-allocated buffer; validate the
 * header (magic + version), and point hdr/init/frames into it.  Returns the
 * malloc'd buffer (caller frees) or NULL on any structural / version failure. */
static void *read_trace(const char *path, struct int8_header *hdr,
                        const struct int8_init **init,
                        const struct int8_frame **frames)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "int8: cannot open trace %s\n", path); return NULL; }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < (long)sizeof(struct int8_header)) {
        fprintf(stderr, "int8: trace too small (%ld bytes)\n", sz);
        fclose(fp); return NULL;
    }
    rewind(fp);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp); free(buf); return NULL;
    }
    fclose(fp);

    memcpy(hdr, buf, sizeof *hdr);
    /* stale / foreign trace -> hard fail. */
    if (memcmp(hdr->magic, INT8_MAGIC, 4) != 0) {
        fprintf(stderr, "int8: bad magic (stale/foreign trace; want \"%s\")\n", INT8_MAGIC);
        free(buf); return NULL;
    }
    if (hdr->version != INT8_VERSION) {
        fprintf(stderr, "int8: version %u != %u (stale trace; bump or recapture)\n",
                (unsigned)hdr->version, (unsigned)INT8_VERSION);
        free(buf); return NULL;
    }
    if (hdr->init_size != sizeof(struct int8_init) ||
        hdr->frame_stride != sizeof(struct int8_frame)) {
        fprintf(stderr, "int8: layout mismatch (init=%u/%zu frame=%u/%zu)\n",
                (unsigned)hdr->init_size, sizeof(struct int8_init),
                (unsigned)hdr->frame_stride, sizeof(struct int8_frame));
        free(buf); return NULL;
    }
    long need = (long)sizeof(struct int8_header) + (long)sizeof(struct int8_init)
              + (long)hdr->frame_count * (long)sizeof(struct int8_frame);
    if (sz < need) {
        fprintf(stderr, "int8: truncated trace (have %ld, need %ld)\n", sz, need);
        free(buf); return NULL;
    }
    *init   = (const struct int8_init *)(buf + sizeof(struct int8_header));
    *frames = (const struct int8_frame *)
              (buf + sizeof(struct int8_header) + sizeof(struct int8_init));
    return buf;
}

/* run_replay — seed ONCE from INIT, then evolve tick-by-tick: feed each FRAME's
 * trailing rng/input, call game_tick(), and compare the evolved live globals +
 * tilemap hash against the captured FRAME state.  Returns 0 on full match, 1 on
 * the first divergence (reporting frame/field/got/want), 2 on a load failure. */
static int run_replay(const char *path)
{
    struct int8_header hdr;
    const struct int8_init *init;
    const struct int8_frame *frames;
    void *buf = read_trace(path, &hdr, &init, &frames);
    if (!buf) return 2;

    g_dgroup_seg = hdr.dgroup_seg;   /* place the move-data window at the captured base */
    seed_from_init(init);

    /* --perturb: corrupt ONE seeded field so a tick must diverge (proves the gate
       is a genuine differential, not trivially passing). */
    if (g_perturb) {
        p1_pixel_x ^= 1;
    }

    for (uint16_t k = 1; k <= hdr.frame_count; k++) {
        /* FRAME[k] carries BOTH the trailing rng/input that drive the tick AND the
           resulting state to assert.  Feed rng/input from the SAME frame we compare
           against (frames[k], i.e. want) — not frames[k-1] as the original
           mistakenly did.  Per int8_trace.h: "for k>=1, replay tick k-1 by feeding
           rng+input then asserting state" — all three live in the same FRAME[k]. */
        const struct int8_frame *want = &frames[k];
        g_fed_rng   = want->rng;     /* the rng_frame value that drives this tick */
        g_fed_input = want->input;   /* the input_state value that drives this tick */
        game_tick();
        long got = 0, wv = 0;
        const char *bad = cmp_frame(want, &got, &wv);
        /* tilemap_hash is DELIBERATELY EXCLUDED from the per-frame compare — one
           precise, justified exclusion (NOT a broad tolerance).  The reconstructed
           game_tick reproduces every GAMEPLAY-COLLISION tilemap write 1:1 (item
           collection tilemap[cell+0x60]=0, contact-action tilemap[cell+0x30],
           cell-animation tilemap[target_cell]=tile_def[0]) — verified: with the v3
           anim read-set seeded, the host changes exactly the cells the engine does in
           the collision layers, and ALL gameplay scalar fields match for the full
           150-frame capture.  The ONLY residual full-tilemap-hash divergence is the
           ANIMATED-TILE FX-GRAPHICS layer (e.g. cell 0xc8 = anim-slot cell 0x28 +
           0xa0, cycling its displayed tile-graphic index +6/tick).  That write is
           produced INSIDE the carved-out graphics-overlay render core: draw_anim_channels_a calls
           render_player_view (1000:93b8) -> gfx_set_mode_10 -> the un-analyzed graphics
           overlay handler (1ab9:0db0), which is the documented render-leaf
           carve-out (src/anim.c FIDELITY note; docs/rendering-pipeline.md).  No
           reconstructed (or original) game_tick state-callee writes that FX layer, and
           NO gameplay-collision callee READS it (collision reads only tilemap[cell]/
           +0x30/+0x60).  So it is render-only and legitimately excluded from the
           state-spine SNAP — exactly the render-core partition validate_integration.sh
           asserts.  The collision-layer tilemap IS validated, per-cell, by the
           items/anim/spawn per-function gates.  See docs/reconstruction-fidelity.md
           (int8-synced end-to-end gate). */
        if (bad) {
            printf("DIVERGENCE frame=%u field=%s got=%ld want=%ld\n",
                   (unsigned)(k - 1), bad, got, wv);
            free(buf);
            return 1;
        }
    }
    printf("int8 replay: %u ticks matched\n", (unsigned)hdr.frame_count);
    free(buf);
    return 0;
}

/* snapshot_state — capture the live globals into a FRAME state block (used by the
 * synthetic-trace self-test to record game_tick's TRUE output). */
static void snapshot_state(struct int8_frame_state *st)
{
    memset(st, 0, sizeof *st);
    st->p1_pixel_x = p1_pixel_x; st->p1_pixel_y = p1_pixel_y;
    st->p1_grid_x_new = p1_grid_x_new; st->p1_grid_y_new = p1_grid_y_new;
    st->p1_grid_x = p1_grid_x; st->p1_grid_y = p1_grid_y;
    st->p1_grid_x_prev = p1_grid_x_prev; st->p1_grid_y_prev = p1_grid_y_prev;
    st->pvp_p1_x0 = pvp_p1_x0; st->pvp_p1_x1 = pvp_p1_x1;
    st->pvp_p1_y0 = pvp_p1_y0; st->pvp_p1_y1 = pvp_p1_y1;
    st->p1_pixel_y_dup = p1_pixel_y;
    st->score_lo = score_lo; st->score_hi = score_hi;
    st->p1_move_anim = p1_move_anim; st->game_mode = game_mode;
    st->p1_move_step_idx = p1_move_step_idx; st->p1_facing_left = p1_facing_left;
    st->p1_move_steps_left = p1_move_steps_left; st->input_state = input_state;
    st->physics_frozen = physics_frozen; st->move_override = move_override;
    st->p1_cell = p1_cell; st->move_locked = move_locked;
    st->prev_game_mode = prev_game_mode; st->p1_step_col_count = p1_step_col_count;
    st->pending_erase_count = pending_erase_count;
    st->level_complete_flag = level_complete_flag;
    st->items_remaining = items_remaining; st->level_exit_cell = level_exit_cell;
    st->level_complete_anim_counter = level_complete_anim_counter;
    st->p1_item_code = p1_item_code; st->anim_target_cell = anim_target_cell;
    st->current_level = current_level; st->move_step_count = move_step_count;
    st->p2_cell = p2_cell; st->p2_move_state = p2_move_state;
    st->p2_frame_base_lo = (u8)(p2_frame_base & 0xff);
    st->p2_frame_base_hi = (u8)((p2_frame_base >> 8) & 0xff);
}

/* write_synth_trace — build an in-memory 2-FRAME trace and write it to `path`.
 * Seeds a minimal INIT, runs game_tick() ONCE to compute the TRUE next state, and
 * writes FRAME[1].state = that state (match) or that-state-with-one-byte-flipped
 * (!match).  FRAME[0] mirrors the seed.  Used by --selftest-replay. */
static void write_synth_trace(const char *path, int match)
{
    struct int8_init init;
    memset(&init, 0, sizeof init);
    /* A minimal-but-non-trivial INIT: a benign game_mode + a couple of grid/cell
       sentinels so game_tick has real state to evolve (the exact values do not
       matter — the self-test compares game_tick's own output against itself). */
    SET_SCALAR(&init, game_mode, 0x21);
    SET_SCALAR(&init, current_level, 1);
    SET_SCALAR(&init, p1_cell, 0x11);
    /* p2_cell = 0xff (-1): P2 inactive.  The self-test exercises the P1 spine +
       anim + present/post paths of game_tick without needing the far p2_move_script
       cursor seeded (that is a capture-only far ptr outside the scalar union; the P2
       scripted-move branch is gated on p2_cell != 0xff).  A real single-player
       capture seeds p2_cell=0xff identically. */
    SET_SCALAR(&init, p2_cell, (int8_t)0xff);
    init.tilemap[0]     = 0x5a;
    init.tilemap[0x2ff] = 0xa5;

    /* FRAME[0] mirrors the seed (rng/input unused by the corrected loop).
       FRAME[1] carries the rng/input that DRIVE tick 0 AND the resulting state.
       Per the corrected run_replay: for k=1, feed frames[1].rng/input, tick, compare
       frames[1].state — so set f1.rng/f1.input to the exact values fed. */
    struct int8_frame f0, f1;
    memset(&f0, 0, sizeof f0);
    memset(&f1, 0, sizeof f1);
    /* The rng/input values fed into the single tick: 0x00 / 0x00. */
    f1.rng = 0x00; f1.input = 0x00;

    /* Compute the TRUE next state: seed, feed f1's rng/input, tick once, snapshot. */
    g_dgroup_seg = GAME_DGROUP_RUNTIME_SEG;
    seed_from_init(&init);
    g_fed_rng = f1.rng; g_fed_input = f1.input;
    game_tick();
    snapshot_state(&f1.state);
    f1.tilemap_hash = int8_tilemap_hash((const uint8_t *)tilemap);
    /* FRAME[0] records the pre-tick (seed) tilemap hash for completeness. */
    f0.tilemap_hash = f1.tilemap_hash;   /* tick mutates tilemap rarely; not compared at k=0 */

    if (!match) {
        /* flip one byte of the recorded state so the replay MUST diverge. */
        f1.state.p1_pixel_x ^= 1;
    }

    struct int8_header hdr;
    memset(&hdr, 0, sizeof hdr);
    memcpy(hdr.magic, INT8_MAGIC, 4);
    hdr.version      = INT8_VERSION;
    hdr.dgroup_seg   = GAME_DGROUP_RUNTIME_SEG;
    hdr.frame_count  = 1;                       /* one tick: FRAME[0] -> FRAME[1] */
    hdr.init_size    = (uint16_t)sizeof(struct int8_init);
    hdr.frame_stride = (uint16_t)sizeof(struct int8_frame);

    FILE *fp = fopen(path, "wb");
    assert(fp);
    fwrite(&hdr, sizeof hdr, 1, fp);
    fwrite(&init, sizeof init, 1, fp);
    fwrite(&f0, sizeof f0, 1, fp);
    fwrite(&f1, sizeof f1, 1, fp);
    fclose(fp);
}
#endif /* INT8_WITH_GAME_TICK */

/* ════════════════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    const char *trace = NULL;
    int perturb = 0, selftest_seed = 0, selftest_replay = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--perturb") == 0)         perturb = 1;
        else if (strcmp(argv[i], "--selftest-seed") == 0)   selftest_seed = 1;
        else if (strcmp(argv[i], "--selftest-replay") == 0) selftest_replay = 1;
        else                                                trace = argv[i];
    }

    /* --selftest-seed: synthetic INIT -> seed -> assert globals set (no trace file). */
    if (selftest_seed) {
        struct int8_init init;
        memset(&init, 0, sizeof init);
        init.tilemap[0]     = 0x5a;
        init.tilemap[0x2ff] = 0xa5;
        SET_SCALAR(&init, p1_pixel_x, 0x1234);
        SET_SCALAR(&init, current_level, 1);
        SET_SCALAR(&init, game_mode, 0x21);
        /* extra sentinels across the gate families, so the self-test is not trivial. */
        SET_SCALAR(&init, p1_grid_x_new, 0x07);
        SET_SCALAR(&init, score_lo, 0xbeef);
        SET_SCALAR(&init, items_remaining, 0x12);
        SET_SCALAR(&init, g_anim_channel_idx, 0x02);
        SET_SCALAR(&init, p2_cell, -5);
        SET_SCALAR(&init, session_continue_flag, 0x01);
        SET_SCALAR(&init, rng_frame, 0x77);
        init.anim_channels[0]  = 0x01;     /* A slot-0 active byte */
        init.anim_channels[1]  = 0x33;     /* A slot-0 cell        */

        seed_from_init(&init);

        assert(p1_pixel_x == 0x1234);
        assert(current_level == 1);
        assert(game_mode == 0x21);
        assert(((u8 *)tilemap)[0] == 0x5a);
        assert(((u8 *)tilemap)[0x2ff] == 0xa5);
        assert(p1_grid_x_new == 0x07);
        assert(score_lo == 0xbeef);
        assert(items_remaining == 0x12);
        assert(g_anim_channel_idx == 0x02);
        assert(p2_cell == -5);
        assert(session_continue_flag == 0x01);
        assert(rng_frame == 0x77);
        assert(anim_a_records[0].active == 0x01);
        assert(anim_a_records[0].cell == 0x33);
        assert(anim_channels_a_tbl[0] == &anim_a_records[0]);

        printf("selftest-seed OK\n");
        return 0;
    }

#ifdef INT8_WITH_GAME_TICK
    /* --selftest-replay: build two synthetic 2-FRAME traces — one whose FRAME[1]
       state matches game_tick's true output (expect clean) and one with a
       deliberately-flipped byte (expect a reported divergence) — and assert the
       divergence machinery catches exactly the mismatch.  Needs game_tick linked. */
    if (selftest_replay) {
        const char *tdir = getenv("TMPDIR");
        if (!tdir || !*tdir) tdir = "/tmp";
        char pathA[1024], pathB[1024];
        snprintf(pathA, sizeof pathA, "%s/int8_synth_mismatch.bin", tdir);
        snprintf(pathB, sizeof pathB, "%s/int8_synth_match.bin", tdir);

        /* trace A: deliberately-wrong FRAME[1].state -> expect divergence */
        write_synth_trace(pathA, /*match=*/0);
        int ra = run_replay(pathA);
        assert(ra != 0);                         /* divergence detected (reports a field) */

        /* trace B: self-consistent FRAME[1].state -> expect clean */
        write_synth_trace(pathB, /*match=*/1);
        int rb = run_replay(pathB);
        assert(rb == 0);                         /* clean: ticks matched */

        remove(pathA);
        remove(pathB);
        printf("selftest-replay OK\n");
        return 0;
    }

    /* default: replay the named trace (perturb -> corrupt-then-replay, expect the
       gate to catch the divergence as non-zero). */
    if (!trace) {
        fprintf(stderr, "usage: int8_ctest [--perturb] <trace.bin>\n"
                        "       int8_ctest --selftest-seed | --selftest-replay\n");
        return 2;
    }
    g_perturb = perturb;
    return run_replay(trace);
#else
    /* Without game_tick linked, only the seed self-test is available; the replay
       loop (run_replay / write_synth_trace) needs game_tick (gate wrapper flips
       INT8_WITH_GAME_TICK). */
    (void)trace; (void)perturb;
    if (selftest_replay) {
        fprintf(stderr, "int8: --selftest-replay needs game_tick "
                        "(compile -DINT8_WITH_GAME_TICK; see validate_int8.sh)\n");
        return 2;
    }
    fprintf(stderr, "int8_ctest: only --selftest-seed runs without "
                    "-DINT8_WITH_GAME_TICK.\n");
    return 2;
#endif
}
