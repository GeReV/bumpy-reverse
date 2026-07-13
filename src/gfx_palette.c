#ifndef BUMPY_H
#include <conio.h>   /* outp — the DAC port intrinsic gfx_upload_palette_vga issues.
                        Skipped on the host replay harness (it #defines BUMPY_H and
                        supplies its own outp/inp shims; see tools/gfx_palette_ctest.c).
                        Under wcc this is the real OUT — mirrors screens.c's guard. */
#endif
#include "gfx_palette.h"
#include "screens.h"   /* palette_mode (DGROUP 0x541d) — the VGA stage +0x30 branch */

/* See gfx_palette.h.

   Faithful 1:1 reconstruction of the graphics-overlay (segment 1ab9, Loriciel-custom
   — NOT Borland BGI) palette pipeline: the 6 per-mode STAGE/UPLOAD handlers + the
   shared per-page draw-object slot-offset helper, transcribed from the raw
   disassembly recovered this session (grounded in docs/faithfulness-gap-audit.md §1's
   1ab9:01e1/02b1 entries + src/host/host_gfx.c's disasm citations for 1ab9:0620/0677).

   RECONSTRUCTION FIDELITY: gfx_draw_object_off/gfx_draw_object_seg (DGROUP
   0x5311/0x5313) are new reconstruction globals modelling the overlay's *0x5311
   draw-object far pointer; the playable host does not wire them to a live
   descriptor yet — it keeps its own side-store (host_gfx_page_palette[] in
   src/host/host_gfx.c).  See the header comment for the full note.
*/

/* gfx_draw_object_off / gfx_draw_object_seg — far ptr to the current draw-object
   descriptor (DGROUP 0x5311/0x5313, `*0x5311` in the disasm).  Zero-initialised;
   nothing in this reconstruction wires a live descriptor into them yet — see the
   RECONSTRUCTION FIDELITY note in gfx_palette.h. */
u16 gfx_draw_object_off;   /* DGROUP 0x5311 */
u16 gfx_draw_object_seg;   /* DGROUP 0x5313 */

/* gfx_page_slot_offset — 1ab9:05b6: per-page draw-object slot = page * 99. */
int gfx_page_slot_offset(u8 page)
{
    return (int)page * GFX_PAL_SLOT_STRIDE;
}

/* gfx_stage_palette_cga — 1ab9:0605: bare RET (no-op). */
void gfx_stage_palette_cga(u8 __far *src, u8 page)
{
    (void)src;
    (void)page;
}

/* gfx_stage_palette_ega — 1ab9:0606: rep movsw 8 words (16 bytes) of the AC-index
   palette from src+0x23 into draw-object[page]+0x23. */
void gfx_stage_palette_ega(u8 __far *src, u8 page)
{
    u8 __far *obj = (u8 __far *)MK_FP(gfx_draw_object_seg, gfx_draw_object_off);
    u8 __far *dst = obj + gfx_page_slot_offset(page) + GFX_PAL_EGA_OFF;
    u8 __far *s   = src + GFX_PAL_EGA_OFF;
    u8 i;
    for (i = 0u; i < 16u; i++) { dst[i] = s[i]; }
}

/* gfx_stage_palette_vga — 1ab9:0620: rep movsw 24 words (48 bytes) from src+0x33
   into +0x33 (+0x30 when palette_mode==5). */
void gfx_stage_palette_vga(u8 __far *src, u8 page)
{
    u8 __far *obj = (u8 __far *)MK_FP(gfx_draw_object_seg, gfx_draw_object_off);
    u16 slot = (u16)(gfx_page_slot_offset(page) + GFX_PAL_VGA_OFF);
    u8 __far *dst;
    u8 __far *s = src + GFX_PAL_VGA_OFF;
    u8 i;
    if (palette_mode == 5u) { slot = (u16)(slot + GFX_PAL_VGA_MODE5_ADJUST); }
    dst = obj + slot;
    for (i = 0u; i < 48u; i++) { dst[i] = s[i]; }
}

