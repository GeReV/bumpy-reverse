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
 *  lives here.  Phase-3 Task 4 adds the two level-exit functions —
 *  check_exit_tile_vert (1000:6372) and teleport_to_next_exit_tile (1000:25ad) —
 *  reconstructed 1:1 below and un-stubbed from game_stubs.c.  The exit→level-
 *  advance wiring lives in game.c's run_game_session/game_loop (Phase-3 T4).
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

/* Cross-module globals the exit/teleport functions (T4) touch (owned by player.c). */
extern u8  move_step_count;      /* player.c — DGROUP 0x855e — jump/move-step counter */
extern u8  p1_move_step_idx;     /* player.c — DGROUP 0x792a — move-step sub-index    */
extern u8  physics_frozen;       /* player.c — DGROUP 0xa0ce — physics-freeze flag    */
extern s16 p1_pixel_y;           /* player.c — DGROUP 0x9292 — P1 pixel-Y             */

/* FX / sound callees — STUBBED, owned by game_stubs.c (BUMPY.EXE) / the harness.
   RECONSTRUCTION FIDELITY: the item-complete branch plays a level-complete sound
   and fires the exit animation; these are render/audio-subsystem leaves (Phase 5/6)
   with no effect on the validated semantic state, so they stay extern stubs. */
extern void play_sound(u8 sound_id);        /* 1000:6e11 — sound dispatch (→ Phase 6) */
extern void apply_cell_animation(u8 fx);    /* 1000:69aa — anim-channel alloc (→ P5)  */

/* Player-spine callees the exit/teleport functions invoke (owned by player.c, all
   reconstructed there in Phase 2).  enter_game_mode + dispatch_move_step write
   game_mode/the move-step dispatch (NOT in the validated SNAP); p1_set_pixel_from_cell
   writes move_step_count + p1_pixel_y from the cell-coord table (IN the SNAP — so it
   is a faithful reconstruction in player.c, not a stub). */
extern void enter_game_mode(u8 mode);       /* 1000:4263 — game-mode transition       */
extern void dispatch_move_step(void);       /* 1000:238e — move-step sub-dispatch      */
extern void p1_set_pixel_from_cell(void);   /* 1000:4906 — set p1 pixel_x/y from cell  */

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

/*
 * check_exit_tile_vert — 1000:6372
 * --------------------------------------------------------------------------
 * Vertical exit-tile detection.  If the player is NOT at the row edge
 * (move_step_count != 7) AND the neighbour tile one row below (tilemap[p1_cell +
 * 0x30]) is an exit tile (0x0c), commit the level-exit transition: reset the
 * move-step sub-index (p1_move_step_idx = 0), freeze physics (physics_frozen = 1),
 * enter end-of-level game mode 0x2e (enter_game_mode), and play the exit sound
 * (device-dependent: 0x0d on the OPL device, else 0x03).  Otherwise no-op.
 *
 * Verified against disasm 1000:6372–63bd:
 *   637e CMP [0x855e],7 / JZ → move_step_count != 7
 *   6390 CMP ES:[BX+0x30],0xc / JNZ → tilemap[p1_cell+0x30] == 0x0c
 *   6397 MOV [0x792a],0 → p1_move_step_idx = 0
 *   639c MOV [0xa0ce],1 → physics_frozen = 1
 *   63a4 CALL enter_game_mode(0x2e)
 *   63a9 CMP [0x689c],4 → sound id 0x0d : 0x03
 *
 * RECONSTRUCTION FIDELITY: play_sound + enter_game_mode are player/audio-subsystem
 * leaves; enter_game_mode is reconstructed in player.c, play_sound stays a stub
 * (no effect on the validated semantic state — physics_frozen is the observable).
 */
