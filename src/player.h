#ifndef PLAYER_H
#define PLAYER_H

/*
 * player.h — P1 MOVE-EXECUTION SPINE (Phase 1, Task 6a)
 *
 * Faithful 1:1 decompilation of the four "spine" functions of the player
 * movement state machine, ported from the Ghidra decomp of BUMPY_unpacked.exe
 * (verified live via Ghidra MCP, 2026-06).  See src/player.c for the bodies.
 *
 * Engine addresses (DGROUP is segment 203b in the original; code segment 1000):
 *   p1_step_scripted_move   1000:13df   THE move-step executor (primary validated)
 *   enter_game_mode         1000:4263   central movement-state transition
 *   p1_movement_dispatch    1000:1e02   game-mode handler jump-table dispatch
 *   dispatch_move_step      1000:238e   per-mode move-step sub-dispatch
 *
 * SCOPE (Task 6a): only the four spine functions above.  The gamemode_* handlers
 * they dispatch to, the dispatch-table DATA (game_mode_handlers @ DGROUP 0x7ca,
 * move_step_dispatch_tbl @ DGROUP 0x43c0, mode_script_tbl @ DGROUP 0x2252), and
 * tile collision are TASK 6b — they are forward-declared `extern` here, NOT ported.
 */

#include "bumpy.h"

/* ── P1 movement-state DGROUP globals (defined in player.c) ─────────────────── */

/* P1 sub-pixel/pixel position (DGROUP 203b:0x9290 / 0x9292, signed 16-bit). */
extern s16 p1_pixel_x;
extern s16 p1_pixel_y;

/* P1 current animation-frame value written by the move step (DGROUP 0x824a). */
extern u8  p1_move_anim;

/* Current movement/physics state-machine mode (DGROUP 0x792c). */
extern u8  game_mode;

/* Previous game_mode, saved by p1_movement_dispatch before it runs (DGROUP 0x8552). */
extern u8  prev_game_mode;

/* Step index within the current move sequence (DGROUP 0x792a, byte). */
extern u8  p1_move_step_idx;

/* When nonzero, all movement transitions are inhibited (DGROUP 0x8242). */
extern u8  move_locked;

/* Remaining scripted-move steps; 0 == not currently in a scripted move (DGROUP 0x824d). */
extern u8  p1_move_steps_left;

/* Facing flag: 0 == facing right, nonzero == facing left (negates dx) (DGROUP 0x9bae). */
extern u8  p1_facing_left;

/* Animation-frame index reset by enter_game_mode on a real mode change (DGROUP 0xa0dc). */
extern u8  p1_move_anim_frame_idx;

/* Action code cleared at the top of p1_movement_dispatch (DGROUP 0x7923).
   RECONSTRUCTION FIDELITY: Ghidra types this as a word (`= 0`), but the asm store
   at 1000:1e0e is `MOV byte ptr [0x7923],0` — a single-byte clear; modeled as u8. */
extern u8  p1_queued_action_code;

/* Physics-frozen flag consulted by p1_movement_dispatch (DGROUP 0xa0ce). */
extern u8  physics_frozen;

/* "settle/override" flag consulted by p1_movement_dispatch (DGROUP 0xa1a7, DAT_a1a7). */
extern u8  move_override;

/* Far pointer (off+seg, DGROUP 0xa1ac) into the current [anim,dx,dy] move script.
   Modeled as a far pointer to 16-bit words: script[0]=anim, script[1]=dx, script[2]=dy.
   Advancing by one 6-byte entry == advancing this pointer by 3 words.  The engine
   only writes the OFFSET word on the per-step advance (see player.c fidelity note). */
extern u16 __far *p1_move_script;

/* input_state is OWNED by input.c (DGROUP 0x8244); declared extern, never redefined here. */
extern u8 input_state;

/* ── Forward-declared (TASK 6b) — bodies NOT ported in 6a ──────────────────── *
 * These are the data tables and handlers that the spine functions dispatch
 * THROUGH.  In 6a they are declared so player.c compiles as a structure-faithful
 * mirror; their bodies/contents are Task 6b.  player.c is NOT linked into
 * BUMPY.EXE this task, so the unresolved externs are expected.
 *
 *  - game_mode_handlers     DGROUP 0x7ca   : near-ptr jump table, [game_mode]
 *  - move_step_dispatch_tbl DGROUP 0x43c0  : 2D near-ptr table, [mode][step_idx],
 *                                            stride 0x22 per mode
 *  - mode_script_tbl        DGROUP 0x2252  : far-ptr-to-[anim,dx,dy]-script table,
 *                                            4-byte entries, [mode]
 *  - move_settle            1000:27de      : settle/override handler
 *  - gamemode_* handlers                   : the per-mode movement behaviours
 */

/* Jump table: 64 near function pointers indexed by game_mode (DGROUP 0x7ca). */
extern void (*game_mode_handlers[64])(void);

/* 2D move-step sub-dispatch table (DGROUP 0x43c0).  Per-mode stride is 0x22
   BYTES (0x11 word entries).  Modeled as a byte blob; dispatch_move_step computes
   the [mode][step_idx] near-pointer with the engine's exact stride arithmetic. */
extern u8 move_step_dispatch_tbl[];

/* mode_script_tbl (DGROUP 0x2252): far-ptr (off+seg) per game_mode to its
   [anim,dx,dy] move script.  4 bytes per entry.  Modeled as a byte blob; the
   pointer is reconstructed in enter_game_mode exactly as the engine does. */
extern u8 mode_script_tbl[];

/* Settle/override handler dispatched by p1_movement_dispatch (1000:27de). */
extern void move_settle(void);

/* ── The four ported spine functions ──────────────────────────────────────── */

char p1_step_scripted_move(void);   /* 1000:13df */
void enter_game_mode(u8 mode);      /* 1000:4263 */
void p1_movement_dispatch(void);    /* 1000:1e02 */
void dispatch_move_step(void);      /* 1000:238e */

#endif /* PLAYER_H */
