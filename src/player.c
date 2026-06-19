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
 * SCOPE (Task 6a) — the four spine functions:
 *   p1_step_scripted_move   1000:13df   (primary, validated by tools/player_ctest.c)
 *   enter_game_mode         1000:4263
 *   p1_movement_dispatch    1000:1e02
 *   dispatch_move_step      1000:238e
 *
 * SCOPE (Task 6b — added below the spine, "═ TASK 6b ═" banner):
 *   the minimal level-1 idle/walk/start/move game-mode HANDLER set (slice_model
 *   §4.2), ported 1:1, plus the two dispatch-table DATA structures
 *   (game_mode_handlers @ 0x7ca, move_step_dispatch_tbl @ 0x43c0) reconstructed
 *   from the real static bytes dumped from the unpacked image.  The TILE-COLLISION
 *   leaves the handlers call, and the out-of-scope handler-table targets, are
 *   FORWARD-DECLARED (player.h) → Task 6c.
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
 *
 * RECONSTRUCTION FIDELITY (near-vs-far table access): the original reads the
 * table-entry words mode_script_tbl[mode*4 + 0/2] as NEAR accesses into DGROUP
 * (the table lives in DS=DGROUP), but reads the *pointed-to* script bytes
 * (script[0], script[1], script+2..5) through the FAR pointer it just built.
 * We mirror that split exactly: the two table words use a near `u16 *` read of
 * the `mode_script_tbl` blob, while `script` is a `const u8 __far *`.  Behaviour
 * is identical; the type split documents which access is near vs far.
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

/* ════════════════════════════════════════════════════════════════════════════
 *  TASK 6b — GAME-MODE HANDLER STATE MACHINE  +  the two dispatch-table data
 *  structures.  All handlers ported 1:1 from the Ghidra decomp (verified live via
 *  Ghidra MCP, 2026-06; bodies also in local/build/slice_decomp.txt).  Each cites
 *  its engine address.  The tile-collision leaves they call are forward-declared
 *  in player.h (→ Task 6c).
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── DGROUP game-state globals owned by this module (Task 6b) ────────────────── */

u8  p1_contact_code;     /* contact/landing code resolved per move */
u8  move_step_count;     /* jump_step_counter — steps in current move sequence */
u8  p1_jump_move_ticks;  /* jump/jet tick flag consulted by the idle handler */
u8  p1_pending_action;   /* pending tile/move action (from p1_read_tile_under) */
u8  rng_frame;           /* per-frame RNG byte (move_down random-direction branch) */
u8  p1_cell;             /* 203b:0x856e — P1 grid cell */
u8  p1_cell_prev;        /* saved previous cell */
u8  tile_below_player;   /* tile under player (move_settle sets 0xb) */
u8  p1_current_tile;     /* tile probed by move_left/right teleport check */
s16 sound_device_state;  /* DGROUP 0x689c (ram0x00026c4c): -0x8000 == no sound; 4 == OPL/charger */

/* ── Landing-leaf state (Task 3) ──────────────────────────────────────────────
 * Globals introduced by the two landing leaves (land_on_tile_below /
 * check_tile_below_ladder_or_land). */
u8  anim_target_cell;    /* DGROUP 0x856f — cell-8 view/anim relocation target */
u8  p1_latched_action;   /* DGROUP — latched action index into the land-sound tables */

/*
 * game_mode_handlers — DGROUP 0x7ca  (the jump table p1_movement_dispatch indexes)
 * --------------------------------------------------------------------------
 * REAL static data: the 64 near-pointer entries dumped verbatim from the unpacked
 * image (BUMPY_unpacked.exe, DGROUP file base 0x11440 → table at file 0x11c0a).
 * Confirmed ARRAY BOUND = 64 (0x40): indices 0x00..0x3f hold valid handler near
 * offsets, [0x40] = 0x0000 (the end marker), so Task 6a's [64] guess is CORRECT.
 *
 * index→handler map (verbatim from the dump):
 *   default fill (most indices) ........ gamemode_default_idle   (0x28f9)
 *   0x03, 0x0f ......................... gamemode_03_move        (0x23b6)
 *   0x21 ............................... gamemode_21_start       (0x1e5e)
 *   0x22 ............................... gamemode_22             (0x1e90)
 *   0x23 ............................... gamemode_23_walk        (0x1ec2)
 *   0x24 ............................... gamemode_24_walk        (0x1f3e)
 *   0x25 ............................... gamemode_25_contact     (0x2138)
 *   0x26 ............................... gamemode_26_contact     (0x21e7)
 *   OUT-OF-SCOPE targets (→ T6c, forward-declared in player.h):
 *   0x05 move_walk_right_anim_step(0x2423) 0x0a enter_mode_0b_jump_start(0x2470)
 *   0x0b move_anim_step_to_mode0c(0x248e)  0x0c move_step_check_walkable(0x24d7)
 *   0x0d move_step_dispatch_input(0x250a)  0x0e teleport_to_next_exit_tile(0x25ad)
 *   0x10,0x2c FUN_1000_22b0(0x22b0)        0x1c p1_input_dispatch_bit10(0x4344)
 *   0x1d..0x20 FUN_1000_4437(0x4437)       0x2d FUN_1000_22c1(0x22c1)
 *   0x2e advance_physics_freeze(0x22d2)    0x2f land_on_tile_below(0x2810)
 *   0x30 FUN_1000_1e3d(0x1e3d)
 *
 * RECONSTRUCTION FIDELITY: the engine stores 2-byte NEAR code offsets (the table
 * is in the same code segment as the handlers).  We reconstruct it as a C array
 * of function pointers keyed exactly by the dumped index→offset map, so the
 * structure and routing are 1:1.  This is structure-reconstructed, not a raw
 * byte blob, because near offsets are not host-relocatable; the dump above is the
 * ground truth.  The out-of-scope entries point at their real (forward-declared)
 * T6c handlers so the table is structurally complete.
 */
void (*game_mode_handlers[64])(void) = {
    /* 0x00 */ gamemode_default_idle,
    /* 0x01 */ gamemode_default_idle,
    /* 0x02 */ gamemode_default_idle,
    /* 0x03 */ gamemode_03_move,
    /* 0x04 */ gamemode_default_idle,
    /* 0x05 */ move_walk_right_anim_step,      /* → T6c */
    /* 0x06 */ gamemode_default_idle,
    /* 0x07 */ gamemode_default_idle,
    /* 0x08 */ gamemode_default_idle,
    /* 0x09 */ gamemode_default_idle,
    /* 0x0a */ enter_mode_0b_jump_start,       /* → T6c */
    /* 0x0b */ move_anim_step_to_mode0c,       /* → T6c */
    /* 0x0c */ move_step_check_walkable,       /* → T6c */
    /* 0x0d */ move_step_dispatch_input,       /* → T6c */
    /* 0x0e */ teleport_to_next_exit_tile,     /* → T6c */
    /* 0x0f */ gamemode_03_move,
    /* 0x10 */ FUN_1000_22b0,                  /* → T6c */
    /* 0x11 */ gamemode_default_idle,
    /* 0x12 */ gamemode_default_idle,
    /* 0x13 */ gamemode_default_idle,
    /* 0x14 */ gamemode_default_idle,
    /* 0x15 */ gamemode_default_idle,
    /* 0x16 */ gamemode_default_idle,
    /* 0x17 */ gamemode_default_idle,
    /* 0x18 */ gamemode_default_idle,
    /* 0x19 */ gamemode_default_idle,
    /* 0x1a */ gamemode_default_idle,
    /* 0x1b */ gamemode_default_idle,
    /* 0x1c */ p1_input_dispatch_bit10,        /* → T6c */
    /* 0x1d */ FUN_1000_4437,                  /* → T6c */
    /* 0x1e */ FUN_1000_4437,                  /* → T6c */
    /* 0x1f */ FUN_1000_4437,                  /* → T6c */
    /* 0x20 */ FUN_1000_4437,                  /* → T6c */
    /* 0x21 */ gamemode_21_start,
    /* 0x22 */ gamemode_22,
    /* 0x23 */ gamemode_23_walk,
    /* 0x24 */ gamemode_24_walk,
    /* 0x25 */ gamemode_25_contact,
    /* 0x26 */ gamemode_26_contact,
    /* 0x27 */ gamemode_default_idle,
    /* 0x28 */ gamemode_default_idle,
    /* 0x29 */ gamemode_default_idle,
    /* 0x2a */ gamemode_default_idle,
    /* 0x2b */ gamemode_default_idle,
    /* 0x2c */ FUN_1000_22b0,                  /* → T6c */
    /* 0x2d */ FUN_1000_22c1,                  /* → T6c */
    /* 0x2e */ advance_physics_freeze,         /* → T6c */
    /* 0x2f */ land_on_tile_below,             /* → T6c */
    /* 0x30 */ FUN_1000_1e3d,                  /* → T6c */
    /* 0x31 */ gamemode_default_idle,
    /* 0x32 */ gamemode_default_idle,
    /* 0x33 */ gamemode_default_idle,
    /* 0x34 */ gamemode_default_idle,
    /* 0x35 */ gamemode_default_idle,
    /* 0x36 */ gamemode_default_idle,
    /* 0x37 */ gamemode_default_idle,
    /* 0x38 */ gamemode_default_idle,
    /* 0x39 */ gamemode_default_idle,
    /* 0x3a */ gamemode_default_idle,
    /* 0x3b */ gamemode_default_idle,
    /* 0x3c */ gamemode_default_idle,
    /* 0x3d */ gamemode_default_idle,
    /* 0x3e */ gamemode_default_idle,
    /* 0x3f */ gamemode_default_idle
};