void check_exit_tile_vert(void)
{
    u8 sound_id;

    /* move_step_count != 7  AND  tilemap[p1_cell + 0x30] == 0x0c (exit tile).
       The tilemap deref mirrors the engine's LES BX,[tilemap]; CMP ES:[BX+
       p1_cell+0x30],0xc — the +0x30 neighbour one grid-row below p1_cell. */
    if ((move_step_count != '\a') &&
        ((s8)tilemap[(u16)p1_cell + 0x30] == '\f')) {
        p1_move_step_idx = 0;
        physics_frozen = 1;
        enter_game_mode(0x2e);
        sound_id = (sound_device_state == 4) ? 0x0d : 0x03;
        play_sound(sound_id);                     /* exit sound (stub, → Phase 6) */
    }
    return;
}

/*
 * teleport_to_next_exit_tile — 1000:25ad
 * --------------------------------------------------------------------------
 * Scan the tilemap FORWARD from p1_cell (cell index, wrapping at 0x30 back to 0)
 * for the next teleport/exit tile (tilemap[scan_cell] == 0x0f).  On the first hit:
 * relocate the player there (anim_target_cell = p1_cell = scan_cell), set the pixel
 * position from the new cell (p1_set_pixel_from_cell), nudge pixel-Y down by 0x0d,
 * fire the teleport FX (apply_cell_animation(0x27)), play the teleport sound
 * (0x28 on the OPL device, else 0x03), enter game mode 0x0f (enter_game_mode), and
 * run the move-step dispatch (dispatch_move_step).  The scan loop terminates on the
 * first match (found flag).
 *
 * Verified against disasm 1000:25ad–2633:
 *   25bb found = 0; 25bf scan_cell = p1_cell
 *   25c7 INC scan_cell; 25cf CMP 0x30 → wrap to 0
 *   25e2 CMP ES:[BX],0xf → tilemap[scan_cell] == 0x0f
 *   25eb/25ee anim_target_cell = p1_cell = scan_cell
 *   25f1 CALL p1_set_pixel_from_cell
 *   25f4 [0x9292] += 0xd → p1_pixel_y += 0x0d
 *   2600 CALL apply_cell_animation(0x27)
 *   2605 CMP [0x689c],4 → sound id 0x28 : 0x03
 *   261b CALL enter_game_mode(0x0f)
 *   2620 CALL dispatch_move_step
 *
 * RECONSTRUCTION FIDELITY: apply_cell_animation + play_sound stay stubs (FX/audio
 * leaves, Phase 5/6).  enter_game_mode + dispatch_move_step are reconstructed in
 * player.c (they write game_mode / the move-step dispatch — not validated SNAP
 * fields here).  p1_set_pixel_from_cell (1000:4906) writes move_step_count +
 * p1_pixel_y from the DGROUP cell-coord table (BOTH in the validated SNAP); it is a
 * player.c move/teleport leaf NOT yet reconstructed there, so for the BUMPY.EXE
 * link it is a faithful-signature stub in game_stubs.c (DEFERRED to the player
 * subsystem) and the items host harness (items_ctest.c) reproduces its coord-table
 * effect faithfully for the per-fn differential.
 */
void teleport_to_next_exit_tile(void)
{
    u8 sound_id;
    u8 found;
    u8 scan_cell;

    found = '\0';
    scan_cell = p1_cell;
    while (found == '\0') {
        scan_cell = scan_cell + 1;
        if (scan_cell == 0x30) {
            scan_cell = 0;
        }
        if ((s8)tilemap[(u16)scan_cell] == '\x0f') {
            anim_target_cell = scan_cell;
            p1_cell = scan_cell;
            p1_set_pixel_from_cell();
            p1_pixel_y = p1_pixel_y + 0xd;
            apply_cell_animation('\'');           /* teleport FX (stub, → Phase 5) */
            sound_id = (sound_device_state == 4) ? 0x28 : 0x03;
            play_sound(sound_id);                 /* teleport sound (stub, → Phase 6) */
            enter_game_mode(0x0f);
            dispatch_move_step();
            found = '\x01';
        }
    }
    return;
}
