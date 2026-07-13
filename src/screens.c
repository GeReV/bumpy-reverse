/* ────────────────────────────────────────────────────────────────────────────
 *  screens.c — front-end subsystem (Phase-7 reconstruction).
 *
 *  SKELETON (Phase-7 Task 2): this TU defines ONLY the front-end module's GLOBALS —
 *  NO function bodies.  The ~20 screen/HUD functions (draw_number 1000:0816 ..
 *  wait_vretrace_thunk 1000:9864, full map in tools/screens_oracle.py FN_NAMES)
 *  remain stubbed in game_stubs.c this task; their 1:1 bodies port across Phase-7
 *  T3–T5:
 *    T3 text/number  (draw_number / draw_text_at / draw_number_sprites + the HUD
 *                     compositor draw_hud_composite),
 *    T4 title/menu/  (init_title_graphics / show_title_* / run_main_menu /
 *       highscore     show_menu_select_screen / show_highscore_screen /
 *                     render_highscore_table / enter/highscore_enter_name),
 *    T5 intro/palette (level_intro_screen / show_level_intro_screen /
 *                     play_iris_wipe_transition / wait_vretrace_thunk).
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
 *                                 gfx_overlay.c / entity.c comments (describing the
 *                                 decoded-image buffer); no symbol def — owned here.
 *      timing_flag_accumulator    DGROUP 0x854f.  grep `0x854f`/`timing_flag` finds
 *                                 nothing in src/ — new symbol.
 *      highscore_name_buf[56]     DGROUP 0x8f0 — storage for g_highscore_default_table, the
 *                                 7-entry DEFAULT high-score table (Ghidra HighScoreEntry[7];
 *                                 {char __far *name; u32 score}).  NOT a blank name-entry
 *                                 scratch — it holds the built-in scores render_highscore_table
 *                                 draws + shifts.  grep `0x8f0`/`highscore_name` finds nothing
 *                                 else — owned here.
 *      formatted_number_buf[16]   number-formatter ASCII scratch.  No other TU defines
 *                                 it — new symbol.
 *    None of these names appear in any other src/ TU (checked: game.c, level.c,
 *    input.c, player.c, player2.c, items.c, anim.c, sound.c, entity.c, gfx_overlay.c,
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
#ifdef BUMPY_PLAYABLE
#include "host/host_gfx.h"   /* host_gfx_stage_image_palette / host_gfx_upload_palette_to_dac */
#include <malloc.h>          /* _ffree — release the intro SMF buffer (play_intro_animation_loop) */
#endif

/* ── screen-state scalars ───────────────────────────────────────────────────────── */
u16 palette_mode;                  /* DGROUP 0x541d — display/palette mode + DAC dispatch idx */

/* ── render descriptors / decoded-image buffer (far ptrs) ───────────────────────── */
u8 __far *render_descriptor_ptr;   /* DGROUP 0x0574/0x0576 — view-struct far ptr (DAT_0574) */
u16 fullscreen_buf;                /* DGROUP 0x7926 — decoded-image buffer near off          */
u16 fullscreen_buf_seg;            /* DGROUP 0x7928 — decoded-image buffer seg               */

/* ── per-tick timing accumulator (run_main_menu) ────────────────────────────────── */
u8  timing_flag_accumulator;       /* DGROUP 0x854f */

/* ── highscore default table (DGROUP 0x8f0) + number-formatter scratch ───────────── */
u8  highscore_name_buf[HIGHSCORE_NAME_LEN * HIGHSCORE_TABLE_ROWS];  /* DGROUP 0x8f0 — g_highscore_default_table storage (7 entries) */
char formatted_number_buf[FORMATTED_NUMBER_LEN];                    /* number-formatter ASCII */

#ifdef BUMPY_PLAYABLE
/* ── g_highscore_default_table reconstruction (Ghidra: HighScoreEntry[7] @ DGROUP 0x8f0) ──
 *  The 7 built-in high scores.  In the original these are loader-relocated STATIC data at
 *  DGROUP 0x8f0 — each 8-byte entry { char __far *name; u32 score } holds a far ptr to an
 *  8-char name string (the strings live separately at DGROUP 0x11e6..0x121c, 9 bytes each)
 *  plus the 32-bit points.  '.' padding in a name renders blank (0x2e -> space) unless it is
 *  the freshly-inserted row.  Names + scores decoded 1:1 from the unpacked EXE @ DGROUP 0x8f0.
 *
 *  RECONSTRUCTION FIDELITY: the recon cannot statically embed the DOS-loader-relocated name
 *  far pointers into the highscore_name_buf byte storage, so the playable build POPULATES the
 *  table at startup via init_highscore_default_table (called from the playable main, exactly
 *  like init_move_scripts / init_worldmap_data / init_anim_data relocate their static tables).
 *  The default BUMPY.EXE keeps the zero-init storage — the data is documented here + in Ghidra,
 *  but the byte-faithful build does not run the initializer. */
typedef struct {
    char __far *name;    /* 0x00: name_off:u16 + name_seg:u16 (loader-relocated far ptr) */
    u32         score;   /* 0x04: score_lo:u16 + score_hi:u16 */
} HighScoreEntry;

static char s_hof_name_big_jim[9] = "BIG JIM.";   /* DGROUP 0x11e6 — 5,000,000 */
static char s_hof_name_super_jo[9] = "SUPER JO";  /* DGROUP 0x11ef — 3,000,000 */
static char s_hof_name_steve[9]    = "STEVE...";  /* DGROUP 0x11f8 — 1,000,000 */
static char s_hof_name_wiliam[9]   = "WILIAM..";  /* DGROUP 0x1201 —   200,000 */
static char s_hof_name_johnny[9]   = "JOHNNY..";  /* DGROUP 0x120a —    30,000 */
static char s_hof_name_frank[9]    = "FRANK...";  /* DGROUP 0x1213 —     4,000 */
static char s_hof_name_mike[9]     = "MIKE....";  /* DGROUP 0x121c —       500 */

