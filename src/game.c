/*
 * game.c — SESSION / GAME-LOOP SPINE (Phase 1, Task 7)
 * ============================================================================
 * Faithful 1:1 decompilation of the four session/loop spine functions of
 * BUMPY.EXE, ported from the Ghidra decomp (local/build/slice_decomp.txt,
 * cross-checked LIVE via Ghidra MCP decompile_function_by_address, 2026-06).
 *
 * This is the top of the Phase-1 link graph: main -> init_game_session_state +
 * run_game_session -> game_loop -> (reconstructed level/input/player modules +
 * the not-yet-reconstructed per-tick callees that live as faithful-signature
 * STUBS in src/game_stubs.c, each citing its engine address / "DEFERRED Phase 2").
 *
 * Engine addresses (code segment 1000; DGROUP segment 203b in the original):
 *   run_game_session         1000:0258
 *   init_game_session_state  1000:0282
 *   reset_game_state         1000:0bf9
 *   game_loop                1000:0c18
 *
 * RECONSTRUCTION FIDELITY — global ownership (integration decision, Task 7):
 *   The original keeps all of these in one flat DGROUP.  In this multi-TU C
 *   reconstruction each global is DEFINED by exactly one module and `extern`'d by
 *   the rest, to link BUMPY.EXE without duplicate symbols.  game.c OWNS the
 *   session/round/tick control flags (round/session/frame), plus the few
 *   game-state globals that game_loop/init touch and that have no single natural
 *   home (current_level_index, palette_loaded, cur_level_ptr/level_src_ptr,
 *   the menu/score scratch).  Module-owned globals it reads (current_level,
 *   current_entity_index from level.c; rng_frame, p1/p2 state from player.c;
 *   input from input.c) are declared extern via the module headers.
 *
 * RECONSTRUCTION FIDELITY — the stack-overflow guard:
 *   Every original function begins with
 *       if (stack_check_limit <= &stack0xfffe) FUN_1000_ab83();
 *   which is the Turbo C near-stack-check prologue (calls the stack-overflow
 *   abort thunk).  This is CRT scaffolding, not game logic; it is intentionally
 *   omitted here (the Open Watcom CRT performs its own stack checking).  Noted
 *   once for all four functions.
 * ============================================================================
 */

#include "game.h"
#include "level.h"    /* start_level, current_level, current_entity_index      */
#include "input.h"    /* install_keyboard_isr, get_key_state                    */
#include "player.h"   /* p1_step_scripted_move, rng_frame, p1/p2 move state     */
#include "video.h"    /* video_set_mode_0d                                      */

#include <stdlib.h>   /* rand() — the engine calls the CRT rand (1000 rand thunk)*/

/* ── Session / round / tick control flags (OWNED here) ──────────────────────── */
u8 round_continue_flag;      /* DGROUP 0x9d30 (DAT_9d30) */
u8 session_continue_flag;    /* DGROUP 0x856d (DAT_856d) */
u8 frame_abort_flag;         /* DGROUP 0x928d (DAT_203b_928d) */

/* ── game_loop / init game-state globals OWNED here (no single module home) ──── */
u8  current_level_index;     /* DGROUP 0x7310 — 0-based level index (= current_level-1) */
u8  palette_loaded;          /* DGROUP — set 0 by init, by apply_level_palette later   */
u8  menu_option2_setting;    /* game_loop menu scratch */
u8  settle_countdown;        /* game_loop per-round settle counter (init 5) */
u16 score_lo;                /* score low word  */
u16 score_hi;                /* score high word */
u8  p2_move_state;           /* P2 launch state passed to p2_set_move_state */

/* far-pointer level-archive cursors written by init / load_current_level_data.
   RECONSTRUCTION FIDELITY: the engine keeps these as split off/seg DGROUP word
   pairs (cur_level_ptr @0x6bcc-ish, level_src_ptr @0x75d0); here they are far
   pointers.  game.c only zero-inits them; the real loader (start_level / the
   deferred load_current_level_data) repopulates them. */
