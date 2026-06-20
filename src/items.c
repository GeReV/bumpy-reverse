/* ────────────────────────────────────────────────────────────────────────────
 *  items.c — item-collection & level-exit module (Phase-3 reconstruction).
 *
 *  Phase-3 Task 3 ports the item-collection + scoring trio 1:1 from the live
 *  Ghidra BumpyDecomp (verified against the raw disassembly):
 *      p1_collect_item        1000:6c14
 *      p1_collect_item_score  1000:6c95
 *      move_step_read_item    1000:6627
 *  plus the tiny layer-C read leaf it calls:
 *      read_tile_layer2       1000:6bf4
 *  read_tile_layer2 is the +0x60 sibling of player.c's read_tile_layer_contact
 *  (+0x30) / read_tile_at_cell (+0); it writes THIS module's p1_item_code, so it
 *  lives here.  check_exit_tile_vert (1000:6372) and teleport_to_next_exit_tile
 *  (1000:25ad) are Phase-3 Task 4 — still stubbed in game_stubs.c.
 *
 *  OWNERSHIP / no-duplicate-symbols: this TU defines ONLY the item-module globals
 *  no other TU owns (items_remaining, level_exit_cell, level_complete_anim_counter,
 *  p1_item_code, plus sharp_item_counter @0x791a and collect_mode_2810 @0x2810 —
 *  neither in the replay SNAP).  Every other semantic-state global is owned
 *  elsewhere (game.c: score_lo/hi, tilemap; level.c: current_level; player.c:
 *  level_complete_flag, anim_target_cell, p1_cell, sound_device_state, …) and is
 *  declared extern via items.h, NEVER redefined here.
 *
 *  STACK-CHECK PROLOGUE: every original opens with
 *  `if (stack_check_limit <= &stack0xfffe) FUN_1000_ab83();` — Turbo C's compiler-
 *  emitted stack-overflow probe, NOT game logic.  It is intentionally OMITTED from
 *  the ports (the same convention player.c uses; documented once there).
 *
 *  Source of truth: Ghidra BumpyDecomp + raw disassembly + tools/items_oracle.py +
 *  local/build/items_model.md (the Phase-3 T1 semantic-state capture).
 * ──────────────────────────────────────────────────────────────────────────── */
#include "items.h"

/* Cross-module globals read/written by the ported functions (owned elsewhere). */
extern u16 score_lo, score_hi;   /* game.c   — DGROUP 0xa0d4 / 0xa0d6 (32-bit score) */
extern u8  level_complete_flag;  /* player.c — DGROUP 0xa1b1                          */
extern u8  anim_target_cell;     /* player.c — DGROUP 0x856f                          */
extern u8  p1_cell;              /* player.c — DGROUP 0x856e                          */
extern s16 sound_device_state;   /* player.c — DGROUP 0x689c (==4 → OPL/charger ids)  */
extern u8 __far *tilemap;        /* game.c   — level tilemap far ptr (DGROUP 0xa0d8)  */

/* FX / sound callees — STUBBED, owned by game_stubs.c (BUMPY.EXE) / the harness.
   RECONSTRUCTION FIDELITY: the item-complete branch plays a level-complete sound
   and fires the exit animation; these are render/audio-subsystem leaves (Phase 5/6)
   with no effect on the validated semantic state, so they stay extern stubs. */
extern void play_sound(u8 sound_id);        /* 1000:6e11 — sound dispatch (→ Phase 6) */
extern void apply_cell_animation(u8 fx);    /* 1000:69aa — anim-channel alloc (→ P5)  */

/* ── item/exit module-owned globals (DGROUP 0x203b offsets) ──────────────────── */
u8  items_remaining;             /* DGROUP 0xa0cf — items left on the current level */
u8  level_exit_cell;             /* DGROUP 0x8572 — grid cell of the level exit     */
u8  level_complete_anim_counter; /* DGROUP 0x8550 — set to 0xf2 when last item taken*/
u8  p1_item_code;                /* DGROUP 0x79b8 — latched layer-C item byte        */
u8  sharp_item_counter;          /* DGROUP 0x791a — '#'-item pickup counter          */
u8  collect_mode_2810;           /* DGROUP 0x2810 — set to 0xf at p1_collect_item entry*/

/*
 * read_tile_layer2 — 1000:6bf4
 * --------------------------------------------------------------------------
 * Read the +0x60 tilemap "item" (layer-C) byte at `cell` into p1_item_code.  The
 * +0x60 sibling of read_tile_layer_contact (+0x30) / read_tile_at_cell (+0) in
 * player.c; it writes this module's p1_item_code so it is reconstructed here.
 */
void read_tile_layer2(u8 cell)
{
    p1_item_code = tilemap[(u16)cell + 0x60];
    return;
}

