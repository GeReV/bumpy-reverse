/*
 * player2.c — Player-2 (AI opponent) module  [Phase-4 reconstruction]
 *
 * Phase-4 Task 3 lands the P2 MOVE-STATE MACHINE here (the P1-physics analog):
 * the six move-state/trajectory functions p2_set_move_state (1000:4bc6),
 * p2_step_scripted_move (1000:4c14), p2_update_grid_cell (1000:4b4e),
 * p2_tile_move_check (1000:4c99), p2_set_pixel_from_cell (1000:48a9) and
 * p2_advance_grid_history (1000:13b2), each ported 1:1 from the live Ghidra decomp
 * (verified via Ghidra MCP decompile + disassembly, 2026-06; behaviour also in
 * local/build/p2_model.md).  The FX/sound/AI callees these functions reach
 * (p2_run_move_state_handler, p2_ai_select_move_random, and the per-state
 * move-state handler-table targets at DGROUP 0x870) stay STUBBED in game_stubs.c
 * (the AI group ports in T4; the render/pvp group in T5) — each documented below
 * with a RECONSTRUCTION FIDELITY note.  This TU also defines the Player-2 globals.
 * player2.obj is linked into BUMPY.EXE (Makefile BUMPY_OBJS) as of this task.
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
#include "gfx_overlay.h"  /* player_view_geom_t */
#include "entity.h"       /* sprite_obj_t */

/* ── P2-render globals — OWNED here (see ownership note above) ────────────────── */
s16 p2_pixel_x;          /* DGROUP 0x79ba */
s16 p2_pixel_y;          /* DGROUP 0x79bc */
u16 p2_move_anim;        /* DGROUP 0x8560 */
s8  p2_cell = (s8)0xff;  /* DGROUP 0x8571 — -1 (0xff) sentinel = P2 absent        */
u16 p2_frame_base;       /* DGROUP 0xa0de */

/* ── P2 move-state / trajectory globals — OWNED here ─────────────────────────── */
u8  p2_move_steps_left;  /* DGROUP 0xa1b0 */
u8  p2_step_idx;         /* DGROUP 0x8563 (Ghidra: p2_move_step_idx)              */
u8  p2_facing_neg_dx;    /* DGROUP 0x9d2f (Ghidra: p2_facing_left — negate script dx)*/
u8  p2_move_toggle;      /* DGROUP 0x8243 (Ghidra: p2_move_tick_toggle)           */
s16 p2_grid_col;         /* DGROUP 0xa0ca (Ghidra: p2_grid_x_new — working col)   */
s16 p2_grid_row;         /* DGROUP 0xa0cc (Ghidra: p2_grid_y_new — working row)   */
u8  p2_set_cell_col;     /* DGROUP 0x8564 (Ghidra: p2_col_counter — in-row column)*/
u8  p2_set_cell_row;     /* DGROUP 0x8565 (Ghidra: p2_row_counter — grid row)     */

/* P2 move-script far pointer (DGROUP 0xa0ba off / 0xa0bc seg) — the engine keeps
   it SPLIT as two words (docs/engine.md); here a single u16 __far * carries it,
   the same convention as p1_move_script in player.c.  p2_set_move_state loads it
   from the per-state script table; p2_step_scripted_move reads [anim,dx,dy] 6-byte
   entries through it and advances by 3 words (= 6 bytes) per step. */
u16 __far *p2_move_script;   /* DGROUP 0xa0ba/0xa0bc */

/* P2 sprite/object descriptor far pointer (DGROUP 0x9b9e off / 0x9ba0 seg).
   p2_update_grid_cell reads the sprite origin words at +0x14 (x) / +0x16 (y). */
u8 __far  *p2_sprite;        /* DGROUP 0x9b9e/0x9ba0 */

/* P2 per-state move-script table base (DGROUP 0x2520 off words / 0x2522 seg words).
   One 4-byte far-pointer entry per move-state (indexed by state*4); the pointed-to
   script header is [steps(1), facing(1), move_script_off(2), move_script_seg(2)].
   RECONSTRUCTION FIDELITY: the engine reads the two table words as NEAR DS-relative
   accesses (asm 1000:4be3/4be7 `word ptr [BX+0x2522]` / `[BX+0x2520]`, BX=state*4).
   As with p2_cell_coord_tbl, the OW build's DGROUP layout is uncontrolled, so the
   table is reached through this far shadow base; the indexing/deref is 1:1 with the
   asm (the table is populated by level/engine init; the host harness seeds it). */
u8 __far  *p2_state_script_tbl;/* DGROUP 0x2520/0x2522 (per-state script table) */

/* P2 cell->pixel coordinate table base (DGROUP 0x274 — the posC table, shared with
   P1's p1_set_pixel_from_cell).  Per 4-byte cell entry: X @ +0, Y @ +2.
   RECONSTRUCTION FIDELITY: the engine reads this as a NEAR DS-relative table
   (asm 1000:48e5 `word ptr [BX+0x274]`, BX=cell*4).  As with level.c's g_entity_dg
   DGROUP shadow, the Open Watcom build's DGROUP layout is uncontrolled, so the
   table is reached through this far shadow pointer (level init points it at the
   real coord table; the host harness seeds a synthetic window).  The pointer
   *value* is the only deviation — the indexing arithmetic is 1:1 with the asm. */
u8 __far  *p2_cell_coord_tbl;/* DGROUP 0x0274 (posC X/Y coord table base) */

/* P2 move-state handler table base (DGROUP 0x85c — near-ptr table of per-state
   cell-move handlers, indexed by move_state*2).  p2_run_move_state_handler calls
   (*tbl[move_state])() through it when p2_step_idx==5.
   RECONSTRUCTION FIDELITY: the engine reads this as a NEAR DS-relative table (asm
   1000:501f `CALL word ptr [BX+0x85c]`, BX=move_state*2) holding 16-bit near code
   pointers.  As with p2_state_script_tbl / p2_cell_coord_tbl, the OW build's DGROUP
   layout is uncontrolled and host function pointers are wider than the engine's
   16-bit near pointers, so the table is reached through this far shadow base whose
   entries are host void(*)(void) callables (the cell-move handlers).  The
   indexing/deref is 1:1 with the asm; the table is populated by engine init (the
   host harness seeds it with the reconstructed cell-move handlers). */
