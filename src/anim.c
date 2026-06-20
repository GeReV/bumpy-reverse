/* ────────────────────────────────────────────────────────────────────────────
 *  anim.c — animation-channel FX module (Phase-5 reconstruction).
 *
 *  SKELETON (Phase-5 Task 2): this TU defines ONLY the anim-channel module's
 *  globals — NO function bodies.  The seven anim functions (apply_cell_animation
 *  1000:69aa, step/draw/erase_anim_channels_a/b) remain stubbed in game_stubs.c
 *  this task; their bodies port in Phase-5 T3 (allocator + steppers, validated on
 *  semantic state) and T4 (draw + erase, validated at descriptor level), at which
 *  point each is un-stubbed from game_stubs.c and reconstructed 1:1 here.  Because
 *  this TU contributes no function bodies, anim.obj links cleanly alongside the
 *  game_stubs.c bodies with ZERO duplicate symbols (the same pattern Phase-4 T2's
 *  player2.obj / Phase-3 T2's items.obj used for their globals-only skeletons).
 *
 *  ── OWNERSHIP / no-duplicate-symbols (grep-verified across the src tree) ───────
 *    DEFINED HERE (genuinely new — no other TU owns a symbol of this name):
 *      anim_channels_a_tbl / anim_channels_b_tbl  (the two far-ptr slot tables)
 *      anim_a_records / anim_b_records            (the 3 A + 4 B 12-byte records)
 *      anim_a_tiledef_tbl / anim_a_frame_tbl / anim_b_frame_tbl
 *      anim_a_grid_tbl / anim_b_grid_tbl / anim_posA_tbl / anim_posB_tbl
 *      the seven view descriptors (anim_a_*_view, anim_b_*_view/view0/view1)
 *      p1_sprite (the 0x8884 blit-descriptor far ptr — there is NO `p1_sprite`
 *        variable elsewhere; entity.c references "p1_sprite" only in comments and
 *        accesses that struct via the DG_P1_OBJ 0x792e offset, not a named global)
 *      the step-state scalars / working ptrs (g_anim_stream_ptr,
 *        g_anim_cur_cmd_byte, anim_b_loop_idx, anim_b_stream_ptr,
 *        anim_b_cur_frame_byte)
 *
 *    EXTERN (owned elsewhere — declared in anim.h, defined in the owning module;
 *    grep evidence beside each):
 *      anim_target_cell    — player.c:230  `u8 anim_target_cell;`        (0x856f)
 *      g_anim_channel_idx  — player.c:1664 `u8 g_anim_channel_idx;`      (0x856c)
 *      tilemap             — game.c:79     `u8 __far *tilemap;`          (0xa0d8)
 *      current_level       — level.c:106   `u8 current_level = 1u;`      (0x79b2)
 *
 *  STACK-CHECK PROLOGUE: every original anim fn opens with Turbo C's compiler-
 *  emitted stack-overflow probe (`if (stack_check_limit <= &stack0xfffe) …`); it is
 *  NOT game logic and is intentionally OMITTED from the future ports (the same
 *  convention player.c / items.c / player2.c document).
 *
 *  Source of truth: Ghidra BumpyDecomp + raw disassembly + tools/anim_oracle.py +
 *  local/build/anim_model.md (the Phase-5 T1 anim-channel capture).
 * ──────────────────────────────────────────────────────────────────────────── */
#include "anim.h"

/* ── the channel records (one fixed 12-byte record per slot) ─────────────────── */
anim_chan_rec anim_a_records[ANIM_A_SLOTS];   /* 3 channel-A slots */
anim_chan_rec anim_b_records[ANIM_B_SLOTS];   /* 4 channel-B slots */

/* ── slot tables (far ptrs into the records above) ──────────────────────────────
   DGROUP 0x4c70/0x4c72 (A, 3 slots) and 0x4cbc/0x4cbe (B, 4 slots).  In the engine
   these are populated by the allocator / spawn path; here they default to the
   module's own record array so the steppers/draw/erase fns (T3/T4) and the host
   harness see a consistent table-of-records.  Static initialisers cannot take the
   address of an array element portably across the 16-bit far model for every
   compiler, so they are left zero-initialised here and wired by the harness / the
   engine spawn path; the records themselves are the owned storage. */
anim_chan_rec __far *anim_channels_a_tbl[ANIM_A_SLOTS];
anim_chan_rec __far *anim_channels_b_tbl[ANIM_B_SLOTS];

/* ── per-action / per-frame far-ptr tables ─────────────────────────────────────
   These index action/cmd/frame * 4 into engine tile-def / frame-data tables.  As
   plain DGROUP-resident far pointers they are level/engine data populated at load
   time; defined here as the module's owned storage (zero until populated). */
u8 __far *anim_a_tiledef_tbl;   /* DGROUP 0x2ede/0x2ee0 */
u8 __far *anim_a_frame_tbl;     /* DGROUP 0x3d6a/0x3d6c */
u8 __far *anim_b_frame_tbl;     /* DGROUP 0x40a6/0x40a8 */

/* ── grid-coord / pos tables (draw/erase A/B) ──────────────────────────────────*/
u8 __far *anim_a_grid_tbl;      /* DGROUP 0x32be/0x32c0 */
u8 __far *anim_b_grid_tbl;      /* DGROUP 0x343e/0x3440 */
u8 __far *anim_posA_tbl;        /* DGROUP 0xf4/0xf6     */
u8 __far *anim_posB_tbl;        /* DGROUP 0x3f4/0x3f6   */

/* ── view descriptors (the draw/erase BGI-overlay save-under / restore passes) ──*/
u8 __far *anim_a_erase_view;    /* DGROUP 0x8d4 */
u8 __far *anim_a_draw_view;     /* DGROUP 0x8e0 */
u8 __far *anim_a_clear_view;    /* DGROUP 0x8c0 */
u8 __far *anim_b_view0;         /* DGROUP 0x8c8 */
u8 __far *anim_b_view1;         /* DGROUP 0x8cc */
u8 __far *anim_b_draw_view;     /* DGROUP 0x8d0 */
u8 __far *anim_b_clear_view;    /* DGROUP 0x8bc */

/* ── blit descriptor far ptr (draw fns pass this to blit_sprite) ───────────────*/
u8 __far *p1_sprite;            /* DGROUP 0x8884 */

/* ── step-state scalars / working ptrs ─────────────────────────────────────────*/
u8 __far *g_anim_stream_ptr;    /* DGROUP 0xa0be/0xa0c0 — working stream ptr (A)   */
u8  g_anim_cur_cmd_byte;        /* DGROUP 0x8578 — cur cmd byte (step A)           */
u8  anim_b_loop_idx;            /* DGROUP 0x8566 — loop idx (step B)               */
u8 __far *anim_b_stream_ptr;    /* DGROUP 0xa0c2/0xa0c4 — working stream ptr (B)   */
u8  anim_b_cur_frame_byte;      /* DGROUP 0x8579 — cur frame byte (step B)         */

/* No function bodies in T2 — the seven anim fns stay stubbed in game_stubs.c. */