void init_highscore_default_table(void)
{
    static char __far * const hof_name[HIGHSCORE_TABLE_ROWS] = {
        s_hof_name_big_jim, s_hof_name_super_jo, s_hof_name_steve, s_hof_name_wiliam,
        s_hof_name_johnny,  s_hof_name_frank,    s_hof_name_mike };
    static const u32 hof_score[HIGHSCORE_TABLE_ROWS] = {
        5000000UL, 3000000UL, 1000000UL, 200000UL, 30000UL, 4000UL, 500UL };
    HighScoreEntry __far *tbl = (HighScoreEntry __far *)highscore_name_buf;
    u8 i;

    for (i = 0; i < HIGHSCORE_TABLE_ROWS; i = i + 1) {
        tbl[i].name  = hof_name[i];
        tbl[i].score = hof_score[i];
    }
}
#endif /* BUMPY_PLAYABLE */

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
 *    blit_sprite (1000:942a) is anim.c's anim_blit_sprite_leaf.  The graphics-overlay
 *    text leaves draw_text_at forwards to — FUN_1000_9837 (1000:9837, a thunk to
 *    overlay 1ab9:1441 = SET TEXT POSITION) and FUN_1000_9804 (1000:9804, a thunk
 *    to 1ab9:13ec = DRAW STRING / draw_string_glyphs) — are self-modifying overlay
 *    code with no decomp; the default build keeps faithful-signature no-op stubs
 *    HERE, the playable build routes them to host_render.c's host text model.
 *    RECONSTRUCTION FIDELITY: these leaves preserve each call site 1:1 without
 *    re-driving the graphics overlay / Phase-0 render core; their observable output (the
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
extern u16 score_lo;                        /* game.c  0xa0d4 */
extern u16 score_hi;                        /* game.c  0xa0d6 */
extern void run_n_frames(u8 n);             /* game_stubs.c 1000:05e7 */

/* T5 forward declarations (the highscore / level-intro fns; their 1:1 bodies follow the
   T5 block below).  Declared here so the T4 builders that call them (show_title_and_init,
   show_menu_select_screen) see the correct prototypes. */
void show_highscore_screen(void);           /* 1000:5681 (T5) */
void render_highscore_table(void);          /* 1000:57e1 (T5) */
void highscore_enter_name(u8 row);          /* 1000:59d3 (T5) */
u8   enter_password(u8 col, u8 row);  /* 1000:5c87 (T5) */
void level_intro_screen(void);              /* 1000:3852 (T5) */
void show_level_intro_screen(void);         /* 1000:0d9d (T5) */
u16  draw_name_entry_cursor(u8 col, u8 row, u16 frame, char do_blit); /* 1000:5fdb */

/* render/text leaves (see header block).  FUN_80ac / blit_sprite are anim.c's
   faithful-signature stubs (the SAME engine fns); the graphics-overlay text leaves live here.
   (2026-07-02 misnomer fix: these were previously named text_clip_leaf_9837(clip_w,
   clip_h) / draw_string_glyphs_9804(x,y) — WRONG semantics.  The overlay disasm:
   1000:9837 -> 1ab9:1441 = SET TEXT POSITION (stores x/y to DGROUP 0x6942/0x6944);
   1000:9804 -> 1ab9:13ec = DRAW STRING (walks the NUL-terminated far string at
   str_seg:str_off; per char an 8-wide 1-bpp glyph from the DDFNT2.CAR font object
   bound at DGROUP 0x68a2 is blitted at the current text position, which advances
   by glyph width + font[4] per char).  The call-site arg ROUTING was already 1:1
   (9837 gets args 3/4, 9804 gets args 1/2) — only the names were wrong.) */
void anim_render_leaf_80ac(u8 __far *view);       /* FUN_1000_80ac  1000:80ac (anim.obj) */
void anim_blit_sprite_leaf(u16 obj_off, u16 obj_seg); /* blit_sprite 1000:942a (anim.obj) */
void gfx_set_text_pos_9837(u16 x, u16 y);              /* 1000:9837 -> 1ab9:1441 */
void gfx_draw_string_9804(u16 str_off, u16 str_seg);   /* 1000:9804 -> 1ab9:13ec */

#ifdef BUMPY_PLAYABLE
/* Playable build: route to the host text primitives (host_render.c) — the text
 * position pair models DGROUP 0x6942/0x6944, the string walk + glyph blit models
 * overlay 1ab9:13ec/13bc/1607 with the runtime-loaded DDFNT2.CAR font.
 * RECONSTRUCTION FIDELITY: see host_render.c (fg/bg = the engine's session text
 * colours 14/1, set by init_game_session_state via set_text_color / 1000:97c5). */
extern void host_text_set_pos(u16 x, u16 y);              /* host/host_render.c */
extern void host_text_draw_string(u16 str_off, u16 str_seg);
void gfx_set_text_pos_9837(u16 x, u16 y)            { host_text_set_pos(x, y); }
void gfx_draw_string_9804(u16 str_off, u16 str_seg) { host_text_draw_string(str_off, str_seg); }
#else
/* RECONSTRUCTION FIDELITY: graphics-overlay text leaves — faithful-signature no-op stubs
   in the default build (self-modifying overlay; no clean decomp).  draw_text_at's
   call sites are preserved 1:1; the text pixels are the graphics overlay's. */
void gfx_set_text_pos_9837(u16 x, u16 y)            { (void)x; (void)y; return; }
void gfx_draw_string_9804(u16 str_off, u16 str_seg) { (void)str_off; (void)str_seg; return; }
#endif

/* DS-stamped far-data segment for the HUD view descriptor (+0x12).  Default = the
   Ghidra static DGROUP segment 0x203b; the host harness overrides to the captured
   runtime value (mirrors anim.c's ANIM_DGROUP_RUNTIME_SEG). */
#ifndef SCREENS_DGROUP_RUNTIME_SEG
#ifdef BUMPY_PLAYABLE
extern u16 host_dgroup_seg(void);   /* host_render.c — loaded image's real DGROUP seg */
#define SCREENS_DGROUP_RUNTIME_SEG host_dgroup_seg()
#else
#define SCREENS_DGROUP_RUNTIME_SEG 0x203b
#endif
#endif

/* ════════════════════════════════════════════════════════════════════════════
 *  draw_text_at — 1000:07f0
 *  Render the NUL-terminated string at str_seg:str_off at text position (x, y):
 *  set the text position (FUN_9837 -> overlay 1ab9:1441, DGROUP 0x6942/0x6944)
 *  then draw the glyph string (FUN_9804 -> overlay 1ab9:13ec).  Disasm 07f0:
 *  PUSH [BP+0xa];PUSH [BP+8];CALL 9837;  PUSH [BP+6];PUSH [BP+4];CALL 9804 —
 *  i.e. 9837(x=arg3, y=arg4) then 9804(str_off=arg1, str_seg=arg2).
 *  (Args 3/4 were previously misnamed clip_w/clip_h — see the leaf block above.)
 * ════════════════════════════════════════════════════════════════════════════ */
void draw_text_at(u16 str_off, u16 str_seg, u16 x, u16 y)
{
    gfx_set_text_pos_9837(x, y);
    gfx_draw_string_9804(str_off, str_seg);
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  draw_number — 1000:0816
 *  Format the 32-bit value (val_hi:val_lo) as a right-justified, space-padded decimal
 *  string of `width` digits and draw it via draw_text_at; prints "OVER FLOW" if
 *  width >= 8.  Two trailing args (arg_a, arg_c) are the TEXT POSITION (x, y)
 *  forwarded to draw_text_at.  Disasm 0816: pad digit_buf[0..width-1]=' '; digit_buf[width]=0;
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
    /* The engine passes the digit string's far ptr (off, SS — a stack-local buf) plus
       the caller's (x, y) text position to draw_text_at.  Playable: pass out_str's real
       seg:off (formatted_number_buf is DGROUP-resident here, not SS — the documented
       storage-class deviation above) so the live 9804 leaf can walk it.  Default build /
       64-bit ctest host: the leaves are NOPs and the seg is unused — keep the old
       pointer-narrowing cast so the 64-bit host build stays warning-free; the
       formatted_number_buf contents remain the validated output (semantic gate). */
#ifdef BUMPY_PLAYABLE
    draw_text_at((u16)((u32)(const char __far *)out_str & 0xffffu),
                 (u16)((u32)(const char __far *)out_str >> 16), arg_a, arg_c);
#else
    draw_text_at((u16)(unsigned long)out_str, 0, arg_a, arg_c);
#endif
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
    screen_view_desc __far *d = (screen_view_desc __far *)render_descriptor_ptr;

    /* fill 1 — full descriptor (image far ptr, src 0,0, 0x14×0x19, tile @0x9d3a) */
    d->image_off = fullscreen_buf + 99;          /* image off (+99) */
    d->image_seg = fullscreen_buf_seg;           /* image seg */
    d->src_x = 0;                            /* src x */
    d->src_y = 0;                            /* src y */
    d->width = 0x14;                         /* width  */
    d->height = 0x19;                         /* height */
    d->blit_off = 0x9d3a;                       /* tile-source off */
    d->blit_seg = SCREENS_DGROUP_RUNTIME_SEG;   /* tile-source seg (DS) */
    d->dest_x = 0;                            /* dest x */
    d->dest_y = 0;                            /* dest y */
    d->sub_w = 3;                            /* sub-extent w */
    d->sub_h = 2;                            /* sub-extent h */
    d->subhandler = 0;                            /* subhandler (was mislabeled "clip x") */
    d->clip_w = 3;                            /* clip w */
    d->clip_h = 2;                            /* clip h */
    anim_render_leaf_80ac(render_descriptor_ptr);

    /* fill 2 — src x=4, tile @0x9baf */
    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->src_x = 4;
    d->blit_off = 0x9baf;
    d->blit_seg = SCREENS_DGROUP_RUNTIME_SEG;
    anim_render_leaf_80ac(render_descriptor_ptr);

    /* fill 3 — src 0,8, tile @0x9eba, sub/clip 1×4 */
    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->src_x = 0;
    d->src_y = 8;
    d->blit_off = 0x9eba;
    d->blit_seg = SCREENS_DGROUP_RUNTIME_SEG;
    d->sub_w = 1;
    d->sub_h = 4;
    d->clip_w = 1;
    d->clip_h = 4;
    anim_render_leaf_80ac(render_descriptor_ptr);

    /* fill 4 — src y=3, tile @0x9fba */
    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->src_y = 3;
    d->blit_off = 0x9fba;
    d->blit_seg = SCREENS_DGROUP_RUNTIME_SEG;
    anim_render_leaf_80ac(render_descriptor_ptr);

    /* fill 5 — src 0,0xd, tile @0x8b88, sub/clip 6×2 */
    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->src_x = 0;
    d->src_y = 0xd;
    d->blit_off = 0x8b88;
    d->blit_seg = SCREENS_DGROUP_RUNTIME_SEG;
    d->sub_w = 6;
    d->sub_h = 2;
    d->clip_w = 6;
    d->clip_h = 2;
    anim_render_leaf_80ac(render_descriptor_ptr);

    /* fill 6 — src y=0x11, tile @0x824e */
    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->src_y = 0x11;
    d->blit_off = 0x824e;
    d->blit_seg = SCREENS_DGROUP_RUNTIME_SEG;
    anim_render_leaf_80ac(render_descriptor_ptr);

    /* fill 7 — src y=0x15, tile @0x8582 */
    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->src_y = 0x15;
    d->blit_off = 0x8582;
    d->blit_seg = SCREENS_DGROUP_RUNTIME_SEG;
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
 *    wait_vretrace_thunk         1000:9864   (vsync-wait thunk -> wait_vretrace_dispatch
 *                                             @2036:0000; formerly mis-named
 *                                             upload_vga_dac_palette — see Step-5 note)
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
 *  STUBBED render-core / graphics-overlay leaves (RECONSTRUCTION FIDELITY — preserve each
 *  call site 1:1 without re-driving the Phase-0 render core; their observable output
 *  — the render_descriptor_ptr view struct + the p1_sprite blit descriptor — IS
 *  produced here and is the validated descriptor-level gate):
 *    restore_bg_view / present_frame / init_fullscreen_view_desc  (owned by
 *      gfx_overlay.c / game_stubs.c — extern, NOT redefined here);
 *    FUN_1000_7b93 / FUN_1000_7bca / FUN_1000_7b4a (the per-step view-blit) /
 *      FUN_1000_9410 (set_sprite_table_ptr trampoline) — graphics-overlay leaves, stubbed;
 *    blit_sprite (1000:942a) routed through anim.c's anim_blit_sprite_leaf (same
 *      engine fn, linked once in anim.obj — zero dup);
 *    wait_keypress / poll_input / FUN_1000_75a2 — input path (poll_input is input.c
 *      reconstructed; wait_keypress is game_stubs.c; FUN_75a2 stubbed here).  The host
 *      seeds the scripted input sequence (the captured FUN_75a2 return stream) so the
 *      menu / state-machine loops progress exactly as the engine did.
 *    show_highscore_screen / enter_password — TASK 5 (highscore); stubbed
 *      (show_highscore_screen owned by game_stubs.c — extern; enter_password
 *      stubbed here pending T5).
 *    play_intro_animation_loop / wait_50_frames — animation idle leaves, stubbed.
 *
 *  ── wait_vretrace_thunk / play_iris_wipe_transition — the vsync + DAC carve-out ────
 *  MISNOMER CORRECTION (Task 2):  `wait_vretrace_thunk` (1000:9864, formerly mis-named
 *  upload_vga_dac_palette) is a 1:1 CALLF thunk to `wait_vretrace_dispatch` (2036:0000,
 *  formerly dispatch_by_palette_mode_2036), which indirect-calls the overlay handler at
 *  table `[palette_mode*2 + 0x6976]`.  That table is RUNTIME-POPULATED by the graphics-overlay
 *  init (all-zero in the static image); for the VGA boot (palette_mode==2) the handler is
 *  2036:0015 — a VERTICAL-RETRACE (vsync) WAIT (`mov dx,0x3da; in al,dx; test al,8`), NOT
 *  a DAC upload.  RECONSTRUCTION FIDELITY: the dispatch chain is collapsed into the vsync
 *  poll (see wait_vretrace_dispatch below); 2036:0015 is dynamically-loaded overlay code,
 *  not in the Ghidra corpus.
 *
 *  The genuine DAC upload is a SEPARATE function: `vga_dac_upload_from_buffer` — a
 *  behavior-faithful, clearly-labeled reconstruction (the sprite_blit / bg_render
 *  convention) of the static DAC writer (image off 0xb204).  It reads the 16-colour
 *  6-bit palette from a decoded-image buffer at +0x33 and emits the canonical VGA-DAC
 *  write sequence (`out 0x3c8,0`; 8 colours×RGB to 0x3c9; `out 0x3c8,0x10`; 8 colours×RGB)
 *  — kept as-is (it IS a real DAC upload; only the 9864 thunk was misnamed).  The DAC
 *  port-write gate drives `vga_dac_upload_from_buffer` over a SEEDED palette buffer and
 *  asserts its (port,value) sequence (perturbation-proven).  In the playable path the
 *  real level-palette DAC upload now flows through the level-palette pipeline:
 *  load_palette (host_video.c) -> host_gfx_stage_image_palette + host_gfx_upload_palette_
 *  to_dac (host_gfx.c) -> wait_vretrace_thunk (the vsync wait).  The iris wipe calls
 *  wait_vretrace_thunk 4x/step as the wipe PACING; its descriptor RECT SWEEP is the
 *  faithfully-reconstructed, validated part, its render/present leaves stubbed.
 * ──────────────────────────────────────────────────────────────────────────────── */

/* ── render / graphics-overlay leaves OWNED ELSEWHERE (extern — NOT defined here; resolve
 *    to gfx_overlay.obj / game_stubs.obj / input.obj at the BUMPY.EXE link; the host
 *    replay harness supplies its own host definitions) ──────────────────────────── */
#ifndef BUMPY_PLAYABLE
extern void restore_bg_view(u8 __far *view, u16 seg);    /* gfx_overlay.c 1000:80bc */
#else
/* RECONSTRUCTION FIDELITY — HOST TITLE-PATH restore_bg_view SHIM
 * ─────────────────────────────────────────────────────────────────────────────
 * screens.c models the engine's restore_bg_view (1000:80bc) with its ENGINE-
 * FAITHFUL 2-arg far-pointer signature `(view, seg)` — the descriptor far ptr in
 * DX:AX, the runtime DGROUP seg in BX — and treats it as a STUBBED graphics-overlay
 * render leaf (see the "STUBBED render-core" note above): the observable title
 * present is produced by the descriptor build + present_frame(1) that follow.
 *
 * gfx_overlay.c, however, reconstructs the SAME symbol with the EXPANDED host
 * 3-arg form `restore_bg_view(u8 __huge *planes, const u8 __huge *vga_src,
 * const gfx_view_desc __far *view)` (used by entity.c / player.c / host_view.c).
 * Under __watcall (-ml) that body takes its first two far-ptr args in registers
 * and its THIRD arg ON THE STACK, cleaning it with `retf 0x0004`.  screens.c's
 * 2-arg call pushes NOTHING, so the shared `restore_bg_view_` `retf 4` pops 4
 * bytes the caller never pushed → the stack unbalances by 4 and the title fn's
 * own retf then pops a garbage frame (the observed 0824:5E38 wild jump → mode-0D
 * crash before the menu).  The body also dereferenced an UNINITIALISED stack
 * `view`, risking an OOB 4×8000-byte planar memcpy when word0e<=1.
 *
 * FIX (host build only): route screens.c's title/menu restore_bg_view(view,seg)
 * calls to a host NOP leaf with the MATCHING 2-arg convention, so the host build
 * never invokes the 3-arg gfx_overlay body with a mismatched ABI.  This preserves
 * the documented "stubbed render leaf" semantics (NOP; present via present_frame)
 * and is faithful to the engine's NOP-guard behaviour for these title views.  The
 * DEFAULT BUMPY.EXE build is unaffected (the #ifndef branch above is byte-stable;
 * that build is byte-compared, never executed, so its latent ABI mismatch is inert).
 * Recorded in docs/reconstruction-fidelity.md ("playable host" section).  */
/* host_compose_bg_view (host_render.c): compose the descriptor's source image into
 * host_framebuffer via the real 3-arg restore_bg_view.  The original Task-9 shim was
 * a NOP (it wrongly assumed present_frame alone showed the title) — that left every
 * title/menu/text-screen BLANK.  Route the 2-arg engine call here instead. */
extern void host_compose_bg_view(u8 __far *view);
static void screens_host_restore_bg_view(u8 __far *view, u16 seg)
{
    (void)seg;   /* host: planes = host_framebuffer; source taken from the descriptor */
    host_compose_bg_view(view);
}
#define restore_bg_view(view, seg) screens_host_restore_bg_view((view), (seg))
#endif
extern void present_frame(u8 page);                       /* game_stubs.c / host_video.c */
extern void init_fullscreen_view_desc(u8 mode, u8 flag);  /* game_stubs.c            */
extern void wait_keypress(void);                          /* game_stubs.c 1000:328f  */
extern void poll_input(void);                             /* input.c    1000:1dde    */
extern char fun_75a2_poll_action(u8 arg);                 /* FUN_1000_75a2 (input.c/stub) */
extern void set_resource_table(u16 off, u16 seg);         /* game_stubs.c            */

/* ── unowned leaves (no other definition in the src tree) — faithful-signature stubs
 *    reconstructed HERE so screens.c's 1:1 call sites resolve at the BUMPY.EXE link;
 *    RECONSTRUCTION FIDELITY: file-I/O / decode / graphics-overlay leaves, not game logic. */
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
void vga_dac_upload_from_buffer(u8 __far *img_buf);       /* mode-2 VGA-DAC handler   */

#ifndef BUMPY_PLAYABLE
/* Default build: faithful-signature NOP stubs (this build is byte-compared, never run).
 * The playable build provides real bodies in src/host/host_resource.c (resource loader
 * + .VEC decode), so the title/menu/text backgrounds actually load instead of composing
 * uninitialised garbage.  See host_resource.c + docs/reconstruction-fidelity.md. */
int  open_resource(u16 res_idx, u16 mode) { (void)res_idx; (void)mode; return 0; }
u32  read_chunked(int handle, u16 buf_off, u16 buf_seg, u16 len_off, u16 len_seg)
{ (void)handle; (void)buf_off; (void)buf_seg; (void)len_off; (void)len_seg; return 0; }
void c_close(int handle) { (void)handle; }
void vec_decode(u16 buf_off, u16 buf_seg, u32 size, u16 arg, u16 flag)
{ (void)buf_off; (void)buf_seg; (void)size; (void)arg; (void)flag; }
#endif /* !BUMPY_PLAYABLE */
/* process_sprites (1000:93d8) — the load-time sprite-bank frame processor: a thin
 * wrapper over sprite_proc_dispatch (1cec:2ced), which indirect-calls the per-
 * palette-mode handler (mode 2 = prepare_sprite_frames, sprite_anim.c).  Called from
 * init_title_graphics + load_graphics_resources to prepare each sprite object's
 * current frame + header (and, for ctrl&0x40 frames, expand packed pixels). */
#ifdef BUMPY_PLAYABLE
/* RECONSTRUCTION FIDELITY (playable host deviation): the host does NOT run the load-
 * time prepare_sprite_frames.  Its blit path resolves each frame per-blit via
 * sprite_prepare_frame (sprite_anim.c) against the flat halloc'd bank, and the host
 * resource loader (host_resource.c) drains the engine's sprite-bank reads rather than
 * building the engine's in-place object list at 0xa0c6 — so there is no valid obj list
 * to process here.  Kept a NOP; the faithful body is compiled into the byte-compared
 * default build.  See docs/reconstruction-fidelity.md ("playable host: process_sprites"). */
void process_sprites(u16 buf_off, u16 buf_seg) { (void)buf_off; (void)buf_seg; }
#else
/* Default (faithful, byte-compared, never executed) build: the real dispatch chain. */
void sprite_proc_dispatch(u16 obj_list_off, u16 obj_list_seg);  /* sprite_anim.c */
void process_sprites(u16 buf_off, u16 buf_seg) { sprite_proc_dispatch(buf_off, buf_seg); }
#endif
#ifdef BUMPY_PLAYABLE
/* Playable build: route to host palette-stage primitive (host_gfx.c).
 * fun_7b93 thunk (1000:7b93) dispatches into graphics overlay 1ab9:0620, which
 * copies 48 bytes from [buf_seg:buf_off]+0x33 into the per-page palette slot.
 * 'flag' is the page index.  RECONSTRUCTION FIDELITY: see host/host_gfx.h. */
void fun_7b93_present_blank(u16 buf_off, u16 buf_seg, u16 flag)
{ host_gfx_stage_image_palette(buf_off, buf_seg, flag); }
#else
void fun_7b93_present_blank(u16 buf_off, u16 buf_seg, u16 flag)
{ (void)buf_off; (void)buf_seg; (void)flag; }
#endif
#ifdef BUMPY_PLAYABLE
/* Playable build: route to host DAC-upload primitive (host_gfx.c).
 * fun_7bca thunk (1000:7bca) dispatches into graphics overlay 1ab9:0677, which
 * writes host_gfx_page_palette[page & 1] to VGA DAC ports 0x3c8/0x3c9.
 * RECONSTRUCTION FIDELITY: see host/host_gfx.h. */
void fun_7bca_flip(u8 page) { host_gfx_upload_palette_to_dac(page); }
#else
void fun_7bca_flip(u8 page) { (void)page; }
#endif
#ifdef BUMPY_PLAYABLE
/* Playable build: route to host clip/viewport primitive (host_gfx.c).
 * fun_7b4a thunk (1000:7b4a, Ghidra: gfx_set_viewport_thunk, formerly blit_view_step)
 * dispatches into graphics overlay 1ab9:0179 (gfx_init_viewport): writes CONSTANT clip
 * extents view[+0x18]=0x14, view[+0x1a]=0x19; sets gfx_write_mode_flag_a=2/b=1;
 * VGA dispatch slot 0x4dda[2]=0x0000 -> NULL -> no pixel blit.
 * Called from the iris loop (play_iris_wipe_transition) and all screen builders;
 * on VGA the visible iris = timed hold + blank-palette upload, NOT a geometric shrink.
 * RECONSTRUCTION FIDELITY: see host/host_gfx.h (host_gfx_set_viewport). */
void fun_7b4a_view_blit(u8 __far *view, u16 seg) { host_gfx_set_viewport(view, seg); }
#else
void fun_7b4a_view_blit(u8 __far *view, u16 seg) { (void)view; (void)seg; }
#endif
/* fun_9410_set_sprite_table (1000:9410 → 1cec:2dd2 set_sprite_table_ptr): select the
 * sprite draw page — cur_sprite_data := &sprite_table_base[arg] (DGROUP 0x5415;
 * [0]=a200, [1]=a000, swapped by each present).  The UI screens bracket their draws
 * with (0)…(1): the menu/highscore/pause UI draws onto slot 0 — the page the mode-11
 * sync READS FROM — which is what erases the previous cursor wholesale each frame
 * (the engine has NO cursor save-under).  Leaving this a NOP under the real
 * page-flip present re-baked the stale arrow every frame (2026-07-02 regression). */
#ifdef BUMPY_PLAYABLE
void fun_9410_set_sprite_table(u16 arg)
{
    extern void host_set_draw_page(u8 index);   /* host/host_render.c */
    host_set_draw_page((u8)arg);
}
#else
void fun_9410_set_sprite_table(u16 arg) { (void)arg; }
#endif
/* ── play_intro_animation_loop (1000:30dd) — title-screen music + wait-for-FIRE ─────
 *  Called by show_title_background (after it presents resource 2, the title image).  Loads
 *  the two intro-music resources — with the TITLE resource table active (init_title_graphics
 *  set_resource_table(0x928)): resource 4 = BUMPY.BNK (AdLib instrument bank) into
 *  fullscreen_buf, resource 5 = BUMPY.MID (the SMF song) into the screen-sprite buffer
 *  (DGROUP 0xa0c6/0xa0c8) — then, with the title image still on screen:
 *    - sound OFF (sound_device_state == 0x8000): idle-poll until ANY key (input_state != 0);
 *    - sound ON: (re)start the sequence via midi_play_sequence(song=SMF, aux=BNK, flag=1) and
 *      spin midi_get_track_count() (nonzero while the song plays; the tempo ISR advances it)
 *      polling read_input_action for FIRE (bit 0x10); when the song ends (count 0) replay it;
 *      on FIRE tear the sequencer down (midi_sound_init) and, in PC-speaker mode
 *      (sound_device_state == 0), reset timer slot 0 + strobe the speaker.
 *  copyprot_seed_src (DGROUP 0x119c) += 7 each poll — the intro's input-wait time stirs the
 *  copy-protection PRNG seed.  asm 1000:30dd verbatim.
 *
 *  NAME/CONVENTION NOTE: the screen-sprite buffer (DGROUP 0xa0c6/0xa0c8) is a far-pointer
 *  cell the reconstruction does not model; per the established init_title_graphics convention
 *  (screens.c above; host_resource.c) it is passed as the LITERAL DGROUP offsets 0xa0c6/0xa0c8
 *  (the real asm reads their CONTENTS).  0x7e18 (set_timer_slot_stack) is the stack-arg entry
 *  alias of set_timer_slot_reg (0x7e1f) — the same body; the C set_timer_slot_reg already
 *  models the stack-arg convention, so set_timer_slot_reg(1) is the faithful call. */
void play_intro_animation_loop(void)
{
    extern s16  sound_device_state;         /* player.c   DGROUP 0x689c (-0x8000 == no sound) */
    extern u8   input_state;                /* input.c    DGROUP 0x8244 */
    extern u16  copyprot_seed_src;          /* level.c    DGROUP 0x119c (copy-prot PRNG seed) */
    extern u16  read_input_action(u16 handler_idx);           /* input.c    1000:75a2 */
    extern int  midi_play_sequence(void *song, void *aux, u16 flag);  /* midi.c 1000:8977 */
    extern s16  midi_get_track_count(void);                   /* midi.c     1000:8999 */
    extern void midi_sound_init(void);                        /* midi.c     1000:89a8 */
    extern int  set_timer_slot_reg(int channel);              /* sound.c    1000:7e1f (7e18 alias) */
    extern void record_status_and_strobe_speaker(void);       /* sound.c    1000:946e */
#ifdef BUMPY_PLAYABLE
    extern u8 __far *host_intro_smf_fp;     /* host_resource.c — loaded BUMPY.MID (0 if absent/OOM) */
#endif

    u8  fire = 0;                                              /* 30ec [BP-1] */
    int file_handle;
    u16 action;

    file_handle = open_resource(4, 4);                        /* 30f0..30f5 open_resource(4,4) = BUMPY.BNK */
    read_chunked(file_handle, fullscreen_buf, fullscreen_buf_seg, 0x0956, 0x0958);  /* 30fd..3111 */
    c_close(file_handle);                                      /* 3117..3118 */

    file_handle = open_resource(5, 4);                        /* 311d..3125 open_resource(5,4) = BUMPY.MID */
    read_chunked(file_handle, 0xa0c6, 0xa0c8, 0x0960, 0x0962);  /* 312d..3141 (literal screen_sprite_buf) */
    c_close(file_handle);                                      /* 3147..3148 */

    if (sound_device_state == -0x8000) {                      /* 314d CMP [0x689c],0x8000; JZ 31bd */
        input_state = 0;                                       /* 31bd */
        while (input_state == 0) {                             /* 31d0..31d7 */
            poll_input();                                       /* 31c4 */
            copyprot_seed_src = (u16)(copyprot_seed_src + 7);  /* 31c7..31cd */
        }
    } else {                                                   /* 3155 JMP 319c (sound on) */
        /* song = resource 5 (BUMPY.MID / SMF), aux = resource 4 (BUMPY.BNK / instrument bank).
         *  Faithful: song = the literal screen_sprite_buf far ptr (0xa0c8:0xa0c6, asm 3163/3167).
         *  Playable: that literal is VGA memory, so use the host's real SMF buffer (host_resource.c
         *  loads BUMPY.MID there); if the load failed, host_intro_smf_fp is 0 and the no-hang guard
         *  below keeps input responsive.  aux = fullscreen_buf (asm 315b/315f) holds the BNK. */
        void *aux = (void *)MK_FP(fullscreen_buf_seg, fullscreen_buf);
        void *song;
#ifdef BUMPY_PLAYABLE
        song = (host_intro_smf_fp != (u8 __far *)0)
             ? (void *)host_intro_smf_fp
             : (void *)MK_FP(0xa0c8u, 0xa0c6u);
#else
        song = (void *)MK_FP(0xa0c8u, 0xa0c6u);
#endif
        while (fire == 0) {                                    /* 319c outer: replay until FIRE */
            midi_play_sequence(song, aux, 1u);                 /* 3157..316b flag = 1 (play once) */
#ifdef BUMPY_PLAYABLE
            /* Playable no-hang guard (documented deviation): if the sequence failed to load
             *  (count stays 0 — SMF absent / OOM / device off), the faithful inner loop never
             *  runs and this outer loop would tight-spin without polling input.  Poll once here
             *  so FIRE/Enter still exits.  Harmless when music loaded (count>0 → inner loop runs). */
            if (midi_get_track_count() == 0) {
                copyprot_seed_src = (u16)(copyprot_seed_src + 7);
                if ((read_input_action((u16)(copyprot_seed_src & 0xff00u)) & 0x10) != 0) {
                    fire = 1;
                }
            }
#endif
            while (midi_get_track_count() != 0 && fire == 0) {  /* 318c..319a inner: poll while playing */
                copyprot_seed_src = (u16)(copyprot_seed_src + 7);          /* 3173..3179 */
                action = read_input_action((u16)(copyprot_seed_src & 0xff00u));  /* 317c..317f (AL=0) */
                if ((action & 0x10) != 0) {                    /* 3184 TEST AL,0x10 */
                    fire = 1;                                   /* 3188 FIRE pressed */
                }
            }
        }
        midi_sound_init();                                     /* 31a5 */
        if (sound_device_state == 0) {                         /* 31a8 CMP [0x689c],0; JNZ 31d9 */
            set_timer_slot_reg(1);                             /* 31af..31b3 CALL 7e18 (stack-entry alias) */
            record_status_and_strobe_speaker();                /* 31b8 */
        }
    }
#ifdef BUMPY_PLAYABLE
    /* RECONSTRUCTION FIDELITY — intro-SMF buffer lifetime (host memory constraint, 2026-07-13).
     *  The engine reads BUMPY.MID into screen_sprite_buf (0xa0c6/0xa0c8) — a SHARED session
     *  buffer that alloc_level_buffers (1000:0492) allocates ONCE (malloc 0x5c70) and gameplay
     *  REUSES for each level's sprite bank; release_level_buffers (1000:05ad) frees it only at
     *  exit.  So the SMF is transient scratch: overwritten by the first level's sprite load, never
     *  resident during play — the engine pays NO dedicated MIDI memory.
     *
     *  The playable host does not model screen_sprite_buf (its sprite-bank reads drain to VGA /
     *  discard — process_sprites is a NOP), so play_intro's SMF was staged in a PRIVATE _fmalloc
     *  (host_resource.c).  Holding that 36 KB for the whole run — on TOP of the host-only
     *  hv_saveunder_buf (32 KB) the engine never allocates — overflowed the reduced 640 KB far
     *  heap: at level load hv_saveunder_buf's _fmalloc(32000) then returned NULL (verified: with
     *  the SMF held a 32000-byte _fmalloc fails; the SMF's own 36 KB block allocates), which NULLed
     *  host_clean_bg() and disabled the clean-bg erase/save-under paths (layer-A platform erase +
     *  deferred item erase in anim_restore_bg_view_leaf both early-return on clean==NULL, and
     *  hr_restore_under's top-row flag page-coherence guard is skipped) → platform trails, items
     *  not erased on pickup, and the EGA top-strip flag flicker returned.
     *
     *  The intro is the SMF's only reader and midi_sound_init() above has torn the sequencer down
     *  (the tempo timer slot is removed, so the INT8 slot-sweep no longer dereferences it), so we
     *  free it here.  This mirrors the engine's observable behaviour (the SMF is not resident past
     *  the intro) while respecting the host's tighter heap — the host carries neither the sprite
     *  buffer nor a persistent SMF buffer.  Lazily re-allocated by read_chunked if the intro
     *  replays.  docs/reconstruction-fidelity.md ("playable host: intro-SMF buffer"). */
    if (host_intro_smf_fp != (u8 __far *)0) {
        _ffree(host_intro_smf_fp);
        host_intro_smf_fp = (u8 __far *)0;
    }
#endif
}
/* wait_50_frames (1000:...): idle 50 (0x32) frame ticks via run_n_frames. */
void wait_50_frames(void) { run_n_frames(0x32u); }

/* ── DAC palette-mode dispatch + the reconstructed VGA-DAC handler ────────────────── */

/* palette-mode embedded-palette source: the decoded-image buffer the screen builders
 *  point at.  The mode-2 VGA-DAC handler reads the 16-colour 6-bit palette at +0x33.
 *  Owned here; the host harness / iris-wipe seeds it to the engine's faded palette so
 *  the reconstructed handler's DAC writes match the captured sequence. */
u8 __far *dac_palette_src_buf;     /* far ptr -> decoded-image buffer (palette @ +0x33) */
u16       dac_palette_mode_active; /* the palette_mode the dispatch selected (for notes) */

/* vga_dac_upload_from_buffer — RECONSTRUCTION FIDELITY (behavior-faithful, from the raw
 *  disassembly of the static VGA-DAC writer at image off 0xb204; the runtime graphics
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
    outp(0x3c8, 0x10);                 /* DAC write index 0x10 (graphics-overlay colours 8..15) */
    for (i = 0; i < 8; i++) {
        outp(0x3c9, pal[0]);
        outp(0x3c9, pal[1]);
        outp(0x3c9, pal[2]);
        pal += 3;
    }
    return;
}

