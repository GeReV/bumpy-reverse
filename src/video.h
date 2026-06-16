#ifndef VIDEO_H
#define VIDEO_H

#include "bumpy.h"

/* Minimal video layer for the VEC render slice.

   Capture strategy for this first slice is route (1): the decoded 320x200
   chunky image is written to a file via DOS INT 21h (see src/dosio.c), which
   fully validates the decode without depending on the emulator's planar-VGA
   model. So this layer is a thin stub now: it only sets the BIOS video mode.
   The faithful planar blit to 0xA000 (the way the game draws) is deferred to a
   later slice and will grow here. */

#define VIDEO_MODE_VGA13  0x13   /* 320x200x256 (chunky)  */
#define VIDEO_MODE_EGA0D  0x0d   /* 320x200x16 (planar)   */

/* Set the BIOS video mode via INT 10h AH=00. */
void video_set_mode(u8 mode);

#endif /* VIDEO_H */
