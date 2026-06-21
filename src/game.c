/*
 * game.c — SESSION / GAME-LOOP SPINE (Phase 1, Task 7)
 * ============================================================================
 * Faithful 1:1 decompilation of the four session/loop spine functions of
 * BUMPY.EXE, ported from the Ghidra decomp (local/build/slice_decomp.txt,
 * cross-checked LIVE via Ghidra MCP decompile_function_by_address, 2026-06).
 *
 * This is the top of the link graph: main -> init_game_session_state +
 * run_game_session -> game_loop.  As of Phase-9 T4 (final integration), game_loop's
 * per-tick callees resolve to REAL reconstructed module bodies (level/input/player/
 * player2/anim/screens), EXCEPT a documented set of genuine hardware / CRTC page-flip
 * / int8-timing / render-core carve-outs that remain faithful-signature stubs in
 * src/game_stubs.c.  tools/validate_integration.sh enforces that boundary.
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

/* init_view_anim_descriptors writes its view-descriptor far-ptr SEG halves as the
   DS register (the runtime DGROUP segment).  The static-image link-time DS literal is
   0x203b; the live engine DS at this call is 0x114b (= 0x110 PSP+10 + 0x103b).  The
   descriptor-byte gate (tools/p1_spine_ctest.c) overrides this to the captured runtime
   value so it can compare the genuine engine-written bytes; the source default keeps
   the decomp's static 0x203b.  Same convention as anim.c's ANIM_DGROUP_RUNTIME_SEG. */
#ifndef GAME_DGROUP_RUNTIME_SEG
#define GAME_DGROUP_RUNTIME_SEG 0x203b
#endif

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

/* ── Phase-9 T3: game_post_present / game_post_input cross-module globals ──────
 * Owned elsewhere (extern only here): items.c level-complete counters, player.c
 * p1_cell_prev / anim_target_cell / level_complete_flag / sound_device_state, and
 * the contact/anim leaves (apply_contact_action 6a89, apply_cell_animation 69aa,
 * play_sound 6e11).  player.h (already included) supplies p1_cell_prev,
 * anim_target_cell, level_complete_flag, sound_device_state, apply_contact_action,
 * apply_cell_animation, play_sound.  The two level-complete counters live in items.c. */
extern u8 level_exit_cell;             /* items.c — DGROUP 0x8572 (level exit cell)  */
extern u8 level_complete_anim_counter; /* items.c — DGROUP 0x8550 (exit-anim counter)*/

/* Deferred-contact event queue (game_post_present 1000:629c).  A far ptr that walks
 * a near DGROUP buffer at DS:0x0886: on first/empty call (pointee==0xff) it is reset
 * to point at the buffer head and the head stamped 0xff; otherwise a per-event
 * countdown gates a delayed contact-action(0x18) + contact sound, advancing the ptr.
 * These DGROUP bytes are unnamed in the engine (no Ghidra symbol); defined here as the
 * function's owning module.  The 0x0886 buffer head is the engine's DS:0x886 near
 * offset (deferred_contact_buf), reproduced here as the reset target. */
u8 __far *deferred_contact_ptr;        /* DGROUP 0x9ba6/0x9ba8 — event-queue cursor far ptr */
u8        deferred_contact_countdown;  /* DGROUP 0x79b7 — per-event delay countdown          */
u8        deferred_contact_buf[16];    /* DGROUP 0x0886 — the event buffer (head = reset tgt) */


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
 *   (2) The hardware-setup sub-calls (init_timer_resource_table=7bad,
 *       init_sound_tables=7563, init_display_97a4=97a4 / init_display_97f1=97f1,
 *       init_crtc_window=set_crtc_window 9821, set_display_page=9814,
 *       set_palette_mode=97c5, set_resource_table, install_interrupt_handler,
 *       init_joystick_handlers, mouse_reset, set_disk_swap_callback) are audio/CRTC/
 *       resource hardware init that cannot run without the full game data + real DOS;
 *       they are genuine HARDWARE-INIT CARVE-OUT stubs in game_stubs.c.
 *       install_keyboard_isr is the one REAL reconstructed call (input.c).
 *
 * Boot deviation: the original sets the display via set_crtc_window's (1000:9821)
 * CRTC block; here we additionally call video_set_mode_0d() so the T3 boot harness can assert
 * VGA mode 0x0D is set (the only externally observable boot effect).  DEVIATION.
 * ============================================================================ */
