/*
 * player.c — P1 MOVE-EXECUTION SPINE (Phase 1, Task 6a)
 *
 * Faithful 1:1 decompilation of the four core "spine" functions of Bumpy's player
 * movement state machine, ported from the Ghidra decomp of BUMPY_unpacked.exe and
 * verified live against Ghidra MCP (decompile + disassembly, 2026-06).
 *
 * This is DECOMPILATION, not reimplementation (CLAUDE.md leading tenet): one C
 * function per original function, the same control flow, the same data layout, so
 * the C reads as documentation of what the binary does.  Each function cites its
 * engine address (segment 1000 code; DGROUP is segment 203b in the original).
 *
 * SCOPE (Task 6a) — ONLY these four:
 *   p1_step_scripted_move   1000:13df   (primary, validated by tools/player_ctest.c)
 *   enter_game_mode         1000:4263
 *   p1_movement_dispatch    1000:1e02
 *   dispatch_move_step      1000:238e
 * The gamemode_* handlers, the dispatch-table DATA, and tile collision are TASK 6b
 * (forward-declared extern in player.h; their bodies are NOT here).
 *
 * Build (object compile check; player.c is NOT linked into BUMPY.EXE this task):
 *   wcc -ml -bt=dos -zq -wx src/player.c
 */

#include "player.h"

/* ── DGROUP globals owned by this module ────────────────────────────────────── */

s16        p1_pixel_x;             /* 203b:0x9290 */
s16        p1_pixel_y;             /* 203b:0x9292 */
u8         p1_move_anim;           /* 203b:0x824a */
u8         game_mode;              /* 203b:0x792c */
u8         prev_game_mode;         /* 203b:0x8552 */
u8         p1_move_step_idx;       /* 203b:0x792a */
u8         move_locked;            /* 203b:0x8242 */
u8         p1_move_steps_left;     /* 203b:0x824d */
u8         p1_facing_left;         /* 203b:0x9bae */
u8         p1_move_anim_frame_idx; /* 203b:0xa0dc */
u8         p1_queued_action_code;  /* 203b:0x7923 (byte store; see player.h) */
u8         physics_frozen;         /* 203b:0xa0ce */
u8         move_override;          /* 203b:0xa1a7 (DAT_a1a7) */
u16 __far *p1_move_script;         /* 203b:0xa1ac (off) / 0xa1ae (seg) */

/*
 * p1_step_scripted_move — 1000:13df
 * --------------------------------------------------------------------------
 * THE move-step executor.  Advances one step of P1's current scripted move:
 * applies the (dx,dy) of the current [anim,dx,dy] 6-byte script entry to
 * p1_pixel_x/y (dx negated when facing left), latches the anim value, advances
 * the script pointer by one entry and decrements steps-left.  Returns 0 when the
 * move finishes (and resets p1_move_step_idx), else the new step index.
 *
 * GUARD (does nothing, returns move_locked): if move_locked != 0, or
 * p1_move_steps_left == 0, or game_mode in {0x05, 0x0b, 0x1c}.
 *
 * Ghidra body (verified via MCP) used near-32-bit pointer arithmetic; here the
 * far script pointer is a `u16 __far *`, so script[0]=anim, script[1]=dx,
 * script[2]=dy and "+= 6 bytes" == "+= 3 words".
 *
 * RECONSTRUCTION FIDELITY: the engine's per-step advance writes ONLY the offset
 * word of the far pointer (asm 1000:1450 `ADD word ptr [0xa1ac],0x6`; the
 * segment word at 0xa1ae is left unchanged).  The decomp models this as
 * CONCAT22(seg, off+3).  Pointer-add on a single `u16 __far *` here mutates the
 * offset and preserves the segment, so the behaviour is identical for any script
 * that does not cross a 64 KiB segment boundary (the engine's scripts never do).
 */
char p1_step_scripted_move(void)
{
    char  step_result;
    int   dx;

    step_result = (char)move_locked;
    if (move_locked == 0 && p1_move_steps_left != 0 &&
        game_mode != 0x05 && game_mode != 0x0b && game_mode != 0x1c) {

        p1_move_anim = (u8)p1_move_script[0];          /* anim = script[0] */

        if (p1_facing_left == 0) {
            dx = (int)(s16)p1_move_script[1];          /* facing right: +dx */
        } else {
            dx = -(int)(s16)p1_move_script[1];         /* facing left:  -dx */
        }
        p1_pixel_x = (s16)(p1_pixel_x + dx);
        p1_pixel_y = (s16)(p1_pixel_y + (s16)p1_move_script[2]);

        p1_move_script += 3;                           /* advance one 6-byte entry */
        p1_move_steps_left = (u8)(p1_move_steps_left - 1);

        if (p1_move_steps_left == 0) {
            step_result = 0;
            p1_move_step_idx = 0;
        } else {
            p1_move_step_idx = (u8)(p1_move_step_idx + 1);
            step_result = (char)p1_move_step_idx;
        }
    }
    return step_result;
}