u8 __far *cur_level_ptr;     /* DGROUP — current level block cursor */
u8 __far *level_src_ptr;     /* DGROUP 0x75d0 — level-archive source cursor */

/* tilemap: the level tilemap far pointer.  Read by the player tile-collision
   leaves (player.c) and by load_current_level_data / spawn_and_draw_level_entities
   (reset_game_state's stubbed callees).  No module DEFINES it (level.c renders via
   its own decode buffers), so game.c owns it as the cross-module shared symbol.
   RECONSTRUCTION FIDELITY: in the engine this is the DGROUP far pointer @0xa0d8
   pointed at the static tilemap buffer @203b:0xa0e4 (the block start_level /
   load_current_level_data byte-copies the level header into).  Here it is left
   NULL until a loader populates it; the player leaves that read it run only
   under the deferred per-tick spine. */
u8 __far *tilemap;           /* DGROUP 0xa0d8 — level tilemap far pointer */


/* ============================================================================
 * init_game_session_state — 1000:0282
 * ----------------------------------------------------------------------------
 * One-time subsystem init: install the disk-swap/INT-24 callback, the timer /
 * keyboard / joystick / mouse handlers, the resource table, then reset ~50
 * game-state globals to their session defaults and clear the viewport.
 *
 * Faithful to the decomp's call ORDER and reset SEMANTICS.  Two faithfulness
 * notes:
 *   (1) The ~50 trailing `DAT_203b_xxxx = <const>` resets in the decomp are
 *       UNNAMED opaque DGROUP bytes (audio mixer state, level/score scratch,
 *       resource bookkeeping).  Reproducing them as 50 individually-named externs
 *       would invent structure the binary does not document.  The named, in-scope
 *       resets are performed here; the opaque remainder is collapsed into the
 *       documented reset_opaque_session_globals() stub (game_stubs.c) which
 *       records the exact decomp lines it stands in for.  DEVIATION — see the
 *       fidelity audit.
 *   (2) The hardware-setup sub-calls (FUN_1000_7bad/7563/97a4/97f1/9821/9814/
 *       97c5, set_resource_table, install_interrupt_handler, init_joystick_handlers,
 *       mouse_reset, set_disk_swap_callback) are audio/CRTC/resource hardware init
 *       that cannot run without the full game data + real DOS; they are faithful-
 *       signature stubs in game_stubs.c (DEFERRED Phase 2).  install_keyboard_isr
 *       is the one REAL reconstructed call (input.c).
 *
 * Boot deviation: the original sets the display via FUN_1000_9821's CRTC block;
 * here we additionally call video_set_mode_0d() so the T3 boot harness can assert
 * VGA mode 0x0D is set (the only externally observable boot effect).  DEVIATION.
 * ============================================================================ */
void init_game_session_state(void)
{
    set_disk_swap_callback(0x698, 0x6a9);
    init_timer_resource_table(0x6fac, 0x203b);   /* FUN_1000_7bad */
    install_interrupt_handler();
    install_keyboard_isr();                        /* REAL (input.c) */
    init_joystick_handlers();
    mouse_reset();
    init_sound_tables(0x4c00, 0x4cd0, 0x203b);    /* FUN_1000_7563 */
    init_misc_7bd7();                              /* FUN_1000_7bd7 */
    init_display_97a4();                           /* FUN_1000_97a4 */
    init_misc_7bbd(2);                             /* FUN_1000_7bbd */
    init_display_97f1();                           /* FUN_1000_97f1 */
    init_crtc_window(0, 0, 0x13f, 199);            /* FUN_1000_9821 */
    set_display_page(1);                           /* FUN_1000_9814 */
    set_palette_mode(0xe, 1);                       /* FUN_1000_97c5 */
    set_resource_table(0x90, 0x203b);

    /* Set VGA mode 0x0D (320x200x16 EGA planar). DEVIATION: see header note —
       the original does this inside the FUN_1000_9821 CRTC block; surfaced here
       so the boot harness has an observable mode set. */
    video_set_mode_0d();

    /* The named, in-scope session-default resets (decomp lines 448-494). */
    current_level_index = 0;
    palette_loaded      = 0;
    cur_level_ptr       = (u8 __far *)0;   /* engine: cur_level_ptr = DAT_6bd2 */
    level_src_ptr       = (u8 __far *)0;   /* engine: level_src_ptr = DAT_6bf2 */

    /* The ~46 remaining UNNAMED opaque DGROUP resets — see (1) above. */
    reset_opaque_session_globals();

    clear_viewport();
    return;
}


