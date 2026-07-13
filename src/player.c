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
#include "sound.h"   /* play_exit_sound/_contact/_pickup/_state_79b9/_event_64c1 (move_step_dispatch_tbl targets) */
#include "items.h"   /* check_exit_tile_vert (6372), move_step_read_item (6627) */
#include "input.h"   /* input_state_clear (65d2) */
#include "gfx_overlay.h"  /* player_view_geom_t */
#include "entity.h"       /* sprite_obj_t */
#ifdef BUMPY_PLAYABLE
#include "dosio.h"   /* dosio_save — OOB-mode diagnostic log (playable only) */
#endif

/* ── DGROUP globals owned by this module ────────────────────────────────────── */

s16        p1_pixel_x;             /* 203b:0x9290 */
s16        p1_pixel_y;             /* 203b:0x9292 */
u16        p1_move_anim;           /* 203b:0x824a — WORD (engine stores/reads 16-bit:
                                      1000:1417 MOV [0x824a],AX; 1000:1cc9 word read;
                                      bounce modes 0x3d/0x3f use frames 0x1d1..0x1d7.
                                      Was u8 — truncation drew platform frames at
                                      Bumpy's position; fixed 2026-07-02) */
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
u8         p1_idle_jump_flag;      /* 203b:0x79b4 — written by init_round_state /
                                      gamemode_default_idle (290a/2918) / p1_try_jump_action
                                      (65c3); reset to 0 at round init.  Exact semantics TBD:
                                      those two consumers' reconstructions do not yet read it. */
u16 __far *p1_move_script;         /* 203b:0xa1ac (off) / 0xa1ae (seg) */
#ifdef BUMPY_PLAYABLE
extern u16 __far *move_script_entries(u16 dg);   /* move_scripts.c — relocated blob ptr */
#endif

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

        p1_move_anim = p1_move_script[0];              /* anim = script[0] (full word,
                                                          1000:1414/1417) */

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
#ifdef BUMPY_PLAYABLE
/* HOST DIAGNOSTIC GUARD state (playable only; NOT in the original).
 * enter_game_mode indexes mode_script_tbl[mode] and (next tick) game_mode_handlers
 * [mode] with NO bounds check — faithful: the original (enter_game_mode 1000:4263)
 * has none either.  Valid modes are 0x00..0x3f (game_mode_handlers has 0x40 entries;
 * mode_script_tbl has 0x40 4-byte slots).  The original therefore NEVER calls this
 * with mode >= 0x40 (that would dispatch out of bounds / crash), so a mode >= 0x40
 * reaching here can only come from a RECONSTRUCTION divergence (e.g. a contact/
 * action resolver fed an index the engine would not).  The guard below (a) prevents
 * the OOB dispatch — the wild jump / garbage move-script that shows up in-game as
 * "spontaneous movement" — and (b) records the offending mode + the contact/pending
 * indices + cell so the root divergence can be traced.  It is INERT in faithful
 * operation (never fires).  See docs/reconstruction-fidelity.md. */
u8 g_oob_mode_count   = 0;   /* # of enter_game_mode(mode>=0x40) hits (saturates 0xff) */
u8 g_oob_last_mode    = 0;   /* last offending mode value */
u8 g_oob_last_contact = 0;   /* p1_contact_code when it fired */
u8 g_oob_last_pending = 0;   /* p1_pending_action when it fired */
u8 g_oob_last_cell    = 0;   /* p1_cell when it fired */
/* Ring log of the last 8 OOB events (5 bytes each: mode, contact, pending, cell,
   count) flushed to C:\OOBLOG.BIN on the mounted drive so it lands on the host at
   local/build/capture/game/OOBLOG.BIN after a playtest.  Throttled (every 8th hit)
   to keep DOS file I/O out of the hot path. */
static u8 g_oob_log[8u * 5u];
static u8 g_oob_log_idx = 0;
#endif