void (__far * __far *p2_state_handler_tbl)(void);/* DGROUP 0x085c (per-state handler tbl)*/

/* P2 grid-cell history (p2_advance_grid_history slides new->cur->prev). */
s16 p2_grid_x;           /* DGROUP 0x8558 — current grid col */
s16 p2_grid_y;           /* DGROUP 0x855a — current grid row */
s16 p2_grid_x_prev;      /* DGROUP 0x928e — previous grid col */
s16 p2_grid_y_prev;      /* DGROUP 0x9b94 — previous grid row */

/* ── AI rng-decision globals — OWNED here ────────────────────────────────────── */
u8  p2_ai_threshold;     /* DGROUP 0x7920 */
u8  p2_dir_blocked_0;    /* DGROUP 0xa0e0 */
u8  p2_dir_blocked_1;    /* DGROUP 0xa0e1 */
u8  p2_dir_blocked_2;    /* DGROUP 0xa0e2 — 4th dir flag (read by p2_tile_move_check)*/
u8  p2_dir_blocked_3;    /* DGROUP 0xa1b2 */

/* ── P1/P2 pvp-collision globals — OWNED here ─────────────────────────────────── */
u8  pvp_collision_flag;  /* DGROUP 0xa1aa (Ghidra players_colliding) */
pvp_bbox_t pvp_p1_bbox;  /* DGROUP 0x084c..0x0852 (Ghidra p1_bbox_left/right/top/bottom)  */
pvp_bbox_t pvp_p2_bbox;  /* DGROUP 0x0854..0x085a (Ghidra p2_bbox_left/right/top/bottom)  */

/* ── P2 render/view globals — OWNED here (Phase-4 T5; no other TU names them) ──── */
s16 p2_scroll_x;         /* DGROUP 0x9d34 — P2 view scroll X (render/erase view) */
s16 p2_scroll_y;         /* DGROUP 0x9d32 — P2 view scroll Y (render/erase view) */

/* P2 render/erase view-descriptor far pointers.  render_p2_view writes geometry
   into the descriptor at 0x8ec/0x8ee and presents it; erase_p2_view writes the
   previous-cell geometry into the descriptor at 0x8e8/0x8ea and restores under it.
   The engine keeps each SPLIT as two DGROUP words (off,seg); here a single far
   pointer carries it, the same convention as p2_sprite / p2_move_script above. */
u8 __far *p2_view;       /* DGROUP 0x8ec/0x8ee — render_player_view descriptor */
u8 __far *p2_erase_view; /* DGROUP 0x8e8/0x8ea — restore_bg_view descriptor    */

/* ── cross-module globals the P2 fns read (owned elsewhere; extern only) ──────── */
extern u8 __far *tilemap;     /* game.c 0xa0d8/0xa0da — level tilemap far pointer */
/* tilemap's contact/layer-B band offset (player.h's TILE_CONTACT_LAYER_OFF;
 * duplicated locally — player2.c deliberately does not #include player.h, see
 * the file header note on header collisions).  Bare (unsuffixed, signed-int)
 * literal — matches the original exactly. */
#define TILE_CONTACT_LAYER_OFF 0x30
extern u8  rng_frame;         /* player.c 0x79b3 — the AI rng-decision input (the AI
                                 selectors branch on it; select_move_random overwrites
                                 it with rand()).  Owned by player.c (player.h);
                                 declared here as the P2 AI layer is the heavy reader. */

/* check_pvp_collision (1000:50fb) reads these cross-module globals (owned elsewhere;
   extern only here — declaring their owning headers' symbol, NOT redefining). */
extern u8  physics_frozen;        /* player.c 0xa0ce — physics-freeze gate          */
extern u8  game_mode;             /* game.c   0x792c — '0'(0x30) gate               */
extern u8  session_continue_flag; /* game.c   0x856d — round/session gate           */
extern s16 sound_device_state;    /* player.c 0x689c — ==4 -> OPL/charger sound ids */
extern void play_sound(u8 sound_id); /* 1000:6e11 — audio leaf (stubbed; → Phase 6) */
/* update_p1_bbox (1000:5085, Phase-9 T3) reads the P1 pixel position (player.c). */
extern s16 p1_pixel_x;            /* player.c 0x9290 */
extern s16 p1_pixel_y;            /* player.c 0x9292 */

/* ════════════════════════════════════════════════════════════════════════════
 *  CALLEES of p2_tile_move_check / the AI layer (Phase-4 T4)
 *    - p2_run_move_state_handler (1000:5003) — RECONSTRUCTED below (T4): dispatches
 *      p2_move_state through the near-ptr handler table at DGROUP 0x85c (the cell-move
 *      handler group) when p2_step_idx==5.
 *    - p2_ai_select_move_random (1000:4fd3) — RECONSTRUCTED below (T4): picks a fresh
 *      random move-state (5..8) via rand()/prng_step.
 *    - the per-state move-state handler at handler-table 0x870 (the `(*tbl[state])()`
 *      indirect call in p2_tile_move_check) — dispatched via the helper
 *      p2_dispatch_move_state_handler (game_stubs.c).  GROUNDED (2026-07-03): the
 *      0x870 table is STATIC DGROUP data in the image (file 0x11440+0x870), NEAR
 *      code ptrs — [1..4] = p2_pick_move_priority_a/b/c + p2_ai_dispatch_move
 *      (all reconstructed below), [5..9] = p2_pick_move_priority_a, [0]/[10] = the
 *      empty fn 1000:7111; no runtime writer exists.  The default build keeps the
 *      helper a link-only no-op stub (harness parity); the playable build carries
 *      the real C fn-ptr mirror of the table (see game_stubs.c).
 * ════════════════════════════════════════════════════════════════════════════ */
void p2_dispatch_move_state_handler(void);/* DGROUP 0x870[move_state] — game_stubs.c */

/* P2 cell-direction move handlers — the targets of the DGROUP-0x85c near-ptr handler
   table that p2_run_move_state_handler dispatches through (ported 1:1 below, T4). */
void p2_cell_move_up(void);               /* 1000:5025 */
void p2_cell_move_down(void);             /* 1000:503f */
void p2_cell_move_left(void);             /* 1000:5059 */
void p2_cell_move_right(void);            /* 1000:506f */

/* ════════════════════════════════════════════════════════════════════════════
 *  P2 MOVE-STATE MACHINE — Phase-4 Task 3 (ported 1:1 from the Ghidra decomp).
 * ════════════════════════════════════════════════════════════════════════════ */