/* ============================================================================
 * run_game_session — 1000:0258
 * ----------------------------------------------------------------------------
 * Outer session loop: start at level 1, do the one-time view-descriptor + sound
 * device setup, then run game_loop in two nested do/while loops gated by
 * round_continue_flag (DAT_9d30) and session_continue_flag (DAT_856d).
 *
 * Ported 1:1 from the decomp.
 * ============================================================================ */
void run_game_session(void)
{
    current_level = 1;
    init_view_anim_descriptors();
    sound_select_device();
    do {
        do {
            game_loop();
        } while (round_continue_flag != 0);
    } while (session_continue_flag != 0);
    return;
}


/* ============================================================================
 * reset_game_state — 1000:0bf9
 * ----------------------------------------------------------------------------
 * Per-round reset: reload the current level's data (32b0), respawn+draw its
 * entities (2a78), run the post-spawn round-counter reset (31de), then clear the
 * round/session continue flags.
 *
 * Ported 1:1 from the decomp.  All three sub-calls are DEFERRED stubs:
 *   load_current_level_data       1000:32b0 — copies the level header into the
 *                                  tilemap buffer (the engine's standalone loader;
 *                                  in this slice level.c loads via start_level).
 *   spawn_and_draw_level_entities 1000:2a78 — entity spawn + draw (the layer-A/B/C
 *                                  placement; reconstructed for RENDER as entity.c
 *                                  helpers, but not as this standalone game-state
 *                                  entry — stubbed here for linkability).
 *   reset_round_counters          1000:31de — Task-1 UNCERTAIN, never decompiled
 *                                  (address-out-of-bounds); faithful no-op stub.
 * ============================================================================ */
void reset_game_state(void)
{
    load_current_level_data();
    spawn_and_draw_level_entities();
    reset_round_counters();          /* FUN_1000_31de — UNCERTAIN, stubbed */
    round_continue_flag = 0;
    session_continue_flag = 0;
    return;
}


/* ============================================================================
 * game_loop — 1000:0c18
 * ----------------------------------------------------------------------------
 * THE per-tick gameplay spine.  Structure (slice_model.md §1), ported 1:1 from
 * the decomp:
 *   init sprite/title graphics
 *   LAB_0c2c: clear frame-abort + per-round scratch, run the main menu loop
 *   outer do { start_level; ... } that, per round:
 *     - shows the level intro, snapshots the level, plays the iris wipe,
 *       reset_game_state, sets up P2 + fullscreen view, draws P1/P2, applies the
 *       palette, presents, waits for keypress, primes the grid-history
 *     - inner while (!abort && !session && !round):  the actual per-tick loop —
 *       rng -> grid-history -> p1/p2 scripted step -> cell update -> anim-channel
 *       STEP -> erase/restore -> anim-channel DRAW -> bbox -> anim erase -> render
 *       player views -> draw sprites -> present_frame -> tick wait -> 629c ->
 *       handle_gameplay_input -> p2 tile move -> pvp collision -> 233a ->
 *       pause-key check
 *     - level-complete / advance-level / title-loop tail
 *
 * Almost every callee below is a faithful-signature STUB in game_stubs.c
 * (DEFERRED Phase 2); the reconstructed ones are p1_step_scripted_move (player.c),
 * get_key_state (input.c), start_level (level.c), reset_game_state (above), and
 * rand (CRT).
 *
 * RECONSTRUCTION FIDELITY: the decomp uses raw `DAT_203b_928d` for the
 * frame-abort flag and a local `uVar1`/`uStack_4` pair; here frame_abort_flag is
 * the named owned global and the local key byte is `key`.  The control flow,
 * comparisons (`== -1`, `== 1`, `!= 0`), and call order are reproduced verbatim.
 * ============================================================================ */