/*
 * move_step_dispatch_tbl — DGROUP 0x43c0  (the 2D table dispatch_move_step indexes)
 * --------------------------------------------------------------------------
 * REAL static data: the per-mode rows of 2-byte near step-function pointers,
 * stride 0x22 bytes (0x11 word entries) per mode, dumped verbatim from the
 * unpacked image (file base 0x11440 → table at file 0x15800).  dispatch_move_step
 * (above) indexes it as move_step_dispatch_tbl[game_mode*0x22 + step_idx*2].
 *
 * Modeled as a BYTE BLOB holding the real dumped near offsets (little-endian), so
 * the index arithmetic in dispatch_move_step lands on the exact original entry.
 * Every entry points at a per-step micro-handler (anim/tile step leaf, e.g.
 * 0x6648/0x6717/0x654e; common filler 0x7111) — deeper than the SCOPE of the
 * spine/handler/cell-resolution layers.  We keep the real bytes here so the table
 * is faithful; the micro-handler targets they encode are a later task.
 *
 * RECONSTRUCTION FIDELITY: a host build cannot turn these 16-bit near offsets into
 * real function pointers (no relocation), so unlike game_mode_handlers this table
 * is kept as its literal dumped bytes rather than a typed pointer array.
 *
 * TASK 6c — TABLE BOUND CORRECTED TO THE FULL 0x40 MODES.  Task 6a/6b reproduced
 * only modes 0x00..0x11 under the assumption "the rest are all-0x7111 filler / out
 * of slice scope".  A full dump of the unpacked image disproves that: rows
 * 0x12..0x3f are REAL populated step-handler rows (the very modes the 6c walk/move
 * resolvers — move_left/right_step_resolve, p1_resolve_walk_{left,right}_contact —
 * enter, e.g. 0x12/0x14/0x18/0x1a/0x34..0x3b, all of which then dispatch_move_step).
 * The bound is now 0x40 modes * 0x22 = 0x880 bytes, dumped byte-exact from
 * BUMPY_unpacked.exe (DGROUP 0x43c0 → file 0x15800).  Rows 0x00..0x11 are identical
 * to the 6b values (verified); 0x12..0x3f are the newly-included real rows.
 */
#define MV(w)  (u8)((w) & 0xff), (u8)((w) >> 8)   /* little-endian near offset */
u8 move_step_dispatch_tbl[0x40 * 0x22] = {
    /* mode 0x00 */ MV(0x6648),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x01 */ MV(0x6699),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6326),MV(0x651c),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x6627),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x02 */ MV(0x66d8),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6372),MV(0x6535),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x6627),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x03 */ MV(0x673a),MV(0x7111),MV(0x7111),MV(0x64e2),MV(0x6611),MV(0x7111),MV(0x6627),MV(0x7111),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x04 */ MV(0x64ff),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x7111),MV(0x6627),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x05 */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x06 */ MV(0x673a),MV(0x6611),MV(0x7111),MV(0x6717),MV(0x647e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x07 */ MV(0x673a),MV(0x6611),MV(0x7111),MV(0x7111),MV(0x6717),MV(0x647e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x08 */ MV(0x6748),MV(0x7111),MV(0x6326),MV(0x651c),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x7111),MV(0x6627),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x09 */ MV(0x6789),MV(0x7111),MV(0x6372),MV(0x6535),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x7111),MV(0x6627),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x0a */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x0b */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x0c */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x0d */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x0e */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x0f */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x6627),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x10 */ MV(0x6305),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x11 */ MV(0x6648),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x654e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x12 */ MV(0x6699),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x640c),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x13 */ MV(0x66d8),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x640c),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x14 */ MV(0x6748),MV(0x7111),MV(0x7111),MV(0x640c),MV(0x7111),MV(0x7111),MV(0x654e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x15 */ MV(0x6789),MV(0x7111),MV(0x7111),MV(0x640c),MV(0x7111),MV(0x7111),MV(0x654e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x16 */ MV(0x6699),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6326),MV(0x7111),MV(0x645d),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x7111),
    /* mode 0x17 */ MV(0x66d8),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6372),MV(0x7111),MV(0x645d),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x7111),
    /* mode 0x18 */ MV(0x6748),MV(0x7111),MV(0x6326),MV(0x7111),MV(0x645d),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x654e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x7111),
    /* mode 0x19 */ MV(0x6789),MV(0x7111),MV(0x6372),MV(0x7111),MV(0x645d),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x654e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x7111),
    /* mode 0x1a */ MV(0x65d2),MV(0x6326),MV(0x651c),MV(0x68fe),MV(0x6627),MV(0x651c),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x6627),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x1b */ MV(0x65d2),MV(0x6372),MV(0x6535),MV(0x693a),MV(0x6627),MV(0x6535),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x6627),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x1c */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x1d */ MV(0x64e2),MV(0x6717),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6627),MV(0x65e5),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x1e */ MV(0x64ff),MV(0x6717),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6627),MV(0x65e5),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x1f */ MV(0x651c),MV(0x7111),MV(0x6717),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6627),MV(0x65e5),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x20 */ MV(0x6535),MV(0x7111),MV(0x6717),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6627),MV(0x65e5),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x21 */ MV(0x6699),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x64c1),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x22 */ MV(0x66d8),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x64c1),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x23 */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x24 */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x25 */ MV(0x67e2),MV(0x6627),MV(0x651c),MV(0x65d2),MV(0x7111),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x26 */ MV(0x6813),MV(0x6627),MV(0x6535),MV(0x65d2),MV(0x7111),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x27 */ MV(0x67ca),MV(0x6627),MV(0x640c),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65d2),MV(0x6326),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x28 */ MV(0x67fb),MV(0x6627),MV(0x640c),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65d2),MV(0x6372),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x29 */ MV(0x6832),MV(0x6627),MV(0x7111),MV(0x645d),MV(0x6326),MV(0x7111),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65d2),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x2a */ MV(0x684b),MV(0x6627),MV(0x7111),MV(0x645d),MV(0x6372),MV(0x7111),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65d2),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x2b */ MV(0x673a),MV(0x6611),MV(0x7111),MV(0x6717),MV(0x647e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x2c */ MV(0x6305),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x2d */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x2e */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x2f */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x30 */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x31 */ MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x32 */ MV(0x7111),MV(0x6627),MV(0x7111),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x33 */ MV(0x7111),MV(0x6627),MV(0x7111),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x34 */ MV(0x65d2),MV(0x6326),MV(0x651c),MV(0x68e6),MV(0x6627),MV(0x7111),MV(0x640c),MV(0x6326),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x7111),MV(0x7111),
    /* mode 0x35 */ MV(0x65d2),MV(0x6372),MV(0x6535),MV(0x6922),MV(0x6627),MV(0x7111),MV(0x640c),MV(0x6372),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x7111),MV(0x7111),
    /* mode 0x36 */ MV(0x65d2),MV(0x6326),MV(0x651c),MV(0x68e6),MV(0x6627),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x645d),MV(0x7111),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x7111),MV(0x7111),MV(0x65fb),
    /* mode 0x37 */ MV(0x65d2),MV(0x6372),MV(0x6535),MV(0x6922),MV(0x6627),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x645d),MV(0x7111),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x7111),MV(0x7111),MV(0x65fb),
    /* mode 0x38 */ MV(0x6890),MV(0x6326),MV(0x640c),MV(0x6535),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x6627),MV(0x7111),MV(0x65fb),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x39 */ MV(0x68bb),MV(0x6372),MV(0x640c),MV(0x651c),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x6627),MV(0x7111),MV(0x65fb),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x3a */ MV(0x6890),MV(0x6326),MV(0x65d2),MV(0x7111),MV(0x645d),MV(0x6535),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x6627),MV(0x7111),MV(0x65fb),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x3b */ MV(0x68bb),MV(0x6372),MV(0x65d2),MV(0x7111),MV(0x645d),MV(0x651c),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x65e5),MV(0x6627),MV(0x7111),MV(0x65fb),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),
    /* mode 0x3c */ MV(0x6648),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x3d */ MV(0x6648),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x3e */ MV(0x6648),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000),
    /* mode 0x3f */ MV(0x6648),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x6717),MV(0x654e),MV(0x6587),MV(0x7111),MV(0x7111),MV(0x7111),MV(0x65fb),MV(0x0000),MV(0x0000),MV(0x0000)
};
#undef MV

