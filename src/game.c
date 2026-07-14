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
 *       if (stack_check_limit <= &stack0xfffe) borland_stack_overflow();
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
#ifdef BUMPY_PLAYABLE
#include "host/host.h"  /* host_tick — the relocated INT8-timing leaves spin on it */
#include "screens.h"    /* timing_flag_accumulator (DGROUP 0x854f) */
#include "dosio.h"      /* dosio_save — live per-tick diagnostic logger (below) */
#endif

/* init_view_anim_descriptors writes its view-descriptor far-ptr SEG halves as the
   DS register (the runtime DGROUP segment).  The static-image link-time DS literal is
   0x203b; the live engine DS at this call is 0x114b (= 0x110 PSP+10 + 0x103b).  The
   descriptor-byte gate (tools/p1_spine_ctest.c) overrides this to the captured runtime
   value so it can compare the genuine engine-written bytes; the source default keeps
   the decomp's static 0x203b.  Same convention as anim.c's ANIM_DGROUP_RUNTIME_SEG. */
#ifndef GAME_DGROUP_RUNTIME_SEG
#ifdef BUMPY_PLAYABLE
extern u16 host_dgroup_seg(void);   /* host_render.c — loaded image's real DGROUP seg */
#define GAME_DGROUP_RUNTIME_SEG host_dgroup_seg()
#else
#define GAME_DGROUP_RUNTIME_SEG 0x203b
#endif
#endif

/* ── Session / round / tick control flags (OWNED here) ──────────────────────── */
u8 round_continue_flag;      /* DGROUP 0x9d30 (DAT_9d30) */
u8 session_continue_flag;    /* DGROUP 0x856d (DAT_856d) */
u8 frame_abort_flag;         /* DGROUP 0x928d (DAT_203b_928d) */

/* ── game_loop / init game-state globals OWNED here (no single module home) ──── */
u8  current_level_index;     /* DGROUP 0x7310 — 0-based level index (= current_level-1) */
u8  palette_loaded;          /* DGROUP — set 0 by init, by apply_level_palette later   */
u8  menu_option2_setting;    /* game_loop menu scratch */
u8  settle_countdown;        /* DGROUP 0x791a — ONE engine byte, four users (unified
                                2026-07-02; was split into settle_countdown +
                                items.c sharp_item_counter → draw_icon_row always
                                read 0 and '#' pickups never extended the counter):
                                game_loop inits =5; '#' item pickup ++ (items.c);
                                run_physics_settle -- (player.c); draw_icon_row
                                draws that many icons (screens.c). */
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
 *       fidelity audit.  VERIFIED HARMLESS (audit 2026-06-28): every opaque reset,
 *       including the six NON-zero defaults (0x6bc5=1, 0x6bc4=0x14, 0x6bf6=1,
 *       0x6c26=1, 0x7316=0x6c, 0x6bc7=9), is read NOWHERE in the reconstruction, so
 *       the collapse is behaviourally faithful (BSS-zero matches the zero defaults;
 *       the non-zero ones are dead).  Reproducing them as ~46 named externs would
 *       invent structure the binary does not document.
 *   (2) The hardware-setup sub-calls (init_timer_resource_table=7bad,
 *       init_sound_tables=7563, sound_device_select_screen_thunk=97a4 / gfx_draw_sequence_thunk=97f1,
 *       init_crtc_window=9821, set_display_page=9814,
 *       set_text_color=97c5, set_resource_table, install_interrupt_handler,
 *       init_joystick_handlers, mouse_reset, set_disk_swap_callback) are audio/CRTC/
 *       resource hardware init that cannot run without the full game data + real DOS;
 *       they are genuine HARDWARE-INIT CARVE-OUT stubs in game_stubs.c.
 *       install_keyboard_isr is the one REAL reconstructed call (input.c).
 *
 * Boot deviation: the original's boot-time mode set lives in the graphics-overlay
 * init (absent from the corpus) — NOT in init_crtc_window (1000:9821), which only
 * stores a clip-window record and never touches the CRTC (see host_video.c); here
 * we additionally call video_set_mode_0d() so the T3 boot harness can assert
 * VGA mode 0x0D is set (the only externally observable boot effect).  DEVIATION.
 * ============================================================================ */
