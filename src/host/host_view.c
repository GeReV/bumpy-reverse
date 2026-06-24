#ifdef BUMPY_PLAYABLE
#include <string.h>
#include <malloc.h>           /* _fmalloc */
#include "host.h"             /* host_framebuffer, VGA constants */
#include "../bumpy.h"
#include "../bgi_overlay.h"   /* bgi_view_desc, render_player_view, restore_bg_view,
                                  BGI_PAGE_A000_OFF, BGI_PLANE_SIZE, BGI_PAGE_SIZE */
#include "../screens.h"       /* render_descriptor_ptr, fullscreen_buf/seg,
                                  palette_mode, draw_number, upload_vga_dac_palette,
                                  play_iris_wipe_transition */
#include "../anim.h"          /* p1_sprite */

/* P2 move-state handler table + its four cell-move handlers (player2.c).  Declared
 * locally rather than via player2.h: that header and game.h carry an inconsistent
 * p2_step_scripted_move prototype that only collides when both are pulled into one
 * TU, and host_view.c only needs these five symbols. */
extern void (__far * __far *p2_state_handler_tbl)(void);
extern void p2_cell_move_up(void);
extern void p2_cell_move_down(void);
extern void p2_cell_move_left(void);
extern void p2_cell_move_right(void);
#include "../level.h"         /* level_get_entity_dg, DG_P1_OBJ, DG_P2_OBJ,
                                  OBJ_FTBL_OFF, OBJ_FTBL_SEG */
#include "../input.h"         /* get_key_state */
#include "../game.h"          /* present_frame, set_display_page, draw_number */
/* NOTE: player2.h and game.h declare p2_step_scripted_move with inconsistent
 * return types (void vs u8); we must NOT include player2.h when game.h is present.
 * Use an explicit extern for p2_sprite only. */

/* ── Explicit extern to avoid header conflicts ──────────────────────────── */
extern u8 __far *p2_sprite;   /* player2.c DGROUP 0x9b9e/0x9ba0 */

