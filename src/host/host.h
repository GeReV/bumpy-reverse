/* host.h — pure-DOS platform layer (BUMPY_PLAYABLE). Shared decls + hardware
 * register constants. RECONSTRUCTION FIDELITY: this whole layer is the documented
 * platform leaves IMPLEMENTED (the Devilution model); the faithful default build
 * links game_stubs.c instead. See docs/reconstruction-fidelity.md. */
#ifndef HOST_H
#define HOST_H
#include "bumpy.h"
/* VGA */
#define VGA_SEG_PAGE0   0xA000u      /* page 0 plane window */
#define VGA_SEG_PAGE1   0xA200u      /* page 1 (== A000 + 0x200 paragraphs = +0x2000 B) */
#define VGA_PLANE_BYTES 0x1F40u      /* 320x200/8 per plane per page */
#define VGA_ROW_BYTES   40u
#define SEQ_INDEX       0x3C4u
#define SEQ_DATA        0x3C5u
#define SEQ_MAP_MASK    0x02u
#define CRTC_INDEX      0x3D4u
#define CRTC_DATA       0x3D5u
#define CRTC_START_HI   0x0Cu
#define CRTC_START_LO   0x0Du
#define HOST_PLANE_SIZE 0x10000UL    /* the reconstructed blitters' flat-plane stride */
/* the host framebuffer (flat 4-plane RAM image the blitters compose into) */
extern u8 __huge *host_framebuffer;  /* 4 * HOST_PLANE_SIZE */
void host_fb_init(void);             /* allocate + register the page table into it */
#endif
