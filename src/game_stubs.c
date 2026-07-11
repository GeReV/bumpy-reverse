/*
 * game_stubs.c — GENUINE CARVE-OUTS (Phase 9, Task 4: integration cleanup)
 * ============================================================================
 * These are NOT reconstructions and NOT "deferred" reconstructions — every
 * remaining symbol here is a documented CARVE-OUT: a faithful-SIGNATURE stub for
 * an engine function the reconstructed call graph references but that is
 * deliberately NOT reconstructed in C, in the same spirit as the self-modifying
 * graphics-overlay blitters and the L5 timer ISR.  Each falls into one carve-out
 * class (see the per-symbol notes below):
 *
 *   - HARDWARE INIT  — the init_game_session_state (0282) CRTC/audio/resource/IRQ
 *     setup block (display-controller, timer/joystick/mouse/disk-swap install,
 *     resource-table + viewport clear, the opaque ~46-global session reset).
 *   - CRTC PAGE-FLIP — present_frame (the a000/a200 double-buffer page present).
 *   - int8-TIMING    — run_n_frames / rotate_timing_flags_and_wait / wait_keypress
 *     (frame-tick waits driven by the int8 timer the L5 ISR services).
 *   - ENGINE STANDALONE LOADER — load_current_level_data (0x32b0); this slice
 *     loads level data through level.c start_level instead.
 *   - NEVER-DECOMPILED — reset_round_counters (init_round_state 0x31de): has a
 *     canonical Ghidra label but Ghidra decompilation fails (UNCERTAIN body).
 *   - RENDER-CORE LEAVES — view/sprite/palette present leaves not reconstructed
 *     (init_sprite_structs, init_fullscreen_view_desc, setup_fullscreen_view,
 *     apply_level_palette, show_text_screen, show_pause_screen).
 *   - OUT-OF-SCOPE LEAVES — sound L4/L6 device drivers + player handler-table
 *     targets + the P2 indirect-call backend that the reconstructed bodies
 *     reference but that lie outside any validated slice (each cited below).
 *
 * Bodies are no-ops (void) or a benign default (0).  Where a return value steers
 * control flow the chosen default is noted.  These stubs make BUMPY.EXE LINK; the
 * carve-out boundary (which callees are genuine hardware/CRT/int8-timing leaves
 * vs reconstructed game logic) is enforced by tools/validate_integration.sh and
 * recorded in docs/reconstruction-fidelity.md row "game_stubs.c".
 *
 * NAMING: symbols still spelled FUN_1000_<off> are carve-outs whose Ghidra label
 * IS now canonical (e.g. FUN_1000_6183 = sweep_active_entities) but which are NOT
 * reconstructed here; the FUN_ spelling is kept deliberately to mark "unported
 * carve-out, link-only" and to avoid churning the validated call sites (sound.c,
 * player.c) that reference them.  The canonical Ghidra name is cited in the note.
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
 * row "game_stubs.c" for the full carve-out list.
 * ============================================================================
 */

#include "bumpy.h"

/* ── E. player.c forward-declared leaves — CARVE-OUT (out-of-scope handlers) ───
 *
 * player.c is LINKED into BUMPY.EXE.  Its move spine references a closed set of
 * handler-table targets / leaves that the reconstruction does not port (per the
 * documented STOP-AND-SPLIT rule — see player.h "BOUNDARY" + the ledger): these
 * are out-of-scope game-mode handlers, not reached on any validated path.  Each
 * is a faithful-signature carve-out stub so player.obj resolves; each cites its
 * engine address (canonical Ghidra label noted where it now has one).
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
 *  the deeper L4/L6 device-driver callees they reach are carve-out stubs below (link-only). */
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
void mpu401_reset_to_uart(void)  {}   /* 1000:8a75 mpu401_reset_to_uart — L4 MPU reset (carve) */
void FUN_1000_8b2a(void)         {}   /* 1000:8b2a snddrv_init_substep — snd-driver init (carve) */
void FUN_1000_91cf(void)         {}   /* 1000:91cf snddrv_dispatch_b_mode0 — driver backend (carve) */
void FUN_1000_8af6(void)         {}   /* 1000:8af6 snddrv_dispatch_b_mode4 — driver backend (carve) */
void FUN_1000_8e48(void)         {}   /* 1000:8e48 snddrv_dispatch_b_mode1 — driver backend (carve) */
void FUN_1000_91d7(void)         {}   /* 1000:91d7 snddrv_dispatch_c_mode0 — driver backend (carve) */
void FUN_1000_8b04(void)         {}   /* 1000:8b04 snddrv_dispatch_c_mode4 — driver backend (carve) */
void FUN_1000_8e50(void)         {}   /* 1000:8e50 snddrv_dispatch_c_mode1 — driver backend (carve) */
void FUN_1000_91df(void)         {}   /* 1000:91df snddrv_dispatch_d backend (carve) */
void FUN_1000_8b0d(void)         {}   /* 1000:8b0d snddrv_dispatch_d backend (carve) */
void FUN_1000_8e58(void)         {}   /* 1000:8e58 snddrv_dispatch_d backend (carve) */
void FUN_1000_7fef(void)         {}   /* 1000:7fef timer_teardown_restore — int8 timer teardown (carve) */
void FUN_1000_6183(void)         {}   /* 1000:6183 sweep_active_entities — out-of-scope entity sweep (carve) */
/* apply_contact_action (1000:6a89) — RECONSTRUCTED in player.c (Phase-9 T1); the
   no-op stub is removed (it would now be a duplicate symbol against player.obj). */
