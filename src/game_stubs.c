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
 * (1) The 2 landing leaves past cell-resolution — pull in the anim-channel
 *     allocator apply_cell_animation (69aa) + FX/land/sound tables. */
void land_on_tile_below(void)               {}  /* 1000:2810 */
void check_tile_below_ladder_or_land(void)  {}  /* 1000:29a6 */

/* (2) Sound / anim helper leaves the move handlers call. */
void play_sound(u8 sound_id)                { (void)sound_id; }  /* 1000:6e11 */
void play_action_sound(void)                {}
void apply_contact_action(u8 code)          { (void)code; }
void play_walk_anim_default(void)           {}  /* 1000:4361 */
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
void teleport_to_next_exit_tile(void)       {}  /* 1000:25ad  idx 0x0e */
void FUN_1000_22b0(void)                     {}  /* 1000:22b0  idx 0x10, 0x2c */
void p1_input_dispatch_bit10(void)          {}  /* 1000:4344  idx 0x1c */
void FUN_1000_4437(void)                     {}  /* 1000:4437  idx 0x1d..0x20 */
void FUN_1000_22c1(void)                     {}  /* 1000:22c1  idx 0x2d */
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

/* sound_select_device 1000:6de3 — detect + select sound device. */
void sound_select_device(void)
{
}


/* ── C. reset_game_state (1000:0bf9) callees ────────────────────────────────── */

/* load_current_level_data 1000:32b0 — copy the current level's 0x96-byte header
   into the tilemap buffer (the engine's standalone loader; this slice loads via
   level.c start_level instead). */
void load_current_level_data(void)
{
}

/* spawn_and_draw_level_entities 1000:2a78 — spawn + draw the level's entities
   (reconstructed for RENDER as entity.c helpers; this standalone game-state
   entry is stubbed for linkability). */
void spawn_and_draw_level_entities(void)
{
}

/* FUN_1000_31de — post-spawn round-counter reset.  Task-1 UNCERTAIN: never
   decompiled (address-out-of-bounds).  Faithful no-op stub. */
void reset_round_counters(void)
{
}


/* ── D. game_loop (1000:0c18) per-tick spine callees ────────────────────────── */

/* init_sprite_structs / init_title_graphics — one-time per-game setup. */
void init_sprite_structs(void)   {}
void init_title_graphics(void)   {}

/* run_main_menu — returns the menu selection byte.  Returns 0 ⇒ game_loop's menu
   while-loop exits immediately into the play path (a stub cannot honor menu
   navigation; DEFERRED). */
u8 run_main_menu(void)           { return 0; }

void show_highscore_screen(void)     {}
void show_menu_select_screen(void)   {}
void show_text_screen(void)          {}
void show_pause_screen(void)         {}
void show_title_and_init(void)       {}
void show_level_intro_screen(void)   {}
void level_intro_screen(void)        {}
void play_iris_wipe_transition(void) {}

/* p2_set_move_state — set P2 launch/move state. */
void p2_set_move_state(u8 state)     { (void)state; }

/* init_fullscreen_view_desc — set up the fullscreen view descriptor (mode,flag). */
void init_fullscreen_view_desc(u8 mode, u8 flag) { (void)mode; (void)flag; }

/* P1/P2 sprite draw (the engine's draw_p1_sprite/draw_p2_sprite are reconstructed
   for RENDER as entity.c entity_draw_p1/p2 with explicit args; these zero-arg
   game-loop entries are stubbed for linkability). */
void draw_p1_sprite(void)            {}
void draw_p2_sprite(void)            {}

void apply_level_palette(void)       {}

/* present_frame 1000:... — CRTC page flip / present (one byte arg). */
void present_frame(u8 page)          { (void)page; }

/* run_n_frames — advance N frames (one byte arg). */
void run_n_frames(u8 n)              { (void)n; }

void wait_keypress(void)             {}

/* Grid-cell + grid-history updates. */
void p1_update_grid_cell(void)       {}
void p2_update_grid_cell(void)       {}
void p1_advance_grid_history(void)   {}
void p2_advance_grid_history(void)   {}

/* P2 scripted move step (P2 counterpart of p1_step_scripted_move; DEFERRED). */
void p2_step_scripted_move(void)     {}

/* Anim-channel STEP (advance) — distinct from the entity.c draw side. */
void step_anim_channels_a(void)      {}
void step_anim_channels_b(void)      {}

/* Player-view erase/restore/render (the engine's per-tick view present chain). */
void erase_p1_view(void)             {}
void erase_p2_view(void)             {}
void restore_bg_pending(void)        {}
void render_p1_view(void)            {}
void render_p2_view(void)            {}

/* Anim-channel DRAW + ERASE (per-tick). */
void draw_anim_channels_a(void)      {}
void draw_anim_channels_b(void)      {}
void erase_anim_channels_a(void)     {}
void erase_anim_channels_b(void)     {}

/* P1/P2 bounding-box update. */
void update_p1_bbox(void)            {}
void update_p2_bbox(void)            {}

/* int8-tick timing wait (rotate timing flags + wait for the frame tick). */
void rotate_timing_flags_and_wait(void) {}

/* FUN_1000_629c / FUN_1000_233a — post-present / post-input per-tick helpers. */
void game_post_present_629c(void)    {}
void game_post_input_233a(void)      {}

/* handle_gameplay_input 1000:1d26 — the player-spine input dispatch (forward-
   declared in player.h; its body — p1_movement_dispatch / dispatch_move_step /
   physics-settle — lands with the player subsystem in Phase 2). */
void handle_gameplay_input(void)     {}

/* P2 tile move check + P1↔P2 collision. */
void p2_tile_move_check(void)        {}
void check_pvp_collision(void)       {}

/* all_entries_flag_set — level-complete predicate.  Returns 0 ⇒ the do/while
   stays in the round (a stub cannot detect completion; DEFERRED). */
u8 all_entries_flag_set(void)        { return 0; }