/* ============================================================================
 * host_view.c — view/setup leaves + background save-under  (Plan A, Task 7)
 * ============================================================================
 *
 * Implements the five view/setup/screen leaves that game_stubs.c stubs as
 * carve-outs in the default build.  Under -dBUMPY_PLAYABLE these real bodies
 * replace the NOPs:
 *
 *   init_sprite_structs       — 1:1 structural port of 1000:33c5
 *   init_fullscreen_view_desc — 1:1 structural port of 1000:5181
 *   setup_fullscreen_view     — 1:1 structural port of 1000:483c + host save-under
 *   show_text_screen          — 1:1 structural port of 1000:11eb
 *   show_pause_screen         — 1:1 structural port of 1000:49d7
 *
 * ┌──────────────────────────────────────────────────────────────────────────┐
 * │ RECONSTRUCTION FIDELITY — DOCUMENTED DIVERGENCES                         │
 * ├──────────────────────────────────────────────────────────────────────────┤
 * │ 1. FLAT-RAM SAVE-UNDER (setup_fullscreen_view):                           │
 * │    The original engine's setup_fullscreen_view copies VGA page-0          │
 * │    (a000:0000) into fullscreen_buf via render_player_view + the BGI       │
 * │    overlay's mode-10 subhandler-0 full-plane copy.  In the host flat-RAM  │
 * │    model we additionally copy host_framebuffer's page-0 region (4 planes  │
 * │    × BGI_PAGE_SIZE = 8000 B each) into hv_saveunder_buf.  The view-desc  │
 * │    build at render_descriptor_ptr and the render_player_view call are     │
 * │    faithfully reconstructed 1:1; only the source (flat RAM vs VGA a000)  │
 * │    and dest (hv_saveunder_buf vs fullscreen_buf) differ.                  │
 * │    Recorded in docs/reconstruction-fidelity.md (Playable host / Task 7). │
 * │                                                                            │
 * │ 2. SPRITE-OBJ FAR-PTR INIT (init_sprite_structs):                         │
 * │    The engine stores sprite objects at fixed DGROUP offsets (0x792e        │
 * │    p1_sprite, 0x795a p2_sprite, 0x7986 hud-icon) and seeds each object's  │
 * │    +0x06/+0x08 from screen_sprite_buf (DGROUP 0xa0c6/0xa0c8) and sets the │
 * │    flag byte (+0x09: old & 0x87 | 0x80).  In the host: p1_sprite and      │
 * │    p2_sprite are pointed at g_entity_dg[0x792e] / g_entity_dg[0x795a]    │
 * │    via level_get_entity_dg() — the same shadow buffer hr_blit_obj reads   │
 * │    from — ensuring draw_p1/p2_sprite obj writes and the blit leaf read    │
 * │    the same bytes.  The sprite-sheet buf seed (+0x06/+0x08) and flag byte │
 * │    (+0x09) are SKIPPED: the frametable at +0x06/+0x08 is seeded by        │
 * │    level_populate_dg (level.c) from g_bank_buf (the correct frametable);  │
 * │    screen_sprite_buf is a BGI-overlay ptr unused by the planar pipeline.  │
 * │    hud_icon_sprite_ptr (obj at 0x7986) has no C declaration; skipped.     │
 * │    Recorded in docs/reconstruction-fidelity.md (Playable host / Task 7). │
 * │                                                                            │
 * │ 3. BGI MODE-11 CALL (init_fullscreen_view_desc):                           │
 * │    The engine ends init_fullscreen_view_desc with bgi_set_mode_11_thunk    │
 * │    (→ bgi_set_mode_11 1ab9:126e, dynamically-loaded BGI overlay code).    │
 * │    In the host this call is replaced by present_frame(1) which copies the  │
 * │    composed RAM image to real VGA.  The view-desc build is 1:1.            │
 * │    Recorded in docs/reconstruction-fidelity.md (Playable host / Task 7). │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * The faithful default build (BUMPY.EXE) links game_stubs.c NOPs instead and
 * never sees this file.
 * ============================================================================ */

/* ── Score globals (game.c) ──────────────────────────────────────────────── */
extern u16 score_lo;   /* DGROUP 0xa0d4 */
extern u16 score_hi;   /* DGROUP 0xa0d6 */

/* ── Carve-out stubs local to the playable build ─────────────────────────────
 * These symbols are needed by host_view.c (called from setup_fullscreen_view)
 * but are NOT in game_stubs.c (adding them there would change BUMPY.EXE
 * byte-equality).  Define them here under BUMPY_PLAYABLE.
 *
 * redraw_level_background_tiles (1000:2a0a): walk the level grid (every other
 * row, 20 cols), call setup_sprite_descriptor + restore_bg_tile_run for each
 * nonzero bg tile.  CARVE-OUT: not yet reconstructed; NOP stub for the link.
 * In the default build, setup_fullscreen_view is itself a NOP (game_stubs.c)
 * so this symbol is never needed there. */
void redraw_level_background_tiles(void) {}

/* BGI-overlay screen present thunks (stubs in screens.c).
 * show_text_screen calls these after loading the fullscreen image.
 * In screens.c these are fun_7b93_present_blank / fun_7bca_flip. */
extern void fun_7b93_present_blank(u16 buf_off, u16 buf_seg, u16 flag);  /* FUN_1000_7b93 */
extern void fun_7bca_flip(u8 page);                                        /* FUN_1000_7bca */

/* Sprite-table selector (screens.c stub name; host_render.c provides the real body
 * under the same symbol — host_set_draw_page — via host_render.c's host_set_draw_page;
 * but the engine symbol is fun_9410_set_sprite_table from screens.c extern block).
 * screens.c declares this as fun_9410_set_sprite_table(u16 arg). */
extern void fun_9410_set_sprite_table(u16 arg);   /* screens.c */

