#ifdef BUMPY_PLAYABLE
#ifndef BUMPY_H                /* gcc host harnesses define BUMPY_H and shim these */
#include <i86.h>               /* MK_FP */
#include <conio.h>             /* inp, outp */
#include <malloc.h>            /* halloc */
#endif
#include <string.h>
#include "host.h"
#include "../sprite_chain.h"   /* sprite_view */
#include "../entity.h"         /* entity_draw_p1 / entity_draw_p2 */
#include "../gfx_overlay.h"    /* restore_bg_view / render_player_view + gfx_view_desc */

/* ============================================================================
 * host_render.c — host framebuffer + render-leaf binding  (Plan A, Task 2)
 * ============================================================================
 *
 * This is the KEYSTONE of the playable host layer: it allocates the flat 4-plane
 * RAM framebuffer the validated blitters compose into, registers the engine's
 * VGA page table (sprite_table_base / cur_sprite_data) into that buffer, and makes
 * the per-module render-leaf wrappers REAL — routing the gameplay draw calls
 * (draw_p1_sprite / draw_p2_sprite / the channel-A/B blits) through the already-
 * reconstructed gfx_overlay.c leaves + the validated sprite blitter, exactly as
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
/* Engine sprite_table_base (DGROUP 0x5415): [0] = a200:0000, [1] = a000:0000.
 * (Fixed 2026-07-02: the old init paired seg A200 WITH off 0x2000 — a double count
 * to linear 0xA4000; entries must be seg:0000 exactly like the engine's.)
 * present_frame swaps the two entries (engine 1ab9:06c1 tail), so descriptor page
 * INDICES (word00/word0e) resolve to alternating physical pages — consumers must
 * read the table LIVE, never cache a page. */
u16 host_sprite_table_off[2] = { 0u,            0u            };
u16 host_sprite_table_seg[2] = { VGA_SEG_PAGE1, VGA_SEG_PAGE0 };
u16 host_cur_sprite_data_off = 0u;                        /* deref cache of table[idx] */
u16 host_cur_sprite_data_seg = VGA_SEG_PAGE0;
static u8 hr_cur_page_idx = 1u;   /* set_sprite_table_ptr index (engine boots on 1) */

/* Current draw page as an a000-window byte offset (0x0000 or 0x2000), resolved
 * from the LIVE table entry the cur-sprite pointer selects. */
u16 host_draw_page_off(void)
{
    return (u16)((((u32)host_sprite_table_seg[hr_cur_page_idx] << 4)
                  + host_sprite_table_off[hr_cur_page_idx]) - 0xA0000UL);
}

/* Page offset of an arbitrary table index (for descriptor word00/word0e). */
u16 host_page_off_of(u8 index)
{
    u8 i = (u8)(index & 1u);
    return (u16)((((u32)host_sprite_table_seg[i] << 4)
                  + host_sprite_table_off[i]) - 0xA0000UL);
}

/* present_frame's table swap (engine 1ab9:06c1: xchg table[0] <-> table[1] after
 * the CRTC start flip).  Refreshes the deref cache. */
void host_page_table_swap(void)
{
    u16 t;
    t = host_sprite_table_off[0]; host_sprite_table_off[0] = host_sprite_table_off[1];
    host_sprite_table_off[1] = t;
    t = host_sprite_table_seg[0]; host_sprite_table_seg[0] = host_sprite_table_seg[1];
    host_sprite_table_seg[1] = t;
    host_cur_sprite_data_off = host_sprite_table_off[hr_cur_page_idx];
    host_cur_sprite_data_seg = host_sprite_table_seg[hr_cur_page_idx];
}

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

/* ── Spawn-scan gate for the anim clean-bg repaint (host deviation, see
 *    anim_restore_bg_view_leaf) ────────────────────────────────────────────────
 *   Set for the duration of spawn_and_draw_level_entities so the anim erase leaves
 *   do NOT repaint the flat clean-bg over freshly-blitted layer-A structures during
 *   the one-shot spawn grid scan (the engine's real erase blits a shadow that
 *   preserves the overlapping layer; the host substitutes a destructive dither
 *   repaint — harmless per-tick, but clobbering during spawn).  Cleared afterwards
 *   so per-tick gameplay erases (the layer-B trail-fix) behave exactly as before. */
static u8 hr_in_spawn = 0u;

void host_render_set_spawn(u8 active)
{
    hr_in_spawn = active;
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
    /* Resolve the draw page from the LIVE table entry (the engine's cur_sprite_data
       is a pointer INTO sprite_table_base, so a present's table swap retargets every
       subsequent blit — mirrored by reading through the index each call). */
    view->data_off = host_sprite_table_off[hr_cur_page_idx];
    view->data_seg = host_sprite_table_seg[hr_cur_page_idx];
}

/* ── host_fb_init — allocate the framebuffer + register the page table ─────────
 *   Mirrors tools/composite_ctest.c's work_planes[4*PLANE_SZ] setup and the page-
 *   table description in entity.h:147-152.  Allocates a single flat 4*0x10000 B
 *   image; points sprite_table_base[0]/[1] + cur_sprite_data at page0/page1 within
 *   it.  Idempotent (allocates once). */
void host_fb_init(void)
{
    static u8 hr_fb_reanchor_arm = 0u;   /* 0 on the boot call, 1 on every level-load re-entry */
#ifndef HOST_FB_16K
    if (host_framebuffer == (u8 __huge *)0) {
        host_framebuffer = (u8 __huge *)halloc(4UL * HOST_PLANE_SIZE, 1);
    }
#endif
    /* HOST_FB_16K (the shipping playable build): NO flat framebuffer is allocated.
     * After the real-VGA migration every draw/compose/save-under path writes a000
     * directly (host_vga_*), present_frame is a faithful NOP, and the last flat-RAM
     * consumers (anim erase, clean-bg capture) were rewired to VGA (2026-07-02) —
     * the 64 KB image was dead weight sitting on the conventional-memory cliff.
     * host_framebuffer stays NULL; remaining `!= 0` guards degrade to no-ops and
     * the blit paths ignore their inert `planes` argument under BUMPY_PLAYABLE. */
    /* sprite_table_base (engine DGROUP 0x5415): [0] = a200:0000, [1] = a000:0000. */
    host_sprite_table_off[0] = 0u;
    host_sprite_table_seg[0] = VGA_SEG_PAGE1;
    host_sprite_table_off[1] = 0u;
    host_sprite_table_seg[1] = VGA_SEG_PAGE0;
    /* cur_sprite_data → table[1] (a000:0000), the engine's boot selection. */
    hr_cur_page_idx = 1u;
    host_cur_sprite_data_off = host_sprite_table_off[1];
    host_cur_sprite_data_seg = host_sprite_table_seg[1];

    /* Re-anchor the CRTC DISPLAY to slot 0 (page1 = A200, CRTC start 0x2000) to match the
     * page TABLE reset just performed, so §8.1's `displayed == page[table[0]]` holds
     * DETERMINISTICALLY after a level load.  The table reset above returns the pages to boot
     * parity (S=0), but the CRTC start is NOT otherwise re-programmed per level
     * (init_crtc_window is a no-CRTC clip-window store) — it floats on the menu's accumulated
     * present-flip parity.  Left unanchored, that parity determines whether the overworld-entry
     * iris (level_intro_screen, bracketed to slot 0 below) draws to the displayed or the hidden
     * page — visible after some menu paths, hidden after others (e.g. after entering a password,
     * whose extra run_main_menu present loop flips the parity).  Re-anchoring here restores the
     * draw/display lockstep the whole UI page convention assumes.  SKIP the first (boot) call:
     * main.c calls host_fb_init BEFORE the platform sets video mode 0x0D, and the boot CRTC
     * parity is programmed separately (host_crtc_set_start); only the per-LEVEL re-entries need
     * it.  RECONSTRUCTION FIDELITY: docs/reconstruction-fidelity.md. */
    if (hr_fb_reanchor_arm) {
        outp(CRTC_INDEX, CRTC_START_HI);
        outp(CRTC_DATA,  0x20u);   /* start-high bit5 set → CRTC start 0x2000 = page1 = table[0] */
        outp(CRTC_INDEX, CRTC_START_LO);
        outp(CRTC_DATA,  0x00u);
    }
    hr_fb_reanchor_arm = 1u;
}

