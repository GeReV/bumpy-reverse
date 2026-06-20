/* ────────────────────────────────────────────────────────────────────────────
 *  screens.c — front-end subsystem (Phase-7 reconstruction).
 *
 *  SKELETON (Phase-7 Task 2): this TU defines ONLY the front-end module's GLOBALS —
 *  NO function bodies.  The ~20 screen/HUD functions (draw_number 1000:0816 ..
 *  upload_vga_dac_palette 1000:9864, full map in tools/screens_oracle.py FN_NAMES)
 *  remain stubbed in game_stubs.c this task; their 1:1 bodies port across Phase-7
 *  T3–T5:
 *    T3 text/number  (draw_number / draw_text_at / draw_number_sprites + the HUD
 *                     compositor draw_hud_composite),
 *    T4 title/menu/  (init_title_graphics / show_title_* / run_main_menu /
 *       highscore     show_menu_select_screen / show_highscore_screen /
 *                     render_highscore_table / enter/highscore_enter_name),
 *    T5 intro/palette (level_intro_screen / show_level_intro_screen /
 *                     play_iris_wipe_transition / upload_vga_dac_palette).
 *  At each port the corresponding stub is removed from game_stubs.c and the body
 *  reconstructed here, validated by tools/screens_ctest.c against the Phase-7 T1
 *  trace local/build/render/screens_trace.bin (magic "SCRTRC01").
 *
 *  Because this TU contributes no function bodies, screens.obj links cleanly
 *  alongside the game_stubs.c screen stubs with ZERO duplicate symbols — the
 *  globals-only skeleton pattern Phase-6 T2 (sound.obj) / Phase-5 T2 (anim.obj) /
 *  Phase-4 T2 (player2.obj) / Phase-3 T2 (items.obj) used.
 *
 *  ── OWNERSHIP / no-duplicate-symbols (grep-verified across the src/ tree) ───────
 *    DEFINED HERE (genuinely new — a grep over the src C TUs finds NO other def):
 *      palette_mode               DGROUP 0x541d  (display/palette mode + DAC dispatch
 *                                 index).  grep `palette_mode` over the src C TUs finds
 *                                 only a FUNCTION PARAMETER in sprite.c (sprite_frame_transform
 *                                 / sprite_bank_load_transform) and comments — no global
 *                                 def; globals.c has none.  Owned here.
 *      render_descriptor_ptr      DGROUP 0x0574/0x0576 (DAT_0574 view-struct far ptr).
 *                                 grep finds zero matches anywhere in src/ — new symbol.
 *      fullscreen_buf / _seg      DGROUP 0x7926 / 0x7928.  grep finds the NAME only in
 *                                 bgi_overlay.c / entity.c comments (describing the
 *                                 decoded-image buffer); no symbol def — owned here.
 *      timing_flag_accumulator    DGROUP 0x854f.  grep `0x854f`/`timing_flag` finds
 *                                 nothing in src/ — new symbol.
 *      highscore_name_buf[128]    DGROUP 0x8f0 (name-entry buffer, 8B/row × 16 rows).
 *                                 grep `0x8f0`/`highscore_name` finds nothing — new.
 *      formatted_number_buf[16]   number-formatter ASCII scratch.  No other TU defines
 *                                 it — new symbol.
 *    None of these names appear in any other src/ TU (checked: game.c, level.c,
 *    input.c, player.c, player2.c, items.c, anim.c, sound.c, entity.c, bgi_overlay.c,
 *    globals.c, game_stubs.c) — so defining them here introduces no duplicate symbol.
 *
 *    EXTERN (owned elsewhere — grep evidence beside each; NOT defined here):
 *      current_level         level.c:106  `u8 current_level = 1u;`         (0x79b2)
 *      input_state           input.c:46   `u8 input_state;`                (0x8244)
 *      menu_option2_setting  game.c:56    `u8 menu_option2_setting;`       (0x79b5)
 *      p1_sprite             anim.c:119   `u8 __far *p1_sprite;`           (0x8884)
 *                            — the 0x8884 blit-descriptor far ptr; anim.c is the sole
 *                            owner (there is no other p1_sprite variable; entity.c
 *                            references it only in comments).
 *      score_lo / score_hi   game.c:58-59 `u16 score_lo; u16 score_hi;`    (0xa0d4/0xa0d6)
 *                            — the 32-bit score level_intro_screen draws via draw_number.
 *      frame_abort_flag      game.c:51    `u8 frame_abort_flag;`           (0x928d)
 *                            — the byte show_title_and_init sets (the oracle SNAP's
 *                            "game_state_928d"); already owned by game.c.
 *    These are already declared extern in their owning modules' headers; screens.c does
 *    NOT redefine them.  The screen fn ports (T3–T5) reach them through those headers
 *    when they land — so no extern is re-emitted here this (globals-only) task.  The
 *    host replay harness tools/screens_ctest.c supplies its own host definitions of any
 *    cross-module global a future ported fn references (it does not link the other
 *    .objs), mirroring the sound_ctest / items_ctest / anim_chan_ctest convention.
 *
 *  STACK-CHECK PROLOGUE: every original screen fn opens with Turbo C's compiler-emitted
 *  stack-overflow probe; it is NOT game logic and will be intentionally OMITTED from the
 *  future ports (the convention player.c / items.c / anim.c / sound.c document).
 * ──────────────────────────────────────────────────────────────────────────────── */

#include "screens.h"

/* ── screen-state scalars ───────────────────────────────────────────────────────── */
u16 palette_mode;                  /* DGROUP 0x541d — display/palette mode + DAC dispatch idx */

/* ── render descriptors / decoded-image buffer (far ptrs) ───────────────────────── */
u8 __far *render_descriptor_ptr;   /* DGROUP 0x0574/0x0576 — view-struct far ptr (DAT_0574) */
u16 fullscreen_buf;                /* DGROUP 0x7926 — decoded-image buffer near off          */
u16 fullscreen_buf_seg;            /* DGROUP 0x7928 — decoded-image buffer seg               */

/* ── per-tick timing accumulator (run_main_menu) ────────────────────────────────── */
u8  timing_flag_accumulator;       /* DGROUP 0x854f */

/* ── highscore name-entry buffer + number-formatter scratch ─────────────────────── */
u8  highscore_name_buf[HIGHSCORE_NAME_LEN * HIGHSCORE_TABLE_ROWS];  /* DGROUP 0x8f0 */
char formatted_number_buf[FORMATTED_NUMBER_LEN];                    /* number-formatter ASCII */