/*
 * p2_set_move_state — 1000:4bc6
 * --------------------------------------------------------------------------
 * Set P2's move-state and load that state's script header from the per-state
 * script table at DGROUP 0x2520 (offset words) / 0x2522 (segment words), indexed
 * by state*4 (one 4-byte far-pointer entry per state).  The pointed-to script
 * supplies: steps_left (script[0]), facing flag (script[1]) and the [anim,dx,dy]
 * move-script far pointer (script[2..5] = off,seg).
 *
 * Mirrors enter_game_mode's P1 analog exactly (player.c): the table-entry words
 * are NEAR DGROUP reads, the pointed-to script bytes are FAR reads.
 */
void p2_set_move_state(u8 state)
{
    u16             tbl_off;
    u16             tbl_seg;
    const u8 __far *script;

    p2_move_state = state;

    /* p2_state_script_tbl[state] : 4-byte far pointer (off @ +0/0x2520, seg @ +2/0x2522).
       Reached through the DGROUP-shadow base p2_state_script_tbl (see its decl) —
       indexing state*4 is 1:1 with asm 1000:4be3/4be7. */
    tbl_off = *(u16 __far *)(p2_state_script_tbl + (u16)state * 4 + 0);
    tbl_seg = *(u16 __far *)(p2_state_script_tbl + (u16)state * 4 + 2);
    script  = (const u8 __far *)MK_FP(tbl_seg, tbl_off);

    p2_move_steps_left = script[0];                       /* steps  = script[0] */
    p2_facing_neg_dx   = script[1];                       /* facing = script[1] */
    /* p2_move_script far ptr = (script+2 .. script+5) = (off, seg). */
    p2_move_script = (u16 __far *)MK_FP(*(u16 __far *)(script + 4),
                                        *(u16 __far *)(script + 2));
    return;
}

/*
 * p2_step_scripted_move — 1000:4c14
 * --------------------------------------------------------------------------
 * Advance P2 one step along its move-script, gated to alternating ticks by the
 * XOR-1 half-rate toggle p2_move_toggle.  When the toggle is set AND P2 is present
 * (p2_cell != -1): read [anim,dx,dy] from p2_move_script, latch p2_move_anim, add
 * dx to p2_pixel_x (dx negated when p2_facing_neg_dx), add dy to p2_pixel_y,
 * advance the script by one 6-byte entry, decrement p2_move_steps_left.  Returns 0
 * (and resets p2_step_idx) when the sequence ends, else the new step index.
 *
 * RECONSTRUCTION FIDELITY (far-ptr advance): the engine advances ONLY the offset
 * word (asm 1000:4c76 `ADD word ptr [0xa0ba],0x6`); the segment at 0xa0bc is left
 * unchanged.  Pointer-add on a single u16 __far * mutates the offset and preserves
 * the segment — identical for any script that does not cross a 64 KiB boundary (the
 * engine's scripts never do).  Same convention as p1_step_scripted_move.
 */
u8 p2_step_scripted_move(void)
{
    u8  result;
    int dx;

    p2_move_toggle = (u8)(p2_move_toggle ^ 1);            /* half-rate gate */
    result = p2_move_toggle;

    if (p2_move_toggle != 0 && p2_cell != (s8)0xff) {
        p2_move_anim = p2_move_script[0];                 /* anim = script[0] */

        if (p2_facing_neg_dx == 0) {
            dx = (int)(s16)p2_move_script[1];             /* facing right: +dx */
        } else {
            dx = -(int)(s16)p2_move_script[1];            /* facing left:  -dx */
        }
        p2_pixel_x = (s16)(p2_pixel_x + dx);
        p2_pixel_y = (s16)(p2_pixel_y + (s16)p2_move_script[2]);

        p2_move_script += 3;                              /* advance one 6-byte entry */
        p2_move_steps_left = (u8)(p2_move_steps_left - 1);

        if (p2_move_steps_left == 0) {
            result = 0;
            p2_step_idx = 0;
        } else {
            p2_step_idx = (u8)(p2_step_idx + 1);
            result = p2_step_idx;
        }
    }
    return result;
}

/*
 * p2_update_grid_cell — 1000:4b4e
 * --------------------------------------------------------------------------
 * Recompute P2's working grid cell (p2_grid_col/p2_grid_row, Ghidra
 * p2_grid_x_new/y_new) from the P2 pixel position minus the sprite origin (read
 * from the P2 sprite object at +0x14 / +0x16), then clamp col to 0..0x12 and row
 * to 0..0x16.  No-op when P2 is absent (p2_cell == -1).
 *
 * The X divide is an arithmetic SAR by 4 then -1; the Y divide is SAR by 3 (three
 * 1-bit SARs in the asm) — reproduced as signed >> 4 / >> 3.
 */
void p2_update_grid_cell(void)
{
    if (p2_cell != (s8)0xff) {
        s16 ox = ((sprite_obj_t __far *)p2_sprite)->anchor_x;        /* sprite origin x */
        s16 oy = ((sprite_obj_t __far *)p2_sprite)->anchor_y;        /* sprite origin y */

        p2_grid_col = (s16)(((p2_pixel_x - ox) >> 4) - 1);
        p2_grid_row = (s16)((p2_pixel_y - oy) >> 3);

        if (p2_grid_col < 0) {
            p2_grid_col = 0;
        } else if (p2_grid_col > 0x12) {
            p2_grid_col = 0x12;
        }
        if (p2_grid_row < 0) {
            p2_grid_row = 0;
        } else if (p2_grid_row > 0x16) {
            p2_grid_row = 0x16;
        }
    }
    return;
}