/* ── SCOPE handlers — ported 1:1 from the decomp ──────────────────────────────
 *
 * Note on the FUN_1000_ab83() stack-check prologue: every original function opens
 * with `if (stack_check_limit <= &stack0xfffe) FUN_1000_ab83();` — Turbo C's
 * stack-overflow probe.  It is a compiler-emitted runtime guard, NOT game logic;
 * it is intentionally OMITTED from the ports (documented once here) so the C reads
 * as the actual behaviour.  This is the same convention the 6a spine used.
 */

/*
 * gamemode_default_idle — 1000:28f9   (game_mode 0/default — idle)
 * --------------------------------------------------------------------------
 * The idle handler.  This is where Bumpy stays FROZEN-until-first-input at puzzle
 * start: with no pending action and no jump ticks, it falls (cell<0x28) or settles
 * (do_move_with_sound).  Once p1_read_tile_under latches a p1_pending_action from
 * input, it routes to enter_mode_1c_walk / move_input_tick / handle_move_input.
 */
void gamemode_default_idle(void)
{
    u8 sound_id;

    move_step_count = 8;                              /* jump_step_counter = 8 */
    if (p1_jump_move_ticks == 0 && p1_pending_action != 0) {
        if (p1_pending_action == 0x20) {
            if (sound_device_state == 4) {
                sound_id = 0x28;
            } else {
                sound_id = 3;
            }
            play_sound(sound_id);
        }
        if (p1_pending_action == 0x16) {
            enter_mode_1c_walk();
        } else if (p1_pending_action == 0x03) {
            move_input_tick();
        } else {
            handle_move_input();
        }
    } else {
        p1_jump_move_ticks = 0;
        if (p1_cell < 0x28) {
            enter_mode_04_fall();
        } else {
            do_move_with_sound();
        }
    }
    return;
}

/*
 * gamemode_21_start — 1000:1e5e   (game_mode 0x21 — start/launch, right)
 * --------------------------------------------------------------------------
 * On contact (p1_contact_code==8) play a sound and run gamemode_26_contact; else
 * transition to game_mode 0x24 (idle/walk-left tick).
 */
void gamemode_21_start(void)
{
    u8 sound_id;

    if (p1_contact_code == 8) {
        if (sound_device_state == 4) {
            sound_id = 0x2b;
        } else {
            sound_id = 0x0f;
        }
        play_sound(sound_id);
        gamemode_26_contact();
    } else {
        game_mode = 0x24;
    }
    return;
}

/*
 * gamemode_22 — 1000:1e90   (game_mode 0x22 — start/launch, left; sibling of 0x21)
 * --------------------------------------------------------------------------
 * On contact play a sound and run gamemode_25_contact; else transition to
 * game_mode 0x23 (idle/walk-right tick).
 */
void gamemode_22(void)
{
    u8 sound_id;

    if (p1_contact_code == 8) {
        if (sound_device_state == 4) {
            sound_id = 0x2b;
        } else {
            sound_id = 0x0f;
        }
        play_sound(sound_id);
        gamemode_25_contact();
    } else {
        game_mode = 0x23;
    }
    return;
}

/*
 * gamemode_23_walk — 1000:1ec2   (game_mode 0x23 — idle/walk-right tick)
 * --------------------------------------------------------------------------
 * Advance the walk animation (step_walk_anim, script 203b:1ca4); while right is
 * held (input_state&2) begin a rightward walk.
 */
void gamemode_23_walk(void)
{
    step_walk_anim(0x0b, 5, 0x1ca4, 0x203b);
    if ((input_state & 2) != 0) {
        p1_begin_walk_right();
    }
    return;
}

/*
 * gamemode_24_walk — 1000:1f3e   (game_mode 0x24 — idle/walk-left tick)
 * --------------------------------------------------------------------------
 * Advance the walk animation (step_walk_anim, script 203b:1cba); while left is
 * held (input_state&2) begin a leftward walk.
 */
void gamemode_24_walk(void)
{
    step_walk_anim(0x0b, 5, 0x1cba, 0x203b);
    if ((input_state & 2) != 0) {
        p1_begin_walk_left();
    }
    return;
}

/*
 * gamemode_03_move — 1000:23b6   (game_mode 0x03 / 0x0f — mid-move tick)
 * --------------------------------------------------------------------------
 * Branch on input: left(4)→move_left; right(8)→move_right; else if down(2) is held
 * AND the tile one row up isn't 0x0e, move_down (+sound); else move_settle.
 *
 * The `*(char*)(tilemap + p1_cell - 8) != 0x0e` probe READS the level tilemap — a
 * tile leaf — but it is part of this handler's own control-flow condition, so the
 * skeleton is ported here with the `tilemap` far pointer forward-declared (→ T6c).
 */
void gamemode_03_move(void)
{
    if ((input_state & 4) == 0) {
        if ((input_state & 8) == 0) {
            if ((p1_cell < 8 || tilemap[(u16)p1_cell - 8] != 0x0e) &&
                (input_state & 2) != 0) {
                move_down();
                if (sound_device_state == 4) {
                    play_sound(0x2a);
                } else {
                    play_sound(0x14);
                }
            } else {
                move_settle();
            }
        } else {
            move_right();
        }
    } else {
        move_left();
    }
    return;
}

/*
 * gamemode_25_contact — 1000:2138   (game_mode 0x25 — P1 left-walk contact)
 * --------------------------------------------------------------------------
 * Clears contact.  With up/fire held (input_state&0x12), play sound + enter mode
 * 0x32.  Else at step 0 force contact 0x1f and mode 0x27; otherwise probe cell-1
 * terrain (read_tile_layer_contact, a tile leaf → T6c) and route via
 * contact_transition_tbl_b[p1_contact_code] (0x25 → p1_enter_walk_left_mode).
 * Always ends with dispatch_move_step.
 */
