#include "video.h"

/* Thin stub for the capture-route-(1) slice. Only the BIOS mode-set is wired;
   the planar framebuffer blit is deferred (see video.h). */

void video_set_mode(u8 mode)
{
    union REGS regs;

    regs.h.ah = 0x00;
    regs.h.al = mode;
    int86(0x10, &regs, &regs);
}