/* wait_50_frames (screens.c stub): wait ~50 display frames. */
extern void wait_50_frames(void);                 /* screens.c */

/* draw_icon_row (screens.c stub): draw the in-game HUD icon row. */
extern void draw_icon_row(void);                  /* screens.c */

/* read_input_action stub used in show_pause_screen (screens.c). */
extern char fun_75a2_poll_action(u8 arg);         /* screens.c / host_input.c */

/* anim_blit_sprite_leaf: the host blit leaf (host_render.c under BUMPY_PLAYABLE;
 * NOP stub in anim.c for the default build).  Declared extern here for the
 * show_text_screen glyph-blit call site. */
extern void anim_blit_sprite_leaf(u16 obj_off, u16 obj_seg);

/* ── Host save-under buffer ────────────────────────────────────────────────────
 * hv_saveunder_buf: 4-plane RAM snapshot of host_framebuffer page-0.
 * setup_fullscreen_view captures it; restore_bg_view can read it back.
 * Sized to hold one full VGA page in planar form: 4 × 8000 = 32000 B.
 * (BGI_PAGE_SIZE = 0x1F40 = 8000 B; BGI_PLANE_SIZE = 0x10000 = host plane stride.) */
static u8 __far *hv_saveunder_buf = (u8 __far *)0;

/* Backing storage for the P2 move-state handler table (p2_state_handler_tbl shadow);
 * seeded in init_sprite_structs.  16 far code-ptr slots, [1..4] = the cell-move
 * handlers (see the wiring note in init_sprite_structs). */
static void (__far *hv_p2_state_handlers[16])(void);
#define HV_SAVEUNDER_SIZE (4u * 0x1F40u)   /* 4 planes × 8000 B = 32000 B */

/* ── init_sprite_structs — 1000:33c5 ───────────────────────────────────────────
 *
 * One-time (game_loop entry) setup of the three sprite-object far pointers.
 * Engine (verbatim from Ghidra decomp):
 *   p1_sprite              = (dword)&sprite_obj_203b_792e;   // DGROUP 0x792e
 *   DAT_203b_7934           = screen_sprite_buf_off;          // obj[+0x06]
 *   DAT_203b_7936           = screen_sprite_buf_seg;          // obj[+0x08]
 *   DAT_203b_7938           = (old & 0x87) | 0x80;            // obj[+0x09] flags
 *   p2_sprite              = (dword)&sprite_obj_203b_795a;   // DGROUP 0x795a
 *   ... (same seed for p2 obj) ...
 *   hud_icon_sprite_ptr     = 0x203b7986;                     // DGROUP 0x7986
 *   ... (same seed for hud obj) ...
 *
 * Host: point p1_sprite → g_entity_dg[DG_P1_OBJ], p2_sprite → g_entity_dg[DG_P2_OBJ].
 * The sprite-sheet buf seed and flag byte are SKIPPED (see RECONSTRUCTION FIDELITY §2).
 *
 * RECONSTRUCTION FIDELITY: SPRITE-OBJ FAR-PTR INIT — see file header note §2.
 * ──────────────────────────────────────────────────────────────────────────── */
