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

/* P1 current animation-frame value written by the move step (DGROUP 0x824a).
   WORD: the engine stores/reads 16-bit (1000:1417 / 1000:1cc9); the idle-bounce
   modes 0x3d/0x3f use object-bank frames 0x1d1..0x1d7. */
extern u16 p1_move_anim;

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

/* Jump table: 64 near function pointers indexed by game_mode (DGROUP 0x7ca).
   DEFINED in player.c (Task 6b) — its 64 real static entries dumped from the
   unpacked image; see player.c for the index->handler map. */
extern void (*game_mode_handlers[64])(void);

/* 2D move-step sub-dispatch table (DGROUP 0x43c0).  Per-mode stride is 0x22
   BYTES (0x11 word entries).  Modeled as a byte blob; dispatch_move_step computes
   the [mode][step_idx] near-pointer with the engine's exact stride arithmetic.
   DEFINED in player.c (Task 6b) — real static bytes dumped from the image. */
extern u8 move_step_dispatch_tbl[];

/* mode_script_tbl (DGROUP 0x2252): far-ptr (off+seg) per game_mode to its
   [anim,dx,dy] move script.  4 bytes per entry.  Modeled as a byte blob; the
   pointer is reconstructed in enter_game_mode exactly as the engine does.
   Still forward-declared (its real contents are populated elsewhere; not Task 6b). */
extern u8 mode_script_tbl[];

/* ── The four ported spine functions (Task 6a) ────────────────────────────── */

char p1_step_scripted_move(void);   /* 1000:13df */
void enter_game_mode(u8 mode);      /* 1000:4263 */
void p1_movement_dispatch(void);    /* 1000:1e02 */
void dispatch_move_step(void);      /* 1000:238e */
/* Phase 9 T2: host resolver mapping a move_step_dispatch_tbl 16-bit near offset to
 * its reconstructed host function (the single host-execution deviation — see player.c). */
void (*move_step_handler_for_offset(u16 off))(void);
void begin_physics_freeze(void);    /* 1000:228d */
void handle_gameplay_input(void);   /* 1000:1d26 — player-spine input dispatch (also in game.h) */

/* ══ TASK 6b — game-mode handler state machine ════════════════════════════════
 *
 * The minimal level-1 idle/walk/start/move handler set (slice_model.md §4.2)
 * that game_mode_handlers dispatches to, ported 1:1 from the Ghidra decomp.
 * Each cites its engine address.  See player.c for bodies.
 */

void gamemode_default_idle(void);   /* 1000:28f9  mode 0/default (idle) */
void gamemode_21_start(void);       /* 1000:1e5e  mode 0x21 (start/launch right) */
void gamemode_22(void);             /* 1000:1e90  mode 0x22 (start/launch left)  */
void gamemode_23_walk(void);        /* 1000:1ec2  mode 0x23 (idle/walk-right tick) */
void gamemode_24_walk(void);        /* 1000:1f3e  mode 0x24 (idle/walk-left tick)  */
void gamemode_03_move(void);        /* 1000:23b6  mode 0x03/0x0f (mid-move tick) */
void gamemode_25_contact(void);     /* 1000:2138  mode 0x25 (left-walk contact)  */
void gamemode_26_contact(void);     /* 1000:21e7  mode 0x26 (right-walk contact) */
void p1_begin_walk_right(void);     /* 1000:1f03  begin rightward walk */
void p1_begin_walk_left(void);      /* 1000:1f7f  begin leftward walk  */
void move_left(void);               /* 1000:2634  resolve left-cell move  */
void move_right(void);              /* 1000:26a1  resolve right-cell move */
void move_settle(void);             /* 1000:27de  settle/land (also p1_movement_dispatch override) */
void enter_mode_04_fall(void);      /* 1000:28e0  enter mode 4 (fall) + dispatch */
void enter_mode_1c_walk(void);      /* 1000:4305  enter mode 0x1c + walk anim */
void move_input_tick(void);         /* 1000:463d  3-frame move throttle */
void do_move_with_sound(void);      /* 1000:42d9  mode 0x2d move + sound */
void move_down(void);               /* 1000:4747  move down (tile-below action) */
void handle_move_input(void);       /* 1000:2965  final left/right/down dispatch */

/* ── DGROUP globals owned by this module (Task 6b state machine) ─────────────
 * These are the game-state bytes the handlers read/write.  Addresses are the
 * Ghidra symbol addresses; several are DAT_/unnamed in the decomp and named
 * here for the port.  player.c is an unlinked TU this task, so these are plain
 * module-local externs (no linkage dependency on the rest of src/). */