/* wait_vretrace_dispatch — 2036:0000 (formerly mis-named dispatch_by_palette_mode_2036).
 *  The engine thunk 1000:9864 CALLFs 2036:0000, which indirect-calls the graphics overlay
 *  handler at table [palette_mode*2 + 0x6976].  For the VGA boot (palette_mode==2) that
 *  handler is 2036:0015 — a VERTICAL-RETRACE (vsync) WAIT (`mov dx,0x3da; in al,dx;
 *  test al,8`: wait for the retrace to START, then to END).  It is NOT a DAC upload:
 *  the old names (upload_vga_dac_palette / dispatch_by_palette_mode) were MISNOMERS.
 *  The genuine DAC upload happens earlier via fun_7bca_flip -> host_gfx_upload_palette_
 *  to_dac (and the standalone modelled writer vga_dac_upload_from_buffer).
 *
 *  RECONSTRUCTION FIDELITY: the overlay handler table at 0x6976 is runtime-populated by
 *  the graphics overlay (all-zero in the static image) and 2036:0015 is dynamically-loaded
 *  overlay code — NOT in the Ghidra corpus.  The mode-2 handler chain
 *  (9864 -> 2036:0000 -> table[2] = 2036:0015) is collapsed here into the vsync poll the
 *  playable build needs; the default (byte-compared, never-run) build keeps its verbatim
 *  NOP.  Recorded in docs/reconstruction-fidelity.md + faithfulness-gap-audit.md §1. */
