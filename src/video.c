#include "video.h"
#include <conio.h>  /* outp */

/* Video layer: BIOS mode control + planar-VGA output for the VEC render slice.
   video_blit_planar writes a 32000-byte plane-major buffer to VGA mode 0x0D
   (320x200x16 planar) using write mode 0 with the VGA sequencer map-mask.
   See docs: plane p, row y, byte b at planar[p*8000 + y*40 + b]. */

void video_set_mode_0d(void)
{
    union REGS regs;

    regs.x.ax = 0x000D;   /* AH=0, AL=0x0D: set mode 0D */
    int86(0x10, &regs, &regs);
}

void video_restore_text(void)
{
    union REGS regs;

    regs.x.ax = 0x0003;   /* AH=0, AL=0x03: 80x25 text */
    int86(0x10, &regs, &regs);
}

void video_set_palette6(const u8 *dac)
{
    /* dac: 16*3 bytes of 6-bit DAC values (R,G,B per colour index 0..15). */
    u16 i;

    outp(0x3C8, 0x00);   /* start at DAC entry 0 */
    for (i = 0; i < 16u * 3u; i++) {
        outp(0x3C9, dac[i] & 0x3F);
    }
}

void video_blit_planar(const u8 *planar)
{
    /* Blit a 32000-byte plane-major planar buffer to VGA 0xA000:0.
       Layout: plane p at planar[p*8000 .. p*8000+8000).
       Write mode 0: select each plane with the sequencer map-mask register,
       then copy 8000 bytes verbatim. */

    u8 far *vga = (u8 far *)MK_FP(0xA000, 0x0000);
    u16 p;
    u16 i;

    /* Ensure GC write mode 0 with no bit-mask gating (all bits pass through). */
    outp(0x3CE, 0x05); outp(0x3CF, 0x00);  /* GC mode register: write mode 0 */
    outp(0x3CE, 0x08); outp(0x3CF, 0xFF);  /* GC bit mask: all bits enabled   */

    outp(0x3C4, 0x02);   /* sequencer address = map-mask register */
    for (p = 0; p < 4u; p++) {
        outp(0x3C5, (u8)(1u << p));   /* enable only plane p */
        for (i = 0; i < 8000u; i++) {
            vga[i] = planar[p * 8000u + i];
        }
    }
}

/* Legacy stub kept for back-compat; delegates to video_set_mode_0d. */
void video_set_mode(u8 mode)
{
    union REGS regs;

    regs.h.ah = 0x00;
    regs.h.al = mode;
    int86(0x10, &regs, &regs);
}