extern u8  p1_contact_code;     /* contact/landing code resolved per move */
extern u8  move_step_count;     /* jump_step_counter — steps in current move seq */
extern u8  p1_jump_move_ticks;  /* jump/jet tick flag consulted by the idle handler */
extern u8  p1_pending_action;   /* pending tile/move action (from p1_read_tile_under) */
extern u8  rng_frame;           /* per-frame RNG byte (move_down random-dir branch) */
extern u8  p1_cell;             /* 203b:0x856e — P1 grid cell (read by handlers) */
extern u8  p1_cell_prev;        /* saved previous cell */
extern u8  tile_below_player;   /* tile under player (set by move_settle = 0xb) */
extern u8  p1_current_tile;     /* tile probed by move_left/right teleport check */
extern s16 sound_device_state;  /* DGROUP 0x689c (ram0x00026c4c): -0x8000 == no sound; 4 == OPL/charger */

/* ══ TASK 6c — TILE-COLLISION (CELL-RESOLUTION) LAYER ═════════════════════════
 *
 * The closed set of tilemap-reading "tile leaves" the Task-6b handlers call to
 * resolve a grid cell into a contact code / next game_mode, ported 1:1 from the
 * Ghidra decomp (verified live via MCP, 2026-06).  Each cites its engine address.
 * See player.c for bodies and the contact/collision DATA tables (dumped-real from
 * the unpacked image).  The cell-resolution closure is exactly these leaves plus
 * exec_move_action; the leaves they call that go PAST cell-resolution
 * (land_on_tile_below / check_tile_below_ladder_or_land — animation-channel and
 * FX-table dependent) remain forward-declared below (→ Task 7).
 */

/* tilemap: the level tilemap far pointer (cross-module). DEFINED in game.c (T7 — no
   module naturally DEFINES it; level.c renders via its own buffers). Kept extern here. */
extern u8 __far *tilemap;                  /* level tilemap far pointer (cross-module) */

/* The two raw tilemap reads (the actual "tile leaves"). */
void read_tile_layer_contact(u8 cell);     /* 1000:6bd4 — p1_contact_code = tilemap[cell+0x30] */
void read_tile_at_cell(u8 cell);           /* 1000:6bb5 — p1_current_tile = tilemap[cell] */

/* Cell-resolution leaves (probe tile + index a collision table → enter mode). */
void p1_enter_walk_right_mode(void);       /* 1000:2261 — probe cell+1 -> mode 0x2a/0x26 */
void p1_enter_walk_left_mode(void);        /* 1000:21bb — probe cell-1 -> mode 0x29/0x25 */
void p1_begin_move(u8 mode);               /* 1000:472d — enter_game_mode(mode)+dispatch */
void p1_move_left(void);                   /* 1000:467d — exec_move_action(action_tbl_left[pending]) */
void p1_move_right(void);                  /* 1000:469c — exec_move_action(action_tbl_right[pending]) */
void p1_handle_move_input(void);           /* 1000:47cb — left/right/exec_move_action(default[mode]) */
void exec_move_action(u8 action);          /* 1000:46bb — action -> move dispatch */
void move_left_step_resolve(void);         /* 1000:270c — resolve left cell (collision_mode_table_left) */
void move_right_step_resolve_alt(void);    /* 1000:2776 — resolve right cell (collision_mode_table_right_alt) */
void p1_resolve_walk_left_contact(void);   /* 1000:1fbe — leftward walk-contact resolve (modes 0x1a/0x34/0x36/0x38/0x3a) */
void p1_resolve_walk_right_contact(void);  /* 1000:207d — rightward walk-contact resolve (modes 0x1b/0x35/0x37/0x39/0x3b) */

/* ── Contact/collision resolver-table bank (DEFINED in player.c, dumped-real) ───
 * The engine's ten mode-resolver tables are a CONTIGUOUS 0x20-strided bank in
 * DGROUP starting at 0x4256, indexed by an UNMASKED tilemap byte (p1_contact_code
 * = tilemap[cell+0x30], 0x00..0xff).  A code >= 0x20 reads past its own 32-byte
 * table into the next one — the engine does `*(byte*)(code + <base>)` with no
 * bound — and real levels reach such codes.  So the tables MUST be one contiguous
 * dump [0x4256,0x4476); each name aliases into the bank at its DGROUP offset, and
 * `name[code]` reproduces the engine's flat read for any code 0x00..0xff.
 * (See the long note in player.c.  All uses are `name[idx]`, so the macro aliases
 * are transparent to callers and the ctest.) */
