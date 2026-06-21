#ifndef PLAYER2_H
#define PLAYER2_H

#include "bumpy.h"

/* ────────────────────────────────────────────────────────────────────────────
 *  player2.h — Player-2 (AI opponent) module (Phase-4 reconstruction).
 *
 *  SKELETON (Phase-4 Task 2): this header declares the P2 module's globals + the
 *  eleven P2 function prototypes.  The function BODIES are NOT reconstructed yet
 *  — they port across Phase-4 Tasks 3/4/5 (move-state/trajectory, AI rng-decision,
 *  draw/pvp).  Until then the P2 functions remain stubbed in game_stubs.c (no dup:
 *  src/player2.c defines ONLY globals, no bodies) and player2.obj is NOT yet
 *  linked into BUMPY.EXE.  The host replay harness tools/p2_ctest.c #includes
 *  src/player2.c for the globals and validates each ported body as it lands.
 *
 *  P2 is AI-controlled / autonomous (not key-driven): an rng-decision layer
 *  (p2_ai_dispatch_move + select_move_a/b/random keyed on rng_frame) chooses a
 *  move-state, p2_set_move_state loads the per-state move-script, and
 *  p2_step_scripted_move advances the P2 pixel position along it.
 *
 *  Provenance for every address: tools/p2_oracle.py header + local/build/p2_model.md
 *  §"Resolved P2 DGROUP addresses" (Ghidra DGROUP 0x203b offsets, read live via the
 *  Ghidra MCP from the disassembly operands of the P2 functions, cross-checked
 *  against the Phase-0/6b P2 render globals).
 *
 *  ── OWNERSHIP (the careful part; see src/player2.c for the per-symbol calls) ───
 *    OWNED BY player2.c (defined there — genuinely new; no other TU owns them):
 *      p2_pixel_x          0x79ba  s16   (referenced ONLY in entity.c comments and
 *      p2_pixel_y          0x79bc  s16    passed to entity_draw_p2 by VALUE — never
 *      p2_move_anim        0x8560  u16    defined as a DGROUP symbol anywhere yet)
 *      p2_cell             0x8571  s8     (entity.h/level.h doc it; never defined)
 *      p2_frame_base       0xa0de  u16
 *      p2_move_steps_left  0xa1b0  u8
 *      p2_step_idx         0x8563  u8
 *      p2_facing_neg_dx    0x9d2f  u8
 *      p2_move_toggle      0x8243  u8
 *      p2_grid_col         0xa0ca  s16
 *      p2_grid_row         0xa0cc  s16
 *      p2_set_cell_col     0x8564  u8
 *      p2_set_cell_row     0x8565  u8
 *      p2_ai_threshold     0x7920  u8
 *      p2_dir_blocked_0    0xa0e0  u8
 *      p2_dir_blocked_1    0xa0e1  u8
 *      p2_dir_blocked_3    0xa1b2  u8
 *      pvp_collision_flag  0xa1aa  u8
 *      pvp_p1_bbox[4]      0x084c..0x0852 s16   (x0,x1,y0,y1)
 *      pvp_p2_bbox[4]      0x0854..0x085a s16   (x0,x1,y0,y1)
 *
 *    OWNED ELSEWHERE (declared extern here, defined in their owning module — the
 *    P2 functions read/write them but must NOT redefine them, to avoid a future
 *    dup-symbol when player2.obj is linked in T3):
 *      p2_move_state       0x8562  u8   — game.c (defined game.c:60, passed to
 *                                          p2_set_move_state in game_loop)
 *      rng_frame           0x79b3  u8   — player.c (defined player.c:220; the AI
 *                                          rng-decision input)
 *      game_mode           0x792c  u8   — game.c / player.c spine
 *      physics_frozen      0xa0ce  u8   — player.c (read in check_pvp_collision)
 *      current_level       0x79b2  u8   — level.c
 * ──────────────────────────────────────────────────────────────────────────── */

