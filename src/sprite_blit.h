#ifndef SPRITE_BLIT_H
#define SPRITE_BLIT_H

#include "bumpy.h"

/* Faithful C port of the planar-VGA masked sprite blitter sprite_blit_planar_vga
   (overlay 1cec:10e1).  The engine programs the VGA Graphics/Sequencer registers
   (GC Mode/Data-Rotate, Seq Map-Mask, GC Bit-Mask) and writes the prepared frame
   to 0xA000 column-by-column via a self-modifying, unrolled, jump-table blitter.
   The pixel SEMANTICS (verified byte-exact against the engine, see the 5b design
   doc and tools/blit_ref.py) are modelled here as a portable 4-plane operation:

     per row: carry accumulators bx,cx = 0; per column read 4 source bytes
     = plane0..3, ror by `shift`, keep current-column bits (mask 0xff>>shift in
     both bytes), OR into the carry; write the 4 planes under a GC-bit-mask RMW
     (plane = (val & bm) | (old & ~bm)); recompute the inter-column carry.  A
     final spill column writes the bits that rotated past the byte boundary.

   `planes` is a 4 * 0x10000 buffer laid out plane0||plane1||plane2||plane3 (the
   VGA mode-0xD plane image; plane p byte at offset p*0x10000 + voff).  On the real
   DOS build the same bytes are produced in hardware via the map-mask/bit-mask OUTs.

   Inputs come from the blit descriptor (DGROUP 0x26bd5) built by sprite_blit_setup
   / sprite_blit_clip:
     voff       dest byte offset into the plane image (dst_seg*16+dst_off-0xA0000)
     dst_stride row pitch in bytes (descriptor +0x0e, =0x28=40 for the 320px view)
     full_w     stored frame width in columns (descriptor +0x0c); src pitch = 4*full_w
     cols       visible/drawn columns (descriptor +0x10)
     rows       row count (descriptor +0x12)
     shift      sub-byte horizontal shift 0..7 (descriptor +0x16)

   The dominant path (sel==0) covers every blit observed under the engine.  The clip
   left-edge carry preload (clip_flags bit 1) is ported but UNVALIDATED (no captured
   blit is left-clipped).  sel!=0 (the alternate column dispatch) is not reconstructed
   — see the note in sprite_blit.c.

   --- RECONSTRUCTION FIDELITY (deviates from the engine) ---
   * BEHAVIOR-faithful, NOT structure-faithful: 1cec:10e1 does not decompile (a
     self-modifying, unrolled, jump-table blitter), so this is a semantic
     reconstruction validated byte-exact against the engine's plane output, NOT a
     transcription of the original code.  The original's structure is not preserved.
   * Operates on a 4-plane MEMORY image; the engine drives VGA hardware (out 0x3ce/
     0x3c4 map-mask/bit-mask + 0xA000 RMW writes).  The plane bytes produced are
     identical; the register-OUT sequence is not reproduced. */
void sprite_blit_planar_vga(u8 __huge *planes, const u8 __far *src,
                            u16 voff, u16 dst_stride, u16 full_w,
                            u16 cols, u16 rows, u8 shift, u8 clip_flags);

#endif /* SPRITE_BLIT_H */