void wait_vretrace_dispatch(void)
{
#ifdef BUMPY_PLAYABLE
    /* 2036:0015 — poll VGA Input Status #1 (0x3da) bit 3: wait for the vertical
     * retrace to START (bit goes high), then to END (bit goes low). */
    while ((inp(0x3dau) & 0x08u) == 0u) { ; }
    while ((inp(0x3dau) & 0x08u) != 0u) { ; }
#else
    dac_palette_mode_active = palette_mode;
    return;
#endif
}

/* wait_vretrace_thunk — 1000:9864 (1:1 CALLF thunk -> wait_vretrace_dispatch @2036:0000).
 *  Formerly mis-named upload_vga_dac_palette.  This is the engine's vsync-wait entry; the
 *  iris wipe calls it 4x/step as the wipe PACING, and load_palette calls it at its tail
 *  after staging+uploading the level palette. */
void wait_vretrace_thunk(void)
{
    wait_vretrace_dispatch();
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  play_iris_wipe_transition — 1000:3467
 *  Animate a shrinking/closing iris (rectangle wipe) over the 20x25 tile view by
 *  stepping the blit-view rect inward (10 steps, 4 view-blits + 4 DAC uploads per
 *  step), then clear it.  The descriptor RECT SWEEP (fields +0x14/+0x16/+0x1e/+0x20
 *  and the +0xe/+0x1c/+0x22..+0x25 setup) is reconstructed 1:1; the per-step view-blit
 *  (FUN_7b4a) + present (FUN_7b93/7bca) are stubbed graphics-overlay leaves; the captured
 *  DAC sequence is emitted by FUN_7b4a's faded palette (see the carve-out note).
 * ════════════════════════════════════════════════════════════════════════════ */
void play_iris_wipe_transition(void)
{
    screen_view_desc __far *d;
    u16 clear_idx;
    u8  right_edge;
    u8  bottom_edge;
    u8  step;

    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->status[0] = 0;
    d->status[1] = 0;
    d->status[2] = 0;
    d->status[3] = 0;
    d->flag = 0;
    d->subhandler = 0;
    right_edge  = 0x14;
    bottom_edge = 0x19;
    for (step = 0; step < 10; step = step + 1) {
        d = (screen_view_desc __far *)render_descriptor_ptr;
        d->dest_x = (u16)step;
        d->dest_y = (u16)step;
        d->clip_w = (u16)right_edge;
        d->clip_h = 1;
        fun_7b4a_view_blit(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
        wait_vretrace_thunk();
        *(u16 __far *)(render_descriptor_ptr + 0x16) = 0x18 - (u16)step;
        fun_7b4a_view_blit(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
        wait_vretrace_thunk();
        right_edge = right_edge - 2;
        d = (screen_view_desc __far *)render_descriptor_ptr;
        d->dest_y = (u16)step;
        d->clip_w = 1;
        d->clip_h = (u16)bottom_edge;
        fun_7b4a_view_blit(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
        wait_vretrace_thunk();
        *(u16 __far *)(render_descriptor_ptr + 0x14) = 0x13 - (u16)step;
        fun_7b4a_view_blit(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
        wait_vretrace_thunk();
        bottom_edge = bottom_edge - 2;
    }
    {
        u16 blank_tiles[50];
        for (clear_idx = 0; clear_idx < 0x32; clear_idx = clear_idx + 1) {
            blank_tiles[clear_idx] = 0;
        }
        /* FUN_7b93(&blank_tiles, SS, 0) — present the blanked tile strip.
           RECONSTRUCTION FIDELITY: the original passes the STACK segment (unaff_SS)
           for this stack-local buffer; passing DGROUP here read a garbage palette
           (blank_tiles' SS offset interpreted in DGROUP) so the iris fade staged a
           non-black palette.  Use blank_tiles' real far segment/offset. */
        fun_7b93_present_blank(FP_OFF(blank_tiles), FP_SEG(blank_tiles), 0);
    }
    fun_7bca_flip(0);
    wait_vretrace_thunk();
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

#ifdef BUMPY_PLAYABLE
/* level_palette_ptr_table (DGROUP 0x6e6): the per-level far-ptr array level_intro_screen
 *  (the overworld map) indexes by current_level (1..9) to reach each world's 16-byte AC
 *  palette; defined later in this file, forward-declared here so the init below can fill it.
 *  Kept INSIDE the BUMPY_PLAYABLE guard so the default BUMPY.EXE translation unit is
 *  unchanged (an earlier extern reference would reorder its DGROUP BSS and shift the image). */
extern u16 level_palette_ptr_table[16 * 2];

/* init_ega_palette_patch_tables — populate the EGA AC-index palette tables the playable
 *  build needs when palette_mode==1:
 *   (a) the four per-screen title/menu dgroup_pal_patch_* tables (DGROUP 0x63a/0x72e/0x64a/
 *       0x71e), copied over img+0x23 by the title/menu builders; and
 *   (b) the nine per-level/world overworld tables (DGROUP 0x65a..0x6da, stride 0x10), wired
 *       into level_palette_ptr_table[1..9] so level_intro_screen's img+0x23 patch resolves.
 *  RECONSTRUCTION FIDELITY: every byte below is the binary's own static DGROUP data
 *  (extracted by tools/extract/ega_palette_patch.py — NOT invented).  The original stores the
 *  per-level tables as static data and points level_palette_ptr_table at them at link time
 *  (seg 0x103b); the reconstruction can't use the original link-time segment, so — exactly
 *  like init_worldmap_data / init_password_table — it RELOCATES the pointers to the
 *  reconstructed storage at startup via FP_OFF/FP_SEG.  Same loader-relocated-static-data
 *  constraint; the default BUMPY.EXE keeps the zero-init storage (inert there — the boot
 *  palette_mode is 2, not 1). */
void init_ega_palette_patch_tables(void)
{
    static const u8 s_63a[16] = { 0x00u,0x00u,0x00u,0x04u,0x06u,0x0cu,0x0cu,0x0bu,
                                  0x09u,0x04u,0x06u,0x0cu,0x0eu,0x0eu,0x0fu,0x0fu };
    static const u8 s_72e[16] = { 0x00u,0x00u,0x00u,0x04u,0x06u,0x0cu,0x0cu,0x0bu,
                                  0x09u,0x04u,0x06u,0x0cu,0x0eu,0x0eu,0x0fu,0x0fu };
    static const u8 s_64a[16] = { 0x00u,0x00u,0x00u,0x00u,0x00u,0x00u,0x00u,0x02u,
                                  0x0au,0x04u,0x06u,0x06u,0x0cu,0x0eu,0x0fu,0x0fu };
    static const u8 s_71e[16] = { 0x00u,0x01u,0x08u,0x08u,0x07u,0x0bu,0x0bu,0x09u,
                                  0x01u,0x09u,0x04u,0x04u,0x06u,0x0cu,0x0cu,0x01u };
    /* level_ega_ac_tables[L-1] = the AC palette level_palette_ptr_table[L] points at, for
     * current_level L = 1..9 (DGROUP 0x65a..0x6da).  Level 1 (0x65a) equals copyprot_palette_src. */
    static const u8 level_ega_ac_tables[9][16] = {
        { 0x00u,0x00u,0x01u,0x09u,0x0bu,0x05u,0x04u,0x06u,0x0cu,0x02u,0x0au,0x09u,0x0bu,0x05u,0x07u,0x00u }, /* L1 @0x65a */
        { 0x00u,0x00u,0x06u,0x0eu,0x07u,0x08u,0x04u,0x06u,0x0bu,0x02u,0x0au,0x01u,0x09u,0x07u,0x0bu,0x00u }, /* L2 @0x66a */
        { 0x00u,0x00u,0x04u,0x0bu,0x07u,0x08u,0x04u,0x06u,0x0cu,0x02u,0x0au,0x01u,0x09u,0x07u,0x07u,0x00u }, /* L3 @0x67a */
        { 0x00u,0x00u,0x06u,0x0eu,0x0bu,0x01u,0x0cu,0x06u,0x0cu,0x02u,0x0au,0x09u,0x0bu,0x09u,0x07u,0x00u }, /* L4 @0x68a */
        { 0x00u,0x08u,0x04u,0x07u,0x09u,0x01u,0x04u,0x06u,0x0cu,0x02u,0x0au,0x01u,0x09u,0x09u,0x07u,0x00u }, /* L5 @0x69a */
        { 0x00u,0x01u,0x09u,0x08u,0x07u,0x09u,0x04u,0x06u,0x0cu,0x02u,0x0au,0x09u,0x0bu,0x0bu,0x07u,0x00u }, /* L6 @0x6aa */
        { 0x00u,0x08u,0x09u,0x07u,0x01u,0x09u,0x04u,0x06u,0x0cu,0x02u,0x0au,0x08u,0x01u,0x0bu,0x07u,0x00u }, /* L7 @0x6ba */
        { 0x00u,0x04u,0x06u,0x0cu,0x0cu,0x01u,0x04u,0x06u,0x0cu,0x02u,0x0au,0x09u,0x09u,0x09u,0x07u,0x00u }, /* L8 @0x6ca */
        { 0x00u,0x07u,0x09u,0x09u,0x0du,0x00u,0x04u,0x06u,0x09u,0x02u,0x0au,0x07u,0x09u,0x05u,0x07u,0x00u }, /* L9 @0x6da */
    };
    u8 i;
    for (i = 0u; i < 16u; i++) {
        dgroup_pal_patch_63a[i] = s_63a[i];
        dgroup_pal_patch_72e[i] = s_72e[i];
        dgroup_pal_patch_64a[i] = s_64a[i];
        dgroup_pal_patch_71e[i] = s_71e[i];
    }
    /* Relocate the per-level far pointers to the reconstructed storage (as {off,seg}, the
     * same stride-2 layout password_table uses).  level_intro_screen reads current_level
     * 1..9; entry 0 is never a real level (the original's entry 0 overlaps the tail of the
     * last AC table) — point it at L1 defensively so an out-of-range read can't deref NULL. */
    level_palette_ptr_table[0] = FP_OFF((const u8 __far *)level_ega_ac_tables[0]);
    level_palette_ptr_table[1] = FP_SEG((const u8 __far *)level_ega_ac_tables[0]);
    for (i = 1u; i <= 9u; i++) {
        level_palette_ptr_table[(u16)i * 2u]      = FP_OFF((const u8 __far *)level_ega_ac_tables[i - 1u]);
        level_palette_ptr_table[(u16)i * 2u + 1u] = FP_SEG((const u8 __far *)level_ega_ac_tables[i - 1u]);
    }
}
#endif /* BUMPY_PLAYABLE */

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
    screen_view_desc __far *d;

    file_handle = open_resource(2, 4);
    bytes_read = read_chunked(file_handle, fullscreen_buf, fullscreen_buf_seg, 0x0942, 0x0944);
    c_close(file_handle);
    vec_decode(fullscreen_buf, fullscreen_buf_seg, bytes_read, 0x7d63, 0);
    if (palette_mode == 1) {
        patch_image_palette(dgroup_pal_patch_63a);
    }
    play_iris_wipe_transition();
    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->image_off = fullscreen_buf + 99;
    d->image_seg = fullscreen_buf_seg;
    d->src_x = 0;
    d->src_y = 0;
    d->width = 0x14;
    d->height = 0x19;
    d->flag = 1;
    d->dest_x = 0;
    d->dest_y = 0;
    d->subhandler = 0;
    d->clip_w = 0x14;
    d->clip_h = 0x19;
    restore_bg_view(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
    fun_7b93_present_blank(fullscreen_buf, fullscreen_buf_seg, 0);
    fun_7bca_flip(0);
    present_frame(1);
    wait_vretrace_thunk();
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
    screen_view_desc __far *d;

    show_highscore_screen();
    res_handle = open_resource(0x11, 4);
    bytes_read = read_chunked(res_handle, fullscreen_buf, fullscreen_buf_seg, 0x09d8, 0x09da);
    c_close(res_handle);
    vec_decode(fullscreen_buf, fullscreen_buf_seg, bytes_read, 0x7d63, 0);
    if (palette_mode == 1) {
        patch_image_palette(dgroup_pal_patch_72e);
    }
    play_iris_wipe_transition();
    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->image_off = fullscreen_buf + 99;
    d->image_seg = fullscreen_buf_seg;
    d->src_x = 0;
    d->src_y = 0;
    d->width = 0x14;
    d->height = 0x19;
    d->flag = 1;
    d->dest_x = 0;
    d->dest_y = 0;
    d->subhandler = 0;
    d->clip_w = 0x14;
    d->clip_h = 0x19;
    restore_bg_view(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
    fun_7b93_present_blank(fullscreen_buf, fullscreen_buf_seg, 0);
    fun_7bca_flip(0);
    present_frame(1);
    wait_vretrace_thunk();
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
    screen_view_desc __far *d;
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
    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->image_off = fullscreen_buf + 99;
    d->image_seg = fullscreen_buf_seg;
    d->src_x = 0;
    d->src_y = 0;
    d->width = 0x14;
    d->height = 0x19;
    d->flag = 1;
    d->dest_x = 0;
    d->dest_y = 0;
    d->subhandler = 0;
    d->clip_w = 0x14;
    d->clip_h = 0x19;
    restore_bg_view(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
    fun_7b93_present_blank(fullscreen_buf, fullscreen_buf_seg, 0);
    fun_7bca_flip(0);
    while (selected_item == 0xff) {
        /* draw the option-2 sub-image strip (the cycling option's label). */
        d = (screen_view_desc __far *)render_descriptor_ptr;
        d->image_off = menu_opt2_img_off[menu_option2_setting];
        d->image_seg = menu_opt2_img_seg[menu_option2_setting];
        d->src_x = 0;
        d->src_y = 0;
        d->width = 6;
        d->height = 2;
        d->dest_x = 0xb;
        d->dest_y = 0x12;
        d->clip_w = 6;
        d->clip_h = 2;
        restore_bg_view(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
        present_frame(1);
        init_fullscreen_view_desc(0, 1);
        /* draw the cursor sprite: *p1=0x30, p1[word2]=0, p1[word1]=cursor*0x10+0x70. */
        p = (u16 __far *)p1_sprite;
        p[2] = 0;
        *p = 0x30;
        p[1] = (u16)cursor_index * 0x10 + 0x70;
        anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
#ifdef BUMPY_PLAYABLE
        /* Host: the engine's p1_sprite obj has no DGROUP backing bound pre-level, so
           anim_blit_sprite_leaf NOPs here.  Blit the cursor arrow (FLECHE frame 0) from
           the dedicated host cursor bank at the same (x,y) instead. */
        {
            extern void host_blit_cursor(u16 x, u16 y);   /* host_render.c */
            host_blit_cursor(0x30u, (u16)((u16)cursor_index * 0x10u + 0x70u));
        }
#endif /* BUMPY_PLAYABLE */
        wait_vretrace_thunk();
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
 *  from enter_password() (default 1 if 0), then animate a few frames.
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

#ifdef BUMPY_PLAYABLE
    /* RECONSTRUCTION FIDELITY — menu-select text rows.  The engine fmemcpy's three
     * loader-relocated far pointers (DGROUP 0x11a2/0x11a6/0x11aa → the strings at
     * DGROUP 0x12f5/0x1309/0x1318) into SS-locals, then draws each char as a sprite
     * glyph (frame = char + GLYPH_FRAME_BIAS).  The reconstruction keeps the row buffers as module
     * globals and fills them here from the SAME DGROUP string literals — the relocated
     * far pointers can't be statically embedded (same rationale as
     * init_highscore_default_table).  Without this row1 stayed zero-filled and drew 19×
     * the char-0 glyph (an orb) instead of "ENTER YOUR PASSWORD". */
    {
        static const char r1[0x13]  = "ENTER YOUR PASSWORD";   /* DGROUP 0x12f5 */
        static const char r3a[0x0e] = " PASSWORD OK  ";         /* DGROUP 0x1309 (match) */
        static const char r3b[0x0e] = "PASSWORD ERROR";         /* DGROUP 0x1318 (no match) */
        u8 i;
        for (i = 0; i < 0x13u; i++) { menu_select_row1[i]  = (u8)r1[i]; }
        for (i = 0; i < 0x0eu; i++) { menu_select_row3a[i] = (u8)r3a[i]; }
        for (i = 0; i < 0x0eu; i++) { menu_select_row3b[i] = (u8)r3b[i]; }
    }
#endif
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
    wait_vretrace_thunk();
    /* show_menu_select composes NO background (unlike show_highscore_screen's
     * restore_bg_view): it draws "ENTER YOUR PASSWORD" + the entry field as sprite
     * glyphs over a screen that is BLACK by the time it draws.  That clean-black is
     * the engine's own doing: play_iris_wipe_transition() above drives the geometric
     * rect-fill iris (gfx_init_viewport 1ab9:0179 → the 1ab9:0000 secondary dispatcher
     * → 0x4dcc[0]=1ab9:002b solid black rect fill, now reconstructed in
     * host_gfx_set_viewport), whose shrinking outline rings tile the whole 20×25 view
     * to black.  No separate host clear is needed — capture-confirmed 2026-07-05 that
     * the iris alone clears the code screen (nonzero VGA bytes 1799, vs 36921 for the
     * old un-cleared menu bleed).  (An earlier reconstruction inserted an invented
     * host_vga_clear_display() here on the mistaken premise that the iris was a
     * palette-only blank and fun_7b4a a null no-op; the real primitive supersedes it.) */
    /* row 1 (19 glyphs) at y=0x10. */
    col_pos = 0;
    p = (u16 __far *)p1_sprite;
    p[1] = 0x10;
    for (char_idx = 0; char_idx < 0x13; char_idx = char_idx + 1) {
        bVar1 = menu_select_row1[char_idx];
        ((u16 __far *)p1_sprite)[2] = (u16)bVar1 + GLYPH_FRAME_BIAS;
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
    /* current_level = enter_password(col=0xa, row=7); default 1 + pick the third
       row.  Disasm 0f7a@112d: PUSH 7; PUSH 0xa; CALL 5c87 (C right-to-left → col=0xa,
       row=7).  enter_password is now ported (T5); its captured return is the
       table-match index. */
    current_level = enter_password(0xa, 7);
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
        ((u16 __far *)p1_sprite)[2] = (u16)bVar1 + GLYPH_FRAME_BIAS;
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

/* ════════════════════════════════════════════════════════════════════════════
 *  Phase-7 Task 5 — the highscore screens (incl. the interactive name-entry state
 *  machine) + the per-level intro screen.  Completes the front-end port.
 *
 *  Ported 1:1 from the live Ghidra BumpyDecomp + raw disassembly (decompiled fresh
 *  2026-06):
 *    show_highscore_screen      1000:5681
 *    render_highscore_table     1000:57e1
 *    highscore_enter_name       1000:59d3   (the 8-char table-row name entry SM)
 *    enter_password       1000:5c87   (the 6-char menu-select name entry SM)
 *    draw_name_entry_cursor     1000:5fdb   (the shared cursor draw helper)
 *    level_intro_screen         1000:3852   (the per-level intro + move loop)
 *    show_level_intro_screen    1000:0d9d   (the level-name sprite-glyph screen)
 *  The Turbo C stack-check prologue (`CMP [0x6b4c],SP; CALL ab83`) of each original
 *  is the compiler-emitted stack probe — NOT game logic — intentionally OMITTED (the
 *  documented player/items/anim/sound/T3/T4 convention).
 *
 *  ── NAME-ENTRY STATE MACHINES (the SEMANTIC gate) ──────────────────────────────
 *  highscore_enter_name (59d3) and enter_password (5c87) are the interactive
 *  text-input loops: each polls the engine's ONE input primitive FUN_1000_75a2
 *  (DIRECTLY, not via poll_input), and on the action bits 1=left / 2=right (cycle the
 *  current letter through 0x1ad..0x1cf) / 4=prev char / 8=next char / 0x10=done builds
 *  the name buffer one cursor position at a time, drawing the blinking cursor via
 *  draw_name_entry_cursor (5fdb).  The host replays the captured FUN_75a2 return stream
 *  (the v3 input script) through fun_75a2_poll_action in FIFO lockstep — the engine's
 *  real input path — so the cursor walk + name-buffer edits reproduce host-side.  The
 *  validated SEMANTIC output is the screen-global SCRSNAP (incl. the 0x8f0 row-0 entry)
 *  + the AX return (enter_password's table-match index / void AL leftover).
 *    - highscore_enter_name (59d3): edits the 8-char name string the table row points
 *      at (highscore_entry_ptr = 0x203b:(row*8+0x8f0); the string is at *that ptr).
 *    - enter_password (5c87): edits a 6-char SS-LOCAL buffer (fmemcpy'd from
 *      DGROUP 0x256a), then on 0x10 compares it against the 8-entry name table at
 *      DGROUP 0x135c and returns the matched index + 2 (or 0).  RECONSTRUCTION
 *      FIDELITY: the SS-local buffer is reconstructed as a module-static array so the
 *      edit + compare run host-side; same bytes, same algorithm (the validated output
 *      is the AX return, captured 0 = no match under the boot's empty name table).
 *
 *  ── level_intro_screen (3852) — gameplay leaves STUBBED ───────────────────────────
 *  Loads + vec_decodes the per-level border image (resource current_level+7), installs
 *  the level palette (palette_mode==1 path), iris-wipes in, draws the score HUD
 *  (draw_number — T3) + Bumpy at his start pos, then runs an input-wait loop: the
 *  direction bits dispatch to the p1_move_step_* gameplay steppers, fire (0x10) ->
 *  intro_start_level (which begins the level), a quit key -> game_state=0xff.  The
 *  gameplay/render leaves it calls (p1_move_step_*, render_p1_view, draw_p1_sprite,
 *  draw_icon_row, play_anim_sequence, compute_move_descriptor_ptr, intro_start_level,
 *  …) are RECONSTRUCTION-FIDELITY stubbed/extern (owned by player/level/entity modules
 *  or stubbed here) — the per-level move physics is a separate subsystem; this screen's
 *  faithful, validated output is the screen-global SCRSNAP (timing saved/restored,
 *  input_state cleared, game_state, palette).  intro_start_level's stub returns 1 so the
 *  fire branch exits the loop (it returns did_start=1 when the level starts).
 *
 *  ── SEEDED resource load / STUBBED render leaves (as T4) ──────────────────────────
 *  open_resource / read_chunked / c_close / vec_decode are SEEDED faithful-signature
 *  stubs (already defined in the T4 block); the decoded image is host-seeded.  The
 *  per-screen builders' render-core leaves (restore_bg_view, FUN_7b93/7bca, present,
 *  blit_sprite via anim_blit_sprite_leaf, FUN_7b4a) stay stubbed; their observable
 *  output — the render_descriptor_ptr view struct + the p1_sprite descriptor — IS
 *  produced here and is the validated DESCRIPTOR-LEVEL gate.
 * ──────────────────────────────────────────────────────────────────────────────── */

/* ── leaves / globals level_intro_screen reaches (owned elsewhere — extern; resolve at
 *    the BUMPY.EXE link, host-defined in the ctest).  These are the gameplay/render
 *    steppers + the player-state globals; NOT reconstructed here (separate subsystem). */
extern void render_p1_view(void);                 /* game_stubs.c            */
extern void draw_p1_sprite(void);                 /* game_stubs.c            */
extern void p1_update_grid_cell(void);            /* game_stubs.c            */
extern void p1_advance_grid_history(void);        /* game_stubs.c            */
extern u8   get_key_state(u8 scancode);           /* input.c   1000:7ab4    */
extern u16  p1_pixel_x;                            /* entity.c / level state  */
extern u16  p1_pixel_y;                            /* "                       */
extern u16  p1_start_x;                            /* level.c — DGROUP 0x791c word */
extern u16  p1_start_y;                            /* level.c — DGROUP 0x791e word */
extern u16  p1_move_anim;                          /* player.c — WORD @0x824a  */
extern u8   current_entity_index;                  /* level.c   0x...         */

/* ── overworld/level-intro movement + animation subsystem (1000:3a88..3cf7) ────────
 *    Reconstructed 1:1 from the Ghidra decomp.  level_intro_screen (the overworld map
 *    navigation screen) auto-walks Bumpy node-to-node via these move-descriptor steps,
 *    animates the map nodes (play_anim_sequence), and starts a level (intro_start_level).
 *    Data tables (move_descriptor_table / anim_coord_table_ptr) are loaded per-level by
 *    start_level (level.c) from the binary's per-level pointer tables. */
extern void erase_p1_view(void);                  /* player.c 1000:... */
extern void rotate_timing_flags_and_wait(void);   /* host_video.c / game_stubs */
extern s16  p1_grid_x_new;                         /* player.c */
extern s16  p1_grid_y_new;                         /* player.c */
extern u8 __far *move_descriptor_table;            /* level.c 0x8246 — per-level entry list */
extern u8   saved_entity_index;                    /* defined below (DGROUP) */

void compute_move_descriptor_ptr(void);
void p1_move_step_up(void);
void p1_move_step_down(void);
void p1_move_step_left(void);
void p1_move_step_right(void);
void p1_animate_frame(void);
void draw_anim_seq_frame(void);
void p1_compute_grid_from_pixel(void);
void play_anim_sequence(void);
void draw_icon_row(void);
u8   intro_start_level(void);

/* runtime move-descriptor cursor (DGROUP 0x9baa/0x9bac) — far ptr into the per-level
 * move_descriptor_table for entity current_entity_index (9-byte stride). */
u16 move_descriptor_ptr;        /* DGROUP 0x9baa — offset */
u16 move_descriptor_ptr_seg;    /* DGROUP 0x9bac — segment */
/* per-level anim-coordinate table far ptr (DGROUP 0x8554/0x8556), set by start_level. */
u16 anim_coord_table_ptr;       /* DGROUP 0x8554 — offset of the {x,y} coord table */
u16 anim_coord_table_seg;       /* DGROUP 0x8556 — segment */

/* compute_move_descriptor_ptr (1000:3a88): far ptr to move_descriptor_table entry for
 * current_entity_index (9-byte stride). */
void compute_move_descriptor_ptr(void)
{
    move_descriptor_ptr     = (u16)((u16)FP_OFF(move_descriptor_table)
                                    + (u16)current_entity_index * 9u);
    move_descriptor_ptr_seg = (u16)FP_SEG(move_descriptor_table);
}

/* p1_move_step_{up,down,left,right} (1000:3ab2/3b0f/3b6c/3bc9): move-descriptor fields
 * 1/3/5/7 hold the next-node anim id; if nonzero, step Bumpy 4px N times (N = field
 * 2/4/6/8 >> 2) toward that direction, re-animating each step.  The loop count/anim id
 * are read from the ORIGINAL descriptor (saved before compute_move_descriptor_ptr
 * re-points move_descriptor_ptr to the new node). */
void p1_move_step_up(void)
{
    u8 __far *desc = (u8 __far *)MK_FP(move_descriptor_ptr_seg, move_descriptor_ptr);
    u8 step_count = desc[1];
    if (step_count != 0) {
        current_entity_index = step_count;
        compute_move_descriptor_ptr();
        for (step_count = (u8)(desc[2] >> 2); step_count != 0; step_count--) {
            p1_pixel_y = p1_pixel_y - 4;
            p1_animate_frame();
        }
    }
}

void p1_move_step_down(void)
{
    u8 __far *desc = (u8 __far *)MK_FP(move_descriptor_ptr_seg, move_descriptor_ptr);
    u8 step_count = desc[3];
    if (step_count != 0) {
        current_entity_index = step_count;
        compute_move_descriptor_ptr();
        for (step_count = (u8)(desc[4] >> 2); step_count != 0; step_count--) {
            p1_pixel_y = p1_pixel_y + 4;
            p1_animate_frame();
        }
    }
}

void p1_move_step_left(void)
{
    u8 __far *desc = (u8 __far *)MK_FP(move_descriptor_ptr_seg, move_descriptor_ptr);
    u8 step_count = desc[5];
    if (step_count != 0) {
        current_entity_index = step_count;
        compute_move_descriptor_ptr();
        for (step_count = (u8)(desc[6] >> 2); step_count != 0; step_count--) {
            p1_pixel_x = p1_pixel_x - 4;
            p1_animate_frame();
        }
    }
}

void p1_move_step_right(void)
{
    u8 __far *desc = (u8 __far *)MK_FP(move_descriptor_ptr_seg, move_descriptor_ptr);
    u8 step_count = desc[7];
    if (step_count != 0) {
        current_entity_index = step_count;
        compute_move_descriptor_ptr();
        for (step_count = (u8)(desc[8] >> 2); step_count != 0; step_count--) {
            p1_pixel_x = p1_pixel_x + 4;
            p1_animate_frame();
        }
    }
}

/* p1_animate_frame (1000:3c26): advance Bumpy one animation frame on the map. */
void p1_animate_frame(void)
{
    p1_advance_grid_history();
    p1_update_grid_cell();
    erase_p1_view();
    render_p1_view();
    draw_p1_sprite();
    present_frame(1);
    rotate_timing_flags_and_wait();
}

/* draw_anim_seq_frame (1000:3c9d): set p1_sprite x/y from the anim coord table indexed
 * by (current_entity_index-1), z=0x1da, then blit.  Coord entry = {x word, y word}. */
void draw_anim_seq_frame(void)
{
    u8 __far *act = (u8 __far *)MK_FP(anim_coord_table_seg, anim_coord_table_ptr);
    u16 idx = (u16)(current_entity_index - 1);
    if (p1_sprite == (u8 __far *)0) {
        return;   /* default build: sprite objs never bound (init_sprite_structs is a
                     stub carve-out) — guard like draw_icon_row, don't store to 0000:xxxx */
    }
    *(u16 __far *)(p1_sprite + 4) = 0x1dau;
    *(u16 __far *)(p1_sprite + 0) = (u16)(*(u16 __far *)(act + idx * 4u) - 1u);
    *(u16 __far *)(p1_sprite + 2) = *(u16 __far *)(act + idx * 4u + 2u);
    anim_blit_sprite_leaf(0x792eu, SCREENS_DGROUP_RUNTIME_SEG);   /* blit_sprite(0x792e,DS) */
}

/* p1_compute_grid_from_pixel (1000:3...): grid cell from pixel pos minus sprite origin,
 * clamped to [0..0x12]/[0..0x16]. */
void p1_compute_grid_from_pixel(void)
{
    s16 ox = *(s16 __far *)(p1_sprite + 0x14);
    s16 oy = *(s16 __far *)(p1_sprite + 0x16);
    p1_grid_x_new = (s16)((((s16)p1_pixel_x - ox) + 0xe) >> 4) - 1;
    p1_grid_y_new = (s16)((((s16)p1_pixel_y - oy) - 10) >> 3);
    if (p1_grid_x_new < 0) {
        p1_grid_x_new = 0;
    } else if (p1_grid_x_new > 0x12) {
        p1_grid_x_new = 0x12;
    }
    if (p1_grid_y_new < 0) {
        p1_grid_y_new = 0;
    } else if (p1_grid_y_new > 0x16) {
        p1_grid_y_new = 0x16;
    }
}

/* play_anim_sequence (1000:3c4f): step current_entity_index from 1, drawing each anim
 * frame, until the move-descriptor's [0] byte hits the -1 sentinel. */
void play_anim_sequence(void)
{
    char at_sentinel = '\0';
    saved_entity_index = current_entity_index;
    current_entity_index = 1;
    do {
        u8 __far *mdp;
        compute_move_descriptor_ptr();
        mdp = (u8 __far *)MK_FP(move_descriptor_ptr_seg, move_descriptor_ptr);
        if ((s8)mdp[0] == -1) {
            at_sentinel = '\x01';
        } else if (mdp[0] != 0) {
            draw_anim_seq_frame();
        }
        current_entity_index = current_entity_index + 1;
    } while (at_sentinel == '\0');
}

/* draw_icon_row (1000:6130): draw a horizontal row of `settle_countdown` HUD icon
   sprites (frame 0x1aa, the '#'-item glyph) starting at x=0x50, step 8.  Writes x/y/frame
   into the HUD blit obj at hud_icon_sprite_ptr (g_entity_dg+0x7986) and blits each via the
   shared sprite leaf.  Reconstructed 1:1 from the decomp.  Loop count = DGROUP 0x791a =
   settle_countdown — ONE engine byte (game_loop inits =5, '#' pickups ++, physics settle
   --, this fn draws it); an earlier revision read a split-off sharp_item_counter copy that
   stayed 0 → no icons ever (fixed 2026-07-02). */
void draw_icon_row(void)
{
    extern u8 __far *hud_icon_sprite_ptr;   /* anim.c — HUD icon obj far ptr */
    extern u8        settle_countdown;      /* game.c — DGROUP 0x791a */
    u8 icon_idx;

    if (hud_icon_sprite_ptr == (u8 __far *)0) {
        return;   /* DG shadow not yet bound (pre-level) → nothing to draw */
    }
    *(u16 __far *)(hud_icon_sprite_ptr + 4) = 0x1aau;   /* +0x04 frame */
    *(u16 __far *)(hud_icon_sprite_ptr + 2) = 0u;       /* +0x02 y     */
    for (icon_idx = settle_countdown; icon_idx != 0u; icon_idx--) {
        *(u16 __far *)(hud_icon_sprite_ptr + 0) = (u16)icon_idx * 8u + 0x50u;  /* +0x00 x */
        anim_blit_sprite_leaf(0x7986u, SCREENS_DGROUP_RUNTIME_SEG);
    }
}
/* intro_start_level (1000:3cf7): when fire is pressed on the level-intro screen and the
   current node's descriptor flag is clear, walk Bumpy into the level entrance via the
   0x1114 walk-in move script (22 steps), then begin the level (return 1).  If the node's
   descriptor flag is already set (level done), return 0 (stay on the intro screen).
   Reconstructed 1:1 from the decomp; the walk-in script data is in worldmap_data.c. */
u8 intro_start_level(void)
{
    extern u8        game_mode;                    /* player.h */
    extern u8        timing_flag_accumulator;      /* player.h — DGROUP 0x854f */
    extern u8        p1_move_steps_left;           /* player.h */
    extern u16 __far *p1_move_script;              /* player.h */
    extern s16       sound_device_state;           /* DGROUP 0x689c */
    extern void      play_sound(u8 sound_id);      /* game_stubs.c */
    extern char      p1_step_scripted_move(void);  /* player.c 1000:13df */
    extern u16 __far *intro_walk_script(void);     /* worldmap_data.c — 0x1114 walk-in */
    u8 __far *desc;
    u8 did_start = 0;

    game_mode = 0;
    p1_animate_frame();
    p1_animate_frame();
    compute_move_descriptor_ptr();
    desc = (u8 __far *)MK_FP(move_descriptor_ptr_seg, move_descriptor_ptr);
    if (*desc == 0u) {
        play_sound((sound_device_state == 4) ? 0x28u : 3u);
        timing_flag_accumulator = 0xaau;
        p1_start_y = p1_pixel_y;
        p1_start_x = p1_pixel_x;
        p1_move_script    = intro_walk_script();    /* DGROUP 0x1114 */
        p1_move_steps_left = 0x16u;                  /* 22 steps */
        /* nudge to the entrance, draw the pre-walk frame, then restore */
        p1_pixel_x = (u16)(p1_pixel_x - 0xfu);
        p1_pixel_y = (u16)(p1_pixel_y + 3u);
        p1_move_anim = 0xcbu;
        erase_p1_view();
        draw_p1_sprite();
        render_p1_view();
        p1_pixel_x = (u16)(p1_pixel_x + 0xfu);
        p1_pixel_y = (u16)(p1_pixel_y - 3u);
        p1_move_anim = 0u;
        p1_advance_grid_history();
        p1_compute_grid_from_pixel();
        draw_p1_sprite();
        present_frame(1);
        rotate_timing_flags_and_wait();
        erase_p1_view();
        render_p1_view();
        draw_p1_sprite();
        present_frame(1);
        rotate_timing_flags_and_wait();
        do {
            p1_advance_grid_history();
            p1_step_scripted_move();
            p1_update_grid_cell();
            erase_p1_view();
            render_p1_view();
            draw_p1_sprite();
            present_frame(1);
            rotate_timing_flags_and_wait();
        } while (p1_move_steps_left != 0u);
        wait_50_frames();
        did_start = 1;
    }
    return did_start;
}

/* level-intro saved/scratch globals (no other src TU defines them — owned here). */
u8   saved_timing_flag;     /* DGROUP — saved timing_flag_accumulator across the intro  */
u8   saved_entity_index;    /* DGROUP — saved current_entity_index across the intro     */

/* the table-row entry far ptr render_highscore_table / highscore_enter_name use:
 *  highscore_entry_ptr @ DGROUP 0x8574/0x8576 = far ptr -> the 8-byte entry at
 *  0x203b:(row*8 + 0x8f0) = {u16 name_off, u16 name_seg, u16 score_lo, u16 score_hi}.
 *  The disasm does LES BX,[0x8574]; LES BX,ES:[BX] (double indirection): the entry's
 *  first two words are a far ptr to the name CHARS, rebuilt at the use site with
 *  MK_FP(name_seg, name_off) — the project's _off/_seg far-pointer convention. */
u16 __far *highscore_entry_ptr;   /* DGROUP 0x8574/0x8576 -> the 8-byte table entry */

/* the placeholder name far ptr render_highscore_table stamps into a new qualifying entry
 *  (DGROUP 0x0920/0x0922 = DAT_203b_0920/0922).  Owned here; harness-seeded. */
u16 highscore_new_name_off;   /* DGROUP 0x0920 */
u16 highscore_new_name_seg;   /* DGROUP 0x0922 */

/* ── password / level-code table (DGROUP 0x135c) ──────────────────────────────
 *  The 8 six-char level PASSWORDS enter_password compares the typed code against.
 *  DGROUP 0x135c is an 8-entry far-ptr table → the 6-char strings at DGROUP 0x256e
 *  (stride 7), decoded 1:1 from the unpacked EXE (verified: table[i] → ACCESS,
 *  BUTTON, ISLAND, PRETTY, WINNER, ZOMBIE, LOVELY, SYSTEM).  `matched index + 2` →
 *  current_level, so ACCESS→2 … SYSTEM→9.
 *
 *  MISNOMER CORRECTED (2026-07-05): was named `highscore_name_table` — WRONG.  0x135c
 *  is the PASSWORD table for the "ENTER YOUR PASSWORD" screen (show_menu_select_screen
 *  → enter_password), NOT the high-score initials.  The high-score initials are the
 *  separate 8-char table (BIG JIM./SUPER JO/… at DGROUP 0x11e6, used by
 *  highscore_enter_name).  Renamed in Ghidra + here.
 *
 *  RECONSTRUCTION FIDELITY: same loader-relocated-far-ptr constraint as the HOF
 *  default table (init_highscore_default_table) — the playable build POPULATES
 *  password_table at startup via init_password_table (called from the playable main);
 *  the default BUMPY.EXE keeps the zero-init storage (documented here + in Ghidra, not
 *  run).  Without this the table was all-NULL, so every code mismatched → the screen
 *  always returned 0 = "PASSWORD ERROR". */
u16 password_table[8 * 2];   /* DGROUP 0x135c (8 × far ptr = off/seg pairs) */

#ifdef BUMPY_PLAYABLE
static const char s_pw_access[7] = "ACCESS";   /* DGROUP 0x256e → current_level 2 */
static const char s_pw_button[7] = "BUTTON";   /* DGROUP 0x2575 → 3 */
static const char s_pw_island[7] = "ISLAND";   /* DGROUP 0x257c → 4 */
static const char s_pw_pretty[7] = "PRETTY";   /* DGROUP 0x2583 → 5 */
static const char s_pw_winner[7] = "WINNER";   /* DGROUP 0x258a → 6 */
static const char s_pw_zombie[7] = "ZOMBIE";   /* DGROUP 0x2591 → 7 */
static const char s_pw_lovely[7] = "LOVELY";   /* DGROUP 0x2598 → 8 */
static const char s_pw_system[7] = "SYSTEM";   /* DGROUP 0x259f → 9 */

void init_password_table(void)
{
    static const char __far * const pw[8] = {
        s_pw_access, s_pw_button, s_pw_island, s_pw_pretty,
        s_pw_winner, s_pw_zombie, s_pw_lovely, s_pw_system };
    u8 i;
    for (i = 0; i < 8u; i = i + 1) {
        password_table[i * 2]     = FP_OFF((const char __far *)pw[i]);
        password_table[i * 2 + 1] = FP_SEG((const char __far *)pw[i]);
    }
}
#endif /* BUMPY_PLAYABLE */

/* the per-screen palette patch source render_highscore_table / show_highscore_screen /
 *  show_level_intro_screen copy when palette_mode==1 (DGROUP 0x71e — the same table the
 *  T4 show_menu_select_screen uses).  Reuse dgroup_pal_patch_71e (defined in the T4
 *  block).  Under the boot palette_mode==2 this whole path is skipped. */

/* ════════════════════════════════════════════════════════════════════════════
 *  draw_name_entry_cursor — 1000:5fdb
 *  Position + draw the name-entry cursor sprite at (col,row) with glyph `frame`;
 *  optionally blit; advance 8 frames.  Disasm 5fdb: view[+0x14]=row;
 *  FUN_7b4a(view); p1[2]=frame; *p1=row<<4; p1[1]=col; if(do_blit) blit_sprite; run_n_frames(8).
 * ════════════════════════════════════════════════════════════════════════════ */
u16 draw_name_entry_cursor(u8 col, u8 row, u16 frame, char do_blit)
{
    u16 __far *p;

    *(u16 __far *)(render_descriptor_ptr + 0x14) = (u16)row;
    fun_7b4a_view_blit(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
    p = (u16 __far *)p1_sprite;
    p[2] = frame;
    *(u16 __far *)p1_sprite = (u16)row << 4;
    p[1] = (u16)col;
    if (do_blit != '\0') {
        anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
    }
    run_n_frames(8);
    /* The original is void; the engine's caller reuses AX (= the frame value, the cursor
       glyph just drawn) as `cur_letter` for the next iteration — modelled by returning
       `frame`.  RECONSTRUCTION FIDELITY: the cursor glyph is a stubbed graphics-overlay blit;
       only the returned frame (a non-game AX carry) feeds the next loop, which re-masks
       its low byte — the validated output is the name buffer + the SCRSNAP, not this. */
    return frame;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  highscore_enter_name — 1000:59d3
 *  The interactive 8-char name-entry state machine for table row `row`.  Sets up the
 *  name far ptr (highscore_entry_ptr -> 0x203b:(row*8+0x8f0)), seeds the first char to
 *  'A', then loops: poll FUN_75a2; bit 1=left/2=right cycle the current letter through
 *  0x1ad..0x1cf (wrapping the gap at 0x1d0 -> 0x1a3, mapping '.' 0x2e -> '[' 0x5b for
 *  display); 4=prev char / 8=next char move the cursor (writing the chosen letter into
 *  the name buffer); else blink the cursor; 0x10=done exits.  Disasm 59d3.
 * ════════════════════════════════════════════════════════════════════════════ */
void highscore_enter_name(u8 row)
{
    u16 letter_code;
    u8  blink_counter;
    u8  char_idx;
    u8  input_flags;
    u8 __far *name;

    char_idx = 0;
    /* highscore_entry_ptr -> the 8-byte entry at row*8+0x8f0; the name CHARS are at
       MK_FP(entry[1], entry[0]) (the entry's name far ptr). */
    highscore_entry_ptr = (u16 __far *)&highscore_name_buf[(u16)row * 8];
    name = (u8 __far *)MK_FP(highscore_entry_ptr[1], highscore_entry_ptr[0]);
    letter_code = 0x1b6;
    name[0] = 0x41;                                  /* name[0] = 'A' */
    *(u16 __far *)(render_descriptor_ptr + 0x14) = 0;
    *(u16 __far *)(render_descriptor_ptr + 0x16) = (u16)row * 2 + 8;
    blink_counter = 0;
    draw_name_entry_cursor((u8)(row * 0x10 + 'A'), 0, letter_code, 1);
    while ((input_flags = (u8)fun_75a2_poll_action(0)), (input_flags & 0x10) != 0x10) {
        name = (u8 __far *)MK_FP(highscore_entry_ptr[1], highscore_entry_ptr[0]);
        if (((input_flags & 1) == 0) || ((int)letter_code < 0x1ad)) {
            if (((input_flags & 2) == 0) || (0x1cf < (int)letter_code)) {
                if (((input_flags & 4) == 0) || (char_idx == 0)) {
                    if (((input_flags & 8) == 0) || (6 < char_idx)) {
                        blink_counter = blink_counter + 1;
                        if ((blink_counter & 8) == 0) {
                            anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
                        } else {
                            fun_7b4a_view_blit(render_descriptor_ptr,
                                               SCREENS_DGROUP_RUNTIME_SEG);
                        }
                        run_n_frames(1);
                    } else {                                  /* 8 = next char */
                        if (letter_code == 0x1d0) {
                            letter_code = 0x1a3;
                        }
                        letter_code = letter_code - GLYPH_FRAME_BIAS;
                        name[char_idx] = (u8)letter_code;
                        char_idx = char_idx + 1;
                        letter_code = (u16)name[char_idx];
                        if (letter_code == 0x2e) {
                            letter_code = 0x5b;
                        }
                        letter_code = letter_code + GLYPH_FRAME_BIAS;
                        anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
                        draw_name_entry_cursor((u8)(row * 0x10 + 'A'), char_idx,
                                               letter_code, 0);
                    }
                } else {                                      /* 4 = prev char */
                    if (letter_code == 0x1d0) {
                        letter_code = 0x1a3;
                    }
                    letter_code = letter_code - GLYPH_FRAME_BIAS;
                    name[char_idx] = (u8)letter_code;
                    char_idx = char_idx - 1;
                    letter_code = (u16)name[char_idx];
                    if (letter_code == 0x2e) {
                        letter_code = 0x5b;
                    }
                    letter_code = letter_code + GLYPH_FRAME_BIAS;
                    anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
                    draw_name_entry_cursor((u8)(row * 0x10 + 'A'), char_idx,
                                           letter_code, 0);
                }
            } else {                                          /* 2 = right (next letter) */
                letter_code = letter_code + 1;
                if (letter_code == 0x1d0) {
                    letter_code = 0x1a3;
                }
                letter_code = letter_code - GLYPH_FRAME_BIAS;
                name[char_idx] = (u8)letter_code;
                if (letter_code == 0x2e) {
                    letter_code = 0x5b;
                }
                letter_code = letter_code + GLYPH_FRAME_BIAS;
                draw_name_entry_cursor((u8)(row * 0x10 + 'A'), char_idx,
                                       letter_code, 1);
            }
        } else {                                              /* 1 = left (prev letter) */
            letter_code = letter_code - 1;
            if (letter_code == 0x1d0) {
                letter_code = 0x1a3;
            }
            letter_code = letter_code - GLYPH_FRAME_BIAS;
            name[char_idx] = (u8)letter_code;
            if (letter_code == 0x2e) {
                letter_code = 0x5b;
            }
            letter_code = letter_code + GLYPH_FRAME_BIAS;
            draw_name_entry_cursor((u8)(row * 0x10 + 'A'), char_idx,
                                   letter_code, 1);
        }
    }
    anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  render_highscore_table — 1000:57e1
 *  Render the 7-entry high-score table (name glyphs via blit_sprite + score via
 *  draw_number_sprites, T3).  If the current score (score_hi:score_lo) qualifies, shift
 *  the lower entries down, insert a placeholder ('AAAAAAAA' name + the score), and run
 *  highscore_enter_name on the inserted row; otherwise wait for a keypress.  Disasm 57e1.
 * ════════════════════════════════════════════════════════════════════════════ */
void render_highscore_table(void)
{
    char qualified;
    u8   insert_row;
    u8   row_idx;
    u8   shift_idx;
    u8   char_col;
    u8   name_char;
    u16 __far *p;
    u8 __far *name;

    fun_9410_set_sprite_table(0);
    qualified = '\0';
    insert_row = 0;
    for (row_idx = 0; row_idx < 7; row_idx = row_idx + 1) {
        /* entry = highscore_name_buf[row*8]: {u16 name_off, u16 name_seg, u16 score_lo,
           u16 score_hi}.  highscore_entry_ptr -> that entry; name chars = MK_FP(seg,off). */
        highscore_entry_ptr = (u16 __far *)&highscore_name_buf[(u16)row_idx * 8];
        if ((highscore_entry_ptr[3] <= score_hi) &&
            (((highscore_entry_ptr[3] < score_hi) ||
              (highscore_entry_ptr[2] < score_lo)) &&
             (qualified == '\0'))) {
            qualified = '\x01';
            insert_row = row_idx;
            /* shift the lower entries down (rows row_idx+1..6 <- row_idx..5). */
            for (shift_idx = 6; row_idx < shift_idx; shift_idx = shift_idx - 1) {
                u16 __far *dst = (u16 __far *)&highscore_name_buf[(u16)shift_idx * 8];
                u16 __far *src = (u16 __far *)&highscore_name_buf[(u16)shift_idx * 8 - 8];
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
            /* insert the placeholder name far ptr + 'A'×8 name + the score. */
            highscore_entry_ptr[0] = highscore_new_name_off;
            highscore_entry_ptr[1] = highscore_new_name_seg;
            name = (u8 __far *)MK_FP(highscore_entry_ptr[1], highscore_entry_ptr[0]);
            for (shift_idx = 0; shift_idx < 8; shift_idx = shift_idx + 1) {
                name[shift_idx] = 0x41;
            }
            highscore_entry_ptr[2] = score_lo;
            highscore_entry_ptr[3] = score_hi;
        }
        /* draw the 8 name glyphs for this row. */
        ((u16 __far *)p1_sprite)[1] = (u16)row_idx * 0x10 + 0x41;
        name = (u8 __far *)MK_FP(highscore_entry_ptr[1], highscore_entry_ptr[0]);
        for (char_col = 0; char_col < 8; char_col = char_col + 1) {
            p = (u16 __far *)p1_sprite;
            name_char = name[char_col];
            if (name_char == 0x2e) {
                if ((qualified == '\0') || (insert_row != row_idx)) {
                    name_char = 0x20;
                } else {
                    name_char = 0x5b;
                }
            }
            p[2] = (u16)name_char + GLYPH_FRAME_BIAS;
            *p = (u16)char_col << 4;
            if (name_char != 0x20) {
                anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
            }
        }
        /* draw the row's score via the digit-sprite formatter (T3). */
        draw_number_sprites(highscore_entry_ptr[2], highscore_entry_ptr[3],
                            '\a', 0xb0, (u16)row_idx * 0x10 + 0x41);
    }
    if (qualified == '\0') {
        wait_keypress();
    } else {
        highscore_enter_name(insert_row);
    }
    fun_9410_set_sprite_table(1);
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  show_highscore_screen — 1000:5681
 *  Load + vec_decode the high-score background resource (resource 3), optionally patch
 *  the palette (palette_mode==1), iris-wipe in, build the 20×25 bg view, present, upload
 *  the DAC, then render the highscore table (render_highscore_table).  Disasm 5681.
 * ════════════════════════════════════════════════════════════════════════════ */
void show_highscore_screen(void)
{
    int res_handle;
    u32 decoded_len;
    screen_view_desc __far *d;

    set_resource_table(0x928, SCREENS_DGROUP_RUNTIME_SEG);
    fullscreen_img_buf   = fullscreen_buf;
    highscore_bg_buf_seg = fullscreen_buf_seg;
    res_handle = open_resource(3, 4);
    decoded_len = read_chunked(res_handle, fullscreen_img_buf, highscore_bg_buf_seg,
                               0x094c, 0x094e);
    c_close(res_handle);
    vec_decode(fullscreen_img_buf, highscore_bg_buf_seg, decoded_len, 0x7d63, 0);
    if (palette_mode == 1) {
        patch_image_palette(dgroup_pal_patch_71e);
    }
    play_iris_wipe_transition();
    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->image_off = fullscreen_img_buf + 99;
    d->image_seg = highscore_bg_buf_seg;
    d->src_x = 0;
    d->src_y = 0;
    d->width = 0x14;
    d->height = 0x19;
    d->flag = 1;
    d->dest_x = 0;
    d->dest_y = 0;
    d->subhandler = 0;
    d->clip_w = 0x14;
    d->clip_h = 0x19;
    restore_bg_view(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
    fun_7b93_present_blank(fullscreen_buf, fullscreen_buf_seg, 0);
    fun_7bca_flip(0);
    present_frame(1);
    wait_vretrace_thunk();
    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->flag = 0;
    d->clip_w = 1;
    d->clip_h = 2;
    render_highscore_table();
    return;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  enter_password — 1000:5c87
 *  The interactive 6-char menu-select name-entry state machine.  fmemcpy a 6-char
 *  SS-local buffer from DGROUP 0x256a (reconstructed as the module-static
 *  enter_name_buf), seeds it to 'A', then loops polling FUN_75a2 (same letter-cycle /
 *  cursor-move bits as highscore_enter_name, cursor max 5 here).  On 0x10=done it
 *  compares the 6-char buffer against the 8-entry name table at DGROUP 0x135c and
 *  returns the matched index + 2 (or 0).  Args: param_1=col (x), param_2=row (y).
 *  Disasm 5c87 / 5fdb call layout.
 * ════════════════════════════════════════════════════════════════════════════ */
u8 enter_name_buf[6];   /* SS-local in the original; module-static here (RECON FIDELITY) */

u8 enter_password(u8 col, u8 row)
{
    u16 cur_letter;
    u8  blink_counter;
    u8  matched_idx;
    u8  cursor_pos;
    u8  idx;
    u8  input_flags;
    u8 __far *name_buf;
    char mismatch;

    /* fmemcpy(0x203b:0x256a -> enter_name_buf, 4 words); reconstructed as the local copy.
       cur_letter (BP-0x14) is the persistent letter/glyph-frame accumulator, init 0x1b6;
       the cursor draws are void statements (the engine discards their AX). */
    name_buf = (u8 __far *)enter_name_buf;
    *(u16 __far *)(render_descriptor_ptr + 0x0e) = 0;
    *(u16 __far *)(render_descriptor_ptr + 0x1c) = 0;
    *(u16 __far *)(render_descriptor_ptr + 0x1e) = 1;
    *(u16 __far *)(render_descriptor_ptr + 0x20) = 2;
    blink_counter = 0;
    cursor_pos = 0;
    for (idx = 0; idx < 6; idx = idx + 1) {
        name_buf[idx] = 0x41;
    }
    name_buf[0] = 0x41;
    cur_letter = 0x1b6;
    *(u16 __far *)(render_descriptor_ptr + 0x14) = (u16)row;
    *(u16 __far *)(render_descriptor_ptr + 0x16) = (u16)col << 1;
    draw_name_entry_cursor((u8)(col << 4), row, cur_letter, 1);
    while (1) {
        input_flags = (u8)fun_75a2_poll_action(0);
        if ((input_flags & 0x10) == 0x10) {
            break;
        }
        if (((input_flags & 1) == 0) || ((int)cur_letter < 0x1ad)) {
            if (((input_flags & 2) == 0) || (0x1cf < (int)cur_letter)) {
                if (((input_flags & 4) == 0) || (cursor_pos == 0)) {
                    if (((input_flags & 8) == 0) || (4 < cursor_pos)) {
                        blink_counter = blink_counter + 1;
                        if ((blink_counter & 8) == 0) {
                            anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
                        } else {
                            fun_7b4a_view_blit(render_descriptor_ptr,
                                               SCREENS_DGROUP_RUNTIME_SEG);
                        }
                        run_n_frames(1);
                    } else {                                  /* 8 = next char */
                        if (cur_letter == 0x1d0) {
                            cur_letter = 0x1a3;
                        }
                        cur_letter = cur_letter - GLYPH_FRAME_BIAS;
                        name_buf[cursor_pos] = (u8)cur_letter;
                        cursor_pos = cursor_pos + 1;
                        row = row + 1;
                        cur_letter = (u16)name_buf[cursor_pos];
                        if (cur_letter == 0x2e) {
                            cur_letter = 0x5b;
                        }
                        cur_letter = cur_letter + GLYPH_FRAME_BIAS;
                        anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
                        draw_name_entry_cursor((u8)(col << 4), row, cur_letter, 0);
                    }
                } else {                                      /* 4 = prev char */
                    if (cur_letter == 0x1d0) {
                        cur_letter = 0x1a3;
                    }
                    cur_letter = cur_letter - GLYPH_FRAME_BIAS;
                    name_buf[cursor_pos] = (u8)cur_letter;
                    cursor_pos = cursor_pos - 1;
                    row = row - 1;
                    cur_letter = (u16)name_buf[cursor_pos];
                    if (cur_letter == 0x2e) {
                        cur_letter = 0x5b;
                    }
                    cur_letter = cur_letter + GLYPH_FRAME_BIAS;
                    anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
                    draw_name_entry_cursor((u8)(col << 4), row, cur_letter, 0);
                }
            } else {                                          /* 2 = right (next letter) */
                cur_letter = cur_letter + 1;
                if (cur_letter == 0x1d0) {
                    cur_letter = 0x1a3;
                }
                cur_letter = cur_letter - GLYPH_FRAME_BIAS;
                name_buf[cursor_pos] = (u8)cur_letter;
                if (cur_letter == 0x2e) {
                    cur_letter = 0x5b;
                }
                cur_letter = cur_letter + GLYPH_FRAME_BIAS;
                draw_name_entry_cursor((u8)(col << 4), row, cur_letter, 1);
            }
        } else {                                              /* 1 = left (prev letter) */
            cur_letter = cur_letter - 1;
            if (cur_letter == 0x1d0) {
                cur_letter = 0x1a3;
            }
            cur_letter = cur_letter - GLYPH_FRAME_BIAS;
            name_buf[cursor_pos] = (u8)cur_letter;
            if (cur_letter == 0x2e) {
                cur_letter = 0x5b;
            }
            cur_letter = cur_letter + GLYPH_FRAME_BIAS;
            draw_name_entry_cursor((u8)(col << 4), row, cur_letter, 1);
        }
    }
    anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
    /* compare the 6-char buffer against the 8-entry name table at 0x135c. */
    matched_idx = 0;
    idx = 0;
    while ((idx < 8) && (matched_idx == 0)) {
        u8 __far *tbl = (u8 __far *)MK_FP(password_table[(u16)idx * 2 + 1],
                                          password_table[(u16)idx * 2]);
        mismatch = 0;
        cursor_pos = 0;
        while ((cursor_pos < 6) && (!mismatch)) {
            if (name_buf[cursor_pos] != (char)tbl[cursor_pos]) {
                mismatch = 1;
            }
            cursor_pos = cursor_pos + 1;
        }
        if (!mismatch) {
            matched_idx = idx + 2;
        }
        idx = idx + 1;
    }
    return matched_idx;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  show_level_intro_screen — 1000:0d9d
 *  Load + display the fullscreen image (resource 3), set the palette, render a name
 *  string (SS-local fmemcpy'd) + the current_level name (DGROUP table 0x1354) as sprite
 *  glyphs, then wait until input bit 0x10 (via FUN_75a2).  Disasm 0d9d.
 *
 *  RECONSTRUCTION FIDELITY: the row-1 name string + the per-level name table are engine
 *  DGROUP data (fmemcpy'd into SS-locals / read from 0x1354); reconstructed as module
 *  data the harness seeds.  The glyph blits write the p1_sprite descriptor (the gated
 *  DESCRIPTOR-LEVEL output); the graphics overlay glyph pixels are the stubbed blit_sprite's.
 * ════════════════════════════════════════════════════════════════════════════ */
u8  intro_name_row[0xd];        /* row-1 name string (fmemcpy'd from DGROUP)            */
u16 level_name_table[16 * 2];   /* DGROUP 0x1354 (per-level name far ptrs, stride 4)    */

void show_level_intro_screen(void)
{
    int res_handle;
    u8  glyph_ch;
    u8  char_idx;
    u8  col_pos;
    u16 __far *p;
    u8 __far *level_name;

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
    fun_7b93_present_blank(fullscreen_img_buf, highscore_bg_buf_seg, 0);
    fun_7bca_flip(0);
    wait_vretrace_thunk();
    /* row 1 (13 glyphs) at y=0x50, starting col 4. */
    col_pos = 4;
    ((u16 __far *)p1_sprite)[1] = 0x50;
    for (char_idx = 0; char_idx < 0xd; char_idx = char_idx + 1) {
        p = (u16 __far *)p1_sprite;
        glyph_ch = intro_name_row[char_idx];
        p[2] = (u16)glyph_ch + GLYPH_FRAME_BIAS;
        *p = (u16)col_pos << 4;
        if (glyph_ch != 0x20) {
            anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
        }
        col_pos = col_pos + 1;
    }
    /* row 2 — the current_level name (6 glyphs) at y=0x70, starting col 7. */
    col_pos = 7;
    level_name = (u8 __far *)MK_FP(level_name_table[(u16)current_level * 2 + 1],
                                   level_name_table[(u16)current_level * 2]);
    ((u16 __far *)p1_sprite)[1] = 0x70;
    for (char_idx = 0; char_idx < 6; char_idx = char_idx + 1) {
        p = (u16 __far *)p1_sprite;
        p[2] = (u16)level_name[char_idx] + GLYPH_FRAME_BIAS;
        *p = (u16)col_pos << 4;
        anim_blit_sprite_leaf(0x792e, SCREENS_DGROUP_RUNTIME_SEG);
        col_pos = col_pos + 1;
    }
    do {
        glyph_ch = (u8)fun_75a2_poll_action(0);
    } while ((glyph_ch & 0x10) == 0);
    fun_9410_set_sprite_table(0);
    set_resource_table(0x90, SCREENS_DGROUP_RUNTIME_SEG);
    return;
}

/* the per-level border-image length table + palette table level_intro_screen reads
 *  (DGROUP 0x974 length, 0x6e6 palette ptr — both indexed by current_level).  Owned
 *  here; harness-seeded.  Under the boot palette_mode==2 the palette path is skipped.
 *
 *  level_palette_ptr_table (DGROUP 0x6e6) is now populated (2026-07-11): the per-level EGA
 *  AC-palette SOURCE tables (DGROUP 0x65a..0x6da) were RE'd + extracted + wired into it by
 *  init_ega_palette_patch_tables (relocated to reconstructed storage, like init_worldmap_data).
 *  See that init for the fidelity note.  The engine reads level_palette_ptr_table[current_level]
 *  (1..9) in the palette_mode==1 branch of level_intro_screen below. */
u16 level_img_len_table[16 * 5];    /* DGROUP 0x974 (stride 10 = 5 words/level)         */
u16 level_palette_ptr_table[16 * 2];/* DGROUP 0x6e6 (stride 4 = far ptr/level 1..9)     */

/* ════════════════════════════════════════════════════════════════════════════
 *  level_intro_screen — 1000:3852
 *  The per-level intro screen + interactive move loop.  Loads + vec_decodes the level's
 *  border image (resource current_level+7), installs the level palette (palette_mode==1),
 *  iris-wipes in, draws the score HUD (draw_number — T3) + Bumpy at his start position,
 *  then runs an input-wait loop: directions step Bumpy (p1_move_step_*), fire (0x10) ->
 *  intro_start_level (begins the level), a quit key -> game_state=0xff.  Disasm 3852.
 *  The gameplay/render leaves are STUBBED (separate subsystem); the validated output is
 *  the screen-global SCRSNAP (timing saved/restored, input cleared, game_state, palette).
 * ════════════════════════════════════════════════════════════════════════════ */
void level_intro_screen(void)
{
    u8  quit_key;
    int file_handle;
    u32 bytes_read;
    u8  palette_idx;
    char done_flag;
    screen_view_desc __far *d;
    u8 __far *img;
    u8 __far *pal;

    done_flag = '\0';
    saved_timing_flag = timing_flag_accumulator;
    timing_flag_accumulator = 0;
    input_state = 0;
    set_resource_table(0x928, SCREENS_DGROUP_RUNTIME_SEG);
    file_handle = open_resource(current_level + 7, 4);
    bytes_read = read_chunked(file_handle, fullscreen_buf, fullscreen_buf_seg,
                              level_img_len_table[(u16)current_level * 5],
                              level_img_len_table[(u16)current_level * 5 + 1]);
    c_close(file_handle);
    vec_decode(fullscreen_buf, fullscreen_buf_seg, bytes_read, 0x7d63, 0);
    if (palette_mode == 1) {
        img = (u8 __far *)MK_FP(fullscreen_buf_seg, fullscreen_buf);
        pal = (u8 __far *)MK_FP(level_palette_ptr_table[(u16)current_level * 2 + 1],
                                level_palette_ptr_table[(u16)current_level * 2]);
        for (palette_idx = 0; palette_idx < 0x10; palette_idx = palette_idx + 1) {
            img[(u16)palette_idx + 0x23] = pal[palette_idx];
        }
    }
#ifdef BUMPY_PLAYABLE
    /* Draw the overworld-entry iris on the DISPLAYED slot (0), like every other iris
     * (run_main_menu / show_menu_select_screen bracket theirs with fun_9410(0)).  Without
     * this the iris inherited the gameplay draw slot (1 = hidden) from run_main_menu's
     * trailing fun_9410(1), so whether the wipe was visible depended on the host page-flip
     * parity (host_render.c host_fb_init re-anchors the CRTC display to slot 0 per level so
     * this is now deterministic).  Restore slot 1 immediately after so the composition +
     * overworld walk below (and the mode-11 view-sync at init_fullscreen_view_desc) run on
     * the gameplay draw page exactly as before — the view-sync freeze fix is untouched.
     * Host-only: the default build's fun_9410 is a NOP, and the page model is host-specific;
     * gated so the faithful decomp stays byte-identical.  RECONSTRUCTION FIDELITY noted. */
    fun_9410_set_sprite_table(0);
    play_iris_wipe_transition();
    fun_9410_set_sprite_table(1);
#else
    play_iris_wipe_transition();
#endif
    d = (screen_view_desc __far *)render_descriptor_ptr;
    d->image_off = fullscreen_buf + 99;
    d->image_seg = fullscreen_buf_seg;
    d->src_x = 0;
    d->src_y = 0;
    d->width = 0x14;
    d->height = 0x19;
    d->flag = 1;
    d->dest_x = 0;
    d->dest_y = 0;
    d->subhandler = 0;
    d->clip_w = 0x14;
    d->clip_h = 0x19;
    restore_bg_view(render_descriptor_ptr, SCREENS_DGROUP_RUNTIME_SEG);
    draw_number(score_lo, score_hi, '\a', 1, 8);
    draw_icon_row();
    play_anim_sequence();
    current_entity_index = saved_entity_index;
    fun_7b93_present_blank(fullscreen_buf, fullscreen_buf_seg, 0);
    p1_pixel_x = p1_start_x;
    p1_pixel_y = p1_start_y;
    p1_move_anim = 0x21;
    init_fullscreen_view_desc(1, 0);
    draw_p1_sprite();
    fun_7bca_flip(0);
    present_frame(1);
    wait_vretrace_thunk();
    p1_update_grid_cell();
    p1_advance_grid_history();
    render_p1_view();
    p1_advance_grid_history();
    compute_move_descriptor_ptr();
    while (done_flag == '\0') {
        poll_input();
        if ((input_state & 1) == 0) {
            if ((input_state & 2) == 0) {
                if ((input_state & 4) == 0) {
                    if ((input_state & 8) == 0) {
                        if ((input_state & 0x10) == 0) {
                            quit_key = get_key_state(1);
                            if (quit_key != '\0') {
                                done_flag = -1;
                                frame_abort_flag = 0xff;   /* DAT_203b_928d = 0xff */
                            }
                        } else {
                            done_flag = (char)intro_start_level();
                        }
                    } else {
                        p1_move_step_right();
                    }
                } else {
                    p1_move_step_left();
                }
            } else {
                p1_move_step_down();
            }
        } else {
            p1_move_step_up();
        }
        input_state = 0;
    }
    timing_flag_accumulator = saved_timing_flag;
    return;
}
