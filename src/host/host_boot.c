/* host_boot.c — one-time boot init + per-round level (re)load (BUMPY_PLAYABLE).
 *
 * Implements the eight boot-init leaves init_game_session_state calls (game.c
 * 147-175) and the per-round level reload load_current_level_data that
 * reset_game_state calls (game.c 226).
 *
 * The DEFAULT (non-playable) build links game_stubs.c stubs for all of these;
 * the BUMPY_PLAYABLE guard prevents the stub body from conflicting.
 *
 * ── RECONSTRUCTION FIDELITY ──────────────────────────────────────────────────
 *
 *  init_timer_resource_table (1000:7bad = gfx_overlay_thunk_adab):
 *    Engine body: calls gfx_set_current_pos() which stores register values
 *    AX/DX into gfx_cur_pos_x/gfx_cur_pos_y.  No state the playable host
 *    uses.  BENIGN NO-OP: graphics overlay not installed in the host build.
 *
 *  init_joystick_handlers (1000:7532):
 *    Engine body: zeros the 16-entry far-pointer handler table
 *    g_joystick_handler_table[16] (32 words), then calls calibrate_joystick()
 *    twice for the two joystick ports.  HOST DEVIATION: we zero
 *    g_joystick_handler_table (same semantic — the interpreter loop in input.c
 *    checks these before dispatching) but skip calibrate_joystick (keyboard-
 *    only Tier 1 — no joystick hardware present in the host).
 *    Deviation documented in docs/reconstruction-fidelity.md row "host_boot.c".
 *
 *  mouse_reset (no canonical Ghidra decomp; INT 33h reset):
 *    BENIGN NO-OP: keyboard-only Tier 1.  INT 33h not invoked.
 *    Deviation: documented in docs/reconstruction-fidelity.md.
 *
 *  init_sound_tables (1000:7563):
 *    Engine body: calls set_joystick_handler_slot() with register-passed args
 *    (stores a far function pointer into g_joystick_handler_table at index AX).
 *    The "sound table" the name implies is actually a joystick-slot install for
 *    the sound driver, unrelated to sound playback itself — this leaf stays a
 *    benign no-op in the playable host regardless of the audio subsystem's own
 *    state.  (UPDATED 2026-07-14: the audio subsystem — src/sound.c, src/midi.c —
 *    was completed and merged 2026-07-13 and IS driven for real via
 *    host_timer.c's INT8 ISR; the playable build has audio.  This leaf's no-op
 *    is unaffected either way — it was never on the audio path.)
 *    Deviation: documented in docs/reconstruction-fidelity.md row "host_boot.c".
 *
 *  set_disk_swap_callback (1000:72ef):
 *    Engine body: stores the callback far ptr + installs a DOS INT 24h critical-
 *    error handler.  For a single-mount Tier-1 run (all game files in one
 *    directory) no disk-swap prompt is needed.  BENIGN NO-OP: INT 24h not
 *    installed.  Deviation documented.
 *
 *  set_resource_table (1000:7307):
 *    Engine body: stores the far ptr to the resource-descriptor table (a 10-byte-
 *    per-entry array of filenames + drive letters) into resource_table_ptr.
 *    level.c's start_level bypasses the resource table and builds filenames
 *    directly (see level.c RECONSTRUCTION FIDELITY note #3).  BENIGN NO-OP for
 *    the playable host: the resource table pointer is not used on the level-load
 *    path.  Deviation documented.
 *
 *  reset_opaque_session_globals:
 *    Documented no-op (game_stubs.c records the ~46 DGROUP resets; they are
 *    opaque audio-mixer / level-score bytes with no named C equivalents).
 *    Unchanged behavior under BUMPY_PLAYABLE.
 *
 *  load_current_level_data (1000:32b0):
 *    Engine body: copies the current level's 0x96-byte header from an in-memory
 *    level archive (cur_level_ptr / level_src_ptr far pointers) into the tilemap
 *    buffer at DGROUP 0xa0e4.  In the playable host the level archive is never
 *    loaded into memory (start_level decodes files directly into static buffers
 *    via INT 21h).  HOST IMPLEMENTATION: call start_level(current_level,
 *    current_level) to reload the current level's .PAV/.DEC/.BUM via the real
 *    INT 21h file path.  This reloads the buffers g_dec_buf / g_bum_buf /
 *    g_pav_buf (and re-renders) which is the functional equivalent of the engine's
 *    tilemap reload for the per-round reset path.
 *    DEVIATION: start_level does more than the engine's 32b0 (it also re-renders
 *    and reloads the bank); see DONE_WITH_CONCERNS note in the task-8 report.
 *    Deviation documented in docs/reconstruction-fidelity.md row "host_boot.c".
 * ─────────────────────────────────────────────────────────────────────────── */
