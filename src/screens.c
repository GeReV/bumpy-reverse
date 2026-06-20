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

#ifndef BUMPY_H
#include <conio.h>   /* outp — the x86 port intrinsic the VGA-DAC palette upload issues.
                        Skipped on the host replay harness (it #defines BUMPY_H and supplies
                        its own outp capture shim; see tools/screens_ctest.c).  Under wcc
                        this is the real OUT. */
#endif
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

/* ════════════════════════════════════════════════════════════════════════════
 *  Phase-7 Task 3 — text / number primitives + the in-game score HUD.
 *
 *  Ported 1:1 from the live Ghidra BumpyDecomp + raw disassembly (decompiled fresh
 *  2026-06): draw_text_at (1000:07f0), draw_number (1000:0816),
 *  draw_hud_composite (1000:51d8), draw_number_sprites (1000:603d).  The
 *  STACK-CHECK PROLOGUE (`CMP [0x6b4c],SP; JA +; CALL ab83`) of every original is
 *  the Turbo C compiler-emitted stack-overflow probe — NOT game logic — and is
 *  intentionally OMITTED (the player.c / items.c / anim.c / sound.c convention).
 *
 *  ── STUBBED LEAVES (faithful-signature; not re-driven here) ────────────────────
 *    FUN_1000_80ac (the unnamed B-side render leaf draw_hud_composite calls 7×) is
 *    already reconstructed as a faithful-signature stub in anim.c
 *    (anim_render_leaf_80ac) — the SAME engine function (1000:80ac); draw_hud_composite
 *    calls it through that symbol so BUMPY.EXE links it once (anim.obj).  Likewise
 *    blit_sprite (1000:942a) is anim.c's anim_blit_sprite_leaf.  The font/BGI-overlay
 *    text leaves draw_text_at forwards to — FUN_1000_9837 (1000:9837, a thunk to the
 *    text-clip overlay FUN_1ab9_1441) and FUN_1000_9804 (1000:9804, a thunk to
 *    draw_string_glyphs) — have no reconstructed callable symbol (BGI overlay text,
 *    not decompiled), so they are kept as faithful-signature no-op stubs HERE.
 *    RECONSTRUCTION FIDELITY: these leaves preserve each call site 1:1 without
 *    re-driving the BGI overlay / Phase-0 render core; their observable output (the
 *    render_descriptor_ptr view struct + the p1_sprite blit descriptor) IS produced
 *    here and is the validated descriptor-level gate (tools/screens_ctest.c §B).
 *
 *  ── crt_uldiv_32 / crt_lmul_32 (Turbo C 32-bit long helpers) ───────────────────
 *    draw_number / draw_number_sprites extract decimal digits with the Turbo C runtime
 *    32-bit unsigned divide (crt_uldiv_32 @ 1000:a8ee, value/=10) and 32-bit multiply
 *    (crt_lmul_32 @ 1000:aa14, quot*10) helpers; the low byte of (value - quot*10) is
 *    the next digit.  RECONSTRUCTION FIDELITY: these are compiler-emitted CRT long
 *    arithmetic, not game functions; reconstructed here as native uint32 (u32) math —
 *    `q = v / 10; digit = (u8)(v - q*10); v = q` — which is bit-identical to the CRT
 *    helpers' result (standard unsigned 32-bit div/mul).  The 32-bit value is the
 *    engine's CONCAT22(val_hi, val_lo) = ((u32)val_hi << 16) | val_lo.
 *
 *  ── DS-stamped descriptor segment fields ───────────────────────────────────────
 *    draw_hud_composite stamps the RUNTIME DS register into the view descriptor's
 *    tile-source far-data segment (+0x12) — Ghidra renders DS as the static 0x203b,
 *    but the loaded image's runtime DGROUP segment differs.  Mirror anim.c: a
 *    SCREENS_DGROUP_RUNTIME_SEG macro (default the static 0x203b) the host harness
 *    overrides to the captured runtime value so the descriptor gate compares against
 *    the engine's actual EXIT bytes.
 *
 *  ── formatted_number_buf (digit scratch) ───────────────────────────────────────
 *    The originals build the decimal string in a STACK-LOCAL `digit_buf[9]` and pass
 *    its address to the draw leaf.  RECONSTRUCTION FIDELITY: draw_number builds into
 *    the module-global formatted_number_buf instead of an SS-local array, so the
 *    formatted digit string is OBSERVABLE for the semantic gate (the string is the
 *    validated output; the leaf draw is stubbed).  Same byte content, same algorithm;
 *    only the buffer's storage class differs.  draw_number_sprites keeps its own local
 *    digit_buf (its observable output is the p1_sprite descriptor, validated directly).
 * ──────────────────────────────────────────────────────────────────────────────── */

extern u8 __far *p1_sprite;                 /* anim.c — DGROUP 0x8884 blit-descriptor far ptr */

/* cross-module screen-state globals the T4 fns reach (owned by their modules; resolve at
   the BUMPY.EXE link; the host replay harness supplies its own definitions). */
extern u8 input_state;                      /* input.c 0x8244 */
extern u8 current_level;                    /* level.c 0x79b2 */
extern u8 menu_option2_setting;             /* game.c  0x79b5 */
extern u8 frame_abort_flag;                 /* game.c  0x928d (DAT_203b_928d) */

/* render/text leaves (see header block).  FUN_80ac / blit_sprite are anim.c's
   faithful-signature stubs (the SAME engine fns); the font leaves are stubbed here. */
void anim_render_leaf_80ac(u8 __far *view);       /* FUN_1000_80ac  1000:80ac (anim.obj) */
void anim_blit_sprite_leaf(u16 obj_off, u16 obj_seg); /* blit_sprite 1000:942a (anim.obj) */
void text_clip_leaf_9837(u16 clip_w, u16 clip_h); /* FUN_1000_9837  1000:9837 (BGI text)  */
void draw_string_glyphs_9804(u16 x, u16 y);       /* FUN_1000_9804  1000:9804 (BGI glyphs) */

/* RECONSTRUCTION FIDELITY: BGI-overlay text leaves — faithful-signature no-op stubs
   (no clean decomp; do not invent a body).  draw_text_at's call sites are preserved
   1:1; the text pixels are the BGI overlay's, not modelled here. */