/* host_dgroup_seg — the loaded image's actual DGROUP segment.  Any C global lives in
 * DGROUP (large model near-data), so the segment half of a far pointer to one IS the
 * runtime DGROUP.  The engine leaves stamp this into descriptor seg fields where Ghidra
 * showed the static 0x203b; the recompiled image loads DGROUP elsewhere, so the playable
 * *_DGROUP_RUNTIME_SEG macros resolve here instead. */
u16 host_dgroup_seg(void)
{
    static u16 hr_dgroup_marker;   /* a DGROUP (near-data) global */
    return (u16)((u32)((void __far *)&hr_dgroup_marker) >> 16);
}

/* set_sprite_table_ptr (1cec:2dd2) + dispatch_palette_mode_with_src_ptr (1cec:2d6d):
 * select the draw page by index into sprite_table_base, splitting the chosen far
 * ptr into cur_sprite_data (off, seg).  index 0 → page1, index 1 → page0. */
void host_set_draw_page(u8 index)
{
    u8 i = (u8)(index & 1u);
    hr_cur_page_idx = i;
    host_cur_sprite_data_off = host_sprite_table_off[i];
    host_cur_sprite_data_seg = host_sprite_table_seg[i];
}

/* ── Real-VGA mode-0x0D plane-store primitives ─────────────────────────────────
 * See host.h for the contract.  These are the faithful blit target: the engine's
 * own blitters program these very registers (out 0x3c4/0x3ce) and write the a000
 * window directly.  The reconstructed blitters call these instead of storing into
 * the flat back-buffer, restoring the 1:1 VGA write path.  off < 0x4000 (display
 * extent of the page pair); MK_FP(0xa000, off) addresses the selected page. */
/* Sink for the latch-load read.  A bare `(void)(*(volatile __far*)vp)` is ELIDED by
 * Open Watcom's optimizer (verified via wdis: no read instruction emitted) — which
 * leaves the VGA latches stale, so transparent (bit-mask 0) pixels keep garbage instead
 * of the background → opaque sprites.  Storing the read into a file-scope VOLATILE makes
 * the read an observable side effect the optimizer must keep.
 *
 * NOTE: the per-plane STORE loop below does NOT need the same treatment — the `outp()`
 * (Map-Mask select) calls between the four `*vp = vN` writes are external calls the
 * optimizer must assume may read that memory, so it cannot dead-store-eliminate the
 * writes.  Verified via wdis on the Watcom playable build: all four plane stores are
 * emitted (`vp` non-volatile vs volatile produces byte-identical object code). */
static volatile u8 hr_vga_latch_sink;

void host_vga_rmw4(u16 off, u8 v0, u8 v1, u8 v2, u8 v3, u8 bm)
{
    u8 __far *vp = (u8 __far *)MK_FP(VGA_SEG_PAGE0, off);
    outp(GC_INDEX, GC_BIT_MASK);
    outp(GC_DATA,  bm);                       /* writable bits = coverage mask  */
    hr_vga_latch_sink = *(volatile u8 __far *)vp;   /* dummy read → load all 4 latches */
    outp(SEQ_INDEX, SEQ_MAP_MASK);
    outp(SEQ_DATA, 0x01u); *vp = v0;          /* plane 0: (v0&bm)|(latch0&~bm)   */
    outp(SEQ_DATA, 0x02u); *vp = v1;
    outp(SEQ_DATA, 0x04u); *vp = v2;
    outp(SEQ_DATA, 0x08u); *vp = v3;
}

void host_vga_put4(u16 off, u8 v0, u8 v1, u8 v2, u8 v3)
{
    u8 __far *vp = (u8 __far *)MK_FP(VGA_SEG_PAGE0, off);
    outp(GC_INDEX, GC_BIT_MASK);
    outp(GC_DATA,  0xFFu);                     /* all bits writable (opaque)      */
    outp(SEQ_INDEX, SEQ_MAP_MASK);
    outp(SEQ_DATA, 0x01u); *vp = v0;
    outp(SEQ_DATA, 0x02u); *vp = v1;
    outp(SEQ_DATA, 0x04u); *vp = v2;
    outp(SEQ_DATA, 0x08u); *vp = v3;
}

/* Read the 4 plane bytes at a000:off via GC Read-Map-Select (idx 4).  The per-sprite
 * save-under captures the clean background (before the sprite is drawn) so the next
 * frame's erase can restore it.  Mirrors the engine mode-10 save (a000 -> RAM buf). */
void host_vga_read4(u16 off, u8 *v0, u8 *v1, u8 *v2, u8 *v3)
{
    u8 __far *vp = (u8 __far *)MK_FP(VGA_SEG_PAGE0, off);
    outp(GC_INDEX, GC_READ_MAP);
    outp(GC_DATA, 0u); *v0 = *vp;
    outp(GC_DATA, 1u); *v1 = *vp;
    outp(GC_DATA, 2u); *v2 = *vp;
    outp(GC_DATA, 3u); *v3 = *vp;
}

void host_vga_clear4(u16 off)
{
    u8 __far *vp = (u8 __far *)MK_FP(VGA_SEG_PAGE0, off);
    outp(GC_INDEX, GC_BIT_MASK);
    outp(GC_DATA,  0xFFu);
    outp(SEQ_INDEX, SEQ_MAP_MASK);
    outp(SEQ_DATA, 0x0Fu);                     /* all planes enabled              */
    *vp = 0u;                                  /* one write zeroes all 4 planes   */
}

void host_vga_blit_end(void)
{
    outp(GC_INDEX, GC_BIT_MASK);
    outp(GC_DATA,  0xFFu);                     /* restore default: all bits       */
    outp(SEQ_INDEX, SEQ_MAP_MASK);
    outp(SEQ_DATA, 0x0Fu);                     /* restore default: all planes     */
}

/* Clear the displayed VGA page extent (a000 [0..0x4000): page0 + page1 regions) to
 * black.  The blitters now write a000 directly and only touch the grid cells they
 * draw, so the host level/screen compose must clear a000 FIRST — otherwise empty
 * cells (run_code==0, not drawn by bg_render_grid) read back the op12 decode's
 * leftover VGA "loading screen" garbage (the blue speckle).  Map-Mask=0x0F writes
 * the zero byte to all four planes at once; Bit-Mask=0xFF makes the whole byte
 * writable. */
/* Reset the VGA Graphics Controller to its default write state: no set/reset, no
 * enable-set/reset, no data-rotate/ALU, write-mode 0 (CPU data, not latch), all bits
 * writable.  The iris-wipe / level-transition effects leave the GC in a non-default
 * mode; render_level runs right after them, and write-mode != 0 makes every a000 write
 * (clear, _fmemcpy, and the planar blitters) copy the VGA latches instead of the CPU
 * byte — corrupting the whole level image.  Per-frame draws are unaffected (the GC is
 * back to default by then). */
void host_vga_reset_gc(void)
{
    outp(GC_INDEX, 0x00u); outp(GC_DATA, 0x00u);   /* idx0 set/reset        = 0       */
    outp(GC_INDEX, 0x01u); outp(GC_DATA, 0x00u);   /* idx1 enable set/reset = 0       */
    outp(GC_INDEX, 0x03u); outp(GC_DATA, 0x00u);   /* idx3 data-rotate/func = MOV     */
    outp(GC_INDEX, 0x05u); outp(GC_DATA, 0x00u);   /* idx5 mode = write-mode 0/read 0 */
    outp(GC_INDEX, GC_BIT_MASK); outp(GC_DATA, 0xFFu);
}

