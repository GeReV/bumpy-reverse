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
 *  FUNCTION bodies (draw_number 0x816 .. wait_vretrace_thunk 0x9864 — see
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
 *  wait_vretrace_dispatch uses (DGROUP[palette_mode*2 + 0x6976] handler; mode 2 → the
 *  vsync wait) and the blitters branch on (0=CGA, 2=EGA/VGA).  Modelled as a word (the
 *  dispatch read is a word; the low byte is the mode). */
extern u16 palette_mode;                  /* DGROUP 0x541d */

/* screen_view_desc — the 0x26-byte view/blit descriptor render_descriptor_ptr
 *  points at (a 0x22-byte core + 4 iris-status bytes).  Built by
 *  draw_hud_composite / show_title_background / show_title_and_init /
 *  run_main_menu / show_highscore_screen / level_intro_screen /
 *  play_iris_wipe_transition (screens.c) and their host counterparts
 *  (view_setup.c); consumed by anim_render_leaf_80ac / fun_7b4a_view_blit /
 *  fun_7b93_present_blank.  Every field below is write-only in this codebase
 *  (no read-back site exists), so field roles come from the constructors.
 *
 *  Two fields carry more than one role depending on which underlying blit
 *  primitive consumes the descriptor (documented here, not guessed away):
 *    - blit_off/blit_seg (+0x10/+0x12): a "tile source" far ptr for the
 *      sprite-tile HUD blit (draw_hud_composite), but view_setup.c's own
 *      comment calls the identical field a "dest far ptr" for the
 *      fullscreen-image writer — same bytes, different consumer.
 *    - subhandler (+0x1c): named to match gfx_view_desc's identical-offset
 *      dispatch-selector field; one screens.c comment calls it "clip x"
 *      instead — a pre-existing inconsistency in the ported comments, not a
 *      genuinely different field. */
typedef struct {
    u16 mode;         /* +0x00 mode/page-index selector (view_setup.c only; never
                          written by the default-build screens.c functions) */
    u16 image_off;    /* +0x02 source image far ptr: offset half */
    u16 image_seg;    /* +0x04 source image far ptr: segment half */
    u16 src_x;        /* +0x06 source X within the image */
    u16 src_y;        /* +0x08 source Y within the image */
    u16 width;        /* +0x0a width */
    u16 height;       /* +0x0c height */
    u16 flag;         /* +0x0e 0/1 flag ("field7" / NOP guard per view_setup.c) */
    u16 blit_off;     /* +0x10 second far ptr: offset half (role varies, see above) */
    u16 blit_seg;     /* +0x12 second far ptr: segment half */
    u16 dest_x;       /* +0x14 dest X */
    u16 dest_y;       /* +0x16 dest Y */
    u16 sub_w;        /* +0x18 sub-extent width */
    u16 sub_h;        /* +0x1a sub-extent height */
    u16 subhandler;   /* +0x1c dispatch selector (see above) */
    u16 clip_w;       /* +0x1e clip width */
    u16 clip_h;       /* +0x20 clip height */
    u8  status[4];    /* +0x22..+0x25 iris-wipe status bytes (render_descriptor_ptr only) */
} screen_view_desc;

/* render_descriptor_ptr (far ptr @ DGROUP 0x0574/0x0576, `DAT_0574`): points at the
 *  screen_view_desc the screen builders fill.  Split off/seg per the project's
 *  deliberate _off/_seg far-pointer convention; here modelled as a single `__far`
 *  pointer (matches the engine's LES read). */
extern u8 __far *render_descriptor_ptr;   /* DGROUP 0x0574/0x0576 */

/* fullscreen_buf (off DGROUP 0x7926 / seg DGROUP 0x7928): the engine's post-vec_decode
 *  decoded-image buffer.  The screen builders point the view descriptor's image field at
 *  fullscreen_buf+99 : fullscreen_buf_seg and the DAC writer (vga_dac_upload_from_buffer)
 *  reads the embedded 16-colour 6-bit-RGB palette at +0x33.  Split off/seg per the
 *  _off/_seg convention. */
extern u16 fullscreen_buf;                /* DGROUP 0x7926 — near off */
extern u16 fullscreen_buf_seg;            /* DGROUP 0x7928 — seg      */

/* timing_flag_accumulator (DGROUP 0x854f): the per-tick timing accumulator run_main_menu
 *  advances; observed in the screen SNAP. */
extern u8  timing_flag_accumulator;       /* DGROUP 0x854f */

/* highscore_name_buf (DGROUP 0x8f0): storage for g_highscore_default_table — the 7-entry
 *  DEFAULT high-score table (Ghidra: HighScoreEntry[7]).  Each 8-byte entry is
 *  { char __far *name (name_off:u16 + name_seg:u16, loader-relocated); u32 score }: a far
 *  ptr to the 8-char name + the 32-bit points.  render_highscore_table draws it and, when
 *  the current score qualifies, shifts the lower rows down + inserts a placeholder;
 *  highscore_enter_name edits the inserted row's name in place.  Row N = 0x8f0 + N*8; the
 *  SNAP captures row 0 (8 bytes).  The table ends exactly at 0x928 (the resource-table base)
 *  — i.e. 7 entries (56 bytes), NOT 16, and NOT a blank name-entry scratch. */
#define HIGHSCORE_NAME_LEN     8    /* bytes per entry (also the 8-char name length) */
#define HIGHSCORE_TABLE_ROWS   7    /* g_highscore_default_table has 7 entries (0x8f0..0x928) */
extern u8  highscore_name_buf[HIGHSCORE_NAME_LEN * HIGHSCORE_TABLE_ROWS];  /* DGROUP 0x8f0 */

#ifdef BUMPY_PLAYABLE
/* init_highscore_default_table — populate highscore_name_buf with the 7 built-in default
 *  high scores.  RECONSTRUCTION FIDELITY: the original stores g_highscore_default_table as
 *  loader-relocated STATIC data (the DOS EXE loader fixes up each entry's name far ptr); the
 *  recon cannot statically embed relocated far pointers into the byte storage, so the
 *  playable build fills the table at startup (same pattern as init_move_scripts /
 *  init_worldmap_data / init_anim_data).  The default BUMPY.EXE keeps the zero-init storage. */
void init_highscore_default_table(void);

/* init_password_table — populate password_table (DGROUP 0x135c) with the 8 six-char level
 *  passwords (ACCESS/BUTTON/ISLAND/PRETTY/WINNER/ZOMBIE/LOVELY/SYSTEM).  Same
 *  loader-relocated-far-ptr constraint as the HOF table; playable build only. */
void init_password_table(void);

/* init_ega_palette_patch_tables — populate dgroup_pal_patch_63a/72e/64a/71e (DGROUP
 *  0x63a/0x72e/0x64a/0x71e) with the real EGA->VGA AC palette-patch bytes the
 *  palette_mode==1 title/menu builders copy into the decoded image.  Same
 *  loader-relocated-static-data constraint as init_password_table; playable build only
 *  (inert under the default boot's palette_mode==2). */
void init_ega_palette_patch_tables(void);
#endif

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
 *  PORTED (Phase-7 T3): the text/number primitives + the in-game score HUD.  Their
 *  1:1 bodies live in screens.c; the matching game_stubs.c stubs are removed.  The
 *  remaining screen fns (title/menu/highscore/intro — T4/T5) stay stubbed in
 *  game_stubs.c with their own faithful-signature declarations until ported. */

/* draw_text_at (1000:07f0): set the text position (x, y) (overlay 1ab9:1441 → DGROUP
 *  0x6942/0x6944) then draw the NUL-terminated glyph string at str_seg:str_off
 *  (overlay 1ab9:13ec).  (Fixed 2026-07-02: args 3/4 were misnamed clip_w/clip_h.) */
void draw_text_at(u16 str_off, u16 str_seg, u16 x, u16 y);

/* draw_number (1000:0816): format the 32-bit value (val_hi:val_lo) as a right-justified,
 *  space-padded decimal string of `width` digits ("OVER FLOW" if width>=8) into
 *  formatted_number_buf and draw it via draw_text_at (arg_a/arg_c = text position x/y). */
void draw_number(u16 val_lo, u16 val_hi, u8 width, u16 arg_a, u16 arg_c);

/* draw_number_sprites (1000:603d): render `width` right-justified decimal digit SPRITES
 *  via the p1_sprite blit descriptor (frame = digit + 0x17c, x = base_x + i*0x10). */
void draw_number_sprites(u16 value_lo, u16 value_hi, u8 width, u16 base_x, u16 frame_y);

/* draw_hud_composite (1000:51d8): build the in-game status row — fill the
 *  render_descriptor_ptr view struct and call FUN_1000_80ac 7× (7 HUD sprite tiles). */
void draw_hud_composite(void);

/* ── PORTED (Phase-7 T4): title screens + main menu + iris-wipe + DAC palette ────────
 *  1:1 bodies in screens.c (addresses cited there); the matching game_stubs.c stubs are
 *  removed.  These are also declared in game.h (the game-loop spine's callees); the
 *  prototypes match.  The remaining highscore / level-intro fns stay stubbed (T5). */

/* init_title_graphics (1000:2ef8): set the resource table, show the title background,
 *  load+process the sprite resource + the HUD image, draw the HUD composite. */
void init_title_graphics(void);

/* show_title_background (1000:2fac): load resource 2 (vec-decoded fullscreen image),
 *  optional palette patch, iris-wipe in, build the 20x25 bg view, present, upload DAC,
 *  run the intro animation loop. */
void show_title_background(void);

/* show_title_and_init (1000:3ed4): show highscore screen (T5), load+decode resource
 *  0x11, iris-wipe in, present, wait for a keypress, current_level=1. */
void show_title_and_init(void);

/* run_main_menu (1000:35a5): the 4-option cursor STATE MACHINE — load the menu image,
 *  poll up/down/fire, draw the cursor sprite (p1_sprite[1]=cursor_index*0x10+0x70), cycle
 *  menu_option2_setting on option 2; returns the selected item (0xff while selecting). */
u8 run_main_menu(void);

/* show_menu_select_screen (1000:0f7a): fullscreen image (resource 3) + three sprite-glyph
 *  text rows; current_level = enter_password() (default 1). */
void show_menu_select_screen(void);

/* play_iris_wipe_transition (1000:3467): the rectangle-wipe screen transition — step the
 *  blit-view rect inward over 10 steps (4 view-blits + DAC uploads per step), then clear. */
void play_iris_wipe_transition(void);

/* wait_vretrace_thunk (1000:9864): CALLF thunk -> wait_vretrace_dispatch (2036:0000),
 *  whose mode-2 overlay handler (2036:0015) is a VERTICAL-RETRACE (vsync) WAIT — poll
 *  Input Status #1 (0x3da) bit 3 until the retrace starts, then ends.  FORMERLY mis-named
 *  upload_vga_dac_palette: it is NOT a DAC upload (Task-2 misnomer correction).  The real
 *  DAC upload is host_gfx_upload_palette_to_dac / vga_dac_upload_from_buffer. */
void wait_vretrace_thunk(void);

/* wait_vretrace_dispatch (2036:0000): the vsync-wait dispatch wait_vretrace_thunk CALLFs
 *  (formerly dispatch_by_palette_mode_2036). */
void wait_vretrace_dispatch(void);

/* ── PORTED (Phase-7 T5): highscore screens + name-entry + level-intro ───────────────
 *  1:1 bodies in screens.c (addresses cited there); the matching game_stubs.c stubs are
 *  removed.  Completes the front-end port. */

/* show_highscore_screen (1000:5681): load+display the highscore background, build the
 *  bg view, then render the highscore table. */
void show_highscore_screen(void);

/* render_highscore_table (1000:57e1): render the 7-entry highscore table (name glyphs +
 *  scores via draw_number_sprites); insert+name-entry if the score qualifies, else wait. */
void render_highscore_table(void);

/* highscore_enter_name (1000:59d3): the interactive 8-char table-row name-entry state
 *  machine (polls FUN_75a2; left/right cycle letters, prev/next move the cursor). */
void highscore_enter_name(u8 row);

/* enter_password (1000:5c87): the interactive 6-char menu-select name-entry state
 *  machine; compares the typed name vs the 8-entry table and returns the matched index
 *  + 2 (or 0).  Args: col (x), row (y). */
u8 enter_password(u8 col, u8 row);

/* draw_name_entry_cursor (1000:5fdb): position + draw the blinking name-entry cursor
 *  sprite at (col,row) with glyph `frame`; the shared helper of both name-entry SMs. */
u16 draw_name_entry_cursor(u8 col, u8 row, u16 frame, char do_blit);

/* level_intro_screen (1000:3852): the per-level intro screen + interactive move loop
 *  (load+decode the level border image, draw the HUD + Bumpy, then directions/fire/quit). */
void level_intro_screen(void);

/* show_level_intro_screen (1000:0d9d): display the fullscreen image + the current_level
 *  name as sprite glyphs, then wait for the start input. */
void show_level_intro_screen(void);

#endif /* SCREENS_H */