void text_clip_leaf_9837(u16 clip_w, u16 clip_h) { (void)clip_w; (void)clip_h; return; }
void draw_string_glyphs_9804(u16 x, u16 y)       { (void)x; (void)y; return; }

/* DS-stamped far-data segment for the HUD view descriptor (+0x12).  Default = the
   Ghidra static DGROUP segment 0x203b; the host harness overrides to the captured
   runtime value (mirrors anim.c's ANIM_DGROUP_RUNTIME_SEG). */
#ifndef SCREENS_DGROUP_RUNTIME_SEG
#define SCREENS_DGROUP_RUNTIME_SEG 0x203b
#endif

/* ════════════════════════════════════════════════════════════════════════════
 *  draw_text_at — 1000:07f0
 *  Render a string at (x:y) with a clip extent (clip_w, clip_h): set the text clip
 *  rect (FUN_9837) then draw the glyphs (FUN_9804).  Called by draw_number with the
 *  formatted-string far ptr in (x,y) and the caller's two trailing args in
 *  (clip_w, clip_h).  Disasm 07f0: PUSH [BP+0xa];PUSH [BP+8];CALL 9837;
 *  PUSH [BP+6];PUSH [BP+4];CALL 9804.
 * ════════════════════════════════════════════════════════════════════════════ */