extern u8   g_contact_resolver_bank[0x220];   /* DGROUP [0x4256,0x4476) */
#define contact_action_tbl_left          (g_contact_resolver_bank + 0x000) /* 203b:0x4256 move_left */
#define collision_mode_table_right       (g_contact_resolver_bank + 0x020) /* 203b:0x4276 move_right */
#define collision_mode_table_left        (g_contact_resolver_bank + 0x040) /* 203b:0x4296 move_left_step_resolve */
#define collision_mode_table_right_alt   (g_contact_resolver_bank + 0x060) /* 203b:0x42b6 move_right_step_resolve_alt */
#define contact_transition_tbl_b         (g_contact_resolver_bank + 0x080) /* 203b:0x42d6 gamemode_25 left -> next mode */
#define contact_transition_tbl           (g_contact_resolver_bank + 0x0a0) /* 203b:0x42f6 gamemode_26 right -> next mode */
#define left_walk_contact_tbl_34         (g_contact_resolver_bank + 0x0c0) /* 203b:0x4316 p1_resolve_walk_left_contact */
#define right_walk_contact_tbl_35        (g_contact_resolver_bank + 0x0e0) /* 203b:0x4336 p1_resolve_walk_right_contact */
#define left_walk_contact_tbl_38         (g_contact_resolver_bank + 0x100) /* 203b:0x4356 p1_resolve_walk_left_contact */
#define right_walk_contact_tbl_39        (g_contact_resolver_bank + 0x120) /* 203b:0x4376 p1_resolve_walk_right_contact */

/* Move-action LUT bank (DEFINED in player.c, dumped-real).  action_tbl_left/right
   and down_action_lut are flat-indexed by UNMASKED tilemap bytes (p1_pending_action
   / tile-below) that can exceed 0x30, so — like the contact bank — the tables must
   be one contiguous dump [0x36ee,0x384e); each name aliases into it so name[idx]
   reproduces the engine's `*(byte*)(idx + base)` read for any byte.  action_tbl_default
   is indexed by game_mode (< 0x40, never overflows) but lives inside the bank. */
extern u8   g_action_lut_bank[0x160];         /* DGROUP [0x36ee,0x384e) */
#define action_tbl_left     (g_action_lut_bank + 0x000) /* 203b:0x36ee p1_move_left[pending]  */
#define action_tbl_right    (g_action_lut_bank + 0x030) /* 203b:0x371e p1_move_right[pending] */
#define down_action_lut     (g_action_lut_bank + 0x060) /* 203b:0x374e move_down[tile-below]  */
#define action_tbl_default  (g_action_lut_bank + 0x090) /* 203b:0x377e p1_handle_move_input[game_mode] */

/* ══ TASK 3 (Phase 2) — THE TWO LANDING / COLLISION LEAVES (now DEFINED) ═══════
 * land_on_tile_below and check_tile_below_ladder_or_land sit one layer beyond the
 * cell-resolution set: their PHYSICS / mode-transition body is ported 1:1 in
 * player.c (the (mode,fx) land table @ DGROUP 0x76a + the latched-action sound
 * tables @ 0x266e/0x269e are reconstructed there, dumped-real).  The FX/anim
 * allocator and the two move-step delegates they reach remain extern stubs
 * (apply_cell_animation, p1_exec_pending_action, move_down_step — declared below). */
void land_on_tile_below(void);                /* 1000:2810 — landing resolution leaf */
void check_tile_below_ladder_or_land(void);   /* 1000:29a6 — ladder/land probe       */

/* Landing-leaf DGROUP globals + dumped-real DATA tables (DEFINED in player.c). */
extern u8 anim_target_cell;       /* DGROUP 0x856f — cell-8 view/anim relocation target */
extern u8 p1_latched_action;      /* latched action index into the land-sound tables */
extern u8 land_mode_fx_tbl[0x200]; /* DGROUP 0x76a — [mode,fx] per tile (land_on_tile_below) */
extern u8 land_sound_tbl_opl[0x100]; /* DGROUP 0x266e — landing sound (OPL/charger device) */
extern u8 land_sound_tbl_std[0x100]; /* DGROUP 0x269e — landing sound (other devices)      */