/* play_walk_anim_default (1000:4361) — RECONSTRUCTED in player.c (audit 2026-06-28); stub removed. */
/* p1_set_pixel_from_cell (1000:4906) — RECONSTRUCTED in player.c (audit 2026-06-28);
   the no-op stub is removed (would be a duplicate symbol against player.obj). */
/* step_walk_anim (1000:495c) — RECONSTRUCTED in player.c (audit 2026-06-28); stub removed. */
/* ── game-mode handler-table targets — RECONSTRUCTED from Ghidra (no longer carve-outs).
   FUN_ names kept to avoid churning the game_mode_handlers[] call sites (NAMING rule). */
extern void play_sound(u8 sound_id);
extern void apply_cell_animation(u8 action_code);
extern void enter_game_mode(u8 mode);
extern void dispatch_move_step(void);
extern void compute_move_descriptor_ptr(void);
extern void p1_input_dispatch_bit01(void);
extern void p1_settle_idle(void);
extern u8   anim_target_cell;
extern u8   p1_cell;
extern u8   round_continue_flag;
extern u8   input_state;
extern u16  sound_device_state;
extern u16  move_descriptor_ptr;
extern u16  move_descriptor_ptr_seg;
u8 dgroup_flag_a1a9;     /* DGROUP 0xa1a9 — round-activate flag set by idx30 handler */

/* FUN_1000_4802 = move_step_teleport_exit (1000:4802): exit/teleport trigger. */
void FUN_1000_4802(void)
{
    u8 sound_id = (sound_device_state == 4u) ? 0x28u : 3u;
    play_sound(sound_id);
    anim_target_cell = p1_cell;
    apply_cell_animation(0x27u);
    enter_game_mode(0x0eu);
    dispatch_move_step();
}

/* (3) Out-of-scope handler-table targets (modes outside the §4.2 slice set):
 *     bounce / jump / teleport / fall-step / physics-freeze / die / pvp modes.
 *     Referenced by game_mode_handlers[] but not reached on the level-1 path. */
/* move_walk_right_anim_step (1000:2423) — RECONSTRUCTED in player.c (audit 2026-06-28); stub removed. */
/* enter_mode_0b_jump_start (1000:2470) — RECONSTRUCTED in player.c (audit 2026-06-28); stub removed. */
/* move_anim_step_to_mode0c (1000:248e) — RECONSTRUCTED in player.c (audit 2026-06-28); stub removed. */
/* move_step_check_walkable (1000:24d7) — RECONSTRUCTED in player.c (audit 2026-06-28); stub removed. */
/* move_step_dispatch_input (1000:250a) — RECONSTRUCTED in player.c (audit 2026-06-28); stub removed. */
/* teleport_to_next_exit_tile (1000:25ad, idx 0x0e) is RECONSTRUCTED in items.c
   (Phase-3 T4) — no longer stubbed here (would be a duplicate symbol). */
/* FUN_1000_22b0 (idx 0x10/0x2c) + run_physics_settle_wrap (1000:22c1, idx 0x2d)
   are now RECONSTRUCTED in player.c (Phase 2, Task 4). */
