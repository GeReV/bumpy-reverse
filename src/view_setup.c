#ifdef BUMPY_PLAYABLE
#include <string.h>
#include <malloc.h>           /* _fmalloc */
#include <conio.h>            /* inp/outp — mode-11 latch-copy GC programming */
#include <i86.h>              /* MK_FP */
#include "host/host.h"        /* host_framebuffer, VGA constants */
#include "bumpy.h"
#include "gfx_overlay.h"      /* gfx_view_desc, render_player_view, restore_bg_view,
                                  GFX_PAGE_A000_OFF, GFX_PLANE_SIZE, GFX_PAGE_SIZE */
#include "screens.h"          /* render_descriptor_ptr, fullscreen_buf/seg,
                                  palette_mode, draw_number, wait_vretrace_thunk,
                                  play_iris_wipe_transition */
#include "anim.h"             /* p1_sprite */

/* P2 move-state handler table + its four cell-move handlers (player2.c).  Declared
 * locally rather than via player2.h: that header and game.h carry an inconsistent
 * p2_step_scripted_move prototype that only collides when both are pulled into one
 * TU, and host_view.c only needs these five symbols. */
extern void (__far * __far *p2_state_handler_tbl)(void);
extern void p2_cell_move_up(void);
extern void p2_cell_move_down(void);
extern void p2_cell_move_left(void);
extern void p2_cell_move_right(void);
#include "level.h"            /* level_get_entity_dg, DG_P1_OBJ, DG_P2_OBJ */
#include "entity.h"           /* sprite_obj_t */
#include "input.h"            /* get_key_state */
#include "game.h"             /* present_frame, set_display_page, draw_number */
/* NOTE: player2.h and game.h declare p2_step_scripted_move with inconsistent
 * return types (void vs u8); we must NOT include player2.h when game.h is present.
 * Use an explicit extern for p2_sprite only. */

/* ── Explicit extern to avoid header conflicts ──────────────────────────── */
extern u8 __far *p2_sprite;   /* player2.c DGROUP 0x9b9e/0x9ba0 */

/* ============================================================================
 * view_setup.c — view/setup leaves + background save-under  (Plan A, Task 7)
 * ============================================================================
 *
 * These are reconstructed ENGINE functions (view/screen setup), not host platform
 * glue — so they live in src/ proper, not src/host/.  They implement the five
 * view/setup/screen leaves that game_stubs.c stubs as carve-outs in the default
 * build; under -dBUMPY_PLAYABLE these real bodies replace the NOPs:
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
 * │    (a000:0000) into fullscreen_buf via render_player_view + the gfx       │
 * │    overlay's mode-10 subhandler-0 full-plane copy.  In the host flat-RAM  │
 * │    model we additionally copy host_framebuffer's page-0 region (4 planes  │
 * │    × GFX_PAGE_SIZE = 8000 B each) into hv_saveunder_buf.  The view-desc  │
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
 * │    screen_sprite_buf is a gfx-overlay ptr unused by the planar pipeline.  │
 * │    hud_icon_sprite_ptr (obj at 0x7986) has no C declaration; skipped.     │
 * │    Recorded in docs/reconstruction-fidelity.md (Playable host / Task 7). │
 * │                                                                            │
 * │ 3. GFX-OVERLAY MODE-11 CALL (init_fullscreen_view_desc):                   │
 * │    The engine ends init_fullscreen_view_desc with gfx_set_mode_11_thunk    │
 * │    (→ gfx_set_mode_11 1ab9:126e, dynamically-loaded gfx overlay code).    │
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
void redraw_level_background_tiles(void)
{
    /* The engine's redraw_level_background_tiles (1000:2a0a) rebuilds the level's bg tile
       runs — driven by setup_fullscreen_view at GAME-ENTRY (inside spawn_and_draw_level_
       entities, after the iris).  The recon renders the tiles via the semantic
       level_render_bg() (clear a000 + bg_render_grid) rather than the original's
       self-modifying graphics-overlay tile-run chain (setup_sprite_descriptor/restore_bg_tile_run,
       which do not decompile).  This is where the playable level bg is painted now —
       render_level (start_level) only BINDS the context (no early paint → no level flash
       before the overworld, and the bg survives for gameplay).  docs/reconstruction-fidelity.md. */
    extern void level_render_bg(void);   /* level.c */
    level_render_bg();
}