void game_loop(void)
{
    u8 key;

    init_sprite_structs();
    init_title_graphics();
LAB_0c2c:
    frame_abort_flag = 0;
    settle_countdown = 5;
    menu_option2_setting = 0;
    score_lo = 0;
    score_hi = 0;
    while (1) {
        key = run_main_menu();
        if (key == 0) {
            break;
        }
        if (key == 1) {
            show_highscore_screen();
        } else {
            show_menu_select_screen();
        }
    }
    do {
        /* Engine: start_level() is parameterless and reads current_level from
           DGROUP.  The reconstructed level.c added (world,level) params for
           clarity; it ignores `world` and assigns current_level = level, so
           passing current_level for both reproduces the engine's read exactly. */
        start_level(current_level, current_level);
        while (1) {
            do {
                while (1) {
                    if (frame_abort_flag != 0) {
                        return;
                    }
                    level_intro_screen();
                    if (frame_abort_flag == (u8)-1) {
                        show_text_screen();
                        goto LAB_0c2c;
                    }
                    current_level_index = (u8)(current_entity_index - 1);
                    play_iris_wipe_transition();
                    reset_game_state();
                    p2_set_move_state(p2_move_state);
                    init_fullscreen_view_desc(1, 0);
                    draw_p1_sprite();
                    draw_p2_sprite();
                    apply_level_palette();
                    present_frame(1);
                    run_n_frames(1);
                    wait_keypress();
                    p1_update_grid_cell();
                    p2_update_grid_cell();
                    p1_advance_grid_history();
                    p2_advance_grid_history();
                    render_p1_view();
                    render_p2_view();
                    p1_advance_grid_history();
                    p2_advance_grid_history();
                    erase_p2_view();
                    erase_p1_view();
                    p1_update_grid_cell();
                    p2_update_grid_cell();
                    while (frame_abort_flag == 0 &&
                           session_continue_flag == 0 &&
                           round_continue_flag == 0) {
                        rng_frame = (u8)rand();
                        p1_advance_grid_history();
                        p2_advance_grid_history();
                        p1_step_scripted_move();
                        p2_step_scripted_move();
                        p1_update_grid_cell();
                        p2_update_grid_cell();
                        step_anim_channels_a();
                        step_anim_channels_b();
                        erase_p2_view();
                        erase_p1_view();
                        restore_bg_pending();
                        draw_anim_channels_a();
                        draw_anim_channels_b();
                        update_p1_bbox();
                        update_p2_bbox();
                        erase_anim_channels_a();
                        erase_anim_channels_b();
                        render_p1_view();
                        render_p2_view();
                        draw_p1_sprite();
                        draw_p2_sprite();
                        present_frame(1);
                        rotate_timing_flags_and_wait();
                        game_post_present_629c();        /* FUN_1000_629c */
                        handle_gameplay_input();
                        p2_tile_move_check();
                        check_pvp_collision();
                        game_post_input_233a();           /* FUN_1000_233a */
                        key = get_key_state(0x19);
                        if (key != 0) {
                            show_pause_screen();
                        }
                    }
                    if (frame_abort_flag != (u8)-1) {
                        break;
                    }
                    show_text_screen();
                    show_highscore_screen();
                }
                key = all_entries_flag_set();
            } while (key == 0);
            current_level = (u8)(current_level + 1);
            if (current_level != 0x0a) {
                break;
            }
            show_title_and_init();
        }
        show_level_intro_screen();
    } while (1);
}
