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
