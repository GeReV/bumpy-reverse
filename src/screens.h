#ifndef SCREENS_H
#define SCREENS_H

#include "bumpy.h"

/* ────────────────────────────────────────────────────────────────────────────
 *  screens.h — front-end subsystem (Phase-7 reconstruction): the title / menu /
 *  highscore / level-intro screens + the HUD/number formatters + the DAC palette
 *  upload (Ghidra seg 1000; full map in tools/screens_oracle.py FN_NAMES).
 *
 *  SKELETON (Phase-7 Task 2): this header declares the front-end module's GLOBALS
 *  only — the screen-state scalars (palette_mode, menu_option2_setting, current_level)
 *  the engine's render descriptors hang off, the render_descriptor_ptr view-struct
 *  far ptr, the fullscreen_buf decoded-image far ptr (off/seg split), the highscore
 *  name-entry buffer, and the formatted-number output buffer.  The ~20 screen
 *  FUNCTION bodies (draw_number 0x816 .. upload_vga_dac_palette 0x9864 — see
 *  tools/screens_oracle.py FN_NAMES) remain stubbed in game_stubs.c this task; their
 *  1:1 bodies port across Phase-7 T3–T5, validated by the host replay harness
 *  tools/screens_ctest.c against the Phase-7 T1 capture
 *  local/build/render/screens_trace.bin (magic "SCRTRC01", version 1).
 *
 *  Because screens.c contributes NO function bodies this task, screens.obj links
 *  cleanly alongside the game_stubs.c screen stubs with ZERO duplicate symbols — the
 *  globals-only-skeleton pattern Phase-6 T2 (sound.obj) / Phase-5 T2 (anim.obj) /
 *  Phase-4 T2 (player2.obj) / Phase-3 T2 (items.obj) used.
 *
 *  Provenance for every address/layout: tools/screens_oracle.py header (the frozen
 *  trace layout + resolved DGROUP/CODE offsets) + local/build/screens_model.md (the
 *  Phase-7 T1 capture), grounded in the Ghidra BumpyDecomp + raw disassembly.
 * ──────────────────────────────────────────────────────────────────────────── */

/* ── DEFINED in screens.c (genuinely new — no other src/ TU defines them) ─────────
 *  See the OWNERSHIP block at the top of screens.c for the per-symbol grep evidence. */

/* palette_mode (DGROUP 0x541d): the active display/palette mode — the dispatch index
 *  upload_vga_dac_palette uses (DGROUP[palette_mode*2 + 0x6976] handler) and the
 *  blitters branch on (0=CGA, 2=EGA/VGA).  Modelled as a word (the dispatch read is a
 *  word; the low byte is the mode). */
extern u16 palette_mode;                  /* DGROUP 0x541d */

/* render_descriptor_ptr (far ptr @ DGROUP 0x0574/0x0576, `DAT_0574`): points at the
 *  0x22-byte view struct the screen builders fill (image far ptr, src/dst x/y, w/h,
 *  flags, clip rect).  Split off/seg per the project's deliberate _off/_seg far-pointer
 *  convention; here modelled as a single `__far` pointer (matches the engine's LES read). */
extern u8 __far *render_descriptor_ptr;   /* DGROUP 0x0574/0x0576 */

/* fullscreen_buf (off DGROUP 0x7926 / seg DGROUP 0x7928): the engine's post-vec_decode
 *  decoded-image buffer.  The screen builders point the view descriptor's image field at
 *  fullscreen_buf+99 : fullscreen_buf_seg and (upload_vga_dac_palette) read the embedded
 *  16-colour 6-bit-RGB palette at +0x33.  Split off/seg per the _off/_seg convention. */
extern u16 fullscreen_buf;                /* DGROUP 0x7926 — near off */
extern u16 fullscreen_buf_seg;            /* DGROUP 0x7928 — seg      */

/* timing_flag_accumulator (DGROUP 0x854f): the per-tick timing accumulator run_main_menu
 *  advances; observed in the screen SNAP. */
extern u8  timing_flag_accumulator;       /* DGROUP 0x854f */

/* highscore name-entry buffer (DGROUP 0x8f0): the highscore table's name strings, 8
 *  bytes per entry; row N starts at 0x8f0 + N*8.  highscore_enter_name / enter_highscore_
 *  name edit it in place.  The SNAP captures row 0 (8 bytes).  Sized for 16 table rows. */
#define HIGHSCORE_NAME_LEN     8
#define HIGHSCORE_TABLE_ROWS   16
extern u8  highscore_name_buf[HIGHSCORE_NAME_LEN * HIGHSCORE_TABLE_ROWS];  /* DGROUP 0x8f0 */

/* formatted_number_buf: the ASCII scratch the number formatters (draw_number 0x816 /
 *  draw_number_sprites 0x603d) build the decimal/score digits into before blitting them.
 *  Owned here (no other TU defines it); sized generously for the widest field (score). */
#define FORMATTED_NUMBER_LEN   16
extern char formatted_number_buf[FORMATTED_NUMBER_LEN];

/* ── EXTERN — owned elsewhere (grep evidence in screens.c ownership block) ─────────
 *  current_level          level.c:106  `u8 current_level = 1u;`        (0x79b2)
 *  input_state            input.c:46   `u8 input_state;`               (0x8244)
 *  menu_option2_setting   game.c:56    `u8 menu_option2_setting;`      (0x79b5)
 *  p1_sprite              anim.c:119   `u8 __far *p1_sprite;`          (0x8884)
 *  score_lo / score_hi    game.c:58-59 `u16 score_lo; u16 score_hi;`   (0xa0d4/0xa0d6)
 *  frame_abort_flag       game.c:51    `u8 frame_abort_flag;`          (0x928d)
 *  These are already declared extern in their owning modules' headers; screens.c does
 *  NOT redefine them.  Their declarations are NOT re-emitted here (the screen fn ports,
 *  T3–T5, pull them via the owning headers when they land), avoiding redundant
 *  declarations this task. */

/* ── screen / HUD function prototypes ─────────────────────────────────────────────
 *  UNPORTED this task: every screen fn body remains stubbed in game_stubs.c.  Their
 *  1:1 bodies, and the matching prototypes, land in Phase-7 T3–T5 (text/number
 *  formatters + HUD; title/menu/highscore state machines; level-intro + the iris-wipe
 *  / DAC-upload palette path).  No prototypes are declared here yet — the stubs in
 *  game_stubs.c carry their own (faithful-signature) declarations. */

#endif /* SCREENS_H */