/* graphics-overlay screen present thunks (stubs in screens.c).
 * show_text_screen calls these after loading the fullscreen image.
 * In screens.c these are gfx_stage_image_palette_thunk / gfx_upload_palette_to_dac_thunk. */
extern void gfx_stage_image_palette_thunk(u16 buf_off, u16 buf_seg, u16 flag);  /* 1000:7b93 */
extern void gfx_upload_palette_to_dac_thunk(u8 page);                          /* 1000:7bca */

/* Sprite-table selector (screens.c stub name; host_render.c provides the real body
 * under the same symbol — host_set_draw_page — via host_render.c's host_set_draw_page;
 * but the engine symbol is set_sprite_table_ptr from screens.c extern block).
 * screens.c declares this as set_sprite_table_ptr(u16 arg). */
extern void set_sprite_table_ptr(u16 arg);   /* screens.c */

/* wait_50_frames (screens.c stub): wait ~50 display frames. */
extern void wait_50_frames(void);                 /* screens.c */

/* draw_icon_row (screens.c stub): draw the in-game HUD icon row. */
extern void draw_icon_row(void);                  /* screens.c */

/* read_input_action_byte stub used in show_pause_screen (screens.c). */
extern char read_input_action_byte(u8 arg);            /* screens.c / host_input.c */

/* anim_blit_sprite_leaf: the host blit leaf (host_render.c under BUMPY_PLAYABLE;
 * NOP stub in anim.c for the default build).  Declared extern here for the
 * show_text_screen glyph-blit call site. */
extern void anim_blit_sprite_leaf(u16 obj_off, u16 obj_seg);

/* Resource loader + EGA palette-patch table used by show_text_screen (below), mirroring
 * show_menu_select_screen (screens.c).  open_resource/read_chunked/c_close have real
 * bodies in host/host_resource.c under BUMPY_PLAYABLE (faithful-signature stubs in the
 * default build).  dgroup_pal_patch_71e (DGROUP 0x71e) is the reddish EGA AC-index table
 * — the SAME table show_text_screen (1000:11eb) and show_menu_select_screen (1000:0f7a)
 * both copy into the loaded image's embedded palette at +0x23 when palette_mode==1. */
extern int  open_resource(u16 res_idx, u16 mode);
extern u32  read_chunked(int handle, u16 buf_off, u16 buf_seg, u16 len_off, u16 len_seg);
extern void c_close(int handle);
extern u8   dgroup_pal_patch_71e[16];   /* DGROUP 0x71e — screens.c */

/* ── Host save-under buffer ────────────────────────────────────────────────────
 * hv_saveunder_buf: 4-plane RAM snapshot of host_framebuffer page-0.
 * setup_fullscreen_view captures it; restore_bg_view can read it back.
 * Sized to hold one full VGA page in planar form: 4 × 8000 = 32000 B.
 * (GFX_PAGE_SIZE = 0x1F40 = 8000 B; GFX_PLANE_SIZE = 0x10000 = host plane stride.) */
static u8 __far *hv_saveunder_buf = (u8 __far *)0;

/* Backing storage for the P2 move-state handler table (p2_state_handler_tbl shadow);
 * seeded in init_sprite_structs.  16 far code-ptr slots, [1..4] = the cell-move
 * handlers (see the wiring note in init_sprite_structs). */
static void (__far *hv_p2_state_handlers[16])(void);

