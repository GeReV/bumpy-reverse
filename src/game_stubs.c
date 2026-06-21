/*
 * game_stubs.c — LINKABILITY SCAFFOLDING (Phase 1, Task 7)
 * ============================================================================
 * These are NOT reconstructions.  They are minimal, faithful-SIGNATURE stub
 * bodies for the engine functions that game.c's session/loop spine
 * (run_game_session / init_game_session_state / reset_game_state / game_loop)
 * transitively calls but that are NOT YET reconstructed.  Their sole purpose is
 * to let BUMPY.EXE LINK so the Phase-1 call graph is in place; every one is
 * DEFERRED to Phase 2.
 *
 * Each stub cites its engine address.  Bodies are no-ops (void) or return a
 * benign default (0).  Where a return value steers control flow in game_loop the
 * chosen default and its loop effect are noted — but note these stubs make the
 * loop LINK, not RUN correctly: the per-tick spine cannot run faithfully until
 * these are ported (the Phase-1 end-to-end gate is DEFERRED — see the ledger and
 * docs/reconstruction-fidelity.md).
 *
 * GROUPING (by where they are called):
 *   A. init_game_session_state (0282) hardware/resource setup + the opaque
 *      ~46-global reset block.
 *   B. run_game_session (0258) one-time view/sound setup.
 *   C. reset_game_state (0bf9) level-data load + entity respawn + round reset.
 *   D. game_loop (0c18) the per-tick spine: menu, transition, P2, anim channels,
 *      grid-history, present, input, collision, screens.
 *
 * RECONSTRUCTION FIDELITY: signatures mirror the decomp prototypes (arg count +
 * width).  No game logic is implemented.  See docs/reconstruction-fidelity.md
 * row "game_stubs.c" for the full deferral list.
 * ============================================================================
 */

#include "bumpy.h"

/* ── E. player.c (Task 6a-6c) forward-declared leaves — DEFERRED Phase 2 ──────
 *
 * player.c is now LINKED into BUMPY.EXE.  Its move spine references a closed set
 * of handlers/leaves that Task 6c stopped short of porting (per the documented
 * STOP-AND-SPLIT rule — see player.h "BOUNDARY" + the ledger).  These are
 * faithful-signature stubs so player.obj resolves; each cites its engine address.
 * NONE of these run on the level-1 boot path that this slice exercises; they are
 * reached only by the not-yet-reconstructed per-tick handler dispatch.
 *
 * (1) The 2 landing leaves past cell-resolution are RECONSTRUCTED in player.c
 *     (Phase 2, Task 3); the move-step substates + their two delegates
 *     p1_exec_pending_action (465e) / move_down_step (253f) are RECONSTRUCTED in
 *     player.c (Phase 2, Task 4).  The anim-channel/FX allocator
 *     apply_cell_animation (69aa) is RECONSTRUCTED in anim.c (Phase 5, Task 3);
 *     its stub is removed (dup-symbol once anim.obj links). */
/* apply_cell_animation (1000:69aa) — RECONSTRUCTED in anim.c (Phase-5 T3). */

/* (2) Sound / anim helper leaves the move handlers call.
 *  play_sound (1000:6e11) — RECONSTRUCTED in sound.c (Phase-6 T3); its stub is removed
 *  here (dup-symbol once sound.obj links).  play_sound_effect (6e30) +
 *  schedule_timer_callback_a/b/c (9488/9502/956d) are also reconstructed in sound.c (T3);
 *  the deeper callees they reach are stubbed below for the BUMPY.EXE link (deferred T4/T5). */
/* play_action_sound (1000:63be) — RECONSTRUCTED in sound.c (Phase-6 T4); stub removed
 *  (dup-symbol once sound.obj's T4 bodies link).  The other L1 event wrappers
 *  (play_contact/exit/pickup/event/state_sound) + the L2 device fns + the L3 timer-table
 *  fns are likewise reconstructed in sound.c (T4); their stubs are not (re)defined here. */