void init_game_session_state(void)
{
    set_disk_swap_callback(0x698, 0x6a9);
    init_timer_resource_table(0x6fac, 0x203b);   /* 1000:7bad gfx_overlay_thunk_adab */
    install_interrupt_handler();
    install_keyboard_isr();                        /* REAL (input.c) */
    init_joystick_handlers();
    mouse_reset();
    init_sound_tables(0x4c00, 0x4cd0, 0x203b);    /* 1000:7563 init_sound_tables */
    gfx_overlay_thunk_init();                      /* 1000:7bd7 */
    sound_device_select_screen_thunk();            /* 1000:97a4 thunk→sound_device_select_screen (F5..F8 sound menu; already
                                                       run from main.c); host folds the mode-0x0D set into this slot */
    gfx_overlay_thunk_0232(2);                     /* 1000:7bbd */
    gfx_draw_sequence_thunk();                     /* 1000:97f1 → 1ab9:137b gfx_draw_sequence (text pos/window/page/colour init) */
    init_crtc_window(0, 0, 0x13f, 199);            /* 1000:9821 → 1ab9:1422 clip-window store (NO CRTC — see host_video.c) */
    set_display_page(1);                           /* 1000:9814 set_active_display_page */
    set_text_color(0xe, 1);                        /* 1000:97c5 → 1ab9:1311/14ef: session text colours fg=14, bg=1 (disasm @1000:02f1) */
    set_resource_table(0x90, 0x203b);

#ifndef BUMPY_PLAYABLE
    /* Set VGA mode 0x0D (320x200x16 EGA planar). DEVIATION: see header note —
       the original's mode set lives in the graphics-overlay init (absent from the
       corpus); surfaced here so the boot harness has an observable mode set.
       PLAYABLE build: the mode set is folded into sound_device_select_screen_thunk
       above — repeating it HERE would reset the CRTC start address to 0 AFTER
       gfx_draw_sequence_thunk programmed the 0x2000 boot page parity, inverting
       `displayed == page[table[0]]` for the whole session (the 2026-07-03
       menu-cursor hidden-page bug).  See host_video.c host_crtc_set_start. */
    video_set_mode_0d();
#endif /* !BUMPY_PLAYABLE */

    /* The named, in-scope session-default resets (decomp lines 448-494). */
    current_level_index = 0;
    palette_loaded      = 0;

    /* engine (1000:03b1-03e1): cur_level_ptr = DAT_6bd2 + current_level_index*0x32c;
       level_src_ptr = DAT_6bf2 + current_level_index*0xc2 — a real per-level table
       lookup (DAT_6bd2/DAT_6bf2 are the level-archive base pointers set by
       load_current_level_data, see level.c). NOT a literal NULL in general — but this
       is the ONE call site (init_game_session_state has a single caller, the program
       bootstrap, so this always runs before any level has ever loaded) where both the
       index (just zeroed above) AND the table bases themselves are 0: DAT_6bd2/DAT_6bd4
       and DAT_6bf2/DAT_6bf4 are compiled as 00 00 00 00 in the binary (verified via raw
       file bytes, 2026-07-14), so the formula evaluates to exactly NULL here. */
    cur_level_ptr       = (u8 __far *)0;
    level_src_ptr       = (u8 __far *)0;

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

/* ============================================================================
 * game_tick — one iteration of game_loop's innermost per-tick loop.
 * ----------------------------------------------------------------------------
 * RECONSTRUCTION FIDELITY (extraction-only deviation): the original game_loop
 * (1000:0c18) inlines this body inside its innermost `while` (game.c 331-367).
 * It is extracted here VERBATIM — same statements, same order — so the int8
 * end-to-end harness (tools/int8_ctest.c) can drive one tick at a time.  No
 * statement is added, removed, or reordered.  Recorded in the fidelity audit.
 * ============================================================================ */
#if defined(BUMPY_PLAYABLE) && defined(HOST_TICKLOG)
/* ----------------------------------------------------------------------------
 * HOST DIAGNOSTIC (playable-only, #ifdef BUMPY_PLAYABLE) — LIVE PER-TICK TRACE.
 * Inert in the faithful build (default BUMPY.EXE) and in the int8 gate (neither
 * defines BUMPY_PLAYABLE).  Not part of the decompilation.
 *
 * Records the same state fields the int8 differential trace captures, one 16-byte
 * record per game_tick, into a 256-entry ring.  Every 64 ticks the whole ring is
 * flushed to C:\TICKLOG.BIN (the mounted game dir -> local/build/capture/game/).
 * Each record carries a tick counter [14..15] so the offline parser can reorder
 * the wrapped ring and drop stale entries.  Purpose: catch the exact tick where
 * position changes without a matching fresh input (the "spontaneous movement"),
 * without needing the (bit-rotted) DOSBox original-capture harness.
 *
 * LAYOUT-NEUTRALITY (important): the 4 KB ring MUST live in FAR_DATA, NOT in the
 * near DGROUP.  This host is DGROUP-layout-sensitive (the _dgroup_pal_patch_* /
 * per-level pointer machinery); adding 4 KB of static BSS to DGROUP shifts every
 * global linked after it and regressed level rendering to all-black (vganz=0).
 * `__far` keeps the near-DGROUP image byte-identical to a build without the logger,
 * so the diagnostic can never perturb the very state it is trying to observe.
 * The flush therefore uses the streaming (far-buffer) dosio API, not dosio_save.
 * -------------------------------------------------------------------------- */
#define TICKLOG_RECS 256u
#define TICKLOG_RECSZ 16u
static u8 __far g_ticklog[TICKLOG_RECS * TICKLOG_RECSZ];
static u16 g_ticklog_idx = 0;      /* next record slot (wraps) */
static u16 g_ticklog_tick = 0;     /* monotonic tick counter (low 16 bits) */

static void ticklog_record(void)
{
    u8 __far *r = g_ticklog + (u16)g_ticklog_idx * TICKLOG_RECSZ;
    r[0]  = input_state;
    r[1]  = game_mode;
    r[2]  = prev_game_mode;
    r[3]  = p1_cell;
    r[4]  = (u8)(p1_pixel_x & 0xffu);
    r[5]  = (u8)((p1_pixel_x >> 8) & 0xffu);
    r[6]  = (u8)(p1_pixel_y & 0xffu);
    r[7]  = (u8)((p1_pixel_y >> 8) & 0xffu);
    r[8]  = p1_move_steps_left;
    r[9]  = move_step_count;
    r[10] = move_locked;
    r[11] = p1_contact_code;
    r[12] = p1_pending_action;
    r[13] = current_level;
    r[14] = (u8)(g_ticklog_tick & 0xffu);
    r[15] = (u8)((g_ticklog_tick >> 8) & 0xffu);

    g_ticklog_idx = (u16)((g_ticklog_idx + 1u) % TICKLOG_RECS);
    g_ticklog_tick++;
#ifndef TICKLOG_NOWRITE
    if ((g_ticklog_tick & 0x3fu) == 0u) {
        int fd = dosio_create("TICKLOG.BIN");   /* far-buffer streaming write */
        if (fd >= 0) {
            dosio_write(fd, g_ticklog, (u16)sizeof g_ticklog);
            dosio_close(fd);
        }
    }
#endif
}
#endif /* BUMPY_PLAYABLE */

void game_tick(void)
{
    u8 key;

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
    game_post_present();
    handle_gameplay_input();
    p2_tile_move_check();
    check_pvp_collision();
    game_post_input();
    key = get_key_state(0x19);
    if (key != 0) {
        show_pause_screen();
    }
#if defined(BUMPY_PLAYABLE) && defined(HOST_TICKLOG)
    ticklog_record();   /* host-only live per-tick trace (inert in faithful build) */
#endif
}

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
#ifndef AUTOKEY
                    wait_keypress();        /* level-entry 'press a key to begin' pause.
                                               AUTOKEY (headless capture harness only)
                                               skips this so captures reach gameplay. */
#endif
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
                        game_tick();
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
 *       play_sound(sound_device_state==4 ? 0x0e : 0x11);  // device-dependent id (==4 → 0x0e per disasm 62e6)
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
extern u8 __far *anim_b_view0;             /* anim.c 0x8c8 — ALIAS of p2_anim_clear_view_8c8 */
extern u8 __far *anim_b_view1;             /* anim.c 0x8cc — ALIAS of p2_anim_clear_view_8cc */
/* The four unnamed P2-side anim view descriptors (owned here).  NOTE: 0x8c8/0x8cc
 * are ALSO written by draw_anim_channels_b (anim.c anim_b_view0/anim_b_view1) — the
 * recon carries two pointer names for the same engine descriptor, so BOTH names must
 * be bound to the same slot (fixed 2026-07-02: anim_b_view0/1 were left NULL, so any
 * active layer-B channel far-stored through 0000:xxxx). */
u8 __far *p2_anim_clear_view_8c8;          /* DGROUP 0x8c8 — P2 anim clear/draw view (a) */
u8 __far *p2_anim_clear_view_8cc;          /* DGROUP 0x8cc — P2 anim clear view (b)       */
u8 __far *p2_anim_erase_view_8d8;          /* DGROUP 0x8d8 — P2 anim erase view (a)       */
u8 __far *p2_anim_erase_view_8dc;          /* DGROUP 0x8dc — P2 anim erase view (b)       */

#ifdef BUMPY_PLAYABLE
/* ============================================================================
 * host_view_descriptors_init — PLAYABLE-BUILD storage binding (Plan A, Task 9)
 * ----------------------------------------------------------------------------
 * INTEGRATION FIX (host-leaf; default build byte-unchanged — see note below).
 *
 * In the ORIGINAL engine the per-tick view-descriptor structs (p1_view 0x8b8,
 * p2_view 0x8ec, the channel-A/B anim views 0x8bc..0x8e0, the four unnamed P2-side
 * anim views 0x8c8..0x8dc, the in-game status render descriptor 0x574, and the
 * deferred pending-erase view 0x8e4) live as fixed-offset storage inside DGROUP;
 * the "far pointers" to them (LES BX,[view]) are simply DGROUP addresses set up at
 * startup.  The reconstruction split each descriptor's POINTER into its own C
 * global (owned by screens.c / player.c / player2.c / anim.c / game.c) but the
 * BACKING STORAGE those pointers must reference was never allocated in the binary
 * itself — only the differential test harnesses (the tools _ctest.c set) bound them
 * to static buffers.  init_view_anim_descriptors (below) writes THROUGH every one of
 * these pointers; with them left NULL (BSS-zero = far 0000:0000) the writes land in
 * the interrupt vector table, and the next host INT8/PIT tick then vectors through
 * the corrupted IVT → invalid-opcode (#UD) → triple fault.  (Diagnosed in the Task-9
 * boot bring-up: fault at near-null CS:IP right after init_view_anim_descriptors.)
 *
 * This leaf binds all 15 descriptor pointers to 0x40-byte slots within ONE static
 * DGROUP buffer (init_view_anim_descriptors writes at most +0x20 → 0x22 bytes; 0x40
 * matches the test harnesses' VIEW_LEN headroom).  Called from the playable main
 * BEFORE run_game_session (which calls init_view_anim_descriptors).
 *
 * Why a STATIC DGROUP buffer (not _fmalloc): in the original these structs ARE
 * DGROUP-resident, so a static DGROUP block is the faithful backing.  It is also
 * robust: the Task-9 boot bring-up first tried _fmalloc here, but host_fb_init's
 * preceding halloc(4*0x10000 = 256 KB) huge-block allocation exhausted the far heap,
 * so _fmalloc returned NULL, the binder early-returned, every descriptor pointer was
 * left BSS-zero (far 0000:0000), and show_title_background's
 * `LDS BX,render_descriptor_ptr` then loaded DS:BX = 0000:0000 and wrote the view
 * fields straight into the interrupt vector table — the next PIT/INT8 tick vectored
 * through the corrupted IVT and the CPU ran off into DGROUP (diagnosed live: DS→0000,
 * a far-RET popping CS=DGROUP, IP sliding through zeroed BSS).  A static buffer has
 * no allocation to fail and matches the engine's DGROUP-resident layout.
 *
 * RECONSTRUCTION FIDELITY: this is a HOST-PLATFORM storage-binding leaf, not engine
 * logic.  The original needs no such call (the structs are DGROUP-resident); the
 * reconstruction's pointer-split layout requires the backing to be bound explicitly.
 * It is #ifdef BUMPY_PLAYABLE and invoked only from the playable main, so the
 * default BUMPY.EXE is byte-unchanged.  Recorded in docs/reconstruction-fidelity.md
 * ("playable host: view-descriptor storage binding").
 * ============================================================================ */
#define HV_DESC_LEN   0x40u    /* per-descriptor slot (>= 0x22 written; harness uses 0x40) */
#define HV_DESC_COUNT 15u      /* 15 view-descriptor pointers bound below */

/* Static DGROUP backing for the 15 view descriptors (faithful: DGROUP-resident; no
 * far-heap dependency).  Zero-initialised by the C runtime (BSS). */
static u8 s_host_view_desc_blk[HV_DESC_LEN * HV_DESC_COUNT];

void host_view_descriptors_init(void)
{
    u8 __far *blk;
    u16 i;

    /* Far pointer to the static DGROUP block, then zero it (idempotent re-init). */
    blk = (u8 __far *)s_host_view_desc_blk;
    for (i = 0; i < (u16)(HV_DESC_LEN * HV_DESC_COUNT); i++) {
        blk[i] = 0;
    }

    /* Bind each descriptor pointer to its own 0x40-byte slot. */
    render_descriptor_ptr  = blk + 0x00u * HV_DESC_LEN;   /* screens.c 0x574 */
    p1_view                = blk + 0x01u * HV_DESC_LEN;   /* player.c  0x8b8 */
    p1_erase_view          = blk + 0x02u * HV_DESC_LEN;   /* player.c  0x8c4 */
    pending_erase_view     = blk + 0x03u * HV_DESC_LEN;   /* player.c  0x8e4 */
    p2_view                = blk + 0x04u * HV_DESC_LEN;   /* player2.c 0x8ec */
    p2_erase_view          = blk + 0x05u * HV_DESC_LEN;   /* player2.c 0x8e8 */
    anim_b_clear_view      = blk + 0x06u * HV_DESC_LEN;   /* anim.c    0x8bc */
    anim_a_clear_view      = blk + 0x07u * HV_DESC_LEN;   /* anim.c    0x8c0 */
    anim_b_draw_view       = blk + 0x08u * HV_DESC_LEN;   /* anim.c    0x8d0 */
    anim_a_erase_view      = blk + 0x09u * HV_DESC_LEN;   /* anim.c    0x8d4 */
    anim_a_draw_view       = blk + 0x0au * HV_DESC_LEN;   /* anim.c    0x8e0 */
    p2_anim_clear_view_8c8 = blk + 0x0bu * HV_DESC_LEN;   /* game.c    0x8c8 */
    p2_anim_clear_view_8cc = blk + 0x0cu * HV_DESC_LEN;   /* game.c    0x8cc */
    anim_b_view0           = p2_anim_clear_view_8c8;      /* anim.c    0x8c8 (same desc) */
    anim_b_view1           = p2_anim_clear_view_8cc;      /* anim.c    0x8cc (same desc) */
    p2_anim_erase_view_8d8 = blk + 0x0du * HV_DESC_LEN;   /* game.c    0x8d8 */
    p2_anim_erase_view_8dc = blk + 0x0eu * HV_DESC_LEN;   /* game.c    0x8dc */
}

/* ── rotate_timing_flags_and_wait (1000:1349) ───────────────────────────────────
 * Engine per-tick frame pacing (relocated from src/host/host_timer.c — engine
 * logic, not the hardware ISR).  Read timing_flag_accumulator bit 0, ROR the byte
 * by 1, and wait 2 host ticks if the carried-out bit was 1 else 1 tick. */
void rotate_timing_flags_and_wait(void)
{
    unsigned char frames_to_wait;
    unsigned char carry_bit;
    unsigned char acc;

    frames_to_wait = 1u;
    acc = timing_flag_accumulator;

    if ((acc & 0x01u) != 0u) {
        carry_bit = 0x80u;    /* LSB was 1 — wraps to MSB */
        frames_to_wait = 2u;
    } else {
        carry_bit = 0x00u;
    }

    /* ROR 1: logical right-shift then OR in the carry. */
    timing_flag_accumulator = (unsigned char)((acc >> 1u) | carry_bit);

    run_n_frames((unsigned char)frames_to_wait);
}

/* ── run_n_frames (1000:05e7) ───────────────────────────────────────────────────
 * Wait n frame ticks.  1:1 with the engine (1000:05e7): `while (n) { wait_vretrace_
 * thunk(); n--; }` — the engine paces each game frame on the VGA VERTICAL RETRACE
 * (wait_vretrace_thunk -> 2036:0015 vsync poll, ~70 Hz for mode 0x0D), NOT on the
 * ~500 Hz PIT/INT8 tick.
 *
 * RECONSTRUCTION FIDELITY (bug fix 2026-06-30): this previously spun on host_tick
 * (the 500 Hz INT8 ISR primitive), which paced the game loop ~7x too fast (~333 fps
 * vs the engine's ~35-70 fps) — the user-reported "movement too fast".  The host
 * INT8 ISR (host_timer.c) is still installed and drives sound/BIOS-clock chaining at
 * the engine's 500 Hz divisor; it is simply not the FRAME-pace source.  The decompiled
 * engine run_n_frames calls wait_vretrace_thunk, so this is both the faithful and the
 * correct behaviour. */
void run_n_frames(unsigned char n)
{
    extern void wait_vretrace_thunk(void);   /* screens.c — 1000:9864 vsync wait */

    while (n != 0u) {
        wait_vretrace_thunk();
        n--;
    }
}
#endif /* BUMPY_PLAYABLE */

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
/* Recurring view-descriptor field offset legend (mirrors anim.c's writers for the
 * same descriptor layout — see draw_anim_channels_a/b for the per-field breakdown
 * cited inline there):
 *   +0x00       flag word (1 = active/in-use)
 *   +0x02/+0x04 far-data source (some descriptors only)
 *   +0x06/+0x08 sprite-sheet/screen_sprite_buf far ptr (some descriptors only)
 *   +0x0a       flags byte (some descriptors only)
 *   +0x0c       additional per-descriptor field, view-type dependent (some
 *               descriptors only; exact role not independently identified here)
 *   +0x0e       sub-handler tag (some descriptors only)
 *   +0x10/+0x12 work-buffer far ptr off/seg
 *   +0x14/+0x16 rect origin x/y
 *   +0x18/+0x1a rect width/height
 *   +0x1e/+0x20 alternate rect origin (some descriptors only)
 *   +0x1c       sub-handler tag / trailer byte
 * Each block below sets only the subset a given descriptor actually uses. */
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
