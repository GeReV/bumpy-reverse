#include <string.h>
#include "gfx_overlay.h"

/* See gfx_overlay.h.

   NAMING (2026-07-11): the segment-1ab9 overlay + these `gfx_*` symbols were RENAMED
   from `bgi_*` — an early naming pass wrongly assumed Borland BGI.  The overlay is the
   game's OWN Loriciel VGA planar driver — NOT stock Borland BGI (verified: no
   EGAVGA.BGI linked or loaded, no Borland driver banner in the image; only a 42-byte
   incidental code match).  `gfx_` = "the graphics overlay".  See CLAUDE.md +
   docs/rendering-pipeline.md §1.

   Behavioral reconstruction of the two overlay dispatch wrappers:
     restore_bg_view   (1000:80bc → 1ab9:0d77)  — bg-tile erase (mode-01)
     render_player_view (1000:93b8 → 1ab9:1028 → 1ab9:0db0)  — planar copy (mode-10)

   Both dispatch guards are SIGNED in the original (asm CMP/JG): the handler
   runs iff the descriptor's guard word (word[0] / word[0x0e]) is < 2 as a
   signed int.  Reconstructed faithfully per the disasm in
   local/build/present_model.md §3.  (An earlier note claimed these were
   "always NOP" in the layer-A/B context via an unsigned compare; that premise
   was wrong — see the signed-guard note at each call site.)

   Host-build notes: __far / __huge are shimmed away for gcc (composite_ctest.c
   defines them away before including any src/ file).  All plane arithmetic uses
   u32 offsets into flat arrays to avoid DOS segmented-address behaviour.
*/

/* Local shorthands (avoid collisions with sprite_blit.c / bg_render.c macros
   when all .c files are #include'd into one translation unit). */
#define GFX_OVL_PLANE_SIZE    GFX_PLANE_SIZE    /* 0x10000UL */
#define GFX_OVL_PAGE_SIZE     GFX_PAGE_SIZE     /* 0x1F40UL = 8000 */
#define GFX_OVL_ROWS          GFX_ROWS          /* 200 */
#define GFX_OVL_ROW_BYTES     GFX_ROW_BYTES     /* 40 */
#define GFX_OVL_PAGE_A200_OFF GFX_PAGE_A200_OFF /* 0x2000UL */
#define GFX_OVL_PAGE_A000_OFF GFX_PAGE_A000_OFF /* 0x0000UL */

#ifdef BUMPY_PLAYABLE
/* DGROUP write-mode flags (0x541f / 0x5420) that the graphics-overlay mode-01/mode-10
   dispatchers set before calling their handler; defined in host/host_gfx.c. */
extern u8 gfx_write_mode_flag_a;
extern u8 gfx_write_mode_flag_b;
#endif

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
/* NOTE on the plane-stride asymmetry below: the SOURCE is a VGA page (plane stride
   GFX_OVL_PLANE_SIZE = 0x10000, the full 64KB plane window) while the DEST is the
   PACKED fullscreen_buf (plane stride GFX_OVL_PAGE_SIZE = 0x1F40, 4 contiguous planes).
   This matches the only call shape exercised (save-under to fullscreen_buf). If ever
   invoked with a VGA-page DEST, the dest stride would need to be GFX_OVL_PLANE_SIZE.
   UNVALIDATED: this path is unreachable in the harness (sub-handler 0 fires only when
   the gfx_set_mode_10 NOP guard passes, which never happens for the layer-A/B views). */
static void render_player_view_full_copy(u8 __huge *dest_buf,
                                         const u8 __huge *src_planes,
                                         u32 src_page_off,
                                         u32 dest_lin_off)
{
    u8 plane;

    for (plane = 0; plane < 4u; plane++) {
        const u8 __huge *src = src_planes
                             + (u32)plane * GFX_OVL_PLANE_SIZE
                             + src_page_off;
        u8 __huge *dst = dest_buf
                       + (u32)plane * GFX_OVL_PAGE_SIZE
                       + dest_lin_off;
        memcpy((void __huge *)dst,
               (const void __huge *)src,
               (size_t)(GFX_OVL_ROWS * GFX_OVL_ROW_BYTES));
    }
}