/*
 * p1_collect_item_score — 1000:6c95
 * --------------------------------------------------------------------------
 * Queue an erase of the player's cell view, then award score for the collected
 * item under p1_cell (layer-C code p1_item_code).  The 32-bit score lives in
 * score_hi:score_lo and is ALWAYS incremented by the 250-pt base first; then per
 * item code: '#' (0x23) increments sharp_item_counter and adds nothing further;
 * '/' (0x2f) adds +9750 (→ 10000 total); '0' (0x30) adds +49750 (→ 50000 total);
 * any other code keeps just the base 250.
 *
 * RECONSTRUCTION FIDELITY (score arithmetic): the decompiler hoists the base add
 * into a temporary and reconstructs the carry with comparator constants
 * (`0xff05 < score_lo`, `0xd9e9 < uVar1`, …).  The MACHINE CODE (disasm
 * 6ceb–6d1f) is a straight `ADD [score_lo],imm; ADC [score_hi],0` sequence: base
 * +0xfa always, then +0x2616 ('/') or +0xc256 ('0') with carry.  This port mirrors
 * the machine code (the faithful form); the result is identical to the decomp.
 *
 * RECONSTRUCTION FIDELITY (cell-view erase queue, disasm 6ca1–6ce7): the engine
 * stages a 2-tile erase of the player's cell view — pending_erase_count=2,
 * pending_erase_x/y from the posC pixel tables at DGROUP [0x274]/[0x276], and an
 * erase_kind (1/2 by cell parity) into the pending_erase_view descriptor.  These
 * are render-subsystem state (the per-tick view-present chain, Phase 5) and are
 * NOT part of the validated semantic state, so the erase staging is documented
 * here rather than reproduced against invented render globals.
 */
void p1_collect_item_score(void)
{
    /* --- cell-view erase staging (render-subsystem, Phase 5) — see note above ---
       pending_erase_count = 2;
       pending_erase_x = posC_X[p1_cell] >> 4;   (DGROUP [p1_cell*4 + 0x274] >> 4)
       pending_erase_y = posC_Y[p1_cell] >> 3;   (DGROUP [p1_cell*4 + 0x276] >> 3)
       pending_erase_view->kind = (p1_cell & 1) ? 1 : 2;                          */

    /* base award: +250 to the 32-bit score (ADD score_lo,0xfa; ADC score_hi,0). */
    score_hi = score_hi + (u16)(score_lo + 0xfa < score_lo);   /* carry out of +0xfa */
    score_lo = score_lo + 0xfa;

    if (p1_item_code == '#') {
        sharp_item_counter = sharp_item_counter + 1;
    } else if (p1_item_code == '/') {
        score_hi = score_hi + (u16)(score_lo + 0x2616 < score_lo);   /* +9750  */
        score_lo = score_lo + 0x2616;
    } else if (p1_item_code == '0') {
        score_hi = score_hi + (u16)(score_lo + 0xc256 < score_lo);   /* +49750 */
        score_lo = score_lo + 0xc256;
    }
    /* else: just the base 250. */
    return;
}

/*
 * p1_collect_item — 1000:6c14
 * --------------------------------------------------------------------------
 * Pick up the item at p1_cell: award score (p1_collect_item_score), clear the
 * +0x60 layer-C byte; unless the item is '\x01' or '#', decrement items_remaining
 * and — when it reaches 0 — play the level-complete sound (device-dependent),
 * relocate the exit animation (anim_target_cell = level_exit_cell;
 * apply_cell_animation('Y')), set level_complete_flag, and arm
 * level_complete_anim_counter = 0xf2.  When items remain, play the pickup sound.
 *
 * (collect_mode_2810 = 0xf is a DGROUP tag the engine stamps at entry — disasm
 * 6c20; not part of the validated semantic state.)
 */
void p1_collect_item(void)
{
    u8 sound_id;

    collect_mode_2810 = 0xf;
    p1_collect_item_score();
    tilemap[(u16)p1_cell + 0x60] = 0;                 /* clear the collected item */

    if (p1_item_code != '\x01' && p1_item_code != '#') {
        items_remaining = items_remaining - 1;
        if (items_remaining == 0) {
            sound_id = (sound_device_state == 4) ? 0x2c : 0x0b;
            play_sound(sound_id);                     /* level-complete sound (stub) */
            anim_target_cell = level_exit_cell;
            apply_cell_animation('Y');                /* exit anim (stub, → Phase 5) */
            level_complete_flag = 1;
            level_complete_anim_counter = 0xf2;
        } else {
            sound_id = (sound_device_state == 4) ? 0x21 : 0x0e;
            play_sound(sound_id);                     /* item-pickup sound (stub)    */
        }
    }
    return;
}

/*
 * move_step_read_item — 1000:6627
 * --------------------------------------------------------------------------
 * Move-step handler: read the layer-C item byte under p1_cell (read_tile_layer2 →
 * p1_item_code = tilemap[p1_cell+0x60]); if it is nonzero, collect it
 * (p1_collect_item).  Dispatched by address via move_step_dispatch_tbl.
 */
void move_step_read_item(void)
{
    read_tile_layer2(p1_cell);
    if (p1_item_code != '\0') {
        p1_collect_item();
    }
    return;
}
