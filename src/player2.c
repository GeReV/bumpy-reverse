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
   it SPLIT as two words (docs/06-engine.md); here a single u16 __far * carries it,
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
u8  pvp_collision_flag;  /* DGROUP 0xa1aa */
s16 pvp_p1_x0;           /* DGROUP 0x084c */
s16 pvp_p1_x1;           /* DGROUP 0x084e */
s16 pvp_p1_y0;           /* DGROUP 0x0850 */
s16 pvp_p1_y1;           /* DGROUP 0x0852 */
s16 pvp_p2_x0;           /* DGROUP 0x0854 */
s16 pvp_p2_x1;           /* DGROUP 0x0856 */
s16 pvp_p2_y0;           /* DGROUP 0x0858 */
s16 pvp_p2_y1;           /* DGROUP 0x085a */

/* ── cross-module globals the P2 move-state fns read (owned elsewhere) ────────── */
extern u8 __far *tilemap;     /* game.c 0xa0d8/0xa0da — level tilemap far pointer */

/* ════════════════════════════════════════════════════════════════════════════
 *  STUBBED CALLEES (RECONSTRUCTION FIDELITY)
 *  p2_tile_move_check (below) dispatches to three not-yet-ported callees:
 *    - p2_run_move_state_handler (1000:5003) — per-state cell-move handler dispatch
 *      (routes move_state through the near-ptr handler table at DGROUP 0x85c).  → T4.
 *    - p2_ai_select_move_random (1000:4fd3) — picks a fresh random move-state when
 *      all four directions are blocked; calls rand()/prng_step.  → T4 (its prng
 *      determinism is the open AI-replay item, see tools/p2_ctest.c §AI-DETERMINISM).
 *    - the per-state move-state handler at handler-table 0x870 (the `(*tbl[state])()`
 *      indirect call) — the P2 move-state handlers.  → T4.
 *  All three remain faithful-signature stubs in game_stubs.c this task; the harness
 *  exercises p2_tile_move_check via records that do not cross into those callees
 *  (the captured tilemap path), so the un-ported callees do not perturb the gate.
 *  The handler-table indirect call is modelled below as a forward-declared dispatch
 *  helper (p2_dispatch_move_state_handler) so player2.c links without the table data.
 * ════════════════════════════════════════════════════════════════════════════ */
void p2_run_move_state_handler(void);     /* 1000:5003 — stub (game_stubs.c), → T4 */
void p2_ai_select_move_random(void);      /* 1000:4fd3 — stub (game_stubs.c), → T4 */

/* Indirect move-state handler dispatch: the engine calls
 *   (*(void(**)(void))(0x870 + p2_move_state*2))()
 * a near-ptr table of per-state move-state handlers at DGROUP 0x870.  Those
 * handlers are the AI move-state group (→ T4); the table data is not yet
 * reconstructed.  RECONSTRUCTION FIDELITY: modelled as a stubbed dispatch helper so
 * the indirect call site in p2_tile_move_check is preserved 1:1 without inventing
 * the (deferred) handler-table bytes.  → T4. */
void p2_dispatch_move_state_handler(void);/* DGROUP 0x870[move_state] — stub, → T4 */

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
        s16 ox = *(s16 __far *)(p2_sprite + 0x14);        /* sprite origin x */
        s16 oy = *(s16 __far *)(p2_sprite + 0x16);        /* sprite origin y */

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
            p2_run_move_state_handler();                  /* → T4 stub */
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
            if (p2_set_cell_col != 0 && tilemap[(u16)cell + 0x2f] == 0) {
                p2_dir_blocked_2 = 0;
                if (tilemap[(u16)cell - 1] == 0x0b) {
                    p2_dir_blocked_2 = 1;
                }
            }
            if (p2_set_cell_col != 7 && tilemap[(u16)cell + 0x30] == 0) {
                p2_dir_blocked_3 = 0;
                if (tilemap[(u16)cell + 1] == 0x0b) {
                    p2_dir_blocked_3 = 1;
                }
            }

            if ((u8)(p2_dir_blocked_0 + p2_dir_blocked_1 +
                     p2_dir_blocked_2 + p2_dir_blocked_3) == 4) {
                p2_ai_select_move_random();               /* → T4 stub */
            } else {
                p2_dispatch_move_state_handler();         /* (*0x870[state])() → T4 */
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