void init_sprite_structs(void)
{
    u8 __far *dg;
    u8        i;

    /* ── HOST: wire the anim-channel slot tables (RECONSTRUCTION FIDELITY) ──────────
     * In the engine the channel-A/B slot tables (DGROUP 0x4c70/0x4c72 off/seg and
     * 0x4cbc/0x4cbe) are far-ptr arrays the LEVEL-LOAD path populates with pointers
     * to the per-channel records plus a 0xFF-terminator record (so the steppers,
     * the apply_cell_animation allocator, and the draw/erase `do … while
     * (slot->active != 0xFF)` scans terminate).  The reconstruction keeps the
     * records as anim.c-owned static storage (anim_a_records / anim_b_records +
     * anim_a_terminator / anim_b_terminator) and — exactly as the ctest harnesses
     * (tools/anim_chan_ctest.c, tools/int8_ctest.c) — leaves the table-pointer
     * WIRING to the caller, since a portable static initialiser cannot take the
     * address of a far array element across compilers.  The default BUMPY.EXE link
     * (and its differential ctests) supply this wiring; the playable build had NO
     * such caller, so the tables stayed all-NULL and game_tick()'s
     * draw_anim_channels_a `do … while (active != 0xFF)` never found a terminator →
     * infinite spin (it dereferenced NULL slots forever, never reaching the
     * per-tick loop body).  Wire them here, at init_sprite_structs (game_loop's
     * one-time per-game setup, the faithful point this channel-struct init belongs
     * to), BEFORE start_level / spawn_and_draw_level_entities deref the slots.
     * Recorded in docs/reconstruction-fidelity.md. */
    for (i = 0; i < ANIM_A_SLOTS; i = i + 1) {
        anim_channels_a_tbl[i] = &anim_a_records[i];
    }
    anim_channels_a_tbl[ANIM_A_SLOTS] = &anim_a_terminator;
    for (i = 0; i < ANIM_B_SLOTS; i = i + 1) {
        anim_channels_b_tbl[i] = &anim_b_records[i];
    }
    anim_channels_b_tbl[ANIM_B_SLOTS] = &anim_b_terminator;

    /* ── HOST: wire the P2 move-state handler table (RECONSTRUCTION FIDELITY) ───────
     * p2_run_move_state_handler (player2.c 1000:5003), reached every per-tick when
     * P2 has steps left, dispatches `(*p2_state_handler_tbl[p2_move_state])()` once
     * p2_step_idx==5 — an indirect FAR call through the DGROUP 0x085c shadow table.
     * The engine populates that table at level/round init; the reconstruction keeps
     * it a host-seeded far shadow (player2.c decl note) and — exactly as the
     * differential harness tools/p2_ctest.c (seed_state_handler_tbl) — leaves the
     * WIRING to the caller.  The default BUMPY.EXE link + its p2 ctest supply it; the
     * playable build had NO caller, so p2_state_handler_tbl stayed NULL and the first
     * P2 dispatch did `call far [0000:p2_move_state*4]` → executed the IVT/wild memory
     * → invalid-opcode (INT6) storm (the "reaches mode=4 then hangs" crash).  Seed the
     * four cell-move handlers at their captured/natural indices (1..4), identical to
     * p2_ctest, and point the shadow at it.  Recorded in docs/reconstruction-fidelity.md. */
    for (i = 0; i < 16u; i = i + 1) {
        hv_p2_state_handlers[i] = (void (__far *)(void))0;
    }
    hv_p2_state_handlers[1] = p2_cell_move_up;      /* cell -= 8 (row up)    */
    hv_p2_state_handlers[2] = p2_cell_move_down;    /* cell += 8 (row down)  */
    hv_p2_state_handlers[3] = p2_cell_move_left;    /* cell -= 1 (col left)  */
    hv_p2_state_handlers[4] = p2_cell_move_right;   /* cell += 1 (col right) */
    p2_state_handler_tbl = hv_p2_state_handlers;

    dg = level_get_entity_dg();
    if (dg == (u8 __far *)0) {
        /* DG shadow not yet allocated (level not loaded yet) — NOP.
         * level.c's start_level allocates it before game_loop's first draw calls. */
        return;
    }

    /* p1_sprite → sprite obj at DG_P1_OBJ (DGROUP 0x792e).
     * Mirrors: p1_sprite = (dword)&sprite_obj_203b_792e; */
    p1_sprite = dg + DG_P1_OBJ;

    /* p2_sprite → sprite obj at DG_P2_OBJ (DGROUP 0x795a).
     * Mirrors: p2_sprite = (dword)&sprite_obj_203b_795a; */
    p2_sprite = dg + DG_P2_OBJ;

    /* hud_icon_sprite_ptr (DGROUP 0x7986): not declared in the C source; skipped.
     * No callers in the validated gameplay path.  See RECONSTRUCTION FIDELITY §2. */
}