/* No-op for the table slots outside the four cell-move handlers.  GROUNDED
 * (2026-07-03): the engine's 0x85c table is STATIC DGROUP data in the image (file
 * 0x11440+0x85c, 10 NEAR code-ptr entries, no runtime writer): [1..4] = the four
 * cell-move handlers (1000:5025/503f/5059/506f) and [0],[5..9] = the compiled EMPTY
 * fn at 1000:7111 — so seeding every non-move slot with a no-op IS the faithful
 * table (slots 10..15 here are only harness headroom, never indexed: move states
 * are 1..9).  Recorded in docs/reconstruction-fidelity.md. */
static void __far hv_p2_state_noop(void) {}
#define HV_SAVEUNDER_SIZE (4u * VGA_PLANE_BYTES)   /* 4 planes × 8000 B = 32000 B */

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
        hv_p2_state_handlers[i] = hv_p2_state_noop; /* no NULL slots (see note) */
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

    /* hud_icon_sprite_ptr → HUD icon-row obj at DGROUP 0x7986 (draw_icon_row, 1000:6130,
     * called from level_intro_screen + show_pause_screen).
     * Mirrors: hud_icon_sprite_ptr = (dword)&sprite_obj_203b_7986; */
    {
        extern u8 __far *hud_icon_sprite_ptr;   /* anim.c */
        hud_icon_sprite_ptr = dg + DG_HUD_ICON_OBJ;
    }
}

/* ── init_fullscreen_view_desc — 1000:5181 ─────────────────────────────────────
 *
 * Set up the fullscreen view/blit descriptor at render_descriptor_ptr and call
 * the graphics-overlay present path (engine: gfx_set_mode_11_thunk; host: present_frame).
 *
 * Engine decomp (verbatim field writes into render_descriptor_ptr pointee):
 *   [+0x00] = sprite_id (mode arg)        [+0x06] = 0
 *   [+0x08] = 0                            [+0x0e] = field7 (flag arg)
 *   [+0x14] = 0                            [+0x16] = 0
 *   [+0x1c] = 0                            [+0x1e] = 0x14
 *   [+0x20] = 0x19
 *   then: gfx_set_mode_11_thunk(off, seg)   (host: present_frame(1))
 *
 * RECONSTRUCTION FIDELITY: GFX-OVERLAY MODE-11 CALL — see file header note §3.
 * ──────────────────────────────────────────────────────────────────────────── */