/* ── Phase-6 T3/T4 still-stubbed sound callees (faithful-signature; for the link only) ──
 *  RECONSTRUCTION FIDELITY: these are reached by the ported sound pipeline but are NOT
 *  part of the validated semantic state.  No-op body so BUMPY.EXE links.
 *    record_min_status_code (1000:945b) — records a min status code into CS:[0x946c].
 *  speaker_gate_reset (1000:9440) + FUN_1000_8a07 (1000:8a07) are NO LONGER stubbed —
 *  they are L4 hardware drivers RECONSTRUCTED in sound.c (Phase-6 T5; dup-symbol once
 *  sound.obj links).  FUN_1000_7df9 (set_timer_slot_raw) was un-stubbed in T4. */
void record_min_status_code(u16 status)        { (void)status; }   /* 1000:945b */

/* ── Phase-6 T4/T5 still-stubbed callees the sound bodies reach ──────────────────────
 *  RECONSTRUCTION FIDELITY: faithful-signature no-op stubs so BUMPY.EXE links.  The L4
 *  PC-speaker / MPU / OPL drivers are now RECONSTRUCTED in sound.c (T5; their stubs are
 *  removed here to avoid dup symbols).  STILL stubbed (T6 / out-of-scope): the MPU/init
 *  carve (mpu401_reset_to_uart 8a75 + FUN_8b2a), the dispatch_b/c/d backends, the timer
 *  teardown FUN_7fef, and the entity sweep FUN_6183 (reached from play_contact_sound for
 *  contact codes 0xe..0x11). */
void mpu401_reset_to_uart(void)  {}   /* 1000:8a75 L4 MPU reset (carve → T6) */
void FUN_1000_8b2a(void)         {}   /* 1000:8b2a snddrv_init substep (carve → T6) */
void FUN_1000_91cf(void)         {}   /* 1000:91cf dispatch_b backend → T6 */
void FUN_1000_8af6(void)         {}   /* 1000:8af6 dispatch_b backend → T6 */
void FUN_1000_8e48(void)         {}   /* 1000:8e48 dispatch_b backend → T6 */
void FUN_1000_91d7(void)         {}   /* 1000:91d7 dispatch_c backend → T6 */
void FUN_1000_8b04(void)         {}   /* 1000:8b04 dispatch_c backend → T6 */
void FUN_1000_8e50(void)         {}   /* 1000:8e50 dispatch_c backend → T6 */
void FUN_1000_91df(void)         {}   /* 1000:91df dispatch_d backend → T6 */
void FUN_1000_8b0d(void)         {}   /* 1000:8b0d dispatch_d backend → T6 */
void FUN_1000_8e58(void)         {}   /* 1000:8e58 dispatch_d backend → T6 */
void FUN_1000_7fef(void)         {}   /* 1000:7fef timer teardown/restore → T5/T6 */
void FUN_1000_6183(void)         {}   /* 1000:6183 out-of-scope entity sweep (→ entity) */
/* apply_contact_action (1000:6a89) — RECONSTRUCTED in player.c (Phase-9 T1); the
   no-op stub is removed (it would now be a duplicate symbol against player.obj). */
void play_walk_anim_default(void)           {}  /* 1000:4361 */
/* p1_set_pixel_from_cell 1000:4906 — set p1_pixel_x/y + move_step_count from the
   cell-coord table at DGROUP 0x274/0x276.  A player.c leaf (move/teleport spine)
   not yet reconstructed there; items.c's teleport_to_next_exit_tile (Phase-3 T4)
   calls it, so a faithful-signature stub satisfies the BUMPY.EXE link.  It writes
   semantic state (move_step_count/p1_pixel_y), so the FULL port lands with the
   player move-spine; the items host harness (items_ctest.c) reproduces its effect
   faithfully for the per-fn differential.  DEFERRED (player subsystem). */
void p1_set_pixel_from_cell(void)           {}  /* 1000:4906 */
void step_walk_anim(u8 anim_base, u8 period, u16 frame_off, u16 frame_seg)
{ (void)anim_base; (void)period; (void)frame_off; (void)frame_seg; }  /* 1000:495c */
void FUN_1000_4802(void)                    {}  /* handle_move_input pending==0x0f leaf */

/* (3) Out-of-scope handler-table targets (modes outside the §4.2 slice set):
 *     bounce / jump / teleport / fall-step / physics-freeze / die / pvp modes.
 *     Referenced by game_mode_handlers[] but not reached on the level-1 path. */
