#ifndef GAME_H
#define GAME_H

/*
 * game.h — SESSION / GAME-LOOP SPINE (Phase 1, Task 7)
 *
 * Faithful 1:1 decompilation of the four session/loop "spine" functions of
 * BUMPY.EXE, ported from the Ghidra decomp (local/build/slice_decomp.txt,
 * verified live via Ghidra MCP, 2026-06).  See src/game.c for the bodies.
 *
 * Engine addresses (code segment 1000; DGROUP is segment 203b in the original):
 *   run_game_session         1000:0258   outer session loop (level 1 -> game_loop)
 *   init_game_session_state  1000:0282   one-time subsystem init + ~50 global resets
 *   reset_game_state         1000:0bf9   per-round level reload + entity respawn
 *   game_loop                1000:0c18   the per-tick gameplay spine
 *
 * INTEGRATION ROLE: game.c is the top of the link graph.  It OWNS the session/
 * round/tick control flags and the few genuinely cross-module game-state globals
 * that have no single natural module owner (see "owned globals" below).  As of
 * Phase-9 T4 the per-tick callees of game_loop / run_game_session / reset_game_state
 * resolve to REAL reconstructed module bodies, EXCEPT a documented set of genuine
 * hardware / CRTC-page-flip / int8-timing / render-core / never-decompiled
 * carve-outs that remain faithful-signature stubs in src/game_stubs.c.  The carve-out
 * boundary is enforced by tools/validate_integration.sh.
 */

#include "bumpy.h"

/* ── Session / round / tick control flags (OWNED here, defined in game.c) ─────
 *
 * round_continue_flag   DGROUP 0x9d30 (DAT_9d30): inner do/while gate in
 *                       run_game_session — nonzero == replay the current round.
 * session_continue_flag DGROUP 0x856d (DAT_856d): outer do/while gate — nonzero
 *                       == restart the whole session.
 * frame_abort_flag      DGROUP 0x928d (DAT_203b_928d): set by the pause/quit
 *                       paths inside game_loop; -1 == text screen, 1 == abort.
 *
 * reset_game_state clears round_continue_flag/session_continue_flag; game_loop
 * reads all three.  No other module touches these, so game.c owns them. */
extern u8 round_continue_flag;
extern u8 session_continue_flag;
extern u8 frame_abort_flag;

/* rng_frame (per-frame RNG byte) is OWNED by player.c (DGROUP 0x...); declared
 * extern in player.h.  game_loop writes it (rng_frame = rand()); we use the
 * player.c definition. */

/* ── The four ported session/loop functions ─────────────────────────────────── */

void run_game_session(void);         /* 1000:0258 */
void init_game_session_state(void);  /* 1000:0282 */
#ifdef BUMPY_PLAYABLE
/* host_view_descriptors_init (game.c, Task 9): bind backing storage to the
   per-tick view-descriptor far pointers before init_view_anim_descriptors runs.
   PLAYABLE-only host-leaf; default build byte-unchanged. */
void host_view_descriptors_init(void);
#endif
void reset_game_state(void);         /* 1000:0bf9 */
void game_loop(void);                /* 1000:0c18 */

/* game_tick — one iteration of game_loop's innermost per-tick loop (game.c
   331-367), factored out so the int8 end-to-end harness can drive the per-tick
   body once per captured tick.  RECONSTRUCTION FIDELITY: pure extraction — the
   statement sequence and order are byte-identical to the inline loop body; no
   reordering, no added logic.  See docs/reconstruction-fidelity.md. */
void game_tick(void);

/* ── Session/loop spine callees (forward declarations) ────────────────────────
 * These declare the engine functions the session/loop spine transitively calls,
 * so game.c compiles as a structure-faithful mirror.  As of Phase-9 T4 MOST have
 * REAL bodies in their owning module (.obj); only the documented CARVE-OUTS
 * (hardware-init / CRTC page-flip / int8-timing / render-core / never-decompiled)
 * still resolve to faithful-signature stubs in game_stubs.c.  Resolution is
 * enforced by tools/validate_integration.sh; per-symbol notes live in game_stubs.c. */

/* init_game_session_state setup callees */
void set_disk_swap_callback(u16 int24_handler, u16 callback);
void init_timer_resource_table(u16 off, u16 seg);   /* 1000:7bad bgi_overlay_thunk_adab (carve) */
void install_interrupt_handler(void);
void init_joystick_handlers(void);
void mouse_reset(void);
void init_sound_tables(u16 a, u16 b, u16 seg);       /* 1000:7563 init_sound_tables (carve) */
void init_misc_7bd7(void);
void init_display_97a4(void);
void init_misc_7bbd(u8 mode);
void init_display_97f1(void);
void init_crtc_window(u16 x0, u16 y0, u16 x1, u16 y1);/* 1000:9821 set_crtc_window (carve) */
void set_display_page(u8 page);
void set_palette_mode(u8 mode, u8 flag);
void set_resource_table(u16 off, u16 seg);
void clear_viewport(void);
void reset_opaque_session_globals(void);

/* run_game_session setup callees */
void init_view_anim_descriptors(void);
void sound_select_device(void);

/* reset_game_state callees */
void load_current_level_data(void);
void spawn_and_draw_level_entities(void);
void reset_round_counters(void);                     /* 1000:31de init_round_state (carve) */

/* game_loop per-tick callees */
void init_sprite_structs(void);
void init_title_graphics(void);
u8   run_main_menu(void);
void show_highscore_screen(void);
void show_menu_select_screen(void);
void show_text_screen(void);
void show_pause_screen(void);
void show_title_and_init(void);
void show_level_intro_screen(void);
void level_intro_screen(void);
void play_iris_wipe_transition(void);
void p2_set_move_state(u8 state);
void init_fullscreen_view_desc(u8 mode, u8 flag);
void draw_p1_sprite(void);
void draw_p2_sprite(void);
void apply_level_palette(void);
void present_frame(u8 page);
void run_n_frames(u8 n);
void wait_keypress(void);
void p1_update_grid_cell(void);
void p2_update_grid_cell(void);
void p1_advance_grid_history(void);
void p2_advance_grid_history(void);
void p2_step_scripted_move(void);
void step_anim_channels_a(void);
void step_anim_channels_b(void);
void erase_p1_view(void);
void erase_p2_view(void);
void restore_bg_pending(void);
void render_p1_view(void);
void render_p2_view(void);
void draw_anim_channels_a(void);
void draw_anim_channels_b(void);
void erase_anim_channels_a(void);
void erase_anim_channels_b(void);
void update_p1_bbox(void);
void update_p2_bbox(void);
void rotate_timing_flags_and_wait(void);
void game_post_present(void);                        /* 1000:629c (Phase-9 T3) */
void game_post_input(void);                          /* 1000:233a (Phase-9 T3) */
/* handle_gameplay_input (1000:1d26) is declared in player.h (player-spine input
   dispatch); game.c includes player.h, so it is NOT redeclared here. */
void p2_tile_move_check(void);
void check_pvp_collision(void);
u8   all_entries_flag_set(void);

#endif /* GAME_H */