/*
 * p2_tile_move_check — 1000:4c99
 * --------------------------------------------------------------------------
 * P2 tile-movement / collision check, run when P2's scripted move has finished.
 * Gated by the half-rate toggle (p2_move_toggle) and P2 presence (p2_cell != -1):
 *   - if steps remain (p2_move_steps_left != 0): p2_run_move_state_handler().
 *   - else: probe the level tilemap around P2's current cell to set the four
 *     direction-open flags (p2_dir_blocked_0/1/2/3); if ALL four are blocked,
 *     p2_ai_select_move_random(); otherwise dispatch by p2_move_state through the
 *     handler table at DGROUP 0x870.
 *
 * The four probes (1:1 with the asm):
 *   blocked_0 ← cell>=8   && tilemap[cell-8]==0           (up)
 *   blocked_1 ← cell<0x28 && tilemap[cell]==0             (down)
 *   blocked_2 ← col!=0    && tilemap[cell+0x2f]==0; then 1 if tilemap[cell-1]==0x0b
 *   blocked_3 ← col!=7    && tilemap[cell+0x30]==0; then 1 if tilemap[cell+1]==0x0b
 * (col = p2_set_cell_col / Ghidra p2_col_counter).  p2_cell doubles as the
 * 'P2 present' sentinel (0xff = no P2).
 */
void p2_tile_move_check(void)
{
    u8 cell;

    if (p2_move_toggle != 0 && p2_cell != (s8)0xff) {
        if (p2_move_steps_left != 0) {
            p2_run_move_state_handler();                  /* 1000:5003 (T4) */
        } else {
            cell = (u8)p2_cell;
            p2_dir_blocked_3 = 1;
            p2_dir_blocked_2 = 1;
            p2_dir_blocked_1 = 1;
            p2_dir_blocked_0 = 1;

            if (cell >= 8 && tilemap[(u16)cell - 8] == 0) {
                p2_dir_blocked_0 = 0;
            }
            if (cell < 0x28 && tilemap[(u16)cell] == 0) {
                p2_dir_blocked_1 = 0;
            }
            if (p2_set_cell_col != 0 && tilemap[(u16)cell + TILE_CONTACT_LAYER_OFF - 1] == 0) {
                p2_dir_blocked_2 = 0;
                if (tilemap[(u16)cell - 1] == 0x0b) {
                    p2_dir_blocked_2 = 1;
                }
            }
            if (p2_set_cell_col != 7 && tilemap[(u16)cell + TILE_CONTACT_LAYER_OFF] == 0) {
                p2_dir_blocked_3 = 0;
                if (tilemap[(u16)cell + 1] == 0x0b) {
                    p2_dir_blocked_3 = 1;
                }
            }

            if ((u8)(p2_dir_blocked_0 + p2_dir_blocked_1 +
                     p2_dir_blocked_2 + p2_dir_blocked_3) == 4) {
                p2_ai_select_move_random();               /* 1000:4fd3 (T4) */
            } else {
                p2_dispatch_move_state_handler();         /* (*0x870[state])() — game_stubs.c */
            }
        }
    }
    return;
}

/*
 * p2_set_pixel_from_cell — 1000:48a9
 * --------------------------------------------------------------------------
 * If P2 is present (p2_cell != -1): derive the grid row (cell>>3) and in-row column
 * (cell - row*8), then set the P2 pixel position from the posC cell-coordinate
 * table at DGROUP 0x274/0x276 (X @ cell*4+0x274, Y @ cell*4+0x276), each +7.
 * (posC_X[cell] = col*40+8, posC_Y[cell] = row*32+8 — the level grid geometry.)
 */
void p2_set_pixel_from_cell(void)
{
    u8  cell;

    if (p2_cell != (s8)0xff) {
        cell = (u8)p2_cell;
        p2_set_cell_row = (u8)(cell >> 3);                /* grid row */
        p2_set_cell_col = (u8)(cell - (u8)(p2_set_cell_row * 8));  /* in-row col */
        p2_pixel_x = (s16)(*(s16 __far *)(p2_cell_coord_tbl + (u16)cell * 4 + 0) + 7);
        p2_pixel_y = (s16)(*(s16 __far *)(p2_cell_coord_tbl + (u16)cell * 4 + 2) + 7);
    }
    return;
}

/*
 * p2_advance_grid_history — 1000:13b2
 * --------------------------------------------------------------------------
 * Slide P2's grid-cell history one step (cur -> prev, new -> cur), only when P2 is
 * active (p2_cell != -1).  Same shape as P1's grid-history advance.
 */