/* FX/anim-channel allocator — OUT OF SCOPE (→ Phase 5/6), kept an extern stub.
 * The two move-step delegates the landing leaves reach (p1_exec_pending_action /
 * move_down_step) are now PORTED in player.c (Phase-2 Task 4) — declared in the
 * Task-4 block below. */
extern void apply_cell_animation(u8 fx_code);    /* 1000:69aa — anim-channel/FX allocator (→ Phase 5/6) */

/* Other leaves the handlers call (sound/anim helpers; not tile-collision). */
extern void play_sound(u8 sound_id);             /* 1000:6e11 */
extern void play_action_sound(void);             /* move_left/right action sound */
/* apply_contact_action is RECONSTRUCTED in player.c (Phase-9 T1) — see the contact-
   action handler family declarations below; no longer a game_stubs.c no-op. */
extern void play_walk_anim_default(void);        /* 1000:4361 — enter_mode_1c_walk anim */
extern void step_walk_anim(u8 anim_base, u8 period, u16 frame_off, u16 frame_seg); /* 1000:495c */
extern void FUN_1000_4802(void);                 /* handle_move_input pending==0x0f leaf */

/* OUT-OF-SCOPE handler-table targets (modes outside the §4.2 slice set).  These
   are referenced by game_mode_handlers[] but their bodies are deferred to T6c
   (bounce / jump / teleport / fall-step / physics-freeze / die / pvp modes). */
extern void move_walk_right_anim_step(void);     /* 1000:2423  idx 0x05 */
extern void enter_mode_0b_jump_start(void);      /* 1000:2470  idx 0x0a */
extern void move_anim_step_to_mode0c(void);      /* 1000:248e  idx 0x0b */
extern void move_step_check_walkable(void);      /* 1000:24d7  idx 0x0c */
extern void move_step_dispatch_input(void);      /* 1000:250a  idx 0x0d */
extern void teleport_to_next_exit_tile(void);    /* 1000:25ad  idx 0x0e */
extern void p1_input_dispatch_bit10(void);       /* 1000:4344  idx 0x1c */
extern void FUN_1000_4437(void);                 /* 1000:4437  idx 0x1d..0x20 */
extern void advance_physics_freeze(void);        /* 1000:22d2  idx 0x2e */
extern void FUN_1000_1e3d(void);                 /* 1000:1e3d  idx 0x30 */

/* P1 movement / input-dispatch subsystem leaves — RECONSTRUCTED in player.c
 * (audit 2026-06-28; dispatch-knot completion).  See per-function headers. */
char p1_cell_solid(u8 cell);
s8 p1_cell_below_solid(u8 cell);
void p1_advance_move_anim(u8 frame_count, const u16 __far *frame_table);
void p1_begin_move_anim(u8 mode);
void p1_commit_left(void);
void p1_commit_right(void);
void p1_try_move_up(void);
void p1_try_move_down(void);
void p1_try_move_left(void);
void p1_try_move_right(void);
void p1_settle_idle(void);
void p1_input_dispatch_bit01(void);
void p1_input_dispatch_bit02(void);
void p1_input_dispatch_bit04(void);
void p1_input_dispatch_bit04b(void);
void FUN_1000_43ef(void);

/* ══ PHASE 2, TASK 4 — JUMP / FALL / BOUNCE MOVE-STEP SUBSTATES ════════════════
 *
 * The per-step micro-handlers the T1 capture's jump/fall/bounce scenarios reach
 * (dispatched via move_step_dispatch_tbl), the run_physics_settle handler, and the
 * two delegates check_tile_below_ladder_or_land tail-calls.  Ported 1:1 in
 * player.c; each cites its engine address there.  The boundary callees
 * (apply_cell_animation, play_sound, FUN_1000_4802) stay stubbed with
 * RECONSTRUCTION FIDELITY notes; apply_contact_action is now RECONSTRUCTED
 * (Phase-9 T1, see the contact-action family declarations near the end of this file).
 */

/* DGROUP move-step substate globals (DEFINED in player.c). */
extern u8 p1_grid_row;            /* 0x855c — cursor row counter */
extern u8 p1_step_col_count;      /* 0x855e — cursor column counter (move_step_last_variant ==7) */
extern u8 g_anim_channel_idx;     /* 0x856c — anim-channel index probed by move_step_landed */
extern u8 level_complete_flag;    /* 0xa1b1 — cleared by move_step_landed on the '[' tile */

