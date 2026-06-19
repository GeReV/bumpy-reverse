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

/* -------------------------------------------------------------------------
 * main — reconstructed entry point.
 *
 * RECONSTRUCTION FIDELITY: the original's `main` is unreachable CRT garble
 * (not decompilable). This Open Watcom main is the reconstructed equivalent: it
 * calls init_game_session_state (subsystem init) then run_game_session (the
 * session loop), matching the original's call graph above the CRT level.
 * ------------------------------------------------------------------------- */
int main(void)
{
    init_game_session_state();   /* game.c — 1000:0282 */
    run_game_session();          /* game.c — 1000:0258 */
    return 0;
}
