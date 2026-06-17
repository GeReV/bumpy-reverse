#ifndef SPRITE_CHAIN_H
#define SPRITE_CHAIN_H

#include "bumpy.h"

/* Faithful C port of the sprite-blit geometry chain that builds the 0x18-byte
   blit descriptor consumed by sprite_blit_planar_vga (see sprite_blit.h):
     sprite_blit_object_list (1cec:0e48) — per object: screen cell x/y, shift,
       visibility + view-bounds cull;
     sprite_blit_clip        (1cec:0f50) — left/right/top/bottom clip margins,
       visible width/height, dest byte offset;
     sprite_blit_setup       (1cec:103d) — assemble the descriptor.
   All three decompile cleanly; this mirrors them (decomp-grounded, validated
   descriptor-exact against the engine by tools/chain_ctest.c).

   Sprite object fields (set by prepare_sprite_frames):
     +0x00 X (s16)            +0x02 Y (s16)         +0x0a flags
       (0x80 visible, 0x20 h-flip, 0x01 align-to-8)
     +0x0c prepared-frame far ptr (off @0x0c, seg @0x0e)
     +0x10 width in words     +0x12 height (rows)
     +0x14 X anchor (s16)     +0x16 Y anchor (s16)

   The view window + the current VGA page base are engine globals: */
typedef struct {
    s16 left;       /* iRam00026bbd — view left bound  (cells) */
    s16 right;      /* iRam00026bbf — view right bound (cells) */
    s16 top;        /* iRam00026bc1 — view top bound   (rows)  */
    s16 bottom;     /* iRam00026bc3 — view bottom bound (rows) */
    s16 height;     /* iRam00026bc7 — view height      (rows)  */
    u16 data_off;   /* cur_sprite_data_off — VGA dest base offset */
    u16 data_seg;   /* cur_sprite_data_seg — VGA dest segment (page-flipped) */
} sprite_view;

/* Build the blit descriptor for one sprite object. Returns 1 and fills desc[0x18]
   if the sprite is visible and overlaps the view; 0 (descriptor untouched) if the
   object is hidden or fully culled.  Mirrors object_list's per-object body +
   clip + setup. */
int sprite_blit_build_desc(const u8 __far *obj, const sprite_view *view, u8 *desc);

#endif /* SPRITE_CHAIN_H */
