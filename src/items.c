/* ────────────────────────────────────────────────────────────────────────────
 *  items.c — item-collection & level-exit module (Phase-3 reconstruction).
 *
 *  SKELETON (Phase-3 Task 2).  This translation unit currently defines ONLY the
 *  item/exit module's game-state globals (the four DGROUP bytes no other module
 *  owns).  The five hooked item/exit functions declared in items.h
 *  (p1_collect_item, p1_collect_item_score, check_exit_tile_vert,
 *  move_step_read_item, teleport_to_next_exit_tile) are NOT reconstructed yet —
 *  Phase-3 Tasks 3/4 port them.  They remain stubbed for the BUMPY.EXE link
 *  (teleport_to_next_exit_tile in game_stubs.c) and are exercised only by the
 *  host replay harness tools/items_ctest.c (which marks them UNPORTED).
 *
 *  Defining no bodies here keeps the link free of duplicate symbols: this TU
 *  contributes only the four globals below.  The remaining semantic-state
 *  globals the item/exit functions touch are owned by game.c (score_lo/hi),
 *  level.c (current_level), and player.c (level_complete_flag, anim_target_cell,
 *  p1_cell, move_step_count, physics_frozen, p1_pixel_y) — declared extern via
 *  items.h, never redefined here.
 *
 *  Source of truth: Ghidra BumpyDecomp + tools/items_oracle.py +
 *  local/build/items_model.md (the Phase-3 T1 semantic-state capture).
 * ──────────────────────────────────────────────────────────────────────────── */
#include "items.h"

/* ── item/exit module-owned globals (DGROUP 0x203b offsets) ──────────────────── */
u8  items_remaining;             /* DGROUP 0xa0cf — items left on the current level */
u8  level_exit_cell;             /* DGROUP 0x8572 — grid cell of the level exit     */
u8  level_complete_anim_counter; /* DGROUP 0x8550 — set to 0xf2 when last item taken*/
u8  p1_item_code;                /* DGROUP 0x79b8 — latched layer-C item byte        */

/* The five item/exit function bodies are reconstructed in Phase-3 Tasks 3/4. */