void move_walk_right_anim_step(void)        {}  /* 1000:2423  idx 0x05 */
void enter_mode_0b_jump_start(void)         {}  /* 1000:2470  idx 0x0a */
void move_anim_step_to_mode0c(void)         {}  /* 1000:248e  idx 0x0b */
void move_step_check_walkable(void)         {}  /* 1000:24d7  idx 0x0c */
void move_step_dispatch_input(void)         {}  /* 1000:250a  idx 0x0d */
/* teleport_to_next_exit_tile (1000:25ad, idx 0x0e) is RECONSTRUCTED in items.c
   (Phase-3 T4) — no longer stubbed here (would be a duplicate symbol). */
/* FUN_1000_22b0 (idx 0x10/0x2c) + run_physics_settle_wrap (1000:22c1, idx 0x2d)
   are now RECONSTRUCTED in player.c (Phase 2, Task 4). */
void p1_input_dispatch_bit10(void)          {}  /* 1000:4344  idx 0x1c */
void FUN_1000_4437(void)                     {}  /* 1000:4437  idx 0x1d..0x20 */
void advance_physics_freeze(void)           {}  /* 1000:22d2  idx 0x2e */
void FUN_1000_1e3d(void)                     {}  /* 1000:1e3d  idx 0x30 */

/* (4) mode_script_tbl (DGROUP 0x2252): far-ptr-per-game_mode to its [anim,dx,dy]
 *     move script.  Runtime-populated in the engine (not statically resolvable —
 *     confirmed in Task 6a); a zeroed blob of the right size satisfies the link.
 *     Sized 0x40 modes × 4 bytes/entry. */
u8 mode_script_tbl[0x40 * 4] = { 0 };

/* (5) dos_abort — input.c declares `extern int dos_abort(void)` for the faithful
 *     control flow of its stack-check / abort path (the engine calls the CRT
 *     abort thunk).  In the linked build it is never reached on the boot path. */
int dos_abort(void)                         { return 0; }


/* ── A. init_game_session_state (1000:0282) setup callees ───────────────────── */

/* set_disk_swap_callback 1000:72ef — install INT24 + disk-swap prompt callback. */
void set_disk_swap_callback(u16 int24_handler, u16 callback)
{
    (void)int24_handler; (void)callback;
}

/* FUN_1000_7bad — timer/resource-table init (off,seg). */
void init_timer_resource_table(u16 off, u16 seg)
{
    (void)off; (void)seg;
}

/* install_interrupt_handler — INT timer handler install. */
void install_interrupt_handler(void)
{
}

/* init_joystick_handlers — joystick handler-script install. */
void init_joystick_handlers(void)
{
}

/* mouse_reset — INT33 mouse reset. */
void mouse_reset(void)
{
}

/* FUN_1000_7563 — sound table init (off,off,seg). */
void init_sound_tables(u16 a, u16 b, u16 seg)
{
    (void)a; (void)b; (void)seg;
}

/* FUN_1000_7bd7 — misc subsystem init. */
void init_misc_7bd7(void)
{
}

/* FUN_1000_97a4 — display controller init. */
void init_display_97a4(void)
{
}

/* FUN_1000_7bbd — misc init (one byte arg). */
void init_misc_7bbd(u8 mode)
{
    (void)mode;
}

/* FUN_1000_97f1 — display controller init. */
void init_display_97f1(void)
{
}

/* FUN_1000_9821 — CRTC window setup (x0,y0,x1,y1). */
void init_crtc_window(u16 x0, u16 y0, u16 x1, u16 y1)
{
    (void)x0; (void)y0; (void)x1; (void)y1;
}

/* FUN_1000_9814 — set active display page. */
void set_display_page(u8 page)
{
    (void)page;
}

/* FUN_1000_97c5 — set palette/display mode (mode, flag). */
void set_palette_mode(u8 mode, u8 flag)
{
    (void)mode; (void)flag;
}

/* set_resource_table 1000:... — point the resource table at (off,seg). */
void set_resource_table(u16 off, u16 seg)
{
    (void)off; (void)seg;
}

/* clear_viewport 1000:03ea — clear the on-screen viewport. */
void clear_viewport(void)
{
}