/* ── init_fullscreen_view_desc — 1000:5181 ─────────────────────────────────────
 *
 * Set up the fullscreen view/blit descriptor at render_descriptor_ptr and call
 * the BGI present path (engine: bgi_set_mode_11_thunk; host: present_frame).
 *
 * Engine decomp (verbatim field writes into render_descriptor_ptr pointee):
 *   [+0x00] = sprite_id (mode arg)        [+0x06] = 0
 *   [+0x08] = 0                            [+0x0e] = field7 (flag arg)
 *   [+0x14] = 0                            [+0x16] = 0
 *   [+0x1c] = 0                            [+0x1e] = 0x14
 *   [+0x20] = 0x19
 *   then: bgi_set_mode_11_thunk(off, seg)   (host: present_frame(1))
 *
 * RECONSTRUCTION FIDELITY: BGI MODE-11 CALL — see file header note §3.
 * ──────────────────────────────────────────────────────────────────────────── */
void init_fullscreen_view_desc(u8 mode, u8 flag)
{
    u8 __far *d;

    if (render_descriptor_ptr == (u8 __far *)0) {
        return;    /* descriptor not yet allocated — NOP */
    }
    d = render_descriptor_ptr;

    /* 1:1 field-writes from the Ghidra decomp. */
    *(u16 __far *)(d + 0x00) = (u16)mode;   /* sprite_id / source page index */
    *(u16 __far *)(d + 0x06) = 0;
    *(u16 __far *)(d + 0x08) = 0;
    *(u16 __far *)(d + 0x0e) = (u16)flag;   /* field7 / NOP guard */
    *(u16 __far *)(d + 0x14) = 0;
    *(u16 __far *)(d + 0x16) = 0;
    *(u16 __far *)(d + 0x1c) = 0;
    *(u16 __far *)(d + 0x1e) = 0x14;
    *(u16 __far *)(d + 0x20) = 0x19;

    /* Engine: bgi_set_mode_11_thunk(off, seg) — dynamically-loaded BGI overlay
     * (1ab9:126e), not reconstructable.  Host: present the composed framebuffer
     * to real VGA.  See RECONSTRUCTION FIDELITY note §3. */
    present_frame(1);
}

/* ── setup_fullscreen_view — 1000:483c ─────────────────────────────────────────
 *
 * Per-load fullscreen background save-under:
 *   1. redraw_level_background_tiles() — rebuild bg tile runs (1:1 call).
 *   2. Build the 0x14 × 0x19 fullscreen view descriptor at render_descriptor_ptr
 *      pointing dest at fullscreen_buf:seg (1:1 from decomp).
 *   3. render_player_view() — copy VGA page-0 → fullscreen_buf (the save-under).
 *
 * Engine decomp (verbatim field writes):
 *   [+0x00]=1  [+0x06]=0  [+0x08]=0
 *   [+0x10]=fullscreen_buf  [+0x12]=fullscreen_buf_seg   (dest far ptr)
 *   [+0x14]=0  [+0x16]=0  [+0x18]=0x14  [+0x1a]=0x19
 *   [+0x1c]=0  [+0x1e]=0x14  [+0x20]=0x19
 *   then: render_player_view(off, seg)
 *
 * Host: descriptor build is 1:1.  render_player_view is driven with the
 * reconstructed 3-arg signature.  Additionally, host_framebuffer page-0 is
 * captured into hv_saveunder_buf for the restore_bg_view erase path.
 *
 * RECONSTRUCTION FIDELITY: FLAT-RAM SAVE-UNDER — see file header note §1.
 * ──────────────────────────────────────────────────────────────────────────── */