void init_fullscreen_view_desc(u8 mode, u8 flag)
{
    screen_view_desc __far *d;

    if (render_descriptor_ptr == (u8 __far *)0) {
        return;    /* descriptor not yet allocated — NOP */
    }
    d = (screen_view_desc __far *)render_descriptor_ptr;

    /* 1:1 field-writes from the Ghidra decomp. */
    d->mode = (u16)mode;   /* sprite_id / source page index */
    d->src_x = 0;
    d->src_y = 0;
    d->flag = (u16)flag;   /* field7 / NOP guard */
    d->dest_x = 0;
    d->dest_y = 0;
    d->subhandler = 0;
    d->clip_w = SCREEN_W_TILES;
    d->clip_h = SCREEN_H_TILES;

    /* Engine: gfx_set_mode_11_thunk(off, seg) — the FULL-SCREEN PAGE SYNC: copy
     * page[word00=mode] → page[word0e=flag] (e.g. (1,0) copies the just-composed
     * gameplay page onto the other page so BOTH hold the clean screen before the
     * sprite draws + the first present flip).  A prior revision routed this to
     * present_frame(1) — harmless while present was a NOP, but wrong (and a stray
     * page flip) under the real flip model; corrected 2026-07-02 with the real
     * copy against the LIVE page table. */
    {
        /* PARITY-HARDENED SOURCE — copy the JUST-COMPOSED page onto the other page
           so BOTH VGA pages hold the clean screen (the engine's page[table[mode]] →
           page[table[flag]] intent).
           WHY NOT host_page_off_of(mode)/(flag): the host's draw-page index
           (hr_cur_page_idx) is NOT kept in lockstep with the engine's descriptor page
           convention.  At the (1,0) game/overworld-entry callers the screen is composed
           on slot 0 (they bracket their draw with set_sprite_table_ptr(0)), but
           mode=1 resolves to slot 1 — the OTHER, non-composed page — so a literal
           host_page_off_of(mode) sourced the WRONG page (and host_page_off_of(flag=0)
           targeted the draw page itself).  While the iris wipe was a NOP that page held
           a near-identical previous bg, so the mis-source was invisible; once the
           reconstructed GEOMETRIC iris (host_gfx_set_viewport) paints real BLACK there,
           the sync propagated BLACK over the freshly-composed page → the overworld walk
           "freezes on the frame-before-last" and the in-level save-under captures black
           under the moving sprite → sprite flicker.
           Sourcing the LIVE draw page (unambiguously "the just-composed screen") and
           targeting its complement equals host_page_off_of(mode)/(flag) whenever host
           page parity matches (the menu (0,1) sync — behaviour unchanged) and corrects
           it when it doesn't (the (1,0) entries).  All three callers compose to the draw
           page before this sync, so sourcing it is correct-by-construction.
           DO NOT revert to host_page_off_of(mode): this regression has RECURRED several
           times.  It kept getting reverted because it was masked by two confounds — the
           world-2 background bug (PAV read truncation, fixed 2026-07-07) and a headless
           input-injection "phantom freeze" — both now removed.  RECONSTRUCTION FIDELITY:
           docs/reconstruction-fidelity.md. */
        u16 src = host_draw_page_off();
        u16 dst = (u16)(src ^ 0x2000u);
        {
            /* VGA LATCH COPY (write mode 1): one read loads all 4 planes into the
               latches, one write stores them — the mode the hardware provides for
               exactly this page-to-page copy (the overlay's own copy is a rep-movs
               latch loop). */
            const u8 __far *sp = (const u8 __far *)MK_FP(VGA_SEG_PAGE0, src);
            u8 __far       *dp = (u8 __far *)MK_FP(VGA_SEG_PAGE0, dst);
            u16 off;
            outp(SEQ_INDEX, SEQ_MAP_MASK); outp(SEQ_DATA, SEQ_MAP_ALL_PLANES); /* all planes */
            outp(GC_INDEX, GC_MODE);       outp(GC_DATA, 0x01u);   /* write mode 1  */
            for (off = 0u; off < (u16)VGA_PLANE_BYTES; off++) {
                dp[off] = sp[off];                     /* latch copy, 4 planes   */
            }
            outp(GC_INDEX, GC_MODE);       outp(GC_DATA, 0x00u);   /* back to write mode 0 */
            host_vga_blit_end();                       /* Bit-Mask FF / Map 0F   */
        }
    }
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
    screen_view_desc __far *d;
    const gfx_view_desc __far *view;

    /* Step 1: rebuild background tile runs (1:1 engine call). */
    redraw_level_background_tiles();

    if (render_descriptor_ptr == (u8 __far *)0) {
        return;
    }
    d = (screen_view_desc __far *)render_descriptor_ptr;

    /* Step 2: build the fullscreen view descriptor 1:1 with the engine decomp. */
    d->mode = 1;                /* source page-index 1 = a000 (page-0) */
    d->src_x = 0;
    d->src_y = 0;
    d->blit_off = fullscreen_buf;   /* dest far ptr offset */
    d->blit_seg = fullscreen_buf_seg;
    d->dest_x = 0;
    d->dest_y = 0;
    d->sub_w = SCREEN_W_TILES;
    d->sub_h = SCREEN_H_TILES;
    d->subhandler = 0;               /* sub-handler 0 = full 4-plane copy */
    d->clip_w = SCREEN_W_TILES;
    d->clip_h = SCREEN_H_TILES;

    /* Step 3: clean-background capture (RECONSTRUCTION FIDELITY note §1).
     * Engine calls render_player_view(off, seg) which copies VGA a000 → fullscreen_buf.
     * Host: allocate hv_saveunder_buf if needed, then read the freshly-painted a000
     * page-0 (4 planes × GFX_PAGE_SIZE bytes, via host_vga_read4) into it.
     * (2026-07-02 FIX: this previously memcpy'd from the flat host_framebuffer, which
     * the real-VGA blitters no longer write — the snapshot was all zeros and the
     * anim-channel erase repainted black.  redraw_level_background_tiles above has
     * just painted the level bg to VGA, so a000 page-0 IS the clean background.) */
    if (hv_saveunder_buf == (u8 __far *)0) {
        hv_saveunder_buf = (u8 __far *)_fmalloc(HV_SAVEUNDER_SIZE);
    }
    if (hv_saveunder_buf != (u8 __far *)0) {
        u16 off;
        u16 pg = host_draw_page_off();   /* the page the bg was just painted to */
        for (off = 0u; off < (u16)VGA_PLANE_BYTES; off++) {
            host_vga_read4((u16)(pg + off),
                           &hv_saveunder_buf[off],
                           &hv_saveunder_buf[(u16)VGA_PLANE_BYTES + off],
                           &hv_saveunder_buf[2u * (u16)VGA_PLANE_BYTES + off],
                           &hv_saveunder_buf[3u * (u16)VGA_PLANE_BYTES + off]);
        }
    }

    /* Structural 1:1 call site: the engine drives render_player_view with the built
     * descriptor to fill fullscreen_buf.  The host's clean-bg equivalent is the
     * hv_saveunder_buf VGA capture above; the flat host_framebuffer copy this used
     * to do was orphaned by the real-VGA migration and is dropped. */
    view = (const gfx_view_desc __far *)d;
    (void)view;
}

