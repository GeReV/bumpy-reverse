#include <string.h>
#include "bgi_overlay.h"

/* See bgi_overlay.h.

   Behavioral reconstruction of the two BGI-overlay dispatch wrappers:
     restore_bg_view   (1000:80bc → 1ab9:0d77)  — bg-tile erase
     render_player_view (1000:93b8 → 1ab9:1028 → 1ab9:0db0)  — planar copy

   Both are NOPs in the layer-A/B context (code-embedded view descriptors,
   word[0] or word[0x0e] > 1 → NOP guard trips).  Reconstructed faithfully
   per the disasm in local/build/present_model.md §3.

   Host-build notes: __far / __huge are shimmed away for gcc (composite_ctest.c
   defines them away before including any src/ file).  All plane arithmetic uses
   u32 offsets into flat arrays to avoid DOS segmented-address behaviour.
*/

/* Local shorthands (avoid collisions with sprite_blit.c / bg_render.c macros
   when all .c files are #include'd into one translation unit). */
#define BGI_OVL_PLANE_SIZE    BGI_PLANE_SIZE    /* 0x10000UL */
#define BGI_OVL_PAGE_SIZE     BGI_PAGE_SIZE     /* 0x1F40UL = 8000 */
#define BGI_OVL_ROWS          BGI_ROWS          /* 200 */
#define BGI_OVL_ROW_BYTES     BGI_ROW_BYTES     /* 40 */
#define BGI_OVL_PAGE_A200_OFF BGI_PAGE_A200_OFF /* 0x2000UL */
#define BGI_OVL_PAGE_A000_OFF BGI_PAGE_A000_OFF /* 0x0000UL */

/* -----------------------------------------------------------------------
   render_player_view_full_copy — sub-handler 0 (1ab9:0de0)
   Reconstructs the 4-plane GC Read-Map-Select + rep-movsw copy:
     for plane in 0..3:
       memcpy(dest_base[plane] + dest_off,
              src_base[plane]  + src_off,
              ROWS × ROW_BYTES)
   Source: indexed by view->word00 via the page table (0 → a200, 1 → a000).
   Dest: view->dest_seg:view->dest_off (split far ptr).

   In the engine, "memcpy" is done row-by-row via `rep movsw` with:
     source row stride = 40 (VGA scanline pitch)
     dest row stride   = [DGROUP:0x5429] (= 40 in the fullscreen_buf path)
   For simplicity (behavior-faithful), we copy 200×40 bytes in one block
   per plane, which is byte-identical for stride=40.

   NOTE: on real VGA, the source plane is selected via OUT 3CE/3CF index 4
   (GC Read Map Select) so that reads from the MMIO window return bytes from
   the chosen plane.  In the host memory model there is no VGA MMIO — each
   plane is an independent array slice, so we just copy the right slice.
   ----------------------------------------------------------------------- */
static void render_player_view_full_copy(u8 __huge *dest_buf,
                                         const u8 __huge *src_planes,
                                         u32 src_page_off,
                                         u32 dest_lin_off)
{
    u8 plane;

    for (plane = 0; plane < 4u; plane++) {
        const u8 __huge *src = src_planes
                             + (u32)plane * BGI_OVL_PLANE_SIZE
                             + src_page_off;
        u8 __huge *dst = dest_buf
                       + (u32)plane * BGI_OVL_PAGE_SIZE
                       + dest_lin_off;
        memcpy((void __huge *)dst,
               (const void __huge *)src,
               (size_t)(BGI_OVL_ROWS * BGI_OVL_ROW_BYTES));
    }
}

/* -----------------------------------------------------------------------
   restore_bg_view — 1000:80bc → bgi_set_mode_01 (1ab9:0d77)

   Faithfully reconstructs the outer dispatch wrapper:
     1. Check view->word0e (field at +0x0e): if > 1 → return (NOP).
     2. Set global [0x541f] = 0 (source from view+0x02, not pointer table).
     3. Set global [0x5420] = 1 (dest from pointer table[view->word0e]).
     4. Dispatch via mode-01 VGA handler (1ab9:0aa0 for palette_mode=2).

   The inner mode-01 blit (1ab9:0aa0) is the masked bg-tile blitter,
   reconstructed in bg_render.c (bg_tile_run / bg_render_grid).  Here we
   model only the dispatch wrapper; the actual erase copy (fullscreen_buf →
   VGA page) is modelled as a plain plane-by-plane memcpy of the bg source.

   For the fullscreen_buf restore path (sub-handler 0, setup_fullscreen_view):
     - source = vga_src (the fullscreen_buf capture, planar sequential)
     - dest   = VGA page indexed by view->word0e (0 → a200, 1 → a000)
     Copies PAGE_SIZE bytes per plane into planes[word0e_page_off].

   EFFECTIVE IN HARNESS: NOP for all layer-A/B calls because the code-embedded
   view descriptors have word0e > 1.
   ----------------------------------------------------------------------- */