/* p1_input_dispatch_bit10 (1000:4344) — RECONSTRUCTED in player.c (audit 2026-06-28); stub removed. */
/* FUN_1000_4437 = game_mode_handler_idx1d (1000:4437, idx 0x1d..0x20): input router —
   fire (0x10) settles to idle, else dispatch the direction bits.
   (Binary: 1000:4443 TEST [0x8244],0x10; JZ 444f; CALL 440c; 444f CALL 4398.)
   NOTE 2026-07-02: this body and FUN_1000_1e3d's were SWAPPED in the original
   reconstruction (each carried the other address's code); re-verified against the
   disasm and the dispatch table at DGROUP 0x7ca (2-byte near offsets) and corrected. */
void FUN_1000_4437(void)
{
    if ((input_state & 0x10u) == 0u) {
        p1_input_dispatch_bit01();
    } else {
        p1_settle_idle();
    }
}
/* advance_physics_freeze (1000:22d2) — RECONSTRUCTED in player.c (audit 2026-06-28); stub removed. */
/* FUN_1000_1e3d = game_mode_handler_idx30 (1000:1e3d, idx 0x30): activate current
   move-descriptor entry (set round-continue + a1a9 flags, mark the entry done).
   (Binary: 1000:1e49 MOV AL,1; MOV [0x9d30],AL; MOV [0xa1a9],AL; CALL 3a88;
   LES BX,[0x9baa]; MOV ES:[BX],0x1.)  See the swap note above. */
void FUN_1000_1e3d(void)
{
    round_continue_flag = 1u;
    dgroup_flag_a1a9 = 1u;
    compute_move_descriptor_ptr();
    *(u8 __far *)MK_FP(move_descriptor_ptr_seg, move_descriptor_ptr) = 1u;
}

/* (4) mode_script_tbl (DGROUP 0x2252): far-ptr-per-game_mode to its [anim,dx,dy]
 *     move script.  Runtime-populated in the engine (not statically resolvable —
 *     confirmed in Task 6a); a zeroed blob of the right size satisfies the link.
 *     Sized 0x40 modes × 4 bytes/entry. */
u8 mode_script_tbl[0x40 * 4] = { 0 };

/* (5) dos_abort — input.c declares `extern int dos_abort(void)` for the faithful
 *     control flow of its stack-check / abort path (the engine calls the CRT
 *     abort thunk).  In the linked build it is never reached on the boot path. */
int dos_abort(void)                         { return 0; }


/* ── A. init_game_session_state (1000:0282) setup callees — HARDWARE-INIT CARVE-OUT
 *  These are the engine's one-time CRTC / audio / resource-table / IRQ install
 *  block: real-mode port writes + DOS interrupt installs that cannot run without
 *  the full game data + real DOS, in the same carve-out class as the graphics-overlay
 *  blitters and the L5 timer ISR.  Faithful-signature no-op stubs for the link.
 *  The descriptive C names (init_crtc_window etc.) are the reconstruction's own;
 *  the canonical Ghidra label is cited per stub.  install_keyboard_isr is the one
 *  REAL reconstructed call in this block (input.c).
 *
 *  BUMPY_PLAYABLE: all hardware-init symbols below (through init_misc_7bbd) are
 *  owned by src/host/host_boot.c and src/host/host_video.c in the playable build.
 *  Guard them out so both game_stubs.obj + host_*.obj can link without duplicates. */

#ifndef BUMPY_PLAYABLE
/* set_disk_swap_callback 1000:72ef — install INT24 + disk-swap prompt callback. */
void set_disk_swap_callback(u16 int24_handler, u16 callback)
{
    (void)int24_handler; (void)callback;
}

/* 1000:7bad gfx_overlay_thunk_adab — timer/resource-table init (off,seg). */
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

/* 1000:7563 init_sound_tables — sound table init (off,off,seg). */
void init_sound_tables(u16 a, u16 b, u16 seg)
{
    (void)a; (void)b; (void)seg;
}
#endif /* !BUMPY_PLAYABLE */

/* 1000:7bd7 gfx_overlay_thunk_gfx_init — misc subsystem init. */
void init_misc_7bd7(void)
{
}

#ifndef BUMPY_PLAYABLE
/* 1000:97a4 — near-thunk to detect_video_adapter (202c:0000), an adapter probe
   (stores adapter/mode code at 0x26c4c).  Default build: no-op (no live BIOS). */