/* ── host_clean_bg — accessor for the flat-RAM clean-background save-under ───────
 * Returns the page-0 clean-background snapshot captured by setup_fullscreen_view
 * (hv_saveunder_buf), or NULL if not yet captured.  This is the host's stand-in for
 * the engine's fullscreen_buf clean-bg image (a000 gameplay is single-page — see
 * host_video.c present_frame).  The anim-channel erase leaf (host_render.c) reads it
 * to repaint the background under active-platform sprites, mirroring the engine's
 * restore_bg_view(fullscreen_buf) erase in draw_anim_channels_a (anim.c:422).
 * BUMPY_PLAYABLE only. */
const u8 __far *host_clean_bg(void)
{
    return hv_saveunder_buf;
}

/* ── show_text_screen — 1000:11eb ───────────────────────────────────────────────
 *
 * Display a fullscreen image (resource 3) + render 9 sprite-glyph characters at
 * row 0x60, then idle 2 × 50 frames.
 *
 * Structural port of 1000:11eb.  Resource-load (open_resource/read_chunked/c_close)
 * and the palette_mode==1 EGA palette patch are now LIVE via the host resource loader
 * (host_resource.c) + the DGROUP 0x71e AC table (screens.c) — a 1:1 mirror of the
 * sibling show_menu_select_screen (they share resource 3 and the 0x71e palette).  The
 * graphics-overlay present/flip leaves route through screens.c's gfx_stage_image_palette_thunk/gfx_upload_palette_to_dac_thunk
 * host bodies (host_gfx.c); the p1_sprite glyph-blit path is live.
 * The engine fmemcpy's the far ptr at DGROUP 0x11ae into a stack-local text_buf
 * ptr and renders text_buf[0..8]; statically 0x11ae = {0x1327, DGROUP} — the
 * string "GAME OVER" at DGROUP 0x1327.  Modelled here as a file-static string +
 * far-ptr pair (hv_text_ptr_11ae) the loop reads through, mirroring the engine's
 * indirection (nothing else writes 0x11ae in the corpus).
 *
 * FIX 2026-07-12: the load + start draw-page bracket + EGA palette patch were
 * previously stubbed here, so the Game Over screen showed the stale gameplay palette
 * (bluish, not the image's red) and no visible iris (the wipe drew onto the wrong
 * page).  Restored 1:1 from the 1000:11eb disassembly.  See docs/reconstruction-fidelity.md.
 * ──────────────────────────────────────────────────────────────────────────── */

/* DGROUP 0x1327: the "GAME OVER" string; DGROUP 0x11ae: the far ptr to it that
 * show_text_screen copies into its stack-local text_buf. */
