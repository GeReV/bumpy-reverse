#ifndef GFX_OVERLAY_H
#define GFX_OVERLAY_H

#include "bumpy.h"

/* Faithful C reconstructions of the two BGI-overlay dispatch functions used
   by the entity draw pipeline:

   restore_bg_view   (1000:80bc) → gfx_set_mode_01 (1ab9:0d77)
     Background / erase blit.  Mode-01 dispatcher: keys on view->word[0x0e];
     if > 1 → NOP.  When active: [0x541f]=0 (source from view+0x02), [0x5420]=1
     (dest from pointer table[view+0x0e]), then calls the mode-01 VGA handler
     (1ab9:0aa0) — the masked bg-tile blit (reconstructed in bg_render.c).
     Used in the engine's erase phase to restore fullscreen_buf over the old
     entity cell before blitting the new position.

   render_player_view (1000:93b8) → gfx_set_mode_10 (1ab9:1028)
     Planar rectangular COPY between a VGA page and a destination buffer.
     Mode-10 dispatcher: keys on view->word[0] (= view[0..1] LE u16); if > 1
     → NOP.  When active: [0x541f]=1 (source from pointer table[word[0]]),
     [0x5420]=0 (dest from les view+0x10), then calls the mode-10 VGA handler
     (1ab9:0db0) → sub-dispatches on view->word[0x1c]:
       0 → full 4-plane copy (GC Read-Map-Select + rep movsw, 200×40 bytes)
       1,2 → ret (NOP)
       3,4,5 → masked copy (AND bitmask + OR) with preset masks
       6 → masked copy variant
     Used for save-unders (capture VGA page → system buffer) and
     read-backs / buffer present.

   --- KEY FINDING (present_model.md §5) ---
   In draw_anim_channels_a (1000:165e) and draw_anim_channels_b (1000:17c7),
   BOTH the erase_view (0x114b:0x74a0) and the draw_view (0x114b:0x751e)
   descriptors BEGIN with machine code (0xb385 / 0xc3fb respectively).
   For gfx_set_mode_01: word[0x0e] of erase_view = 0x85b3 > 1 → NOP.
   For gfx_set_mode_10: word[0] of draw_view = 0xc3fb > 1 → NOP.
   Consequence: ALL restore_bg_view / render_player_view calls inside
   draw_anim_channels_a/b are structural NOPs for every entity blit.
   The visible entity pixels come solely from blit_sprite_vga (step 2).

   The reconstruction still models the full behavior of each function including
   the guard — the host harness passes NULL views (word[0] = 0xffff/machine-
   code equivalent) so the guards fire and the functions are no-ops for the
   composite harness.  The structural call sequence in entity_draw_layer_a/_b
   faithfully mirrors the engine's erase → blit → save-under sequence.

   --- RECONSTRUCTION FIDELITY ---
   * restore_bg_view: BEHAVIOR-FAITHFUL reconstruction of 1000:80bc +
     1ab9:0d77.  The inner mode-01 blit (1ab9:0aa0) is reconstructed in
     bg_render.c (bg_render_grid / bg_tile_run); here we model only the outer
     dispatch wrapper.  Sub-handler 0 (full copy from fullscreen_buf) is the
     only path exercised by setup_fullscreen_view; the other sub-handlers are
     UNVALIDATED.  EFFECTIVE: NOP in layer-A/B context (code-embedded views).
   * render_player_view: BEHAVIOR-FAITHFUL reconstruction of 1000:93b8 +
     1ab9:1028 + 1ab9:0db0 (mode-10).  Sub-handler 0 (full 4-plane copy,
     GC Read-Map-Select + rep movsw) is fully reconstructed; sub-handlers
     3–6 (masked copy variants) are STUBBED + marked UNVALIDATED.
     EFFECTIVE: NOP in layer-A/B context (code-embedded views).
   * The host composite harness does NOT call these functions directly — the
     composite's entity_draw_layer_a/_b call them via the structural call but
     pass a NULL/stub view that trips the NOP guard, so the composite pixel
     result is unchanged.  Both functions are STRUCTURALLY PRESENT, documented,
     and their NOP nature in the layer-A/B context is grounded in the decomp.
*/

/* View descriptor layout for mode-10 (render_player_view) and mode-01
   (restore_bg_view).  Far pointer fields are kept split as the engine does
   (seg and off separately) to avoid DOS MK_FP at the C level.

   word[0x00] (u16) — mode-10: source-page index (0=a200:0000, 1=a000:0000);
                               NOP guard: > 1 → skip.
   word[0x0e] (u16) — mode-01: dest-page index; NOP guard: > 1 → skip.
   word[0x10] (u16) — mode-10 dest far ptr offset (les view+0x10).
   word[0x12] (u16) — mode-10 dest far ptr segment.
   word[0x1c] (u16) — mode-10 sub-handler index (0=full copy, 1-2=NOP,
                                                   3-6=masked variants).
   All other fields are set per-entity in draw_anim_channels_a/b. */