void init_display_97a4(void)
{
}
#endif /* !BUMPY_PLAYABLE */

/* 1000:7bbd gfx_overlay_thunk_0232 — misc init (one byte arg). */
void init_misc_7bbd(u8 mode)
{
    (void)mode;
}

#ifndef BUMPY_PLAYABLE
/* 1000:97f1 init_display_controller_b — display controller init. */
void init_display_97f1(void)
{
}

/* 1000:9821 set_crtc_window — CRTC window setup (x0,y0,x1,y1). */
void init_crtc_window(u16 x0, u16 y0, u16 x1, u16 y1)
{
    (void)x0; (void)y0; (void)x1; (void)y1;
}

/* 1000:9814 set_active_display_page — set active display page. */
void set_display_page(u8 page)
{
    (void)page;
}

/* 1000:97c5 — graphics-overlay set-text-colour thunk → 1ab9:1311 → pm-2 handler 1ab9:14ef
   (fg/bg 4-plane expansions at DGROUP 0x68a6/0x68ae).  Formerly misnomered
   set_palette_display_mode/set_palette_mode — it never touches palette_mode. */
void set_text_color(u8 fg, u8 bg)
{
    (void)fg; (void)bg;
}

/* set_resource_table 1000:... — point the resource table at (off,seg). */
#ifndef BUMPY_PLAYABLE
void set_resource_table(u16 off, u16 seg)
{
    (void)off; (void)seg;
}
#endif /* !BUMPY_PLAYABLE (playable: src/host/host_resource.c) */

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
#endif /* !BUMPY_PLAYABLE */


/* ── B. run_game_session (1000:0258) one-time setup ─────────────────────────── */

/* init_view_anim_descriptors 1000:535e — RECONSTRUCTED in game.c (Phase-9 T3): the
   one-time p1/p2 view + anim-channel descriptor struct-init.  Stub removed (dup-symbol
   once game.obj's body links). */

/* sound_select_device 1000:6de3 — RECONSTRUCTED in sound.c (Phase-6 T4); stub removed
 *  (dup-symbol once sound.obj's T4 bodies link). */


/* ── C. reset_game_state (1000:0bf9) callees ────────────────────────────────── */

/* load_current_level_data 1000:32b0 — CARVE-OUT (engine standalone loader): copies
   the current level's 0x96-byte header into the tilemap buffer.  The reconstruction
   loads level data through level.c start_level instead, so this standalone loader is
   not reconstructed; faithful no-op stub for the link.
   BUMPY_PLAYABLE: owned by src/host/host_boot.c in the playable build. */
#ifndef BUMPY_PLAYABLE
void load_current_level_data(void)
{
}
#endif /* !BUMPY_PLAYABLE */

/* spawn_and_draw_level_entities 1000:2a78 — RECONSTRUCTED 1:1 in src/spawn.c
   (Phase-8 T2: the channel-A/B record populator + layer-C static-sprite blitter +
   P1/P2 BUM-header spawn reader).  No longer stubbed here — spawn.obj owns the body
   (no dup; validated by tools/validate_spawn.sh). */

/* 1000:31de init_round_state (recon name reset_round_counters) — RECONSTRUCTED in
   player.c from the disassembly (decompiler still fails on it, but the asm is complete
   and every store maps to a named global).  It was previously a no-op stub; that DROPPED
   its p1_set_pixel_from_cell/p2_set_pixel_from_cell calls (player spawned at the stale
   world-map pixel) and its move-step counter resets (movement state garbage).  Owned by
   player.obj now (most of its written globals are player.c-local). */


/* ── D. game_loop (1000:0c18) per-tick spine callees ────────────────────────── */

/* init_sprite_structs — CARVE-OUT (render-core leaf): one-time per-game sprite-struct
   setup, not reconstructed; no-op stub for the link.
   BUMPY_PLAYABLE: owned by src/host/host_view.c in the playable build. */
#ifndef BUMPY_PLAYABLE
void init_sprite_structs(void)   {}
#endif /* !BUMPY_PLAYABLE */

/* init_title_graphics / run_main_menu / show_menu_select_screen / show_title_and_init /
   play_iris_wipe_transition — RECONSTRUCTED 1:1 in screens.c (Phase-7 T4); their stubs
   are removed here (would be duplicate symbols once screens.obj links). */

