#ifndef ITEMS_H
#define ITEMS_H

#include "bumpy.h"

/* ────────────────────────────────────────────────────────────────────────────
 *  items.h — item-collection & level-exit module (Phase-3 reconstruction).
 *
 *  SKELETON (Phase-3 Task 2): this header declares the item/exit game-state
 *  globals and the five hooked item/exit functions.  The FUNCTION BODIES are
 *  NOT reconstructed yet — Phase-3 Tasks 3/4 port them.  Until then the five
 *  functions remain stubbed (teleport_to_next_exit_tile in game_stubs.c; the
 *  four collect/exit functions are referenced only by the host replay harness
 *  tools/items_ctest.c, which marks them UNPORTED).  This header + src/items.c
 *  exist so that harness compiles against the real module surface now.
 *
 *  SEMANTIC-STATE GLOBALS (resolved DGROUP 0x203b offsets — see
 *  local/build/items_model.md §"Resolved semantic-state DGROUP addresses"):
 *
 *    OWNED BY items.c (defined there — no other TU owns them):
 *      items_remaining             0xa0cf  u8
 *      level_exit_cell             0x8572  u8
 *      level_complete_anim_counter 0x8550  u8
 *      p1_item_code                0x79b8  u8
 *
 *    OWNED ELSEWHERE (declared extern here, defined in their owning module —
 *    the item/exit functions read/write them but must NOT redefine them):
 *      score_lo / score_hi    0xa0d4 / 0xa0d6  u16  (game.c)
 *      current_level          0x79b2           u8   (level.c)
 *      level_complete_flag    0xa1b1           u8   (player.c)
 *      anim_target_cell       0x856f           u8   (player.c)
 *      p1_cell                0x856e           u8   (player.c)
 *      move_step_count        0x855e           u8   (player.c)
 *      physics_frozen         0xa0ce           u8   (player.c)
 *      p1_pixel_y             0x9292           s16  (player.c)
 * ──────────────────────────────────────────────────────────────────────────── */

/* ── item/exit module-owned globals (defined in src/items.c) ─────────────────── */
extern u8  items_remaining;             /* DGROUP 0xa0cf — items left on the level */
extern u8  level_exit_cell;             /* DGROUP 0x8572 — cell of the level exit  */
extern u8  level_complete_anim_counter; /* DGROUP 0x8550 — set 0xf2 on completion  */
extern u8  p1_item_code;                /* DGROUP 0x79b8 — latched layer-C item byte*/

/* ── the five hooked item/exit functions (bodies: Phase-3 T3/T4) ─────────────── */
void p1_collect_item(void);             /* 1000:6c14 — collect + score + complete  */
void p1_collect_item_score(void);       /* 1000:6c95 — award score for an item code */
void check_exit_tile_vert(void);        /* 1000:6372 — vertical exit-tile detection */
void move_step_read_item(void);         /* 1000:6627 — read layer-C item, maybe coll*/
/* teleport_to_next_exit_tile (1000:25ad) is declared in player.h (dispatch idx 0x0e)
   and currently stubbed in game_stubs.c; not re-declared here to avoid duplication. */

#endif /* ITEMS_H */