void setup_fullscreen_view(void)
{
    u8 __far *d;
    const bgi_view_desc __far *view;
    u8  plane;

    /* Step 1: rebuild background tile runs (1:1 engine call). */
    redraw_level_background_tiles();

    if (render_descriptor_ptr == (u8 __far *)0) {
        return;
    }
    d = render_descriptor_ptr;

    /* Step 2: build the fullscreen view descriptor 1:1 with the engine decomp. */
    *(u16 __far *)(d + 0x00) = 1;                /* source page-index 1 = a000 (page-0) */
    *(u16 __far *)(d + 0x06) = 0;
    *(u16 __far *)(d + 0x08) = 0;
    *(u16 __far *)(d + 0x10) = fullscreen_buf;   /* dest far ptr offset */
    *(u16 __far *)(d + 0x12) = fullscreen_buf_seg;
    *(u16 __far *)(d + 0x14) = 0;
    *(u16 __far *)(d + 0x16) = 0;
    *(u16 __far *)(d + 0x18) = 0x14;
    *(u16 __far *)(d + 0x1a) = 0x19;
    *(u16 __far *)(d + 0x1c) = 0;               /* sub-handler 0 = full 4-plane copy */
    *(u16 __far *)(d + 0x1e) = 0x14;
    *(u16 __far *)(d + 0x20) = 0x19;

    /* Step 3: host flat-RAM save-under capture (RECONSTRUCTION FIDELITY note §1).
     * Engine calls render_player_view(off, seg) which copies VGA a000 → fullscreen_buf.
     * Host: allocate hv_saveunder_buf if needed, then copy host_framebuffer page-0
     * (4 planes × BGI_PAGE_SIZE bytes) into it — the flat-RAM equivalent. */
    if (hv_saveunder_buf == (u8 __far *)0) {
        hv_saveunder_buf = (u8 __far *)_fmalloc(HV_SAVEUNDER_SIZE);
    }
    if (host_framebuffer != (u8 __huge *)0 && hv_saveunder_buf != (u8 __far *)0) {
        /* Copy page-0 of each plane (BGI_PAGE_SIZE = 0x1F40 B per plane)
         * from host_framebuffer into hv_saveunder_buf[plane * 0x1F40].
         * Each plane's page-0 starts at plane*BGI_PLANE_SIZE + BGI_PAGE_A000_OFF. */
        for (plane = 0; plane < 4u; plane++) {
            const u8 __huge *src = host_framebuffer
                                 + (u32)plane * (u32)BGI_PLANE_SIZE
                                 + (u32)BGI_PAGE_A000_OFF;
            u8 __far *dst = hv_saveunder_buf + (u16)plane * (u16)0x1F40u;
            memcpy((void __far *)dst, (const void __huge *)src, (size_t)0x1F40u);
        }
    }

    /* Drive the reconstructed render_player_view with the built descriptor
     * (structural 1:1 call site; in the host the copy lands from host_framebuffer
     * page-0 into the fullscreen_buf dest ptr — the engine's fullscreen_buf is
     * the decoded-image buffer, not planar VGA, so this diverges structurally but
     * the critical save-under is the hv_saveunder_buf copy above). */
    view = (const bgi_view_desc __far *)d;
    if (host_framebuffer != (u8 __huge *)0) {
        render_player_view(host_framebuffer, host_framebuffer, view);
    }
}

/* ── show_text_screen — 1000:11eb ───────────────────────────────────────────────
 *
 * Display a fullscreen image (resource 3) + render 9 sprite-glyph characters at
 * row 0x60, then idle 2 × 50 frames.
 *
 * Structural port of 1000:11eb.  Resource-load / BGI-overlay / DAC leaves are
 * faithful stubs (screens.c); the p1_sprite glyph-blit path is live.
 * The text-buf data comes from a DGROUP stack-local (fmemcpy in the engine); in the
 * host this buffer is not accessible, so glyphs render as spaces (0x20) — runtime
 * text correctness deferred to Task 9/11.
 *
 * RECONSTRUCTION FIDELITY: resource-load / BGI-overlay leaves stubbed (same
 * convention as screens.c screen fns).  text_buf not accessible in host model.
 * ──────────────────────────────────────────────────────────────────────────── */