/* Opaque ~46-global session reset (init_game_session_state decomp lines 448-494):
   the trailing DAT_203b_xxxx = <const> stores to UNNAMED DGROUP bytes (audio
   mixer / level-score / resource bookkeeping).  Reproducing each as a named
   extern would invent structure the binary does not document.  Collapsed here as
   a single documented no-op; the engine semantics (most → 0, a few → 1/0x14/0x6c/
   9/0x90 constants) are recorded in docs/reconstruction-fidelity.md. */
void reset_opaque_session_globals(void)
{
}


/* ── B. run_game_session (1000:0258) one-time setup ─────────────────────────── */

/* init_view_anim_descriptors 1000:535e — one-time p1/p2 view + anim-channel
   descriptor setup (a long faithful struct-init; DEFERRED Phase 2). */
void init_view_anim_descriptors(void)
{
}

/* sound_select_device 1000:6de3 — RECONSTRUCTED in sound.c (Phase-6 T4); stub removed
 *  (dup-symbol once sound.obj's T4 bodies link). */


/* ── C. reset_game_state (1000:0bf9) callees ────────────────────────────────── */

/* load_current_level_data 1000:32b0 — copy the current level's 0x96-byte header
   into the tilemap buffer (the engine's standalone loader; this slice loads via
   level.c start_level instead). */
void load_current_level_data(void)
{
}

/* spawn_and_draw_level_entities 1000:2a78 — RECONSTRUCTED 1:1 in src/spawn.c
   (Phase-8 T2: the channel-A/B record populator + layer-C static-sprite blitter +
   P1/P2 BUM-header spawn reader).  No longer stubbed here — spawn.obj owns the body
   (no dup; validated by tools/validate_spawn.sh). */

/* FUN_1000_31de — post-spawn round-counter reset.  Task-1 UNCERTAIN: never
   decompiled (address-out-of-bounds).  Faithful no-op stub. */
void reset_round_counters(void)
{
}


/* ── D. game_loop (1000:0c18) per-tick spine callees ────────────────────────── */

/* init_sprite_structs — one-time per-game setup (still stubbed). */
void init_sprite_structs(void)   {}

/* init_title_graphics / run_main_menu / show_menu_select_screen / show_title_and_init /
   play_iris_wipe_transition — RECONSTRUCTED 1:1 in screens.c (Phase-7 T4); their stubs
   are removed here (would be duplicate symbols once screens.obj links). */

/* FUN_1000_75a2 — the engine's input-poll primitive (returns the action byte in AL).
   Faithful-signature stub for the BUMPY.EXE link; reconstructed in input.c later.  The
   screens.c menu / state-machine loops call it through this symbol. */
char fun_75a2_poll_action(u8 arg)    { (void)arg; return 0; }

/* show_highscore_screen (1000:5681), show_level_intro_screen (1000:0d9d),
   level_intro_screen (1000:3852) — RECONSTRUCTED 1:1 in screens.c (Phase-7 T5); their
   stubs are removed here (would be duplicate symbols once screens.obj links). */
void show_text_screen(void)          {}
void show_pause_screen(void)         {}

/* p2_set_move_state (1000:4bc6) — RECONSTRUCTED in player2.c (Phase-4 T3); stub
   removed (would be a duplicate symbol once player2.obj links). */

/* init_fullscreen_view_desc — set up the fullscreen view descriptor (mode,flag). */
void init_fullscreen_view_desc(u8 mode, u8 flag) { (void)mode; (void)flag; }

/* setup_fullscreen_view 1000:483c — the per-load fullscreen view/page restore the
   spawn orchestrator runs once before the grid scan (the fullscreen_buf -> page
   copy bgi_overlay.c models for sub-handler 0).  Render-core leaf, not yet
   reconstructed; faithful no-op stub for linkability (called by spawn.c). */
void setup_fullscreen_view(void) {}

/* P1/P2 sprite draw (the engine's draw_p1_sprite/draw_p2_sprite are reconstructed
   for RENDER as entity.c entity_draw_p1/p2 with explicit args; these zero-arg
   game-loop entries are stubbed for linkability). */
void draw_p1_sprite(void)            {}
/* draw_p2_sprite (1000:1cea) — RECONSTRUCTED in player2.c (Phase-4 T5); stub
   removed (dup-symbol once player2.obj links). */

void apply_level_palette(void)       {}

/* present_frame 1000:... — CRTC page flip / present (one byte arg). */
void present_frame(u8 page)          { (void)page; }

