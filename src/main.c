/*
 * main.c — Reconstructed game entry point.
 *
 * RECONSTRUCTION FIDELITY NOTE (CRT/startup deviation):
 *   The original BUMPY.EXE was built with Turbo C++ and uses ~60 CRT-garble
 *   startup functions (CRT0, __cstart, etc.) that are not decompilable and are
 *   NOT reproduced here.  This reconstruction uses the Open Watcom CRT with a
 *   standard `main`.  The MEANINGFUL original startup — everything in
 *   init_game_session_state (1000:0282) — is reproduced faithfully in game.c;
 *   the CRT scaffolding itself is intentionally replaced.
 *
 * Boot path (matches the original at the meaningful-init level):
 *   main()
 *     → init_game_session_state()  (game.c, 1000:0282) — subsystem init + ~50
 *                                    global resets + clear_viewport + VGA mode 0x0D
 *     → run_game_session()         (game.c, 1000:0258) — the session loop:
 *                                    current_level=1; init_view_anim_descriptors();
 *                                    sound_select_device(); do{ do{ game_loop(); }
 *                                    while(round_continue_flag); }while(session_continue_flag);
 *
 * Phase-1 Task 7: main is now wired to the REAL session in game.c (the T3 stubs
 * are gone).  game.c's session/loop spine is ported 1:1; its many not-yet-
 * reconstructed per-tick callees are faithful-signature stubs in game_stubs.c
 * (DEFERRED Phase 2).  Under the boot harness (tools/run_bumpy.py) there is no
 * DOS INT 21h file I/O, so once the loop reaches start_level / the per-tick
 * spine it cannot load level data — that is EXPECTED and DEFERRED (see the
 * fidelity audit's Phase-1 slice-status note).
 */

#include "bumpy.h"
#include "game.h"
#ifdef BUMPY_PLAYABLE
#include "host/host.h"   /* host_fb_init, host_timer_teardown, restore_keyboard_isr */
#include "screens.h"     /* palette_mode (DGROUP 0x541d) */
#include "config_screens.h" /* gfx_driver_init, sound_device_select_screen (engine screens) */
#endif

/* -------------------------------------------------------------------------
 * main — reconstructed entry point.
 *
 * RECONSTRUCTION FIDELITY: the original's `main` is unreachable CRT garble
 * (not decompilable). This Open Watcom main is the reconstructed equivalent: it
 * calls init_game_session_state (subsystem init) then run_game_session (the
 * session loop), matching the original's call graph above the CRT level.
 * ------------------------------------------------------------------------- */
#ifdef BUMPY_PLAYABLE
/* -------------------------------------------------------------------------
 * PLAYABLE-BUILD ENTRY (Plan A, Task 9 — INTEGRATION MILESTONE).
 *
 * Wires the host platform layer (Tasks 2-8) into the reconstructed boot/
 * session flow so BUMPYP.EXE boots through title→menu→level-intro into the
 * level-1 per-tick gameplay loop in BIOS video mode 0x0D, presenting frames.
 *
 * Entry sequence:
 *   palette_mode = 2     — force the validated EGA/VGA palette/blit path.
 *   host_fb_init()       — allocate the flat 4-plane RAM framebuffer + register
 *                          the VGA page table (host_render.c, Task 2).
 *   init_game_session_state()  (game.c 0282) — installs the host INT8 + INT9
 *                          handlers (install_interrupt_handler / install_keyboard_isr,
 *                          now the host bodies via host_timer.c / host_input.c) and
 *                          sets mode 0x0D, then resets the session globals.
 *   run_game_session()         (game.c 0258) — the session/round/tick loop.
 *   host_timer_teardown() + restore_keyboard_isr() — restore the saved INT8/INT9
 *                          vectors + the BIOS PIT divisor on exit (clean DOS state).
 *
 * ── RECONSTRUCTION FIDELITY: gfx_driver_init palette-select SCREEN skipped ──
 * The original BUMPY.EXE boots into gfx_driver_init's interactive F2/F5
 * graphics-mode/palette-SELECT screen (the user picks CGA/EGA/VGA before the
 * title appears), which writes palette_mode and only then drops into mode 0x0D.
 * The playable host SKIPS that selection screen entirely and hardcodes
 * palette_mode = 2 (the EGA/VGA path the validated blitters + the DAC upload
 * use), going straight to the gameplay graphics.  This is a deliberate boot
 * deviation: the F2/F5 selection UI is a hardware-probe/config front-end with no
 * gameplay effect once the VGA path is chosen, and reproducing its interactive
 * mode-flips is out of scope for the host bring-up.  The corresponding F2/F5
 * key pulses are therefore omitted from the DOSBox boot input script.
 * Recorded in docs/reconstruction-fidelity.md ("playable host: palette-select
 * screen skip").  NOTE: the default BUMPY.EXE main is NO LONGER byte-unchanged —
 * it gained the init_worldmap_data()/init_anim_data() calls (both builds need the
 * relocated tables: start_level reads the worldmap accessors and spawn/anim read
 * the anim tables unconditionally).  Nothing automated consumes the old image
 * md5; the "byte-unchanged"/md5 baselines recorded in docs are historical.
 * ------------------------------------------------------------------------- */