void host_vga_clear_display(void)
{
    u8 __far *vga = (u8 __far *)MK_FP(VGA_SEG_PAGE0, 0u);
    host_vga_reset_gc();                            /* ensure write-mode 0 (post-iris) */
    outp(SEQ_INDEX, SEQ_MAP_MASK); outp(SEQ_DATA, 0x0Fu);
    _fmemset(vga, 0, 0x4000u);
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
    u16 ftbl_off, ftbl_seg;
    u32 ftbl_lin;

    if (hr_dg == (const u8 __far *)0) {
        return;   /* not bound yet (no level loaded) → faithful NOP */
    }
    /* Read X/Y/frame from the populated obj in the dg shadow (LE u16). */
    x     = (u16)((u16)hr_dg[obj_off + 0u] | ((u16)hr_dg[obj_off + 1u] << 8));
    y     = (u16)((u16)hr_dg[obj_off + 2u] | ((u16)hr_dg[obj_off + 3u] << 8));
    frame = (u16)((u16)hr_dg[obj_off + 4u] | ((u16)hr_dg[obj_off + 5u] << 8));

    hr_cur_view(&view);

    /* Frame-table pointer (obj +6/+8) selects the bank.  In-level the DG shadow holds
       g_bank_buf — a real host bank ptr whose linear is >= hr_bank_base_lin.  The FRONT-END
       screens instead carry the engine's DGROUP placeholder far ptrs, whose linear is far
       BELOW the heap bank: the menu cursor's 0x6c2c (FLECHE — drawn separately by
       host_blit_cursor) and the main-sprite-bank source 0xa0c6 (DAT_a0c6, which in the
       engine names the loaded BUMSPJEU bank but in the recon is just a DGROUP literal, NOT a
       valid host bank ptr).  Route by that ptr so front-end glyph TEXT (Hall-of-Fame names +
       scores, the menu-select code screen, the level-intro title) resolves from the one host
       main bank (hr_bank) via the guard-free screen-sprite path; the cursor is skipped; and
       the validated in-level P1/P2 path is left byte-for-byte unchanged.
       ROOT CAUSE: without this, sprite_prepare_frame computes ent_off = ftbl_lin -
       bank_base_lin + frame*4 from the placeholder ptr → a wild offset → the sprite culls →
       every front-end glyph drew nothing (blank HOF / password screen). */
    ftbl_off = (u16)((u16)hr_dg[obj_off + 6u] | ((u16)hr_dg[obj_off + 7u] << 8));
    ftbl_seg = (u16)((u16)hr_dg[obj_off + 8u] | ((u16)hr_dg[obj_off + 9u] << 8));
    ftbl_lin = ((u32)ftbl_seg << 4) + (u32)ftbl_off;

    if (obj_off == 0x795au) {
        /* p2_sprite obj path. p2_cell == -1 guard is the caller's; here the obj is
           already valid, so draw unconditionally via the p2 pipeline (frame_base=0,
           cell=0 → entity_draw_p2 blits the obj as-built). */
        entity_draw_p2(host_framebuffer, hr_dg, x, y, frame, 0u, (s8)0,
                       hr_bank, hr_bank_base_lin, &view);
    } else if (ftbl_off == 0x6c2cu) {
        return;   /* menu cursor (FLECHE) — drawn from the cursor bank by host_blit_cursor */
    } else if (hr_bank != (u8 __huge *)0 && ftbl_lin < hr_bank_base_lin) {
        /* front-end screen glyph: force the host main bank + skip the P1 hidden/move-anim
           guards that blit_sprite (1000:942a) never had (they belong to draw_p1_sprite). */
        u16 bseg = (u16)(hr_bank_base_lin >> 4);
        u16 boff = (u16)(hr_bank_base_lin & 0x0fUL);
        entity_draw_screen_sprite(host_framebuffer, x, y, frame, boff, bseg,
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
 *   The reconstructed gfx_overlay.c functions take (planes, vga_src, view).  In the
 *   gameplay (P1/P2 + channel-A/B) context these are STRUCTURAL NOPs: the engine's
 *   view descriptors are code-embedded (word00=0xc3fb / word0e=0x85b3 > 1) so the
 *   gfx_set_mode_01/10 guard trips and nothing is copied (present_model.md §5;
 *   gfx_overlay.c).  We faithfully drive the reconstructed leaf with that NOP view,
 *   so the host behaviour matches the engine: no-op.  The visible per-tick pixels
 *   come solely from the blit leaves above. */
/* ── Per-sprite VGA save-under (the faithful delta double-buffer) ──────────────
 * The engine keeps moving sprites' backgrounds intact by SAVING the clean bg under
 * each sprite before drawing it (render_player_view, mode-10, a000 -> RAM buf) and
 * RESTORING it next frame before the sprite moves (restore_bg_view, mode-01, RAM buf
 * -> a000).  In the real-VGA host the blitters write a000 directly, so the save/erase
 * must operate on real VGA too — read-map-select to save, map-mask to restore — not
 * the old flat back-buffer (which is no longer the display).
 *
 * Geometry (grounded in init_view_anim_descriptors@1000:535e + gfx_overlay.c): a
 * descriptor's tile cell (cx,cy) maps to a000 byte offset cy*8*40 + cx*2 (tiles are
 * 16px wide = 2 bytes, 8px tall = 8 rows); the rect is extent_x tiles wide (×2 bytes)
 * by extent_y tiles tall (×8 rows).
 *
 * CELL — per-frame:  render_p1_view (mode-10 save) writes the SAVE cell at +0x06/+0x08
 * (grid_x/grid_y, current);  erase_p1_view (mode-01 restore) writes the RESTORE cell at
 * +0x14/+0x16 (grid_x_prev/grid_y_prev).  The grid history makes the erase cell at
 * frame N+1 equal the save cell at frame N, so the erase repaints exactly where the
 * sprite was drawn.
 *
 * EXTENT — FIXED, set once by init_view_anim_descriptors (NOT per-frame):
 *   erase view p1_erase_view (mode-01): width @ +0x0a, height @ +0x0c  (= 4, 4 tiles)
 *   save  view p1_view       (mode-10): width @ +0x18, height @ +0x1a  (= 4, 4 tiles)
 * → a fixed 4×4-tile (64×32 px) footprint, matching restore_bg_view's own +0x0a/+0x0c
 *   read in gfx_overlay.c.  The +0x1e/+0x20 fields are the per-frame SCROLL offsets
 *   (p1_scroll_x/y): 4 at rest but SHRINK below 4 near the screen edges (0x14-grid_x /
 *   0x19-grid_y).  Reading extent from those (the prior bug) shrank the erase rect near
 *   the edges so it stopped covering the sprite → trails.  We read the real fixed
 *   footprint field, passed per call site as ext_off. */
#define HR_SU_TOP_MARGIN 1u                       /* extra tiles above the cell (see hr_su_rect) */
#define HR_SU_MAX_W   8u                          /* 4 tiles × 2 bytes                */
#define HR_SU_MAX_H   40u                         /* (4 + HR_SU_TOP_MARGIN) tiles × 8 */
#define HR_SU_PLANE   (HR_SU_MAX_W * HR_SU_MAX_H) /* 320 bytes/plane                  */

static u8 s_p1_saveunder[4u * HR_SU_PLANE];
static u8 s_p2_saveunder[4u * HR_SU_PLANE];
static u8 s_p1_su_valid = 0u;   /* buffer holds a saved bg (skip the cold-start erase) */

extern const u8 __far *host_clean_bg(void);   /* view_setup.c — coherent level-bg snapshot */
static u8 s_p2_su_valid = 0u;

/* ── Layer-B anim masked save-under (faithful mode-00 reconstruction — task B) ────
 * The engine erases each moving channel-B sprite via a MASKED shadow composite
 * (draw_anim_channels_b: mode-00 blit_view_masked → shadows 0x9eba/0x9fba/0x8888,
 * then mode-01 restore_bg_view).  That composite is the self-modifying Loriciel
 * graphics overlay (NOT Borland BGI) that does not decompile.  The host previously
 * substituted an UNMASKED clean-bg repaint over the fixed 1×4-tile anim_b_view1 rect
 * (host adaptation 2026-07-05), which spilled into the neighbouring static layer-A
 * platform above the B cell (world-2 "missing rows"); the F1 hr_in_spawn gate only
 * suppressed it during the spawn scan.
 *
 * Behaviour-faithful reconstruction: capture the EXACT page rect the sprite's blit is
 * about to overwrite — its real footprint (voff + cols×rows, stride) from
 * sprite_blit_build_desc, inherently masked to the sprite — BEFORE the blit, and
 * restore it the next time this page is drawn to erase the sprite.  Preserves the
 * neighbouring platform (outside the sprite footprint) AND keeps a jumped platform's
 * disappear intact (that is draw_anim_channels_a's fullscreen_buf erase, unchanged).
 * Per-page (2) because the a000/a200 flip redraws each page every other tick.  Same
 * mechanism as the P1/P2 save-under above, sized to the sprite rather than a fixed rect.
 * DEVIATION: reproduces the net effect, not the exact masked-composite pixels (the
 * mode-00 overlay is undecompilable).  docs/reconstruction-fidelity.md. */
#define HR_ANIMB_CH    4u                       /* 4 channel-B slots               */
#define HR_ANIMB_MAXW  5u                       /* max footprint bytes/row (40 px) */
#define HR_ANIMB_MAXH  40u                      /* max footprint rows (5 tiles)    */
#define HR_ANIMB_PLANE (HR_ANIMB_MAXW * HR_ANIMB_MAXH)   /* 200 bytes/plane        */
/* SINGLE buffer per channel (matches the engine's one 0x7e3e shadow/ch): the bg-under
   is page-independent (both a000/a200 pages hold identical bg + spawn statics via the
   init_fullscreen_view_desc mode-11 copy), so the footprint origin is stored PAGE-
   RELATIVE (voff & 0x1fff) and re-based onto the current draw page at restore. */
static u8  s_animb_su[HR_ANIMB_CH][4u * HR_ANIMB_PLANE];
static u16 s_animb_toff[HR_ANIMB_CH];           /* footprint origin, page-relative */
static u8  s_animb_w[HR_ANIMB_CH];              /* footprint width  (bytes)        */
static u8  s_animb_h[HR_ANIMB_CH];              /* footprint height (rows)         */
static u8  s_animb_stride[HR_ANIMB_CH];         /* dst row stride (bytes)          */
static u8  s_animb_valid[HR_ANIMB_CH];
static s8  s_animb_cap_ch = -1;                 /* active capture channel, -1=off  */

/* Called by draw_anim_channels_b BEFORE anim_blit_sprite_leaf: arm the footprint
   capture for channel `ch`.  The blit path (entity_blit_object) then calls
   host_animb_capture with the real footprint, which stores it here. */
void host_animb_begin_capture(u8 ch)
{
    s_animb_cap_ch = (ch < HR_ANIMB_CH) ? (s8)ch : (s8)-1;
}

void host_animb_end_capture(void)
{
    s_animb_cap_ch = -1;
}

/* Called from entity_blit_object (BUMPY_PLAYABLE) with the sprite's exact page
   footprint, just before sprite_blit_planar_vga writes it.  No-op unless a channel-B
   capture is armed.  Saves the untouched bg-under (bg + any overlapping static) so the
   next restore erases the sprite masked to its own footprint. */
void host_animb_capture(u16 voff, u16 cols, u16 rows, u16 stride)
{
    u8  ch, r, c;
    u16 toff, w, h;
    if (s_animb_cap_ch < 0 || hr_in_spawn != 0u) {
        /* Spawn draws each layer-B static ONCE via reused slot 0 at a different cell
           each call — they must NOT enter the save-under (the next call's restore would
           erase the previous static).  The save-under is gameplay-only. */
        return;
    }
    ch   = (u8)s_animb_cap_ch;
    toff = (u16)(voff & 0x1fffu);               /* page-relative origin            */
    /* +1 byte covers the blit's sub-byte shift carry into the next column. */
    w = (u16)(cols + 1u);
    h = rows;
    if (w > HR_ANIMB_MAXW) { w = HR_ANIMB_MAXW; }
    if (h > HR_ANIMB_MAXH) { h = HR_ANIMB_MAXH; }
    for (r = 0u; r < (u8)h; r++) {
        for (c = 0u; c < (u8)w; c++) {
            u16 off = (u16)(voff + (u16)r * stride + c);
            u16 bi  = (u16)((u16)r * w + c);
            host_vga_read4(off, &s_animb_su[ch][bi],
                           &s_animb_su[ch][HR_ANIMB_PLANE + bi],
                           &s_animb_su[ch][2u*HR_ANIMB_PLANE + bi],
                           &s_animb_su[ch][3u*HR_ANIMB_PLANE + bi]);
        }
    }
    s_animb_toff[ch]   = toff;
    s_animb_w[ch]      = (u8)w;
    s_animb_h[ch]      = (u8)h;
    s_animb_stride[ch] = (u8)stride;
    s_animb_valid[ch]  = 1u;
}

/* Called by draw_anim_channels_b BEFORE the blit (and before begin_capture): restore
   channel `ch`'s saved footprint on the CURRENT draw page, erasing the sprite this
   page last drew while leaving the neighbouring platform untouched. */
void host_animb_restore(u8 ch)
{
    u8  r, c;
    u16 base, w, h, stride;
    if (ch >= HR_ANIMB_CH || s_animb_valid[ch] == 0u || hr_in_spawn != 0u) {
        return;   /* gameplay-only (see host_animb_capture) */
    }
    base   = (u16)(host_draw_page_off() + s_animb_toff[ch]);  /* re-base to draw page */
    w      = s_animb_w[ch];
    h      = s_animb_h[ch];
    stride = s_animb_stride[ch];
    for (r = 0u; r < (u8)h; r++) {
        for (c = 0u; c < (u8)w; c++) {
            u16 off = (u16)(base + (u16)r * stride + c);
            u16 bi  = (u16)((u16)r * w + c);
            host_vga_put4(off, s_animb_su[ch][bi],
                          s_animb_su[ch][HR_ANIMB_PLANE + bi],
                          s_animb_su[ch][2u*HR_ANIMB_PLANE + bi],
                          s_animb_su[ch][3u*HR_ANIMB_PLANE + bi]);
        }
    }
    host_vga_blit_end();
}

static u16 hr_vw(const u8 __far *v, u16 off)       /* LE u16 from far descriptor */
{
    return (u16)((u16)v[off] | ((u16)v[off + 1u] << 8));
}

/* Resolve the rect (origin a000 offset + byte width + row height) from a descriptor.
 * Returns 0 if the rect is empty (caller skips). */
static int hr_su_rect(const u8 __far *view, u16 cell_off, u16 ext_off,
                      u16 *origin, u16 *w, u16 *h)
{
    u16 cx = hr_vw(view, cell_off);
    u16 cy = hr_vw(view, (u16)(cell_off + 2u));
    u16 ex = hr_vw(view, ext_off);              /* real footprint field (see header) */
    u16 ey = hr_vw(view, (u16)(ext_off + 2u));
    if (ex > 4u) { ex = 4u; }                      /* buffer holds at most 4 tiles wide */
    if (ey > 4u) { ey = 4u; }
    if (ex == 0u || ey == 0u || cx >= 20u || cy >= 25u) {
        return 0;
    }
    /* RECONSTRUCTION FIDELITY (host save-under top margin): benign over-coverage kept
     * on top of the now-faithful fixed footprint (ex/ey from the real +0x0a/+0x0c /
     * +0x18/+0x1a fields above).  p1_update_grid_cell (1000:1473) computes grid_x with a
     * -1 tile bias (giving the rect a left margin) but grid_y with NONE, so the fixed
     * 4-tile-tall rect's top sits at the sprite's top; a 1-tile upward pad guarantees the
     * erase covers the previous sprite on UP moves (the original never trails).  Applied
     * to BOTH the save (0x06) and the paired erase (0x14) so their geometry still matches
     * (shared buffer indexing).  Superset of the engine rect → visually identical (it just
     * repaints one extra clean-bg tile row that was also saved). */
    {
        u16 top = (cy >= HR_SU_TOP_MARGIN) ? HR_SU_TOP_MARGIN : cy;
        cy = (u16)(cy - top);
        ey = (u16)(ey + top);
    }
    *w = (u16)(ex * 2u);
    *h = (u16)(ey * 8u);
    /* Origin on the CURRENT draw page (live table) — under the real page-flip
       present, gameplay alternates pages every tick and the 2-tick grid history
       pairs each save with the erase on the SAME physical page. */
    *origin = (u16)(host_draw_page_off() + cy * 320u + cx * 2u);
    return 1;
}

/* SAVE: a000[rect @ cell_off] -> buf (clean bg, before the sprite is drawn). */
static void hr_save_under(const u8 __far *view, u8 *buf, u16 cell_off, u16 ext_off)
{
    u16 origin, w, h, row, col;
    if (!hr_su_rect(view, cell_off, ext_off, &origin, &w, &h)) {
        return;
    }
    for (row = 0u; row < h; row++) {
        for (col = 0u; col < w; col++) {
            u16 off = (u16)(origin + row * 40u + col);
            u16 bi  = (u16)(row * w + col);
            host_vga_read4(off, &buf[bi], &buf[HR_SU_PLANE + bi],
                           &buf[2u*HR_SU_PLANE + bi], &buf[3u*HR_SU_PLANE + bi]);
        }
    }
}

/* RESTORE: buf -> a000[rect @ cell_off] (erase the sprite by repainting the bg). */
static void hr_restore_under(const u8 __far *view, const u8 *buf, u16 cell_off, u16 ext_off)
{
    u16 origin, w, h, row, col;
    if (!hr_su_rect(view, cell_off, ext_off, &origin, &w, &h)) {
        return;
    }
    {
        const u8 __far *clean = host_clean_bg();
        for (row = 0u; row < h; row++) {
            for (col = 0u; col < w; col++) {
                u16 off = (u16)(origin + row * 40u + col);
                u16 bi  = (u16)(row * w + col);
                u8  q0 = buf[bi];
                u8  q1 = buf[HR_SU_PLANE + bi];
                u8  q2 = buf[2u*HR_SU_PLANE + bi];
                u8  q3 = buf[3u*HR_SU_PLANE + bi];
                /* PAGE-COHERENCE GUARD (2026-07-12) — top-center flag flicker fix.
                   The static top-strip bg (tile 167 = index-1 BLUE) can be page-incoherent:
                   blue on one VGA page, black on the other.  When Bumpy bounces to the top his
                   save-under's top-margin (HR_SU_TOP_MARGIN) footprint reaches that cell and can
                   CAPTURE the black page, then this restore stamps black over the blue -> the
                   top-center flicker (P1 save-under, not host_animb — grounded 2026-07-12).  The
                   captured value is only wrong when it's fully BLACK where the coherent clean bg
                   (hv_saveunder_buf, the level tile bg captured before sprites) is NON-black; in
                   that case restore the CLEAN bg so both pages converge on the true tile colour.
                   Non-black captures (real dynamic content) are untouched -> no regression to the
                   validated P1/P2 save-under.  docs/reconstruction-fidelity.md. */
                if (clean != (const u8 __far *)0 &&
                    q0 == 0u && q1 == 0u && q2 == 0u && q3 == 0u) {
                    u16 pr = (u16)(off & 0x1fffu);
                    if (pr < (u16)GFX_PAGE_SIZE) {
                        u8 c0 = clean[pr];
                        u8 c1 = clean[(u16)GFX_PAGE_SIZE + pr];
                        u8 c2 = clean[2u*(u16)GFX_PAGE_SIZE + pr];
                        u8 c3 = clean[3u*(u16)GFX_PAGE_SIZE + pr];
                        if ((c0 | c1 | c2 | c3) != 0u) {
                            q0 = c0; q1 = c1; q2 = c2; q3 = c3;
                        }
                    }
                }
                host_vga_put4(off, q0, q1, q2, q3);
            }
        }
    }
    host_vga_blit_end();
}

/* ── anim-channel background erase (layer-A active platforms) ───────────────────
 * RECONSTRUCTION FIDELITY: gameplay is SINGLE-PAGE (a000; the runtime oracle shows no
 * CRTC flip during play — host_video.c present_frame note).  The engine erases an active-
 * platform sprite by repainting the clean background from fullscreen_buf over the sprite's
 * grid cell BEFORE the sprite is re-blitted (draw_anim_channels_a: the erase-view 0x8d4 at
 * anim.c:422, before the blit_sprite at :430).  The host holds that clean background in the
 * flat-RAM save-under hv_saveunder_buf, captured once per load by setup_fullscreen_view;
 * host_clean_bg() (view_setup.c) exposes it.  Here we repaint the cell's tile rect from it
 * into the displayed page-0, plane-by-plane.
 *
 * DISPATCH: only the layer-A erase-view (anim_a_erase_view, DGROUP 0x8d4 — the one
 * draw_anim_channels_a drives from fullscreen_buf BEFORE the blit) erases; matched by
 * pointer identity.  The per-channel composited save/restore views (draw-view 0x8e0 /
 * clear-view 0x8c0/0x8bc) are the single-page-vestigial double-buffer paths — NOP here
 * (a per-page save/restore is a no-op on one displayed page).  The engine's save-under
 * buffers at DGROUP 0x79be/0x7e3e are therefore not needed by the host's single-page
 * erase.  LAYER-B (2026-07-05, was a known gap): draw_anim_channels_b calls this leaf
 * twice per active B channel with anim_b_view1 (0x8cc) — in the engine that view's
 * word0e==1 dispatches an a000 blit from the 0x9eba/0x9fba shadow + 0x8888 sources.  The
 * host never populates those shadows (the render/mask pass gfx_set_mode_00 is a NOP), so
 * it now repaints the CLEAN TILE BACKGROUND over the B cell instead (extent view+0x0a/0x0c),
 * the same mechanism as the layer-A erase — restoring the bg under the moving B sprite and
 * fixing the layer-B trails/flicker + world-2 speckles.  See the per-view branch below +
 * docs/reconstruction-fidelity.md ("playable host: anim-channel under-erase").
 *
 * 2026-07-02 FIX: this leaf previously repainted into the flat host_framebuffer and
 * host_clean_bg() was captured FROM host_framebuffer — but after the real-VGA migration
 * nothing displays that buffer and nothing paints it, so the erase was dead end-to-end
 * (zeros copied into invisible RAM; on-screen platform trails).  It now repaints the
 * displayed a000 page-0 via host_vga_put4, and the capture side (view_setup.c) reads
 * the freshly-painted VGA page via host_vga_read4. */
extern u8 __far *anim_a_erase_view;            /* anim.c 0x8d4 (layer-A erase descriptor) */
extern const u8 __far *host_clean_bg(void);    /* view_setup.c — hv_saveunder_buf */

void anim_render_view_leaf(u8 __far *view)     { (void)view; }  /* save: single-page no-op */

void anim_restore_bg_view_leaf(u8 __far *view)
{
    const u8 __far *clean;
    u16 cx, cy, ex, ey, wbytes, hrows, sox, dox, oy, row, col;
    u16 flags;

    /* The layer-A erase-view AND the deferred item-erase view (pending_erase_view,
       0x8e4 — staged by p1_collect_item_score, consumed by restore_bg_pending; its
       engine descriptor sources fullscreen_buf with word0e==1) repaint the clean
       background; the DGROUP-sourced save/shadow views are single-page no-ops
       (layer-B gap: see header). */
    {
        extern u8 __far *pending_erase_view;   /* player.c 0x8e4 */
        /* anim_b_view1 (layer-B) is NO LONGER erased here: the faithful masked
           save-under (host_animb_* — see this file's header + draw_anim_channels_b)
           now erases channel-B sprites to their own footprint, so the old unmasked
           clean-bg repaint (which spilled into the neighbouring layer-A platform) is
           retired.  Only the layer-A (anim_a_erase_view, the platform-DISAPPEAR erase)
           and the deferred item erase (pending_erase_view) remain clean-bg. */
        if (view != anim_a_erase_view && view != pending_erase_view) {
            return;
        }
    }

    /* RECONSTRUCTION FIDELITY: the F1 spawn-suppression of this clean-bg erase (added
       2026-07-11 as a band-aid for the layer-B over-paint) is RETIRED.  The destructive
       over-painter — the layer-B anim_b_view1 clean-bg repaint reaching into the
       neighbouring platform — is gone (reconstructed as the masked save-under host_animb_*),
       so only the layer-A erase (anim_a_erase_view, 2×2 at the cell) and the deferred item
       erase (pending_erase_view) remain here.  The engine's spawn grid scan itself runs
       draw_anim_channels_a as ERASE(fullscreen_buf)-before-BLIT per cell; the 2×2 erase
       reaches only the current cell + its right/down neighbours (drawn LATER in the
       row-major scan, then re-blitted), so it is non-destructive — letting it run matches
       the engine.  hr_in_spawn is retained solely as the layer-B save-under's gameplay gate
       (host_animb_capture/restore).  docs/reconstruction-fidelity.md. */
    clean = host_clean_bg();
    if (clean == (const u8 __far *)0) {
        return;
    }

    cx = *(u16 __far *)(view + 0x14u);         /* grid cell X (tiles) */
    cy = *(u16 __far *)(view + 0x16u);         /* grid cell Y (tiles) */
    {
        extern u8 __far *anim_b_view1;         /* anim.c 0x8cc */
        if (view == anim_b_view1) {
            /* RECONSTRUCTION FIDELITY (layer-B under-erase — host-adaptation, 2026-07-05).
               draw_anim_channels_b (anim.c:516/522) restores the layer-B background under each
               active B channel via mode-01 (restore_bg_view/gfx_set_mode_01) with anim_b_view1,
               whose EXTENT is view+0x0a (width) × view+0x0c (height) tiles and whose SOURCE is a
               pre-composited layer-B shadow (DGROUP 0x9eba/0x9fba for the sprite pass, 0x8888 for
               the bg pass — see gfx_overlay.c restore_bg_view).  The host never populates those
               shadows: the render/mask pass anim_render_leaf_80ac (blit_view_masked/gfx_set_mode_00)
               is a NOP because the engine's composited double-buffer is single-page-vestigial here.
               With no shadow to blit, the host instead REPAINTS THE CLEAN TILE BACKGROUND over the
               B cell — the SAME mechanism the layer-A erase (anim_a_erase_view) uses — restoring the
               bg under the moving layer-B sprite before draw_anim_channels_b re-blits it.  Fixes the
               layer-B trails/flicker + world-2 background speckles (the KNOWN GAP noted in this
               file's dispatch header).  Extent from the faithful +0x0a/+0x0c mode-01 fields (NOT the
               +0x1e/+0x20 mode-10 footprint the layer-A path reads; for anim_b_view1 those are 1/4).
               DEVIATION vs the engine: the clean-bg repaint does not composite overlapping STATIC
               layer-B content the shadow blit would preserve.  docs/reconstruction-fidelity.md. */
            ex = *(u16 __far *)(view + 0x0au);     /* mode-01 width  (tiles), runtime 1 or 3 */
            ey = *(u16 __far *)(view + 0x0cu);     /* mode-01 height (tiles), = 4            */
        } else {
            ex = *(u16 __far *)(view + 0x1eu);     /* layer-A / pending footprint width  (tiles) */
            ey = *(u16 __far *)(view + 0x20u);     /* layer-A / pending footprint height (tiles) */
        }
    }
    wbytes = (u16)(ex * 2u);                   /* 2 bytes/tile (16 px) */
    hrows  = (u16)(ey * 8u);                   /* 8 rows/tile          */
    sox    = (u16)(cx * 2u);
    dox    = sox;
    oy     = (u16)(cy * 8u);

    /* Half-tile-offset flag word +0x1c (engine descriptor parser 1ab9:03c5/0400/044f):
       bit 0x200 → SOURCE X += 1 byte (8 px);  bit 0x400 → DEST X += 1 byte;
       bit 0x100 → SOURCE Y += view+0x26 rows.  draw_anim_channels_a sets 0x600 for
       ODD cells (whose posA draw X sits at grid_x*16+8) — dropping these bits left
       the sprite's rightmost 8-px column un-erased on every odd cell (the
       right-tilt-platform trails, 2026-07-02).  The engine self-clears each bit as
       it is consumed; mirrored here (the caller rewrites +0x1c per call anyway). */
    flags = *(u16 __far *)(view + 0x1cu);
    if ((flags & 0x200u) != 0u) {
        sox = (u16)(sox + 1u);
        flags = (u16)(flags & 0xfdffu);
    }
    if ((flags & 0x400u) != 0u) {
        dox = (u16)(dox + 1u);
        flags = (u16)(flags & 0xfbffu);
    }
    if ((flags & 0x100u) != 0u) {
        oy = (u16)(oy + *(u16 __far *)(view + 0x26u));
        flags = (u16)(flags & 0xfeffu);
    }
    *(u16 __far *)(view + 0x1cu) = flags;

    {
        u16 pg = host_draw_page_off();   /* repaint the CURRENT draw page */
        for (row = 0u; row < hrows; row++) {
            u16 sline, dline;
            if ((u16)(oy + row) >= 200u) {
                break;
            }
            sline = (u16)(((u16)(oy + row)) * 40u + sox);
            dline = (u16)(((u16)(oy + row)) * 40u + dox);
            for (col = 0u; col < wbytes; col++) {
                u16 soff = (u16)(sline + col);   /* clean-bg source (page-relative) */
                u16 doff = (u16)(dline + col);   /* page dest       (page-relative) */
                if (soff < (u16)GFX_PAGE_SIZE && doff < (u16)GFX_PAGE_SIZE) {
                    host_vga_put4((u16)(pg + doff),
                                  clean[soff],
                                  clean[(u16)GFX_PAGE_SIZE + soff],
                                  clean[2u * (u16)GFX_PAGE_SIZE + soff],
                                  clean[3u * (u16)GFX_PAGE_SIZE + soff]);
                }
            }
        }
    }
    host_vga_blit_end();
}

/* SAVE cell @ +0x06/+0x08, extent @ +0x18/+0x1a (mode-10 p1_view/p2_view footprint).
 * ERASE cell @ +0x14/+0x16, extent @ +0x0a/+0x0c (mode-01 p1_erase_view/p2_erase_view).
 * Both extents are the fixed 4×4-tile footprint, so save/erase share buffer geometry. */
void p1_render_view_leaf(u8 __far *view)        /* SAVE  (render_p1_view / mode-10) */
{
    hr_save_under(view, s_p1_saveunder, 0x06u, 0x18u);
    s_p1_su_valid = 1u;
}

void p1_restore_view_leaf(u8 __far *view)       /* ERASE (erase_p1_view / mode-01) */
{
    if (s_p1_su_valid != 0u) {
        hr_restore_under(view, s_p1_saveunder, 0x14u, 0x0au);
    }
}

void p2_render_view_leaf(u8 __far *view)
{
    hr_save_under(view, s_p2_saveunder, 0x06u, 0x18u);
    s_p2_su_valid = 1u;
}

void p2_restore_view_leaf(u8 __far *view)
{
    if (s_p2_su_valid != 0u) {
        hr_restore_under(view, s_p2_saveunder, 0x14u, 0x0au);
    }
}

/* ── host_compose_bg_view — title/menu/text-screen background compose (HOST) ────
 *   screens.c's per-screen builders (show_title_background, show_*_screen, the
 *   level-intro/highscore screens) vec_decode a fullscreen image into fullscreen_buf
 *   then call the engine's restore_bg_view(view, seg) to copy it into the displayed
 *   page.  Unlike the gameplay view leaves above (code-embedded word0e>1 NOP views),
 *   THESE descriptors are real: the source planar image is at view+0x02/+0x04 and
 *   view->word0e == 1 selects page A000 (offset 0).  Drive the reconstructed 3-arg
 *   restore_bg_view with planes = host_framebuffer and that source, so the background
 *   actually composes into the RAM image present_frame copies to VGA.  Without this
 *   the title/menu/text screens stay BLANK (host_framebuffer never gets the image —
 *   the original Task-9 shim NOP'd this, assuming present_frame alone would show it).
 *   RECONSTRUCTION FIDELITY: documented in docs/reconstruction-fidelity.md. */
/* hr_compose_screen_vga — real-VGA equivalent of restore_bg_view's clipped-rect blit
 * for the title/menu/text-screen background compose.  Copies the planar-sequential
 * source image (packed at its own width) into the a000 display page via the Sequencer
 * Map-Mask (opaque per-plane store), instead of the old flat host_framebuffer.  Same
 * descriptor geometry as restore_bg_view (mode-01): dest page = word0e (1→a000:0x0000,
 * 0→a200:0x2000), rect w/h tiles @ +0x0a/+0x0c, dest origin tiles @ +0x14/+0x16. */
static void hr_compose_screen_vga(const u8 __far *view, const u8 __huge *src)
{
    u16 word0e   = hr_vw(view, 0x0eu);
    u16 w_tiles  = hr_vw(view, 0x0au);
    u16 h_tiles  = hr_vw(view, 0x0cu);
    u16 dx_tiles = hr_vw(view, 0x14u);
    u16 dy_tiles = hr_vw(view, 0x16u);
    u16 row_bytes = (u16)(w_tiles * 2u);
    u16 rows      = (u16)(h_tiles * 8u);
    /* word0e is a sprite_table_base INDEX (engine: [0]=a200, [1]=a000, swapped by
       each present) — resolve through the LIVE table, not a fixed page. */
    u16 dest_page_off = host_page_off_of((u8)word0e);
    u32 src_plane_stride = (u32)row_bytes * (u32)rows;
    u16 dst_origin = (u16)(dest_page_off + (u16)(dy_tiles * 8u) * 40u + dx_tiles * 2u);
    u8  plane;
    u16 row, col;

    if ((s16)word0e >= 2) {                 /* mode-01 guard (signed, as the engine) */
        return;
    }
    if (row_bytes == 0u || rows == 0u) {
        return;
    }
    host_vga_reset_gc();                     /* write-mode 0 + bit-mask 0xFF (post-iris) */
    for (plane = 0u; plane < 4u; plane++) {
        const u8 __huge *sp = src + (u32)plane * src_plane_stride;
        outp(SEQ_INDEX, SEQ_MAP_MASK);
        outp(SEQ_DATA, (u8)(1u << plane));   /* enable just this plane (opaque store) */
        for (row = 0u; row < rows; row++) {
            u8 __far *dp = (u8 __far *)MK_FP(VGA_SEG_PAGE0,
                                             (u16)(dst_origin + row * 40u));
            const u8 __huge *srow = sp + (u32)row * row_bytes;
            for (col = 0u; col < row_bytes; col++) {
                dp[col] = srow[col];
            }
        }
    }
    host_vga_blit_end();
}

void host_compose_bg_view(u8 __far *view)
{
    u16 src_off;
    u16 src_seg;
    const u8 __huge *vga_src;

    if (view == (u8 __far *)0) {
        return;
    }
    src_off = *(u16 __far *)(view + 0x02);
    src_seg = *(u16 __far *)(view + 0x04);
    vga_src = (const u8 __huge *)MK_FP(src_seg, src_off);
    /* Real-VGA host: compose the screen background DIRECTLY into the a000 display
       page (the blitters no longer use the flat host_framebuffer, and present_frame
       is a faithful NOP — so the old restore_bg_view(host_framebuffer,...) compose
       landed in an invisible buffer, leaving the title/menu screens black). */
    hr_compose_screen_vga(view, vga_src);
}

/* ── Out-of-scope HUD / text render leaves (faithful-signature stubs) ───────────
 *   FUN_1000_80ac (B-side render leaf, no clean decomp) has no reconstructed
 *   work-buffer body; the engine's HUD/text path is out of scope for the keystone
 *   gameplay compose.  Kept a faithful NOP so the screens.c / anim.c call sites
 *   stay byte-faithful (same convention as the default build's game_stubs leaves).
 *   The graphics-overlay text leaves (gfx_set_text_pos_9837 / gfx_draw_string_9804) live in
 *   screens.c — NOPs in the default build, routed to host_text_* below when playable. */
void anim_render_leaf_80ac(u8 __far *view)        { (void)view; }

/* ── Menu / level-select cursor bank (FLECHE.BIN) ──────────────────────────────
 *   The menu cursor arrow is a SEPARATE sprite resource (engine resource 9 ->
 *   DAT_6c2c, the file FLECHE.BIN) from the gameplay bank (BUMSPJEU.BIN / g_bank_buf).
 *   run_main_menu draws frame 0 of this table.  Because the cursor is drawn BEFORE any
 *   level loads (hr_dg / hr_bank unbound → hr_blit_obj NOPs), it cannot route through
 *   the normal gameplay leaf; host_resource.c::host_load_cursor_bank loads+transforms
 *   FLECHE.BIN and registers it here, and run_main_menu calls host_blit_cursor.
 *   ftbl_off/seg = the cursor bank's own far ptr (table at bank offset 0, the
 *   sprite_bank_relocate_frames form), so sprite_prepare_frame resolves frame i at
 *   bank+table[i].  RECONSTRUCTION FIDELITY: host-platform screen-sprite path. */
static u8 __huge *hr_cursor_bank     = (u8 __huge *)0;
static u32        hr_cursor_base_lin = 0UL;
static u16        hr_cursor_ftbl_off = 0u;
static u16        hr_cursor_ftbl_seg = 0u;

void host_cursor_bind(u8 __huge *bank, u32 base_lin, u16 ftbl_off, u16 ftbl_seg)
{
    hr_cursor_bank     = bank;
    hr_cursor_base_lin = base_lin;
    hr_cursor_ftbl_off = ftbl_off;
    hr_cursor_ftbl_seg = ftbl_seg;
}

/* NO CURSOR SAVE-UNDER (2026-07-03).  The ENGINE has no cursor save-under at all:
 * run_main_menu brackets its draws on sprite-table slot 0 (set_sprite_table_ptr(0)
 * before the loop) and the per-iteration mode-11 full-page sync
 * (init_fullscreen_view_desc(0,1): copy page[table[0]] → page[table[1]]) IS the
 * erase — the freshly-flipped-in displayed page is synced over the previous
 * frame's page, wiping last frame's arrow wholesale.  An earlier host revision
 * kept a box save/restore here (a flat-framebuffer-era leftover); with the real
 * page-flip present + live page table it was redundant at best and fought the
 * sync when the boot page parity slipped.  Deleted — the blit below is all the
 * engine does. */
void host_blit_cursor(u16 x, u16 y)
{
    sprite_view view;

    if (hr_cursor_bank == (u8 __huge *)0) {
        return;   /* cursor bank not ready → NOP */
    }
    hr_cur_view(&view);
    entity_draw_screen_sprite(host_framebuffer, x, y, 0u,
                              hr_cursor_ftbl_off, hr_cursor_ftbl_seg,
                              hr_cursor_bank, hr_cursor_base_lin, &view);
}

/* ── gfx text rendering (the 1000:9837 / 1000:9804 leaves' host bodies) ────────────
 *   Engine model (graphics overlay, disassembled from the unpacked image — these routines
 *   are the self-modifying overlay segment 1ab9 and have no Ghidra decomp):
 *     1ab9:1441  SET TEXT POSITION — stores x/y into DGROUP 0x6942/0x6944.
 *     1ab9:13ec  DRAW STRING       — walks the NUL-terminated far string, per char
 *                far-calls 1ab9:13bc.
 *     1ab9:13bc  per-char range check: font far ptr at DGROUP 0x68a2 (byte[0]=first,
 *                byte[1]=last; draw only if first <= ch < last), then dispatches the
 *                palette_mode glyph handler via table [0x541d]*2+0x6952.
 *     1ab9:1607  the pm=2 glyph handler (full disasm 2026-07-03): glyph offset =
 *                font + BE16(font[6+(ch-first)*2]) (op 14d3 `xchg ah,al` — the
 *                big-endian Loriciel offset table); glyph entry = {byte w_px,
 *                byte h_rows, byte y_skip, rows...} (1 bpp, 1 byte/row, MSB =
 *                leftmost pixel).  The cell is drawn OPAQUELY: op 155f prefills
 *                the staging buffer (DGROUP 0x68b6, [row][2 cols][4 planes]) with
 *                the BG colour expansion (0x68ae), the row loop (1635) merges the
 *                FG expansion (0x68a6) under each row's glyph bits at staging row
 *                y_skip.. (161c: di += y_skip*8), op 15bd trims the cell to w_px
 *                bits, and op 148e blits font[3] (row_count) rows to the screen
 *                at top = y - font[2] (1690: neg dx + [0x6944]; px_height, NOT
 *                +y_skip — y_skip offsets the glyph rows INSIDE the cell).  Then
 *                x += w_px + font[4] (169c; the w_px==0 path 16dd skips both the
 *                draw and the advance).  fg/bg come from the 14ef expansions:
 *                plane p = 0xff iff (colour>>p)&1 — set session-wide by
 *                init_game_session_state's set_text_color(14, 1) (1000:02f1).
 *   The font object is game data: DDFNT2.CAR, loaded by load_graphics_resources
 *   (1000:0a2c; open_resource(4) on the 0x0090 table base) and bound as the graphics-overlay
 *   "current object" via 1000:97df -> 1ab9:132b (font ptr -> DGROUP 0x68a2).  The host
 *   loads the same file (host_resource.c host_load_font) — never embedded in src.
 *
 *   RECONSTRUCTION FIDELITY: the staging-buffer expansion + 1cec codec blit is
 *   collapsed into direct host_vga_rmw4 row writes to the CURRENT draw page (same
 *   per-pixel result: fg colour under the glyph bits, bg colour on the rest of the
 *   w_px × font[3] cell).  Recorded in docs/reconstruction-fidelity.md. */
static u16 hr_text_x  = 0u;      /* model of DGROUP 0x6942 (current text x, pixels) */
static u16 hr_text_y  = 0u;      /* model of DGROUP 0x6944 (current text y, pixels) */
static u8  hr_text_fg = 0x0fu;   /* model of the 0x68a6 fg expansion (colour index) */
static u8  hr_text_bg = 0x00u;   /* model of the 0x68ae bg expansion (colour index) */

void host_text_set_pos(u16 x, u16 y)
{
    hr_text_x = x;
    hr_text_y = y;
}

/* set_text_color's storage (engine: the 1ab9:14ef plane expansions).  The engine
 * session values are fg=14, bg=1 (init_game_session_state → set_text_color). */
void host_text_set_color(u8 fg, u8 bg)
{
    hr_text_fg = (u8)(fg & 0x0fu);
    hr_text_bg = (u8)(bg & 0x0fu);
}

/* Write one glyph-cell row byte: fg planes under the glyph bits, bg planes on the
 * rest of the cell coverage cm, committed with Bit-Mask cm (pixels outside the
 * cell keep the background via the latches). */
static void hr_text_row_write(u16 off, u8 m, u8 cm)
{
    u8 v[4];
    u8 p;
    for (p = 0u; p < 4u; p++) {
        v[p] = (u8)((((hr_text_fg >> p) & 1u) ? m : 0u) |
                    (((hr_text_bg >> p) & 1u) ? (u8)(cm & (u8)~m) : 0u));
    }
    host_vga_rmw4(off, v[0], v[1], v[2], v[3], cm);
}

/* Draw one glyph at the current text position and advance x (1ab9:13bc + 1607). */
static void hr_text_draw_char(u8 ch)
{
    const u8 __far *font = host_font_ptr();
    const u8 __far *g;
    u16 goff;
    u16 pg;
    u16 r;
    u16 cell_rows;
    u8  w;
    u8  cov;
    int top;

    if (font == (const u8 __far *)0) {
        return;   /* DDFNT2.CAR not loaded → NOP (graceful) */
    }
    if (ch < font[0] || ch >= font[1]) {
        return;   /* 13bc range check: first <= ch < last */
    }
    goff = (u16)(((u16)font[6u + (u16)(ch - font[0]) * 2u] << 8) |
                 font[7u + (u16)(ch - font[0]) * 2u]);          /* BE16 offset table */
    g = font + goff;
    w = g[0];
    if (w == 0u) {
        return;   /* 1607: zero-width entry → skip, NO advance (16dd path) */
    }
    if (w > 8u) {
        w = 8u;   /* 1-byte rows bound the cell at 8 px (DDFNT2 glyphs are ≤ 8) */
    }
    cov       = (u8)~(u8)(0xffu >> w);       /* cell coverage: leftmost w bits   */
    top       = (int)hr_text_y - (int)font[2];               /* y - px_height    */
    cell_rows = (u16)font[3];                                /* blit row_count   */
    pg        = host_draw_page_off();
    for (r = 0u; r < cell_rows; r++) {
        u8  m = 0u;                          /* glyph bits: rows y_skip..y_skip+h */
        int y = top + (int)r;
        u16 cell;
        u8  sh;
        if (r >= (u16)g[2] && r < (u16)(g[2] + g[1])) {
            m = (u8)(g[3u + (r - (u16)g[2])] & cov);
        }
        if (y < 0 || y > 199) {
            continue;
        }
        cell = (u16)(pg + (u16)y * 40u + (hr_text_x >> 3));
        sh   = (u8)(hr_text_x & 7u);
        if (sh == 0u) {
            hr_text_row_write(cell, m, cov);
        } else {
            u8 c1 = (u8)(cov >> sh);
            u8 c2 = (u8)(cov << (8u - sh));
            if (c1 != 0u) {
                hr_text_row_write(cell, (u8)(m >> sh), c1);
            }
            if (c2 != 0u && ((hr_text_x >> 3) + 1u) < 40u) {
                hr_text_row_write((u16)(cell + 1u), (u8)(m << (8u - sh)), c2);
            }
        }
    }
    hr_text_x = (u16)(hr_text_x + g[0] + font[4]);              /* advance w + font[4] */
}

void host_text_draw_string(u16 str_off, u16 str_seg)
{
    const u8 __far *s;

    /* The engine's static call sites stamp the Ghidra-static DGROUP segment 0x203b
       into string far ptrs; remap to the loaded image's real DGROUP (the same
       *_DGROUP_RUNTIME_SEG convention the descriptor seg fields use). */
    if (str_seg == 0x203bu) {
        str_seg = host_dgroup_seg();
    }
    s = (const u8 __far *)MK_FP(str_seg, str_off);
    while (*s != 0u) {                                          /* 1ab9:13ec walk */
        hr_text_draw_char(*s);
        s++;
    }
    host_vga_blit_end();   /* restore Bit-Mask=FF / Map=0F after the glyph RMWs */
}

#endif /* BUMPY_PLAYABLE */