/* 1000:75a2 read_input_action — the engine's input-poll primitive (returns the action
   byte in AL).  CARVE-OUT: faithful-signature stub for the BUMPY.EXE link.  The
   screens.c menu / state-machine loops call it through this symbol.
   BUMPY_PLAYABLE: owned by src/host/host_input.c in the playable build. */
#ifndef BUMPY_PLAYABLE
char fun_75a2_poll_action(u8 arg)    { (void)arg; return 0; }
#endif /* !BUMPY_PLAYABLE */

/* show_highscore_screen (1000:5681), show_level_intro_screen (1000:0d9d),
   level_intro_screen (1000:3852) — RECONSTRUCTED 1:1 in screens.c (Phase-7 T5); their
   stubs are removed here (would be duplicate symbols once screens.obj links). */
/* show_text_screen / show_pause_screen — CARVE-OUT (render-core leaves): the
   text-screen + pause-screen present paths, not reconstructed; no-op stubs.
   BUMPY_PLAYABLE: owned by src/host/host_view.c in the playable build. */
#ifndef BUMPY_PLAYABLE
void show_text_screen(void)          {}
void show_pause_screen(void)         {}
#endif /* !BUMPY_PLAYABLE */

/* p2_set_move_state (1000:4bc6) — RECONSTRUCTED in player2.c (Phase-4 T3); stub
   removed (would be a duplicate symbol once player2.obj links). */

/* init_fullscreen_view_desc — CARVE-OUT (render-core leaf): set up the fullscreen
   view descriptor (mode,flag); not reconstructed, no-op stub.
   BUMPY_PLAYABLE: owned by src/host/host_view.c in the playable build. */
#ifndef BUMPY_PLAYABLE
void init_fullscreen_view_desc(u8 mode, u8 flag) { (void)mode; (void)flag; }
#endif /* !BUMPY_PLAYABLE */

/* setup_fullscreen_view 1000:483c — CARVE-OUT (render-core leaf): the per-load
   fullscreen view/page restore the spawn orchestrator runs once before the grid scan
   (the fullscreen_buf -> page copy gfx_overlay.c models for sub-handler 0).  Not
   reconstructed; faithful no-op stub for linkability (called by spawn.c).
   BUMPY_PLAYABLE: owned by src/host/host_view.c in the playable build. */
#ifndef BUMPY_PLAYABLE
void setup_fullscreen_view(void) {}
#endif /* !BUMPY_PLAYABLE */

/* P1/P2 sprite draw — the zero-arg game-loop ENTRIES are now RECONSTRUCTED:
   draw_p1_sprite (1000:1cb2) in player.c (Phase-9 T3), draw_p2_sprite (1000:1cea) in
   player2.c (Phase-4 T5).  Their stubs are removed (dup-symbol once those objs link).
   The explicit-arg RENDER helpers (entity_draw_p1/p2, entity.c) are distinct symbols. */

/* apply_level_palette — CARVE-OUT (render-core leaf): program the level's VGA
   palette (DAC port writes); not reconstructed, no-op stub.
   BUMPY_PLAYABLE: owned by src/host/host_video.c in the playable build. */
#ifndef BUMPY_PLAYABLE
void apply_level_palette(void)       {}
#endif /* !BUMPY_PLAYABLE */

/* present_frame — CARVE-OUT (CRTC page-flip): flips the a000/a200 VGA double-buffer
   to the displayed page (CRTC start-address port write).  Hardware leaf, no-op stub.
   BUMPY_PLAYABLE: owned by src/host/host_video.c in the playable build. */
#ifndef BUMPY_PLAYABLE
void present_frame(u8 page)          { (void)page; }
#endif /* !BUMPY_PLAYABLE */

/* run_n_frames — CARVE-OUT (int8-timing): block for N frame ticks driven by the int8
   timer the L5 ISR services.  Timing leaf, no-op stub.
   BUMPY_PLAYABLE: owned by src/host/host_timer.c in the playable build. */
#ifndef BUMPY_PLAYABLE
void run_n_frames(u8 n)              { (void)n; }
#endif /* !BUMPY_PLAYABLE */

/* wait_keypress — CARVE-OUT (int8-timing): spin until a key tick; timing leaf.
   BUMPY_PLAYABLE: owned by src/host/host_input.c in the playable build. */
