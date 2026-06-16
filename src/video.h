#ifndef VIDEO_H
#define VIDEO_H

#include "bumpy.h"

/* Video layer for the VEC render slice.
   Provides BIOS mode control and a plane-at-a-time planar blit to VGA mode
   0x0D (320x200x16 EGA planar, sequencer map-mask write mode 0). */

#define VIDEO_MODE_VGA13  0x13   /* 320x200x256 (chunky)  */
#define VIDEO_MODE_EGA0D  0x0d   /* 320x200x16 (planar)   */

/* Set mode 0x0D: 320x200x16 EGA planar via INT 10h AX=0x000D. */
void video_set_mode_0d(void);

/* Restore 80x25 text mode via INT 10h AX=0x0003. */
void video_restore_text(void);

/* Program the DAC for 16 colours.  dac[i*3+0..2] are 6-bit R,G,B values for
   colour index i.  Written to ports 0x3C8/0x3C9 starting at DAC index 0. */
void video_set_palette6(const u8 *dac);

/* Blit a 32000-byte plane-major planar buffer to VGA 0xA000:0.
   Layout: plane p at buf[p*8000 .. p*8000+8000).  Uses sequencer map-mask
   (ports 0x3C4/0x3C5) with write mode 0 (one plane per pass). */
void video_blit_planar(const u8 *planar);

/* Legacy: set an arbitrary BIOS video mode via INT 10h AH=0. */
void video_set_mode(u8 mode);

#endif /* VIDEO_H */