typedef struct {
    u16 word00;          /* +0x00 source index / NOP guard */
    u16 _pad[6];         /* +0x02..0x0d padding / source geometry */
    u16 word0e;          /* +0x0e dest index / mode-01 NOP guard */
    u16 dest_off;        /* +0x10 dest far ptr offset (mode-10) */
    u16 dest_seg;        /* +0x12 dest far ptr segment (mode-10) */
    u16 _pad2[4];        /* +0x14..0x1b */
    u16 subhandler;      /* +0x1c sub-dispatch index (mode-10) */
} gfx_view_desc;

/* VGA page table (mirrors sprite_table_base at DGROUP:0x5415):
     entry[0] = a200:0000  (VGA plane offset 0x2000 — page1 / sprite-scratch)
     entry[1] = a000:0000  (VGA plane offset 0x0000 — page0 / visible)
   Indexed by view->word00 (mode-10) or view->word0e (mode-01). */
#define GFX_PAGE_A200_OFF  0x2000UL  /* VGA byte offset for a200:0000 */
#define GFX_PAGE_A000_OFF  0x0000UL  /* VGA byte offset for a000:0000 */
#define GFX_PAGE_SIZE      0x1F40UL  /* 200 rows × 40 bytes = 8000 B per plane */
/* bytes per plane in host work buffer — must match HOST_PLANE_SIZE.  HOST_FB_16K
   (playable EXE) → 16 KB/plane; default build + ctests → full 64 KB (byte-unchanged). */
#ifdef HOST_FB_16K
#define GFX_PLANE_SIZE     0x4000UL
#else
#define GFX_PLANE_SIZE     0x10000UL
#endif
#define GFX_ROWS           200u      /* display rows */
#define GFX_ROW_BYTES      40u       /* VGA row stride (320px ÷ 8 bits) */

/* restore_bg_view — wrapper for gfx_set_mode_01 (1000:80bc → 1ab9:0d77).
   Parameters:
     planes      — 4-plane host work buffer (4 × 0x10000 B), plane p at p×0x10000
     vga_src     — pointer into a flat VGA-page buffer (used as the bg source region);
                   for the fullscreen_buf restore path, this is the save-under capture.
     view        — view descriptor far ptr (engine: DX:AX).
                   NOP guard: if view->word0e > 1, returns immediately (no blit).
                   In layer-A/B context: view is code-embedded, word0e > 1 → NOP.
   Reconstruction: BEHAVIOR-FAITHFUL (1000:80bc + 1ab9:0d77 outer dispatch).
   The inner blit (1ab9:0aa0) models are in bg_render.c; this reconstructs
   only the dispatch wrapper and the fullscreen_buf→page copy path.
   UNVALIDATED in the harness (NOP for all layer-A/B calls). */
void restore_bg_view(u8 __huge *planes,
                     const u8 __huge *vga_src,
                     const gfx_view_desc __far *view);

/* render_player_view — wrapper for gfx_set_mode_10 (1000:93b8 → 1ab9:1028).
   Parameters:
     planes      — 4-plane host work buffer.
     vga_src     — VGA source page base pointer (indexed by view->word00).
                   word00=0 → a200 (VGA offset 0x2000); word00=1 → a000 (offset 0).
     view        — view descriptor.
                   NOP guard: if view->word00 > 1, returns immediately.
                   In layer-A/B context: view is code-embedded, word00 > 1 → NOP.
   Sub-handler dispatch (view->subhandler):
     0 → render_player_view_full_copy: full 4-plane rep-movsw copy (reconstructed).
     1,2 → NOP.
     3,4,5,6 → STUBBED (masked copy variants, UNVALIDATED).
   Reconstruction: BEHAVIOR-FAITHFUL (1000:93b8 + 1ab9:1028 + 0db0 + 0de0).
   Sub-handler 0 faithfully reconstructs the GC Read-Map-Select + rep-movsw
   4-plane loop from the disasm in local/build/present_model.md §3.
   UNVALIDATED in the harness (NOP for all layer-A/B calls). */
void render_player_view(u8 __huge *planes,
                        const u8 __huge *vga_src,
                        const gfx_view_desc __far *view);

#endif /* GFX_OVERLAY_H */
