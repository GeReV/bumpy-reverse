#ifdef BUMPY_PLAYABLE
#include <string.h>
#include <malloc.h>            /* halloc */
#include "host.h"
#include "../sprite_chain.h"   /* sprite_view */
#include "../entity.h"         /* entity_draw_p1 / entity_draw_p2 */
#include "../bgi_overlay.h"    /* restore_bg_view / render_player_view + bgi_view_desc */

/* ============================================================================
 * host_render.c — host framebuffer + render-leaf binding  (Plan A, Task 2)
 * ============================================================================
 *
 * This is the KEYSTONE of the playable host layer: it allocates the flat 4-plane
 * RAM framebuffer the validated blitters compose into, registers the engine's
 * VGA page table (sprite_table_base / cur_sprite_data) into that buffer, and makes
 * the per-module render-leaf wrappers REAL — routing the gameplay draw calls
 * (draw_p1_sprite / draw_p2_sprite / the channel-A/B blits) through the already-
 * reconstructed bgi_overlay.c leaves + the validated sprite blitter, exactly as
 * tools/composite_ctest.c wires them.
 *
 * ┌──────────────────────────────────────────────────────────────────────────┐
 * │ RECONSTRUCTION FIDELITY — THE DOCUMENTED CORE DIVERGENCE                    │
 * ├──────────────────────────────────────────────────────────────────────────┤
 * │ The original engine blits STRAIGHT TO VGA hardware: the planar blitter      │
 * │ programs the VGA Sequencer / Graphics-Controller registers (out 0x3c4 /     │
 * │ 0x3ce map-mask + bit-mask) and writes the prepared frame into the A000/A200 │
 * │ MMIO window, double-buffering across two VGA pages (page0=a000:0000,         │
 * │ page1=a200:0000) and presenting via a CRTC start-address page flip.         │
 * │                                                                            │
 * │ This host layer instead composes into a FLAT 4-PLANE RAM IMAGE              │
 * │ (host_framebuffer, 4 × 0x10000 B, plane p at p*0x10000) — the same memory   │
 * │ image the validated blitters (sprite_blit_planar_vga / bg_render_grid) and  │
 * │ the composite gate (tools/composite_ctest.c) already produce byte-exact.    │
 * │ host_video.c's present path then packs page-0 and copies that RAM image to  │
 * │ real VGA. The two VGA pages are modelled as byte offsets 0x0000 / 0x2000    │
 * │ WITHIN each plane (mirroring the engine's a000/a200 layout), selected via   │
 * │ the page table below. This is a behavior-faithful MEMORY model of the       │
 * │ engine's VGA-hardware port writes + A000/A200 double-buffer, NOT a 1:1      │
 * │ transcription of the hardware register sequence. Recorded in                │
 * │ docs/reconstruction-fidelity.md (“playable host” section).                  │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * The faithful default build (BUMPY.EXE) links game_stubs.c instead and never
 * sees this file; the gameplay-module leaf stubs are #ifndef BUMPY_PLAYABLE so
 * their real bodies come from here only under -dBUMPY_PLAYABLE.
 * ============================================================================ */

/* ── The flat 4-plane RAM framebuffer ──────────────────────────────────────── */
u8 __huge *host_framebuffer = (u8 __huge *)0;   /* 4 * HOST_PLANE_SIZE */

/* ── VGA page table — real C globals mirroring the engine's DGROUP state ──────
 *   sprite_table_base (DGROUP 0x5415): two far ptrs into the VGA plane window:
 *     [0] = a200:0000 (page1, plane byte offset 0x2000)
 *     [1] = a000:0000 (page0, plane byte offset 0x0000)
 *   cur_sprite_data (DGROUP 0x56e2/0x56e4): the CURRENT draw page, split into
 *     (off, seg). dispatch_palette_mode_with_src_ptr is the only engine writer.
 *
 *   In the host memory model the "segment" is a synthetic VGA tag (0xa000 / 0xa200)
 *   and the plane base is always host_framebuffer; the page is expressed purely
 *   through the (off, seg) pair the blitter folds into the dest offset
 *   (sprite_chain.c: dst_off += view->data_off; desc[0x0a] = view->data_seg;
 *    entity.c: voff = data_seg*16 + dst_off - 0xA0000).  See entity.h:147-167. */
u16 host_sprite_table_off[2] = { (u16)BGI_PAGE_A200_OFF, (u16)BGI_PAGE_A000_OFF };
u16 host_sprite_table_seg[2] = { VGA_SEG_PAGE1,          VGA_SEG_PAGE0 };
u16 host_cur_sprite_data_off = (u16)BGI_PAGE_A000_OFF;   /* default draw page = page0 */
u16 host_cur_sprite_data_seg = VGA_SEG_PAGE0;

