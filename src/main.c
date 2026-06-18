/*
 * main.c — Reconstructed game entry point + one-time session init.
 *
 * RECONSTRUCTION FIDELITY NOTE (CRT/startup deviation):
 *   The original BUMPY.EXE was built with Turbo C++ and uses ~60 CRT-garble
 *   startup functions (CRT0, __cstart, etc.) that are not decompilable and are
 *   NOT reproduced here.  This reconstruction uses the Open Watcom CRT with a
 *   standard `main`.  The MEANINGFUL original startup — everything in
 *   init_game_session_state (1000:0282) — is reproduced faithfully; the CRT
 *   scaffolding itself is intentionally replaced.
 *
 *   Sources: local/build/slice_decomp.txt (init_game_session_state @ 0282,
 *   run_game_session @ 0258), local/build/slice_model.md §2.
 *
 * Boot path (matches the original at the meaningful-init level):
 *   main()
 *     → init_game_session_state()  — video/CRTC/keyboard/joystick/resource init,
 *                                    ~50 global resets, clear_viewport
 *     → run_game_session()         — STUBBED: returns immediately in this task
 *
 * run_game_session (1000:0258) in the original does:
 *   current_level = 1; init_view_anim_descriptors(); sound_select_device();
 *   then loops: game_loop() while round_continue_flag / session_continue_flag.
 * All of those sub-functions are NOT YET IMPLEMENTED; the stub just returns.
 *
 * init_game_session_state (1000:0282) in the original does:
 *   set_disk_swap_callback, FUN_7bad, install_interrupt_handler,
 *   install_keyboard_isr, init_joystick_handlers, mouse_reset,
 *   FUN_7563, FUN_7bd7, FUN_97a4, FUN_7bbd(2), FUN_97f1,
 *   FUN_9821(0,0,0x13f,199), FUN_9814(1), FUN_97c5(0xe,1),
 *   set_resource_table(0x90,0x203b),
 *   ~50 global zero/init assignments,
 *   clear_viewport().
 * Many of those sub-calls are audio/CRTC/hardware setup that cannot be
 * replicated without the full game data.  In this task's stub we perform
 * only the STRUCTURALLY MANDATORY part: set VGA mode 0x0D via video.c and
 * then return, which is the minimum the boot harness needs to assert.
 *
 * UNCERTAIN: FUN_1000_31de (1000:31de) — Ghidra decompile failed
 * (address-out-of-bounds).  Called from reset_game_state (1000:0bf9) after
 * spawn_and_draw_level_entities; likely resets per-round counters.  Stubbed
 * here as reset_game_state_counters_UNVALIDATED().  Not called from main
 * (reset_game_state is called from game_loop, not from init_game_session_state).
 */

#include "bumpy.h"
#include "video.h"

/* -------------------------------------------------------------------------
 * Forward declarations for stubs not yet implemented.
 * Each stub is annotated with the original's address + decompile status.
 * ------------------------------------------------------------------------- */

/* run_game_session (1000:0258) — STUBBED (Task 3).
 * Full body: current_level=1; init_view_anim_descriptors(); sound_select_device();
 * do { do { game_loop(); } while(round_continue_flag); } while(session_continue_flag); */
static void run_game_session_stub(void);

/* init_game_session_state (1000:0282) — STUBBED (Task 3).
 * Full body: video/CRTC/keyboard/joystick/resource init + ~50 global resets.
 * Here we perform only VGA mode 0x0D (the structurally mandatory minimum). */
static void init_game_session_state_stub(void);

/* -------------------------------------------------------------------------
 * Stub implementations
 * ------------------------------------------------------------------------- */

/*
 * init_game_session_state — one-time subsystem init.
 *
 * Original (1000:0282) calls (in order, from slice_decomp.txt):
 *   set_disk_swap_callback(0x698, 0x6a9)
 *   FUN_1000_7bad(0x6fac, 0x203b)
 *   install_interrupt_handler()
 *   install_keyboard_isr()
 *   init_joystick_handlers()
 *   mouse_reset()
 *   FUN_1000_7563(0x4c00, 0x4cd0, 0x203b)
 *   FUN_1000_7bd7()
 *   FUN_1000_97a4()
 *   FUN_1000_7bbd(CONCAT11(extraout_AH, 2))
 *   FUN_1000_97f1()
 *   FUN_1000_9821(0, 0, 0x13f, 199)
 *   FUN_1000_9814(1)
 *   FUN_1000_97c5(0xe, 1)
 *   set_resource_table(0x90, 0x203b)
 *   [~50 global zero/init assignments — see slice_decomp.txt lines 447-494]
 *   clear_viewport()
 *
 * STUB: only the VGA mode set is performed (the minimum the boot harness
 * asserts).  Keyboard ISR, joystick, resource table, and global resets are
 * deferred to later tasks.
 */
static void init_game_session_state_stub(void)
{
    /* Set VGA mode 0x0D: 320x200x16 EGA planar via INT 10h AX=0x000D.
     * This mirrors the CRTC init block inside FUN_1000_9821 in the original
     * (which calls set_vga_mode_0d as part of its display-controller setup). */
    video_set_mode_0d();

    /* STUB: the remaining ~50 global resets, keyboard ISR, joystick
     * calibration, resource table setup, and clear_viewport() are deferred
     * to later tasks when those modules are reconstructed. */
}

/*
 * run_game_session — outer session loop.
 *
 * Original (1000:0258):
 *   current_level = 1;
 *   init_view_anim_descriptors();
 *   sound_select_device();
 *   do {
 *     do { game_loop(); } while (round_continue_flag != 0);
 *   } while (session_continue_flag != 0);
 *
 * STUB (Task 3): returns immediately.  game_loop/input/player/level are
 * not yet implemented.
 */
static void run_game_session_stub(void)
{
    /* STUB: no gameplay yet — returns immediately so the boot harness can
     * assert mode set + clean exit. */
}

/* -------------------------------------------------------------------------
 * main — reconstructed entry point.
 *
 * RECONSTRUCTION FIDELITY: the original's `main` is unreachable CRT garble
 * (not decompilable). This open-Watcom main is the reconstructed equivalent.
 * It calls init_game_session_state (for subsystem init) and then
 * run_game_session (for the session loop), matching the original's call
 * graph above the CRT level.
 * ------------------------------------------------------------------------- */
int main(void)
{
    init_game_session_state_stub();
    run_game_session_stub();
    return 0;
}