#ifndef BUMPY_PLAYABLE
void wait_keypress(void)             {}
#endif /* !BUMPY_PLAYABLE */

/* Grid-cell + grid-history updates.  The P2 entries (p2_update_grid_cell 1000:4b4e,
   p2_advance_grid_history 1000:13b2) are RECONSTRUCTED in player2.c (Phase-4 T3); the
   P1 entries (p1_update_grid_cell 1000:1473, p1_advance_grid_history 1000:138c) in
   player.c (Phase-9 T3).  All four stubs are removed (dup-symbol once those objs link). */

/* p2_step_scripted_move (1000:4c14) — RECONSTRUCTED in player2.c (Phase-4 T3). */

/* Anim-channel STEP (advance) — RECONSTRUCTED in anim.c (Phase-5 T3:
   step_anim_channels_a 1000:14e4, step_anim_channels_b 1000:15a1); their stubs are
   removed (dup-symbol once anim.obj links). */

/* Player-view erase/restore/render (the engine's per-tick view present chain).
   The P2 entries (erase_p2_view 1000:19a1, render_p2_view 1000:1c41) are RECONSTRUCTED
   in player2.c (Phase-4 T5); the P1 entries (erase_p1_view 1000:19e4,
   render_p1_view 1000:1bd7) + the shared deferred-restore helper
   (restore_bg_pending 1000:1a20) in player.c (Phase-9 T3).  Their stubs are removed
   (dup-symbol once those objs link). */

/* Anim-channel DRAW + ERASE (per-tick) — RECONSTRUCTED in anim.c (Phase-5 T4:
   draw_anim_channels_a 1000:165e, draw_anim_channels_b 1000:17c7,
   erase_anim_channels_a 1000:1a67, erase_anim_channels_b 1000:1b2b); their stubs
   are removed (dup-symbol once anim.obj's bodies link).  Their graphics-overlay present
   leaves stay faithful-signature stubs INSIDE anim.c (anim_*_leaf). */

/* P1/P2 bounding-box update.  update_p2_bbox (1000:50c0) + update_p1_bbox (1000:5085)
   are RECONSTRUCTED in player2.c (Phase-4 T5 / Phase-9 T3 — both write the pvp bbox
   words player2.c owns); their stubs are removed (dup-symbol once player2.obj links). */

/* rotate_timing_flags_and_wait — CARVE-OUT (int8-timing): rotate the timing flags and
   block for the frame tick (the per-tick int8 wait at the bottom of game_loop).
   BUMPY_PLAYABLE: owned by src/host/host_timer.c in the playable build. */
#ifndef BUMPY_PLAYABLE
void rotate_timing_flags_and_wait(void) {}
#endif /* !BUMPY_PLAYABLE */

/* game_post_present (1000:629c) / game_post_input (1000:233a) — RECONSTRUCTED in
   game.c (Phase-9 T3): the per-tick deferred-contact queue step + the level-complete
   exit-anim tick.  Their stubs are removed (dup-symbol once game.obj's bodies link). */

/* handle_gameplay_input 1000:1d26 — RECONSTRUCTED in player.c (Phase 9 T2): the
   player-spine input dispatch (F1..F7 debug keys + p1_read_tile_under / poll_input /
   p1_movement_dispatch / dispatch_move_step / begin_physics_freeze).  Stub removed. */

/* P2 tile move check (1000:4c99) — RECONSTRUCTED in player2.c (Phase-4 T3).
   P1↔P2 collision (check_pvp_collision 1000:50fb) — RECONSTRUCTED in player2.c
   (Phase-4 T5); its stub is removed (dup-symbol once player2.obj links). */