#ifdef BUMPY_PLAYABLE
#include "host.h"
#include "../level.h"    /* start_level, current_level */
#include "../input.h"    /* g_joystick_handler_table   */
#include <string.h>      /* memset */

/* init_timer_resource_table 1000:7bad (gfx_overlay_thunk_adab) — graphics-overlay cur-pos
 * store.  BENIGN NO-OP: graphics overlay not installed in the host build.
 *
 * NOTE (name↔body uncertainty): the function name implies timer + resource-table
 * init, but the Ghidra body at 1000:7bad is a graphics-overlay thunk.  The no-op is
 * correct for the host (graphics overlay absent), but the name↔body mapping is
 * uncertain — this may be a mislabel.  Do NOT treat the no-op as cementing a
 * verified name↔body match. */
void init_timer_resource_table(u16 off, u16 seg)
{
    (void)off;
    (void)seg;
    /* Engine: gfx_set_current_pos() stores AX/DX register values into
     * gfx_cur_pos_x/gfx_cur_pos_y.  Not relevant to the host path. */
}

/* init_joystick_handlers 1000:7532 — zero the joystick handler table, then (HOST)
 * install the keyboard handler-script for handler 0.
 *
 * Engine: zeros g_joystick_handler_table[16] (32 words) then calls
 * calibrate_joystick() twice; the keyboard handler SCRIPT for slot 0 is then
 * populated by a runtime path (decoded from the resolved int8 capture — see
 * docs/dosbox-int8-capture.md "scancode->input_state map" and src/input.c's
 * read_input_action note: the script DATA's runtime populator was a Task-1 OPEN
 * ITEM).  Without slot 0 populated, read_input_action(0) hits its null-script
 * guard and calls dos_abort() on the FIRST poll_input — which is exactly what
 * hung the Task-9 boot (run_main_menu's first poll aborted into runaway code,
 * level/game_mode never advanced).
 *
 * HOST IMPLEMENTATION: bind slot 0 to the faithful keyboard handler script
 * (s_kbd_handler_script below).  Joystick calibration is skipped (keyboard-only
 * Tier 1) so only slot 0 is populated.
 *
 * RECONSTRUCTION FIDELITY (data, not logic): read_input_action (the INTERPRETER)
 * is reconstructed 1:1.  The script DATA encodes the engine's decoded
 * scancode->input_state map (the captured g_joystick_handler_table[0] content,
 * decoded in the int8 bring-up):
 *     0x01 = UP    (scancode 0x48)
 *     0x02 = DOWN  (scancode 0x50)
 *     0x04 = LEFT  (scancode 0x4b)
 *     0x08 = RIGHT (scancode 0x4d)
 *     0x10 = FIRE  (scancode 0x1c Enter, 0x39 Space, 0x74)
 * in read_input_action's opcode format: a leading 0xFD opens phase 2 (skips the
 * joystick-accumulate loop), then each group is <out-value> <scancode...>,
 * groups separated by 0xFD, the final group's scancode list ended by 0xFE.
 * Recorded in docs/reconstruction-fidelity.md ("playable host: keyboard handler
 * script").  This is #ifdef BUMPY_PLAYABLE host code; default build unchanged. */