static const char hv_text_str_1327[] = "GAME OVER";
static const char __far *hv_text_ptr_11ae = hv_text_str_1327;

void show_text_screen(void)
{
    u8   char_idx;
    u8   col_pos;
    u8   ch;
    int  res_handle;
    u8   pal_idx;

    /* ── Engine prologue (1000:11eb, verbatim order) ────────────────────────────
     * Select the 0x928 (title) resource base, bracket the draw onto slot 0, load
     * resource 3 (the fullscreen background — the SAME image show_menu_select_screen
     * uses), and, for EGA, patch its embedded palette.  This whole block was previously
     * stubbed here ("resource-load path not yet live"), which is why the Game Over
     * screen showed the stale gameplay palette (bluish, not the image's red) and no
     * visible iris.  It is now a 1:1 mirror of show_menu_select_screen (screens.c). */
    set_resource_table(0x928, host_dgroup_seg());   /* 1000:120c — hr_base_idx = title */

    /* 1000:1217 — set_sprite_table_ptr(0): bracket the draw onto slot 0 (the page the
     * mode-11 sync READS FROM) BEFORE the iris.  Without this start bracket the iris +
     * present drew onto the stale gameplay draw page, so the wipe was invisible.  The
     * binary ends the routine with (1) — the (0)…(1) UI bracket (see screens.c). */
    set_sprite_table_ptr(0);

    /* 1000:1235 — load resource 3 into fullscreen_buf (raw decoded image: 16-byte EGA
     * AC-index palette at +0x23, 48-byte VGA DAC palette at +0x33, planar raster at +99).
     * No vec_decode: resource 3 is already in decoded form (matches the binary). */
    res_handle = open_resource(3, 4);
    read_chunked(res_handle, fullscreen_buf, fullscreen_buf_seg, 99, 0);
    c_close(res_handle);

    /* 1000:125a — EGA (palette_mode==1): overwrite the loaded image's embedded 16-byte
     * palette (img+0x23) with the DGROUP 0x71e AC-index table (the reddish Game-Over /
     * menu-select palette).  host_gfx_stage_image_palette then stages +0x23 and
     * host_gfx_upload_palette_to_dac programs the 16 Attribute-Controller regs from it.
     * VGA (palette_mode==2) skips this and uses the image's own +0x33 DAC palette. */
    if (palette_mode == 1) {
        u8 __far *img = (u8 __far *)
            (((u32)fullscreen_buf_seg << 16) | (u32)fullscreen_buf);
        for (pal_idx = 0; pal_idx < 0x10; pal_idx = pal_idx + 1) {
            img[(u16)pal_idx + 0x23] = dgroup_pal_patch_71e[pal_idx];
        }
    }

    /* Iris-wipe in, present fullscreen image + upload DAC palette.
     * Engine calls gfx_overlay_thunk_01e1 + _02b1 (Ghidra names); in screens.c
     * these are gfx_stage_image_palette_thunk + gfx_upload_palette_to_dac_thunk (the reconstructed names). */
    play_iris_wipe_transition();
    gfx_stage_image_palette_thunk(fullscreen_buf, fullscreen_buf_seg, 0);
    gfx_upload_palette_to_dac_thunk(0);
    wait_vretrace_thunk();   /* vsync wait (1000:9864); formerly mis-named upload_vga_dac_palette */

    /* Render 9 sprite glyphs at row 0x60 starting at col 6.
     * p1_sprite.y = 0x60; per char: x = col * 16, frame = ch + 0x175.
     * Skip spaces (ch == 0x20).  text_buf = the DGROUP 0x11ae far ptr copied to a
     * stack-local in the engine; here read through hv_text_ptr_11ae ("GAME OVER"). */
    col_pos = 6;
    if (p1_sprite != (u8 __far *)0) {
        sprite_obj_t __far *so = (sprite_obj_t __far *)p1_sprite;
        so->y = 0x60;
        for (char_idx = 0; char_idx < 9; char_idx = char_idx + 1) {
            ch = (u8)hv_text_ptr_11ae[char_idx];
            so->frame = (u16)(ch + 0x175u);
            so->x     = (s16)((u16)col_pos << 4);
            if (ch != 0x20) {
                /* blit_sprite(DG_P1_OBJ, DS) — engine stamps the static DGROUP 0x203b;
                 * pass the loaded image's real DGROUP (host_dgroup_seg convention). */
                anim_blit_sprite_leaf(DG_P1_OBJ, host_dgroup_seg());
            }
            col_pos = col_pos + 1;
        }
    }

    /* Idle 2 × 50-frame delays. */
    for (char_idx = 0; char_idx < 2; char_idx = char_idx + 1) {
        wait_50_frames();
    }
    set_sprite_table_ptr(1);
}