/* ── Render context the blit leaves consume ───────────────────────────────────
 *   The engine's blit leaf (blit_sprite, 1000:942a) takes only the obj far ptr
 *   (off, seg) and reads everything else from engine globals (the sprite bank,
 *   the DGROUP entity shadow, and the active sprite_view).  The reconstructed
 *   blit pipeline (entity_draw_p1/p2 → entity_blit_object → sprite_blit_planar_vga)
 *   needs those same three inputs passed explicitly.  level.c (the per-load owner
 *   of the bank / dg shadow / planes) registers them here via host_render_bind so
 *   the leaf can resolve them, mirroring the engine's "read from globals" leaf. */
static u8 __huge *hr_bank          = (u8 __huge *)0;
static u32        hr_bank_base_lin  = 0UL;
static const u8 __far *hr_dg        = (const u8 __far *)0;

void host_render_bind(u8 __huge *bank, u32 bank_base_lin, const u8 __far *dg)
{
    hr_bank         = bank;
    hr_bank_base_lin = bank_base_lin;
    hr_dg           = dg;
}

/* Build the active full-screen sprite_view targeting the CURRENT draw page.
 * data_off/data_seg express the page selection (page0 → 0x0000/0xa000,
 * page1 → 0x2000/0xa200) exactly as composite_ctest's view does. */
static void hr_cur_view(sprite_view *view)
{
    view->left     = 0;
    view->right    = 40;
    view->top      = 0;
    view->bottom   = 199;
    view->height   = 199;
    view->data_off = host_cur_sprite_data_off;
    view->data_seg = host_cur_sprite_data_seg;
}

/* ── host_fb_init — allocate the framebuffer + register the page table ─────────
 *   Mirrors tools/composite_ctest.c's work_planes[4*PLANE_SZ] setup and the page-
 *   table description in entity.h:147-152.  Allocates a single flat 4*0x10000 B
 *   image; points sprite_table_base[0]/[1] + cur_sprite_data at page0/page1 within
 *   it.  Idempotent (allocates once). */
void host_fb_init(void)
{
    if (host_framebuffer == (u8 __huge *)0) {
        host_framebuffer = (u8 __huge *)halloc(4UL * HOST_PLANE_SIZE, 1);
    }
    /* sprite_table_base[0] = page1 (a200, off 0x2000); [1] = page0 (a000, off 0). */
    host_sprite_table_off[0] = (u16)BGI_PAGE_A200_OFF;
    host_sprite_table_seg[0] = VGA_SEG_PAGE1;
    host_sprite_table_off[1] = (u16)BGI_PAGE_A000_OFF;
    host_sprite_table_seg[1] = VGA_SEG_PAGE0;
    /* cur_sprite_data → page0 (a000:0000) — the page the static level compose and
       the composite gate render to (composite_ctest: view.data_seg=0xa000). */
    host_cur_sprite_data_off = (u16)BGI_PAGE_A000_OFF;
    host_cur_sprite_data_seg = VGA_SEG_PAGE0;
}

/* set_sprite_table_ptr (1cec:2dd2) + dispatch_palette_mode_with_src_ptr (1cec:2d6d):
 * select the draw page by index into sprite_table_base, splitting the chosen far
 * ptr into cur_sprite_data (off, seg).  index 0 → page1, index 1 → page0. */
void host_set_draw_page(u8 index)
{
    u8 i = (u8)(index & 1u);
    host_cur_sprite_data_off = host_sprite_table_off[i];
    host_cur_sprite_data_seg = host_sprite_table_seg[i];
}

/* ── Blit-sprite leaves — blit_sprite (1000:942a) ─────────────────────────────
 *   The engine's blit_sprite(obj_off, obj_seg=0x203b/DGROUP) blits the obj struct
 *   already populated (X/Y/frame) by the caller (draw_p1_sprite / draw_p2_sprite /
 *   the channel draw loops) at DGROUP:obj_off into the current VGA page.  Here we
 *   read those obj fields back from the registered dg shadow and re-run the
 *   validated entity_draw_{p1,p2} pipeline into host_framebuffer's current page —
 *   the exact call shape composite_ctest uses (planes=framebuffer base, view→page).
 *   obj_off 0x792e = p1_sprite (used by P1, layers A/B/C, screens);
 *   obj_off 0x795a = p2_sprite.  Any other off falls back to the p1 path. */