void gamemode_25_contact(void)
{
    u8 sound_id;
    u8 next_mode;

    p1_contact_code = 0;
    if ((input_state & 0x12) == 0) {
        if (move_step_count == 0) {
            p1_contact_code = 0x1f;
            enter_game_mode(0x27);
        } else {
            p1_cell_prev = (u8)(p1_cell + 0xff);             /* cell - 1 */
            read_tile_layer_contact(p1_cell_prev);
            next_mode = contact_transition_tbl_b[p1_contact_code];
            if (next_mode == 0x25) {
                p1_enter_walk_left_mode();
            } else {
                enter_game_mode(next_mode);
            }
        }
    } else {
        if (sound_device_state == 4) {
            sound_id = 0x2a;
        } else {
            sound_id = 0x15;
        }
        play_sound(sound_id);
        enter_game_mode(0x32);
    }
    dispatch_move_step();
    return;
}

/*
 * gamemode_26_contact — 1000:21e7   (game_mode 0x26 — P1 right-walk contact)
 * --------------------------------------------------------------------------
 * With up/fire held (input_state&0x12), play sound + enter mode 0x33.  Else if the
 * step counter==7 force contact 0x1f and enter mode 0x28; otherwise save cell,
 * probe terrain (read_tile_layer_contact → T6c) and route via
 * contact_transition_tbl[p1_contact_code] (0x42f6) — 0x26 → p1_enter_walk_right_mode.
 * Always ends with dispatch_move_step.
 */
void gamemode_26_contact(void)
{
    u8 sound_id;
    u8 next_mode;

    if ((input_state & 0x12) == 0) {
        if (move_step_count == 7) {
            p1_contact_code = 0x1f;
            enter_game_mode(0x28);
        } else {
            p1_cell_prev = p1_cell;
            read_tile_layer_contact(p1_cell);
            next_mode = contact_transition_tbl[p1_contact_code];
            if (next_mode == 0x26) {
                p1_enter_walk_right_mode();
            } else {
                enter_game_mode(next_mode);
            }
        }
    } else {
        if (sound_device_state == 4) {
            sound_id = 0x2a;
        } else {
            sound_id = 0x15;
        }
        play_sound(sound_id);
        enter_game_mode(0x33);
    }
    dispatch_move_step();
    return;
}

/*
 * p1_begin_walk_right — 1000:1f03
 * --------------------------------------------------------------------------
 * Begin rightward walk: game_mode=1, load move script 203b:140c (4 steps),
 * facing_left=9, step_idx=9, save cell, apply contact action 0x16, dispatch step.
 *
 * RECONSTRUCTION FIDELITY: the decomp writes the far script pointer field-by-field
 * (p1_move_script._0_2_ = 0x140c; ._2_2_ = 0x203b).  6a models p1_move_script as a
 * `u16 __far *`, so we rebuild it with MK_FP(seg, off) — same stored value.
 * Note facing_left/step_idx are set to 9 (the engine's literal), not 0/1.
 */
void p1_begin_walk_right(void)
{
    game_mode = 1;
    p1_move_script = (u16 __far *)MK_FP(0x203b, 0x140c);
    p1_move_steps_left = 4;
    p1_facing_left = 9;
    p1_move_step_idx = 9;
    p1_cell_prev = p1_cell;
    apply_contact_action(0x16);
    dispatch_move_step();
    return;
}

/*
 * p1_begin_walk_left — 1000:1f7f
 * --------------------------------------------------------------------------
 * Begin leftward walk: game_mode=2, load move script 203b:1460 (4 steps),
 * facing_left=0, step_idx=9, prev cell = cell-1, apply contact action 0x16,
 * dispatch step.  (Same far-ptr fidelity note as p1_begin_walk_right.)
 */
void p1_begin_walk_left(void)
{
    game_mode = 2;
    p1_move_script = (u16 __far *)MK_FP(0x203b, 0x1460);
    p1_move_steps_left = 4;
    p1_move_step_idx = 9;
    p1_facing_left = 0;
    p1_cell_prev = (u8)(p1_cell - 1);
    apply_contact_action(0x16);
    dispatch_move_step();
    return;
}

/*
 * move_left — 1000:2634
 * --------------------------------------------------------------------------
 * Resolve a one-cell left move.  Clears contact, plays the action sound; at step 0
 * forces contact 0x1f / mode 0x12, else decrements the cell, resolves the
 * collision via contact_action_tbl_left[contact_code]; for blocked code 1 probes
 * teleport tile 0x0b (mode 0x16 vs 1).  Enters the resolved mode + dispatches.
 *
 * read_tile_layer_contact / read_tile_at_cell are tile leaves (→ T6c).
 */
void move_left(void)
{
    u8 mode;
    u8 resolved_mode;

    p1_contact_code = 0;
    play_action_sound();
    if (move_step_count == 0) {
        p1_contact_code = 0x1f;
        mode = 0x12;
    } else {
        p1_cell_prev = (u8)(p1_cell + 0xff);                 /* cell - 1 */
        read_tile_layer_contact(p1_cell_prev);
        resolved_mode = contact_action_tbl_left[p1_contact_code];
        mode = resolved_mode;
        if (resolved_mode == 1) {
            read_tile_at_cell(p1_cell_prev);
            if (p1_current_tile == 0x0b) {
                mode = 0x16;
            } else {
                mode = 1;
            }
        }
    }
    enter_game_mode(mode);
    dispatch_move_step();
    return;
}

/*
 * move_right — 1000:26a1
 * --------------------------------------------------------------------------
 * Resolve a one-cell right move.  At step 7 forces contact 0x1f / mode 0x13, else
 * advances the cell, resolves via collision_mode_table_right[contact_code]; for
 * blocked code 2 probes teleport tile 0x0b at cell+1 (mode 0x17 vs 2).  Enters the
 * resolved mode + dispatches.  Tile leaves → T6c.
 */
void move_right(void)
{
    u8 mode;
    u8 resolved_mode;

    p1_contact_code = 0;
    play_action_sound();
    if (move_step_count == 7) {
        p1_contact_code = 0x1f;
        mode = 0x13;
    } else {
        p1_cell_prev = p1_cell;
        read_tile_layer_contact(p1_cell);
        resolved_mode = collision_mode_table_right[p1_contact_code];
        mode = resolved_mode;
        if (resolved_mode == 2) {
            read_tile_at_cell((u8)(p1_cell + 1));
            if (p1_current_tile == 0x0b) {
                mode = 0x17;
            } else {
                mode = 2;
            }
        }
    }
    enter_game_mode(mode);
    dispatch_move_step();
    return;
}

/*
 * move_settle — 1000:27de   (also dispatched by p1_movement_dispatch on override)
 * --------------------------------------------------------------------------
 * Clears the queued action + override, sets tile_below_player=0xb, then on pending
 * action 0x11 enters mode 0x2f else lands (land_on_tile_below → T6c); dispatches.
 */
void move_settle(void)
{
    p1_queued_action_code = 0;
    move_override = 0;
    tile_below_player = 0x0b;
    if (p1_pending_action == 0x11) {
        enter_game_mode(0x2f);
    } else {
        land_on_tile_below();
    }
    dispatch_move_step();
    return;
}

/*
 * enter_mode_04_fall — 1000:28e0
 * --------------------------------------------------------------------------
 * Enter game mode 4 (fall) and dispatch the first move step.
 */
void enter_mode_04_fall(void)
{
    enter_game_mode(4);
    dispatch_move_step();
    return;
}

/*
 * enter_mode_1c_walk — 1000:4305
 * --------------------------------------------------------------------------
 * Set game_mode=0x1c and play the default walk animation (play_walk_anim_default
 * @ 1000:4361 → T6c).
 */
void enter_mode_1c_walk(void)
{
    game_mode = 0x1c;
    play_walk_anim_default();
    return;
}