/* ── show_pause_screen — 1000:49d7 ─────────────────────────────────────────────
 *
 * Pause/status overlay: save a 0x14 × 1 strip, draw score panel + HUD icon row,
 * poll for resume (scancode 0x19) or quit (read_input_action_byte), optionally
 * execute the tileflip cheat (scancodes 0x1d + 0x21), then restore the bg view.
 *
 * Structural 1:1 port of 1000:49d7.  The tileflip cheat body (walk
 * move_descriptor_table) is structurally present but the far ptr is not exposed
 * from player.c here — deferred to Task 9/11 (playable boot).
 *
 * RECONSTRUCTION FIDELITY: move_descriptor_table cheat path deferred (not in
 * validated gameplay path).  The strip SAVE/RESTORE (engine: render_player_view /
 * restore_bg_view driven by the descriptor below, word00/word0e = 0 → page[table[0]])
 * is executed as a direct VGA 4-plane copy between page[table[0]] and hv_pause_strip
 * (the model of the engine's DGROUP 0x9694 save buffer) — the descriptor-driven
 * graphics-overlay copy leaves are not live on the real-VGA host; the descriptor field
 * writes are kept 1:1 as documentation.
 * ──────────────────────────────────────────────────────────────────────────── */

/* Engine DGROUP 0x9694: the 0x500-byte pause strip save buffer — 0x14 tiles × 1 tile
 * row = 40 bytes × 8 rows × 4 planes, PLANE-MAJOR (320 bytes per plane). */
#define PAUSE_STRIP_DGROUP_OFF 0x9694u
#define PAUSE_STRIP_PLANE_SIZE (8u * VGA_ROW_BYTES)   /* 8 rows x 40 B/row = 320 B/plane */
#define SCANCODE_PAUSE   0x19u
#define SCANCODE_CHEAT_1 0x1du
#define SCANCODE_CHEAT_2 0x21u
static u8 hv_pause_strip[4u * PAUSE_STRIP_PLANE_SIZE];