void enter_game_mode(u8 mode)
{
    u16          tbl_off;
    u16          tbl_seg;
    const u8 __far *script;

    input_state = 0;
#ifdef BUMPY_PLAYABLE
    if (mode >= 0x40u) {
        /* OOB mode — see the guard note above.  Record + skip the OOB dispatch so
           game_mode is NOT set to an out-of-range value (which would index
           game_mode_handlers/mode_script_tbl past their 0x40 entries next tick). */
        if (g_oob_mode_count < 0xffu) { g_oob_mode_count++; }
        g_oob_last_mode    = mode;
        g_oob_last_contact = p1_contact_code;
        g_oob_last_pending = p1_pending_action;
        g_oob_last_cell    = p1_cell;
        {
            u8 *r = g_oob_log + (u16)g_oob_log_idx * 5u;
            r[0] = mode; r[1] = p1_contact_code; r[2] = p1_pending_action;
            r[3] = p1_cell; r[4] = g_oob_mode_count;
            g_oob_log_idx = (u8)((g_oob_log_idx + 1u) & 7u);
        }
        if ((g_oob_mode_count & 7u) == 0u) {
            dosio_save("OOBLOG.BIN", g_oob_log, (u16)sizeof g_oob_log);
        }
        return;
    }
#endif
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
 * then calls the near function pointer stored there.
 *
 * RECONSTRUCTION FIDELITY (Phase 9 T2): the table holds little-endian 16-bit engine
 * NEAR offsets (move_step_dispatch_tbl bytes are byte-identical to the unpacked
 * image — see below).  On the host those offsets are not callable code addresses, so
 * instead of `(*(void(**)())slot)()` we read the raw 16-bit offset and route it to its
 * reconstructed host function via move_step_handler_for_offset().  This resolver is the
 * ONLY host-execution deviation here; the index arithmetic and the table bytes stay 1:1.
 */
void dispatch_move_step(void)
{
    u16 off;

    off = *(u16 *)(move_step_dispatch_tbl +
                   (u16)game_mode * 0x22 + (u16)p1_move_step_idx * 2);
    move_step_handler_for_offset(off)();
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
 *   0x10,0x2c FUN_1000_22b0(0x22b0, T4)    0x1c p1_input_dispatch_bit10(0x4344)
 *   0x1d..0x20 FUN_1000_4437(0x4437)       0x2d run_physics_settle_wrap(0x22c1, T4)
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
    /* 0x2d */ run_physics_settle_wrap,        /* 1000:22c1 (Phase-2 T4) */
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
 * 0x6648/0x6717/0x654e; common filler 0x7111).  The reached physics micro-handlers
 * are now reconstructed in player.c (Phase-2 Task 4, "═ PHASE 2, TASK 4 ═" banner);
 * we keep the real bytes here so the table is faithful, and the host harness maps
 * each offset to its reconstructed C fn (item/sound/FX micro-handlers outside the
 * reached physics set remain boundary stubs).
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
        if (p1_step_col_count == 0) {             /* 0x855e */
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
        if (p1_step_col_count == 7) {             /* 0x855e */
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
#ifdef BUMPY_PLAYABLE
    /* Engine offset 0x140c is in the reconstructed move-script blob; resolve it to
       the runtime g_move_script_blob far ptr (move_scripts.c).  The raw 0x203b:0x140c
       below is the original's literal — garbage in the recon's DGROUP layout, which is
       what crashed movement (bug #3: INT 6 from the bad move-script). */
    p1_move_script = move_script_entries(0x140cu);
#else
    p1_move_script = (u16 __far *)MK_FP(P1_MOVE_SCRIPT_STATIC_SEG, MOVE_SCRIPT_WALK_RIGHT_OFF);
#endif
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
#ifdef BUMPY_PLAYABLE
    p1_move_script = move_script_entries(0x1460u);   /* relocated; see p1_begin_walk_right */
#else
    p1_move_script = (u16 __far *)MK_FP(P1_MOVE_SCRIPT_STATIC_SEG, MOVE_SCRIPT_WALK_LEFT_OFF);
#endif
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
    if (p1_step_col_count == 0) {                 /* 0x855e */
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
    if (p1_step_col_count == 7) {                 /* 0x855e */
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

/* ── Contact/collision/action resolver tables (DUMPED-REAL, contiguous bank) ───
 * The engine keeps these ten mode-resolver tables as a CONTIGUOUS bank of
 * 0x20-strided near byte tables in DGROUP starting at 0x4256, and indexes each
 * with an UNMASKED tilemap byte: p1_contact_code = tilemap[cell+0x30] (0x00..0xff,
 * see read_tile_layer_contact @1000:6bd4 / move_left @1000:2634 which does
 * `resolved_mode = *(byte*)(&contact_action_tbl_left + p1_contact_code)`).  A
 * contact code >= 0x20 therefore legitimately reads PAST its own 32-byte table
 * into the physically-adjacent following table(s) — the engine computes
 * `*(byte*)(code + <table_base>)` with no bound.  Real levels reach such codes
 * (a captured in-level contact layer holds 0x28/0x29/0x2c ... up to 0xa9), so the
 * previous model — ten isolated u8[0x40] arrays zero-filled at idx 0x20..0x3f —
 * silently returned mode 0 (the fall script) at those tiles.  That surfaced as
 * position-dependent input: correct where the contact code was < 0x20, wrong
 * (ignored / wrong direction / stuck) where it was >= 0x20.
 *
 * RECONSTRUCTION FIDELITY: model the whole thing as ONE verbatim dump of DGROUP
 * [0x4256, 0x4476) and alias each named table to its DGROUP offset (macros in
 * player.h), so `tbl[code]` reproduces the engine's flat `*(byte*)(code + base)`
 * read for every code 0x00..0xff.  The trailing bytes (offsets >= 0x140, i.e.
 * DGROUP >= 0x4396) are the real tile_followup_action_lut + move_step_dispatch_tbl
 * bytes the engine spills into for the highest codes — dumped verbatim, not
 * invented.  Ground truth verified by local/build/op12-handoff/{validate_contact_
 * tbls.py, gen_contact_bank.py}.  (The low half 0x00..0x1f is byte-identical to the
 * former ten arrays, so codes < 0x20 are unchanged.) */
u8 g_contact_resolver_bank[0x220] = {
    0x01,0x12,0x01,0x01,0x01,0x12,0x01,0x12,0x21,0x12,0x12,0x12,0x01,0x21,0x12,0x12,
    0x12,0x12,0x12,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x13,0x02,0x02,0x02,0x13,0x13,0x02,0x22,0x13,0x13,0x13,0x02,0x22,0x13,0x13,
    0x13,0x13,0x13,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x08,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x08,0x14,0x14,0x14,
    0x14,0x14,0x14,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x09,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x09,0x15,0x15,0x15,
    0x15,0x15,0x15,0x09,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x25,0x27,0x25,0x25,0x25,0x27,0x25,0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x27,
    0x27,0x27,0x27,0x25,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x26,0x28,0x26,0x26,0x26,0x28,0x28,0x26,0x28,0x28,0x28,0x28,0x28,0x28,0x28,0x28,
    0x28,0x28,0x28,0x26,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x1a,0x34,0x1a,0x1a,0x1a,0x34,0x1a,0x34,0x34,0x34,0x34,0x34,0x1a,0x34,0x34,0x34,
    0x34,0x34,0x34,0x1a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x1b,0x35,0x1b,0x1b,0x1b,0x35,0x35,0x1b,0x35,0x35,0x35,0x35,0x1b,0x35,0x35,0x35,
    0x35,0x35,0x35,0x1b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x1a,0x38,0x38,0x38,0x38,0x38,0x38,0x38,0x38,0x38,0x38,0x38,0x1a,0x38,0x38,0x38,
    0x38,0x38,0x38,0x1a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x1b,0x39,0x39,0x39,0x39,0x39,0x39,0x39,0x39,0x39,0x39,0x39,0x1b,0x39,0x39,0x39,
    0x39,0x39,0x39,0x1b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x2c,0x2d,0x2e,0x00,0x00,0x00,0x3f,0x00,0x00,0x00,0x00,0x00,0x56,
    0x5b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x48,0x66,0x11,0x71,0x11,0x71,
    0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,0x17,0x67,0x4e,0x65,0x87,0x65,0x11,0x71,
    0x11,0x71,0x11,0x71,0xfb,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x99,0x66,0x11,0x71,
    0x11,0x71,0x11,0x71,0x26,0x63,0x1c,0x65,0x17,0x67,0x4e,0x65,0x87,0x65,0xe5,0x65,
    0x27,0x66,0x11,0x71,0xfb,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xd8,0x66,
    0x11,0x71,0x11,0x71,0x11,0x71,0x72,0x63,0x35,0x65,0x17,0x67,0x4e,0x65,0x87,0x65,
    0xe5,0x65,0x27,0x66,0x11,0x71,0xfb,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x3a,0x67,0x11,0x71,0x11,0x71,0xe2,0x64,0x11,0x66,0x11,0x71,0x27,0x66,0x11,0x71,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0xff,0x64,0x17,0x67,0x4e,0x65,0x87,0x65,0xe5,0x65,0x11,0x71,0x27,0x66,
    0xfb,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,
};

/* ── Move-action LUT bank (DUMPED-REAL, contiguous — same flat-index model) ─────
 * action_tbl_left/right and down_action_lut are indexed with UNMASKED tilemap
 * bytes: p1_move_left/right do `exec_move_action(*(byte*)(p1_pending_action +
 * 0x36ee/0x371e))` and move_down does `down_action_lut[tilemap[cell-8]]`.  Those
 * bytes share the tilemap's byte pool with the contact layer (which reaches 0xa9),
 * so an index >= 0x30 reads PAST the 32-byte table into the next one — the tables
 * are physically adjacent at 0x30 stride.  Modelling each as an isolated u8[0x30]
 * made those high-index reads OUT-OF-BOUNDS (undefined) instead of the engine's
 * defined spill into the following table, so move_down/left/right did the wrong
 * thing at certain tiles (position-dependent).  Same fix as the contact bank:
 * one verbatim dump of DGROUP [0x36ee,0x384e) with each table aliased to its
 * offset (macros in player.h).  action_tbl_default is indexed by game_mode
 * (bounded < 0x40) so it never overflowed, but it lives inside the bank.
 * Verbatim; ground truth via local/build/op12-handoff/gen_action_bank.py. */
u8 g_action_lut_bank[0x160] = {
    0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x01,0x01,0x01,0x01,0x00,0x01,0x01,0x01,0x01,
    0x01,0x01,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x01,0x01,0x01,0x01,0x01,0x10,
    0x30,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x02,0x02,0x02,0x02,0x01,0x02,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x02,
    0x02,0x02,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x02,0x02,0x02,0x02,0x02,0x10,
    0x30,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x11,0x00,0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x11,0x11,0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x11,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x08,0x09,0x00,0x00,0x00,0x00,0x00,0x08,0x09,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x09,0x08,0x09,0x08,0x09,0x08,0x09,0x08,0x08,0x09,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x09,0x08,0x09,0x08,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x09,0x08,0x09,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x05,0x00,0x40,0x00,0x05,0x00,0x41,0x00,0x04,0x00,0x42,0x00,0x02,0x00,0x43,0x00,
    0x02,0x00,0x44,0x00,0x02,0x00,0x45,0x00,0x05,0x00,0x46,0x00,0x05,0x00,0x47,0x00,
    0x04,0x00,0x48,0x00,0x05,0x00,0x49,0x00,0x05,0x00,0x4a,0x00,0x01,0x00,0x4b,0x00,
    0x05,0x00,0x4c,0x00,0x02,0x00,0x4d,0x00,0x02,0x00,0x4e,0x00,0x03,0x00,0x4f,0x00,
    0x03,0x00,0x50,0x00,0x03,0x00,0x51,0x00,0x03,0x00,0x52,0x00,0x05,0x00,0x53,0x00,
    0x05,0x00,0x54,0x00,0x04,0x00,0x55,0x00,0x02,0x00,0x56,0x00,0x02,0x00,0x57,0x00,
    0x02,0x00,0x58,0x00,0x05,0x00,0x59,0x00,0x05,0x00,0x5a,0x00,0x05,0x00,0x5b,0x00,
    0x05,0x00,0x5c,0x00,0x05,0x00,0x5d,0x00,0x04,0x00,0x5e,0x00,0x03,0x00,0x5f,0x00,
    0x03,0x00,0x60,0x00,0x03,0x00,0x61,0x00,0x05,0x00,0x62,0x00,0x05,0x00,0x63,0x00,
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
    p1_contact_code = tilemap[(u16)cell + TILE_CONTACT_LAYER_OFF];
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
    if (p1_step_col_count == 0) {                 /* 0x855e */
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
    if (p1_step_col_count == 7) {                 /* 0x855e */
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
    if (p1_step_col_count == 0) {                 /* 0x855e */
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
            } else if (p1_step_col_count == 1) {  /* 0x855e */
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
    if (p1_step_col_count == 7) {                 /* 0x855e */
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
            } else if (p1_step_col_count == 6) {  /* 0x855e */
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
u8 land_mode_fx_tbl[0x200] = {
    0x03,0x00,0x06,0x40,0x06,0x41,0x06,0x42,0x00,0x00,0x2b,0x43,0x2b,0x44,0x06,0x45,
    0x06,0x46,0x06,0x47,0x06,0x48,0x07,0x00,0x06,0x49,0x06,0x4a,0x0a,0x24,0x06,0x27,
    0x03,0x33,0x06,0x4c,0x2c,0x00,0x06,0x4d,0x2b,0x57,0x2b,0x58,0x06,0x4e,0x06,0x4f,
    0x06,0x50,0x03,0x3f,0x06,0x51,0x06,0x52,0x06,0x53,0x06,0x54,0x2c,0x55,0x06,0x56,
    0x06,0x00,0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xf9,0x28,0xf9,0x28,0xf9,0x28,0xb6,0x23,0xf9,0x28,0x23,0x24,0xf9,0x28,0xf9,0x28,
    0xf9,0x28,0xf9,0x28,0x70,0x24,0x8e,0x24,0xd7,0x24,0x0a,0x25,0xad,0x25,0xb6,0x23,
    0xb0,0x22,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,
    0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,0x44,0x43,0x37,0x44,0x37,0x44,0x37,0x44,
    0x37,0x44,0x5e,0x1e,0x90,0x1e,0xc2,0x1e,0x3e,0x1f,0x38,0x21,0xe7,0x21,0xf9,0x28,
    0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xb0,0x22,0xc1,0x22,0xd2,0x22,0x10,0x28,
    0x3d,0x1e,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,
    0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,0xf9,0x28,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x11,0x71,0x25,0x50,0x3f,0x50,0x59,0x50,0x6f,0x50,0x11,0x71,0x11,0x71,
    0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,0xbf,0x4d,0x44,0x4e,0xc9,0x4e,0x4e,0x4f,
    0xbf,0x4d,0xbf,0x4d,0xbf,0x4d,0xbf,0x4d,0xbf,0x4d,0x11,0x71,0xff,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7a,0x73,
    0x3b,0x10,0xa4,0x73,0x3b,0x10,0xce,0x73,0x3b,0x10,0xf8,0x73,0x3b,0x10,0x22,0x74,
    0x3b,0x10,0x4c,0x74,0x3b,0x10,0x76,0x74,0x3b,0x10,0xa0,0x74,0x3b,0x10,0xca,0x74,
    0x3b,0x10,0xf4,0x74,0x3b,0x10,0x1e,0x75,0x3b,0x10,0x48,0x75,0x3b,0x10,0x72,0x75,
    0x3b,0x10,0x9c,0x75,0x3b,0x10,0xe6,0x11,0x3b,0x10,0x40,0x4b,0x4c,0x00,0xef,0x11,
    0x3b,0x10,0xc0,0xc6,0x2d,0x00,0xf8,0x11,0x3b,0x10,0x40,0x42,0x0f,0x00,0x01,0x12,
    0x3b,0x10,0x40,0x0d,0x03,0x00,0x0a,0x12,0x3b,0x10,0x30,0x75,0x00,0x00,0x13,0x12,
    0x3b,0x10,0xa0,0x0f,0x00,0x00,0x1c,0x12,0x3b,0x10,0xf4,0x01,0x00,0x00,0x25,0x12,
    0x3b,0x10,0x61,0x00,0x90,0x5f,0x01,0x00,0x32,0x12,0x3b,0x10,0x61,0x00,0x63,0x7d,
    0x00,0x00,0x3f,0x12,0x3b,0x10,0x61,0x00,0x63,0x7d,0x00,0x00,0x4c,0x12,0x3b,0x10,
    0x61,0x00,0x63,0x7d,0x00,0x00,0x56,0x12,0x3b,0x10,0x62,0x00,0x5c,0x1a,0x00,0x00,
    0x60,0x12,0x3b,0x10,0x62,0x00,0x45,0x89,0x00,0x00,0x6a,0x12,0x3b,0x10,0x7a,0x00,
};

/* land_sound_tbl_opl @ DGROUP 0x266e (file 0x13aae) / land_sound_tbl_std @ 0x269e
 * (file 0x13ade) — land_on_tile_below indexes these by p1_latched_action to pick
 * the landing sound id (OPL/charger device vs. the others).  0x30 bytes each. */
u8 land_sound_tbl_opl[0x100] = {
    0x00,0x04,0x04,0x04,0x00,0x00,0x00,0x04,0x04,0x04,0x04,0x00,0x04,0x04,0x04,0x00,
    0x04,0x00,0x00,0x04,0x04,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x02,0x02,0x02,0x00,0x00,0x00,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x00,
    0x02,0x00,0x00,0x02,0x02,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x03,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x00,0x03,
    0x00,0x03,0x11,0x03,0x03,0x03,0x00,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x11,0x03,
    0x03,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x09,0x09,0x09,0x09,0x0b,0x0b,0x0b,0x00,0x0b,
    0x00,0x09,0x12,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x00,0x0b,0x0b,0x0b,0x0b,0x12,0x0b,
    0x0b,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x2f,0x2f,0x2f,0x2f,0x2f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x2f,0x2f,0x2f,0x2f,0x2f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x0d,0x0d,0x0d,0x0d,0x0d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x0d,0x0d,0x0d,0x0d,0x0d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
u8 land_sound_tbl_std[0x100] = {
    0x00,0x02,0x02,0x02,0x00,0x00,0x00,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x00,
    0x02,0x00,0x00,0x02,0x02,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x03,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x00,0x03,
    0x00,0x03,0x11,0x03,0x03,0x03,0x00,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x11,0x03,
    0x03,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x09,0x09,0x09,0x09,0x0b,0x0b,0x0b,0x00,0x0b,
    0x00,0x09,0x12,0x0b,0x0b,0x0b,0x00,0x0b,0x0b,0x00,0x0b,0x0b,0x0b,0x0b,0x12,0x0b,
    0x0b,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x2f,0x2f,0x2f,0x2f,0x2f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x2f,0x2f,0x2f,0x2f,0x2f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x0d,0x0d,0x0d,0x0d,0x0d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x0d,0x0d,0x0d,0x0d,0x0d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x0e,0x10,0x10,0x10,0x12,0x0e,0x0a,0x0a,
    0x0a,0x0a,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,
    0x00,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0e,0x10,0x10,0x11,0x12,0x0e,0x0a,0x0a,
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

/* ════════════════════════════════════════════════════════════════════════════
 *  PHASE 2, TASK 4 — JUMP / FALL / BOUNCE MOVE-STEP SUBSTATES
 *  --------------------------------------------------------------------------
 *  The per-step micro-handlers dispatched through move_step_dispatch_tbl (raw
 *  near offsets, blob above), the run_physics_settle handler, and the two
 *  move-step delegates check_tile_below_ladder_or_land tail-calls.  Ported 1:1
 *  from the Ghidra decomp (verified live via MCP + disassembly, 2026-06).  Each
 *  cites its engine address.  These un-stub the bodies the T1 capture's jump/
 *  fall/bounce scenarios reach so the physics harness's trajectory-stitch can run
 *  the full chain and check_tile_below's delegate path gets a full exit-diff.
 *
 *  RECONSTRUCTION FIDELITY (boundary callees kept stubbed, per the Task brief):
 *    - apply_cell_animation (1000:69aa)      — anim-channel / FX allocator  (→ Phase 5)
 *    - play_sound (1000:6e11)                — sound playback                (→ Phase 6)
 *    - apply_contact_action                  — contact sound + anim-slot     (→ Phase 5/6)
 *    - FUN_1000_4802 / FUN_1000_22b0         — teleport / settle-wrap leaves (extern)
 *    - read_tile_at_cell / read_tile_layer_contact — tile leaves (already in T6c)
 *  Each remains an extern declared in player.h; the physics globals each substate
 *  writes (px/py/anim/mode/cell/input/steps) are reconstructed 1:1, which is what
 *  the per-fn comparator gates on.
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── DGROUP move-step substate globals (Task 4) ───────────────────────────────
 * Two DISTINCT engine counters live here; keep them separate:
 *   move_step_count   (DGROUP 0x824c, jump_step_counter) — the existing global,
 *                     stored 8 by gamemode_default_idle and incremented/tested by
 *                     move_down_step; REUSED here (not redefined).
 *   p1_step_col_count (DGROUP 0x855e) — the cursor / move-step COLUMN counter
 *                     (cursor_move_right increments it; the contact/walk-step
 *                     resolvers and the apply_contact handlers test it ==0/1/6/7).
 * The bytes below are new. */
u8  p1_grid_row;          /* DGROUP 0x855c — cursor row counter (cursor_move_up/down) */
u8  anim_frame_ctr;       /* DGROUP 0x855d — walk-anim per-frame tick counter (step_walk_anim/p1_advance_move_anim) */
u8  p1_step_col_count;    /* DGROUP 0x855e — cursor/move-step column counter (read by
                             gamemode_25/26_contact, move_left/right(_step_resolve),
                             p1_resolve_walk_*_contact, p1_apply_contact_action_*,
                             move_step_last_variant; written by cursor_move_right). */
u8  g_anim_channel_idx;   /* DGROUP 0x856c — anim-channel index probed by move_step_landed */
u8  level_complete_flag;  /* DGROUP 0xa1b1 — cleared by move_step_landed on the '[' tile */

/* tile_followup_action_lut @ DGROUP 0x4396 (file 0x157d6) — p1_step_landed indexes
 * it by p1_current_tile to find a follow-up action tile to queue.  Dumped byte-exact
 * from BUMPY_unpacked.exe.
 * RECONSTRUCTION FIDELITY: this table is only 0x2a bytes before move_step_dispatch_tbl
 * begins at DGROUP 0x43c0; the engine reads it as a raw near byte table, so tile
 * indices 0x2a..0x2f read into the dispatch table's first bytes (0x48,0x66,0x11,0x71,
 * 0x11,0x71 — the low/high bytes of dispatch offsets).  We reproduce those exact
 * tail bytes so the index arithmetic lands identically. */
u8 tile_followup_action_lut[0x100] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x2c,0x2d,0x2e,0x00,0x00,0x00,0x3f,0x00,0x00,0x00,0x00,0x00,0x56,
    0x5b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x48,0x66,0x11,0x71,0x11,0x71,
    0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,0x17,0x67,0x4e,0x65,0x87,0x65,0x11,0x71,
    0x11,0x71,0x11,0x71,0xfb,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x99,0x66,0x11,0x71,
    0x11,0x71,0x11,0x71,0x26,0x63,0x1c,0x65,0x17,0x67,0x4e,0x65,0x87,0x65,0xe5,0x65,
    0x27,0x66,0x11,0x71,0xfb,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xd8,0x66,
    0x11,0x71,0x11,0x71,0x11,0x71,0x72,0x63,0x35,0x65,0x17,0x67,0x4e,0x65,0x87,0x65,
    0xe5,0x65,0x27,0x66,0x11,0x71,0xfb,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x3a,0x67,0x11,0x71,0x11,0x71,0xe2,0x64,0x11,0x66,0x11,0x71,0x27,0x66,0x11,0x71,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0xff,0x64,0x17,0x67,0x4e,0x65,0x87,0x65,0xe5,0x65,0x11,0x71,0x27,0x66,
    0xfb,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,
    0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,0x11,0x71,
    0x11,0x71,0x11,0x71,0x11,0x71,0x3a,0x67,0x11,0x66,0x11,0x71,0x17,0x67,0x7e,0x64,
};

/* Pending-action / contact LUTs the substates index (raw near byte tables in the
 * engine; modeled as dumped C arrays so the index arithmetic lands identically).
 *   p1_try_trigger_pending_action: table[p1_pending_action + 0x3cda]
 *   p1_move_step_with_sound:       table[p1_pending_action + 0x3d0a] (action),
 *                                  table[p1_pending_action + 0x25ae/0x25de] (sound)
 *   move_step_last_variant:        apply_contact_action(table[p1_contact_code+0x35de])
 *   p1_dispatch_pending_action:    table[p1_pending_action + 0x3caa]
 *   p1_exec_pending_action:        exec_move_action(table[p1_pending_action+0x36be])
 * These are reconstructed below from the unpacked image. */
/* DGROUP 0x3cda (file 0x1511a) — p1_try_trigger_pending_action anim LUT. */
u8 pending_anim_lut_3cda[0x100] = {
    0x00,0x03,0x3d,0x07,0x00,0x00,0x00,0x0a,0x0d,0x10,0x16,0x00,0x1c,0x20,0x22,0x00,
    0x39,0x2a,0x00,0x2c,0x2d,0x2e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x5e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x04,0x3e,0x00,0x00,0x00,0x00,0x11,0x12,0x13,0x17,0x00,0x1d,0x21,0x23,0x27,
    0x3a,0x2b,0x00,0x2c,0x2d,0x2e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x55,0x00,
    0x00,0x5f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x8d,0x07,0x00,0x10,0x12,0x14,0x1d,0x26,0x32,0xc5,0x45,0x2f,0x5c,0x60,
    0x64,0x69,0x74,0x75,0x85,0x8b,0x8c,0x4a,0x49,0x4b,0x98,0x96,0x94,0x92,0x9d,0xa4,
    0x7f,0x7e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xbe,0x37,0x3b,0x10,0xc2,0x37,0x3b,0x10,0xc6,0x37,0x3b,0x10,
    0xca,0x37,0x3b,0x10,0xce,0x37,0x3b,0x10,0xd2,0x37,0x3b,0x10,0xd6,0x37,0x3b,0x10,
    0xda,0x37,0x3b,0x10,0xde,0x37,0x3b,0x10,0xe2,0x37,0x3b,0x10,0xe6,0x37,0x3b,0x10,
    0xea,0x37,0x3b,0x10,0xee,0x37,0x3b,0x10,0xf2,0x37,0x3b,0x10,0xf6,0x37,0x3b,0x10,
    0xfa,0x37,0x3b,0x10,0xfe,0x37,0x3b,0x10,0x02,0x38,0x3b,0x10,0x06,0x38,0x3b,0x10,
    0x0a,0x38,0x3b,0x10,0x0e,0x38,0x3b,0x10,0x12,0x38,0x3b,0x10,0x16,0x38,0x3b,0x10,
    0x1a,0x38,0x3b,0x10,0x1e,0x38,0x3b,0x10,0x22,0x38,0x3b,0x10,0x26,0x38,0x3b,0x10,
};
/* DGROUP 0x3caa (file 0x150ea) — p1_dispatch_pending_action anim LUT. */
u8 pending_anim_lut_3caa[0x100] = {
    0x00,0x02,0x3c,0x06,0x00,0x31,0x32,0x09,0x0c,0x0f,0x15,0x00,0x1b,0x1f,0x36,0x26,
    0x38,0x29,0x00,0x2c,0x2d,0x2e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x55,0x00,
    0x00,0x5d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x03,0x3d,0x07,0x00,0x00,0x00,0x0a,0x0d,0x10,0x16,0x00,0x1c,0x20,0x22,0x00,
    0x39,0x2a,0x00,0x2c,0x2d,0x2e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x5e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x04,0x3e,0x00,0x00,0x00,0x00,0x11,0x12,0x13,0x17,0x00,0x1d,0x21,0x23,0x27,
    0x3a,0x2b,0x00,0x2c,0x2d,0x2e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x55,0x00,
    0x00,0x5f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x8d,0x07,0x00,0x10,0x12,0x14,0x1d,0x26,0x32,0xc5,0x45,0x2f,0x5c,0x60,
    0x64,0x69,0x74,0x75,0x85,0x8b,0x8c,0x4a,0x49,0x4b,0x98,0x96,0x94,0x92,0x9d,0xa4,
    0x7f,0x7e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xbe,0x37,0x3b,0x10,0xc2,0x37,0x3b,0x10,0xc6,0x37,0x3b,0x10,
    0xca,0x37,0x3b,0x10,0xce,0x37,0x3b,0x10,0xd2,0x37,0x3b,0x10,0xd6,0x37,0x3b,0x10,
    0xda,0x37,0x3b,0x10,0xde,0x37,0x3b,0x10,0xe2,0x37,0x3b,0x10,0xe6,0x37,0x3b,0x10,
    0xea,0x37,0x3b,0x10,0xee,0x37,0x3b,0x10,0xf2,0x37,0x3b,0x10,0xf6,0x37,0x3b,0x10,
};
/* DGROUP 0x3d0a (file 0x1514a) — p1_move_step_with_sound anim LUT. */
u8 pending_anim_lut_3d0a[0x100] = {
    0x00,0x04,0x3e,0x00,0x00,0x00,0x00,0x11,0x12,0x13,0x17,0x00,0x1d,0x21,0x23,0x27,
    0x3a,0x2b,0x00,0x2c,0x2d,0x2e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x55,0x00,
    0x00,0x5f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x8d,0x07,0x00,0x10,0x12,0x14,0x1d,0x26,0x32,0xc5,0x45,0x2f,0x5c,0x60,
    0x64,0x69,0x74,0x75,0x85,0x8b,0x8c,0x4a,0x49,0x4b,0x98,0x96,0x94,0x92,0x9d,0xa4,
    0x7f,0x7e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xbe,0x37,0x3b,0x10,0xc2,0x37,0x3b,0x10,0xc6,0x37,0x3b,0x10,
    0xca,0x37,0x3b,0x10,0xce,0x37,0x3b,0x10,0xd2,0x37,0x3b,0x10,0xd6,0x37,0x3b,0x10,
    0xda,0x37,0x3b,0x10,0xde,0x37,0x3b,0x10,0xe2,0x37,0x3b,0x10,0xe6,0x37,0x3b,0x10,
    0xea,0x37,0x3b,0x10,0xee,0x37,0x3b,0x10,0xf2,0x37,0x3b,0x10,0xf6,0x37,0x3b,0x10,
    0xfa,0x37,0x3b,0x10,0xfe,0x37,0x3b,0x10,0x02,0x38,0x3b,0x10,0x06,0x38,0x3b,0x10,
    0x0a,0x38,0x3b,0x10,0x0e,0x38,0x3b,0x10,0x12,0x38,0x3b,0x10,0x16,0x38,0x3b,0x10,
    0x1a,0x38,0x3b,0x10,0x1e,0x38,0x3b,0x10,0x22,0x38,0x3b,0x10,0x26,0x38,0x3b,0x10,
    0x2a,0x38,0x3b,0x10,0x2e,0x38,0x3b,0x10,0x32,0x38,0x3b,0x10,0x36,0x38,0x3b,0x10,
    0x3a,0x38,0x3b,0x10,0x3e,0x38,0x3b,0x10,0x42,0x38,0x3b,0x10,0x46,0x38,0x3b,0x10,
    0x4a,0x38,0x3b,0x10,0x4e,0x38,0x3b,0x10,0x52,0x38,0x3b,0x10,0x56,0x38,0x3b,0x10,
};
/* DGROUP 0x36be (file 0x14afe) — p1_exec_pending_action action LUT. */
u8 pending_action_lut_36be[0x100] = {
    0x00,0x00,0x00,0x05,0x00,0x01,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0e,
    0x00,0x11,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x00,0x00,0x00,0x00,0x00,0x10,
    0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x01,0x01,0x01,0x01,0x00,0x01,0x01,0x01,0x01,
    0x01,0x01,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x01,0x01,0x01,0x01,0x01,0x10,
    0x30,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x02,0x02,0x02,0x02,0x01,0x02,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x02,
    0x02,0x02,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x02,0x02,0x02,0x02,0x02,0x10,
    0x30,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x11,0x00,0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x11,0x11,0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x11,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x08,0x09,0x00,0x00,0x00,0x00,0x00,0x08,0x09,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x09,0x08,0x09,0x08,0x09,0x08,0x09,0x08,0x08,0x09,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x09,0x08,0x09,0x08,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x09,0x08,0x09,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
/* DGROUP 0x35de (file 0x14a1e) — move_step_last_variant contact-action LUT. */
u8 contact_sound_lut_35de[0x100] = {
    0x00,0x01,0x02,0x03,0x04,0x17,0x00,0x06,0x07,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x02,0x03,0x04,0x17,0x05,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x02,0x03,0x04,0x17,0x00,0x06,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x14,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x14,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x05,0x00,0x01,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0e,
    0x00,0x11,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x00,0x00,0x00,0x00,0x00,0x10,
};
/* DGROUP 0x25ae (file 0x139ee) — p1_move_step_with_sound OPL/charger sound LUT. */
u8 move_sound_lut_opl_25ae[0x100] = {
    0x00,0x01,0x02,0x00,0x03,0x04,0x04,0x05,0x06,0x07,0x08,0x00,0x09,0x0a,0x0b,0x00,
    0x0c,0x0d,0x00,0x00,0x00,0x00,0x00,0x0e,0x0e,0x00,0x0f,0x0f,0x0f,0x0f,0x10,0x11,
    0x00,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x01,0x00,0x0b,0x09,0x09,0x05,0x06,0x07,0x09,0x00,0x09,0x09,0x01,0x00,
    0x01,0x0a,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x0b,0x0b,0x0b,0x0b,0x0b,0x00,
    0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x13,0x14,0x15,0x16,0x17,0x17,0x18,0x19,0x1a,0x1b,0x00,0x1c,0x1d,0x1e,0x1f,
    0x20,0x21,0x11,0x00,0x00,0x00,0x00,0x22,0x22,0x00,0x23,0x23,0x23,0x23,0x24,0x00,
    0x00,0x25,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x01,0x0e,0x0b,0x03,0x03,0x05,0x06,0x07,0x01,0x00,0x04,0x01,0x01,0x01,
    0x01,0x08,0x12,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x0b,0x0b,0x0b,0x0b,0x0b,0x00,
    0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x04,0x04,0x04,0x00,0x00,0x00,0x04,0x04,0x04,0x04,0x00,0x04,0x04,0x04,0x00,
    0x04,0x00,0x00,0x04,0x04,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x02,0x02,0x02,0x00,0x00,0x00,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x00,
};
/* DGROUP 0x25de (file 0x13a1e) — p1_move_step_with_sound std-device sound LUT. */
u8 move_sound_lut_std_25de[0x100] = {
    0x00,0x01,0x01,0x00,0x0b,0x09,0x09,0x05,0x06,0x07,0x09,0x00,0x09,0x09,0x01,0x00,
    0x01,0x0a,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x0b,0x0b,0x0b,0x0b,0x0b,0x00,
    0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x13,0x14,0x15,0x16,0x17,0x17,0x18,0x19,0x1a,0x1b,0x00,0x1c,0x1d,0x1e,0x1f,
    0x20,0x21,0x11,0x00,0x00,0x00,0x00,0x22,0x22,0x00,0x23,0x23,0x23,0x23,0x24,0x00,
    0x00,0x25,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x01,0x0e,0x0b,0x03,0x03,0x05,0x06,0x07,0x01,0x00,0x04,0x01,0x01,0x01,
    0x01,0x08,0x12,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x0b,0x0b,0x0b,0x0b,0x0b,0x00,
    0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x04,0x04,0x04,0x00,0x00,0x00,0x04,0x04,0x04,0x04,0x00,0x04,0x04,0x04,0x00,
    0x04,0x00,0x00,0x04,0x04,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x02,0x02,0x02,0x00,0x00,0x00,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x00,
    0x02,0x00,0x00,0x02,0x02,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x03,0x03,0x03,0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x00,0x03,
};
/* DGROUP 0x35be (file 0x149fe) — move_step_first_variant contact-action LUT
 * (the first-step counterpart of contact_sound_lut_35de @ 0x35de). */
u8 contact_action_lut_35be[0x100] = {
    0x00,0x01,0x02,0x03,0x04,0x17,0x05,0x00,0x07,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x02,0x03,0x04,0x17,0x00,0x06,0x07,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x02,0x03,0x04,0x17,0x05,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x02,0x03,0x04,0x17,0x00,0x06,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x14,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x14,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
/* DGROUP 0x3c7a (file 0x150ba) — move_step_first_variant pending-action anim LUT
 * (the first-step counterpart of pending_anim_lut_3caa @ 0x3caa). */
u8 pending_anim_lut_3c7a[0x100] = {
    0x00,0x01,0x3b,0x05,0x00,0x31,0x32,0x08,0x0b,0x0e,0x14,0x00,0x1a,0x1e,0x35,0x25,
    0x37,0x28,0x00,0x2c,0x2d,0x2e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x55,0x00,
    0x00,0x5c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x02,0x3c,0x06,0x00,0x31,0x32,0x09,0x0c,0x0f,0x15,0x00,0x1b,0x1f,0x36,0x26,
    0x38,0x29,0x00,0x2c,0x2d,0x2e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x55,0x00,
    0x00,0x5d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x03,0x3d,0x07,0x00,0x00,0x00,0x0a,0x0d,0x10,0x16,0x00,0x1c,0x20,0x22,0x00,
    0x39,0x2a,0x00,0x2c,0x2d,0x2e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x5e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x04,0x3e,0x00,0x00,0x00,0x00,0x11,0x12,0x13,0x17,0x00,0x1d,0x21,0x23,0x27,
    0x3a,0x2b,0x00,0x2c,0x2d,0x2e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x55,0x00,
    0x00,0x5f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x8d,0x07,0x00,0x10,0x12,0x14,0x1d,0x26,0x32,0xc5,0x45,0x2f,0x5c,0x60,
    0x64,0x69,0x74,0x75,0x85,0x8b,0x8c,0x4a,0x49,0x4b,0x98,0x96,0x94,0x92,0x9d,0xa4,
    0x7f,0x7e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xbe,0x37,0x3b,0x10,0xc2,0x37,0x3b,0x10,0xc6,0x37,0x3b,0x10,
};

/*
 * run_physics_settle — 1000:22fc
 * --------------------------------------------------------------------------
 * Unfreezes physics, runs 1000 tile-read iterations to settle, then decrements
 * the per-round settle countdown and returns it (-1 once expired, also raising
 * frame_abort_flag DGROUP 0x928d).
 *
 * RECONSTRUCTION FIDELITY: session_continue_flag / frame_abort_flag /
 * settle_countdown are DEFINED in game.c (cross-module DGROUP bytes 0x856d /
 * 0x928d / settle counter); declared extern in player.h for this port.  The 1000
 * iterations of p1_read_tile_under re-latch p1_pending_action from tilemap[p1_cell]
 * each pass (the engine's busy-settle); the loop is reproduced verbatim.
 */
char run_physics_settle(void)
{
    char ret;
    int i;

    physics_frozen = 0;
    for (i = 1000; i != 0; i = i - 1) {
        p1_read_tile_under();
    }
    session_continue_flag = 1;
    if (settle_countdown == 0) {
        frame_abort_flag = 0xff;
        ret = -1;
    } else {
        settle_countdown = settle_countdown - 1;
        ret = (char)settle_countdown;
    }
    return ret;
}

/* run_physics_settle_wrap — 1000:22c1   (game_mode_handlers[0x2d]) — a thin wrapper
 * that calls run_physics_settle and discards the return. */
void run_physics_settle_wrap(void)
{
    run_physics_settle();
    return;
}

/* FUN_1000_22b0 — 1000:22b0   (game_mode_handlers[0x10]/[0x2c]; also called by
 * move_down_step for pending-action 0x12/0x1f).  Byte-identical to
 * run_physics_settle_wrap: calls run_physics_settle, discards the return. */
void FUN_1000_22b0(void)
{
    run_physics_settle();
    return;
}

/* p1_read_tile_under — 1000:236f
 * Reads the tilemap byte at p1_cell into p1_pending_action (the move-step tile
 * leaf the settle loop and idle dispatch use). */
void p1_read_tile_under(void)
{
    p1_pending_action = tilemap[(u16)p1_cell];
    return;
}

/* ── anim-channel / cell-animation setters (thin FX-allocator wrappers) ────────
 * Each sets anim_target_cell = p1_cell, then tail-calls the stubbed FX allocator
 * apply_cell_animation (→ Phase 5).  The override variant also latches move_override. */

/* p1_set_cell_animation — 1000:695e */
void p1_set_cell_animation(char action_code)
{
    anim_target_cell = p1_cell;
    if (action_code != 0) {
        move_override = (u8)action_code;
        apply_cell_animation((u8)action_code);
    }
    return;
}

/* p1_set_cell_animation_no_override — 1000:6987 (does NOT touch move_override) */
void p1_set_cell_animation_no_override(char action_code)
{
    anim_target_cell = p1_cell;
    if (action_code != 0) {
        apply_cell_animation((u8)action_code);
    }
    return;
}

/* p1_trigger_cell_animation — 1000:6d94 (unconditional FX on the current cell) */
void p1_trigger_cell_animation(u8 action)
{
    anim_target_cell = p1_cell;
    apply_cell_animation(action);
    return;
}

/* p1_dispatch_pending_action — 1000:6d6a
 * If movement is not locked, run the pending-action anim for the table entry
 * selected by p1_pending_action (action_table near base passed by the caller). */
void p1_dispatch_pending_action(u8 *action_table)
{
    if (move_locked == 0) {
        p1_set_cell_animation_no_override((char)action_table[p1_pending_action]);
    }
    return;
}

/* p1_step_landed — 1000:6d26
 * After a cell step: read the tile under p1_cell; if it carries a follow-up action
 * tile (tile_followup_action_lut[p1_current_tile]), latch it (p1_latched_action,
 * p1_queued_action_code) and trigger its cell animation. */
void p1_step_landed(void)
{
    u8 action_tile;

    read_tile_at_cell(p1_cell);
    action_tile = tile_followup_action_lut[p1_current_tile];
    if (action_tile != 0) {
        p1_latched_action = p1_pending_action;
        p1_queued_action_code = action_tile;
        p1_trigger_cell_animation(action_tile);
    }
    return;
}

/* ── The input-mask micro-handlers (trivial bit masks on input_state) ──────────── */

/* input_state_mask_10 — 1000:65e5  (keep only bit 0x10) */
void input_state_mask_10(void)
{
    input_state = input_state & 0x10;
    return;
}

/* input_state_mask_1d — 1000:65fb  (mask &= 0x1d — clears bit 1) */
void input_state_mask_1d(void)
{
    input_state = input_state & 0x1d;
    return;
}

/* input_state_mask_0f — 1000:6611  (mask &= 0x0f — clears bit 0x10) */
void input_state_mask_0f(void)
{
    input_state = input_state & 0x0f;
    return;
}

#ifdef BUMPY_PLAYABLE
extern u8 __far *level_get_entity_dg(void);  /* level.c — entity-dg shadow (posC @ +0x274) */
#endif

/* p1_set_pixel_from_cell — 1000:4906
 * Derive P1's grid row (cell>>3) and in-row column, then set p1_pixel_x/y from the
 * posC cell-coordinate table (X @ dg[cell*4+0x274] +7, Y @ dg[cell*4+0x276] +0xf).
 * NOTE: the decomp's `move_step_count = cell - row*8` writes DGROUP 0x855e, which is
 * p1_step_col_count (the in-row COLUMN counter) — Ghidra mislabels 0x855e as
 * "move_step_count" (the items.c counter-aliasing correction; the real move_step_count
 * is 0x824c).  The posC pixel reads use g_entity_dg (level_populate_dg) under
 * BUMPY_PLAYABLE; the faithful default build (gameplay never run) sets only the
 * semantic row/col state, as g_entity_dg has no default-build accessor. */
void p1_set_pixel_from_cell(void)
{
    u8 cell = p1_cell;

    p1_grid_row = (u8)(cell >> 3);
    p1_step_col_count = (u8)(cell - (u8)(p1_grid_row * 8));   /* DGROUP 0x855e (column) */
#ifdef BUMPY_PLAYABLE
    {
        u8 __far *dg = level_get_entity_dg();
        u16 base = (u16)(0x274u + (u16)cell * 4u);
        s16 px = (s16)((u16)dg[base]     | ((u16)dg[base + 1] << 8));
        s16 py = (s16)((u16)dg[base + 2] | ((u16)dg[base + 3] << 8));
        p1_pixel_x = (s16)(px + 7);
        p1_pixel_y = (s16)(py + 0xf);
    }
#endif
}

/* reset_round_counters — 1000:31de (Ghidra canonical: init_round_state)
 * ----------------------------------------------------------------------------
 * Post-spawn per-round reset, called from reset_game_state (game.c) after
 * spawn_and_draw_level_entities.  The decompiler fails on this body, but the
 * disassembly (1000:31de..328e) is complete and every store maps to a named
 * DGROUP global, so it is reconstructed here verbatim (it was a no-op stub).
 * Restores: (1) movement/anim/contact state to zero, (2) the player & P2 pixel
 * positions FROM their start cells (p1/p2_set_pixel_from_cell — the dropped call
 * that left the player at the stale world-map pixel), (3) the entry "drop-in"
 * scripted move (p1_move_script @0x1394, 4 steps).  1:1 with the asm order.
 *
 * RECONSTRUCTION FIDELITY: the move-script far ptr (engine DS:0x1394) is resolved
 * through the relocated blob (move_script_entries) in the playable build, exactly
 * as p1_begin_walk_right does for 0x140c; the default build keeps the literal
 * 203b:0x1394.  p1_move_anim is the engine's WORD at 0x824a (see the
 * p1_advance_move_anim note — the old "0x824b never read" claim was wrong).
 */
void reset_round_counters(void)
{
    extern u8  g_anim_cur_cmd_byte;       /* anim.c   0x8578 */
    extern u8  anim_b_cur_frame_byte;     /* anim.c   0x8579 */
    extern u8  g_anim_a_active_mirror;    /* spawn.c  0x8e8b */
    extern u8  g_anim_b_active_mirror;    /* spawn.c  0x8e8c */
    extern u8  input_state;               /* input.c  0x8244 */
    extern u8  level_complete_anim_counter; /* items.c 0x8550 */
    extern u8  p2_step_idx;               /* player2.c 0x8563 */
    extern u8  p2_move_steps_left;        /* player2.c 0xa1b0 */
    extern u8  p2_move_toggle;            /* player2.c 0x8243 */
    extern u8  dgroup_flag_a1a9;          /* game_stubs.c 0xa1a9 */
    extern u8 __far *deferred_contact_ptr;/* game.c   0x9ba6/0x9ba8 */
    extern u8  deferred_contact_buf[];    /* game.c   0x0886 (engine DS:0x886) */
    extern u8  deferred_contact_countdown;/* game.c   0x79b7 */
    extern void p2_set_pixel_from_cell(void); /* player2.c 1000:48a9 */

    move_locked                 = 0;   /* 31ea [0x8242] */
    p1_move_step_idx            = 0;   /* 31ef [0x792a] */
    p1_move_anim                = 0;   /* 31f4 [0x824a] (word store) */
    physics_frozen              = 0;   /* 31fa [0xa0ce] */
    g_anim_cur_cmd_byte         = 0;   /* 3201 [0x8578] */
    g_anim_a_active_mirror      = 0;   /* 3204 [0x8e8b] */
    anim_b_cur_frame_byte       = 0;   /* 3209 [0x8579] */
    g_anim_b_active_mirror      = 0;   /* 320c [0x8e8c] */
    input_state                 = 0;   /* 320f [0x8244] */
    move_override               = 0;   /* 3214 [0xa1a7] */
    p1_idle_jump_flag           = 0;   /* 3219 [0x79b4] */
    p1_queued_action_code       = 0;   /* 321e [0x7923] */
    move_step_count             = 0;   /* 3223 [0x824c] */
    pending_erase_count         = 0;   /* 3228 [0xa1a8] */
    level_complete_flag         = 0;   /* 322d [0xa1b1] */
    level_complete_anim_counter = 0;   /* 3232 [0x8550] */
    p2_step_idx                 = 0;   /* 3237 [0x8563] */
    p2_move_steps_left          = 0;   /* 323c [0xa1b0] */
    p2_move_toggle              = 0;   /* 3241 [0x8243] */
    deferred_contact_countdown  = 0;   /* 3246 [0x79b7] */

    /* 324b..3259: deferred_contact_ptr = DS:0x886 (the event buffer head); stamp
       *ptr = 0xff (queue empty).  Engine DS:0x886 == the recon's deferred_contact_buf. */
    deferred_contact_ptr  = (u8 __far *)deferred_contact_buf;
    *deferred_contact_ptr = 0xffu;

    p1_set_pixel_from_cell();           /* 325d CALL 0x4906 — p1 pixel from start cell */
    p2_set_pixel_from_cell();           /* 3260 CALL 0x48a9 — p2 pixel from start cell */

    p1_pixel_y = (s16)(p1_pixel_y - 12);/* 3263..3269 [0x9292] += 0xfff4 */
    game_mode  = 0;                     /* 326c [0x792c] */

    /* 3271..3277: p1_move_script = DS:0x1394 (the entry drop-in move script). */
#ifdef BUMPY_PLAYABLE
    p1_move_script = move_script_entries(0x1394u);
#else
    p1_move_script = (u16 __far *)MK_FP(P1_MOVE_SCRIPT_STATIC_SEG, MOVE_SCRIPT_ROUND_ENTRY_OFF);
#endif
    p1_move_steps_left = 0x0au;          /* 327b [0x824d] = 10 */
    p1_facing_left     = 4;              /* 3282 [0x9bae] = 4 */
    p1_move_step_idx   = 4;              /* 3285 [0x792a] = 4 */
    dgroup_flag_a1a9   = 0;              /* 3288 [0xa1a9] */
}

/* ── The cursor-step micro-handlers (redraw via input_state_mask_0f, move cell) ── */

/* cursor_move_up — 1000:64e2  (p1_cell -= 8 = one row up; row counter--) */
void cursor_move_up(void)
{
    input_state_mask_0f();
    p1_cell = (u8)(p1_cell - 8);
    p1_grid_row = (u8)(p1_grid_row - 1);
    return;
}

/* cursor_move_down — 1000:64ff  (p1_cell += 8 = one row down; row counter++) */
void cursor_move_down(void)
{
    input_state_mask_0f();
    p1_cell = (u8)(p1_cell + 8);
    p1_grid_row = (u8)(p1_grid_row + 1);
    return;
}

/* cursor_move_right — 1000:6535  (p1_cell += 1; column counter++) */
void cursor_move_right(void)
{
    input_state_mask_0f();
    p1_cell = (u8)(p1_cell + 1);
    p1_step_col_count = (u8)(p1_step_col_count + 1);
    return;
}

/* ── The pending-action / jump trigger micro-handlers ──────────────────────────── */

/* p1_try_trigger_pending_action — 1000:654e
 * If not suppressed (p1_queued_action_code==0) and input bit 0x10 or 0x01 set,
 * latch p1_pending_action and fire its anim via pending_anim_lut_3cda. */
void p1_try_trigger_pending_action(void)
{
    if (p1_queued_action_code == 0 &&
        (((input_state & 0x10) != 0) || ((input_state & 1) != 0))) {
        p1_latched_action = p1_pending_action;
        p1_set_cell_animation((char)pending_anim_lut_3cda[p1_pending_action]);
    }
    return;
}

/* p1_try_jump_action — 1000:6587
 * If not overridden, p1_pending_action==2 and the jump input bit (2) is set: play
 * the jump sound, record the cell, arm a 0x34-tick move and run the jump FX 0x34. */
void p1_try_jump_action(void)
{
    u8 sound_id;

    if (move_override == 0 && p1_pending_action == 0x02 && (input_state & 2) != 0) {
        if (sound_device_state == 4) {
            sound_id = 9;
        } else {
            sound_id = 4;
        }
        play_sound(sound_id);
        anim_target_cell = p1_cell;
        p1_jump_move_ticks = 0x34;
        apply_cell_animation(0x34);
    }
    return;
}

/* ── The move-action micro-handlers (sound + step) ─────────────────────────────── */

/* p1_move_step_with_sound — 1000:6648
 * Play the per-action move sound (device-selected table) if nonzero, then step the
 * move animation via p1_set_cell_animation_no_override(pending_anim_lut_3d0a[...]). */
void p1_move_step_with_sound(void)
{
    u8 sound_id;

    if (sound_device_state == 4) {
        sound_id = move_sound_lut_opl_25ae[p1_pending_action];
    } else {
        sound_id = move_sound_lut_std_25de[p1_pending_action];
    }
    if (sound_id != 0) {
        play_sound(sound_id);
    }
    p1_set_cell_animation_no_override((char)pending_anim_lut_3d0a[p1_pending_action]);
    return;
}

/* move_step_last_variant — 1000:66d8
 * Unless the prev mode was 0x03/0x0f, refresh the pending-action view via
 * p1_dispatch_pending_action(pending_anim_lut_3caa); then, if the column counter
 * != 7, apply the contact action contact_sound_lut_35de[p1_contact_code]. */
void move_step_last_variant(void)
{
    if (prev_game_mode != 0x03 && prev_game_mode != 0x0f) {
        p1_dispatch_pending_action(pending_anim_lut_3caa);
    }
    if (p1_step_col_count != 7) {
        apply_contact_action(contact_sound_lut_35de[p1_contact_code]);
    }
    return;
}

/* move_step_landed — 1000:6717
 * Record p1_cell as the anim target, run p1_step_landed; if the landed
 * anim-channel index is '[' (0x5b), clear level_complete_flag. */
void move_step_landed(void)
{
    anim_target_cell = p1_cell;
    p1_step_landed();
    if (g_anim_channel_idx == 0x5b) {
        level_complete_flag = 0;
    }
    return;
}

/* ── First/last-step move variants reconstructed in Phase 9 T2 ────────────────────
 * These nine move-step micro-handlers (6699/6748/6789/67ca/67e2/67fb/6813 + the
 * 6326/651c siblings of check_exit_tile_vert/cursor_move_right) are targets of
 * move_step_dispatch_tbl but were not reconstructed in earlier phases.  Ported 1:1
 * from the live Ghidra decomp + raw disasm (verified fresh via MCP, 2026-06) so the
 * Phase-9 offset→fn resolver maps EVERY table offset to a real host function. */

/* move_step_first_variant — 1000:6699
 * First-step counterpart of move_step_last_variant (66d8): unless prev mode 3/0xf,
 * refresh the pending-action view (pending_anim_lut_3c7a); then, if
 * p1_step_col_count (0x855e) != 0, apply contact_action_lut_35be[p1_contact_code].
 * Disasm: 66a5 CMP[0x8552],3 / 66ac CMP[0x8552],0xf / 66be CMP[0x855e],0 /
 * 66c5 [0x8551] index 0x35be / 66d1 CALL 6a89(apply_contact_action). */
void move_step_first_variant(void)
{
    if (prev_game_mode != 0x03 && prev_game_mode != 0x0f) {
        p1_dispatch_pending_action(pending_anim_lut_3c7a);
    }
    if (p1_step_col_count != 0) {
        apply_contact_action(contact_action_lut_35be[p1_contact_code]);
    }
    return;
}

/* move_step_first_variant_b — 1000:6748
 * Play the move sound (0x2f on the OPL/charger device, else 8), run the cell FX 0x18,
 * then if p1_step_col_count (0x855e) != 0 apply contact_action_lut_35fe[p1_contact_code].
 * Disasm: 6754 CMP[0x689c],4 → 0x2f:8 / 6767 FX 0x18 / 676f CMP[0x855e],0 /
 * 6776 [0x8551] index 0x35fe / 6782 CALL 6a89. */
void move_step_first_variant_b(void)
{
    u8 sound_id;

    sound_id = (sound_device_state == 4) ? 0x2f : 0x08;
    play_sound(sound_id);
    p1_trigger_cell_animation(0x18);
    if (p1_step_col_count != 0) {
        apply_contact_action(contact_action_lut_35fe[p1_contact_code]);
    }
    return;
}

/* move_step_last_variant_b — 1000:6789
 * Last-step counterpart of 6748: play the move sound (0x2f:8), run the cell FX 0x19,
 * then if p1_step_col_count (0x855e) != 7 apply contact_action_lut_361e[p1_contact_code].
 * Disasm: 6795 CMP[0x689c],4 / 67a8 FX 0x19 / 67b0 CMP[0x855e],7 /
 * 67b7 [0x8551] index 0x361e / 67c3 CALL 6a89. */
void move_step_last_variant_b(void)
{
    u8 sound_id;

    sound_id = (sound_device_state == 4) ? 0x2f : 0x08;
    play_sound(sound_id);
    p1_trigger_cell_animation(0x19);
    if (p1_step_col_count != 7) {
        apply_contact_action(contact_action_lut_361e[p1_contact_code]);
    }
    return;
}

/* move_step_body_c — 1000:67e2
 * Dispatch the contact action for the current cell via contact_action_lut_363e.
 * Disasm: 67ef MOV AX,0x363e / 67f3 CALL 686a(p1_dispatch_contact_action). */
void move_step_body_c(void)
{
    p1_dispatch_contact_action(contact_action_lut_363e);
    return;
}

/* move_step_first_gate_c — 1000:67ca
 * First-step gate: if p1_step_col_count (0x855e) != 0, run move_step_body_c.
 * Disasm: 67ce CMP[0x855e],0 / 67d8 CALL 67e2(move_step_body_c). */
void move_step_first_gate_c(void)
{
    if (p1_step_col_count != 0) {
        move_step_body_c();
    }
    return;
}

/* move_step_last_body_c — 1000:6813
 * Save p1_cell_prev = p1_cell, then dispatch the contact action via contact_action_lut_365e.
 * Disasm: 681f MOV AL,[0x856e] / 6822 MOV[0x8570],AL (p1_cell_prev=p1_cell) /
 * 6826 MOV AX,0x365e / 682a CALL 686a(p1_dispatch_contact_action). */
void move_step_last_body_c(void)
{
    p1_cell_prev = p1_cell;
    p1_dispatch_contact_action(contact_action_lut_365e);
    return;
}

/* move_step_last_gate_c — 1000:67fb
 * Last-step gate: if p1_step_col_count (0x855e) != 7, run move_step_last_body_c.
 * Disasm: 67ff CMP[0x855e],7 / 6809 CALL 6813(move_step_last_body_c). */
void move_step_last_gate_c(void)
{
    if (p1_step_col_count != 7) {
        move_step_last_body_c();
    }
    return;
}

/* check_exit_tile_horiz — 1000:6326
 * Horizontal exit-tile detection (sibling of check_exit_tile_vert @ 6372 in items.c):
 * if p1_step_col_count (0x855e) != 0 AND the neighbour tile at
 * tilemap[p1_cell + 0x2f] (one column back) is the exit tile 0x0c, commit the
 * end-of-level transition (p1_move_step_idx=0, physics_frozen=1, enter_game_mode(0x2e),
 * play exit sound 0x0d on the OPL device else 0x03).
 * Disasm: 6332 CMP[0x855e],0 / 6344 CMP ES:[BX+0x2f],0xc / 634b [0x792a]=0 /
 * 6350 [0xa0ce]=1 / 6358 CALL enter_game_mode(0x2e) / 635d CMP[0x689c],4 → 0xd:3. */
void check_exit_tile_horiz(void)
{
    u8 sound_id;

    if ((p1_step_col_count != 0) &&
        ((s8)tilemap[(u16)p1_cell + TILE_CONTACT_LAYER_OFF - 1] == '\f')) {
        p1_move_step_idx = 0;
        physics_frozen = 1;
        enter_game_mode(0x2e);
        sound_id = (sound_device_state == 4) ? 0x0d : 0x03;
        play_sound(sound_id);
    }
    return;
}

/* cursor_move_left — 1000:651c
 * Cursor left (sibling of cursor_move_right @ 6535): redraw via input_state_mask_0f,
 * p1_cell -= 1, decrement the column counter p1_step_col_count (0x855e).
 * Disasm: 6528 CALL 6611(input_state_mask_0f) / 652b DEC[0x856e] / 652f DEC[0x855e]. */
void cursor_move_left(void)
{
    input_state_mask_0f();
    p1_cell = (u8)(p1_cell - 1);
    p1_step_col_count = (u8)(p1_step_col_count - 1);
    return;
}

/* move_step_noop — 1000:673a  (empty move action; body was only the stack check) */
void move_step_noop(void)
{
    return;
}

/* move_step_noop_sentinel — 1000:7111
 * The common dispatch-table filler offset.  No function is defined at 1000:7111 in
 * the decomp (it is a bare `RET`/sentinel slot the dispatch table points at for
 * "no per-step action"); reconstructed here as an empty handler so the host map
 * can route the (very common) filler slot.
 * RECONSTRUCTION FIDELITY: no Ghidra function exists at 0x7111 — modeled as a no-op
 * matching the slot's "do nothing" semantics. */
void move_step_noop_sentinel(void)
{
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  PHASE 9, TASK 1 — PLAYER-1 CONTACT-ACTION HANDLER FAMILY
 *  --------------------------------------------------------------------------
 *  The move_step_dispatch_tbl (DGROUP 0x43c0, above) routes a number of mode/step
 *  slots to this family of contact-action micro-handlers (engine offsets, dumped
 *  into the table verbatim): p1_apply_contact_action_main (6832, mode 0x29),
 *  _prev (684b, 0x2a), _at_start (6890, 0x38/0x3a), _before_end (68bb, 0x39/0x3b),
 *  _at_start_b (68e6, 0x34/0x36), _before_end_b (6922, 0x35/0x37),
 *  _tbl_367e (68fe, 0x1a), _tbl_369e (693a, 0x1b); plus move_step_*_variant
 *  (66d8 etc.) all funnel into apply_contact_action.  Each contact handler maps
 *  p1_contact_code through a DGROUP byte LUT to an *action code*, then runs
 *  apply_contact_action(action), which plays the contact sound + claims a
 *  channel-B animation slot keyed by p1_cell_prev + stamps the action's tile into
 *  the +0x30 tilemap layer.
 *
 *  Ported 1:1 from the live Ghidra decomp + raw disassembly (verified fresh via
 *  MCP, 2026-06).  Each fn cites its engine address.  This un-stubs the no-op
 *  apply_contact_action that was a game_stubs.c placeholder (Phases 2–8): every
 *  move_step_dispatch_tbl target offset now resolves to a real function (the
 *  prerequisite for the Phase-9 T2 offset->fn resolver).
 *
 *  The six dispatch LUTs (35fe/361e/363e/365e/367e/369e) + the two contact-sound
 *  LUTs (272e/274e) + the contact tile-def far-ptr table (3256/3258) are dumped
 *  byte-exact from BUMPY_unpacked.exe (DGROUP file base 0x11440); see the blobs
 *  below.
 *
 *  RECONSTRUCTION FIDELITY (channel-B record/table access):
 *    - apply_contact_action is the channel-B analogue of apply_cell_animation
 *      (anim.c, 69aa): it scans anim_channels_b_tbl (4 slots, DGROUP 0x4cbc/0x4cbe,
 *      OWNED by anim.c) for a slot whose cell == p1_cell_prev (or a free slot), and
 *      on claim writes slot->cell = p1_cell_prev, slot->stream ptr = tile_def[+2..+5],
 *      slot->active = 1, and stamps tilemap[p1_cell_prev + 0x30] = tile_def[0].
 *    - The engine stores the action code into DGROUP byte 0x8566 as last_contact_action
 *      at function entry.  That byte is the SAME DGROUP byte the channel-B stepper
 *      reuses as its loop index (anim.c labels it anim_b_loop_idx; the decomp labels
 *      it last_contact_action in step_anim_channels_b).  To keep one owner per global
 *      we write that engine store through the anim.c-owned anim_b_loop_idx symbol;
 *      see the alias note at anim.c:268.  No new symbol is introduced.
 *    - contact_tiledef_tbl (0x3256/0x3258, action*4 -> far ptr) is modeled as a raw
 *      byte blob (the same representation anim.c's anim_a_tiledef_tbl / player.c's
 *      mode_script_tbl use); an entry's far ptr is rebuilt with MK_FP at the use
 *      site.  Its seg halves are the static-image link-time DGROUP seg 0x103b; like
 *      anim.c's far-ptr tables the host harness seeds/relocates them for the gate.
 *    - p1_dispatch_contact_action takes the action_table as a far-pointer argument
 *      (the engine passes DS:0x363e / DS:0x365e); the _main/_prev wrappers pass a
 *      pointer to the corresponding dumped LUT.  The remaining handlers index the
 *      LUT as a near DGROUP read (p1_contact_code + 0x35fe etc.) — modeled as a
 *      direct array index, identical result.
 *    - play_sound (6e11) stays the Phase-6 sound leaf (already linked from sound.c).
 * ════════════════════════════════════════════════════════════════════════════ */
#include "anim.h"   /* anim_chan_rec, anim_channels_b_tbl (0x4cbc), anim_b_loop_idx (0x8566) */

/* ── DGROUP contact-action dispatch LUTs (p1_contact_code -> action code) ──────────
 * Six overlapping 0x30-byte byte tables at DGROUP 0x35fe..0x369e+0x30 (file base
 * 0x11440).  In the engine these overlap in memory; each handler reads only
 * table[p1_contact_code] (p1_contact_code < 0x30), so reproducing each as its own
 * 0x30-byte array yields identical index results (same idiom as player.c's
 * tile_followup_action_lut / contact_sound_lut_35de).  Dumped byte-exact. */
/* DGROUP 0x35fe (file 0x14a3e) — p1_apply_contact_action_at_start LUT. */
u8 contact_action_lut_35fe[0x100] = {
    0x00,0x01,0x02,0x03,0x04,0x17,0x05,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x02,0x03,0x04,0x17,0x00,0x06,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x14,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x14,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x05,0x00,0x01,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0e,
    0x00,0x11,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x00,0x00,0x00,0x00,0x00,0x10,
    0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x01,0x01,0x01,0x01,0x00,0x01,0x01,0x01,0x01,
};
/* DGROUP 0x361e (file 0x14a5e) — p1_apply_contact_action_before_end LUT. */
u8 contact_action_lut_361e[0x100] = {
    0x00,0x01,0x02,0x03,0x04,0x17,0x00,0x06,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x14,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x14,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x05,0x00,0x01,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0e,
    0x00,0x11,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x00,0x00,0x00,0x00,0x00,0x10,
    0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x01,0x01,0x01,0x01,0x00,0x01,0x01,0x01,0x01,
    0x01,0x01,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x01,0x01,0x01,0x01,0x01,0x10,
    0x30,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
/* DGROUP 0x363e (file 0x14a7e) — p1_apply_contact_action_main LUT. */
u8 contact_action_lut_363e[0x100] = {
    0x00,0x01,0x11,0x12,0x13,0x17,0x14,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x14,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x05,0x00,0x01,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0e,
    0x00,0x11,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x00,0x00,0x00,0x00,0x00,0x10,
    0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x01,0x01,0x01,0x01,0x00,0x01,0x01,0x01,0x01,
    0x01,0x01,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x01,0x01,0x01,0x01,0x01,0x10,
    0x30,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x02,0x02,0x02,0x02,0x01,0x02,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x02,
    0x02,0x02,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x02,0x02,0x02,0x02,0x02,0x10,
};
/* DGROUP 0x365e (file 0x14a9e) — p1_apply_contact_action_prev LUT. */
u8 contact_action_lut_365e[0x100] = {
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x14,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x05,0x00,0x01,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0e,
    0x00,0x11,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x00,0x00,0x00,0x00,0x00,0x10,
    0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x01,0x01,0x01,0x01,0x00,0x01,0x01,0x01,0x01,
    0x01,0x01,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x01,0x01,0x01,0x01,0x01,0x10,
    0x30,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x02,0x02,0x02,0x02,0x01,0x02,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x02,
    0x02,0x02,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x02,0x02,0x02,0x02,0x02,0x10,
    0x30,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x11,0x00,0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
/* DGROUP 0x367e (file 0x14abe) — p1_apply_contact_action_tbl_367e LUT. */
u8 contact_action_lut_367e[0x100] = {
    0x00,0x01,0x11,0x12,0x13,0x17,0x14,0x00,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x05,0x00,0x01,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0e,
    0x00,0x11,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x00,0x00,0x00,0x00,0x00,0x10,
    0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x01,0x01,0x01,0x01,0x00,0x01,0x01,0x01,0x01,
    0x01,0x01,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x01,0x01,0x01,0x01,0x01,0x10,
    0x30,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x02,0x02,0x02,0x02,0x01,0x02,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x02,
    0x02,0x02,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x02,0x02,0x02,0x02,0x02,0x10,
    0x30,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x11,0x00,0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x11,0x11,0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x11,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
/* DGROUP 0x369e (file 0x14ade) — p1_apply_contact_action_tbl_369e LUT. */
u8 contact_action_lut_369e[0x100] = {
    0x00,0x01,0x11,0x12,0x13,0x17,0x00,0x15,0x00,0x08,0x09,0x0a,0x00,0x00,0x0b,0x0c,
    0x0d,0x0e,0x0f,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x05,0x00,0x01,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0e,
    0x00,0x11,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x00,0x00,0x00,0x00,0x00,0x10,
    0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x01,0x01,0x01,0x01,0x00,0x01,0x01,0x01,0x01,
    0x01,0x01,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x01,0x01,0x01,0x01,0x01,0x10,
    0x30,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x02,0x02,0x02,0x02,0x01,0x02,0x02,0x02,0x02,0x02,0x00,0x02,0x02,0x02,0x02,
    0x02,0x02,0x10,0x03,0x1a,0x1b,0x00,0x01,0x02,0x03,0x02,0x02,0x02,0x02,0x02,0x10,
    0x30,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x11,0x00,0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x11,0x11,0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x11,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x08,0x09,0x00,0x00,0x00,0x00,0x00,0x08,0x09,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x09,0x08,0x09,0x08,0x09,0x08,0x09,0x08,0x08,0x09,0x00,0x00,0x00,0x00,
};

/* ── DGROUP contact-sound LUTs (action code -> sound id; device-selected) ──────────
 * apply_contact_action selects the OPL table (0x272e) when sound_device_state==4,
 * else the std table (0x274e).  0x30-byte tables, dumped byte-exact (they overlap
 * by 0x10 bytes in the image, like the dispatch LUTs above). */
/* DGROUP 0x272e (file 0x13b6e) — contact-sound LUT, OPL/charger device. */
u8  contact_sound_lut_opl_272e[0x30] = {
    0x00,0x00,0x2f,0x2f,0x2f,0x2f,0x2f,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x07,0x2f,0x2f,0x2f,0x2f,0x2f,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0d,0x0d,
    0x0d,0x0d,0x0d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
/* DGROUP 0x274e (file 0x13b8e) — contact-sound LUT, std device. */
u8  contact_sound_lut_std_274e[0x30] = {
    0x00,0x00,0x0d,0x0d,0x0d,0x0d,0x0d,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x07,0x0d,0x0d,0x0d,0x0d,0x0d,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x03,0x03,
    0x03,0x03,0x03,0x03,0x0e,0x10,0x10,0x10,0x12,0x0e,0x0a,0x0a
};

/* ── DGROUP contact tile-def far-ptr table (action*4 -> far ptr) ───────────────────
 * apply_contact_action reads tile_def = *(far ptr)(action*4 + 0x3256/0x3258).  A
 * table of 4-byte FAR POINTERS (off @ a*4+0, seg @ a*4+2) into the per-action
 * tile-def stream.  Modeled as a raw byte blob (same as anim.c's anim_a_tiledef_tbl);
 * the far ptr is rebuilt with MK_FP at the use site.  Sized to 256 entries so any
 * action byte indexes in range (actions seen are <= 0x17).  Dumped byte-exact for
 * actions 0..0x17 (the static image's range); the seg halves are the static
 * link-time DGROUP seg 0x103b (host-seeded/relocated for the gate, see fidelity note
 * above and anim.c's far-ptr-table comment). */
#define CONTACT_TILEDEF_TBL_LEN  (256 * 4)
u8  contact_tiledef_tbl[CONTACT_TILEDEF_TBL_LEN] = {
    0x00,0x00,0x00,0x00, 0x6a,0x30,0x3b,0x10, 0x82,0x30,0x3b,0x10, 0x9a,0x30,0x3b,0x10,
    0xb2,0x30,0x3b,0x10, 0xc6,0x30,0x3b,0x10, 0xda,0x30,0x3b,0x10, 0xf2,0x30,0x3b,0x10,
    0x00,0x31,0x3b,0x10, 0x0e,0x31,0x3b,0x10, 0x26,0x31,0x3b,0x10, 0x3a,0x31,0x3b,0x10,
    0x4e,0x31,0x3b,0x10, 0x62,0x31,0x3b,0x10, 0x76,0x31,0x3b,0x10, 0x8a,0x31,0x3b,0x10,
    0x98,0x31,0x3b,0x10, 0xae,0x31,0x3b,0x10, 0xc4,0x31,0x3b,0x10, 0xda,0x31,0x3b,0x10,
    0xee,0x31,0x3b,0x10, 0x02,0x32,0x3b,0x10, 0x26,0x32,0x3b,0x10, 0x3c,0x32,0x3b,0x10
    /* actions 0x18..0xff: zero (no tile-def; action codes never exceed 0x17) */
};

/* DGROUP 0x8566 — last_contact_action: the engine stores the action code here at
 * apply_contact_action entry; it is the SAME byte the channel-B stepper reuses as
 * anim_b_loop_idx (OWNED by anim.c).  Aliased via that symbol — see the fidelity
 * note above. */

/*
 * apply_contact_action — 1000:6a89
 * --------------------------------------------------------------------------
 * Apply a contact/tile action by code: store it as last_contact_action; if 0,
 * return.  Otherwise play its contact sound (table selected by sound_device_state
 * == 4 -> OPL, else std), then claim a channel-B animation slot keyed by
 * p1_cell_prev and stamp the action's tile into the +0x30 tilemap layer.
 *
 * The slot SCAN mirrors apply_cell_animation (anim.c) exactly, but on channel B
 * (anim_channels_b_tbl, 4 slots) and keyed by p1_cell_prev (not anim_target_cell):
 *   Scan 1 (do/while): advance slot_idx over the B table skipping ACTIVE slots
 *     (active=='\0' continues); on the first non-zero slot, if it is the 0xFF
 *     terminator -> reset slot_idx=0 and fall into Scan 2 (LAB_6b26); otherwise if
 *     its [1]==p1_cell_prev -> claim it (LAB_6b78); else loop Scan 1 again.
 *   Scan 2 (LAB_6b26): advance from slot 0 looking for a FREE ('\0') slot, stopping
 *     on '\0' (claim) or 0xFF (give up -> return).  A non-'\0', non-0xFF byte keeps
 *     scanning.  On reaching '\0' -> claim (LAB_6b78); else (0xFF) -> return.
 * On claim (LAB_6b78): write [1]=p1_cell_prev, stamp tilemap[p1_cell_prev+0x30] =
 * tile_def[0], copy tile_def[2..5] -> slot[2..5] (the stream far ptr), set [0]=1.
 *
 * Verified against disasm 1000:6a89–6bb4.  See the RECONSTRUCTION FIDELITY banner
 * above for the channel-B / last_contact_action alias / table-blob notes.
 */
void apply_contact_action(u8 action_code)
{
    u8                sound_id;
    const u8 __far   *tile_def;     /* tile_def_ptr — far ptr at tiledef_tbl[a*4] */
    u16               tdef_off, tdef_seg;
    u8                slot_idx;
    u8                cell_key;      /* p1_cell_prev latched (bVar1) */
    anim_chan_rec __far *slot;       /* slot_entry_ptr / slot_ptr                  */

    anim_b_loop_idx = action_code;   /* last_contact_action @ DGROUP 0x8566 (alias) */
    if (action_code == 0) {
        return;
    }
    /* contact sound: device-selected LUT (action_code index). */
    if (sound_device_state == 4) {
        sound_id = contact_sound_lut_opl_272e[action_code];
    } else {
        sound_id = contact_sound_lut_std_274e[action_code];
    }
    if (sound_id != 0) {
        play_sound(sound_id);
    }

    cell_key = p1_cell_prev;
    /* tile_def far ptr = contact_tiledef_tbl[action_code*4] (off @ +0, seg @ +2). */
    tdef_off = *(u16 *)(contact_tiledef_tbl + (u16)action_code * 4 + 0);
    tdef_seg = *(u16 *)(contact_tiledef_tbl + (u16)action_code * 4 + 2);
    tile_def = (const u8 __far *)MK_FP(tdef_seg, tdef_off);

    /* ── Scan 1: skip active slots; act on the first non-active slot. ─────────────── */
    slot_idx = 0;
    do {
        do {
            slot = anim_channels_b_tbl[slot_idx];
            slot_idx = (u8)(slot_idx + 1);
        } while (slot->active == '\0');
        if (slot->active == (u8)0xff) {
            slot_idx = 0;
            goto LAB_6b26;
        }
    } while (slot->cell != p1_cell_prev);
    goto LAB_6b78;

    /* ── Scan 2: from slot 0, find a FREE ('\0') slot; 0xFF terminator gives up. ──── */
    while (slot->active != (u8)0xff) {
LAB_6b26:
        slot = anim_channels_b_tbl[slot_idx];
        slot_idx = (u8)(slot_idx + 1);
        if (slot->active == '\0') {
            break;
        }
    }
    if (slot->active != '\0') {
        return;   /* exited on 0xFF without a free slot */
    }

LAB_6b78:
    slot->cell = p1_cell_prev;
    tilemap[(u16)cell_key + TILE_CONTACT_LAYER_OFF] = tile_def[0];
    slot->stream_off = *(u16 __far *)(tile_def + 2);
    slot->stream_seg = *(u16 __far *)(tile_def + 4);
    slot->active = 1;
    return;
}

/*
 * p1_dispatch_contact_action — 1000:686a
 * --------------------------------------------------------------------------
 * Look up action_table[p1_contact_code] and apply it via apply_contact_action;
 * clear input_state.  The engine passes action_table as a far pointer (DS:offset);
 * the _main/_prev wrappers pass a pointer to the corresponding dumped DGROUP LUT.
 */
void p1_dispatch_contact_action(u8 *action_table)
{
    apply_contact_action(action_table[p1_contact_code]);
    input_state = 0;
    return;
}

/* p1_apply_contact_action_main — 1000:6832
 * Dispatch contact action for the current cell via the action table at 0x363e. */
void p1_apply_contact_action_main(void)
{
    p1_dispatch_contact_action(contact_action_lut_363e);
    return;
}

/* p1_apply_contact_action_prev — 1000:684b
 * Save p1_cell into p1_cell_prev, then dispatch via the action table at 0x365e. */
void p1_apply_contact_action_prev(void)
{
    p1_cell_prev = p1_cell;
    p1_dispatch_contact_action(contact_action_lut_365e);
    return;
}

/* p1_apply_contact_action_at_start — 1000:6890
 * If a move step is in progress (p1_step_col_count @ 0x855e != 0) apply the contact
 * action for p1_contact_code via table 0x35fe; clears input_state. */
void p1_apply_contact_action_at_start(void)
{
    if (p1_step_col_count != 0) {                 /* 0x855e */
        apply_contact_action(contact_action_lut_35fe[p1_contact_code]);
        input_state = 0;
    }
    return;
}

/* p1_apply_contact_action_before_end — 1000:68bb
 * Unless the move step is at its final frame (p1_step_col_count @ 0x855e != 7) apply
 * the contact action for p1_contact_code via table 0x361e; clears input_state. */
void p1_apply_contact_action_before_end(void)
{
    if (p1_step_col_count != 7) {                 /* 0x855e */
        apply_contact_action(contact_action_lut_361e[p1_contact_code]);
        input_state = 0;
    }
    return;
}

/* p1_apply_contact_action_tbl_367e — 1000:68fe
 * Apply the contact action for p1_contact_code via table 0x367e; clears
 * input_state. */
void p1_apply_contact_action_tbl_367e(void)
{
    apply_contact_action(contact_action_lut_367e[p1_contact_code]);
    input_state = 0;
    return;
}

/* p1_apply_contact_action_tbl_369e — 1000:693a
 * Apply the contact action for p1_contact_code via table 0x369e; clears
 * input_state. */
void p1_apply_contact_action_tbl_369e(void)
{
    apply_contact_action(contact_action_lut_369e[p1_contact_code]);
    input_state = 0;
    return;
}

/* p1_apply_contact_action_at_start_b — 1000:68e6
 * If a move step is in progress (p1_step_col_count @ 0x855e != 0) apply the contact
 * action via table 0x367e (p1_apply_contact_action_tbl_367e). */
void p1_apply_contact_action_at_start_b(void)
{
    if (p1_step_col_count != 0) {                 /* 0x855e */
        p1_apply_contact_action_tbl_367e();
    }
    return;
}

/* p1_apply_contact_action_before_end_b — 1000:6922
 * Unless the move step is at its final frame (p1_step_col_count @ 0x855e != 7) apply
 * the contact action via table 0x369e (p1_apply_contact_action_tbl_369e). */
void p1_apply_contact_action_before_end_b(void)
{
    if (p1_step_col_count != 7) {                 /* 0x855e */
        p1_apply_contact_action_tbl_369e();
    }
    return;
}

/* ── The two move-step delegates check_tile_below_ladder_or_land tail-calls ────── */

/* p1_exec_pending_action — 1000:465e
 * Look up p1_pending_action in pending_action_lut_36be and run exec_move_action
 * with the mapped action code. */
void p1_exec_pending_action(void)
{
    exec_move_action(pending_action_lut_36be[p1_pending_action]);
    return;
}

/* move_down_step — 1000:253f
 * Downward move step: handle pending-action tiles (0x0f teleport leaf,
 * 0x12/0x1f settle-wrap), mask input bit 1, play the step sound, advance the
 * jump_step_counter (== move_step_count, DGROUP 0x824c); on the 9th step relocate
 * the view cell-8 and trigger FX 0x24, then enter mode 0x0d and dispatch.
 *
 * RECONSTRUCTION FIDELITY: FUN_1000_4802 (pending==0x0f teleport leaf) and
 * FUN_1000_22b0 (settle-wrap) stay extern stubs; the trailing dispatch_move_step()
 * call-through routes a RAW engine offset on the host, so the harness drives this
 * substate via its delegate-route accounting, not a host call-through of dispatch. */
void move_down_step(void)
{
    u8 sound_id;

    if (p1_pending_action == 0x0f) {
        FUN_1000_4802();
    } else if (p1_pending_action == 0x12 || p1_pending_action == 0x1f) {
        FUN_1000_22b0();
    }
    input_state_mask_1d();
    if (sound_device_state == 4) {
        sound_id = 9;
    } else {
        sound_id = 0x14;
    }
    play_sound(sound_id);
    move_step_count = (u8)(move_step_count + 1);   /* jump_step_counter (0x824c) */
    if (move_step_count == 9) {
        anim_target_cell = (u8)(p1_cell - 8);
        apply_cell_animation(0x24);
        move_step_count = 0;
    }
    enter_game_mode(0x0d);
    dispatch_move_step();
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  PHASE 9, TASK 2 — move_step_dispatch_tbl OFFSET → HOST FUNCTION RESOLVER
 *  --------------------------------------------------------------------------
 *  move_step_dispatch_tbl (DGROUP 0x43c0, above) is a byte-exact dump of the
 *  engine's 2D table of little-endian 16-bit NEAR offsets.  dispatch_move_step
 *  reads the raw offset for [game_mode][p1_move_step_idx] and routes it here.
 *
 *  RECONSTRUCTION FIDELITY: the engine `CALL`s through those near offsets directly
 *  (the offset IS the code address in real mode).  The host cannot relocate a bare
 *  16-bit near offset to a reconstructed function, so this switch is the single
 *  isolated host-execution deviation for the move-step dispatch: each distinct
 *  offset in the table maps to the already-reconstructed host function that lives at
 *  1000:<offset>.  The table bytes and the dispatch_move_step index arithmetic stay
 *  byte/structure-identical to the original; only the final indirect call is
 *  re-expressed as an offset→fn lookup.
 *
 *  0x7111 → move_step_noop_sentinel (the common "no per-step action" filler slot;
 *           no real function exists at 1000:7111 — it is a bare sentinel offset).
 *  0x0000 → a documented no-op terminator: the trailing 0-padding of each mode row
 *           (the engine never dispatches a 0 offset; p1_move_step_idx never indexes a
 *           terminator slot because the move sequence ends first).  Routed to the
 *           noop sentinel so the host map is total.
 *  Every other offset maps to its reconstructed host function (verified 1:1 via
 *  Ghidra MCP get_function_by_address 0x1000:<offset>, 2026-06). */
void (*move_step_handler_for_offset(u16 off))(void)
{
    switch (off) {
    case 0x0000: return move_step_noop_sentinel; /* terminator (never dispatched) */
    case 0x7111: return move_step_noop_sentinel; /* common filler slot */
    case 0x6305: return play_exit_sound;
    case 0x6326: return check_exit_tile_horiz;
    case 0x6372: return check_exit_tile_vert;
    case 0x640c: return play_contact_sound;
    case 0x645d: return play_pickup_sound;
    case 0x647e: return play_state_sound_79b9;
    case 0x64c1: return play_event_sound_64c1;
    case 0x64e2: return cursor_move_up;
    case 0x64ff: return cursor_move_down;
    case 0x651c: return cursor_move_left;
    case 0x6535: return cursor_move_right;
    case 0x654e: return p1_try_trigger_pending_action;
    case 0x6587: return p1_try_jump_action;
    case 0x65d2: return input_state_clear;
    case 0x65e5: return input_state_mask_10;
    case 0x65fb: return input_state_mask_1d;
    case 0x6611: return input_state_mask_0f;
    case 0x6627: return move_step_read_item;
    case 0x6648: return p1_move_step_with_sound;
    case 0x6699: return move_step_first_variant;
    case 0x66d8: return move_step_last_variant;
    case 0x6717: return move_step_landed;
    case 0x673a: return move_step_noop;
    case 0x6748: return move_step_first_variant_b;
    case 0x6789: return move_step_last_variant_b;
    case 0x67ca: return move_step_first_gate_c;
    case 0x67e2: return move_step_body_c;
    case 0x67fb: return move_step_last_gate_c;
    case 0x6813: return move_step_last_body_c;
    case 0x6832: return p1_apply_contact_action_main;
    case 0x684b: return p1_apply_contact_action_prev;
    case 0x6890: return p1_apply_contact_action_at_start;
    case 0x68bb: return p1_apply_contact_action_before_end;
    case 0x68e6: return p1_apply_contact_action_at_start_b;
    case 0x68fe: return p1_apply_contact_action_tbl_367e;
    case 0x6922: return p1_apply_contact_action_before_end_b;
    case 0x693a: return p1_apply_contact_action_tbl_369e;
    default:     return move_step_noop_sentinel; /* unreachable: table is closed */
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  PHASE 9, TASK 2 — PLAYER-SPINE INPUT DISPATCH
 *  Reconstructed 1:1 from the live Ghidra decomp + raw disasm (verified fresh via
 *  MCP, 2026-06).  These were anticipated to land "with their player-spine callees"
 *  (see input.h) and un-stub handle_gameplay_input from game_stubs.c.
 * ════════════════════════════════════════════════════════════════════════════ */

/* Cross-module DGROUP globals + leaves consumed by the gameplay-input spine. */
extern u8  timing_flag_accumulator;   /* screens.c — DGROUP 0x854f (F1..F7 debug accumulator) */
extern u8  round_continue_flag;       /* game.c     — DGROUP 0x9d30 */
extern u8  pvp_collision_flag;        /* player2.c  — DGROUP 0xa1aa (Ghidra players_colliding) */

/* begin_physics_freeze — 1000:228d
 * Freeze physics, clear the move-step sub-index and the P1<->P2 collision flag, and
 * enter end-of-level game mode 0x2e.
 * Disasm: 2299 [0xa0ce]=1 (physics_frozen) / 22a0 [0x792a]=0 (p1_move_step_idx) /
 * 22a3 [0xa1aa]=0 (pvp_collision_flag) / 22a9 CALL enter_game_mode(0x2e). */
void begin_physics_freeze(void)
{
    physics_frozen = 1;
    p1_move_step_idx = 0;
    pvp_collision_flag = 0;
    enter_game_mode(0x2e);
    return;
}

/* handle_gameplay_input — 1000:1d26
 * --------------------------------------------------------------------------
 * The per-tick gameplay input handler (the player spine of the game loop).
 *
 * Debug/cheat function keys (polled via get_key_state, scancodes F1..F7 + 'D'):
 *   F1 (0x3b) → timing_flag_accumulator = 0
 *   F2 (0x3c) → timing_flag_accumulator = 0x88
 *   F3 (0x3d) → timing_flag_accumulator = 0xaa
 *   F4 (0x3e) → timing_flag_accumulator = 0xee
 *   F5 (0x3f) → timing_flag_accumulator = 0xff
 *   F6 (0x01) → run_physics_settle()                       [the raw key code is 0x01]
 *   D  (0x44) → frame_abort_flag = 1; round_continue_flag = 0; session_continue_flag = 0
 * The seven probes are an else-if chain (first match wins), matching the engine's
 * nested JZ ladder at 1d32..1dbb.
 *
 * Then: if NOT colliding (pvp_collision_flag == 0): read the tile under P1,
 * poll input, and either start a fresh movement (p1_move_steps_left == 0 →
 * p1_movement_dispatch) or continue the scripted move (→ dispatch_move_step).
 * If colliding: begin_physics_freeze().
 *
 * Disasm 1d26..1ddd verified: get_key_state arg ladder 0x3b/0x3c/0x3d/0x3e/0x3f/0x01/
 * 0x44; 1dbb CMP[0xa1aa],0; 1dc2 CALL begin_physics_freeze; 1dc7 CALL p1_read_tile_under;
 * 1dca CALL poll_input; 1dcd CMP[0x824d],0 → p1_movement_dispatch : dispatch_move_step.
 */
void handle_gameplay_input(void)
{
    if (get_key_state(0x3b) != 0) {
        timing_flag_accumulator = 0x00;
    } else if (get_key_state(0x3c) != 0) {
        timing_flag_accumulator = 0x88;
    } else if (get_key_state(0x3d) != 0) {
        timing_flag_accumulator = 0xaa;
    } else if (get_key_state(0x3e) != 0) {
        timing_flag_accumulator = 0xee;
    } else if (get_key_state(0x3f) != 0) {
        timing_flag_accumulator = 0xff;
    } else if (get_key_state(0x01) != 0) {
        run_physics_settle();
    } else if (get_key_state(0x44) != 0) {
        frame_abort_flag = 1;
        round_continue_flag = 0;
        session_continue_flag = 0;
    }

    if (pvp_collision_flag == 0) {
        p1_read_tile_under();
        poll_input();
        if (p1_move_steps_left == 0) {
            p1_movement_dispatch();
        } else {
            dispatch_move_step();
        }
    } else {
        begin_physics_freeze();
    }
    return;
}

/* ══ PHASE 9, TASK 3 — PLAYER-1 PER-TICK SPINE (grid / view / draw) ═════════════
 *
 * The symmetric P1 counterparts of the VALIDATED P2 per-tick functions in
 * player2.c (cross-checked structure-for-structure against them).  These are THIN
 * game-loop wrappers run once per tick by game_loop (src/game.c): recompute P1's
 * grid cell from its pixel position, slide the grid history, compute the view
 * scroll + present the P1 view (render/erase), draw the P1 sprite, and run any
 * deferred background restore.  Each is ported 1:1 from the live Ghidra decomp +
 * raw disassembly (addresses cited per-fn); the engine's Turbo-C stack-probe
 * prologue (`if (stack_check_limit <= &stack...) borland_stack_overflow()`) is the
 * per-fn stack-overflow guard, omitted here as elsewhere in src/ (a pure runtime
 * check, no observable state).
 *
 * P1 has NO `p1_cell != 0xff` presence guard (P1 is always present; that guard is
 * P2-specific — P2 may be absent on a 1-player level).  This is the one consistent
 * structural difference vs the p2_* versions: P1 runs unconditionally.
 *
 * RECONSTRUCTION FIDELITY (the present/blit leaf calls) — identical to player2.c's
 * P2 render wrappers: the engine's render leaves are FAR-POINTER calls taking the
 * descriptor's (off,seg):
 *     render_p1_view  -> render_player_view(p1_view off,seg)   (1000:93b8)
 *     erase_p1_view   -> restore_bg_view  (p1_erase_view off,seg)(1000:80bc)
 *     restore_bg_pending -> restore_bg_view(pending_erase_view off,seg)(1000:80bc)
 *     draw_p1_sprite  -> blit_sprite(0x792e, DS=0x203b)        (1000:942a)
 * Phase-0 reconstructed those render leaves as behavior-faithful semantic
 * reconstructions driven by host WORK BUFFERS (see src/gfx_overlay.h /
 * src/entity.c); `blit_sprite` was inlined into its validated pipeline stages and
 * has no callable symbol.  These game-loop wrappers do not hold that work-buffer
 * render context, so the present/blit LEAF is modeled here as a faithful-signature
 * stub (p1_render_view_leaf / p1_restore_view_leaf / p1_blit_sprite_leaf)
 * preserving the call site 1:1; the OBSERVABLE output — the descriptor field-writes
 * into the view / p1_sprite struct — IS produced here and is the validated gate
 * (tools/p1_spine_ctest.c, over the plane-exact Phase-0 blitter).  This mirrors
 * draw_p2_sprite / render_p2_view / erase_p2_view in player2.c exactly.
 * ════════════════════════════════════════════════════════════════════════════ */

/* P1 grid-cell history + view-scroll DGROUP globals (DEFINED here).  Addresses read
   live from the disassembly operands of the P1 fns (Ghidra DGROUP 0x203b offsets). */
s16 p1_grid_x_new;       /* DGROUP 0x9d36 — freshly-computed col (p1_update_grid_cell) */
s16 p1_grid_y_new;       /* DGROUP 0x9d38 — freshly-computed row */
s16 p1_grid_x;           /* DGROUP 0x857a — current col (after grid-history advance)  */
s16 p1_grid_y;           /* DGROUP 0x857c — current row */
s16 p1_grid_x_prev;      /* DGROUP 0x8882 — previous col */
s16 p1_grid_y_prev;      /* DGROUP 0x8e88 — previous row */
s16 p1_scroll_x;         /* DGROUP 0x9ba4 — P1 view scroll X (render/erase view) */
s16 p1_scroll_y;         /* DGROUP 0x9b9c — P1 view scroll Y */

/* P1 sprite-object + render/erase view-descriptor far pointers.  p1_sprite is the
   P1 object struct (DGROUP 0x8884/0x8886); p1_update_grid_cell reads its origin
   words at +0x14 (x) / +0x16 (y); draw_p1_sprite writes its x/y/frame fields.  It is
   OWNED by anim.c (the channel-A draw path also stamps it) — extern only here.
   p1_view (0x8b8/0x8ba) / p1_erase_view (0x8c4/0x8c6) are the render/restore view
   descriptors, owned here.  All set up by init_view_anim_descriptors (game.c). */
extern u8 __far *p1_sprite; /* anim.c — DGROUP 0x8884/0x8886 — P1 sprite/object far ptr */
u8 __far *p1_view;       /* DGROUP 0x8b8/0x8ba — render_player_view descriptor far ptr  */
u8 __far *p1_erase_view; /* DGROUP 0x8c4/0x8c6 — restore_bg_view descriptor far ptr     */

/* Deferred background-restore (pending-erase) DGROUP globals (restore_bg_pending). */
u8       pending_erase_count;  /* DGROUP 0xa1a8 — # pending cell restores               */
s16      pending_erase_x;      /* DGROUP 0x9b9a — pending cell col                       */
s16      pending_erase_y;      /* DGROUP 0x9ba2 — pending cell row                       */
u8 __far *pending_erase_view;  /* DGROUP 0x8e4/0x8e6 — restore_bg_view descriptor far ptr */

/* Present/blit leaves — faithful-signature stubs (see the FIDELITY note above). */
void p1_render_view_leaf(u8 __far *view);   /* render_player_view 1000:93b8 — leaf */
void p1_restore_view_leaf(u8 __far *view);  /* restore_bg_view    1000:80bc — leaf */
void p1_blit_sprite_leaf(u16 obj_off, u16 obj_seg); /* blit_sprite 1000:942a — leaf */

/*
 * p1_update_grid_cell — 1000:1473
 * --------------------------------------------------------------------------
 * Recompute P1's working grid cell (p1_grid_x_new/y_new) from the P1 pixel position
 * minus the sprite origin (read from the P1 sprite object at +0x14 / +0x16), then
 * clamp col to 0..0x12 and row to 0..0x16.  Mirror of p2_update_grid_cell (4b4e),
 * minus the P2 presence guard (P1 is always present).
 *
 * The X divide is an arithmetic SAR by 4 then -1; the Y divide is SAR by 3 (three
 * 1-bit SARs in the asm 149f/14a1/14a3) — reproduced as signed >> 4 / >> 3.
 */
void p1_update_grid_cell(void)
{
    s16 ox = ((sprite_obj_t __far *)p1_sprite)->anchor_x;        /* sprite origin x */
    s16 oy = ((sprite_obj_t __far *)p1_sprite)->anchor_y;        /* sprite origin y */

    p1_grid_x_new = (s16)(((p1_pixel_x - ox) >> 4) - 1);
    p1_grid_y_new = (s16)((p1_pixel_y - oy) >> 3);

    if (p1_grid_x_new < 0) {
        p1_grid_x_new = 0;
    } else if (p1_grid_x_new > 0x12) {
        p1_grid_x_new = 0x12;
    }
    if (p1_grid_y_new < 0) {
        p1_grid_y_new = 0;
    } else if (p1_grid_y_new > 0x16) {
        p1_grid_y_new = 0x16;
    }
    return;
}

/*
 * p1_advance_grid_history — 1000:138c
 * --------------------------------------------------------------------------
 * Slide P1's grid-cell history one step (cur -> prev, new -> cur).  Mirror of
 * p2_advance_grid_history (13b2), minus the P2 presence guard.
 *   p1_grid_x_prev = p1_grid_x; p1_grid_y_prev = p1_grid_y;
 *   p1_grid_x = p1_grid_x_new;  p1_grid_y = p1_grid_y_new;
 */
void p1_advance_grid_history(void)
{
    p1_grid_x_prev = p1_grid_x;
    p1_grid_y_prev = p1_grid_y;
    p1_grid_x = p1_grid_x_new;                        /* new col (0x9d36) -> cur */
    p1_grid_y = p1_grid_y_new;                        /* new row (0x9d38) -> cur */
    return;
}

/*
 * render_p1_view — 1000:1bd7
 * --------------------------------------------------------------------------
 * P1 view-copy (save-under): compute the P1 scroll offsets from the P1 grid cell,
 * write the view geometry into the render descriptor at p1_view (DGROUP 0x8b8/0x8ba),
 * then present it via render_player_view.  Mirror of render_p2_view (1c41), minus
 * the P2 presence guard.
 *   p1_scroll_x = (p1_grid_x > 0x10) ? 0x14 - p1_grid_x : 4
 *   p1_scroll_y = (p1_grid_y > 0x15) ? 0x19 - p1_grid_y : 4
 *   view[+6]  = p1_grid_x   view[+8]  = p1_grid_y
 *   view[+1e] = p1_scroll_x view[+20] = p1_scroll_y
 *   render_player_view(p1_view off, seg)                 (present leaf — see note)
 *
 * 1:1 with the asm 1bd7.. (MOV [0x9ba4],4 ; CMP [0x857a],0x10 ; ... ; LES BX,[0x8b8] ;
 * ES:[BX+6/8/1e/20] writes ; PUSH [0x8ba],[0x8b8] ; CALL 0x93b8).
 */
void render_p1_view(void)
{
    player_view_geom_t __far *view;

    p1_scroll_x = 4;
    if (p1_grid_x > 0x10) {
        p1_scroll_x = (s16)(0x14 - p1_grid_x);
    }
    p1_scroll_y = 4;
    if (p1_grid_y > 0x15) {
        p1_scroll_y = (s16)(0x19 - p1_grid_y);
    }

    view = (player_view_geom_t __far *)p1_view;
    view->pos_x    = p1_grid_x;
    view->pos_y    = p1_grid_y;
    view->scroll_x = p1_scroll_x;
    view->scroll_y = p1_scroll_y;
    p1_render_view_leaf(p1_view);                                /* present leaf */
    return;
}

/*
 * erase_p1_view — 1000:19e4
 * --------------------------------------------------------------------------
 * Restore the background under P1's PREVIOUS cell: write the previous-cell grid
 * coords + the current scroll offsets into the erase descriptor at p1_erase_view
 * (DGROUP 0x8c4/0x8c6), then restore via restore_bg_view.  Mirror of erase_p2_view
 * (19a1), minus the P2 presence guard.
 *   view[+14] = p1_grid_x_prev   view[+16] = p1_grid_y_prev
 *   view[+1e] = p1_scroll_x      view[+20] = p1_scroll_y
 *   restore_bg_view(p1_erase_view off, seg)              (present leaf — see note)
 *
 * 1:1 with the asm 19f0 LES BX,[0x8c4] ; ES:[BX+14]=[0x8882] ; ES:[BX+16]=[0x8e88] ;
 * ES:[BX+1e/20]=scroll ; PUSH [0x8c6],[0x8c4] ; CALL 0x80bc.
 */
void erase_p1_view(void)
{
    player_view_geom_t __far *view;

    view = (player_view_geom_t __far *)p1_erase_view;
    view->prev_x   = p1_grid_x_prev;
    view->prev_y   = p1_grid_y_prev;
    view->scroll_x = p1_scroll_x;
    view->scroll_y = p1_scroll_y;
    p1_restore_view_leaf(p1_erase_view);                         /* restore leaf */
    return;
}

/*
 * restore_bg_pending — 1000:1a20
 * --------------------------------------------------------------------------
 * If a deferred background-restore is queued (pending_erase_count != 0), decrement
 * the count and restore the saved cell (pending_erase_x/y) via restore_bg_view.
 * The cell coords are written into BOTH the view[+6/+8] (grid) AND view[+14/+16]
 * (origin) descriptor fields.  No P1/P2 analog — this is the shared deferred-erase
 * helper (staged by p1_collect_item_score, items.c).
 *   view[+6]  = pending_erase_x   view[+8]  = pending_erase_y
 *   view[+14] = pending_erase_x   view[+16] = pending_erase_y
 *   restore_bg_view(pending_erase_view off, seg)         (restore leaf — see note)
 *
 * 1:1 with the asm 1a2c CMP [0xa1a8],0 ; 1a33 DEC [0xa1a8] ; LES BX,[0x8e4] ;
 * ES:[BX+6]=[0x9b9a] ; ES:[BX+8]=[0x9ba2] ; ES:[BX+14/16] same ; CALL 0x80bc.
 */
void restore_bg_pending(void)
{
    extern void anim_restore_bg_view_leaf(u8 __far *view);  /* anim.c / host_render.c */
    u8 __far *view;

    if (pending_erase_count != 0) {
        pending_erase_count = (u8)(pending_erase_count - 1);
        view = pending_erase_view;
        *(s16 __far *)(view + 0x06) = pending_erase_x;
        *(s16 __far *)(view + 0x08) = pending_erase_y;
        *(s16 __far *)(view + 0x14) = pending_erase_x;
        *(s16 __far *)(view + 0x16) = pending_erase_y;
        /* restore leaf: the engine's 0x8e4 descriptor sources fullscreen_buf (the
           CLEAN background) with word0e==1 — a real clean-bg repaint over the cell,
           NOT a P1 save-under restore.  Routed to the clean-bg leaf accordingly
           (2026-07-02; the old p1_restore_view_leaf routing would have repainted
           the collected item back from Bumpy's save-under buffer). */
        anim_restore_bg_view_leaf(pending_erase_view);
    }
    return;
}

/*
 * draw_p1_sprite — 1000:1cb2
 * --------------------------------------------------------------------------
 * Build P1's sprite object descriptor at the far ptr p1_sprite (DGROUP 0x8884/0x8886)
 * from the current P1 state, then blit it.  Skipped when P1 is hidden
 * (p1_move_anim == 100 / 0x64).  Mirror of draw_p2_sprite (1cea) — the P2 version
 * gates on p2_cell!=0xff and adds p2_frame_base; the P1 version gates on the hidden
 * sentinel and uses p1_move_anim directly.
 *   obj[+4] (word) = p1_move_anim         (frame index)
 *   obj[+0] (word) = p1_pixel_x
 *   obj[+2] (word) = p1_pixel_y
 *   blit_sprite(0x792e, DS=0x203b)        (present leaf — see note)
 *
 * 1:1 with the asm 1cbe CMP word[0x824a],0x64 (hidden gate) ; 1cc5 LES BX,[0x8884] ;
 * +4 = [0x824a] (move_anim) ; +0 = [0x9290] (px) ; +2 = [0x9292] (py) ;
 * PUSH DS ; PUSH 0x792e ; CALL 0x942a.  draw_p1_sprite reads p1_move_anim as a WORD
 * (CMP word ptr); the byte global lives at 0x824a (high byte 0 in the hidden test).
 *
 * RECONCILIATION with entity.c's entity_draw_p1: entity_draw_p1 is the explicit-arg
 * RENDER HELPER (planes/dg/bank/view + pixel/anim args) used by the validated
 * spawn/composite blit path (src/entity.c) — it builds an OBJ_SIZE scratch struct and
 * runs the full plane-exact pipeline.  This draw_p1_sprite is the ZERO-ARG GAME-LOOP
 * ENTRY (1000:1cb2): it reads the live P1 globals, writes the engine's p1_sprite obj
 * descriptor (DGROUP 0x792e pointee via the 0x8884 far ptr), and issues the present
 * leaf.  Exactly the same split as P2 (draw_p2_sprite here vs entity_draw_p2 in
 * entity.c).  Only ONE symbol named draw_p1_sprite exists (this one); entity_draw_p1
 * is a distinct symbol — no duplicate.
 */
void draw_p1_sprite(void)
{
    u8 __far *obj;

    if (p1_move_anim != 100) {
        sprite_obj_t __far *so;
        obj = p1_sprite;
        so = (sprite_obj_t __far *)obj;
        so->frame = (u16)p1_move_anim;                                /* frame */
        so->x     = (s16)p1_pixel_x;                                  /* x     */
        so->y     = (s16)p1_pixel_y;                                  /* y     */
        p1_blit_sprite_leaf(0x792e, 0x203b);                         /* present leaf */
    }
    return;
}

/* ── Present/blit leaf stubs (P1 spine) — faithful-signature no-ops; preserve the
 *    render-core call sites 1:1 without re-driving the Phase-0 work-buffer core (the
 *    same convention as player2.c's p2_*_leaf / anim.c's anim_*_leaf). */
/* ════════════════════════════════════════════════════════════════════════════
 * P1 movement / input-dispatch subsystem — reconstructed from local/decomp
 * (audit 2026-06-28).  The dispatch knot's leaf handlers, previously no-op stubs
 * in game_stubs.c (or entirely absent), ported 1:1 here.  See per-function headers.
 * ════════════════════════════════════════════════════════════════════════════ */

/*
 * p1_cell_solid — 1000:45cf
 * --------------------------------------------------------------------------
 * Solid/blocking-tile predicate: returns nonzero if the base-layer tile at
 * `cell` (tilemap[cell]) is a solid/blocking tile — i.e. nonempty (!= 0) AND
 * not the 0x19 passable marker.  Used by the P1 move-step collision checks.
 *
 * The engine reads tilemap[cell] as a near byte off the DGROUP tilemap far
 * base (DS-relative); here `tilemap` is a `u8 __far *` so the index is applied
 * to the far pointer.  The comparisons are equality tests, so the decomp's
 * signed-char read is value-identical to the u8 read used here.
 *
 * RECONSTRUCTION FIDELITY: the decomp's dead `uVar1 = (undefined2)((ulong)
 * tilemap >> 0x10)` (the far-pointer SEGMENT load, never used) is omitted, and
 * the Borland stack-check prologue is omitted (compiler guard, not game logic).
 */
char p1_cell_solid(u8 cell)
{
    char is_solid;

    is_solid = 0;
    if (tilemap[(u16)cell] != 0 && tilemap[(u16)cell] != 0x19) {
        is_solid = 1;
    }
    return is_solid;
}

/*
 * p1_cell_below_solid — 1000:4605
 * --------------------------------------------------------------------------
 * Returns nonzero if the tile one row below `cell` — tilemap[cell + 0x30],
 * the "contact"/support layer — is solid: nonempty (!= 0x00) AND != 0x13
 * (the layer's passable marker).  Used as a ground/support check by the
 * horizontal move resolvers p1_try_move_left (44c0) and p1_try_move_right
 * (4532).  Companion to p1_cell_solid (45cf), which probes the base layer
 * (+0x00) against the 0x19 marker.
 *
 * The decomp materialises the tilemap segment word (uVar1 = tilemap >> 0x10)
 * but never uses it — a dead artifact of far-pointer codegen; with `tilemap`
 * a `u8 __far *` here the +0x30 offset is applied directly to the far pointer.
 * The engine read is a signed `char`, but both tests are inequalities so the
 * boolean is identical to the u8 read used by read_tile_layer_contact (6bd4).
 *
 * Stack-check prologue (stack_check_limit <= &stack0xfffe) OMITTED — compiler
 * guard, not game logic.
 */
s8 p1_cell_below_solid(u8 cell)
{
    s8 is_solid;

    is_solid = 0;
    if (tilemap[(u16)cell + TILE_CONTACT_LAYER_OFF] != 0 &&
        tilemap[(u16)cell + TILE_CONTACT_LAYER_OFF] != 0x13) {
        is_solid = 1;
    }
    return is_solid;
}

/*
 * p1_advance_move_anim — 1000:4995
 * --------------------------------------------------------------------------
 * Walk-animation frame advance (called by step_walk_anim @ 1000:495c once the
 * per-frame tick counter reaches the period).  Clears the tick counter
 * anim_frame_ctr (DGROUP 0x855d), cyclically advances the move-anim frame index
 * p1_move_anim_frame_idx (0xa0dc) — incrementing while (idx+1) < frame_count,
 * else wrapping to 0 — then latches the selected frame value from
 * frame_table[idx] into p1_move_anim (0x824a).
 *
 * frame_table is a FAR pointer to an array of words: the asm loads it with
 * `LES BX,[BP+6]` and indexes `ES:[BX + idx*2]` (1000:49c3 SHL AX,1).  The caller
 * step_walk_anim pushes (count, off, seg), i.e. frame_table = MK_FP(seg, off).
 *
 * The engine stores the frame as a WORD (1000:49d2 `MOV [0x824a],AX`) and
 * p1_move_anim IS a word throughout — an earlier revision modeled it as u8 on the
 * (wrong) claim that "frame values are < 256 and 0x824b is never read"; in fact
 * draw_p1_sprite reads the full word (1000:1cc9) and the idle-bounce move scripts
 * for modes 0x3d/0x3f carry frames 0x1d1..0x1d7 — the u8 truncation blitted
 * platform/object frames at Bumpy's position (fixed 2026-07-02).  The Borland
 * stack-check prologue (1000:4998) is the non-semantic compiler guard and is
 * omitted, per the reconstruction convention.
 */
void p1_advance_move_anim(u8 frame_count, const u16 __far *frame_table)
{
    anim_frame_ctr = 0;                                  /* DGROUP 0x855d */
    if (p1_move_anim_frame_idx + 1 < (u16)frame_count) {
        p1_move_anim_frame_idx = (u8)(p1_move_anim_frame_idx + 1);
    } else {
        p1_move_anim_frame_idx = 0;
    }
    p1_move_anim = frame_table[p1_move_anim_frame_idx];   /* word store @ 0x824a */
}

/*
 * p1_begin_move_anim — 1000:45a0
 * --------------------------------------------------------------------------
 * Enter the given game/animation mode; if a tile/move action is pending, latch
 * the relocation cell and fire the unconditional cell FX (effect 0x30), then
 * dispatch the first move step.
 *
 * Sibling of p1_begin_move (1000:472d): both do enter_game_mode(mode) + a
 * trailing dispatch_move_step(); this variant inserts the pending-action FX.
 *
 * RECONSTRUCTION FIDELITY: the decomp at 1000:45a0 emits the explicit
 * `anim_target_cell = p1_cell;` assignment AND then calls
 * p1_trigger_cell_animation(0x30) — which itself re-does `anim_target_cell =
 * p1_cell;` (1000:6d94) before tail-calling apply_cell_animation.  The inline
 * store is therefore redundant with the callee's first statement, but it is
 * present in the original's compiled form, so both are reproduced verbatim and
 * in order for a 1:1 mirror.  The decomp's '0' character literal is 0x30 (the
 * FX/effect code named in the decomp comment).  The Borland stack-check
 * prologue (stack_check_limit / FUN_1000_ab83) is omitted as elsewhere.
 */
void p1_begin_move_anim(u8 mode)
{
    enter_game_mode(mode);
    if (p1_pending_action != 0) {
        anim_target_cell = p1_cell;
        p1_trigger_cell_animation(0x30);
    }
    dispatch_move_step();
    return;
}

/*
 * step_walk_anim — 1000:495c
 * --------------------------------------------------------------------------
 * The idle-walk animation tick, called by gamemode_23_walk (script 203b:1ca4)
 * and gamemode_24_walk (script 203b:1cba).  Increments the walk-anim period
 * counter anim_frame_ctr (DGROUP 0x855d); on the tick where it reaches `period`,
 * advances P1's move-anim frame via p1_advance_move_anim (1000:4995) — which
 * itself resets anim_frame_ctr to 0, cyclically advances p1_move_anim_frame_idx
 * (0xa0dc, wrap at frame_count=anim_base) and latches p1_move_anim from
 * frame_table[idx].  If the counter ever overshoots `period`
 * (anim_frame_ctr > period) it is reset to 0.
 *
 * Disasm (1000:495c): INC byte[0x855d] ; MOV AL,[0x855d] / CMP AL,period / JNZ ;
 *   (==) PUSH frame_seg / PUSH frame_off / PUSH anim_base / CALL 4995 ;
 *   else MOV AL,[0x855d] / CMP AL,period / JBE (skip) / (>) MOV byte[0x855d],0.
 * All compares are u8 (unsigned), matching the JBE/JNZ encodings.
 *
 * (Borland stack-check prologue `if (stack_check_limit <= &stack0xfffe)
 *  borland_stack_overflow();` omitted — non-semantic compiler guard.)
 *
 * RECONSTRUCTION FIDELITY (far-vs-near callee arg): before CALL 4995 the engine
 * pushes BOTH the frame_seg (0x203b) and frame_off (e.g. 0x1ca4) words — i.e. a
 * far pointer's two words in stack order.  p1_advance_move_anim is a NEAR
 * function (DS = DGROUP = 0x203b at runtime), so its decomp consumes only the
 * offset as a near `void *` and leaves the segment as an unused stack leftover
 * (`in_stack_00000006`).  Here we recombine (frame_seg, frame_off) into a far
 * pointer with MK_FP so the move-anim frame table in DGROUP is addressed
 * correctly in the host build — matching the asm's two pushed words and the
 * MK_FP(seg, off) idiom already used in enter_game_mode (player.c).
 */
#ifdef BUMPY_PLAYABLE
/* Walk/jump animation frame-index tables.  The engine stores these as DGROUP word arrays;
   step_walk_anim's callers (player.c 551/566/3545/3626/3675) pass their engine NEAR
   offsets + the static DGROUP seg 0x203b.  On the host MK_FP(0x203b,<off>) is WILD memory
   (the recon's DGROUP lives elsewhere), so the walk animation read garbage frame indices →
   garbled/flickering walk anim.  These arrays are the VERBATIM engine data (BUMPY_unpacked.exe,
   DGROUP file base 0x11440 + off); walk_anim_table_for_offset maps the engine offset → the
   recon array, the same convention as move_step_handler_for_offset.  RECONSTRUCTION FIDELITY:
   docs/reconstruction-fidelity.md. */
static const u16 walk_anim_tbl_1ca4[] = {8,9,10,11,10,10,9,10,10,10,10};
static const u16 walk_anim_tbl_1cba[] = {8,9,10,11,10,10,9,10,10,10,10};
static const u16 walk_anim_tbl_14ea[] = {9,9,10,10,11,11,0,0,7,6,6,7,0,1,2,2,1,0,0,
                                         12,13,14,15,16,9,12,13,14,15,16,9,8,9};
static const u16 walk_anim_tbl_1664[] = {0,0x2d,0x2e,0x2f,0x30,0x31,0x32,0x33,0x34,0x35,
                                         0x36,0x37,0x32,0x2f,0x2e,0x2d,0,7,1,7,1,7,1,7,1,
                                         10,10,11,11,10,10,9,9,8,8,9,9};
static const u16 walk_anim_tbl_1b70[] = {7,6,6,5,5,6,6,7,0,1,2,2,3,3,2,2,1,0,0,0,0};

static const u16 __far *walk_anim_table_for_offset(u16 off)
{
    switch (off) {
    case 0x1ca4u: return walk_anim_tbl_1ca4;
    case 0x1cbau: return walk_anim_tbl_1cba;
    case 0x14eau: return walk_anim_tbl_14ea;
    case 0x1664u: return walk_anim_tbl_1664;
    case 0x1b70u: return walk_anim_tbl_1b70;
    default:      return walk_anim_tbl_1ca4;   /* closed set of callers — unreachable */
    }
}
#endif

void step_walk_anim(u8 anim_base, u8 period, u16 frame_off, u16 frame_seg)
{
    anim_frame_ctr = (u8)(anim_frame_ctr + 1);
    if (anim_frame_ctr == period) {
#ifdef BUMPY_PLAYABLE
        (void)frame_seg;   /* engine 0x203b → resolve the engine offset to the recon array */
        p1_advance_move_anim(anim_base, walk_anim_table_for_offset(frame_off));
#else
        p1_advance_move_anim(anim_base, (const u16 __far *)MK_FP(frame_seg, frame_off));
#endif
    } else if (period < anim_frame_ctr) {
        anim_frame_ctr = 0;
    }
    return;
}

/*
 * p1_commit_left — 1000:450c
 * --------------------------------------------------------------------------
 * Commit a left step.  Unless a pending action is active (p1_pending_action ==
 * 0x16), latch the current cell as the bg-restore / view-relocation target and
 * fire cell-effect 0x2f ('/'), then resolve the one-cell left move via
 * move_left().
 *
 * NOTE: p1_trigger_cell_animation (1000:6d94) itself sets anim_target_cell =
 * p1_cell; the explicit assignment here is the original's own redundant write
 * and is mirrored 1:1 for faithfulness (cf. p1_commit_right 1000:457a).
 */
void p1_commit_left(void)
{
    if (p1_pending_action != 0x16) {
        anim_target_cell = p1_cell;
        p1_trigger_cell_animation(0x2f);
    }
    move_left();
    return;
}

/*
 * p1_commit_right — 1000:457a   (commit a right step)
 * --------------------------------------------------------------------------
 * Mirror of p1_commit_left (1000:450c).  Unless a pending action 0x16 is active,
 * latch the current cell as the bg-restore target and fire the step cell FX
 * (effect 0x2f, '/'); then perform the actual right move (move_right, 1000:26a1).
 * (Borland stack-check prologue omitted — non-semantic compiler guard.)
 */
void p1_commit_right(void)
{
    if (p1_pending_action != 0x16) {
        anim_target_cell = p1_cell;
        p1_trigger_cell_animation(0x2f);
    }
    move_right();
    return;
}

/* p1_try_move_up — 1000:4454
 * P1 "move up" input handler.  If P1 is on the top row (p1_cell < 8, i.e. no cell
 * above) fall back into the input-dispatch ladder at p1_input_dispatch_bit02.
 * Otherwise probe the cell directly above (p1_cell - 8): if it is free
 * (p1_cell_solid == 0) start the up-move animation via p1_begin_move_anim(0x1d)
 * (enter_game_mode 0x1d); if the cell above is solid, fall back to
 * p1_input_dispatch_bit02 as well.
 * No globals written here (pure dispatch).  Siblings p1_cell_solid /
 * p1_begin_move_anim / p1_input_dispatch_bit02 are added in this same input-dispatch
 * + move-step batch (same module).  Stack-check prologue omitted per convention. */
void p1_try_move_up(void)
{
    u8 above_solid;

    if (p1_cell < 8) {
        p1_input_dispatch_bit02();
    } else {
        above_solid = p1_cell_solid((u8)(p1_cell - 8));
        if (above_solid == 0) {
            p1_begin_move_anim(0x1d);
        } else {
            p1_input_dispatch_bit02();
        }
    }
    return;
}

/*
 * p1_try_move_down — 1000:448a
 * --------------------------------------------------------------------------
 * P1 move-down input handler.  If not already at/below the bottom row
 * (p1_cell < 0x28) AND the cell one row below (p1_cell + 8) is free, start the
 * down-move animation via p1_begin_move_anim(0x1e) (which enter_game_mode(0x1e)s);
 * otherwise fall back down the input-dispatch ladder via p1_input_dispatch_bit04b.
 *
 * Direct mirror of p1_try_move_up (1000:4454): same cell-neighbour probe + the
 * begin-move-anim vs dispatch-fallback branch, with the DOWN delta (+8), the DOWN
 * bottom-edge bound (0x28), and the DOWN mode (0x1e).
 *
 * Globals: p1_cell (DGROUP 0x856e).  Callees (sibling batch): p1_cell_solid (45cf),
 * p1_begin_move_anim (45a0), p1_input_dispatch_bit04b (43d2).
 *
 * Stack-check prologue (stack_check_limit / FUN_1000_ab83) omitted — non-semantic
 * compiler guard, omitted throughout the reconstruction.
 */
void p1_try_move_down(void)
{
    char below_solid;

    if (p1_cell < 0x28) {
        below_solid = p1_cell_solid((u8)(p1_cell + 8));
        if (below_solid == 0) {
            p1_begin_move_anim(0x1e);
        } else {
            p1_input_dispatch_bit04b();
        }
    } else {
        p1_input_dispatch_bit04b();
    }
    return;
}

/*
 * p1_try_move_left — 1000:44c0
 * --------------------------------------------------------------------------
 * P1 move-left handler.  At the left edge of the row (column counter == 0)
 * there is no cell to the left, so route to FUN_1000_43ef (the try-right /
 * settle-idle input router).  Otherwise look at the cell to the left
 * (cell - 1): if that cell is solid, OR it has ground support one row below,
 * commit the step (p1_commit_left); if it is free AND unsupported, start the
 * left-move animation via p1_begin_move_anim(0x1f).
 *
 * RECONSTRUCTION FIDELITY: the decomp's `move_step_count == 0` guard reads
 * DGROUP 0x855e, i.e. p1_step_col_count (the in-row COLUMN counter) — NOT the
 * real move_step_count @ 0x824c.  Ghidra mislabels 0x855e as "move_step_count".
 * Verified at 1000:44cc `mov al,[0x855e]` (same edge-check the sibling
 * move_left @2634 / p1_walk_left_resolve already map to p1_step_col_count).
 * The `p1_cell + 0xff` argument is (u8)(p1_cell - 1) (1000:44dd `add al,0xff`).
 */
void p1_try_move_left(void)
{
    u8 left_solid;

    if (p1_step_col_count == 0) {                            /* 0x855e */
        FUN_1000_43ef();
    } else {
        left_solid = p1_cell_solid((u8)(p1_cell + 0xff));       /* cell - 1 */
        if (left_solid == 0) {
            left_solid = p1_cell_below_solid((u8)(p1_cell + 0xff));  /* cell - 1 */
            if (left_solid == 0) {
                p1_begin_move_anim(0x1f);
            } else {
                p1_commit_left();
            }
        } else {
            p1_commit_left();
        }
    }
    return;
}

/*
 * p1_try_move_right — 1000:4532   (P1 move-right handler)
 * --------------------------------------------------------------------------
 * Mirror of p1_try_move_left (1000:44c0).  At the right edge of the row
 * (column counter == 7) there is no cell to the right, so settle to idle
 * (p1_settle_idle).  Otherwise look at the cell to the right (cell + 1): if it
 * is solid, OR the current cell has ground support one row below, commit the
 * step (p1_commit_right); if it is free AND unsupported, start the right-move
 * animation via p1_begin_move_anim(0x20).
 *
 * RECONSTRUCTION FIDELITY: the decomp's `move_step_count == 7` guard reads
 * DGROUP 0x855e = p1_step_col_count (the in-row COLUMN counter, right edge),
 * which Ghidra mislabels "move_step_count" (the real move_step_count is 0x824c)
 * — the symmetric edge check to p1_try_move_left's p1_step_col_count == 0.
 * Note the support probe uses p1_cell_below_solid(p1_cell) (the CURRENT cell),
 * not cell+1 — preserved exactly from the decomp.
 * (Borland stack-check prologue omitted.)
 */
void p1_try_move_right(void)
{
    char right_solid;

    if (p1_step_col_count == 7) {                            /* 0x855e, right edge */
        p1_settle_idle();
    } else {
        right_solid = p1_cell_solid((u8)(p1_cell + 1));         /* cell + 1 */
        if (right_solid == 0) {
            right_solid = p1_cell_below_solid(p1_cell);         /* current cell support */
            if (right_solid == 0) {
                p1_begin_move_anim(0x20);
            } else {
                p1_commit_right();
            }
        } else {
            p1_commit_right();
        }
    }
    return;
}

/*
 * p1_settle_idle — 1000:440c   (P1 idle/settle handler)
 * --------------------------------------------------------------------------
 * No held direction: latch the current cell as the bg-restore target, read the
 * tile under the player (p1_read_tile_under, 1000:236f), then — if a pending
 * action 0x16 is active — enter the walk mode (enter_mode_1c_walk, 1000:4305);
 * otherwise fire the idle cell FX (effect 0x2f, '/').
 * (Borland stack-check prologue omitted — non-semantic compiler guard.)
 */
void p1_settle_idle(void)
{
    anim_target_cell = p1_cell;
    p1_read_tile_under();
    if (p1_pending_action == 0x16) {
        enter_mode_1c_walk();
    } else {
        p1_trigger_cell_animation(0x2f);
    }
    return;
}

/*
 * p1_input_dispatch_bit01 — 1000:4398   (P1 input-dispatch ladder, up rung)
 * --------------------------------------------------------------------------
 * One rung of the P1 movement input-dispatch ladder.  If the up-bit
 * (input_state & 1) is held, branch to the up-move handler (p1_try_move_up,
 * 1000:4454); otherwise fall through to the next rung, p1_input_dispatch_bit02
 * (1000:43b5), which tests the next input bit.
 *
 * Disasm: 4398 stack-check (omitted — non-semantic Borland guard) /
 *         TEST byte[input_state],1 / JZ → CALL p1_input_dispatch_bit02 (43b5) /
 *         else CALL p1_try_move_up (4454) / RET.
 *
 * input_state is the latched-input bitmask owned by input.c (DGROUP 0x8244),
 * visible here through #include "input.h".
 */
void p1_input_dispatch_bit01(void)
{
    if ((input_state & 1) == 0) {
        p1_input_dispatch_bit02();
    }
    else {
        p1_try_move_up();
    }
    return;
}

/*
 * p1_input_dispatch_bit02 — 1000:43b5   (P1 input ladder, DOWN rung)
 * --------------------------------------------------------------------------
 * Input-dispatch ladder rung for the DOWN bit (input_state & 2).  If DOWN is
 * held, route to the down-move handler p1_try_move_down (1000:448a); otherwise
 * fall through to the next rung, p1_input_dispatch_bit04b (the LEFT bit,
 * 1000:43d2).
 *
 * Reached from p1_input_dispatch_bit01 (1000:4398) when the UP bit (input_state
 * & 1) is clear.  (input_state bits: 1=up, 2=down, 4=left, 8=right, 0x10=fire.)
 *
 * (Borland stack-check prologue `if (stack_check_limit <= &stack0xfffe)
 *  FUN_1000_ab83();` omitted — non-semantic compiler guard.)
 */
void p1_input_dispatch_bit02(void)
{
    if ((input_state & 2) == 0) {
        p1_input_dispatch_bit04b();
    } else {
        p1_try_move_down();
    }
    return;
}

/*
 * p1_input_dispatch_bit04 — 1000:431b
 * --------------------------------------------------------------------------
 * P1 input-dispatch ladder rung. Reached from p1_input_dispatch_bit10 (1000:4344,
 * game-mode idx 0x1c) when input_state&0x10 is set. Branches on the move bits:
 *   input_state&4 (left)  -> move_left              (1000:2634)
 *   else &8       (right) -> move_right             (1000:26a1)
 *   else                  -> play_walk_anim_default (1000:4361 — default walk anim)
 *
 * Mirrors gamemode_03_move's left/right test (same constants/order) but without the
 * down/settle branch. No globals are written; this rung only reads input_state and
 * dispatches to a leaf. Borland stack-check prologue omitted (non-semantic).
 */
void p1_input_dispatch_bit04(void)
{
    if ((input_state & 4) == 0) {
        if ((input_state & 8) == 0) {
            play_walk_anim_default();
        } else {
            move_right();
        }
    } else {
        move_left();
    }
    return;
}

/*
 * p1_input_dispatch_bit04b — 1000:43d2   (P1 input-dispatch ladder, left rung)
 * --------------------------------------------------------------------------
 * Reached from p1_input_dispatch_bit02 once down(2) is clear.  If left(4) is
 * held, commit a left move (p1_try_move_left); otherwise fall through to the
 * ladder terminal FUN_1000_43ef (checks right(8) → p1_try_move_right, else
 * settles idle via p1_settle_idle).  Pure dispatch — touches only input_state.
 *
 * (Borland stack-check prologue omitted — non-semantic compiler guard.)
 */
void p1_input_dispatch_bit04b(void)
{
    if ((input_state & 4) == 0) {
        FUN_1000_43ef();
    } else {
        p1_try_move_left();
    }
    return;
}

/*
 * FUN_1000_43ef — 1000:43ef   (P1 input ladder tail: right/idle router)
 * --------------------------------------------------------------------------
 * Final rung of the P1 input-dispatch ladder (reached from p1_input_dispatch_bit04b
 * when the left bit is clear, and from p1_try_move_left at the left edge).  If the
 * right bit (input_state & 8) is clear there is no held direction -> settle to idle
 * (p1_settle_idle, 1000:440c); otherwise attempt the right step (p1_try_move_right,
 * 1000:4532).  Unnamed in the Ghidra corpus; kept as FUN_1000_43ef since the
 * reconstructed callers reference it by that label.
 * (Borland stack-check prologue omitted — non-semantic compiler guard.)
 */
void FUN_1000_43ef(void)
{
    if ((input_state & 8) == 0) {
        p1_settle_idle();
    } else {
        p1_try_move_right();
    }
    return;
}

/*
 * p1_input_dispatch_bit10 — 1000:4344  (game_mode_handlers idx 0x1c)
 * --------------------------------------------------------------------------
 * Top of the P1 input-dispatch ladder.  Branch on the fire bit of input_state
 * (0x10): with fire held, dispatch to p1_input_dispatch_bit04 (the
 * left/right/walk-anim leaf, 0x431b); otherwise fall through to
 * p1_input_dispatch_bit01 (the up/jump leaf, 0x4398).
 *
 * (Stack-check prologue at 0x4344 — `if (stack_check_limit <= &stack0xfffe)
 * FUN_1000_ab83();` — is the non-semantic Borland compiler guard; omitted.)
 */
void p1_input_dispatch_bit10(void)
{
    if ((input_state & 0x10) == 0) {
        p1_input_dispatch_bit01();
    } else {
        p1_input_dispatch_bit04();
    }
    return;
}

/*
 * move_walk_right_anim_step — 1000:2423   (game_mode 0x05 handler — T6c)
 * --------------------------------------------------------------------------
 * Move-step handler: advance the walk animation one frame (step_walk_anim,
 * anim_base 0x21, period 4, script 203b:14ea), then branch on input —
 * left(4)→move_left; else if right(8) is held→move_right.
 *
 * In the decomp the four stack temps (local_3=0x21, local_4=4, local_8=0x14ea,
 * local_6=0x203b) are merely the compiler staging the step_walk_anim() call
 * arguments; they carry no other semantics.
 */
void move_walk_right_anim_step(void)
{
    step_walk_anim(0x21, 4, 0x14ea, 0x203b);
    if ((input_state & 4) == 0) {
        if ((input_state & 8) != 0) {
            move_right();
        }
    } else {
        move_left();
    }
    return;
}

/*
 * move_step_check_walkable — 1000:24d7   (game_mode 0x0c — mid-step ground probe)
 * --------------------------------------------------------------------------
 * Mid-walk step tick.  If there is no pending action AND the tile one grid-row
 * below the player (tilemap[p1_cell + 8]) is not 0x0b (solid ground underfoot),
 * the player has stepped off an edge → enter mode 4 (fall, enter_mode_04_fall,
 * 1000:28e0).  Otherwise hand off to this step's input dispatcher
 * (move_step_dispatch_input, 1000:250a).
 *
 * The tilemap[p1_cell + 8] deref is the level collision tilemap (the +8 neighbour
 * one grid-row below p1_cell) — a tile-leaf read that is part of this handler's
 * own control-flow condition, so it ports inline via the `tilemap` far pointer
 * (extern, player.h), exactly as gamemode_03_move's `tilemap[p1_cell - 8]` probe.
 * Decomp: *(char *)((uint)p1_cell + (int)tilemap + 8) != '\v';  '\v' == 0x0b.
 */
void move_step_check_walkable(void)
{
    if (p1_pending_action == 0 && tilemap[(u16)p1_cell + 8] != 0x0b) {
        enter_mode_04_fall();
    } else {
        move_step_dispatch_input();
    }
    return;
}

/* move_step_dispatch_input — 1000:250a   (game_mode 0x0d — move-step input dispatch)
 * --------------------------------------------------------------------------
 * The idx-0x0d game_mode_handlers entry, reached once a move step has landed the
 * player at mode 0x0d (move_down_step / dispatch_move_step both enter_game_mode(0x0d)).
 * Branch on the latched input_state, in this exact priority order:
 *   left (bit 4)  -> move_left        (1000:2634)
 *   right(bit 8)  -> move_right       (1000:26a1)
 *   down (bit 2)  -> move_down_step   (1000:253f; the decomp's "FUN_253f")
 *   else          -> move_settle      (1000:27de)
 *
 * Pure dispatch: no globals written, no posC/render-DG reads — straight call-out to
 * already-reconstructed move leaves.  (Borland stack-check prologue omitted.)
 */
void move_step_dispatch_input(void)
{
    if ((input_state & 4) == 0) {
        if ((input_state & 8) == 0) {
            if ((input_state & 2) == 0) {
                move_settle();
            } else {
                move_down_step();
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
 * move_anim_step_to_mode0c — 1000:248e   (game_mode 0x0b — move-step handler)
 * --------------------------------------------------------------------------
 * Advance the walk animation (step_walk_anim, anim base 0x25, period 6, script
 * 203b:1664).  If the down/action bit (input_state & 2) is held, transition to
 * game mode 0x0c and re-enter the move-step sub-dispatch (dispatch_move_step).
 *
 * The decomp's local_3/local_4/local_8/local_6 are merely the spilled arguments
 * being materialised for the step_walk_anim call (anim_base=0x25, period=6,
 * frame_off=0x1664, frame_seg=0x203b) — not independent state writes — so they
 * collapse into the call.  Borland stack-check prologue omitted as elsewhere.
 */
void move_anim_step_to_mode0c(void)
{
    step_walk_anim(0x25, 6, 0x1664, 0x203b);
    if ((input_state & 2) != 0) {
        enter_game_mode(0x0c);
        dispatch_move_step();
    }
    return;
}

/*
 * enter_mode_0b_jump_start — 1000:2470   (game_mode_handlers[0x0a])
 * --------------------------------------------------------------------------
 * Jump/jet start handler.  Reset the jump-step counter (move_step_count, the
 * genuine 0x824c global — NOT the 0x855e column-counter Ghidra mislabels as
 * "move_step_count") to 8, enter game mode 0x0b, then dispatch the first move
 * step.  Structurally identical to p1_begin_move (1000:472d) with the counter
 * reset prepended and the mode hardcoded to 0x0b.
 *
 *   decomp (1000:2470):
 *     jump_step_counter = 8;        // DGROUP 0x824c
 *     enter_game_mode(0xb);
 *     dispatch_move_step();
 *
 * (Borland FUN_1000_ab83 stack-check prologue omitted — see the SCOPE-handlers
 * note above; non-semantic compiler guard.)
 */
void enter_mode_0b_jump_start(void)
{
    move_step_count = 8;             /* jump_step_counter (0x824c) = 8 */
    enter_game_mode(0xb);
    dispatch_move_step();
    return;
}

/*
 * play_walk_anim_default — 1000:4361
 * --------------------------------------------------------------------------
 * Play P1's default walk animation: a thin wrapper that runs the walk-anim
 * stepper with anim base 0x15, period 4, over the script table at 203b:0x1b70.
 * Called by enter_mode_1c_walk (1000:4305) after it sets game_mode = 0x1c.
 *
 * In the decomp the four arguments are staged into stack locals
 * (local_3=0x15, local_4=4, local_8=0x1b70, local_6=0x203b) before the call —
 * a Turbo C argument-marshalling artifact with no semantic content; the body is
 * exactly the single step_walk_anim() call.  (Same shape as gamemode_23_walk /
 * gamemode_24_walk, which call step_walk_anim with their own script tables.)
 * The Borland stack-check prologue is a non-semantic compiler guard — omitted.
 */
void play_walk_anim_default(void)
{
    step_walk_anim(0x15, 4, 0x1b70, 0x203b);
    return;
}

/* advance_physics_freeze — 1000:22d2   (game_mode_handlers[0x2e])
 * --------------------------------------------------------------------------
 * Advances the end-of-level freeze: increments physics_frozen.  On reaching 3
 * it runs the settle (run_physics_settle); otherwise it re-enters game mode
 * 0x2e and dispatches the next scripted move step.  Paired with
 * begin_physics_freeze (1000:228d), which latches physics_frozen=1 and first
 * enters mode 0x2e.
 *
 * Disasm 22d2..22f8: 22de INC byte[0xa0ce] (physics_frozen) /
 *   22e2 CMP byte[0xa0ce],3 / JNZ → { CALL enter_game_mode(0x2e);
 *   CALL dispatch_move_step; } else CALL run_physics_settle.
 * (Borland stack-check prologue at 22d2..22dc omitted — non-semantic.) */
void advance_physics_freeze(void)
{
    physics_frozen = physics_frozen + 1;
    if (physics_frozen == 3) {
        run_physics_settle();
    } else {
        enter_game_mode(0x2e);
        dispatch_move_step();
    }
    return;
}


/* Default build: NOP stubs (no work-buffer context here).  Under -dBUMPY_PLAYABLE
 * the real bodies come from src/host/host_render.c (blit leaf → validated blitter
 * into the host framebuffer; the view leaves stay NOP per present_model.md §5). */
#ifndef BUMPY_PLAYABLE
void p1_render_view_leaf(u8 __far *view)  { (void)view; return; }
void p1_restore_view_leaf(u8 __far *view) { (void)view; return; }
void p1_blit_sprite_leaf(u16 obj_off, u16 obj_seg)
{
    (void)obj_off; (void)obj_seg;
    return;
}
#endif /* !BUMPY_PLAYABLE */