/* Pending/contact/sound LUTs (DEFINED dumped-real in player.c). */
extern u8 tile_followup_action_lut[0x100]; /* 0x4396 */
extern u8 pending_anim_lut_3cda[0x100];    /* 0x3cda */
extern u8 pending_anim_lut_3caa[0x100];    /* 0x3caa */
extern u8 pending_anim_lut_3c7a[0x100];    /* 0x3c7a (move_step_first_variant) */
extern u8 pending_anim_lut_3d0a[0x100];    /* 0x3d0a */
extern u8 contact_action_lut_35be[0x100];  /* 0x35be (move_step_first_variant) */
extern u8 pending_action_lut_36be[0x100];  /* 0x36be */
extern u8 contact_sound_lut_35de[0x100];   /* 0x35de */
extern u8 move_sound_lut_opl_25ae[0x100];  /* 0x25ae */
extern u8 move_sound_lut_std_25de[0x100];  /* 0x25de */

/* settle handler + tile leaf (run_physics_settle's cross-module DGROUP bytes
   session_continue_flag / frame_abort_flag / settle_countdown live in game.c). */
extern u8   session_continue_flag;        /* game.c — DGROUP 0x856d */
extern u8   frame_abort_flag;             /* game.c — DGROUP 0x928d */
extern u8   settle_countdown;             /* game.c — per-round settle counter */
char run_physics_settle(void);            /* 1000:22fc */
void run_physics_settle_wrap(void);       /* 1000:22c1  (game_mode_handlers[0x2d]) */
void FUN_1000_22b0(void);                 /* 1000:22b0  (game_mode_handlers[0x10]/[0x2c]) */
void p1_read_tile_under(void);            /* 1000:236f  (tile leaf: p1_pending_action = tilemap[p1_cell]) */

/* anim-channel / cell-animation setters (thin FX-allocator wrappers). */
void p1_set_cell_animation(char action_code);             /* 1000:695e */
void p1_set_cell_animation_no_override(char action_code); /* 1000:6987 */
void p1_trigger_cell_animation(u8 action);                /* 1000:6d94 */
void p1_dispatch_pending_action(u8 *action_table);        /* 1000:6d6a */
void p1_step_landed(void);                                /* 1000:6d26 */

/* input-mask + cursor-step + pending/move micro-handlers (the dispatch substates). */
void input_state_mask_10(void);           /* 1000:65e5 */
void input_state_mask_1d(void);           /* 1000:65fb */
void input_state_mask_0f(void);           /* 1000:6611 */
void cursor_move_up(void);                /* 1000:64e2 */
void cursor_move_down(void);              /* 1000:64ff */
void cursor_move_left(void);              /* 1000:651c */
void cursor_move_right(void);             /* 1000:6535 */
void p1_try_trigger_pending_action(void); /* 1000:654e */
void p1_try_jump_action(void);            /* 1000:6587 */
void p1_move_step_with_sound(void);       /* 1000:6648 */
void move_step_first_variant(void);       /* 1000:6699 */
void move_step_last_variant(void);        /* 1000:66d8 */
void move_step_landed(void);              /* 1000:6717 */
void move_step_noop(void);                /* 1000:673a */
void move_step_first_variant_b(void);     /* 1000:6748 */
void move_step_last_variant_b(void);      /* 1000:6789 */
void move_step_first_gate_c(void);        /* 1000:67ca */
void move_step_body_c(void);              /* 1000:67e2 */
void move_step_last_gate_c(void);         /* 1000:67fb */
void move_step_last_body_c(void);         /* 1000:6813 */
void move_step_noop_sentinel(void);       /* 1000:7111 (sentinel filler slot) */
void check_exit_tile_horiz(void);         /* 1000:6326 */

/* the two move-step delegates check_tile_below_ladder_or_land tail-calls. */
void p1_exec_pending_action(void);        /* 1000:465e */
void move_down_step(void);                /* 1000:253f */

/* ══ PHASE 9, TASK 1 — PLAYER-1 CONTACT-ACTION HANDLER FAMILY ══════════════════
 * The move_step_dispatch_tbl contact-action micro-handlers + the apply_contact_action
 * leaf (RECONSTRUCTED in player.c, no longer game_stubs.c no-ops).  Each maps
 * p1_contact_code through a dumped DGROUP byte LUT to an action code, then runs
 * apply_contact_action (channel-B anim-slot claim + contact sound + tilemap stamp). */