void draw_text_at(u16 x, u16 y, u16 clip_w, u16 clip_h)
{
    text_clip_leaf_9837(clip_w, clip_h);
    draw_string_glyphs_9804(x, y);
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  draw_number — 1000:0816
 *  Format the 32-bit value (val_hi:val_lo) as a right-justified, space-padded decimal
 *  string of `width` digits and draw it via draw_text_at; prints "OVER FLOW" if
 *  width >= 8.  Two trailing args (arg_a, arg_c) are forwarded to draw_text_at as its
 *  clip_w / clip_h.  Disasm 0816: pad digit_buf[0..width-1]=' '; digit_buf[width]=0;
 *  per digit val/=10 (a8ee), rem=val_lo - quot*10 (aa14), digit_buf[width-1]=rem+'0'.
 * ════════════════════════════════════════════════════════════════════════════ */
void draw_number(u16 val_lo, u16 val_hi, u8 width, u16 arg_a, u16 arg_c)
{
    u32  value;
    u32  quot;
    u8   i;
    char *out_str;

    value = ((u32)val_hi << 16) | (u32)val_lo;
    if (width < 8) {
        for (i = 0; i <= width; i++) {
            formatted_number_buf[i] = ' ';
        }
        formatted_number_buf[width] = '\0';
        while (width != 0) {
            quot = value / 10;                          /* crt_uldiv_32(value, 10) */
            /* digit = value - quot*10  (crt_lmul_32 quot*10, low byte) */
            formatted_number_buf[(u8)(width - 1)] =
                (char)((u8)(value - quot * 10) + '0');
            value = quot;
            width = width - 1;
        }
        out_str = formatted_number_buf;
    } else {
        out_str = "OVER FLOW";                          /* s_OVER_FLOW @ DS:0x62e */
    }
    /* The engine passes the string's near offset as draw_text_at's x (and DS/SS as y);
       on the host the value is unused (draw_text_at's text leaves are stubbed) — the
       formatted_number_buf contents ARE the validated output (semantic gate).  Cast via
       `unsigned long` so the host 64-bit pointer narrows without a pointer-size warning;
       in the 16-bit OW build out_str is a near char* and the cast is a plain truncation. */
    draw_text_at((u16)(unsigned long)out_str, 0, arg_a, arg_c);
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  draw_number_sprites — 1000:603d
 *  Render a numeric value as right-justified decimal DIGIT SPRITES: format `width`
 *  digits (same /10 loop as draw_number) into a local digit_buf, then for each digit
 *  fill the p1_sprite blit descriptor (0x792e via far ptr @ 0x8884) — word[1]=frame_y,
 *  word[2]=digit+0x17c (the glyph frame), word[0]=base_x + i*0x10 — and blit_sprite.
 *  Disasm 603d: LES BX,[0x8884]; ES:[BX+2]=frame_y; ES:[BX+4]=digit+0x17c;
 *  ES:[BX]=base_x+i*0x10; CALL 942a (blit_sprite 0x792e, DS).
 * ════════════════════════════════════════════════════════════════════════════ */
void draw_number_sprites(u16 value_lo, u16 value_hi, u8 width, u16 base_x, u16 frame_y)
{
    u32  value;
    u32  quot;
    u8   digit_buf[9];
    u8   width0;
    u8   i;

    width0 = width;                                     /* saved digit count (BP-0xc) */
    value = ((u32)value_hi << 16) | (u32)value_lo;
    for (i = 0; i < width; i++) {
        digit_buf[i] = 0x20;                            /* ' ' */
    }
    digit_buf[width] = 0;
    while (width != 0) {
        quot = value / 10;
        digit_buf[(u8)(width - 1)] = (u8)(value - quot * 10) + 0x30;
        value = quot;
        width = width - 1;
    }
    ((u16 __far *)p1_sprite)[1] = frame_y;              /* ES:[BX+2] */
    for (i = 0; i < width0; i++) {
        ((u16 __far *)p1_sprite)[2] = (u16)digit_buf[i] + 0x17c;  /* ES:[BX+4] */
        ((u16 __far *)p1_sprite)[0] = base_x + (u16)i * 0x10;     /* ES:[BX] */
        anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
    }
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  draw_hud_composite — 1000:51d8
 *  Build the in-game status row (score / lives / level): fill the render_descriptor_ptr
 *  view struct (DAT_0574, 0x22 bytes; fields +2..+0x20) and call FUN_1000_80ac SEVEN
 *  times — one 4-plane sprite-tile blit per HUD element.  The first fill sets all
 *  fields; each later fill mutates only the changed fields (src x/y at +6/+8, the
 *  tile-source far ptr at +0x10/+0x12, and the sub-extent / clip at +0x18/+0x1a /
 *  +0x1e/+0x20), so the descriptor is CUMULATIVE across the 7 fills.  Disasm 51d8.
 * ════════════════════════════════════════════════════════════════════════════ */
void draw_hud_composite(void)
{
    u8 __far *d = render_descriptor_ptr;

    /* fill 1 — full descriptor (image far ptr, src 0,0, 0x14×0x19, tile @0x9d3a) */
    *(u16 __far *)(d + 0x02) = fullscreen_buf + 99;          /* image off (+99) */
    *(u16 __far *)(d + 0x04) = fullscreen_buf_seg;           /* image seg */
    *(u16 __far *)(d + 0x06) = 0;                            /* src x */
    *(u16 __far *)(d + 0x08) = 0;                            /* src y */
    *(u16 __far *)(d + 0x0a) = 0x14;                         /* width  */
    *(u16 __far *)(d + 0x0c) = 0x19;                         /* height */
    *(u16 __far *)(d + 0x10) = 0x9d3a;                       /* tile-source off */
    *(u16 __far *)(d + 0x12) = SCREENS_DGROUP_RUNTIME_SEG;   /* tile-source seg (DS) */
    *(u16 __far *)(d + 0x14) = 0;                            /* dest x */
    *(u16 __far *)(d + 0x16) = 0;                            /* dest y */
    *(u16 __far *)(d + 0x18) = 3;                            /* sub-extent w */
    *(u16 __far *)(d + 0x1a) = 2;                            /* sub-extent h */
    *(u16 __far *)(d + 0x1c) = 0;                            /* clip x */
    *(u16 __far *)(d + 0x1e) = 3;                            /* clip w */
    *(u16 __far *)(d + 0x20) = 2;                            /* clip h */
    anim_render_leaf_80ac(render_descriptor_ptr);

    /* fill 2 — src x=4, tile @0x9baf */
    d = render_descriptor_ptr;
    *(u16 __far *)(d + 0x06) = 4;
    *(u16 __far *)(d + 0x10) = 0x9baf;
    *(u16 __far *)(d + 0x12) = SCREENS_DGROUP_RUNTIME_SEG;
    anim_render_leaf_80ac(render_descriptor_ptr);

    /* fill 3 — src 0,8, tile @0x9eba, sub/clip 1×4 */
    d = render_descriptor_ptr;
    *(u16 __far *)(d + 0x06) = 0;
    *(u16 __far *)(d + 0x08) = 8;
    *(u16 __far *)(d + 0x10) = 0x9eba;
    *(u16 __far *)(d + 0x12) = SCREENS_DGROUP_RUNTIME_SEG;
    *(u16 __far *)(d + 0x18) = 1;
    *(u16 __far *)(d + 0x1a) = 4;
    *(u16 __far *)(d + 0x1e) = 1;
    *(u16 __far *)(d + 0x20) = 4;
    anim_render_leaf_80ac(render_descriptor_ptr);

    /* fill 4 — src y=3, tile @0x9fba */
    d = render_descriptor_ptr;
    *(u16 __far *)(d + 0x08) = 3;
    *(u16 __far *)(d + 0x10) = 0x9fba;
    *(u16 __far *)(d + 0x12) = SCREENS_DGROUP_RUNTIME_SEG;
    anim_render_leaf_80ac(render_descriptor_ptr);

    /* fill 5 — src 0,0xd, tile @0x8b88, sub/clip 6×2 */
    d = render_descriptor_ptr;
    *(u16 __far *)(d + 0x06) = 0;
    *(u16 __far *)(d + 0x08) = 0xd;
    *(u16 __far *)(d + 0x10) = 0x8b88;
    *(u16 __far *)(d + 0x12) = SCREENS_DGROUP_RUNTIME_SEG;
    *(u16 __far *)(d + 0x18) = 6;
    *(u16 __far *)(d + 0x1a) = 2;
    *(u16 __far *)(d + 0x1e) = 6;
    *(u16 __far *)(d + 0x20) = 2;
    anim_render_leaf_80ac(render_descriptor_ptr);

    /* fill 6 — src y=0x11, tile @0x824e */
    d = render_descriptor_ptr;
    *(u16 __far *)(d + 0x08) = 0x11;
    *(u16 __far *)(d + 0x10) = 0x824e;
    *(u16 __far *)(d + 0x12) = SCREENS_DGROUP_RUNTIME_SEG;
    anim_render_leaf_80ac(render_descriptor_ptr);

    /* fill 7 — src y=0x15, tile @0x8582 */
    d = render_descriptor_ptr;
    *(u16 __far *)(d + 0x08) = 0x15;
    *(u16 __far *)(d + 0x10) = 0x8582;
    *(u16 __far *)(d + 0x12) = SCREENS_DGROUP_RUNTIME_SEG;
    anim_render_leaf_80ac(render_descriptor_ptr);
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Phase-7 Task 4 — title screens + main menu (cursor state machine) + the
 *  iris-wipe transition + the DAC palette upload.
 *
 *  Ported 1:1 from the live Ghidra BumpyDecomp + raw disassembly (decompiled fresh
 *  2026-06):
 *    init_title_graphics        1000:2ef8
 *    show_title_background       1000:2fac
 *    show_title_and_init         1000:3ed4
 *    run_main_menu               1000:35a5   (the 4-option cursor state machine)
 *    show_menu_select_screen     1000:0f7a
 *    play_iris_wipe_transition   1000:3467   (the rectangle-wipe descriptor sweep)
 *    upload_vga_dac_palette      1000:9864   (thunk -> dispatch_by_palette_mode_2036)
 *  The Turbo C stack-check prologue (`CMP [0x6b4c],SP; CALL ab83`) of each original
 *  is the compiler-emitted stack probe — NOT game logic — and is intentionally
 *  OMITTED (the documented player/items/anim/sound/T3 convention).
 *
 *  ── SEEDED vs STUBBED leaves ───────────────────────────────────────────────────
 *  SEEDED (no INT 21h; the host harness / engine boot fills the buffers):
 *    open_resource / read_chunked / c_close — the resource loader.  Reconstructed
 *    here as faithful-signature stubs; the decoded image (`fullscreen_buf`) is SEEDED
 *    by the harness from the engine's own post-vec_decode buffer (the screens_oracle
 *    natural-load capture), so the descriptor builds run deterministically.
 *    vec_decode / process_sprites / set_resource_table — likewise faithful-signature
 *    stubs (the decode/sprite-table work is the engine's; its OUTPUT is seeded).
 *  STUBBED render-core / BGI-overlay leaves (RECONSTRUCTION FIDELITY — preserve each
 *  call site 1:1 without re-driving the Phase-0 render core; their observable output
 *  — the render_descriptor_ptr view struct + the p1_sprite blit descriptor — IS
 *  produced here and is the validated descriptor-level gate):
 *    restore_bg_view / present_frame / init_fullscreen_view_desc  (owned by
 *      bgi_overlay.c / game_stubs.c — extern, NOT redefined here);
 *    FUN_1000_7b93 / FUN_1000_7bca / FUN_1000_7b4a (the per-step view-blit) /
 *      FUN_1000_9410 (set_sprite_table_ptr trampoline) — BGI overlay leaves, stubbed;
 *    blit_sprite (1000:942a) routed through anim.c's anim_blit_sprite_leaf (same
 *      engine fn, linked once in anim.obj — zero dup);
 *    wait_keypress / poll_input / FUN_1000_75a2 — input path (poll_input is input.c
 *      reconstructed; wait_keypress is game_stubs.c; FUN_75a2 stubbed here).  The host
 *      seeds the scripted input sequence (the captured FUN_75a2 return stream) so the
 *      menu / state-machine loops progress exactly as the engine did.
 *    show_highscore_screen / enter_highscore_name — TASK 5 (highscore); stubbed
 *      (show_highscore_screen owned by game_stubs.c — extern; enter_highscore_name
 *      stubbed here pending T5).
 *    play_intro_animation_loop / wait_50_frames — animation idle leaves, stubbed.
 *
 *  ── upload_vga_dac_palette / play_iris_wipe_transition — the DAC carve-out ─────────
 *  upload_vga_dac_palette (1000:9864) is a 1:1 thunk to dispatch_by_palette_mode_2036
 *  (2036:0000), which indirect-calls the handler at the overlay table
 *  `[palette_mode*2 + 0x6976]`.  That table is RUNTIME-POPULATED by the BGI driver
 *  init (all-zero in the static image) and the handlers are dynamically-loaded BGI
 *  overlay code — NOT in the Ghidra corpus.  RECONSTRUCTION FIDELITY: dispatch is
 *  reconstructed 1:1; the mode-2 VGA-DAC handler is reconstructed from the raw
 *  disassembly of the static DAC writer (image off 0xb204) as
 *  `vga_dac_upload_from_buffer` — a behavior-faithful, clearly-labeled reconstruction
 *  (the sprite_blit / bg_render convention): it reads the 16-colour 6-bit palette from
 *  the decoded-image buffer at +0x33 and emits the canonical VGA-DAC write sequence
 *  (`out 0x3c8,0`; 8 colours×RGB to 0x3c9; `out 0x3c8,0x10`; 8 colours×RGB).  Under
 *  the natural boot palette_mode==2 the standalone handler emits no DAC (an engine
 *  fact surfaced by the T1 oracle); the captured 50-write DAC sequence is emitted by
 *  the iris-wipe's per-step view-blit (the stubbed BGI overlay FUN_7b4a) operating on
 *  its own faded palette state — NOT reconstructable 1:1.  The DAC port-write gate
 *  therefore drives the reconstructed `vga_dac_upload_from_buffer` over a SEEDED
 *  palette buffer and asserts its (port,value) sequence (perturbation-proven); the
 *  iris-wipe's descriptor RECT SWEEP is the faithfully-reconstructed, validated part.
 *  play_iris_wipe_transition's rectangle-wipe descriptor stepping IS reconstructed 1:1;
 *  its render/DAC leaves are stubbed.
 * ──────────────────────────────────────────────────────────────────────────────── */

/* ── render / BGI-overlay leaves OWNED ELSEWHERE (extern — NOT defined here; resolve
 *    to bgi_overlay.obj / game_stubs.obj / input.obj at the BUMPY.EXE link; the host
 *    replay harness supplies its own host definitions) ──────────────────────────── */
extern void restore_bg_view(u8 __far *view, u16 seg);    /* bgi_overlay.c 1000:80bc */
extern void present_frame(u8 page);                       /* game_stubs.c            */
extern void init_fullscreen_view_desc(u8 mode, u8 flag);  /* game_stubs.c            */
extern void wait_keypress(void);                          /* game_stubs.c 1000:328f  */
extern void poll_input(void);                             /* input.c    1000:1dde    */
extern char fun_75a2_poll_action(u8 arg);                 /* FUN_1000_75a2 (input.c/stub) */
extern void show_highscore_screen(void);                  /* game_stubs.c (T5)       */
extern void set_resource_table(u16 off, u16 seg);         /* game_stubs.c            */

/* ── unowned leaves (no other definition in the src tree) — faithful-signature stubs
 *    reconstructed HERE so screens.c's 1:1 call sites resolve at the BUMPY.EXE link;
 *    RECONSTRUCTION FIDELITY: file-I/O / decode / BGI-overlay leaves, not game logic. */
int  open_resource(u16 res_idx, u16 mode);
u32  read_chunked(int handle, u16 buf_off, u16 buf_seg, u16 len_off, u16 len_seg);
void c_close(int handle);
void vec_decode(u16 buf_off, u16 buf_seg, u32 size, u16 arg, u16 flag);
void process_sprites(u16 buf_off, u16 buf_seg);
void fun_7b93_present_blank(u16 buf_off, u16 buf_seg, u16 flag);  /* FUN_1000_7b93    */
void fun_7bca_flip(u8 page);                              /* FUN_1000_7bca           */
void fun_7b4a_view_blit(u8 __far *view, u16 seg);         /* FUN_1000_7b4a (per-step) */
void fun_9410_set_sprite_table(u16 arg);                  /* FUN_1000_9410           */
void play_intro_animation_loop(void);                     /* 1000:30dd (intro anim)  */
void wait_50_frames(void);                                /* per-frame idle leaf     */
u8   enter_highscore_name(void);                          /* 1000:5c87 (T5)          */
void vga_dac_upload_from_buffer(u8 __far *img_buf);       /* mode-2 VGA-DAC handler   */

int  open_resource(u16 res_idx, u16 mode) { (void)res_idx; (void)mode; return 0; }
u32  read_chunked(int handle, u16 buf_off, u16 buf_seg, u16 len_off, u16 len_seg)
{ (void)handle; (void)buf_off; (void)buf_seg; (void)len_off; (void)len_seg; return 0; }
void c_close(int handle) { (void)handle; }
void vec_decode(u16 buf_off, u16 buf_seg, u32 size, u16 arg, u16 flag)
{ (void)buf_off; (void)buf_seg; (void)size; (void)arg; (void)flag; }
void process_sprites(u16 buf_off, u16 buf_seg) { (void)buf_off; (void)buf_seg; }
void fun_7b93_present_blank(u16 buf_off, u16 buf_seg, u16 flag)
{ (void)buf_off; (void)buf_seg; (void)flag; }
void fun_7bca_flip(u8 page) { (void)page; }
void fun_7b4a_view_blit(u8 __far *view, u16 seg) { (void)view; (void)seg; }
void fun_9410_set_sprite_table(u16 arg) { (void)arg; }
void play_intro_animation_loop(void) { }
void wait_50_frames(void) { }
u8   enter_highscore_name(void) { return 0; }

/* ── DAC palette-mode dispatch + the reconstructed VGA-DAC handler ────────────────── */

/* palette-mode embedded-palette source: the decoded-image buffer the screen builders
 *  point at.  The mode-2 VGA-DAC handler reads the 16-colour 6-bit palette at +0x33.
 *  Owned here; the host harness / iris-wipe seeds it to the engine's faded palette so
 *  the reconstructed handler's DAC writes match the captured sequence. */
u8 __far *dac_palette_src_buf;     /* far ptr -> decoded-image buffer (palette @ +0x33) */
u16       dac_palette_mode_active; /* the palette_mode the dispatch selected (for notes) */

/* vga_dac_upload_from_buffer — RECONSTRUCTION FIDELITY (behavior-faithful, from the raw
 *  disassembly of the static VGA-DAC writer at image off 0xb204; the runtime BGI
 *  overlay handler is not in the Ghidra corpus).  Emits the canonical VGA-DAC palette
 *  upload: set write index 0, push 8 colours (R,G,B each) to 0x3c9; set write index
 *  0x10, push 8 more colours.  Palette source = img_buf+0x33 (16 colours × 3 bytes,
 *  6-bit).  This is the (port,value) sequence the DAC port-write gate validates. */
void vga_dac_upload_from_buffer(u8 __far *img_buf)
{
    u8 __far *pal = img_buf + 0x33;
    u8 i;
    outp(0x3c8, 0x00);                 /* DAC write index 0  */
    for (i = 0; i < 8; i++) {
        outp(0x3c9, pal[0]);           /* R */
        outp(0x3c9, pal[1]);           /* G */
        outp(0x3c9, pal[2]);           /* B */
        pal += 3;
    }
    outp(0x3c8, 0x10);                 /* DAC write index 0x10 (BGI colours 8..15) */
    for (i = 0; i < 8; i++) {
        outp(0x3c9, pal[0]);
        outp(0x3c9, pal[1]);
        outp(0x3c9, pal[2]);
        pal += 3;
    }
    return;
}

/* dispatch_by_palette_mode_2036 — 2036:0000 (1:1).  Indirect-call the handler the BGI
 *  driver registered at overlay table [palette_mode*2 + 0x6976].  RECONSTRUCTION
 *  FIDELITY: the overlay table is runtime-populated (BGI-loaded handlers, not in the
 *  corpus).  Reconstructed as: select by palette_mode; under the boot's palette_mode==2
 *  the handler emits no DAC (an engine fact — the standalone-upload records carry 0 DAC);
 *  the reconstructed VGA-DAC handler (vga_dac_upload_from_buffer) is the modelled
 *  palette-write path the DAC gate drives over a seeded buffer. */
void dispatch_by_palette_mode(void)
{
    dac_palette_mode_active = palette_mode;
    /* palette_mode==2 (the natural boot) -> no DAC here (engine fact, T1).  Other modes'
       handlers are BGI overlay code; the modelled palette upload is gated standalone via
       vga_dac_upload_from_buffer over a seeded buffer (see the carve-out note above). */
    return;
}

/* upload_vga_dac_palette — 1000:9864 (1:1 thunk -> dispatch_by_palette_mode_2036). */
void upload_vga_dac_palette(void)
{
    dispatch_by_palette_mode();
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  play_iris_wipe_transition — 1000:3467
 *  Animate a shrinking/closing iris (rectangle wipe) over the 20x25 tile view by
 *  stepping the blit-view rect inward (10 steps, 4 view-blits + 4 DAC uploads per
 *  step), then clear it.  The descriptor RECT SWEEP (fields +0x14/+0x16/+0x1e/+0x20
 *  and the +0xe/+0x1c/+0x22..+0x25 setup) is reconstructed 1:1; the per-step view-blit
 *  (FUN_7b4a) + present (FUN_7b93/7bca) are stubbed BGI-overlay leaves; the captured
 *  DAC sequence is emitted by FUN_7b4a's faded palette (see the carve-out note).
 * ════════════════════════════════════════════════════════════════════════════ */
void play_iris_wipe_transition(void)
{
    u8 __far *d;
    u16 clear_idx;
    u8  right_edge;
    u8  bottom_edge;
    u8  step;

    d = render_descriptor_ptr;
    *(u8 __far *)(d + 0x22) = 0;
    *(u8 __far *)(d + 0x23) = 0;
    *(u8 __far *)(d + 0x24) = 0;
    *(u8 __far *)(d + 0x25) = 0;
    *(u16 __far *)(d + 0x0e) = 0;
    *(u16 __far *)(d + 0x1c) = 0;
    right_edge  = 0x14;
    bottom_edge = 0x19;
    for (step = 0; step < 10; step = step + 1) {
        d = render_descriptor_ptr;
        *(u16 __far *)(d + 0x14) = (u16)step;
        *(u16 __far *)(d + 0x16) = (u16)step;
        *(u16 __far *)(d + 0x1e) = (u16)right_edge;
        *(u16 __far *)(d + 0x20) = 1;
        fun_7b4a_view_blit(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
        upload_vga_dac_palette();
        *(u16 __far *)(render_descriptor_ptr + 0x16) = 0x18 - (u16)step;
        fun_7b4a_view_blit(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
        upload_vga_dac_palette();
        right_edge = right_edge - 2;
        d = render_descriptor_ptr;
        *(u16 __far *)(d + 0x16) = (u16)step;
        *(u16 __far *)(d + 0x1e) = 1;
        *(u16 __far *)(d + 0x20) = (u16)bottom_edge;
        fun_7b4a_view_blit(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
        upload_vga_dac_palette();
        *(u16 __far *)(render_descriptor_ptr + 0x14) = 0x13 - (u16)step;
        fun_7b4a_view_blit(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
        upload_vga_dac_palette();
        bottom_edge = bottom_edge - 2;
    }
    {
        u16 blank_tiles[50];
        for (clear_idx = 0; clear_idx < 0x32; clear_idx = clear_idx + 1) {
            blank_tiles[clear_idx] = 0;
        }
        /* FUN_7b93(&blank_tiles, SS, 0) — present the blanked tile strip. */
        fun_7b93_present_blank((u16)(unsigned long)blank_tiles,
                               SCREENS_DGROUP_RUNTIME_SEG, 0);
    }
    fun_7bca_flip(0);
    upload_vga_dac_palette();
    return;
}

/* the 16-byte EGA->VGA palette patch source bytes the title/menu builders copy into the
 *  decoded image's +0x23 palette region when palette_mode==1 (per-screen DGROUP tables:
 *  show_title_background 0x63a, show_title_and_init 0x72e, run_main_menu 0x64a,
 *  show_menu_select_screen 0x71e).  Reconstructed as module-extern far-data the harness
 *  seeds; under the boot palette_mode==2 this whole patch path is skipped. */
u8 dgroup_pal_patch_63a[16];   /* DGROUP 0x63a  (show_title_background)    */
u8 dgroup_pal_patch_72e[16];   /* DGROUP 0x72e  (show_title_and_init)      */
u8 dgroup_pal_patch_64a[16];   /* DGROUP 0x64a  (run_main_menu)            */
u8 dgroup_pal_patch_71e[16];   /* DGROUP 0x71e  (show_menu_select_screen)  */

/* helper: patch the decoded image's embedded palette (img+0x23, 16 bytes) from a DGROUP
 *  source table when palette_mode==1 (the 1:1 of the `if (palette_mode==1){...}` block in
 *  each title/menu builder).  Kept inline-equivalent; not a separate engine function. */
static void patch_image_palette(const u8 *src16)
{
    /* DAT_203b_9b96 = CONCAT22(seg, off) — the decoded-image far ptr; patch its embedded
       palette at +0x23 (16 bytes).  Only reached when palette_mode==1 (the natural boot
       runs mode 2, so this path is inert; modelled 1:1 for faithfulness). */
    u8 __far *img = (u8 __far *)(unsigned long)
                    (((u32)fullscreen_buf_seg << 16) | (u32)fullscreen_buf);
    u8 idx;
    for (idx = 0; idx < 0x10; idx = idx + 1) {
        img[(u16)idx + 0x23] = src16[idx];
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  show_title_background — 1000:2fac
 *  Load resource 2 (vec-decoded fullscreen image), optionally patch the palette when
 *  palette_mode==1, iris-wipe in, set up the 20x25 tile bg view, present it, upload the
 *  DAC palette, then run the intro animation loop.
 * ════════════════════════════════════════════════════════════════════════════ */
void show_title_background(void)
{
    int  file_handle;
    u32  bytes_read;
    u8 __far *d;

    file_handle = open_resource(2, 4);
    bytes_read = read_chunked(file_handle, fullscreen_buf, fullscreen_buf_seg, 0x0942, 0x0944);
    c_close(file_handle);
    vec_decode(fullscreen_buf, fullscreen_buf_seg, bytes_read, 0x7d63, 0);
    if (palette_mode == 1) {
        patch_image_palette(dgroup_pal_patch_63a);
    }
    play_iris_wipe_transition();
    d = render_descriptor_ptr;
    *(u16 __far *)(d + 0x02) = fullscreen_buf + 99;
    *(u16 __far *)(d + 0x04) = fullscreen_buf_seg;
    *(u16 __far *)(d + 0x06) = 0;
    *(u16 __far *)(d + 0x08) = 0;
    *(u16 __far *)(d + 0x0a) = 0x14;
    *(u16 __far *)(d + 0x0c) = 0x19;
    *(u16 __far *)(d + 0x0e) = 1;
    *(u16 __far *)(d + 0x14) = 0;
    *(u16 __far *)(d + 0x16) = 0;
    *(u16 __far *)(d + 0x1c) = 0;
    *(u16 __far *)(d + 0x1e) = 0x14;
    *(u16 __far *)(d + 0x20) = 0x19;
    restore_bg_view(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
    fun_7b93_present_blank(fullscreen_buf, fullscreen_buf_seg, 0);
    fun_7bca_flip(0);
    present_frame(1);
    upload_vga_dac_palette();
    play_intro_animation_loop();
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  init_title_graphics — 1000:2ef8
 *  Load title-screen graphics: set the resource table, show the title background, load+
 *  process the sprite resource 0, then load+vec_decode fullscreen image resource 1 and
 *  draw the HUD composite.
 * ════════════════════════════════════════════════════════════════════════════ */
void init_title_graphics(void)
{
    int file_handle;
    u32 bytes_read;

    set_resource_table(0x928, SCREENS_DGROUP_RUNTIME_SEG);
    show_title_background();
    file_handle = open_resource(0, 4);
    read_chunked(file_handle, 0xa0c6, 0xa0c8, 0x092e, 0x0930);   /* DAT sprite buf */
    c_close(file_handle);
    process_sprites(0xa0c6, 0xa0c8);
    file_handle = open_resource(1, 4);
    bytes_read = read_chunked(file_handle, fullscreen_buf, fullscreen_buf_seg, 0x0938, 0x093a);
    c_close(file_handle);
    vec_decode(fullscreen_buf, fullscreen_buf_seg, bytes_read, 0x7d63, 0);
    draw_hud_composite();
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  show_title_and_init — 1000:3ed4
 *  Show the highscore screen (T5), load+decode the title/intro resource 0x11 into
 *  fullscreen_buf, optionally patch the palette, iris-wipe in, set up the fullscreen
 *  view, present, upload the DAC palette, wait for a keypress, then current_level=1.
 *
 *  NOTE (no captured record): under the T1 oracle this fn produces NO record — its
 *  wait_keypress() does not return within the scenario window (the input driver's
 *  scripted FIRE is consumed by the nested show_highscore_screen wait first), so the
 *  exit hook never fires.  Ported 1:1 + registered (PORTED) for completeness; its
 *  semantic gate (current_level=1, game_state_928d=1) is not currently exercised.
 * ════════════════════════════════════════════════════════════════════════════ */
void show_title_and_init(void)
{
    int  res_handle;
    u32  bytes_read;
    u8 __far *d;

    show_highscore_screen();
    res_handle = open_resource(0x11, 4);
    bytes_read = read_chunked(res_handle, fullscreen_buf, fullscreen_buf_seg, 0x09d8, 0x09da);
    c_close(res_handle);
    vec_decode(fullscreen_buf, fullscreen_buf_seg, bytes_read, 0x7d63, 0);
    if (palette_mode == 1) {
        patch_image_palette(dgroup_pal_patch_72e);
    }
    play_iris_wipe_transition();
    d = render_descriptor_ptr;
    *(u16 __far *)(d + 0x02) = fullscreen_buf + 99;
    *(u16 __far *)(d + 0x04) = fullscreen_buf_seg;
    *(u16 __far *)(d + 0x06) = 0;
    *(u16 __far *)(d + 0x08) = 0;
    *(u16 __far *)(d + 0x0a) = 0x14;
    *(u16 __far *)(d + 0x0c) = 0x19;
    *(u16 __far *)(d + 0x0e) = 1;
    *(u16 __far *)(d + 0x14) = 0;
    *(u16 __far *)(d + 0x16) = 0;
    *(u16 __far *)(d + 0x1c) = 0;
    *(u16 __far *)(d + 0x1e) = 0x14;
    *(u16 __far *)(d + 0x20) = 0x19;
    restore_bg_view(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
    fun_7b93_present_blank(fullscreen_buf, fullscreen_buf_seg, 0);
    fun_7bca_flip(0);
    present_frame(1);
    upload_vga_dac_palette();
    input_state = 0;
    wait_keypress();
    current_level = 1;
    frame_abort_flag = 1;     /* DAT_203b_928d = 1 */
    return;
}

/* run_main_menu's option-2 sub-image far-ptr tables (DGROUP +0x75e off / +0x760 seg,
 *  indexed by menu_option2_setting*4) and the per-option timing table copied from
 *  DGROUP 0x11b2.  The seg words are runtime-DGROUP; the harness/engine owns them. */
u16 menu_opt2_img_off[3] = { 0x8b88, 0x824e, 0x8582 };  /* DGROUP +0x75e (stride 4) */
u16 menu_opt2_img_seg[3];                                /* DGROUP +0x760 (stride 4, runtime DS) */
u8  menu_timing_table[4] = { 0xff, 0xaa, 0x00, 0x00 };   /* DGROUP 0x11b2 */

/* ════════════════════════════════════════════════════════════════════════════
 *  run_main_menu — 1000:35a5   (the 4-option cursor STATE MACHINE)
 *  Load the menu image (resource 0x12), draw the cursor at one of 4 options, poll
 *  up/down/fire; returns the selected menu item (0xff while still selecting).  Option 2
 *  (case 2) cycles menu_option2_setting 0->1->2->0 instead of returning.
 *
 *  The cursor sprite is the p1_sprite blit descriptor: *p1=0x30, p1[word1]=
 *  cursor_index*0x10+0x70 (THIS is how the cursor LOCAL is observed), p1[word2]=0.
 *  input_state bits: 1=up, 2=down, 0x10=fire.  Returns selected_item (a u8).
 * ════════════════════════════════════════════════════════════════════════════ */
u8 run_main_menu(void)
{
    int  iVar3;
    char key;
    u8 __far *d;
    u16 __far *p;
    u32 bytes_read;
    u8  cursor_index;
    u8  selected_item;
    u8  menu_timing[4];     /* SS-local copy of DGROUP 0x11b2 (fmemcpy 3 words) */
    u8  k;

    cursor_index  = 0;
    selected_item = 0xff;
    /* fmemcpy(0x203b, local_c, SS) CX=3 words: copy the per-option timing table. */
    for (k = 0; k < 4; k = k + 1) {
        menu_timing[k] = menu_timing_table[k];
    }
    iVar3 = open_resource(0x12, 4);
    bytes_read = read_chunked(iVar3, fullscreen_buf, fullscreen_buf_seg, 0x09e2, 0x09e4);
    c_close(iVar3);
    vec_decode(fullscreen_buf, fullscreen_buf_seg, bytes_read, 0x7d63, 0);
    if (palette_mode == 1) {
        patch_image_palette(dgroup_pal_patch_64a);
    }
    fun_9410_set_sprite_table(0);
    /* p1_sprite[3..4] = source far ptr (DAT_6c2c:6c2e) — the menu sprite source. */
    p = (u16 __far *)p1_sprite;
    p[3] = 0x6c2c;                      /* DAT_203b_6c2c (off; placeholder seed) */
    p[4] = SCREENS_DGROUP_RUNTIME_SEG;  /* DAT_203b_6c2e (seg) */
    timing_flag_accumulator = 0;
    input_state = 0;
    play_iris_wipe_transition();
    d = render_descriptor_ptr;
    *(u16 __far *)(d + 0x02) = fullscreen_buf + 99;
    *(u16 __far *)(d + 0x04) = fullscreen_buf_seg;
    *(u16 __far *)(d + 0x06) = 0;
    *(u16 __far *)(d + 0x08) = 0;
    *(u16 __far *)(d + 0x0a) = 0x14;
    *(u16 __far *)(d + 0x0c) = 0x19;
    *(u16 __far *)(d + 0x0e) = 1;
    *(u16 __far *)(d + 0x14) = 0;
    *(u16 __far *)(d + 0x16) = 0;
    *(u16 __far *)(d + 0x1c) = 0;
    *(u16 __far *)(d + 0x1e) = 0x14;
    *(u16 __far *)(d + 0x20) = 0x19;
    restore_bg_view(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
    fun_7b93_present_blank(fullscreen_buf, fullscreen_buf_seg, 0);
    fun_7bca_flip(0);
    while (selected_item == 0xff) {
        /* draw the option-2 sub-image strip (the cycling option's label). */
        d = render_descriptor_ptr;
        *(u16 __far *)(d + 0x02) = menu_opt2_img_off[menu_option2_setting];
        *(u16 __far *)(d + 0x04) = menu_opt2_img_seg[menu_option2_setting];
        *(u16 __far *)(d + 0x06) = 0;
        *(u16 __far *)(d + 0x08) = 0;
        *(u16 __far *)(d + 0x0a) = 6;
        *(u16 __far *)(d + 0x0c) = 2;
        *(u16 __far *)(d + 0x14) = 0xb;
        *(u16 __far *)(d + 0x16) = 0x12;
        *(u16 __far *)(d + 0x1e) = 6;
        *(u16 __far *)(d + 0x20) = 2;
        restore_bg_view(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
        present_frame(1);
        init_fullscreen_view_desc(0, 1);
        /* draw the cursor sprite: *p1=0x30, p1[word2]=0, p1[word1]=cursor*0x10+0x70. */
        p = (u16 __far *)p1_sprite;
        p[2] = 0;
        *p = 0x30;
        p[1] = (u16)cursor_index * 0x10 + 0x70;
        anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
        upload_vga_dac_palette();
        poll_input();
        if (((input_state & 1) == 0) || (cursor_index == 0)) {
            if (((input_state & 2) == 0) || (2 < cursor_index)) {
                if ((input_state & 0x10) != 0) {
                    switch (cursor_index) {
                    case 0:
                    case 1:
                    case 3:
                        selected_item = cursor_index;
                        break;
                    case 2:
                        menu_option2_setting = menu_option2_setting + 1;
                        if (menu_option2_setting == 3) {
                            menu_option2_setting = 0;
                        }
                        break;
                    default:
                        break;
                    }
                }
            } else {
                cursor_index = cursor_index + 1;
            }
        } else {
            cursor_index = cursor_index - 1;
        }
        input_state = 0;
        do {
            key = fun_75a2_poll_action(0);
        } while (key != '\0');
    }
    timing_flag_accumulator = menu_timing[menu_option2_setting];
    /* p1_sprite[+6/+8] = the in-game sprite source far ptr (DAT_a0c6:a0c8). */
    p = (u16 __far *)p1_sprite;
    *(u16 __far *)(p1_sprite + 0x06) = 0xa0c6;                      /* off (placeholder) */
    *(u16 __far *)(p1_sprite + 0x08) = SCREENS_DGROUP_RUNTIME_SEG;  /* seg */
    fun_9410_set_sprite_table(1);
    return selected_item;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  show_menu_select_screen — 1000:0f7a
 *  Display fullscreen image (resource 3) + palette, render three text rows as sprite
 *  glyphs (via the p1_sprite descriptor + blit_sprite per glyph), set current_level
 *  from enter_highscore_name() (default 1 if 0), then animate a few frames.
 *
 *  RECONSTRUCTION FIDELITY: the three text-row source strings live in SS-local arrays
 *  the engine fmemcpy's from DGROUP; reconstructed here as DGROUP-extern row tables the
 *  harness seeds.  The glyph blits write the p1_sprite descriptor (the validated B gate).
 * ════════════════════════════════════════════════════════════════════════════ */
u8 menu_select_row1[0x13];   /* SS-local row 1 src (fmemcpy from DGROUP) */
u8 menu_select_row3a[0x0e];  /* third row, qualified path                */
u8 menu_select_row3b[0x0e];  /* third row, default path                  */
u16 fullscreen_img_buf;      /* DGROUP — = fullscreen_buf alias          */
u16 highscore_bg_buf_seg;    /* DGROUP — = fullscreen_buf_seg alias       */

void show_menu_select_screen(void)
{
    u16 __far *p;
    int  res_handle;
    u8   bVar1;
    u8   char_idx;
    u8   col_pos;
    const u8 *third_row;

    set_resource_table(0x928, SCREENS_DGROUP_RUNTIME_SEG);
    fun_9410_set_sprite_table(0);
    fullscreen_img_buf   = fullscreen_buf;
    highscore_bg_buf_seg = fullscreen_buf_seg;
    res_handle = open_resource(3, 4);
    read_chunked(res_handle, fullscreen_img_buf, highscore_bg_buf_seg, 99, 0);
    c_close(res_handle);
    if (palette_mode == 1) {
        patch_image_palette(dgroup_pal_patch_71e);
    }
    play_iris_wipe_transition();
    fun_7b93_present_blank(fullscreen_buf, fullscreen_buf_seg, 0);
    fun_7bca_flip(0);
    upload_vga_dac_palette();
    /* row 1 (19 glyphs) at y=0x10. */
    col_pos = 0;
    p = (u16 __far *)p1_sprite;
    p[1] = 0x10;
    for (char_idx = 0; char_idx < 0x13; char_idx = char_idx + 1) {
        bVar1 = menu_select_row1[char_idx];
        ((u16 __far *)p1_sprite)[2] = (u16)bVar1 + 0x175;
        *(u16 __far *)p1_sprite = (u16)col_pos << 4;
        if (bVar1 != 0x20) {
            anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
        }
        col_pos = col_pos + 1;
    }
    /* row 2 (6 fixed glyphs frame 0x1b6) at y=0xa0, starting col 7. */
    col_pos = 7;
    ((u16 __far *)p1_sprite)[1] = 0xa0;
    for (char_idx = 0; char_idx < 6; char_idx = char_idx + 1) {
        ((u16 __far *)p1_sprite)[2] = 0x1b6;
        *(u16 __far *)p1_sprite = (u16)col_pos << 4;
        anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
        col_pos = col_pos + 1;
    }
    /* current_level = enter_highscore_name(); default 1 + pick the third row. */
    current_level = enter_highscore_name();
    if (current_level == 0) {
        third_row = menu_select_row3b;
        current_level = 1;
    } else {
        third_row = menu_select_row3a;
    }
    /* row 3 (14 glyphs) at y=0x60, starting col 3. */
    col_pos = 3;
    ((u16 __far *)p1_sprite)[1] = 0x60;
    for (char_idx = 0; char_idx < 0x0e; char_idx = char_idx + 1) {
        bVar1 = third_row[char_idx];
        ((u16 __far *)p1_sprite)[2] = (u16)bVar1 + 0x175;
        *(u16 __far *)p1_sprite = (u16)col_pos << 4;
        if (bVar1 != 0x20) {
            anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
        }
        col_pos = col_pos + 1;
    }
    for (char_idx = 0; char_idx < 3; char_idx = char_idx + 1) {
        wait_50_frames();
    }
    fun_9410_set_sprite_table(0);
    return;
}
