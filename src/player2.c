/*
 * player2.c — Player-2 (AI opponent) module  [Phase-4 reconstruction]
 *
 * SKELETON (Phase-4 Task 2).  This translation unit defines ONLY the Player-2
 * module's globals; the eleven P2 functions (declared in player2.h) are NOT
 * reconstructed here yet — their bodies port across Phase-4 Tasks 3/4/5 and, until
 * then, remain the faithful stubs in game_stubs.c.  Keeping the bodies out of this
 * TU means there is NO duplicate symbol with game_stubs.c, and player2.obj is NOT
 * yet linked into BUMPY.EXE (the Makefile's BUMPY_OBJS is unchanged), so the link
 * set is unaffected — only these (currently unreferenced) globals are introduced.
 *
 * The host replay harness tools/p2_ctest.c #includes THIS file to obtain the P2
 * globals, seeds them from the Phase-4 T1 capture (local/build/render/p2_trace.bin),
 * and validates each P2 function body against the capture as Tasks 3/4/5 land it.
 *
 * Provenance for every address: tools/p2_oracle.py header + local/build/p2_model.md
 * §"Resolved P2 DGROUP addresses" (Ghidra DGROUP 0x203b offsets).
 *
 * ── OWNERSHIP CALLS (one per symbol; the careful part) ────────────────────────
 *   The P2-render globals (p2_pixel_x/y, p2_move_anim, p2_cell, p2_frame_base) are
 *   the ones entity.c touches.  Grep of the src tree (.c and .h) shows entity.c
 *   references them
 *   ONLY in comments (the entity_draw_p2 decomp narrative) and the actual
 *   entity_draw_p2() takes them as VALUE PARAMETERS (u16 pixel_x, ..., s8 p2_cell);
 *   level.c likewise passes -1 / 0 to entity_draw_p2 by value (no symbol).  NO TU
 *   currently DEFINES any of them as a DGROUP symbol.  => player2.c OWNS them
 *   (defines them here).  This is the correct owner for when player2.obj links in
 *   T3: entity.obj does not reference the symbol, so there is no link dependency to
 *   break, and defining them here gives the P2 functions their backing storage.
 *
 *   p2_move_state (0x8562) is ALREADY DEFINED by game.c (game.c:60 — game_loop
 *   passes it to p2_set_move_state).  => extern here (do NOT redefine — that would
 *   dup-symbol against game.obj when player2.obj links in T3).
 *
 *   rng_frame (0x79b3) is ALREADY DEFINED by player.c (player.c:220).  => extern via
 *   player.h.  game_mode / current_level / physics_frozen are owned by game.c /
 *   level.c / player.c respectively. => extern via their headers.
 *
 *   The remaining P2 move-state / AI / trajectory / pvp globals are genuinely new
 *   (no other TU names them) => player2.c OWNS them.
 * ──────────────────────────────────────────────────────────────────────────── */

#include "bumpy.h"
#include "player2.h"

/* ── P2-render globals — OWNED here (see ownership note above) ────────────────── */
s16 p2_pixel_x;          /* DGROUP 0x79ba */
s16 p2_pixel_y;          /* DGROUP 0x79bc */
u16 p2_move_anim;        /* DGROUP 0x8560 */
s8  p2_cell = (s8)0xff;  /* DGROUP 0x8571 — -1 (0xff) sentinel = P2 absent        */
u16 p2_frame_base;       /* DGROUP 0xa0de */

/* ── P2 move-state / trajectory globals — OWNED here ─────────────────────────── */
u8  p2_move_steps_left;  /* DGROUP 0xa1b0 */
u8  p2_step_idx;         /* DGROUP 0x8563 */
u8  p2_facing_neg_dx;    /* DGROUP 0x9d2f */
u8  p2_move_toggle;      /* DGROUP 0x8243 */
s16 p2_grid_col;         /* DGROUP 0xa0ca */
s16 p2_grid_row;         /* DGROUP 0xa0cc */
u8  p2_set_cell_col;     /* DGROUP 0x8564 */
u8  p2_set_cell_row;     /* DGROUP 0x8565 */

/* ── AI rng-decision globals — OWNED here ────────────────────────────────────── */
u8  p2_ai_threshold;     /* DGROUP 0x7920 */
u8  p2_dir_blocked_0;    /* DGROUP 0xa0e0 */
u8  p2_dir_blocked_1;    /* DGROUP 0xa0e1 */
u8  p2_dir_blocked_3;    /* DGROUP 0xa1b2 */

/* ── P1/P2 pvp-collision globals — OWNED here ─────────────────────────────────── */
u8  pvp_collision_flag;  /* DGROUP 0xa1aa */
s16 pvp_p1_x0;           /* DGROUP 0x084c */
s16 pvp_p1_x1;           /* DGROUP 0x084e */
s16 pvp_p1_y0;           /* DGROUP 0x0850 */
s16 pvp_p1_y1;           /* DGROUP 0x0852 */
s16 pvp_p2_x0;           /* DGROUP 0x0854 */
s16 pvp_p2_x1;           /* DGROUP 0x0856 */
s16 pvp_p2_y0;           /* DGROUP 0x0858 */
s16 pvp_p2_y1;           /* DGROUP 0x085a */

/* ────────────────────────────────────────────────────────────────────────────
 *  P2 function bodies: NOT reconstructed in this skeleton task.
 *
 *  Phase-4 Task 3 ports the move-state/trajectory group (p2_set_move_state,
 *  p2_step_scripted_move, p2_update_grid_cell, p2_set_pixel_from_cell,
 *  p2_run_move_state_handler); Task 4 ports the AI rng-decision group
 *  (p2_ai_dispatch_move, p2_ai_select_move_a/b/random — note select_move_random
 *  calls rand()/prng_step, so its determinism is seeded from the prng state, see
 *  tools/p2_ctest.c §AI-DETERMINISM); Task 5 ports draw_p2_sprite + the pvp
 *  collision (check_pvp_collision).  Each body, when landed here, is un-stubbed
 *  from game_stubs.c and its tools/p2_ctest.c PORTED-registry NULL is swapped for
 *  the function's C name.
 * ──────────────────────────────────────────────────────────────────────────── */