/*
 * move_input_tick — 1000:463d
 * --------------------------------------------------------------------------
 * Per-frame throttle: every 3rd call clears move_locked and runs handle_move_input
 * (movement is processed once per 3 frames).
 *
 * RECONSTRUCTION FIDELITY: move_locked here doubles as the 0..2 frame counter (the
 * engine reuses the move_locked byte); that is the original's behaviour, ported
 * verbatim.
 */
void move_input_tick(void)
{
    move_locked = (u8)(move_locked + 1);
    if (move_locked == 3) {
        move_locked = 0;
        handle_move_input();
    }
    return;
}

/*
 * do_move_with_sound — 1000:42d9   (game_mode 0x2d move — seen latched in trace)
 * --------------------------------------------------------------------------
 * Play the move sound (0xd if sound_device_state==4 else 3), enter mode 0x2d,
 * dispatch the move step.
 */
void do_move_with_sound(void)
{
    u8 sound_id;

    if (sound_device_state == 4) {
        sound_id = 0x0d;
    } else {
        sound_id = 3;
    }
    play_sound(sound_id);
    enter_game_mode(0x2d);
    dispatch_move_step();
    return;
}

/*
 * move_down — 1000:4747
 * --------------------------------------------------------------------------
 * Pick a move action from the tile below the player (tilemap[cell-8] indexed into
 * the action table at 0x374e); if none, fall back to a randomized down-direction
 * action keyed off rng_frame; then p1_begin_move(action).
 *
 * RECONSTRUCTION FIDELITY: the engine reads the tile-below action through two raw
 * near tables — `tilemap[cell-8]` (the level tilemap, a tile leaf → T6c) and the
 * fixed action LUT at DGROUP 0x374e.  The tilemap read is kept via the
 * forward-declared `tilemap` far pointer; the 0x374e LUT is a constant table not
 * yet reconstructed, so it is read through a forward-declared extern blob
 * (`down_action_lut` → T6c).  Control flow + the rng_frame thresholds are 1:1.
 */
void move_down(void)
{
    u8 move_action;
    u8 tile_below;

    if (p1_cell < 8) {
        move_action = 0;
    } else {
        tile_below = tilemap[(u16)p1_cell - 8];
        move_action = down_action_lut[tile_below];
    }
    if (move_action == 0) {
        if (rng_frame < 0xec) {
            if (rng_frame < 0xd8) {
                if (rng_frame < 0xc4) {
                    if (0xaf < rng_frame) {
                        move_action = 0x3f;
                    }
                } else {
                    move_action = 0x3e;
                }
            } else {
                move_action = 0x3d;
            }
        } else {
            move_action = 0x3c;
        }
    }
    p1_begin_move(move_action);
    return;
}

/*
 * handle_move_input — 1000:2965   (the input→player move entry deferred from T5)
 * --------------------------------------------------------------------------
 * Final left/right/down dispatch from idle: left(4)→p1_move_left; right(8)→
 * p1_move_right; else branch on the pending tile action — 0x0a→p1_handle_move_input,
 * 0x0f→FUN_1000_4802, else check_tile_below_ladder_or_land.  All of the called
 * leaves READ the tilemap to resolve the move → Task 6c.
 */
