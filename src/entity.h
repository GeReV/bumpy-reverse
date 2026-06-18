#ifndef ENTITY_H
#define ENTITY_H

#include "bumpy.h"
#include "sprite_chain.h"   /* sprite_view */

/* Faithful C port of the layer-C static-sprite draw loop inlined in
   spawn_and_draw_level_entities (1000:2a78).

   Layer C is purely BUM-data-sourced: for each of the 48 grid cells
   (6 rows x 8 cols), if the cell byte bum[0x60+cell] is nonzero, the engine
   builds a temporary sprite object and blits it.  No runtime animation state
   is involved — every cell is fully determined by the level header.

   This function extracts that loop into a standalone, host-testable function
   and drives it through the VALIDATED pipeline:
     sprite_prepare_frame -> sprite_blit_build_desc -> sprite_blit_planar_vga

   --- RECONSTRUCTION FIDELITY ---
   * STRUCTURE-faithful: the loop bounds (0..5 rows, 0..7 cols), cell index
     formula (row*8+col), BUM offset (0x60), frame bias (0x179), and the three
     pipeline stages mirror the engine's inlined code exactly.
   * BEHAVIOR-faithful at the pixel level: validated by comparing the
     bg+layer-C composite against the captured engine frame (bg-only 79.0% ->
     bg+C should be substantially higher; see Task 4 report for results).
   * Object layout: the engine reuses the shared p1_sprite struct at DGROUP:0x792e
     for all entity blits.  Here we build an equivalent 0x18-byte stack object per
     cell; only the caller-set fields (+0, +2, +4, +0xa) differ between cells;
     sprite_prepare_frame fills the rest (+6/8 frame-table ptr, +0b ctrl,
     +0c/0e prepared ptr, +10 width, +12 height, +14/16 anchors).
   * posC table: the engine reads this from DGROUP:0x274 (interleaved XY pairs,
     4 bytes/cell, X at 0x274+cell*4, Y at 0x276+cell*4).  The `dg` parameter
     carries the captured full DGROUP snapshot; reads are done from that to
     preserve exact engine values (identical to anim_tables.json posC).
   * The blit is not driven through the engine's blit_sprite wrapper (which
     also calls sprite_prepare_frame internally); here we call each stage
     directly to match the validated pipeline structure in anim_ctest.c /
     chain_ctest.c.
   * No erase / restore_bg_view pass: layer-C is a one-way draw-only pass (no
     background erasing between frames for static sprites in this context).
*/

/* Draw all nonzero layer-C cells from `bum` into `planes`.
   Parameters:
     planes        — 4-plane VGA buffer (4 * 0x10000 B), plane p at p*0x10000
     bum           — pointer to the level block at tilemap base; layer-C grid at
                     bum[0x60+cell].  Note: the FRM3 oracle captures this block with
                     a +2 byte offset (hdr_lin = block_lin + 2); callers using FRM3
                     data must subtract 2 from the captured pointer before passing here.
     dg            — 0x10000-byte DGROUP snapshot; posC table at dg[0x274..0x393]
     bank          — sprite bank (bank_inmem.bin), flat huge buffer
     bank_base_lin — runtime linear address of bank[0] (0x4eae0 for bank_inmem.bin)
     view          — sprite viewport (full-screen: left=0,right=40,top=0,
                     bottom=199,height=199,data_off=0,data_seg=0xa000)
*/
void entity_draw_layer_c(u8 __huge *planes, const u8 __far *bum,
                         const u8 __far *dg, u8 __huge *bank,
                         u32 bank_base_lin, const sprite_view *view);

#endif /* ENTITY_H */
