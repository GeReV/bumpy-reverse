#ifndef ITEMS_H
#define ITEMS_H

#include "bumpy.h"

/* ────────────────────────────────────────────────────────────────────────────
 *  items.h — item-collection & level-exit module (Phase-3 reconstruction).
 *
 *  Phase-3 Task 3 ports the item-collection + scoring trio 1:1 from the live
 *  Ghidra decomp: p1_collect_item (1000:6c14), p1_collect_item_score
 *  (1000:6c95), move_step_read_item (1000:6627) — plus the tiny layer-C read
 *  leaf read_tile_layer2 (1000:6bf4) that move_step_read_item calls (the +0x60
 *  sibling of player.c's read_tile_layer_contact/read_tile_at_cell; it writes
 *  this module's p1_item_code, so it lives here).  check_exit_tile_vert
 *  (1000:6372) and teleport_to_next_exit_tile (1000:25ad) are Phase-3 Task 4 and
 *  remain stubbed (game_stubs.c).
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
 *      sound_device_state     0x689c           s16  (player.c) — ==4 selects OPL ids
 *      tilemap (off/seg)      0xa0d8           far  (game.c) — item byte at [cell+0x60]
 * ──────────────────────────────────────────────────────────────────────────── */

/* ── item/exit module-owned globals (defined in src/items.c) ─────────────────── */
extern u8  items_remaining;             /* DGROUP 0xa0cf — items left on the level */
extern u8  level_exit_cell;             /* DGROUP 0x8572 — cell of the level exit  */
extern u8  level_complete_anim_counter; /* DGROUP 0x8550 — set 0xf2 on completion  */
extern u8  p1_item_code;                /* DGROUP 0x79b8 — latched layer-C item byte*/

/* Two further item-collection DGROUP bytes no other module owns (NOT part of the
   semantic-state SNAP / replay gate — see items.c for their use & addresses): */
extern u8  sharp_item_counter;          /* DGROUP 0x791a — '#'-item pickup counter  */
extern u8  collect_mode_2810;           /* DGROUP 0x2810 — p1_collect_item entry tag */

/* ── the item/exit functions (T3 ported; check_exit_tile_vert is T4-stubbed) ──── */
void p1_collect_item(void);             /* 1000:6c14 — collect + score + complete (T3)  */
void p1_collect_item_score(void);       /* 1000:6c95 — award score for an item code (T3) */
void move_step_read_item(void);         /* 1000:6627 — read layer-C item, maybe coll (T3)*/
void read_tile_layer2(u8 cell);         /* 1000:6bf4 — p1_item_code = tilemap[cell+0x60] (T3) */
void check_exit_tile_vert(void);        /* 1000:6372 — vertical exit-tile detection (T4) */
/* teleport_to_next_exit_tile (1000:25ad) is declared in player.h (dispatch idx 0x0e)
   and currently stubbed in game_stubs.c; not re-declared here to avoid duplication. */

#endif /* ITEMS_H */