/* -----------------------------------------------------------------------
   restore_bg_view — 1000:80bc → gfx_set_mode_01 (1ab9:0d77)

   Faithfully reconstructs the outer dispatch wrapper:
     1. Check view->word0e (field at +0x0e): if > 1 → return (NOP).
     2. Set global [0x541f] = 0 (source from view+0x02, not pointer table).
     3. Set global [0x5420] = 1 (dest from pointer table[view->word0e]).
     4. Dispatch via mode-01 VGA handler (1ab9:0aa0 for palette_mode=2).

   The inner mode-01 blit (1ab9:0aa0) is the masked bg-tile blitter,
   reconstructed in bg_render.c (bg_tile_run / bg_render_grid).  Here we
   model only the dispatch wrapper; the actual erase copy (fullscreen_buf →
   VGA page) is modelled as a plain plane-by-plane memcpy of the bg source.

   CLIPPED-RECT blit (the engine's actual behavior, NOT a blind full-page copy):
   the view descriptor carries the blit rectangle —
     view+0x0a (u16) = source/copy WIDTH  in tiles  (×2 bytes/tile)
     view+0x0c (u16) = source/copy HEIGHT in tiles  (×8 px/tile)
     view+0x14 (u16) = dest origin X in tiles
     view+0x16 (u16) = dest origin Y in tiles
   The source (vga_src) is a planar-sequential image PACKED at its own width:
   plane p starts at vga_src + p*(W_bytes*H_px); rows are W_bytes apart.  The
   dest is the VGA page (row stride 40, plane stride 0x10000) at the tile origin.

   This is a strict generalization of the old full-page copy: the full-screen
   background descriptor (W=0x14, H=0x19, origin 0,0) reduces to the identical
   200×40 bytes/plane contiguous copy, byte-for-byte.  But a SUB-RECT descriptor
   — run_main_menu's 6×2 option strip at tile (0xb,0x12), or
   show_object_preview_wait_input's 20×1 strip — now blits ONLY its rectangle
   instead of smearing a full page of the (small) source over the whole screen.
   The copy extent is driven by +0x0a/+0x0c (the source dims), matching every
   word0e≤1 call site (run_main_menu / show_object_preview_wait_input); the
   +0x1e/+0x20 "extent" fields belong to the mode-10 path, not this one.

   EFFECTIVE IN HARNESS: still NOP for all layer-A/B calls (code-embedded view
   descriptors have word0e > 1); the gameplay erase/anim-channel pipeline and the
   composite ctests never reach the copy.  RECONSTRUCTION FIDELITY: recorded in
   docs/reconstruction-fidelity.md ("playable host" section).
   ----------------------------------------------------------------------- */
void restore_bg_view(u8 __huge *planes,
                     const u8 __huge *vga_src,
                     const gfx_view_desc __far *view)
{
    u16 word0e;
    u32 dest_page_off;
    u8  plane;

    /* gfx_set_mode_01 (1ab9:0d77) guard is SIGNED in the original
       (asm `CMP BP,1` / `JG`; decomp `if (*(int*)(AX+0xe) < 2)`): do the work iff
       (s16)word0e < 2.  An earlier reconstruction used an UNSIGNED compare
       (word0e > 1u), which diverges for high-bit word0e values — e.g. the
       0x85b3 the default-build layer-A/B descriptor reads from code is < 2 when
       signed, so the original dispatches there rather than NOPing.  In the
       playable build word0e is a real 0/1, so signed and unsigned agree. */
    word0e = view->word0e;
    if ((s16)word0e >= 2) {
        return;
    }

#ifdef BUMPY_PLAYABLE
    /* gfx_set_mode_01 sets these DGROUP write-mode flags (0x541f/0x5420) before
       dispatching to the mode-01 handler; host_gfx tracks them. */
    gfx_write_mode_flag_a = 0u;
    gfx_write_mode_flag_b = 1u;
#endif

    /* Dest page selection: table[word0e]
         word0e == 0 → a200:0000 (plane offset 0x2000)
         word0e == 1 → a000:0000 (plane offset 0x0000) */
    if (word0e == 0u) {
        dest_page_off = GFX_OVL_PAGE_A200_OFF;
    } else {
        dest_page_off = GFX_OVL_PAGE_A000_OFF;
    }

#ifdef BUMPY_PLAYABLE
    /* CLIPPED-RECT blit — the executed (playable) path.  Reads the blit rectangle
       from the descriptor and copies ONLY that rectangle (see the function header).
       This is the faithful behavior; the #else full-page copy below is retained for
       the default BUMPY.EXE so that build's gfx_overlay.obj stays byte-identical
       (it is byte-compared, never executed — its restore_bg_view is unreachable at
       runtime, so the behavioral content is immaterial there). */
    {
        const u8 __far *vb = (const u8 __far *)view;
        u16 w_tiles, h_tiles, dx_tiles, dy_tiles;
        u16 row_bytes, rows, row;
        u32 dst_origin_off, src_plane_stride;

        /* LE u16 reads — portable across the DOS far-pointer build and the
           flat-pointer host ctest build. */
        w_tiles  = (u16)((u16)vb[0x0a] | ((u16)vb[0x0b] << 8));
        h_tiles  = (u16)((u16)vb[0x0c] | ((u16)vb[0x0d] << 8));
        dx_tiles = (u16)((u16)vb[0x14] | ((u16)vb[0x15] << 8));
        dy_tiles = (u16)((u16)vb[0x16] | ((u16)vb[0x17] << 8));

        row_bytes = (u16)(w_tiles * 2u);   /* 16-px tiles → 2 bytes/plane-row */
        rows      = (u16)(h_tiles * 8u);   /* 8-px-tall tiles                 */
        if (row_bytes == 0u || rows == 0u) {
            return;
        }

        /* Source: planar-sequential, packed at its own width.
           Dest: VGA page, row stride 40, at the tile origin. */
        src_plane_stride = (u32)row_bytes * (u32)rows;
        dst_origin_off   = dest_page_off
                         + (u32)((u32)dy_tiles * 8u) * GFX_OVL_ROW_BYTES
                         + (u32)dx_tiles * 2u;

        for (plane = 0; plane < 4u; plane++) {
            const u8 __huge *src = vga_src + (u32)plane * src_plane_stride;
            u8 __huge *dst = planes
                           + (u32)plane * GFX_OVL_PLANE_SIZE
                           + dst_origin_off;
            for (row = 0; row < rows; row++) {
                memcpy((void __huge *)(dst + (u32)row * GFX_OVL_ROW_BYTES),
                       (const void __huge *)(src + (u32)row * row_bytes),
                       (size_t)row_bytes);
            }
        }
    }
#else
    /* Default build (byte-compared, never executed): the original behavioral model
       — a flat plane-by-plane PAGE_SIZE copy.  Kept verbatim so gfx_overlay.obj is
       byte-stable in BUMPY.EXE.  See the #ifdef branch above for the executed,
       clip-aware playable path. */
    for (plane = 0; plane < 4u; plane++) {
        const u8 __huge *src = vga_src + (u32)plane * GFX_OVL_PAGE_SIZE;
        u8 __huge *dst = planes
                       + (u32)plane * GFX_OVL_PLANE_SIZE
                       + dest_page_off;
        memcpy((void __huge *)dst,
               (const void __huge *)src,
               (size_t)GFX_OVL_PAGE_SIZE);
    }
#endif
}