void show_text_screen(void)
{
    u16 __far *p;
    u8   char_idx;
    u8   col_pos;
    u8   ch;

    /* Engine: set_resource_table + set_sprite_table_ptr + load resource 3 into
     * fullscreen_buf.  Stubbed (resource-load path not yet live). */

    /* Iris-wipe in, present fullscreen image + upload DAC palette.
     * Engine calls bgi_overlay_thunk_01e1 + _02b1 (Ghidra names); in screens.c
     * these are fun_7b93_present_blank + fun_7bca_flip (the reconstructed names). */
    play_iris_wipe_transition();
    fun_7b93_present_blank(fullscreen_buf, fullscreen_buf_seg, 0);
    fun_7bca_flip(0);
    upload_vga_dac_palette();

    /* Render 9 sprite glyphs at row 0x60 starting at col 6.
     * p1_sprite word[1] = y = 0x60; per char: x = col * 16, frame = ch + 0x175.
     * Skip spaces (ch == 0x20).  text_buf: DGROUP stack-local in engine; not
     * accessible in host — default all chars to space, nothing blitted. */
    col_pos = 6;
    if (p1_sprite != (u8 __far *)0) {
        p = (u16 __far *)p1_sprite;
        p[1] = 0x60;                          /* y = 0x60 (obj word at +0x02) */
        for (char_idx = 0; char_idx < 9; char_idx = char_idx + 1) {
            ch = 0x20;                        /* default: space (text_buf not live) */
            p[2] = (u16)(ch + 0x175u);        /* frame */
            p[0] = (u16)((u16)col_pos << 4);  /* x = col * 16 */
            if (ch != 0x20) {
                anim_blit_sprite_leaf(0x792e, 0x203b);
            }
            col_pos = col_pos + 1;
        }
    }

    /* Idle 2 × 50-frame delays. */
    for (char_idx = 0; char_idx < 2; char_idx = char_idx + 1) {
        wait_50_frames();
    }
    fun_9410_set_sprite_table(1);
}

/* ── show_pause_screen — 1000:49d7 ─────────────────────────────────────────────
 *
 * Pause/status overlay: save a 0x14 × 1 strip, draw score panel + HUD icon row,
 * poll for resume (scancode 0x19) or quit (fun_75a2_poll_action), optionally
 * execute the tileflip cheat (scancodes 0x1d + 0x21), then restore the bg view.
 *
 * Structural 1:1 port of 1000:49d7.  render_player_view / restore_bg_view use
 * the reconstructed host 3-arg signatures.  The tileflip cheat body (walk
 * move_descriptor_table) is structurally present but the far ptr is not exposed
 * from player.c here — deferred to Task 9/11 (playable boot).
 *
 * RECONSTRUCTION FIDELITY: move_descriptor_table cheat path deferred (not in
 * validated gameplay path).  Runtime pause correctness deferred to Task 9/11.
 * ──────────────────────────────────────────────────────────────────────────── */