void show_pause_screen(void)
{
    u8  key_state;
    char quit_pressed;
    screen_view_desc __far *d;
    u16 pg;
    u8  r;
    u8  c;

    if (render_descriptor_ptr == (u8 __far *)0) {
        return;
    }
    d = (screen_view_desc __far *)render_descriptor_ptr;

    /* Build the pause-overlay source-copy descriptor (1:1 from decomp):
     * word[0]=0 (source page-1 = a200), dest=0x9694:0x203b, 0x14 × 1 rect,
     * sub-handler=0 (full copy). */
    d->mode = 0;
    d->src_x = 0;
    d->src_y = 0;
    d->blit_off = PAUSE_STRIP_DGROUP_OFF;
    d->blit_seg = ENGINE_STATIC_DGROUP_SEG;
    d->dest_x = 0;
    d->dest_y = 0;
    d->sub_w = SCREEN_W_TILES;
    d->sub_h = 1;
    d->subhandler = 0;
    d->clip_w = SCREEN_W_TILES;
    d->clip_h = 1;

    /* SAVE (engine: render_player_view with the descriptor above, word00=0 →
     * source page[table[0]]): read the 0x14×1-tile strip at (0,0) — 40 bytes ×
     * 8 rows × 4 planes — from VGA into hv_pause_strip, plane-major (320 B/plane),
     * mirroring the engine's copy into DGROUP 0x9694.  page[table[0]] is resolved
     * LIVE (host_page_off_of(0)); at this point it is the DISPLAYED page the pause
     * UI below draws onto inside the set_sprite_table_ptr(0)…(1) bracket. */
    pg = host_page_off_of(0);
    for (r = 0; r < 8u; r++) {
        for (c = 0; c < VGA_ROW_BYTES; c++) {
            u16 i = (u16)((u16)r * VGA_ROW_BYTES + c);
            host_vga_read4((u16)(pg + i),
                           &hv_pause_strip[i],
                           &hv_pause_strip[PAUSE_STRIP_PLANE_SIZE + i],
                           &hv_pause_strip[2u * PAUSE_STRIP_PLANE_SIZE + i],
                           &hv_pause_strip[3u * PAUSE_STRIP_PLANE_SIZE + i]);
        }
    }

    /* Switch to visible page 0 + sprite table 0 (draw to page1). */
    set_display_page(0);
    set_sprite_table_ptr(0);

    /* Draw score + HUD icon row. */
    draw_number(score_lo, score_hi, 7, 0, 7);
    draw_icon_row();

    /* Restore draw page. */
    set_sprite_table_ptr(1);
    set_display_page(1);

    /* Wait for pause key (0x19) to be RELEASED before entering poll loop. */
    do {
        key_state = get_key_state(SCANCODE_PAUSE);
    } while (key_state != '\0');

    /* Poll until resume key pressed (0x19) OR action input (read_input_action_byte). */
    while ((key_state = get_key_state(SCANCODE_PAUSE), key_state == '\0') &&
           (quit_pressed = read_input_action_byte(0), quit_pressed == '\0')) {
        key_state = get_key_state(SCANCODE_CHEAT_1);
        if (key_state != '\0') {
            key_state = get_key_state(SCANCODE_CHEAT_2);
            if (key_state != '\0') {
                /* Cheat: fill move_descriptor_table entries (far ptr from level.c).
                 * move_descriptor_table not directly accessible here; deferred. */
            }
        }
    }

    /* Wait for all keys + action to be released. */
    do {
        do {
            key_state = get_key_state(SCANCODE_PAUSE);
        } while (key_state != '\0');
        quit_pressed = read_input_action_byte(0);
    } while (quit_pressed != '\0');

    /* Build the restore-view descriptor (1:1 from decomp): source=0x9694:0x203b,
     * word0e=0 (restore to page a200), 0x14 × 1 rect. */
    d->image_off = PAUSE_STRIP_DGROUP_OFF;
    d->image_seg = ENGINE_STATIC_DGROUP_SEG;
    d->src_x = 0;
    d->src_y = 0;
    d->width = SCREEN_W_TILES;
    d->height = 1;
    d->flag = 0;           /* word0e = 0 → restore to a200 page */
    d->dest_x = 0;
    d->dest_y = 0;

    /* RESTORE (engine: restore_bg_view with the descriptor above, word0e=0 →
     * dest page[table[0]], source the 0x9694 strip buffer): write hv_pause_strip's
     * 4 planes back to the same VGA rect, then end the plane-store sequence. */
    pg = host_page_off_of(0);
    for (r = 0; r < 8u; r++) {
        for (c = 0; c < VGA_ROW_BYTES; c++) {
            u16 i = (u16)((u16)r * VGA_ROW_BYTES + c);
            host_vga_put4((u16)(pg + i),
                          hv_pause_strip[i],
                          hv_pause_strip[PAUSE_STRIP_PLANE_SIZE + i],
                          hv_pause_strip[2u * PAUSE_STRIP_PLANE_SIZE + i],
                          hv_pause_strip[3u * PAUSE_STRIP_PLANE_SIZE + i]);
        }
    }
    host_vga_blit_end();
}

#endif /* BUMPY_PLAYABLE */