/* ── P2-render globals — OWNED BY player2.c (defined there) ──────────────────── */
extern s16 p2_pixel_x;        /* DGROUP 0x79ba — P2 sprite pixel X                */
extern s16 p2_pixel_y;        /* DGROUP 0x79bc — P2 sprite pixel Y                */
extern u16 p2_move_anim;      /* DGROUP 0x8560 — P2 anim offset (frame = base+anim)*/
extern s8  p2_cell;           /* DGROUP 0x8571 — P2 grid cell; -1 (0xff) = absent */
extern u16 p2_frame_base;     /* DGROUP 0xa0de — P2 frame-table base index        */

/* ── P2 move-state / trajectory globals — OWNED BY player2.c ─────────────────── */
extern u8  p2_move_steps_left;/* DGROUP 0xa1b0 — steps remaining in current script*/
extern u8  p2_step_idx;       /* DGROUP 0x8563 — script entry index (==5 gate)    */
extern u8  p2_facing_neg_dx;  /* DGROUP 0x9d2f — facing flag (negate script dx)   */
extern u8  p2_move_toggle;    /* DGROUP 0x8243 — XOR-1 half-rate move gate        */
extern s16 p2_grid_col;       /* DGROUP 0xa0ca — P2 grid column (clamp 0..0x12)   */
extern s16 p2_grid_row;       /* DGROUP 0xa0cc — P2 grid row (clamp 0..0x16)      */
extern u8  p2_set_cell_col;   /* DGROUP 0x8564 — cell->pixel column scratch       */
extern u8  p2_set_cell_row;   /* DGROUP 0x8565 — cell->pixel row scratch          */
extern u16 __far *p2_move_script;  /* DGROUP 0xa0ba/0xa0bc — [anim,dx,dy] script ptr */
extern u8  __far *p2_sprite;       /* DGROUP 0x9b9e/0x9ba0 — P2 sprite obj ptr     */
extern u8  __far *p2_state_script_tbl;/* DGROUP 0x2520/0x2522 — per-state script tbl*/
extern u8  __far *p2_cell_coord_tbl;/* DGROUP 0x0274 — posC cell->pixel coord table */
extern void (__far * __far *p2_state_handler_tbl)(void);/* DGROUP 0x085c — per-state handler tbl */
extern s16 p2_grid_x;         /* DGROUP 0x8558 — current grid col (history)       */
extern s16 p2_grid_y;         /* DGROUP 0x855a — current grid row (history)       */
extern s16 p2_grid_x_prev;    /* DGROUP 0x928e — previous grid col (history)      */
extern s16 p2_grid_y_prev;    /* DGROUP 0x9b94 — previous grid row (history)      */

/* ── P2 render/view globals — OWNED BY player2.c (Phase-4 T5) ────────────────── */
extern s16 p2_scroll_x;       /* DGROUP 0x9d34 — P2 view scroll X                 */
extern s16 p2_scroll_y;       /* DGROUP 0x9d32 — P2 view scroll Y                 */
extern u8 __far *p2_view;       /* DGROUP 0x8ec/0x8ee — render_player_view desc    */
extern u8 __far *p2_erase_view; /* DGROUP 0x8e8/0x8ea — restore_bg_view desc       */

/* ── AI rng-decision globals — OWNED BY player2.c ────────────────────────────── */
extern u8  p2_ai_threshold;   /* DGROUP 0x7920 — rng_frame branch threshold       */
extern u8  p2_dir_blocked_0;  /* DGROUP 0xa0e0 — AI dispatch dir-blocked flag 0   */
extern u8  p2_dir_blocked_1;  /* DGROUP 0xa0e1 — AI dispatch dir-blocked flag 1   */
extern u8  p2_dir_blocked_2;  /* DGROUP 0xa0e2 — dir-blocked flag 2 (tile check)  */
extern u8  p2_dir_blocked_3;  /* DGROUP 0xa1b2 — AI dispatch dir-blocked flag 3   */