void p2_advance_grid_history(void)
{
    if (p2_cell != (s8)0xff) {
        p2_grid_x_prev = p2_grid_x;
        p2_grid_y_prev = p2_grid_y;
        p2_grid_x = p2_grid_col;                          /* new col (0xa0ca) -> cur */
        p2_grid_y = p2_grid_row;                          /* new row (0xa0cc) -> cur */
    }
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  P2 AI DECISION LAYER — Phase-4 Task 4 (ported 1:1 from the Ghidra decomp).
 *
 *  P2 is AI-controlled: when its scripted move ends, p2_ai_dispatch_move routes by
 *  the four dir-blocked flags to one of the rng-threshold move selectors, which pick
 *  the next move-state and (re)enter it via p2_set_move_state.  All selectors share
 *  the same rng-branch shape: rng_frame < p2_ai_threshold splits even/odd on
 *  rng_frame&1, with the dir-blocked flags steering the chosen state.
 *
 *  Every fn cited to its Ghidra seg-1000 address (decompile_function_by_address,
 *  2026-06) and matches local/build/p2_model.md §"AI rng-decision behavior".  The
 *  Ghidra stack-probe prologue (`if (stack_check_limit <= &stack0xfffe) FUN_ab83()`)
 *  is the Turbo-C stack-overflow guard emitted on every fn; it is omitted here (a
 *  pure runtime check, no observable state — same convention as every other ported
 *  src/ function).
 * ════════════════════════════════════════════════════════════════════════════ */

/*
 * p2_engine_rand — 1000:93b1 (rand() thunk)
 * --------------------------------------------------------------------------
 * The engine's rand() (1000:93b1) is NOT the CRT rand: it CALLF's prng_step
 * (1ce5:001f) and returns AL = the low byte of the new prng_state0 (asm: prng_step
 * ends `MOV [0x5676],AX` = prng_state0, then the caller reads AL).  Modelled here as
 * a faithful static helper so p2_ai_select_move_random's rand()&1 parity is driven
 * by the reconstructed src/prng.c state exactly as the engine's is — the AI replay
 * is deterministic once prng_state0/1/2 are seeded to the engine's entry value.
 * RECONSTRUCTION FIDELITY: the engine's rand() is a tiny thunk (CALLF prng_step;
 * RET) with no DGROUP symbol of its own; reconstructed inline here rather than as a
 * separate TU.  prng_state0/1/2 + prng_step are owned by globals.c/prng.c.
 */
static u8 p2_engine_rand(void)
{
    prng_step();
    return (u8)prng_state0;
}

/*
 * p2_ai_select_move_a — 1000:4f04
 * --------------------------------------------------------------------------
 * rng/flag-driven selection of move-state 1/2/3, then p2_set_move_state.
 *   rng_frame <  threshold, even -> (dir_blocked_1==0) ? 2 : 3
 *   rng_frame <  threshold, odd  -> (dir_blocked_0==0) ? set_move_state(1) : 3
 *   rng_frame >= threshold       -> 3
 * (the "a" tail-state is 3.)
 */
void p2_ai_select_move_a(void)
{
    u8 state_id;

    if (rng_frame < p2_ai_threshold) {
        if ((rng_frame & 1) == 0) {
            state_id = (p2_dir_blocked_1 == 0) ? 2 : 3;
        } else {
            if (p2_dir_blocked_0 == 0) {
                p2_set_move_state(1);
                return;
            }
            state_id = 3;
        }
    } else {
        state_id = 3;
    }
    p2_set_move_state(state_id);
    return;
}

/*
 * p2_ai_select_move_b — 1000:4f89
 * --------------------------------------------------------------------------
 * rng/flag-driven selection of move-state 1/2/4, then p2_set_move_state.  Identical
 * shape to select_move_a except the tail-state is 4 (instead of 3).
 *   rng_frame <  threshold, even -> (dir_blocked_1==0) ? 2 : 4
 *   rng_frame <  threshold, odd  -> (dir_blocked_0==0) ? set_move_state(1) : 4
 *   rng_frame >= threshold       -> 4
 */
void p2_ai_select_move_b(void)
{
    u8 state_id;

    if (rng_frame < p2_ai_threshold) {
        if ((rng_frame & 1) == 0) {
            state_id = (p2_dir_blocked_1 == 0) ? 2 : 4;
        } else {
            if (p2_dir_blocked_0 == 0) {
                p2_set_move_state(1);
                return;
            }
            state_id = 4;
        }
    } else {
        state_id = 4;
    }
    p2_set_move_state(state_id);
    return;
}

/*
 * p2_choose_move_state1 — 1000:4dfa
 * --------------------------------------------------------------------------
 * Pick the next P2 move favouring state 1: rng parity + the dir-blocked flags
 * (p2_dir_blocked_3 / p2_dir_blocked_2) choose set_move_state(1/3/4).
 *   rng_frame <  threshold, even -> (dir_blocked_3==0) ? 4 : 1
 *   rng_frame <  threshold, odd  -> (dir_blocked_2==0) ? set_move_state(3) : 1
 *   rng_frame >= threshold       -> 1
 */
void p2_choose_move_state1(void)
{
    u8 state_id;

    if (rng_frame < p2_ai_threshold) {
        if ((rng_frame & 1) == 0) {
            state_id = (p2_dir_blocked_3 == 0) ? 4 : 1;
        } else {
            if (p2_dir_blocked_2 == 0) {
                p2_set_move_state(3);
                return;
            }
            state_id = 1;
        }
    } else {
        state_id = 1;
    }
    p2_set_move_state(state_id);
    return;
}

/*
 * p2_choose_move_state2 — 1000:4e7f
 * --------------------------------------------------------------------------
 * Pick the next P2 move favouring state 2: same shape as p2_choose_move_state1 but
 * the favoured state is 2 (instead of 1).
 *   rng_frame <  threshold, even -> (dir_blocked_3==0) ? 4 : 2
 *   rng_frame <  threshold, odd  -> (dir_blocked_2==0) ? set_move_state(3) : 2
 *   rng_frame >= threshold       -> 2
 */
void p2_choose_move_state2(void)
{
    u8 state_id;

    if (rng_frame < p2_ai_threshold) {
        if ((rng_frame & 1) == 0) {
            state_id = (p2_dir_blocked_3 == 0) ? 4 : 2;
        } else {
            if (p2_dir_blocked_2 == 0) {
                p2_set_move_state(3);
                return;
            }
            state_id = 2;
        }
    } else {
        state_id = 2;
    }
    p2_set_move_state(state_id);
    return;
}

/*
 * p2_ai_select_move_random — 1000:4fd3
 * --------------------------------------------------------------------------
 * Pick a fresh random move-state in 5..8 when all four directions are blocked:
 *   base       = rng_frame & 3;          (low 2 bits of the CURRENT rng_frame)
 *   rng_frame  = rand();                 (OVERWRITES rng_frame with the new draw)
 *   move_state = (rng_frame & 1) + base + 5;
 * then p2_set_move_state(move_state).
 *
 * RECONSTRUCTION FIDELITY (AI DETERMINISM): the parity bit (rand()&1) comes from the
 * NEW rng_frame value, which the engine's rand() (1000:93b1) draws from prng_step —
 * NOT from the entry rng_frame.  asm 1000:4fe1-4ff4: AND AL,3 (base) ; CALL rand ;
 * MOV [0x79b3],AL (rng_frame = rand) ; AND AL,1 ; ADD AL,base ; ADD AL,5.  Replaying
 * this deterministically therefore requires the host prng_state0/1/2 to equal the
 * engine's at this call (the T4 capture records them at the AI-random record's ENTRY;
 * see tools/p2_oracle.py + tools/p2_ctest.c §AI-DETERMINISM).
 */
void p2_ai_select_move_random(void)
{
    u8 base_offset;
    u8 state_id;

    base_offset = (u8)(rng_frame & 3);
    rng_frame   = p2_engine_rand();                       /* rng_frame = rand() */
    state_id    = (u8)((rng_frame & 1) + base_offset + 5);
    p2_set_move_state(state_id);
    return;
}

/*
 * p2_ai_dispatch_move — 1000:4f4e
 * --------------------------------------------------------------------------
 * Top P2-AI dispatcher: route to one of four move selectors by the dir-blocked
 * flags, in priority order.
 *   dir_blocked_3==0 -> p2_ai_select_move_b
 *   dir_blocked_1==0 -> p2_choose_move_state2
 *   dir_blocked_0==0 -> p2_choose_move_state1
 *   else             -> p2_ai_select_move_a
 */
void p2_ai_dispatch_move(void)
{
    if (p2_dir_blocked_3 == 0) {
        p2_ai_select_move_b();
    } else if (p2_dir_blocked_1 == 0) {
        p2_choose_move_state2();
    } else if (p2_dir_blocked_0 == 0) {
        p2_choose_move_state1();
    } else {
        p2_ai_select_move_a();
    }
    return;
}

/*
 * p2_pick_move_priority_a — 1000:4dbf
 * --------------------------------------------------------------------------
 * Alternate AI dispatcher: select the next P2 move by the dir-blocked flags in the
 * priority order a0e0, a1b2, a0e2, else (dispatches to the four direction choosers).
 *   dir_blocked_0==0 -> p2_choose_move_state1
 *   dir_blocked_3==0 -> p2_ai_select_move_b
 *   dir_blocked_2==0 -> p2_ai_select_move_a
 *   else             -> p2_choose_move_state2
 */
void p2_pick_move_priority_a(void)
{
    if (p2_dir_blocked_0 == 0) {
        p2_choose_move_state1();
    } else if (p2_dir_blocked_3 == 0) {
        p2_ai_select_move_b();
    } else if (p2_dir_blocked_2 == 0) {
        p2_ai_select_move_a();
    } else {
        p2_choose_move_state2();
    }
    return;
}

/*
 * p2_pick_move_priority_b — 1000:4e44
 * --------------------------------------------------------------------------
 * Alternate AI dispatcher: priority order a0e1, a0e2, a1b2, else.
 *   dir_blocked_1==0 -> p2_choose_move_state2
 *   dir_blocked_2==0 -> p2_ai_select_move_a
 *   dir_blocked_3==0 -> p2_ai_select_move_b
 *   else             -> p2_choose_move_state1
 */
void p2_pick_move_priority_b(void)
{
    if (p2_dir_blocked_1 == 0) {
        p2_choose_move_state2();
    } else if (p2_dir_blocked_2 == 0) {
        p2_ai_select_move_a();
    } else if (p2_dir_blocked_3 == 0) {
        p2_ai_select_move_b();
    } else {
        p2_choose_move_state1();
    }
    return;
}

/*
 * p2_pick_move_priority_c — 1000:4ec9
 * --------------------------------------------------------------------------
 * Alternate AI dispatcher: priority order a0e2, a0e0, a0e1, else.
 *   dir_blocked_2==0 -> p2_ai_select_move_a
 *   dir_blocked_0==0 -> p2_choose_move_state1
 *   dir_blocked_1==0 -> p2_choose_move_state2
 *   else             -> p2_ai_select_move_b
 */
void p2_pick_move_priority_c(void)
{
    if (p2_dir_blocked_2 == 0) {
        p2_ai_select_move_a();
    } else if (p2_dir_blocked_0 == 0) {
        p2_choose_move_state1();
    } else if (p2_dir_blocked_1 == 0) {
        p2_choose_move_state2();
    } else {
        p2_ai_select_move_b();
    }
    return;
}

/*
 * p2_run_move_state_handler — 1000:5003
 * --------------------------------------------------------------------------
 * When p2_step_idx == 5, invoke the per-state move-state handler from the near-ptr
 * handler table at DGROUP 0x85c, indexed by p2_move_state (asm: `MOV BX, state*2 ;
 * CALL word ptr [BX+0x85c]`).  The handlers are the cell-direction moves
 * (p2_cell_move_up/down/left/right).
 *
 * RECONSTRUCTION FIDELITY (indirect handler dispatch): the engine's table holds
 * 16-bit NEAR code pointers in DGROUP; reached here through the p2_state_handler_tbl
 * far shadow (host void(*)(void) callables — see its decl), the index/deref 1:1 with
 * the asm.  No state mutation of its own; the dispatched handler does the work.
 */
void p2_run_move_state_handler(void)
{
    if (p2_step_idx == 5) {
        (*p2_state_handler_tbl[p2_move_state])();          /* (*tbl[state])() @0x85c */
    }
    return;
}

/*
 * p2_cell_move_up — 1000:5025
 * --------------------------------------------------------------------------
 * P2 step up: p2_cell -= 8 (one grid row up); decrement the row counter (0x8565).
 */
void p2_cell_move_up(void)
{
    p2_cell        = (s8)(p2_cell - 8);
    p2_set_cell_row = (u8)(p2_set_cell_row - 1);
    return;
}

/*
 * p2_cell_move_down — 1000:503f
 * --------------------------------------------------------------------------
 * P2 step down: p2_cell += 8 (one grid row down); increment the row counter (0x8565).
 */
void p2_cell_move_down(void)
{
    p2_cell        = (s8)(p2_cell + 8);
    p2_set_cell_row = (u8)(p2_set_cell_row + 1);
    return;
}

/*
 * p2_cell_move_left — 1000:5059
 * --------------------------------------------------------------------------
 * P2 step left: p2_cell -= 1 (one column left); decrement the column counter (0x8564).
 */
void p2_cell_move_left(void)
{
    p2_cell        = (s8)(p2_cell - 1);
    p2_set_cell_col = (u8)(p2_set_cell_col - 1);
    return;
}

/*
 * p2_cell_move_right — 1000:506f
 * --------------------------------------------------------------------------
 * P2 step right: p2_cell += 1 (one column right); increment the column counter (0x8564).
 */
void p2_cell_move_right(void)
{
    p2_cell        = (s8)(p2_cell + 1);
    p2_set_cell_col = (u8)(p2_set_cell_col + 1);
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  P2 RENDER / VIEW WRAPPERS + PVP COLLISION — Phase-4 Task 5 (ported 1:1).
 *
 *  The three render/view fns (draw_p2_sprite, render_p2_view, erase_p2_view) are
 *  THIN game-loop WRAPPERS: each builds/updates an engine descriptor object from the
 *  current P2 state, then issues ONE present/blit call into the already-validated
 *  Phase-0 render machinery.  The descriptor-BUILD (the field writes) is the part
 *  these fns own and is reconstructed here 1:1 from the disassembly; the trailing
 *  present/blit call is the leaf into the render core.
 *
 *  RECONSTRUCTION FIDELITY (the present/blit leaf calls).  The engine's render leaves
 *  are FAR-POINTER calls taking the descriptor's (off,seg):
 *      draw_p2_sprite  -> blit_sprite(0x795a, 0x203b)        (1000:942a)
 *      render_p2_view  -> render_player_view(p2_view off,seg) (1000:93b8)
 *      erase_p2_view   -> restore_bg_view(p2_erase_view off,seg)(1000:80bc)
 *  Phase-0 reconstructed those render leaves as BEHAVIOR-FAITHFUL semantic
 *  reconstructions driven by host WORK BUFFERS (planes/bank + a 3-arg signature; see
 *  src/entity.c §entity_blit_object and src/gfx_overlay.h) — and `blit_sprite` itself
 *  was inlined there into its three validated pipeline stages, so it has no callable
 *  symbol.  Those game-loop wrappers do not hold the work-buffer render context, and
 *  the Phase-0 core must NOT be modified to take the engine's far-ptr convention.
 *  The present/blit LEAF is therefore modeled here as a faithful-signature stub
 *  (p2_blit_sprite_leaf / p2_render_view_leaf / p2_restore_view_leaf) preserving the
 *  call site 1:1; the OBSERVABLE output of draw_p2_sprite — the P2 object descriptor
 *  bytes (x, y, frame) written at p2_sprite/0x9b9e — IS produced here and is the
 *  validated gate (tools/p2_ctest.c §RENDER-DESCRIPTOR, against the plane-exact 24/24
 *  blitter underneath).  render_p2_view/erase_p2_view's descriptor field-writes are
 *  likewise reconstructed 1:1; their present leaf is the stub. */
void p2_blit_sprite_leaf(u16 obj_off, u16 obj_seg);  /* blit_sprite 1000:942a — leaf  */
void p2_render_view_leaf(u8 __far *view);            /* render_player_view 1000:93b8  */
void p2_restore_view_leaf(u8 __far *view);           /* restore_bg_view   1000:80bc   */

/*
 * draw_p2_sprite — 1000:1cea
 * --------------------------------------------------------------------------
 * Build P2's sprite object descriptor at the far ptr p2_sprite (DGROUP 0x9b9e/0x9ba0)
 * from the current P2 state, then blit it.  No-op when P2 is absent (p2_cell == -1).
 *   obj[+0] (word) = p2_pixel_x
 *   obj[+2] (word) = p2_pixel_y
 *   obj[+4] (word) = p2_frame_base + p2_move_anim        (frame index)
 *   blit_sprite(0x795a, 0x203b)                          (present leaf — see note)
 *
 * The field order/offsets are 1:1 with the asm (1000:1cfd LES BX,[0x9b9e];
 * +4 = frame_base+anim, +0 = px, +2 = py; then PUSH DS; PUSH 0x795a; CALL 0x942a).
 * This is the P2 analog of entity_draw_p1's obj field writes (src/entity.c).
 */
void draw_p2_sprite(void)
{
    u8 __far *obj;

    obj = p2_sprite;
    if (p2_cell != (s8)0xff) {
        sprite_obj_t __far *so = (sprite_obj_t __far *)obj;
        so->frame = (u16)(p2_frame_base + p2_move_anim); /* frame */
        so->x     = (s16)p2_pixel_x;                     /* x     */
        so->y     = (s16)p2_pixel_y;                     /* y     */
        p2_blit_sprite_leaf(0x795a, 0x203b);                          /* present leaf */
    }
    return;
}

/*
 * render_p2_view — 1000:1c41
 * --------------------------------------------------------------------------
 * P2 view-copy (save-under): compute the P2 scroll offsets from the P2 grid cell,
 * write the view geometry into the render descriptor at p2_view (DGROUP 0x8ec/0x8ee),
 * then present it via render_player_view.  No-op when P2 is absent (p2_cell == -1).
 *   p2_scroll_x = (p2_grid_x > 0x10) ? 0x14 - p2_grid_x : 4
 *   p2_scroll_y = (p2_grid_y > 0x15) ? 0x19 - p2_grid_y : 4
 *   view[+6]  = p2_grid_x   view[+8]  = p2_grid_y
 *   view[+1e] = p2_scroll_x view[+20] = p2_scroll_y
 *   render_player_view(p2_view off, seg)                 (present leaf — see note)
 *
 * 1:1 with the asm (1000:1c54.. MOV [0x9d34],4 ; CMP [0x8558],0x10 ; ... ;
 * LES BX,[0x8ec] ; ES:[BX+6/8/1e/20] writes ; PUSH [0x8ee],[0x8ec] ; CALL 0x93b8).
 */
void render_p2_view(void)
{
    player_view_geom_t __far *view;

    if (p2_cell != (s8)0xff) {
        p2_scroll_x = 4;
        if (p2_grid_x > 0x10) {
            p2_scroll_x = (s16)(0x14 - p2_grid_x);
        }
        p2_scroll_y = 4;
        if (p2_grid_y > 0x15) {
            p2_scroll_y = (s16)(0x19 - p2_grid_y);
        }

        view = (player_view_geom_t __far *)p2_view;
        view->pos_x    = p2_grid_x;
        view->pos_y    = p2_grid_y;
        view->scroll_x = p2_scroll_x;
        view->scroll_y = p2_scroll_y;
        p2_render_view_leaf(p2_view);                                /* present leaf */
    }
    return;
}

/*
 * erase_p2_view — 1000:19a1
 * --------------------------------------------------------------------------
 * Restore the background under P2's PREVIOUS cell: write the previous-cell grid
 * coords + the current scroll offsets into the erase descriptor at p2_erase_view
 * (DGROUP 0x8e8/0x8ea), then restore via restore_bg_view.  No-op when P2 absent.
 *   view[+14] = p2_grid_x_prev   view[+16] = p2_grid_y_prev
 *   view[+1e] = p2_scroll_x      view[+20] = p2_scroll_y
 *   restore_bg_view(p2_erase_view off, seg)              (present leaf — see note)
 *
 * 1:1 with the asm (1000:19b4 LES BX,[0x8e8] ; ES:[BX+14]=[0x928e] ;
 * ES:[BX+16]=[0x9b94] ; ES:[BX+1e/20]=scroll ; PUSH [0x8ea],[0x8e8] ; CALL 0x80bc).
 */
void erase_p2_view(void)
{
    player_view_geom_t __far *view;

    if (p2_cell != (s8)0xff) {
        view = (player_view_geom_t __far *)p2_erase_view;
        view->prev_x   = p2_grid_x_prev;
        view->prev_y   = p2_grid_y_prev;
        view->scroll_x = p2_scroll_x;
        view->scroll_y = p2_scroll_y;
        p2_restore_view_leaf(p2_erase_view);                         /* restore leaf */
    }
    return;
}

/*
 * update_p2_bbox — 1000:50c0
 * --------------------------------------------------------------------------
 * Recompute P2's AABB (DGROUP 0x854/0x856/0x858/0x85a) from the P2 pixel position,
 * unless physics is frozen.  Half-open box: x in [px-5, px+6], y in [py-5, py+5].
 *   p2_bbox_left   (0x854) = p2_pixel_x - 5   (asm ADD 0xfffb)
 *   p2_bbox_right  (0x856) = p2_pixel_x + 6
 *   p2_bbox_top    (0x858) = p2_pixel_y - 5   (asm ADD 0xfffb)
 *   p2_bbox_bottom (0x85a) = p2_pixel_y + 5
 * 1:1 with the asm (1000:50cc MOV AL,[0xa0ce] gate; the four ADD/MOV writes).
 */
void update_p2_bbox(void)
{
    if (physics_frozen == 0) {
        pvp_p2_bbox.x0 = (s16)(p2_pixel_x - 5);   /* left   0x854 */
        pvp_p2_bbox.x1 = (s16)(p2_pixel_x + 6);   /* right  0x856 */
        pvp_p2_bbox.y0 = (s16)(p2_pixel_y - 5);   /* top    0x858 */
        pvp_p2_bbox.y1 = (s16)(p2_pixel_y + 5);   /* bottom 0x85a */
    }
    return;
}

/*
 * update_p1_bbox — 1000:5085  (Phase-9 T3)
 * --------------------------------------------------------------------------
 * Recompute P1's AABB (DGROUP 0x84c/0x84e/0x850/0x852, owned here as the pvp_p1_*
 * bbox words) from the P1 pixel position, unless physics is frozen.  Mirror of
 * update_p2_bbox (50c0) — same half-open box, on P1's pixel + P1's bbox words.
 *   p1_bbox_left   (0x84c) = p1_pixel_x - 5   (asm ADD 0xfffb)
 *   p1_bbox_right  (0x84e) = p1_pixel_x + 6
 *   p1_bbox_top    (0x850) = p1_pixel_y - 5   (asm ADD 0xfffb)
 *   p1_bbox_bottom (0x852) = p1_pixel_y + 5
 * 1:1 with the asm (1000:5091 MOV AL,[0xa0ce] freeze gate; the four ADD/MOV writes).
 * Lives in player2.c because it writes the pvp P1 bbox words this module owns (the
 * same words check_pvp_collision tests).
 */
void update_p1_bbox(void)
{
    if (physics_frozen == 0) {
        pvp_p1_bbox.x0 = (s16)(p1_pixel_x - 5);   /* left   0x84c */
        pvp_p1_bbox.x1 = (s16)(p1_pixel_x + 6);   /* right  0x84e */
        pvp_p1_bbox.y0 = (s16)(p1_pixel_y - 5);   /* top    0x850 */
        pvp_p1_bbox.y1 = (s16)(p1_pixel_y + 5);   /* bottom 0x852 */
    }
    return;
}

/*
 * check_pvp_collision — 1000:50fb
 * --------------------------------------------------------------------------
 * P1<->P2 AABB overlap test: set pvp_collision_flag (DGROUP 0xa1aa) to 0/1.  Gated
 * to active two-player play: P2 present (p2_cell != -1), not physics-frozen, not
 * session-continue, and game_mode != '0' (0x30).  On overlap, play a contact sound
 * (id 0x0d when sound_device_state==4, else 0x03).
 *
 * Separating-axis test (1:1 with the asm 1000:5127..; AABBs are p1 0x84c/0x84e/
 * 0x850/0x852 = left/right/top/bottom, p2 0x854/0x856/0x858/0x85a likewise):
 *   if p2_left  > p1_right  -> 0   (asm: AX=[0x854]; CMP AX,[0x84e]; JLE ...)
 *   if p1_left  > p2_right  -> 0
 *   if p2_top   > p1_bottom -> 0
 *   if p1_top   > p2_bottom -> 0
 *   else                    -> 1   (+ play_sound)
 *
 * RECONSTRUCTION FIDELITY (sound leaf): play_sound (1000:6e11) is the audio-subsystem
 * leaf, stubbed project-wide (game_stubs.c / items.c convention; → Phase 6).  The
 * collision-flag output — the validated gate — is produced 1:1 here.
 */
void check_pvp_collision(void)
{
    u8 sound_id;

    if (p2_cell != (s8)0xff && physics_frozen == 0 &&
        session_continue_flag == 0 && game_mode != 0x30) {
        if (pvp_p1_bbox.x1 < pvp_p2_bbox.x0) {              /* p1_right < p2_left */
            pvp_collision_flag = 0;
        } else if (pvp_p2_bbox.x1 < pvp_p1_bbox.x0) {       /* p2_right < p1_left */
            pvp_collision_flag = 0;
        } else if (pvp_p1_bbox.y1 < pvp_p2_bbox.y0) {       /* p1_bottom < p2_top */
            pvp_collision_flag = 0;
        } else if (pvp_p2_bbox.y1 < pvp_p1_bbox.y0) {       /* p2_bottom < p1_top */
            pvp_collision_flag = 0;
        } else {
            pvp_collision_flag = 1;
            if (sound_device_state == 4) {
                sound_id = 0x0d;
            } else {
                sound_id = 3;
            }
            play_sound(sound_id);                 /* 1000:6e11 (stub leaf — see note) */
        }
    }
    return;
}

/* ── Present/blit leaf stubs — the render-core call sites (see the FIDELITY note at
 *    the head of the T5 section).  Faithful-signature no-ops: they preserve the
 *    wrapper call sites 1:1 without re-driving the Phase-0 work-buffer render core. */
/* Default build: NOP stubs.  Under -dBUMPY_PLAYABLE the real bodies come from
 * src/host/host_render.c (blit leaf → validated blitter into the host framebuffer). */
#ifndef BUMPY_PLAYABLE
void p2_blit_sprite_leaf(u16 obj_off, u16 obj_seg)
{
    (void)obj_off; (void)obj_seg;
    return;
}
void p2_render_view_leaf(u8 __far *view)
{
    (void)view;
    return;
}
void p2_restore_view_leaf(u8 __far *view)
{
    (void)view;
    return;
}
#endif /* !BUMPY_PLAYABLE */