void restore_bg_view(u8 __huge *planes,
                     const u8 __huge *vga_src,
                     const bgi_view_desc __far *view)
{
    u16 word0e;
    u32 dest_page_off;
    u8  plane;

    /* bgi_set_mode_01 (1ab9:0d77) guard: if view->word0e > 1 → NOP.
       In layer-A/B context: erase_view at 0x114b:0x74a0 starts with machine
       code; word0e (view+0x0e) = 0x85b3 > 1 → always NOP. */
    word0e = view->word0e;
    if (word0e > 1u) {
        return;
    }

    /* Dest page selection: table[word0e]
         word0e == 0 → a200:0000 (plane offset 0x2000)
         word0e == 1 → a000:0000 (plane offset 0x0000) */
    if (word0e == 0u) {
        dest_page_off = BGI_OVL_PAGE_A200_OFF;
    } else {
        dest_page_off = BGI_OVL_PAGE_A000_OFF;
    }

    /* Source = vga_src (fullscreen_buf: planar sequential, 4 × PAGE_SIZE).
       Copy each plane's PAGE_SIZE bytes from vga_src into the dest VGA page.
       Mirrors the fullscreen_buf → page copy driven by setup_fullscreen_view +
       restore_bg_view (sub-handler 0 of 1ab9:0aa0 for the erase path). */
    for (plane = 0; plane < 4u; plane++) {
        const u8 __huge *src = vga_src + (u32)plane * BGI_OVL_PAGE_SIZE;
        u8 __huge *dst = planes
                       + (u32)plane * BGI_OVL_PLANE_SIZE
                       + dest_page_off;
        memcpy((void __huge *)dst,
               (const void __huge *)src,
               (size_t)BGI_OVL_PAGE_SIZE);
    }
}

/* -----------------------------------------------------------------------
   render_player_view — 1000:93b8 → bgi_set_mode_10 (1ab9:1028) → 1ab9:0db0

   Faithfully reconstructs:
     1. render_player_view (1000:93b8): 7-byte wrapper — calls bgi_set_mode_10
        with the view far ptr.
     2. bgi_set_mode_10 (1ab9:1028): check view->word00; if > 1 → return (NOP).
        Set [0x541f]=1 (source from pointer table), [0x5420]=0 (dest from
        les view+0x10), call 1ab9:0db0 (mode-10 VGA handler, pm=2).
     3. 1ab9:0db0: call 1ab9:052d (setup src DS:SI / dest ES:DI from view
        descriptor + pointer table), then sub-dispatch on view->subhandler via
        DGROUP[idx*2 + 0x568a].

   Sub-handlers:
     0 → render_player_view_full_copy (4-plane rep-movsw, fully reconstructed)
     1,2 → NOP (ret in the engine)
     3,4,5,6 → STUBBED (masked copy variants) — UNVALIDATED

   EFFECTIVE IN HARNESS: NOP for all layer-A/B calls because the code-embedded
   draw_view at 0x114b:0x751e starts with 0xc3fb (sti; ret), so word00 = 0xc3fb
   > 1 → guard trips immediately.
   ----------------------------------------------------------------------- */
void render_player_view(u8 __huge *planes,
                        const u8 __huge *vga_src,
                        const bgi_view_desc __far *view)
{
    u16 word00;
    u32 src_page_off;
    u32 dest_lin_base;
    u16 dest_seg;
    u16 dest_off;

    /* bgi_set_mode_10 (1ab9:1028) guard: if view->word00 > 1 → NOP.
       In layer-A/B context: draw_view at 0x114b:0x751e starts with 0xc3fb
       (sti; ret) → word00 = 0xc3fb > 1 → always NOP. */
    word00 = view->word00;
    if (word00 > 1u) {
        return;
    }

    /* Source page selection: pointer table[word00]
         word00 == 0 → a200:0000 (VGA plane offset 0x2000, sprite scratch)
         word00 == 1 → a000:0000 (VGA plane offset 0x0000, visible page) */
    if (word00 == 0u) {
        src_page_off = BGI_OVL_PAGE_A200_OFF;
    } else {
        src_page_off = BGI_OVL_PAGE_A000_OFF;
    }

    /* Dest: split far ptr from view->dest_seg:view->dest_off (les view+0x10).
       Convert to a linear offset into the planes buffer for sub-handler 0.
       Engine uses 1cda:0089 to paragraph-normalize; here we map seg:off to a
       flat offset for the host memory model. */
    dest_off = view->dest_off;
    dest_seg = view->dest_seg;

    /* Compute flat linear offset relative to the planes buffer start.
       planes[] models VGA plane memory starting at 0xa000:0000; a given
       dest seg:off maps to planes_offset = (dest_seg - 0xa000) * 16 + dest_off.
       For fullscreen_buf (0x67bf:0000): this is outside VGA range — the host
       should pass a separate destination buffer.  For the layer-A/B NOP path
       this arithmetic is never reached (guard fires above).
       We compute it anyway for structural completeness; sub-handler 0 uses it. */
    dest_lin_base = ((u32)dest_seg * 16UL + (u32)dest_off);

    /* Sub-dispatch on view->subhandler (mirrors DGROUP[idx*2+0x568a]) */
    switch (view->subhandler) {
        case 0u:
            /* Sub-handler 0 (1ab9:0de0): full 4-plane planar copy.
               dest_lin_base is an absolute linear address; relative offset
               into the dest buffer is 0 for the full-page copy case. */
            render_player_view_full_copy(planes, vga_src,
                                         src_page_off, 0UL);
            break;
        case 1u:
        case 2u:
            /* Sub-handlers 1,2 (1ab9:0f35 / 0ec3): explicit ret — NOP. */
            break;
        case 3u:
        case 4u:
        case 5u:
        case 6u:
            /* Sub-handlers 3-6: masked copy variants (AND bitmask + OR).
               UNVALIDATED — stub only.  Full reconstruction deferred; grounded
               in disasm at local/build/present_model.md §3.6.
               In all observed engine captures, only sub-handler 0 is active
               for setup_fullscreen_view; 3-6 are not triggered in the layer-A/B
               or fullscreen_buf paths captured in the oracle. */
            break;
        default:
            /* Unknown sub-handler index — treat as NOP (defensive). */
            break;
    }

    /* Suppress unused variable warning when the sub-handler 0 body is
       compiled but dest_lin_base is not consumed (non-zero sub-handler path). */
    (void)dest_lin_base;
}
