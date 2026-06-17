#include "sprite_blit.h"

/* See sprite_blit.h.  This mirrors the inner per-column arithmetic of the engine
   blitter exactly (ror / current-column mask / inter-column carry / RMW plane
   store), validated byte-exact against the engine plane capture by
   tools/blit_ref.py over 23 distinct real blits (w=1,2,4; shift=0,1).

   On `sel`: the engine's 10e1 dispatches on descriptor[0x15] through a 6-byte table
   at 1cec:0x328f, selecting one of several unrolled column-blit variants.  The
   SPRITE pipeline always sets sel==0 (sprite_blit_setup hardcodes descriptor[0x15]=0),
   so this reconstruction implements the sel==0 path, which is exhaustive for every
   sprite blit.  The sel!=0 table entries are reached only by OTHER (non-sprite)
   callers of 10e1 and are out of scope here (not a dead branch of the sprite path). */

#define PLANE_SIZE  0x10000UL

/* rotate a 16-bit word right by n (n in 0..15) */
static u16 ror16(u16 v, u8 n)
{
    n &= 15;
    if (n == 0) {
        return v;
    }
    return (u16)((v >> n) | (v << (16 - n)));
}

/* swap the two bytes of a 16-bit word (the engine's `xchg bl,bh`) */
static u16 swap_bytes(u16 v)
{
    return (u16)((v << 8) | (v >> 8));
}

void sprite_blit_planar_vga(u8 __huge *planes, const u8 __far *src,
                            u16 voff, u16 dst_stride, u16 full_w,
                            u16 cols, u16 rows, u8 shift, u8 clip_flags)
{
    u16 src_stride = (u16)(4u * full_w);
    /* di = (0xff >> shift) replicated into both bytes = the bits kept in the
       current byte-column after the ror; ~di selects the carry-out bits. */
    u16 di = (u16)(((u16)(0x00FFu >> shift)) * 0x0101u);
    u16 ndi = (u16)~di;
    u16 row;
    u16 col;

    for (row = 0; row < rows; row++) {
        const u8 __far *s = src + (u32)row * src_stride;
        u32 d = (u32)voff + (u32)row * dst_stride;
        u16 bx = 0;
        u16 cx = 0;

        /* Left-edge clip carry preload (engine 1cec:0x210b -> 0x2117): when the
           sprite is clipped on the left (clip_flags bit 1), the first column's
           carry comes from the off-screen column just before `s` (s[-4..-1]),
           rotated like a normal column.  UNVALIDATED: no captured engine blit is
           left-clipped, so this path has no oracle coverage. */
        if (clip_flags & 2) {
            u16 pax = ror16((u16)(((u16)s[-3] << 8) | s[-4]), shift);
            u16 pdx = ror16((u16)(((u16)s[-1] << 8) | s[-2]), shift);
            bx = (u16)(swap_bytes(pax) & ndi);
            cx = (u16)(swap_bytes(pdx) & ndi);
        }

        for (col = 0; col <= cols; col++) {
            u16 sav_ax = 0;
            u16 sav_dx = 0;
            u16 both;
            u8 bm;
            u8 vals[4];
            u8 p;

            if (col < cols) {
                u8 p0 = s[0];
                u8 p1 = s[1];
                u8 p2 = s[2];
                u8 p3 = s[3];
                u16 ax = ror16((u16)(((u16)p1 << 8) | p0), shift);
                u16 dx = ror16((u16)(((u16)p3 << 8) | p2), shift);
                s += 4;
                sav_ax = ax;
                sav_dx = dx;
                bx |= (u16)(ax & di);
                cx |= (u16)(dx & di);
            }

            /* GC bit mask = coverage = OR of all four result bytes */
            both = (u16)(bx | cx);
            bm = (u8)((both >> 8) | (both & 0xFF));
            vals[0] = (u8)(bx & 0xFF);
            vals[1] = (u8)(bx >> 8);
            vals[2] = (u8)(cx & 0xFF);
            vals[3] = (u8)(cx >> 8);

            for (p = 0; p < 4; p++) {
                u32 idx = (u32)p * PLANE_SIZE + d;
                u8 old = planes[idx];
                planes[idx] = (u8)((vals[p] & bm) | (old & (u8)~bm));
            }
            d++;

            if (col < cols) {
                /* carry-out: the bits that rotated past the byte boundary, moved
                   into the next column's write position (xchg bytes, mask ~di). */
                bx = (u16)(swap_bytes(sav_ax) & ndi);
                cx = (u16)(swap_bytes(sav_dx) & ndi);
            } else {
                bx = 0;
                cx = 0;
            }
        }
    }
}