/* ── Callee of p2_tile_move_check — the DGROUP 0x870 move-state dispatch ───────
 * Phase-4 T4 RECONSTRUCTED the P2 AI decision layer in player2.c, including
 * p2_run_move_state_handler (1000:5003) and p2_ai_select_move_random (1000:4fd3) —
 * their stubs are REMOVED here (now real symbols in player2.obj).
 *
 * p2_dispatch_move_state_handler models the engine's indirect NEAR call at
 * 1000:4db9 in p2_tile_move_check:
 *     MOV AL,[0x8562] ; MOV AH,0 ; SHL AX,1 ; MOV BX,AX ;
 *     CALL word ptr [BX + 0x870]
 * GROUNDED (2026-07-03): the DGROUP 0x870 table is STATICALLY INITIALIZED in the
 * unpacked image (file 0x11440+0x870) — 16-bit NEAR code pointers into seg 1000;
 * a full opcode scan of the code image finds NO writer (the only references to
 * 0x85c/0x870 are the two `CALL word ptr [BX+disp]` sites at 1000:5021/1000:4db9):
 *     [0]  0x7111  (compiled empty C fn — no-op)
 *     [1]  0x4dbf  p2_pick_move_priority_a
 *     [2]  0x4e44  p2_pick_move_priority_b
 *     [3]  0x4ec9  p2_pick_move_priority_c
 *     [4]  0x4f4e  p2_ai_dispatch_move
 *     [5..9] 0x4dbf  p2_pick_move_priority_a  (the random states 5..9)
 *     [10] 0x7111  (no-op)
 * All four distinct targets are RECONSTRUCTED in player2.c (Phase-4 T4). */
#ifndef BUMPY_PLAYABLE
/* Default (byte-faithful) build: kept as the historical link-only carve-out stub
 * so BUMPY.EXE and the differential ctest harnesses (which shim this symbol
 * themselves) are unchanged.  RECONSTRUCTION FIDELITY: faithful-signature no-op
 * stub; not reached on the harness's captured P2 paths.  CARVE-OUT (P2
 * indirect-call backend). */
void p2_dispatch_move_state_handler(void) {}  /* DGROUP 0x870[move_state] */
#else  /* BUMPY_PLAYABLE */
/* Playable build: the REAL dispatch.  Without it nothing re-arms the P2 state
 * script when p2_move_steps_left hits 0, so p2_step_scripted_move walks past the
 * script end and p2_pixel accumulates garbage (wild blits on the P2 levels).
 *
 * RECONSTRUCTION FIDELITY (indirect handler dispatch): the engine's table holds
 * 16-bit NEAR code pointers in DGROUP (statically initialized, dumped above);
 * the recon mirrors it as a static C function-pointer table with the SAME
 * per-index targets, and the [0]/[10] empty-fn filler (engine 1000:7111) as a
 * local no-op.  The index/deref is 1:1 with the asm (no bounds check — the
 * engine has none; p2_set_move_state only ever produces states 1..9).
 * Recorded in docs/reconstruction-fidelity.md. */
extern u8 p2_move_state;                       /* game.c DGROUP 0x8562 */
extern void p2_pick_move_priority_a(void);     /* player2.c 1000:4dbf */
extern void p2_pick_move_priority_b(void);     /* player2.c 1000:4e44 */
extern void p2_pick_move_priority_c(void);     /* player2.c 1000:4ec9 */
extern void p2_ai_dispatch_move(void);         /* player2.c 1000:4f4e */

static void gs_p2_move_noop(void) {}           /* engine 1000:7111 (empty fn) */

/* Mirror of the static engine table at DGROUP 0x870 (file 0x11440+0x870). */
static void (* const gs_p2_move_handler_tbl[11])(void) = {
    gs_p2_move_noop,            /* [0]  0x7111 */
    p2_pick_move_priority_a,    /* [1]  0x4dbf */
    p2_pick_move_priority_b,    /* [2]  0x4e44 */
    p2_pick_move_priority_c,    /* [3]  0x4ec9 */
    p2_ai_dispatch_move,        /* [4]  0x4f4e */
    p2_pick_move_priority_a,    /* [5]  0x4dbf */
    p2_pick_move_priority_a,    /* [6]  0x4dbf */
    p2_pick_move_priority_a,    /* [7]  0x4dbf */
    p2_pick_move_priority_a,    /* [8]  0x4dbf */
    p2_pick_move_priority_a,    /* [9]  0x4dbf */
    gs_p2_move_noop             /* [10] 0x7111 */
};

void p2_dispatch_move_state_handler(void)      /* (*0x870[p2_move_state])() */
{
    (*gs_p2_move_handler_tbl[p2_move_state])();
}
#endif /* BUMPY_PLAYABLE */

/* all_entries_flag_set (1000:3e8a) — level-complete predicate.  RECONSTRUCTED in
   level.c (Phase-9 T3): walks the per-level move-descriptor table ANDing each
   record's flag.  Its stub is removed (dup-symbol once level.obj's body links). */