void init_game_session_state(void)
{
    set_disk_swap_callback(0x698, 0x6a9);
    init_timer_resource_table(0x6fac, 0x203b);   /* 1000:7bad bgi_overlay_thunk_adab */
    install_interrupt_handler();
    install_keyboard_isr();                        /* REAL (input.c) */
    init_joystick_handlers();
    mouse_reset();
    init_sound_tables(0x4c00, 0x4cd0, 0x203b);    /* 1000:7563 init_sound_tables */
    init_misc_7bd7();                              /* 1000:7bd7 bgi_overlay_thunk_gfx_init */
    init_display_97a4();                           /* 1000:97a4 init_display_controller_a */
    init_misc_7bbd(2);                             /* 1000:7bbd bgi_overlay_thunk_0232 */
    init_display_97f1();                           /* 1000:97f1 init_display_controller_b */
    init_crtc_window(0, 0, 0x13f, 199);            /* 1000:9821 set_crtc_window */
    set_display_page(1);                           /* 1000:9814 set_active_display_page */
    set_palette_mode(0xe, 1);                       /* 1000:97c5 set_palette_display_mode */
    set_resource_table(0x90, 0x203b);

    /* Set VGA mode 0x0D (320x200x16 EGA planar). DEVIATION: see header note —
       the original does this inside the set_crtc_window (1000:9821) CRTC block;
       surfaced here so the boot harness has an observable mode set. */
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
 * Ported 1:1 from the decomp.  Sub-call resolution (Phase-9 T4):
 *   load_current_level_data       1000:32b0 — CARVE-OUT (engine standalone loader):
 *                                  copies the level header into the tilemap buffer;
 *                                  this slice loads via level.c start_level, so this
 *                                  loader stays a game_stubs.c stub.
 *   spawn_and_draw_level_entities 1000:2a78 — REAL: reconstructed 1:1 in src/spawn.c
 *                                  (Phase-8 T2; the channel-A/B record populator +
 *                                  layer-C blitter).  Resolves to spawn.obj.
 *   reset_round_counters          1000:31de (init_round_state) — CARVE-OUT
 *                                  (never-decompiled): Ghidra-labelled but its
 *                                  decompilation fails (UNCERTAIN); no-op stub.
 * ============================================================================ */
void reset_game_state(void)
{
    load_current_level_data();
    spawn_and_draw_level_entities();
    reset_round_counters();          /* 1000:31de init_round_state — UNCERTAIN carve-out */
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
 * Phase-9 T4 — per-tick callee resolution: every callee below resolves to a REAL
 * reconstructed module body (player.c / player2.c / anim.c / screens.c / level.c /
 * input.c / spawn.c via reset_game_state / game.c's own post-present/input helpers),
 * EXCEPT the documented carve-out set that stays a faithful-signature stub in
 * game_stubs.c: init_sprite_structs, init_fullscreen_view_desc, apply_level_palette,
 * show_text_screen, show_pause_screen (render-core leaves); present_frame (CRTC
 * page-flip); run_n_frames / rotate_timing_flags_and_wait / wait_keypress
 * (int8-timing).  tools/validate_integration.sh asserts exactly this partition.
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
                        game_post_present();             /* 1000:629c */
                        handle_gameplay_input();
                        p2_tile_move_check();
                        check_pvp_collision();
                        game_post_input();                /* 1000:233a */
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

/* ============================================================================
 * game_post_present — 1000:629c   (Phase-9 T3; the per-tick post-present helper)
 * ----------------------------------------------------------------------------
 * Drives the DEFERRED-CONTACT event queue, one step per tick, AFTER the frame is
 * presented (game_loop calls it right after rotate_timing_flags_and_wait).  This is
 * REAL game logic (not hardware/CRTC): a far ptr (deferred_contact_ptr @ 0x9ba6)
 * walks a near event buffer at DS:0x886; a per-event countdown (deferred_contact_
 * countdown @ 0x79b7) delays each event's contact-action + sound.
 *
 *   if (*deferred_contact_ptr == 0xff):      // queue empty / uninitialised
 *       deferred_contact_ptr = (DS:0x886);   // reset cursor to the buffer head
 *       *deferred_contact_ptr = 0xff;        // stamp head as empty
 *   else if (countdown == 0):                // event due now
 *       countdown = 0x0a;                    // re-arm the per-event delay
 *       p1_cell_prev = *deferred_contact_ptr;// the queued event's cell
 *       apply_contact_action(0x18);          // fire the deferred contact (1000:6a89)
 *       play_sound(sound_device_state==4 ? 0x11 : 0x0e);  // device-dependent id
 *       deferred_contact_ptr++;              // advance to the next queued event
 *   else:
 *       countdown--;                         // still counting down
 *
 * RECONSTRUCTION DECISION (reconstruct, NOT carve-out): the disasm (1000:629c..6304,
 * decompiled fresh via Ghidra MCP) is pure game logic — a far-ptr queue walk, a
 * countdown, and two engine-fn calls (apply_contact_action, play_sound).  No port
 * I/O, no CRTC.  Reconstructed 1:1.  Verified against the disasm:
 *   62a8 LES BX,[0x9ba6]; 62ac CMP ES:[BX],0xff; 62b2 MOV [0x9ba6],0x886 / [0x9ba8],DS;
 *   62c0 MOV ES:[BX],0xff; 62c6 AL=[0x79b7]; 62cd JNZ dec; 62cf MOV [0x79b7],0xa;
 *   62db MOV [0x8570],*ptr (p1_cell_prev); 62e1 CALL 6a89(0x18); 62e6 CMP [0x689c],4 ->
 *   0x0e : 0x11; 62f4 CALL 6e11 (play_sound); 62f9 INC word [0x9ba6]; 62ff DEC [0x79b7].
 *
 * RECONSTRUCTION FIDELITY (the DS:0x886 reset target): the engine writes the literal
 * near offset 0x886 + the live DS into the far ptr.  Here the reset points the ptr at
 * the reconstructed deferred_contact_buf (the module-owned event buffer) — the same
 * head the engine's DS:0x886 names.  Functionally identical (the cursor walks the
 * buffer); the literal DGROUP offset is not load-bearing for the queue walk.
 */
void game_post_present(void)
{
    if (*deferred_contact_ptr == 0xff) {
        deferred_contact_ptr = (u8 __far *)deferred_contact_buf;   /* reset cursor (DS:0x886) */
        *deferred_contact_ptr = 0xff;                              /* stamp head empty        */
    } else if (deferred_contact_countdown == 0) {
        deferred_contact_countdown = 0x0a;                         /* re-arm delay            */
        p1_cell_prev = *deferred_contact_ptr;                      /* queued event cell       */
        apply_contact_action(0x18);                                /* 1000:6a89               */
        play_sound((sound_device_state == 4) ? 0x0e : 0x11);       /* ==4 ? 0x0e : 0x11 per disasm 62eb */
        deferred_contact_ptr = deferred_contact_ptr + 1;           /* advance cursor          */
    } else {
        deferred_contact_countdown = (u8)(deferred_contact_countdown - 1);
    }
    return;
}

/* ============================================================================
 * game_post_input — 1000:233a   (Phase-9 T3; the per-tick post-input helper)
 * ----------------------------------------------------------------------------
 * The level-complete EXIT-animation tick, run AFTER input each frame (game_loop
 * calls it after p2_tile_move_check / check_pvp_collision).  Once the level is
 * complete (level_complete_flag != 0), it counts level_complete_anim_counter up to
 * 9, then resets it, relocates the anim target to the exit cell, and fires the exit
 * animation (apply_cell_animation('Z')).  REAL game logic.
 *
 *   if (level_complete_flag != 0):
 *       if (level_complete_anim_counter == 9):
 *           level_complete_anim_counter = 0;
 *           anim_target_cell = level_exit_cell;
 *           apply_cell_animation('Z');          // 0x5a — exit FX (1000:69aa)
 *       else:
 *           level_complete_anim_counter++;
 *
 * Ported 1:1 from the live decomp (1000:233a; Ghidra-named globals confirmed:
 * level_complete_flag @ 0xa1b1, level_complete_anim_counter @ 0x8550, level_exit_cell
 * @ 0x8572, anim_target_cell @ 0x856f).  apply_cell_animation (69aa) is the
 * anim-channel allocator RECONSTRUCTED in anim.c (Phase 5).
 */
void game_post_input(void)
{
    if (level_complete_flag != 0) {
        if (level_complete_anim_counter == 9) {
            level_complete_anim_counter = 0;
            anim_target_cell = level_exit_cell;
            apply_cell_animation('Z');                             /* 0x5a — exit FX */
        } else {
            level_complete_anim_counter = (u8)(level_complete_anim_counter + 1);
        }
    }
    return;
}

/* ── Phase-9 T3: init_view_anim_descriptors cross-module view far pointers ─────
 * The one-time descriptor setup writes into view-descriptor structs owned by
 * several modules.  Declared extern here (defined by their owning TU):
 *   render_descriptor_ptr (0x574), fullscreen_buf/_seg (0x7926/0x7928) — screens.c
 *   p1_view (0x8b8), p1_erase_view (0x8c4), pending_erase_view (0x8e4)   — player.c
 *   p2_view (0x8ec), p2_erase_view (0x8e8)                               — player2.c
 *   anim_b_clear_view (0x8bc), anim_a_clear_view (0x8c0),
 *   anim_b_draw_view (0x8d0), anim_a_erase_view (0x8d4),
 *   anim_a_draw_view (0x8e0)                                             — anim.c
 * The four P2-side anim view descriptors at DGROUP 0x8c8/0x8cc/0x8d8/0x8dc are
 * UNNAMED in the engine (no Ghidra symbol; written only here) — owned here as the
 * function's home, named for their role (P2 anim clear/draw/erase views). */
extern u8 __far *render_descriptor_ptr;   /* screens.c 0x574 */
extern u16 fullscreen_buf;                 /* screens.c 0x7926 */
extern u16 fullscreen_buf_seg;             /* screens.c 0x7928 */
extern u8 __far *p1_view;                  /* player.c 0x8b8 */
extern u8 __far *p1_erase_view;            /* player.c 0x8c4 */
extern u8 __far *pending_erase_view;       /* player.c 0x8e4 */
extern u8 __far *p2_view;                  /* player2.c 0x8ec */
extern u8 __far *p2_erase_view;            /* player2.c 0x8e8 */
extern u8 __far *anim_b_clear_view;        /* anim.c 0x8bc */
extern u8 __far *anim_a_clear_view;        /* anim.c 0x8c0 */
extern u8 __far *anim_b_draw_view;         /* anim.c 0x8d0 */
extern u8 __far *anim_a_erase_view;        /* anim.c 0x8d4 */
extern u8 __far *anim_a_draw_view;         /* anim.c 0x8e0 */
/* The four unnamed P2-side anim view descriptors (owned here). */
u8 __far *p2_anim_clear_view_8c8;          /* DGROUP 0x8c8 — P2 anim clear/draw view (a) */
u8 __far *p2_anim_clear_view_8cc;          /* DGROUP 0x8cc — P2 anim clear view (b)       */
u8 __far *p2_anim_erase_view_8d8;          /* DGROUP 0x8d8 — P2 anim erase view (a)       */
u8 __far *p2_anim_erase_view_8dc;          /* DGROUP 0x8dc — P2 anim erase view (b)       */

/* ============================================================================
 * init_view_anim_descriptors — 1000:535e   (Phase-9 T3)
 * ----------------------------------------------------------------------------
 * One-time (run_game_session) setup of every per-tick view descriptor: the P1/P2
 * render + erase views, the channel-A/B anim clear/draw/erase views (both player
 * sides), the in-game status-row render descriptor, and the deferred pending-erase
 * view.  Each is a fixed struct-init: sprite/work-buffer far ptr + rect dims +
 * sub-handler tag.  Ported 1:1 from the live disasm (1000:535e..5680).
 *
 * RECONSTRUCTION FIDELITY:
 *   (a) The far-ptr SEG halves the engine writes as the literal 0x8e8d/0x9294/0x8888
 *       (work-buffer offsets) + DS (the runtime DGROUP register).  The data SEG
 *       stores use the static-image DGROUP literal 0x203b (the same convention as
 *       anim.c's view descriptors — see its DS-segment FIDELITY note); the harness
 *       gate compares the descriptor BYTES the engine actually wrote.
 *   (b) The four P2-side anim view descriptors (0x8c8/0x8cc/0x8d8/0x8dc) are unnamed
 *       in the engine; owned + named here (see the extern block above).
 * The field offsets/values are verbatim from the asm (LES BX,[view]; MOV ES:[BX+N],V).
 * ============================================================================ */
void init_view_anim_descriptors(void)
{
    u8 __far *v;
    u16 fb_off = fullscreen_buf;            /* DGROUP 0x7926 (asm: DX = [0x7926]) */
    u16 fb_seg = fullscreen_buf_seg;        /* DGROUP 0x7928 (asm: AX = [0x7928]) */

    /* render_descriptor_ptr (0x574): clear the +0x22..+0x25 status bytes. */
    v = render_descriptor_ptr;
    v[0x22] = 0; v[0x23] = 0; v[0x24] = 0; v[0x25] = 0;

    /* p1_view (0x8b8): word[0]=1; +0x10/0x12 = work-buf 0x8e8d:DS; +0x14..0x1c rect. */
    v = p1_view;
    *(u16 __far *)(v + 0x00) = 1;
    *(u16 __far *)(v + 0x10) = 0x8e8d;
    *(u16 __far *)(v + 0x12) = GAME_DGROUP_RUNTIME_SEG;
    *(u16 __far *)(v + 0x14) = 0;
    *(u16 __far *)(v + 0x16) = 0;
    *(u16 __far *)(v + 0x18) = 4;
    *(u16 __far *)(v + 0x1a) = 4;
    *(u16 __far *)(v + 0x1c) = 0;

    /* p2_view (0x8ec): same shape, work-buf 0x9294:DS. */
    v = p2_view;
    *(u16 __far *)(v + 0x00) = 1;
    *(u16 __far *)(v + 0x10) = 0x9294;
    *(u16 __far *)(v + 0x12) = GAME_DGROUP_RUNTIME_SEG;
    *(u16 __far *)(v + 0x14) = 0;
    *(u16 __far *)(v + 0x16) = 0;
    *(u16 __far *)(v + 0x18) = 4;
    *(u16 __far *)(v + 0x1a) = 4;
    *(u16 __far *)(v + 0x1c) = 0;

    /* anim_b_clear_view (0x8bc). */
    v = anim_b_clear_view;
    *(u16 __far *)(v + 0x06) = 0;
    *(u16 __far *)(v + 0x08) = 0;
    *(u16 __far *)(v + 0x0a) = 1;
    *(u16 __far *)(v + 0x0c) = 4;
    *(u16 __far *)(v + 0x0e) = 1;
    *(u16 __far *)(v + 0x1c) = 0;
    *(u16 __far *)(v + 0x1e) = 1;
    *(u16 __far *)(v + 0x20) = 4;

    /* anim_a_clear_view (0x8c0). */
    v = anim_a_clear_view;
    *(u16 __far *)(v + 0x06) = 0;
    *(u16 __far *)(v + 0x08) = 0;
    *(u16 __far *)(v + 0x0a) = 3;
    *(u16 __far *)(v + 0x0c) = 2;
    *(u16 __far *)(v + 0x0e) = 1;
    *(u16 __far *)(v + 0x1c) = 0;
    *(u16 __far *)(v + 0x1e) = 2;
    *(u16 __far *)(v + 0x20) = 2;

    /* p1_erase_view (0x8c4): +2/+4 = work-buf 0x8e8d:DS. */
    v = p1_erase_view;
    *(u16 __far *)(v + 0x02) = 0x8e8d;
    *(u16 __far *)(v + 0x04) = GAME_DGROUP_RUNTIME_SEG;
    *(u16 __far *)(v + 0x06) = 0;
    *(u16 __far *)(v + 0x08) = 0;
    *(u16 __far *)(v + 0x0a) = 4;
    *(u16 __far *)(v + 0x0c) = 4;
    *(u16 __far *)(v + 0x0e) = 1;
    *(u16 __far *)(v + 0x1c) = 0;

    /* p2_erase_view (0x8e8): +2/+4 = work-buf 0x9294:DS. */
    v = p2_erase_view;
    *(u16 __far *)(v + 0x02) = 0x9294;
    *(u16 __far *)(v + 0x04) = GAME_DGROUP_RUNTIME_SEG;
    *(u16 __far *)(v + 0x06) = 0;
    *(u16 __far *)(v + 0x08) = 0;
    *(u16 __far *)(v + 0x0a) = 4;
    *(u16 __far *)(v + 0x0c) = 4;
    *(u16 __far *)(v + 0x0e) = 1;
    *(u16 __far *)(v + 0x1c) = 0;

    /* p2_anim_clear_view_8c8 (0x8c8): +2/+4 = fullscreen_buf:seg; sprite 0x8888:DS. */
    v = p2_anim_clear_view_8c8;
    *(u16 __far *)(v + 0x02) = fb_off;
    *(u16 __far *)(v + 0x04) = fb_seg;
    *(u16 __far *)(v + 0x0a) = 0x14;
    *(u16 __far *)(v + 0x0c) = 0x19;
    *(u16 __far *)(v + 0x10) = 0x8888;
    *(u16 __far *)(v + 0x12) = GAME_DGROUP_RUNTIME_SEG;
    *(u16 __far *)(v + 0x14) = 0;
    *(u16 __far *)(v + 0x16) = 0;
    *(u16 __far *)(v + 0x18) = 3;
    *(u16 __far *)(v + 0x1a) = 4;
    *(u16 __far *)(v + 0x1c) = 0;
    *(u16 __far *)(v + 0x1e) = 1;
    *(u16 __far *)(v + 0x20) = 4;

    /* p2_anim_clear_view_8cc (0x8cc): sprite 0x8888:DS. */
    v = p2_anim_clear_view_8cc;
    *(u16 __far *)(v + 0x06) = 0;
    *(u16 __far *)(v + 0x08) = 0;
    *(u16 __far *)(v + 0x0c) = 4;
    *(u16 __far *)(v + 0x10) = 0x8888;
    *(u16 __far *)(v + 0x12) = GAME_DGROUP_RUNTIME_SEG;
    *(u16 __far *)(v + 0x0e) = 1;
    *(u16 __far *)(v + 0x18) = 3;
    *(u16 __far *)(v + 0x1a) = 4;
    *(u16 __far *)(v + 0x1e) = 1;
    *(u16 __far *)(v + 0x20) = 4;

    /* anim_b_draw_view (0x8d0). */
    v = anim_b_draw_view;
    *(u16 __far *)(v + 0x00) = 1;
    *(u16 __far *)(v + 0x14) = 0;
    *(u16 __far *)(v + 0x16) = 0;
    *(u16 __far *)(v + 0x18) = 1;
    *(u16 __far *)(v + 0x1a) = 4;
    *(u16 __far *)(v + 0x1c) = 0;
    *(u16 __far *)(v + 0x1e) = 1;
    *(u16 __far *)(v + 0x20) = 4;

    /* anim_a_erase_view (0x8d4): +2/+4 = fullscreen_buf:seg. */
    v = anim_a_erase_view;
    *(u16 __far *)(v + 0x02) = fb_off;
    *(u16 __far *)(v + 0x04) = fb_seg;
    *(u16 __far *)(v + 0x0a) = 0x14;
    *(u16 __far *)(v + 0x0c) = 0x19;
    *(u16 __far *)(v + 0x0e) = 1;
    *(u16 __far *)(v + 0x1e) = 2;
    *(u16 __far *)(v + 0x20) = 2;

    /* p2_anim_erase_view_8d8 (0x8d8): +2/+4 = fullscreen_buf:seg; sprite 0x8888:DS. */
    v = p2_anim_erase_view_8d8;
    *(u16 __far *)(v + 0x02) = fb_off;
    *(u16 __far *)(v + 0x04) = fb_seg;
    *(u16 __far *)(v + 0x0a) = 0x14;
    *(u16 __far *)(v + 0x0c) = 0x19;
    *(u16 __far *)(v + 0x10) = 0x8888;
    *(u16 __far *)(v + 0x12) = GAME_DGROUP_RUNTIME_SEG;
    *(u16 __far *)(v + 0x14) = 0;
    *(u16 __far *)(v + 0x16) = 0;
    *(u16 __far *)(v + 0x18) = 3;
    *(u16 __far *)(v + 0x1a) = 4;
    *(u16 __far *)(v + 0x1c) = 0;
    *(u16 __far *)(v + 0x1e) = 3;
    *(u16 __far *)(v + 0x20) = 2;

    /* p2_anim_erase_view_8dc (0x8dc): sprite 0x8888:DS. */
    v = p2_anim_erase_view_8dc;
    *(u16 __far *)(v + 0x06) = 0;
    *(u16 __far *)(v + 0x08) = 0;
    *(u16 __far *)(v + 0x0a) = 3;
    *(u16 __far *)(v + 0x0e) = 1;
    *(u16 __far *)(v + 0x10) = 0x8888;
    *(u16 __far *)(v + 0x12) = GAME_DGROUP_RUNTIME_SEG;
    *(u16 __far *)(v + 0x18) = 3;
    *(u16 __far *)(v + 0x1a) = 4;
    *(u16 __far *)(v + 0x1e) = 3;
    *(u16 __far *)(v + 0x20) = 2;

    /* anim_a_draw_view (0x8e0). */
    v = anim_a_draw_view;
    *(u16 __far *)(v + 0x00) = 1;
    *(u16 __far *)(v + 0x14) = 0;
    *(u16 __far *)(v + 0x16) = 0;
    *(u16 __far *)(v + 0x18) = 3;
    *(u16 __far *)(v + 0x1a) = 2;
    *(u16 __far *)(v + 0x1e) = 3;
    *(u16 __far *)(v + 0x20) = 2;

    /* pending_erase_view (0x8e4): +2/+4 = fullscreen_buf:seg. */
    v = pending_erase_view;
    *(u16 __far *)(v + 0x02) = fb_off;
    *(u16 __far *)(v + 0x04) = fb_seg;
    *(u16 __far *)(v + 0x0a) = 0x14;
    *(u16 __far *)(v + 0x0c) = 0x19;
    *(u16 __far *)(v + 0x0e) = 1;
    *(u16 __far *)(v + 0x1c) = 0;
    *(u16 __far *)(v + 0x20) = 2;
    return;
}
