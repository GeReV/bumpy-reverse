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
 *  init_timer_resource_table (1000:7bad = bgi_overlay_thunk_adab):
 *    Engine body: calls gfx_set_current_pos() which stores register values
 *    AX/DX into bgi_cur_pos_x/bgi_cur_pos_y.  No state the playable host
 *    uses.  BENIGN NO-OP: BGI overlay not installed in the host build.
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
 *    the sound driver.  In the playable host:
 *    SILENT SOUND (Tier 1): the sound sequencer is not driven; init_sound_tables
 *    is a benign no-op.  Sound output is Plan B / Tier 2.
 *    Deviation: silent-sound in docs/reconstruction-fidelity.md row "host_boot.c".
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

/* init_timer_resource_table 1000:7bad (bgi_overlay_thunk_adab) — BGI cur-pos
 * store.  BENIGN NO-OP: BGI overlay not installed in the host build. */
void init_timer_resource_table(u16 off, u16 seg)
{
    (void)off;
    (void)seg;
    /* Engine: gfx_set_current_pos() stores AX/DX register values into
     * bgi_cur_pos_x/bgi_cur_pos_y.  Not relevant to the host path. */
}

/* init_joystick_handlers 1000:7532 — zero the joystick handler table.
 * Engine: zeros g_joystick_handler_table[16] (32 words) then calls
 * calibrate_joystick() twice.  HOST DEVIATION: table zeroed; calibration
 * skipped (keyboard-only Tier 1). */
void init_joystick_handlers(void)
{
    /* Zero all 16 far-pointer handler slots (matches engine's 32-word loop). */
    memset(g_joystick_handler_table, 0, sizeof(g_joystick_handler_table));
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
 * SILENT SOUND (Tier 1): benign no-op.  Sound output is Plan B / Tier 2. */
void init_sound_tables(u16 a, u16 b, u16 seg)
{
    (void)a;
    (void)b;
    (void)seg;
    /* Engine stores a sound-driver far-fn pointer into the joystick table.
     * Not needed for Tier 1 (no sound output). */
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
 * filenames directly (see level.c RECONSTRUCTION FIDELITY note #3). */
void set_resource_table(u16 off, u16 seg)
{
    (void)off;
    (void)seg;
    /* Engine: resource_table_ptr._0_2_ = seg; resource_table_ptr._2_2_ = off.
     * Not used on the playable host's level-load path. */
}

/* reset_opaque_session_globals — opaque ~46 DGROUP resets (see game_stubs.c
 * for the full documentation of the unnamed audio-mixer/level-score bytes).
 * Unchanged from the default-build no-op: these resets are benign/inert for
 * the level-1 path and the named equivalents are handled in game.c directly. */
void reset_opaque_session_globals(void)
{
    /* The ~46 unnamed DAT_203b_xxxx = <const> stores documented in
     * game_stubs.c remain no-ops here.  See docs/reconstruction-fidelity.md. */
}

/* load_current_level_data 1000:32b0 — per-round level (re)load.
 *
 * Engine body: copies the current level's 0x96-byte header from the in-memory
 * level archive (cur_level_ptr / level_src_ptr far pointers) into the tilemap
 * buffer at DGROUP 0xa0e4, covering 0x30 + 0x30 + 0x30 + 6 = 0x96 bytes.
 *
 * HOST IMPLEMENTATION: the in-memory level archive is never populated in the
 * playable host (start_level decodes from files directly into g_dec_buf /
 * g_bum_buf / g_pav_buf).  We reload by calling start_level(current_level,
 * current_level), which re-runs the full INT 21h file-load pipeline for the
 * current level.  The level buffers are refreshed for the round reset.
 *
 * DEVIATION (DONE_WITH_CONCERNS): start_level does more than the engine's
 * 32b0 — it also reloads the sprite bank (BUMSPJEU.BIN) and re-renders the
 * level to VGA.  For Tier 1 round resets this is harmless (a duplicate load
 * of the same data + a redundant render), but it is not byte-identical to the
 * engine's lightweight tilemap copy.  Runtime proof (round-reset loads the
 * correct level) is deferred to Task 9/11.
 * See docs/reconstruction-fidelity.md row "host_boot.c". */
void load_current_level_data(void)
{
    /* Reload the current level's resources via the real INT 21h file path.
     * current_level is the 1-based level index (defined in level.c). */
    start_level(current_level, current_level);
}

#endif /* BUMPY_PLAYABLE */