/*
 * enter_game_mode — 1000:4263
 * --------------------------------------------------------------------------
 * The central movement-state transition.  Always clears input_state.  Unless
 * move_locked, sets game_mode = mode; then for modes NOT in {0x05,0x0b,0x1c}
 * resets p1_move_anim_frame_idx and loads that mode's 4-byte far-pointer entry
 * from mode_script_tbl (DGROUP 0x2252, indexed by mode) — the pointed-to script
 * supplies p1_move_steps_left (script[0]), p1_facing_left (script[1]) and the
 * [anim,dx,dy] pointer p1_move_script (script[2..5] = off,seg).
 *
 * The far-ptr table read mirrors the engine exactly: the entry at
 * 0x2252 + mode*4 is (offset, segment); the loaded script far pointer is built
 * from (script+2 .. script+5).  mode_script_tbl is forward-declared (Task 6b);
 * the engine populates it at runtime, so its contents are not available here.
 */
void enter_game_mode(u8 mode)
{
    u16          tbl_off;
    u16          tbl_seg;
    const u8 __far *script;

    input_state = 0;
    if (move_locked == 0) {
        game_mode = mode;
        if (mode != 0x05 && mode != 0x0b && mode != 0x1c) {
            p1_move_anim_frame_idx = 0;

            /* mode_script_tbl[mode] : 4-byte far pointer (off @ +0, seg @ +2). */
            tbl_off = *(u16 *)(mode_script_tbl + (u16)mode * 4 + 0);
            tbl_seg = *(u16 *)(mode_script_tbl + (u16)mode * 4 + 2);
            script  = (const u8 __far *)MK_FP(tbl_seg, tbl_off);

            p1_move_steps_left = script[0];                       /* steps  = script[0] */
            p1_facing_left     = script[1];                       /* facing = script[1] */
            /* p1_move_script far ptr = (script+2 .. script+5) = (off, seg). */
            p1_move_script = (u16 __far *)MK_FP(*(u16 __far *)(script + 4),
                                                *(u16 __far *)(script + 2));
        }
    }
    return;
}

/*
 * p1_movement_dispatch — 1000:1e02
 * --------------------------------------------------------------------------
 * The P1 movement/physics dispatcher (the gameplay state machine entry).  Clears
 * the queued-action code, saves game_mode into prev_game_mode, then: if NOT
 * physics_frozen AND move_override (DAT_a1a7) is set, runs move_settle (1000:27de);
 * otherwise dispatches through the game-mode handler jump table
 * game_mode_handlers (DGROUP 0x7ca) indexed by game_mode.
 *
 * The handlers are forward-declared (Task 6b).  Frozen-until-first-input note:
 * at puzzle start Bumpy stays frozen because game_mode sits at its idle value and
 * the idle handler does nothing until the first input drives a real
 * enter_game_mode(mode)/move; that behaviour lives in the (deferred) idle handler
 * and the guards above, not in this spine function — it falls out of the decomp.
 */
void p1_movement_dispatch(void)
{
    p1_queued_action_code = 0;
    prev_game_mode = game_mode;
    if (physics_frozen == 0 && move_override != 0) {
        move_settle();
    } else {
        game_mode_handlers[game_mode]();
    }
    return;
}

/*
 * dispatch_move_step — 1000:238e
 * --------------------------------------------------------------------------
 * Continues the current move sequence by calling
 * move_step_dispatch_tbl[game_mode][p1_move_step_idx] — a 2D near-pointer table
 * at DGROUP 0x43c0, per-mode stride 0x22 BYTES (0x11 word entries), indexed by
 * [game_mode] then [p1_move_step_idx].  Called at the tail of each move handler.
 *
 * The engine computes the slot as a near offset:
 *     game_mode * 0x22 + p1_move_step_idx * 2 + 0x43c0
 * then calls the near function pointer stored there.  The table contents are
 * forward-declared (Task 6b); only the index arithmetic is ported here.
 */
void dispatch_move_step(void)
{
    void (**slot)(void);

    slot = (void (**)(void))(move_step_dispatch_tbl +
                             (u16)game_mode * 0x22 + (u16)p1_move_step_idx * 2);
    (*slot)();
    return;
}
