/* Host int8-synced END-TO-END replay harness — Phase-9.x Task 3 (PART 1 of 2).
 *
 * PART 1 (this task): INIT parse + seed-once.  Defines the canonical INIT/FRAME
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
void play_walk_anim_default(void) {}
void step_walk_anim(u8 a, u8 p, u16 fo, u16 fs) { (void)a;(void)p;(void)fo;(void)fs; }
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
void move_walk_right_anim_step(void) {}
void enter_mode_0b_jump_start(void) {}
void move_anim_step_to_mode0c(void) {}
void move_step_check_walkable(void) {}
void move_step_dispatch_input(void) {}
void p1_input_dispatch_bit10(void) {}
void FUN_1000_4437(void) {}
void advance_physics_freeze(void) {}
void FUN_1000_1e3d(void) {}
void p2_dispatch_move_state_handler(void) {}
u8   pvp_collision_flag;
u8   mode_script_tbl[0x40 * 4];
void setup_fullscreen_view(void) {}

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
#include "int8_extracted.h"   /* game_tick + game.c/level.c helpers (gate script) */
#endif

/* p1_set_pixel_from_cell (items.c calls it; reconstructed in player.c in the real
   build, but the host player.c include does not provide it — defined here AFTER the
   TUs so p1_cell/move_step_count/p1_pixel_* resolve to the included globals).
   Faithful host model (mirrors items_ctest.c): derives the cell coords analytically. */
void p1_set_pixel_from_cell(void)
{
    u8 row = (u8)(p1_cell >> 3);
    u8 col = (u8)(p1_cell - (u8)(row * 8));
    move_step_count = col;
    p1_pixel_x = (s16)((u16)col * 40u + 8u) + 7;
    p1_pixel_y = (s16)((u16)row * 32u + 8u) + 0xf;
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
    /* entity_state[] is reserved for the spawn/entity arrays (populated when the
       replay loop needs them); no live target yet, so it is carried but not unpacked. */

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

    /* ── [sess] session / level / frame-boundary control ── */
    round_continue_flag   = s->round_continue_flag;
    session_continue_flag = s->session_continue_flag;
    frame_abort_flag      = s->frame_abort_flag;
    settle_countdown      = s->settle_countdown;
    rng_frame             = s->rng_frame;
}

/* ════════════════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    int selftest_seed = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest-seed") == 0) selftest_seed = 1;
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

    fprintf(stderr,
        "int8_ctest (part 1): only --selftest-seed is implemented; the per-tick "
        "replay loop lands in the next task.\n");
    return 2;
}