/* gfx_upload_palette_cga — 1ab9:0661: bare RET (no-op). */
void gfx_upload_palette_cga(u8 page)
{
    (void)page;
}

/* gfx_upload_palette_ega — 1ab9:0662: INT 10h AX=1002h, program the 16 AC regs
   (+overscan) from draw-object[page]+0x23. */
void gfx_upload_palette_ega(u8 page)
{
    u8 __far *obj = (u8 __far *)MK_FP(gfx_draw_object_seg, gfx_draw_object_off);
    u8 __far *tbl = obj + gfx_page_slot_offset(page) + GFX_PAL_EGA_OFF;
    union REGS r; struct SREGS sr;
    segread(&sr);
    sr.es  = FP_SEG(tbl);
    r.x.dx = FP_OFF(tbl);
    r.x.ax = 0x1002u;                 /* set all palette registers + overscan */
    int86x(0x10, &r, &r, &sr);
}

/* gfx_upload_palette_vga — 1ab9:0677: DAC 3c8/3c9 slots 0..7 & 0x10..0x17 from
   draw-object[page]+0x33 (16 colours x 3 bytes RGB).  Disasm: reads
   *0x5311 + page*99 + 0x33; OUT 0x3c8=0, 8xRGB to 0x3c9, OUT 0x3c8=0x10, 8xRGB
   to 0x3c9 (src/host/host_gfx.c cites the identical sequence for 1ab9:0677). */
void gfx_upload_palette_vga(u8 page)
{
    u8 __far *obj = (u8 __far *)MK_FP(gfx_draw_object_seg, gfx_draw_object_off);
    u8 __far *tbl = obj + gfx_page_slot_offset(page) + GFX_PAL_VGA_OFF;
    u8 i;
    outp(GFX_DAC_INDEX_PORT, 0x00);                 /* DAC write index 0 */
    for (i = 0u; i < 8u; i++) {
        outp(GFX_DAC_DATA_PORT, tbl[0]);           /* R */
        outp(GFX_DAC_DATA_PORT, tbl[1]);           /* G */
        outp(GFX_DAC_DATA_PORT, tbl[2]);           /* B */
        tbl += 3;
    }
    outp(GFX_DAC_INDEX_PORT, 0x10);                 /* DAC write index 0x10 (colours 8..15) */
    for (i = 0u; i < 8u; i++) {
        outp(GFX_DAC_DATA_PORT, tbl[0]);
        outp(GFX_DAC_DATA_PORT, tbl[1]);
        outp(GFX_DAC_DATA_PORT, tbl[2]);
        tbl += 3;
    }
}

/* gfx_stage_palette_dispatch / gfx_upload_palette_dispatch — palette_mode-keyed
   dispatch to the STAGE/UPLOAD handlers above.

   RECONSTRUCTION FIDELITY: the engine dispatches via the static DGROUP cmdvec
   tables (cmdvec_stage_palette_modes 0x5435 = {0605,0606,0620};
   cmdvec_upload_palette_modes 0x5441 = {0661,0662,0677}); the switch is the
   1:1 equivalent of that indirection. Tables documented in Ghidra (typed
   word[3], 2026-07-11). */
void gfx_stage_palette_dispatch(u8 __far *src, u8 page)
{
    switch (palette_mode) {
    case 0u: gfx_stage_palette_cga(src, page); break;   /* 1ab9:0605 */
    case 1u: gfx_stage_palette_ega(src, page); break;   /* 1ab9:0606 */
    default: gfx_stage_palette_vga(src, page); break;   /* 1ab9:0620 (mode 2) */
    }
}

void gfx_upload_palette_dispatch(u8 page)
{
    switch (palette_mode) {
    case 0u: gfx_upload_palette_cga(page); break;       /* 1ab9:0661 */
    case 1u: gfx_upload_palette_ega(page); break;       /* 1ab9:0662 */
    default: gfx_upload_palette_vga(page); break;       /* 1ab9:0677 (mode 2) */
    }
}
