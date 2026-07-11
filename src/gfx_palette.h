#ifndef GFX_PALETTE_H
#define GFX_PALETTE_H

#include "bumpy.h"

/* ────────────────────────────────────────────────────────────────────────────
 * gfx_palette.h — faithful C mirror of the graphics-overlay (segment 1ab9,
 * Loriciel-custom — NOT Borland BGI; see CLAUDE.md + docs/rendering-pipeline.md
 * §1) PALETTE PIPELINE: the 6 per-mode STAGE/UPLOAD handlers + the shared
 * per-page draw-object slot-offset helper, transcribed 1:1 from the raw
 * disassembly recovered this session.
 *
 * Model: a *draw-object* descriptor (far ptr at DGROUP 0x5311/0x5313, `*0x5311`
 * in the disasm) holds, per VGA page, a palette slot at
 * `object + page*99 + 0x23` (EGA, 16-byte AC-index palette) and
 * `object + page*99 + 0x33` (VGA, 48-byte 6-bit RGB palette).  STAGE handlers
 * copy the embedded palette out of a decoded-image buffer into that slot;
 * UPLOAD handlers push the staged slot to the display hardware (INT 10h AC
 * registers for EGA, DAC ports 0x3c8/0x3c9 for VGA).
 *
 *   1ab9:05b6  gfx_page_slot_offset    — draw-object slot = page * 99
 *   1ab9:0605  gfx_stage_palette_cga   — bare RET (no-op)
 *   1ab9:0606  gfx_stage_palette_ega   — rep movsw 16B AC-index palette (@+0x23)
 *   1ab9:0620  gfx_stage_palette_vga   — rep movsw 48B RGB palette (@+0x33;
 *                                        @+0x63 when palette_mode==5)
 *   1ab9:0661  gfx_upload_palette_cga  — bare RET (no-op)
 *   1ab9:0662  gfx_upload_palette_ega  — INT 10h AX=1002h from the staged @+0x23
 *                                        AC table
 *   1ab9:0677  gfx_upload_palette_vga  — DAC 0x3c8/0x3c9 writes from the staged
 *                                        @+0x33 RGB table
 *
 * RECONSTRUCTION FIDELITY — gfx_draw_object_off / gfx_draw_object_seg (DGROUP
 * 0x5311/0x5313) are NEW reconstruction globals modelling the overlay's
 * `*0x5311` draw-object far pointer.  The playable host does NOT wire them to a
 * live descriptor — it keeps its own two-entry side-store
 * (host_gfx_page_palette[2][48] in src/host/host_gfx.c) instead of the full
 * *0x5311 descriptor heap; a future task wires the two together.
 * gfx_stage_palette_vga / gfx_upload_palette_vga duplicate the behaviour
 * already modelled in host_gfx.c's side-store — the versions here are the
 * faithful 1:1 form keyed off the *0x5311 descriptor slot.  The default
 * BUMPY.EXE build compiles these handlers but its render leaves are stubbed;
 * the playable build continues to route palette through host_gfx.c until that
 * task rewires it onto these handlers.  See docs/reconstruction-fidelity.md.
 * ──────────────────────────────────────────────────────────────────────────── */

/* gfx_draw_object_off / gfx_draw_object_seg — far ptr to the current draw-object
 * descriptor (DGROUP 0x5311/0x5313, `*0x5311` in the disasm).  DEFINED (init 0)
 * in gfx_palette.c; see the RECONSTRUCTION FIDELITY note above. */
extern u16 gfx_draw_object_off;   /* DGROUP 0x5311 */
extern u16 gfx_draw_object_seg;   /* DGROUP 0x5313 */

/* gfx_page_slot_offset — 1ab9:05b6: per-page draw-object slot = page * 99. */
int gfx_page_slot_offset(u8 page);

/* Palette STAGE handlers — copy the embedded palette from a decoded-image
 * buffer (src) into the draw-object's per-page slot (page selects the slot via
 * gfx_page_slot_offset). */
void gfx_stage_palette_cga(u8 __far *src, u8 page);   /* 1ab9:0605 — NOP */
void gfx_stage_palette_ega(u8 __far *src, u8 page);   /* 1ab9:0606 — 16B @+0x23 */
void gfx_stage_palette_vga(u8 __far *src, u8 page);   /* 1ab9:0620 — 48B @+0x33 */

/* Palette UPLOAD handlers — push the staged per-page slot to the display
 * hardware. */
void gfx_upload_palette_cga(u8 page);   /* 1ab9:0661 — NOP               */
void gfx_upload_palette_ega(u8 page);   /* 1ab9:0662 — INT 10h AX=1002h  */
void gfx_upload_palette_vga(u8 page);   /* 1ab9:0677 — DAC 0x3c8/0x3c9   */

#endif /* GFX_PALETTE_H */