/* -----------------------------------------------------------------------
   render_player_view — 1000:93b8 → gfx_set_mode_10 (1ab9:1028) → 1ab9:0db0

   Faithfully reconstructs:
     1. render_player_view (1000:93b8): 7-byte wrapper — calls gfx_set_mode_10
        with the view far ptr.
     2. gfx_set_mode_10 (1ab9:1028): check view->word00; if > 1 → return (NOP).
        Set [0x541f]=1 (source from pointer table), [0x5420]=0 (dest from
        les view+0x10), call 1ab9:0db0 (mode-10 VGA handler, pm=2).
     3. 1ab9:0db0: call 1ab9:052d (setup src DS:SI / dest ES:DI from view
        descriptor + pointer table), then sub-dispatch on view->subhandler via
        DGROUP[idx*2 + 0x568a].

   Sub-handlers:
     0 → render_player_view_full_copy (4-plane rep-movsw, fully reconstructed)
     1,2 → NOP (ret in the engine)
     3,4,5,6 → STUBBED (masked copy variants) — UNVALIDATED

   Guard is SIGNED (see the call-site note): the handler runs iff (s16)word00 < 2.
   In the playable build word00 is a real 0/1; the default-build layer-A/B
   descriptor reads 0xc3fb from code, which is < 2 signed (the original would
   dispatch — the earlier "always NOP" claim used an unsigned compare).
   ----------------------------------------------------------------------- */
void render_player_view(u8 __huge *planes,
                        const u8 __huge *vga_src,
                        const gfx_view_desc __far *view)
{
    u16 word00;
    u32 src_page_off;
    u32 dest_lin_base;
    u16 dest_seg;
    u16 dest_off;

    /* gfx_set_mode_10 (1ab9:1028) guard is SIGNED (asm `CMP BP,1` / `JG`;
       decomp `if (*(int*)AX < 2)`): run the handler iff (s16)word00 < 2.  Earlier
       reconstruction used an unsigned compare (word00 > 1u), which NOPs for
       high-bit word00 (e.g. the 0xc3fb the default-build descriptor reads from
       code is < 2 signed → the original dispatches).  In the playable build
       word00 is a real 0/1, so signed and unsigned agree. */
    word00 = view->word00;
    if ((s16)word00 >= 2) {
        return;
    }

#ifdef BUMPY_PLAYABLE
    /* gfx_set_mode_10 sets these DGROUP write-mode flags (0x541f/0x5420) before
       dispatching to the mode-10 handler (note: 1/0, opposite of mode-01). */
    gfx_write_mode_flag_a = 1u;
    gfx_write_mode_flag_b = 0u;
#endif

    /* Source page selection: pointer table[word00]
         word00 == 0 → a200:0000 (VGA plane offset 0x2000, sprite scratch)
         word00 == 1 → a000:0000 (VGA plane offset 0x0000, visible page) */
    if (word00 == 0u) {
        src_page_off = GFX_OVL_PAGE_A200_OFF;
    } else {
        src_page_off = GFX_OVL_PAGE_A000_OFF;
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