static void hr_blit_obj(u16 obj_off)
{
    sprite_view view;
    u16 x, y, frame;

    if (host_framebuffer == (u8 __huge *)0 || hr_dg == (const u8 __far *)0) {
        return;   /* not bound yet (no level loaded) → faithful NOP */
    }
    /* Read X/Y/frame from the populated obj in the dg shadow (LE u16). */
    x     = (u16)((u16)hr_dg[obj_off + 0u] | ((u16)hr_dg[obj_off + 1u] << 8));
    y     = (u16)((u16)hr_dg[obj_off + 2u] | ((u16)hr_dg[obj_off + 3u] << 8));
    frame = (u16)((u16)hr_dg[obj_off + 4u] | ((u16)hr_dg[obj_off + 5u] << 8));

    hr_cur_view(&view);

    if (obj_off == 0x795au) {
        /* p2_sprite obj path. p2_cell == -1 guard is the caller's; here the obj is
           already valid, so draw unconditionally via the p2 pipeline (frame_base=0,
           cell=0 → entity_draw_p2 blits the obj as-built). */
        entity_draw_p2(host_framebuffer, hr_dg, x, y, frame, 0u, (s8)0,
                       hr_bank, hr_bank_base_lin, &view);
    } else {
        entity_draw_p1(host_framebuffer, hr_dg, x, y, frame,
                       hr_bank, hr_bank_base_lin, &view);
    }
}

void anim_blit_sprite_leaf(u16 obj_off, u16 obj_seg)
{
    (void)obj_seg;          /* DGROUP runtime seg — irrelevant in the host model */
    hr_blit_obj(obj_off);
}

void p1_blit_sprite_leaf(u16 obj_off, u16 obj_seg)
{
    (void)obj_seg;
    hr_blit_obj(obj_off);
}

void p2_blit_sprite_leaf(u16 obj_off, u16 obj_seg)
{
    (void)obj_seg;
    hr_blit_obj(obj_off);
}

/* ── View leaves — render_player_view (1000:93b8) / restore_bg_view (1000:80bc) ──
 *   The engine's per-tick render/erase view leaves take the descriptor far ptr.
 *   The reconstructed bgi_overlay.c functions take (planes, vga_src, view).  In the
 *   gameplay (P1/P2 + channel-A/B) context these are STRUCTURAL NOPs: the engine's
 *   view descriptors are code-embedded (word00=0xc3fb / word0e=0x85b3 > 1) so the
 *   bgi_set_mode_01/10 guard trips and nothing is copied (present_model.md §5;
 *   bgi_overlay.c).  We faithfully drive the reconstructed leaf with that NOP view,
 *   so the host behaviour matches the engine: no-op.  The visible per-tick pixels
 *   come solely from the blit leaves above. */
static const bgi_view_desc hr_nop_view = {
    0xc3fbu, { 0u,0u,0u,0u,0u,0u }, 0x85b3u, 0u, 0u, { 0u,0u,0u,0u }, 0u
};

void anim_render_view_leaf(u8 __far *view)
{
    (void)view;
    if (host_framebuffer != (u8 __huge *)0) {
        render_player_view(host_framebuffer, host_framebuffer, &hr_nop_view);
    }
}

void anim_restore_bg_view_leaf(u8 __far *view)
{
    (void)view;
    if (host_framebuffer != (u8 __huge *)0) {
        restore_bg_view(host_framebuffer, host_framebuffer, &hr_nop_view);
    }
}

void p1_render_view_leaf(u8 __far *view)
{
    (void)view;
    if (host_framebuffer != (u8 __huge *)0) {
        render_player_view(host_framebuffer, host_framebuffer, &hr_nop_view);
    }
}

void p1_restore_view_leaf(u8 __far *view)
{
    (void)view;
    if (host_framebuffer != (u8 __huge *)0) {
        restore_bg_view(host_framebuffer, host_framebuffer, &hr_nop_view);
    }
}

void p2_render_view_leaf(u8 __far *view)
{
    (void)view;
    if (host_framebuffer != (u8 __huge *)0) {
        render_player_view(host_framebuffer, host_framebuffer, &hr_nop_view);
    }
}

void p2_restore_view_leaf(u8 __far *view)
{
    (void)view;
    if (host_framebuffer != (u8 __huge *)0) {
        restore_bg_view(host_framebuffer, host_framebuffer, &hr_nop_view);
    }
}

/* ── Out-of-scope HUD / text render leaves (faithful-signature stubs) ───────────
 *   FUN_1000_80ac (B-side render leaf, no clean decomp) has no reconstructed
 *   work-buffer body; the engine's HUD/text path is out of scope for the keystone
 *   gameplay compose.  Kept a faithful NOP so the screens.c / anim.c call sites
 *   stay byte-faithful (same convention as the default build's game_stubs leaves).
 *   The BGI text leaves (text_clip_leaf_9837 / draw_string_glyphs_9804) keep their
 *   own NOP bodies in screens.c — unconditional in both builds, not duplicated here. */
void anim_render_leaf_80ac(u8 __far *view)        { (void)view; }

#endif /* BUMPY_PLAYABLE */