void handle_move_input(void)
{
    if ((input_state & 4) == 0) {
        if ((input_state & 8) == 0) {
            if (p1_pending_action == 0x0a) {
                p1_handle_move_input();
            } else if (p1_pending_action == 0x0f) {
                FUN_1000_4802();
            } else {
                check_tile_below_ladder_or_land();
            }
        } else {
            p1_move_right();
        }
    } else {
        p1_move_left();
    }
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  TASK 6c — TILE-COLLISION (CELL-RESOLUTION) LAYER
 *  ----------------------------------------------------------------------------
 *  The tilemap-reading "tile leaves" the Task-6b handlers call, ported 1:1 from
 *  the Ghidra decomp (verified live via MCP, 2026-06; bodies also in
 *  local/build/slice_decomp.txt).  Each cites its engine address.  Plus the
 *  contact/collision/action DATA tables, dumped byte-exact from the unpacked
 *  image (BUMPY_unpacked.exe, DGROUP file base 0x11440 → file_off = 0x11440 +
 *  dgroup_off).
 *
 *  SCOPE — the cell-resolution closure: read_tile_layer_contact, read_tile_at_cell,
 *  p1_enter_walk_{right,left}_mode, p1_begin_move, p1_move_{left,right},
 *  p1_handle_move_input, exec_move_action, move_left_step_resolve,
 *  move_right_step_resolve_alt, p1_resolve_walk_{left,right}_contact.  These read
 *  the tilemap, index a collision table, and enter_game_mode — they do NOT recurse
 *  into the mode menagerie.  The leaves PAST this layer (land_on_tile_below,
 *  check_tile_below_ladder_or_land — animation-channel + FX-table dependent) stay
 *  forward-declared in player.h (→ Task 7), per the 6c STOP-AND-SPLIT rule.
 *
 *  STATE OWNERSHIP: every global these leaves touch (p1_contact_code, p1_current_tile,
 *  p1_cell, p1_cell_prev, p1_pending_action, move_step_count, game_mode,
 *  sound_device_state) is ALREADY defined/declared by Task 6a/6b above; this layer
 *  is leaf-only and introduces NO new state global.  The cross-module globals it
 *  reads (tilemap = level data) stay extern (player.h).  The data tables below are
 *  the only new symbols, and they are player-physics-local (DEFINED here).
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── Contact/collision/action DATA tables (DUMPED-REAL) ────────────────────────
 * Reconstructed as typed C arrays from the verbatim bytes of BUMPY_unpacked.exe.
 * Each line cites its DGROUP offset + the source file offset (0x11440+dgroup).
 * Indices 0x14..0x3f are 0x00 in the engine (zero-fill) and reproduced as such.
 *
 * RECONSTRUCTION FIDELITY: the engine indexes these as raw near byte tables in
 * DGROUP (e.g. `*(byte*)(p1_contact_code + 0x4256)`); we model each as a fixed
 * `u8[N]` holding the exact dumped bytes, so `tbl[contact_code]` lands on the same
 * byte the engine reads.  These are real static data, dumped — not invented. */

/* contact_action_tbl_left @ DGROUP 0x4256 (file 0x15696) — move_left resolved mode. */
u8 contact_action_tbl_left[0x40] = {
    0x01,0x12,0x01,0x01,0x01,0x12,0x01,0x12,0x21,0x12,0x12,0x12,0x01,0x21,0x12,0x12,
    0x12,0x12,0x12,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* collision_mode_table_right @ DGROUP 0x4276 (file 0x156b6) — move_right resolved mode. */
u8 collision_mode_table_right[0x40] = {
    0x02,0x13,0x02,0x02,0x02,0x13,0x13,0x02,0x22,0x13,0x13,0x13,0x02,0x22,0x13,0x13,
    0x13,0x13,0x13,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* collision_mode_table_left @ DGROUP 0x4296 (file 0x156d6) — move_left_step_resolve. */
u8 collision_mode_table_left[0x40] = {
    0x08,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x08,0x14,0x14,0x14,
    0x14,0x14,0x14,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* collision_mode_table_right_alt @ DGROUP 0x42b6 (file 0x156f6) — move_right_step_resolve_alt. */
u8 collision_mode_table_right_alt[0x40] = {
    0x09,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x09,0x15,0x15,0x15,
    0x15,0x15,0x15,0x09,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* contact_transition_tbl_b @ DGROUP 0x42d6 (file 0x15716) — gamemode_25 left next mode. */
u8 contact_transition_tbl_b[0x40] = {
    0x25,0x27,0x25,0x25,0x25,0x27,0x25,0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x27,
    0x27,0x27,0x27,0x25,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* contact_transition_tbl @ DGROUP 0x42f6 (file 0x15736) — gamemode_26 right next mode. */
u8 contact_transition_tbl[0x40] = {
    0x26,0x28,0x26,0x26,0x26,0x28,0x28,0x26,0x28,0x28,0x28,0x28,0x28,0x28,0x28,0x28,
    0x28,0x28,0x28,0x26,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* left_walk_contact_tbl_34 @ DGROUP 0x4316 (file 0x15756) — p1_resolve_walk_left_contact (==0x34). */
u8 left_walk_contact_tbl_34[0x40] = {
    0x1a,0x34,0x1a,0x1a,0x1a,0x34,0x1a,0x34,0x34,0x34,0x34,0x34,0x1a,0x34,0x34,0x34,
    0x34,0x34,0x34,0x1a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* right_walk_contact_tbl_35 @ DGROUP 0x4336 (file 0x15776) — p1_resolve_walk_right_contact (==0x35). */
u8 right_walk_contact_tbl_35[0x40] = {
    0x1b,0x35,0x1b,0x1b,0x1b,0x35,0x35,0x1b,0x35,0x35,0x35,0x35,0x1b,0x35,0x35,0x35,
    0x35,0x35,0x35,0x1b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* left_walk_contact_tbl_38 @ DGROUP 0x4356 (file 0x15796) — p1_resolve_walk_left_contact (==0x38). */
u8 left_walk_contact_tbl_38[0x40] = {
    0x1a,0x38,0x38,0x38,0x38,0x38,0x38,0x38,0x38,0x38,0x38,0x38,0x1a,0x38,0x38,0x38,
    0x38,0x38,0x38,0x1a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* right_walk_contact_tbl_39 @ DGROUP 0x4376 (file 0x157b6) — p1_resolve_walk_right_contact (==0x39). */
u8 right_walk_contact_tbl_39[0x40] = {
    0x1b,0x39,0x39,0x39,0x39,0x39,0x39,0x39,0x39,0x39,0x39,0x39,0x1b,0x39,0x39,0x39,
    0x39,0x39,0x39,0x1b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* action_tbl_left @ DGROUP 0x36ee (file 0x14b2e) — p1_move_left: pending-action -> move action. */
u8 action_tbl_left[0x30] = {
    0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x01,0x01,0x01,0x01,0x00,0x01,0x01,0x01,0x01,
    0x01,0x01,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x01,0x01,0x01,0x01,0x01,0x10,
    0x30,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* action_tbl_right @ DGROUP 0x371e (file 0x14b5e) — p1_move_right: pending-action -> move action. */
u8 action_tbl_right[0x30] = {
    0x02,0x02,0x02,0x02,0x02,0x01,0x02,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x02,
    0x02,0x02,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x02,0x02,0x02,0x02,0x02,0x10,
    0x30,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* down_action_lut @ DGROUP 0x374e (file 0x14b8e) — move_down: tile-below -> move action. */
u8 down_action_lut[0x30] = {
    0x00,0x00,0x00,0x11,0x00,0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x11,0x11,0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x11,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* action_tbl_default @ DGROUP 0x377e (file 0x14bbe) — p1_handle_move_input: game_mode -> action. */
u8 action_tbl_default[0x40] = {
    0x00,0x08,0x09,0x00,0x00,0x00,0x00,0x00,0x08,0x09,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x09,0x08,0x09,0x08,0x09,0x08,0x09,0x08,0x08,0x09,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x09,0x08,0x09,0x08,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x09,0x08,0x09,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* ── Tile leaves (cell-resolution), ported 1:1 ────────────────────────────────
 * Stack-check prologue OMITTED (same convention as 6a/6b — compiler-emitted guard,
 * not game logic). */

/*
 * read_tile_layer_contact — 1000:6bd4
 * --------------------------------------------------------------------------
 * Read the +0x30 tilemap "contact" layer at `cell` into p1_contact_code.  The
 * engine reads tilemap[cell + 0x30] as a near byte (DS=DGROUP-relative add to the
 * tilemap far base); here tilemap is a `u8 __far *` so the +0x30 offset is applied
 * to the far pointer.  This is the primary terrain-contact probe the move/walk
 * resolvers call before indexing a collision table.
 */
void read_tile_layer_contact(u8 cell)
{
    p1_contact_code = tilemap[(u16)cell + 0x30];
    return;
}

/*
 * read_tile_at_cell — 1000:6bb5
 * --------------------------------------------------------------------------
 * Read tilemap[cell] (the base layer) into p1_current_tile — the "tile under the
 * given cell" probe used for the teleport-tile (0x0b) checks.
 */
void read_tile_at_cell(u8 cell)
{
    p1_current_tile = tilemap[(u16)cell];
    return;
}

/*
 * p1_enter_walk_right_mode — 1000:2261
 * --------------------------------------------------------------------------
 * Probe the tile at cell+1: if it is the special teleport tile 0x0b enter mode
 * 0x2a, else mode 0x26 (right-walk contact).
 */
void p1_enter_walk_right_mode(void)
{
    u8 mode;

    read_tile_at_cell((u8)(p1_cell + 1));
    if (p1_current_tile == 0x0b) {
        mode = 0x2a;
    } else {
        mode = 0x26;
    }
    enter_game_mode(mode);
    return;
}

/*
 * p1_enter_walk_left_mode — 1000:21bb
 * --------------------------------------------------------------------------
 * Probe the tile at cell-1: if it is the special teleport tile 0x0b enter mode
 * 0x29, else mode 0x25 (left-walk contact).
 */
void p1_enter_walk_left_mode(void)
{
    u8 mode;

    read_tile_at_cell((u8)(p1_cell + 0xff));          /* cell - 1 */
    if (p1_current_tile == 0x0b) {
        mode = 0x29;
    } else {
        mode = 0x25;
    }
    enter_game_mode(mode);
    return;
}

/*
 * p1_begin_move — 1000:472d
 * --------------------------------------------------------------------------
 * Enter the given game/animation mode and dispatch the first move step.
 */
void p1_begin_move(u8 mode)
{
    enter_game_mode(mode);
    dispatch_move_step();
    return;
}

/*
 * exec_move_action — 1000:46bb
 * --------------------------------------------------------------------------
 * The action -> move dispatch.  Maps a resolved action code to its move handler:
 *   0 move_down, 1 move_left, 2 move_right, 3 move_settle, 8 move_left_step_resolve,
 *   9 move_right_step_resolve_alt, 0x1a p1_resolve_walk_left_contact,
 *   0x1b p1_resolve_walk_right_contact; any other code -> p1_begin_move(action).
 *
 * RECONSTRUCTION FIDELITY: the decomp expresses the low band (0..3) as a `switch`
 * inside an `if (action < 9)` and the special codes (8/9/0x1a/0x1b) as early
 * returns; the control flow is mirrored exactly, including action==8 being handled
 * before the `< 9` switch (the engine special-cases 8 first).
 */
void exec_move_action(u8 action)
{
    if (action == 8) {
        move_left_step_resolve();
        return;
    }
    if (action < 9) {
        switch (action) {
        case 0:
            move_down();
            break;
        case 1:
            move_left();
            break;
        case 2:
            move_right();
            break;
        case 3:
            move_settle();
            break;
        default:
            p1_begin_move(action);
            break;
        }
    } else {
        if (action == 9) {
            move_right_step_resolve_alt();
            return;
        }
        if (action == 0x1a) {
            p1_resolve_walk_left_contact();
            return;
        }
        if (action == 0x1b) {
            p1_resolve_walk_right_contact();
            return;
        }
        p1_begin_move(action);
    }
    return;
}

/*
 * p1_move_left — 1000:467d
 * --------------------------------------------------------------------------
 * Map p1_pending_action through action_tbl_left (DGROUP 0x36ee) and run
 * exec_move_action with the result.
 */
void p1_move_left(void)
{
    exec_move_action(action_tbl_left[p1_pending_action]);
    return;
}

/*
 * p1_move_right — 1000:469c
 * --------------------------------------------------------------------------
 * Map p1_pending_action through action_tbl_right (DGROUP 0x371e) and run
 * exec_move_action with the result.
 */
void p1_move_right(void)
{
    exec_move_action(action_tbl_right[p1_pending_action]);
    return;
}

/*
 * p1_handle_move_input — 1000:47cb
 * --------------------------------------------------------------------------
 * Dispatch P1 movement from input_state: left(4) -> move_left; right(8) ->
 * move_right; else exec_move_action(action_tbl_default[game_mode]) (DGROUP 0x377e).
 * (This is the leaf handle_move_input calls when p1_pending_action == 0x0a.)
 */
void p1_handle_move_input(void)
{
    if ((input_state & 4) == 0) {
        if ((input_state & 8) == 0) {
            exec_move_action(action_tbl_default[game_mode]);
        } else {
            move_right();
        }
    } else {
        move_left();
    }
    return;
}

/*
 * move_left_step_resolve — 1000:270c
 * --------------------------------------------------------------------------
 * Resolve a one-cell left STEP (the exec_move_action action==8 variant).  At step
 * 0 forces contact 0x1f / mode 0x14, else decrements the cell, resolves via
 * collision_mode_table_left[contact_code]; for blocked code 8 probes teleport tile
 * 0x0b (mode 0x18 vs 8).  Enters the resolved mode + dispatches.  (Sibling of the
 * 6b move_left, with the alternate table + step-0/block constants.)
 */
void move_left_step_resolve(void)
{
    u8 mode;
    u8 resolved_mode;

    p1_contact_code = 0;
    if (move_step_count == 0) {
        p1_contact_code = 0x1f;
        mode = 0x14;
    } else {
        p1_cell_prev = (u8)(p1_cell + 0xff);              /* cell - 1 */
        read_tile_layer_contact(p1_cell_prev);
        resolved_mode = collision_mode_table_left[p1_contact_code];
        mode = resolved_mode;
        if (resolved_mode == 8) {
            read_tile_at_cell(p1_cell_prev);
            if (p1_current_tile == 0x0b) {
                mode = 0x18;
            } else {
                mode = 8;
            }
        }
    }
    enter_game_mode(mode);
    dispatch_move_step();
    return;
}

/*
 * move_right_step_resolve_alt — 1000:2776
 * --------------------------------------------------------------------------
 * Resolve a one-cell right STEP (the exec_move_action action==9 variant).  At step
 * 7 forces contact 0x1f / mode 0x15, else advances the cell, resolves via
 * collision_mode_table_right_alt[contact_code]; for blocked code 9 probes teleport
 * tile 0x0b at cell+1 (mode 0x19 vs 9).  Enters the resolved mode + dispatches.
 */
void move_right_step_resolve_alt(void)
{
    u8 mode;
    u8 resolved_mode;

    p1_contact_code = 0;
    if (move_step_count == 7) {
        p1_contact_code = 0x1f;
        mode = 0x15;
    } else {
        p1_cell_prev = p1_cell;
        read_tile_layer_contact(p1_cell);
        resolved_mode = collision_mode_table_right_alt[p1_contact_code];
        mode = resolved_mode;
        if (resolved_mode == 9) {
            read_tile_at_cell((u8)(p1_cell + 1));
            if (p1_current_tile == 0x0b) {
                mode = 0x19;
            } else {
                mode = 9;
            }
        }
    }
    enter_game_mode(mode);
    dispatch_move_step();
    return;
}

/*
 * p1_resolve_walk_left_contact — 1000:1fbe
 * --------------------------------------------------------------------------
 * Resolve a leftward walk step.  Plays the move sound; then walks the cell-1 /
 * cell-2 terrain (read_tile_layer_contact / read_tile_at_cell) against the
 * walk-contact tables (left_walk_contact_tbl_38 @ 0x4356, left_walk_contact_tbl_34
 * @ 0x4316) and the step counter to pick the resulting game_mode
 * (0x1a/0x34/0x36/0x38/0x3a), then enters it and dispatches.  The mode constants
 * compared against the tables are the literal asm values (0x38 / 0x34).
 */
void p1_resolve_walk_left_contact(void)
{
    u8 mode;

    p1_contact_code = 0;
    if (sound_device_state == 4) {
        mode = 0x26;
    } else {
        mode = 2;
    }
    play_sound(mode);
    if (move_step_count == 0) {
        p1_contact_code = 0x1f;
        mode = 0x38;
    } else {
        p1_cell_prev = (u8)(p1_cell + 0xff);              /* cell - 1 */
        read_tile_layer_contact(p1_cell_prev);
        if (left_walk_contact_tbl_38[p1_contact_code] == 0x38) {
            mode = 0x38;
        } else {
            read_tile_at_cell((u8)(p1_cell + 0xff));
            if (p1_current_tile == 0x0b) {
                mode = 0x3a;
            } else if (move_step_count == 1) {
                p1_contact_code = 0x1f;
                mode = 0x34;
            } else {
                p1_cell_prev = (u8)(p1_cell + 0xfe);      /* cell - 2 */
                read_tile_layer_contact(p1_cell_prev);
                if (left_walk_contact_tbl_34[p1_contact_code] == 0x34) {
                    mode = 0x34;
                } else {
                    read_tile_at_cell((u8)(p1_cell + 0xfe));
                    if (p1_current_tile == 0x0b) {
                        mode = 0x36;
                    } else {
                        mode = 0x1a;
                    }
                }
            }
        }
    }
    enter_game_mode(mode);
    dispatch_move_step();
    return;
}

/*
 * p1_resolve_walk_right_contact — 1000:207d
 * --------------------------------------------------------------------------
 * Resolve a rightward walk step (mirror of p1_resolve_walk_left_contact).  Probes
 * cell+1 / cell+2 terrain against the walk-contact tables (right_walk_contact_tbl_39
 * @ 0x4376, right_walk_contact_tbl_35 @ 0x4336) and the step counter to pick the
 * resulting game_mode (0x1b/0x35/0x37/0x39/0x3b), then enters it and dispatches.
 */
void p1_resolve_walk_right_contact(void)
{
    u8 mode;

    p1_contact_code = 0;
    if (sound_device_state == 4) {
        mode = 0x26;
    } else {
        mode = 2;
    }
    play_sound(mode);
    if (move_step_count == 7) {
        p1_contact_code = 0x1f;
        mode = 0x39;
    } else {
        p1_cell_prev = p1_cell;
        read_tile_layer_contact(p1_cell);
        if (right_walk_contact_tbl_39[p1_contact_code] == 0x39) {
            mode = 0x39;
        } else {
            read_tile_at_cell((u8)(p1_cell + 1));
            if (p1_current_tile == 0x0b) {
                mode = 0x3b;
            } else if (move_step_count == 6) {
                p1_contact_code = 0x1f;
                mode = 0x35;
            } else {
                p1_cell_prev = (u8)(p1_cell + 1);
                read_tile_layer_contact(p1_cell_prev);
                if (right_walk_contact_tbl_35[p1_contact_code] == 0x35) {
                    mode = 0x35;
                } else {
                    read_tile_at_cell((u8)(p1_cell + 2));
                    if (p1_current_tile == 0x0b) {
                        mode = 0x37;
                    } else {
                        mode = 0x1b;
                    }
                }
            }
        }
    }
    enter_game_mode(mode);
    dispatch_move_step();
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  TASK 3 (Phase 2) — THE TWO LANDING / COLLISION LEAVES
 *  ----------------------------------------------------------------------------
 *  land_on_tile_below (1000:2810) and check_tile_below_ladder_or_land (1000:29a6),
 *  ported 1:1 from the Ghidra decomp (verified live via MCP, 2026-06).  Task 6c
 *  deferred these because they reach the animation-channel / FX allocator
 *  apply_cell_animation (1000:69aa); their PHYSICS / mode-transition body is ported
 *  here, with the FX/sound callees + the two move-step delegates kept as extern
 *  stubs (player.h / game_stubs.c → Phase 4/5/6).
 *
 *  RECONSTRUCTION FIDELITY (stubbed callees): the leaves' faithful control flow is
 *  reproduced 1:1, but four callees they reach are OUT OF SCOPE for this task and
 *  remain extern stubs (each documented at its declaration in player.h):
 *    - apply_cell_animation (1000:69aa) — the anim-channel / FX allocator (→ Phase 5/6).
 *    - p1_exec_pending_action (1000:465e) — the move-step substate delegate
 *      (pending-action LUT 0x36be → exec_move_action); a Task-4 substate (→ T4).
 *    - move_down_step (1000:253f) — the downward move-step substate (→ T4).
 *  These do NOT change px/py/move_anim; the mode TRANSITIONS that ARE in-leaf (the
 *  cell<8 / land-table / ladder branches) are ported faithfully.  The per-fn
 *  validation gate checks the physics globals only, so the stubbed FX/sound bodies
 *  are irrelevant to it — but every in-leaf state/mode transition is 1:1.
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── Landing DATA tables (DUMPED-REAL from BUMPY_unpacked.exe) ──────────────────
 * Reconstructed byte-exact from the unpacked image (DGROUP base 0x11440 →
 * file_off = 0x11440 + dgroup_off), like the Task-6c contact/collision tables. */

/* land_mode_fx_tbl @ DGROUP 0x76a (file 0x11baa) — land_on_tile_below indexes it by
 * tile_below_player as 2-byte [mode, fx] pairs: land_mode = tbl[tile*2],
 * land_fx_code = tbl[tile*2 + 1].  0x30 tile entries (0..0x2f) = 0x60 bytes (the
 * blob ends exactly where game_mode_handlers @ 0x7ca begins). */
u8 land_mode_fx_tbl[0x60] = {
    0x03,0x00, 0x06,0x40, 0x06,0x41, 0x06,0x42, 0x00,0x00, 0x2b,0x43,
    0x2b,0x44, 0x06,0x45, 0x06,0x46, 0x06,0x47, 0x06,0x48, 0x07,0x00,
    0x06,0x49, 0x06,0x4a, 0x0a,0x24, 0x06,0x27, 0x03,0x33, 0x06,0x4c,
    0x2c,0x00, 0x06,0x4d, 0x2b,0x57, 0x2b,0x58, 0x06,0x4e, 0x06,0x4f,
    0x06,0x50, 0x03,0x3f, 0x06,0x51, 0x06,0x52, 0x06,0x53, 0x06,0x54,
    0x2c,0x55, 0x06,0x56, 0x06,0x00, 0x06,0x00, 0x00,0x00, 0x00,0x00,
    0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00,
    0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

/* land_sound_tbl_opl @ DGROUP 0x266e (file 0x13aae) / land_sound_tbl_std @ 0x269e
 * (file 0x13ade) — land_on_tile_below indexes these by p1_latched_action to pick
 * the landing sound id (OPL/charger device vs. the others).  0x30 bytes each. */
u8 land_sound_tbl_opl[0x30] = {
    0x00,0x04,0x04,0x04,0x00,0x00,0x00,0x04,0x04,0x04,0x04,0x00,
    0x04,0x04,0x04,0x00,0x04,0x00,0x00,0x04,0x04,0x04,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
u8 land_sound_tbl_std[0x30] = {
    0x00,0x02,0x02,0x02,0x00,0x00,0x00,0x02,0x02,0x02,0x02,0x00,
    0x02,0x02,0x02,0x00,0x02,0x00,0x00,0x02,0x02,0x02,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/*
 * land_on_tile_below — 1000:2810   (game_mode_handlers[0x2f])
 * --------------------------------------------------------------------------
 * The landing handler.  Unless the previous mode was 0x03/0x0d/0x10, plays the
 * landing sound for the latched action (land_sound_tbl_opl/_std indexed by
 * p1_latched_action, device-dependent).  Then resolves the tile below the player:
 * if cell < 8 (no tile row below) enters mode 6; else reads tile_below_player =
 * tilemap[cell-8], looks up its (mode, fx) pair in land_mode_fx_tbl, enters that
 * mode; if the entered mode is 0x0a plays the charger sound, and if the fx code is
 * nonzero runs the FX (apply_cell_animation — stubbed, → Phase 5/6).
 *
 * RECONSTRUCTION FIDELITY: the engine reads land_mode_fx_tbl / the sound tables as
 * raw near byte tables in DGROUP (e.g. *(byte*)(tile*2 + 0x76a)); we model each as
 * the dumped C array so the index arithmetic lands on the same byte.  The tilemap
 * read uses the forward-declared `tilemap` far pointer (level data, → T7).  The
 * decomp's `local_3 = tile_below_player` (an unused copy) is omitted.
 * apply_cell_animation is the FX/anim-channel allocator (extern stub, → Phase 5/6).
 */
void land_on_tile_below(void)
{
    u8 sound_id;
    u8 land_sound_id;
    u8 land_fx_code;
    u8 land_mode;

    if (prev_game_mode != 0x03 && prev_game_mode != 0x0d && prev_game_mode != 0x10) {
        if (sound_device_state == 4) {
            land_sound_id = land_sound_tbl_opl[p1_latched_action];
        } else {
            land_sound_id = land_sound_tbl_std[p1_latched_action];
        }
        if (land_sound_id != 0) {
            play_sound(land_sound_id);
        }
    }
    if (p1_cell < 8) {
        enter_game_mode(6);
    } else {
        anim_target_cell = (u8)(p1_cell - 8);
        tile_below_player = tilemap[(u16)anim_target_cell];
        land_mode    = land_mode_fx_tbl[(u16)tile_below_player * 2];
        land_fx_code = land_mode_fx_tbl[(u16)tile_below_player * 2 + 1];
        enter_game_mode(land_mode);
        if (game_mode == 0x0a) {
            if (sound_device_state == 4) {
                sound_id = 9;
            } else {
                sound_id = 0x14;
            }
            play_sound(sound_id);
        }
        if (land_fx_code != 0) {
            apply_cell_animation(land_fx_code);
        }
    }
    return;
}

/*
 * check_tile_below_ladder_or_land — 1000:29a6
 * --------------------------------------------------------------------------
 * Checks the tile below the player (cell-8): if it is the ladder tile 0x0e and no
 * down input is held (input_state&2 == 0), plays the climb sound, runs FX 0x24
 * (apply_cell_animation — stubbed) and enters mode 0x0a (climb).  If down is held,
 * delegates to move_down_step (a move-step substate → T4).  Otherwise (and when
 * cell < 8) delegates to p1_exec_pending_action (the pending-action move-step
 * delegate → T4).
 *
 * RECONSTRUCTION FIDELITY: the ladder branch is ported 1:1 (the leaf's own mode
 * transition to 0x0a).  The two delegate calls (p1_exec_pending_action 1000:465e,
 * move_down_step 1000:253f) are move-step substates — out of scope for this task
 * (Task 4) — and stay extern stubs.  apply_cell_animation is the FX allocator
 * (extern stub, → Phase 5/6).  The tilemap read uses the forward-declared `tilemap`
 * far pointer; the engine reads the cell-8 tile as a SIGNED char (`== '\x0e'`),
 * mirrored here with a signed compare so a high-bit tile never spuriously matches.
 */
void check_tile_below_ladder_or_land(void)
{
    u8 sound_id;

    if (p1_cell < 8) {
        p1_exec_pending_action();
    } else {
        anim_target_cell = (u8)(p1_cell - 8);
        if ((s8)tilemap[(u16)anim_target_cell] == 0x0e) {
            if ((input_state & 2) == 0) {
                if (sound_device_state == 4) {
                    sound_id = 9;
                } else {
                    sound_id = 0x14;
                }
                play_sound(sound_id);
                apply_cell_animation(0x24);
                enter_game_mode(0x0a);
            } else {
                move_down_step();
            }
        } else {
            p1_exec_pending_action();
        }
    }
    return;
}