void show_pause_screen(void)
{
    u8  key_state;
    char quit_pressed;
    u8 __far *d;
    const bgi_view_desc __far *view;

    if (render_descriptor_ptr == (u8 __far *)0) {
        return;
    }
    d = render_descriptor_ptr;
    view = (const bgi_view_desc __far *)d;

    /* Build the pause-overlay source-copy descriptor (1:1 from decomp):
     * word[0]=0 (source page-1 = a200), dest=0x9694:0x203b, 0x14 × 1 rect,
     * sub-handler=0 (full copy). */
    *(u16 __far *)(d + 0x00) = 0;
    *(u16 __far *)(d + 0x06) = 0;
    *(u16 __far *)(d + 0x08) = 0;
    *(u16 __far *)(d + 0x10) = 0x9694u;
    *(u16 __far *)(d + 0x12) = 0x203bu;
    *(u16 __far *)(d + 0x14) = 0;
    *(u16 __far *)(d + 0x16) = 0;
    *(u16 __far *)(d + 0x18) = 0x14u;
    *(u16 __far *)(d + 0x1a) = 1;
    *(u16 __far *)(d + 0x1c) = 0;
    *(u16 __far *)(d + 0x1e) = 0x14u;
    *(u16 __far *)(d + 0x20) = 1;

    /* render_player_view: copy the status-strip from the current draw page. */
    if (host_framebuffer != (u8 __huge *)0) {
        render_player_view(host_framebuffer, host_framebuffer, view);
    }

    /* Switch to visible page 0 + sprite table 0 (draw to page1). */
    set_display_page(0);
    fun_9410_set_sprite_table(0);

    /* Draw score + HUD icon row. */
    draw_number(score_lo, score_hi, 7, 0, 7);
    draw_icon_row();

    /* Restore draw page. */
    fun_9410_set_sprite_table(1);
    set_display_page(1);

    /* Wait for pause key (0x19) to be RELEASED before entering poll loop. */
    do {
        key_state = get_key_state(0x19);
    } while (key_state != '\0');

    /* Poll until resume key pressed (0x19) OR action input (fun_75a2_poll_action). */
    while ((key_state = get_key_state(0x19), key_state == '\0') &&
           (quit_pressed = fun_75a2_poll_action(0), quit_pressed == '\0')) {
        key_state = get_key_state(0x1d);
        if (key_state != '\0') {
            key_state = get_key_state(0x21);
            if (key_state != '\0') {
                /* Cheat: fill move_descriptor_table entries (far ptr from level.c).
                 * move_descriptor_table not directly accessible here; deferred. */
            }
        }
    }

    /* Wait for all keys + action to be released. */
    do {
        do {
            key_state = get_key_state(0x19);
        } while (key_state != '\0');
        quit_pressed = fun_75a2_poll_action(0);
    } while (quit_pressed != '\0');

    /* Build the restore-view descriptor (1:1 from decomp): source=0x9694:0x203b,
     * word0e=0 (restore to page a200), 0x14 × 1 rect. */
    *(u16 __far *)(d + 0x02) = 0x9694u;
    *(u16 __far *)(d + 0x04) = 0x203bu;
    *(u16 __far *)(d + 0x06) = 0;
    *(u16 __far *)(d + 0x08) = 0;
    *(u16 __far *)(d + 0x0a) = 0x14u;
    *(u16 __far *)(d + 0x0c) = 1;
    *(u16 __far *)(d + 0x0e) = 0;           /* word0e = 0 → restore to a200 page */
    *(u16 __far *)(d + 0x14) = 0;
    *(u16 __far *)(d + 0x16) = 0;

    /* restore_bg_view: restore the background at the saved-under area.
     * Host: vga_src = hv_saveunder_buf (flat-RAM save-under; engine uses fullscreen_buf).
     * word0e=0 → dest page a200:0000 (plane offset 0x2000) in host_framebuffer. */
    if (host_framebuffer != (u8 __huge *)0 && hv_saveunder_buf != (u8 __far *)0) {
        restore_bg_view(host_framebuffer,
                        (const u8 __huge *)hv_saveunder_buf,
                        (const bgi_view_desc __far *)d);
    } else if (host_framebuffer != (u8 __huge *)0) {
        /* hv_saveunder_buf not yet allocated — drive the call structurally with
         * a NOP-guarded view (word0e=0 ≤ 1, but vga_src is null; guard will pass).
         * Worst case: no restore (deferred to Task 9/11 when save-under is live). */
        restore_bg_view(host_framebuffer, host_framebuffer,
                        (const bgi_view_desc __far *)d);
    }
}

#endif /* BUMPY_PLAYABLE */