/* run_n_frames — advance N frames (one byte arg). */
void run_n_frames(u8 n)              { (void)n; }

void wait_keypress(void)             {}

/* Grid-cell + grid-history updates.  The P2 entries (p2_update_grid_cell 1000:4b4e,
   p2_advance_grid_history 1000:13b2) are RECONSTRUCTED in player2.c (Phase-4 T3);
   their stubs are removed (dup-symbol once player2.obj links). */
void p1_update_grid_cell(void)       {}
void p1_advance_grid_history(void)   {}

/* p2_step_scripted_move (1000:4c14) — RECONSTRUCTED in player2.c (Phase-4 T3). */

/* Anim-channel STEP (advance) — RECONSTRUCTED in anim.c (Phase-5 T3:
   step_anim_channels_a 1000:14e4, step_anim_channels_b 1000:15a1); their stubs are
   removed (dup-symbol once anim.obj links). */

/* Player-view erase/restore/render (the engine's per-tick view present chain).
   The P2 entries (erase_p2_view 1000:19a1, render_p2_view 1000:1c41) are
   RECONSTRUCTED in player2.c (Phase-4 T5); their stubs are removed (dup-symbol
   once player2.obj links). */
void erase_p1_view(void)             {}
void restore_bg_pending(void)        {}
void render_p1_view(void)            {}

/* Anim-channel DRAW + ERASE (per-tick) — RECONSTRUCTED in anim.c (Phase-5 T4:
   draw_anim_channels_a 1000:165e, draw_anim_channels_b 1000:17c7,
   erase_anim_channels_a 1000:1a67, erase_anim_channels_b 1000:1b2b); their stubs
   are removed (dup-symbol once anim.obj's bodies link).  Their BGI-overlay present
   leaves stay faithful-signature stubs INSIDE anim.c (anim_*_leaf). */

/* P1/P2 bounding-box update.  update_p2_bbox (1000:50c0) is RECONSTRUCTED in
   player2.c (Phase-4 T5); its stub is removed (dup-symbol once player2.obj links). */
void update_p1_bbox(void)            {}

/* int8-tick timing wait (rotate timing flags + wait for the frame tick). */
void rotate_timing_flags_and_wait(void) {}

/* FUN_1000_629c / FUN_1000_233a — post-present / post-input per-tick helpers. */
void game_post_present_629c(void)    {}
void game_post_input_233a(void)      {}

/* handle_gameplay_input 1000:1d26 — RECONSTRUCTED in player.c (Phase 9 T2): the
   player-spine input dispatch (F1..F7 debug keys + p1_read_tile_under / poll_input /
   p1_movement_dispatch / dispatch_move_step / begin_physics_freeze).  Stub removed. */

/* P2 tile move check (1000:4c99) — RECONSTRUCTED in player2.c (Phase-4 T3).
   P1↔P2 collision (check_pvp_collision 1000:50fb) — RECONSTRUCTED in player2.c
   (Phase-4 T5); its stub is removed (dup-symbol once player2.obj links). */

/* ── Callee of p2_tile_move_check still stubbed after Phase-4 T4 ───────────────
 * Phase-4 T4 RECONSTRUCTED the P2 AI decision layer in player2.c, including
 * p2_run_move_state_handler (1000:5003) and p2_ai_select_move_random (1000:4fd3) —
 * their stubs are REMOVED here (now real symbols in player2.obj).  One callee of
 * p2_tile_move_check remains stubbed:
 *   - p2_dispatch_move_state_handler: models the engine's indirect call through the
 *     per-state move-state handler table at DGROUP 0x870 (the handler bytes are
 *     engine level-data, not reached on any captured P2 path); kept as a stub so the
 *     indirect-call site in p2_tile_move_check is preserved 1:1 without inventing the
 *     deferred table.
 * RECONSTRUCTION FIDELITY: faithful-signature no-op stub so player2.obj links;
 * not reached on the harness's captured P2 paths.  → deferred. */
void p2_dispatch_move_state_handler(void) {}  /* DGROUP 0x870[move_state] */

/* all_entries_flag_set — level-complete predicate.  Returns 0 ⇒ the do/while
   stays in the round (a stub cannot detect completion; DEFERRED). */
u8 all_entries_flag_set(void)        { return 0; }