/* ── P1/P2 pvp-collision globals — OWNED BY player2.c ────────────────────────── */
extern u8  pvp_collision_flag;/* DGROUP 0xa1aa — set 0/1 by check_pvp_collision   */
extern s16 pvp_p1_x0;         /* DGROUP 0x084c — P1 AABB x0                        */
extern s16 pvp_p1_x1;         /* DGROUP 0x084e — P1 AABB x1                        */
extern s16 pvp_p1_y0;         /* DGROUP 0x0850 — P1 AABB y0                        */
extern s16 pvp_p1_y1;         /* DGROUP 0x0852 — P1 AABB y1                        */
extern s16 pvp_p2_x0;         /* DGROUP 0x0854 — P2 AABB x0                        */
extern s16 pvp_p2_x1;         /* DGROUP 0x0856 — P2 AABB x1                        */
extern s16 pvp_p2_y0;         /* DGROUP 0x0858 — P2 AABB y0                        */
extern s16 pvp_p2_y1;         /* DGROUP 0x085a — P2 AABB y1                        */

/* ── globals OWNED ELSEWHERE (extern — must NOT be redefined in player2.c) ───── */
extern u8  p2_move_state;     /* game.c   0x8562 — P2 move-state                  */
/* rng_frame, game_mode, current_level, physics_frozen are declared extern by the
   modules that own them (player.h/game.h/level.h); player2.c includes those. */

/* ════════════════════════════════════════════════════════════════════════════
 *  P2 functions — DECLARED here, BODIES reconstructed in Phase-4 T3/T4/T5.
 *  (Until ported they remain stubbed in game_stubs.c; player2.c defines no bodies.)
 *  Engine seg-1000 offsets are noted for the harness's per-fn registry.
 * ════════════════════════════════════════════════════════════════════════════ */
void p2_set_move_state(u8 state);     /* 1000:4bc6 — load script/steps/facing      */
u8   p2_step_scripted_move(void);     /* 1000:4c14 — advance P2 pixel along script */
void p2_update_grid_cell(void);       /* 1000:4b4e — P2 pixel -> grid col/row      */
void p2_tile_move_check(void);        /* 1000:4c99 — tile-move/collision check     */
void p2_set_pixel_from_cell(void);    /* 1000:48a9 — p2_cell -> P2 pixel x/y       */
void p2_advance_grid_history(void);   /* 1000:13b2 — slide P2 grid-cell history    */
void p2_ai_dispatch_move(void);       /* 1000:4f4e — AI dispatch on dir-blocked    */
void p2_ai_select_move_a(void);       /* 1000:4f04 — AI rng-branch -> state 1/2/3  */
void p2_ai_select_move_b(void);       /* 1000:4f89 — AI rng-branch -> state 1/2/4  */
void p2_ai_select_move_random(void);  /* 1000:4fd3 — state=(rng&3)+(rand()&1)+5    */
void p2_choose_move_state1(void);     /* 1000:4dfa — pick move favouring state 1   */
void p2_choose_move_state2(void);     /* 1000:4e7f — pick move favouring state 2   */
void p2_pick_move_priority_a(void);   /* 1000:4dbf — dispatch (a0e0,a1b2,a0e2,else) */
void p2_pick_move_priority_b(void);   /* 1000:4e44 — dispatch (a0e1,a0e2,a1b2,else) */
void p2_pick_move_priority_c(void);   /* 1000:4ec9 — dispatch (a0e2,a0e0,a0e1,else) */
void p2_run_move_state_handler(void); /* 1000:5003 — dispatch move_state -> handler*/
void p2_cell_move_up(void);           /* 1000:5025 — p2_cell -= 8 (row up)         */
void p2_cell_move_down(void);         /* 1000:503f — p2_cell += 8 (row down)       */
void p2_cell_move_left(void);         /* 1000:5059 — p2_cell -= 1 (col left)       */
void p2_cell_move_right(void);        /* 1000:506f — p2_cell += 1 (col right)      */
void check_pvp_collision(void);       /* 1000:50fb — P1/P2 AABB overlap -> flag    */
void draw_p2_sprite(void);            /* 1000:1cea — build P2 object descriptor    */
void render_p2_view(void);            /* 1000:1c41 — P2 save-under view present     */
void erase_p2_view(void);             /* 1000:19a1 — P2 prev-cell bg restore        */
void update_p2_bbox(void);            /* 1000:50c0 — P2 AABB from pixel pos         */
void update_p1_bbox(void);            /* 1000:5085 — P1 AABB (pvp P1 bbox words; T3) */

#endif /* PLAYER2_H */
