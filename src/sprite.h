#ifndef SPRITE_H
#define SPRITE_H

#include "bumpy.h"

/* Sprite bank (BUMSPJEU) load-time transform — faithful C port of the engine's
   sprite_bank_relocate_frames (1cec:0c34) + sprite_frame_transform (1cec:0c77),
   the palette_mode-dispatched post-process applied once after the sprite resource
   is read.  Turns the on-disk big-endian bank into the in-memory form the blitter
   consumes: per frame, byte-swaps the 12-byte header (BE->LE) and reformats the
   pixel words (de-interleave; palette_mode==0 / CGA also bit-reverses + zeroes).

   The bank holds a BE32 frame-offset table at +0 and pixel data from +0x800;
   frame i's pixels begin at bank + 0x800 + table[i], 12-byte header just before.
   The transform is in place. */

/* Transform the whole sprite bank in place for the given palette_mode
   (2 = EGA/VGA on the real DOS build).  Walks the frame table to the 0 terminator. */
void sprite_bank_load_transform(u8 __far *bank, u8 palette_mode);

#endif /* SPRITE_H */
