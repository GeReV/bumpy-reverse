#ifndef SPAWN_H
#define SPAWN_H

#include "bumpy.h"

/* ────────────────────────────────────────────────────────────────────────────
 *  spawn.h — level-load entity-placement orchestrator (Phase-8 reconstruction).
 *
 *  RECONSTRUCTED (Phase-8 T2): declares the ownership of the few DGROUP globals
 *  that spawn_and_draw_level_entities (1000:2a78) DEFINES (no other TU owns them)
 *  and re-states (as documentation) the prototypes of the engine leaves it calls.
 *  The 1:1 body lives in src/spawn.c; game_stubs.c no longer stubs it (no dup).
 *
 *  spawn_and_draw_level_entities is the level-load entity-placement orchestrator:
 *  it resets + activates the channel-A (3-slot) and channel-B (4-slot) animation
 *  records, reads the BUM header spawn fields (P1/P2 cell + state) via tilemap /
 *  level_src_ptr, runs setup_fullscreen_view(), then scans the 6x8 grid across the
 *  three runtime tilemap layers — A (+0x00), B (+0x30), C (+0x60) — populating an
 *  anim-channel record per A/B cell (calling draw/erase_anim_channels_a/b) and
 *  blitting a static sprite per C cell, before deactivating the records.
 *
 *  ── OWNERSHIP (grep-verified — see src/spawn.c per-symbol block) ───────────────
 *    OWNED BY spawn.c (DEFINED there; genuinely new — no other TU owns them):
 *      spawn_a_type_tbl      0x3d3a  tilemap-byte(+0x00) -> entity type (layer A)
 *      spawn_b_type_tbl      0x4086  tilemap-byte(+0x30) -> entity type (layer B)
 *      spawn_p2_frame_tbl    0x2546  header[+0x96]*2 -> p2_frame_base (u16 tbl)
 *      g_anim_a_active_mirror 0x8e8b u8 — secondary A active flag the orchestrator
 *                                         stamps alongside record-A's active byte
 *      g_anim_b_active_mirror 0x8e8c u8 — secondary B active flag (record-B side)
 *
 *    OWNED ELSEWHERE (extern — spawn.c reads/writes but must NOT redefine them):
 *      anim_channels_a_tbl   0x4c70/0x4c72  far-ptr slot table (3 slots)  — anim.c
 *      anim_channels_b_tbl   0x4cbc/0x4cbe  far-ptr slot table (4 slots)  — anim.c
 *      anim_a_frame_tbl      0x3d6a/0x3d6c  type*4 -> far-ptr descriptor  — anim.c
 *      anim_b_frame_tbl      0x40a6/0x40a8  type*4 -> far-ptr descriptor  — anim.c
 *      g_anim_cur_cmd_byte   0x8578  u8  step-A cur-cmd byte             — anim.c
 *      anim_b_cur_frame_byte 0x8579  u8  step-B cur-frame byte           — anim.c
 *      p1_sprite             0x8884  far blit-descriptor ptr             — anim.c
 *      tilemap               0xa0d8  far  base tilemap layer             — game.c
 *      level_src_ptr         0x75d0  far  level-archive source cursor    — game.c
 *      p2_move_state         0x8562  u8                                  — game.c
 *      p1_cell               0x856e  u8                                  — player.c
 *      level_exit_cell       0x8572  u8                                  — items.c
 *      items_remaining       0xa0cf  u8                                  — items.c
 *      p2_cell               0x8571  s8                                  — player2.c
 *      p2_ai_threshold       0x7920  u8                                  — player2.c
 *      p2_frame_base         0xa0de  u16                                 — player2.c
 *      p2_cell_coord_tbl     0x0274  far  layer-C pos table (X@+0,Y@+2)   — player2.c
 * ──────────────────────────────────────────────────────────────────────────── */

/* ── per-cell spawn type-remap tables — OWNED BY spawn.c ──────────────────────────
   Indexed by the runtime tilemap byte; -> entity type (the index into the A/B
   far-ptr frame tables).  256-byte tables (full 0..0xff range). */
#define SPAWN_TYPE_TBL_LEN  0x100
extern u8 spawn_a_type_tbl[SPAWN_TYPE_TBL_LEN];  /* DGROUP 0x3d3a — layer-A type remap */
extern u8 spawn_b_type_tbl[SPAWN_TYPE_TBL_LEN];  /* DGROUP 0x4086 — layer-B type remap */

/* ── P2 frame-base lookup — OWNED BY spawn.c ─────────────────────────────────────
   header[+0x96]*2 indexes this near u16 table; the result becomes p2_frame_base.
   0x20 entries (BUM header field 0x96 is a small selector). */
#define SPAWN_P2_FRAME_TBL_LEN  0x20
extern u16 spawn_p2_frame_tbl[SPAWN_P2_FRAME_TBL_LEN]; /* DGROUP 0x2546 — p2_frame_base tbl */

/* ── secondary active-flag mirrors — OWNED BY spawn.c ─────────────────────────────
   The orchestrator stamps these alongside the channel records' own active byte
   (DAT_203b_8e8b / DAT_203b_8e8c in the decomp). */
extern u8 g_anim_a_active_mirror;   /* DGROUP 0x8e8b */
extern u8 g_anim_b_active_mirror;   /* DGROUP 0x8e8c */

/* ── the orchestrator — BODY in src/spawn.c (also declared in game.h) ──────────── */
void spawn_and_draw_level_entities(void); /* 1000:2a78 */

#endif /* SPAWN_H */