extern void init_move_scripts(void);   /* move_scripts.c — fill mode_script_tbl + reloc blob */
extern void init_worldmap_data(void);  /* worldmap_data.c — relocate overworld nav tables */
extern void init_anim_data(void);      /* anim_data.c — fill+relocate in-level anim/entity tables */
extern void level_preload_session_sprites(void);  /* level.c — session-init sprite-bank bring-up */
int main(void)
{
    gfx_driver_init();           /* config_screens.c — 1ab9:02ce: text mode 0x02, header +
                                    EGA/VGA menu, F2->palette_mode 1 / F3->2. */
    sound_device_select_screen();/* config_screens.c — 202c:0000: F5..F8 sound menu, sets
                                    sound_device_state. */
    host_fb_init();              /* host_render.c — allocate the 256 KB framebuffer FIRST,
                                    while the far heap is unfragmented (it needs one
                                    contiguous block; conventional memory leaves only
                                    ~8 KB of slack over the framebuffer + the program). */
    host_screens_buf_init();     /* host_resource.c — point fullscreen_buf at the
                                    framebuffer's unused per-plane slack (no extra halloc;
                                    each plane uses only ~16 KB of its 64 KB).  MUST follow
                                    host_fb_init so host_framebuffer is allocated. */
    host_view_descriptors_init();/* game.c — bind view-descriptor backing storage
                                    (the reconstruction's pointer-split layout needs
                                    this before init_view_anim_descriptors writes
                                    through the descriptor far pointers; see game.c) */
    init_move_scripts();         /* move_scripts.c — relocate the move-script blob's
                                    far pointers into the runtime DGROUP + fill the
                                    mode_script_tbl stub.  Without this, in-level movement
                                    reads a garbage move script and crashes (INT 6) — the
                                    long-standing bug #3.  Safe to wire now that the
                                    real-VGA host removed the flat-buffer DGROUP fragility. */
    init_worldmap_data();        /* worldmap_data.c — relocate the per-level overworld
                                    move-descriptor + anim-coord lookup tables (extracted
                                    verbatim) so start_level can point move_descriptor_table
                                    / anim_coord_table_ptr at real data (overworld nav). */
    init_anim_data();            /* anim_data.c — fill the zero-init in-level entity DGROUP
                                    tables (layer A/B/C spawn-type / anim-frame far-ptrs /
                                    grid / pos tables + the P2 frame table) so the platform
                                    bars, items, and Bumpy draw via spawn_and_draw_level_entities. */
    init_highscore_default_table();/* screens.c — fill g_highscore_default_table (DGROUP 0x8f0)
                                    with the 7 built-in high scores (loader-relocated static
                                    data in the original) so show_highscore_screen renders the
                                    defaults; see RECONSTRUCTION FIDELITY in screens.c. */
    init_password_table();       /* screens.c — fill password_table (DGROUP 0x135c) with the 8
                                    level passwords (ACCESS/BUTTON/…/SYSTEM) so the "ENTER YOUR
                                    PASSWORD" screen validates codes (else every code -> ERROR). */
    level_preload_session_sprites();/* level.c — load+transform the main sprite bank
                                    (BUMSPJEU.BIN) + wire the DG shadow / p1_sprite / host
                                    render context at SESSION INIT (as the engine does), so the
                                    title/menu/highscore/menu-select screens can blit sprite-
                                    glyph text.  Without it hr_dg/hr_bank stay NULL until a level
                                    loads and every menu-context glyph blit is a NOP (blank HOF /
                                    password screen).  start_level reuses these (==0-guarded). */
    host_load_cursor_bank();     /* host_resource.c — load+transform FLECHE.BIN (the menu
                                    cursor arrow, engine resource 9) so run_main_menu can
                                    blit it; the host resource path drains the engine's
                                    own sprite-bank reads, so load it explicitly here. */
    init_game_session_state();   /* game.c — 1000:0282 (installs host INT8/INT9) */
    run_game_session();          /* game.c — 1000:0258 (session/round/tick loop) */
    /* Teardown: restore the saved interrupt vectors + the BIOS PIT divisor so
       DOS is left in a clean state when the session loop returns. */
    host_timer_teardown();       /* host_timer.c — restore INT8 + BIOS PIT */
    restore_keyboard_isr();      /* host_input.c — restore INT9 */
    return 0;
}
#else
int main(void)
{
    extern void init_worldmap_data(void);  /* worldmap_data.c */
    extern void init_anim_data(void);      /* anim_data.c */
    init_worldmap_data();        /* relocate overworld nav tables (both builds use start_level) */
    init_anim_data();            /* fill+relocate in-level entity tables (platform bars/items/Bumpy) */
    init_game_session_state();   /* game.c — 1000:0282 */
    run_game_session();          /* game.c — 1000:0258 */
    return 0;
}
#endif /* BUMPY_PLAYABLE */