static u8 s_kbd_handler_script[] = {
    0xFD,                          /* phase-1 delimiter: open the keyboard-group scan */
    0x01, 0x48,                    /* UP    <- up arrow                                */
    0xFD,
    0x02, 0x50,                    /* DOWN  <- down arrow                              */
    0xFD,
    0x04, 0x4B,                    /* LEFT  <- left arrow                              */
    0xFD,
    0x08, 0x4D,                    /* RIGHT <- right arrow                             */
    0xFD,
    0x10, 0x1C, 0x39, 0x74,        /* FIRE  <- Enter / Space / 0x74                    */
    0xFE,                          /* end final group -> terminate phase 2            */
};

void init_joystick_handlers(void)
{
    /* Zero all 16 far-pointer handler slots (matches engine's 32-word loop). */
    memset(g_joystick_handler_table, 0, sizeof(g_joystick_handler_table));
    /* HOST: bind slot 0 to the faithful keyboard handler script so
       read_input_action(0) resolves the menu/gameplay keys instead of aborting. */
    g_joystick_handler_table[0] = (u8 __far *)s_kbd_handler_script;
    /* calibrate_joystick() x2 skipped — keyboard-only host, no joystick HW. */
}

/* mouse_reset — INT 33h mouse reset.
 * BENIGN NO-OP: keyboard-only Tier 1. */
void mouse_reset(void)
{
    /* INT 33h AX=0 (reset + get status) not issued — keyboard-only host. */
}

/* init_sound_tables 1000:7563 — joystick-slot install for the sound driver.
 * Engine: calls set_joystick_handler_slot() (register-passed far ptr, index AX).
 * Benign no-op here: this is a joystick-table install, not audio playback itself
 * (the audio subsystem — src/sound.c/midi.c — is fully implemented and driven
 * from host_timer.c's INT8 ISR; this leaf was never on that path either way). */
void init_sound_tables(u16 a, u16 b, u16 seg)
{
    (void)a;
    (void)b;
    (void)seg;
    /* Engine stores a sound-driver far-fn pointer into the joystick table;
     * not consumed by the playable host's joystick handling. */
}

/* set_disk_swap_callback 1000:72ef — install INT 24h + disk-swap callback.
 * BENIGN NO-OP: single-mount Tier-1 run; no disk-swap prompt needed. */
void set_disk_swap_callback(u16 int24_handler, u16 callback)
{
    (void)int24_handler;
    (void)callback;
    /* Engine: disk_swap_callback = callback; install_int24_handler(int24_handler);
     * disk_swap_callback_installed = 1.  Not needed for single-mount host. */
}

/* set_resource_table 1000:7307 — store the resource-descriptor table far ptr.
 * BENIGN NO-OP: level.c start_level bypasses the resource table and builds
 * filenames directly (see level.c RECONSTRUCTION FIDELITY note #3).
 *
 * set_resource_table is now reconstructed in host_resource.c (it selects the active
 * resource-table base so open_resource maps the title/menu indices to the right
 * files); the former NOP duplicate here is removed. */

/* reset_opaque_session_globals — opaque ~46 DGROUP resets (see game_stubs.c
 * for the full documentation of the unnamed audio-mixer/level-score bytes).
 * Unchanged from the default-build no-op: these resets are benign/inert for
 * the level-1 path and the named equivalents are handled in game.c directly. */
void reset_opaque_session_globals(void)
{
    /* The ~46 unnamed DAT_203b_xxxx = <const> stores documented in
     * game_stubs.c remain no-ops here.  See docs/reconstruction-fidelity.md. */
}

/* load_current_level_data (1000:32b0) is engine logic (per-round level reload), not
 * host platform glue — relocated to src/level.c (next to start_level, which it calls). */

#endif /* BUMPY_PLAYABLE */