void apply_contact_action(u8 action_code);          /* 1000:6a89 */
void p1_dispatch_contact_action(u8 *action_table);  /* 1000:686a */
void p1_apply_contact_action_main(void);            /* 1000:6832 (mode 0x29) */
void p1_apply_contact_action_prev(void);            /* 1000:684b (mode 0x2a) */
void p1_apply_contact_action_at_start(void);        /* 1000:6890 (mode 0x38/0x3a) */
void p1_apply_contact_action_before_end(void);      /* 1000:68bb (mode 0x39/0x3b) */
void p1_apply_contact_action_at_start_b(void);      /* 1000:68e6 (mode 0x34/0x36) */
void p1_apply_contact_action_before_end_b(void);    /* 1000:6922 (mode 0x35/0x37) */
void p1_apply_contact_action_tbl_367e(void);        /* 1000:68fe (mode 0x1a) */
void p1_apply_contact_action_tbl_369e(void);        /* 1000:693a (mode 0x1b) */

/* DGROUP contact LUTs/tables (DEFINED dumped-real in player.c). */
extern u8 contact_action_lut_35fe[0x100];   /* 0x35fe */
extern u8 contact_action_lut_361e[0x100];   /* 0x361e */
extern u8 contact_action_lut_363e[0x100];   /* 0x363e */
extern u8 contact_action_lut_365e[0x100];   /* 0x365e */
extern u8 contact_action_lut_367e[0x100];   /* 0x367e */
extern u8 contact_action_lut_369e[0x100];   /* 0x369e */
extern u8 contact_sound_lut_opl_272e[0x30];/* 0x272e */
extern u8 contact_sound_lut_std_274e[0x30];/* 0x274e */
extern u8 contact_tiledef_tbl[256 * 4];    /* 0x3256/0x3258 (action*4 -> far ptr) */

/* ══ PHASE 9, TASK 3 — PLAYER-1 PER-TICK SPINE (grid / view / draw) ════════════
 * The symmetric P1 counterparts of the validated P2 per-tick fns in player2.c.
 * Thin game-loop wrappers (run once per tick by game_loop): grid-cell recompute,
 * grid-history slide, view present (render/erase), sprite draw, deferred bg restore.
 * Reconstructed 1:1 in player.c; each cites its engine address there. */
void p1_update_grid_cell(void);       /* 1000:1473 (mirror p2_update_grid_cell 4b4e) */
void p1_advance_grid_history(void);   /* 1000:138c (mirror p2_advance_grid_history 13b2) */
void render_p1_view(void);            /* 1000:1bd7 (mirror render_p2_view 1c41) */
void erase_p1_view(void);             /* 1000:19e4 (mirror erase_p2_view 19a1) */
void restore_bg_pending(void);        /* 1000:1a20 (deferred bg-restore helper) */
void draw_p1_sprite(void);            /* 1000:1cb2 (mirror draw_p2_sprite 1cea; zero-arg
                                         game-loop ENTRY — distinct from entity_draw_p1) */

/* P1 grid-cell history / view-scroll DGROUP globals (DEFINED in player.c). */
extern s16 p1_grid_x_new;       /* DGROUP 0x9d36 */
extern s16 p1_grid_y_new;       /* DGROUP 0x9d38 */
extern s16 p1_grid_x;           /* DGROUP 0x857a */
extern s16 p1_grid_y;           /* DGROUP 0x857c */
extern s16 p1_grid_x_prev;      /* DGROUP 0x8882 */
extern s16 p1_grid_y_prev;      /* DGROUP 0x8e88 */
extern s16 p1_scroll_x;         /* DGROUP 0x9ba4 */
extern s16 p1_scroll_y;         /* DGROUP 0x9b9c */
/* P1 sprite-object + render/erase view-descriptor far pointers (DEFINED in player.c). */
extern u8 __far *p1_sprite;     /* DGROUP 0x8884/0x8886 */
extern u8 __far *p1_view;       /* DGROUP 0x8b8/0x8ba */
extern u8 __far *p1_erase_view; /* DGROUP 0x8c4/0x8c6 */
/* Deferred background-restore (pending-erase) globals (DEFINED in player.c). */
extern u8       pending_erase_count;  /* DGROUP 0xa1a8 */
extern s16      pending_erase_x;      /* DGROUP 0x9b9a */
extern s16      pending_erase_y;      /* DGROUP 0x9ba2 */
extern u8 __far *pending_erase_view;  /* DGROUP 0x8e4/0x8e6 */

#endif /* PLAYER_H */
