#ifndef BG_RENDER_H
#define BG_RENDER_H

#include "bumpy.h"

/* Faithful C port of the per-cell background tile build restore_bg_tile_run
   (1000:0a90) plus the masked planar tile blit it drives (via restore_bg_view ->
   the BGI overlay).  The blit's pixel semantics were reconstructed byte-exact
   against the engine (tools/bg_blit_ref.py over the engine plane capture).

   For one playfield cell it reads the tile id(s) from the level map and copies
   16x16 tiles from the PAV atlas into the VGA planes:
     - tile id  = map[cell_x*0x27 + (cell_y>>1)*3 + col + 0x20] - 1
     - run cell (run_code >= 0xf1): col_count = ((-run_code)&0xff) - 5 sub-tiles
       at cols 1..col_count-1, all at the same cell (first = opaque base, the rest
       overlay through palette index 0 = transparent); normal cell = one tile.
     - atlas_col = id % 20, atlas_row = id / 20.

   Layouts:
     planes  4 * 0x10000, plane p byte at p*0x10000 + (cy*8+ry)*40 + cx*2 + bx
             (VGA mode-0xD plane image).
     atlas   the PAV raster (buffer +6): 320x192 4-plane PLANE-SEQUENTIAL,
             byte = atlas[p*7680 + row*40 + bcol].
     map     the level grid (_cur_level_ptr), 3 bytes/cell.

   The opaque write also clears 2 bytes past the tile each row (clipped at the
   right screen edge); the masked overlay writes per-bit where any plane bit is set.

   --- RECONSTRUCTION FIDELITY (deviates from the engine) ---
   * The run-loop (bg_tile_run) faithfully mirrors restore_bg_tile_run (1000:0a90).
   * The actual tile blit is BEHAVIOR-faithful, not a port: the engine builds a
     descriptor and calls restore_bg_view -> the BGI overlay (palette_mode dispatch)
     to do the masked planar putimage, which is not cleanly decompilable.  Here the
     "build descriptor + BGI putimage" is collapsed into a direct blit whose pixel
     output was reconstructed byte-exact against the engine.
   * Operates on a 4-plane MEMORY image, not the VGA-hardware OUT sequence (the plane
     bytes are identical; the register OUTs / 0xA000 writes are not reproduced). */
void bg_tile_run(u8 __huge *planes, const u8 __huge *atlas, const u8 __far *map,
                 u8 run_code, u16 cell_x, u16 cell_y);

/* Build the whole playfield background: iterate the 20x13 cell grid (cell_x 0..19,
   cell_y 0,2,..,24) and draw each cell.  The per-cell run_code is the grid byte
   map[cx*0x27 + (cy>>1)*3 + 0x20].  Mirrors start_level's tile loop. */
void bg_render_grid(u8 __huge *planes, const u8 __huge *atlas, const u8 __far *map);

#endif /* BG_RENDER_H */
