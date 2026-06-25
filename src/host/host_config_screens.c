/* ════════════════════════════════════════════════════════════════════════════
 * host_config_screens.c — playable host: the boot graphics/sound selection screens.
 *
 * The original BUMPY.EXE boots into two interactive TEXT-mode config screens before
 * the game proper:
 *   1. gfx_driver_init (1000:…)  — "< F1 >: CGA … < F6 >: VGA256", sets palette_mode
 *      (1 = EGA, 2 = VGA) after a BIOS hardware probe of a 6-entry device table.
 *   2. sound_select_device front-end — "< F5 >: NO SOUND … < F8 >: MT32".
 *
 * The playable build previously SKIPPED both (main.c hardcoded palette_mode = 2 and the
 * audio path auto-selects).  These are reconstructed here as host text-mode screens so
 * the recompilation shows its real, interactive boot — the first proof that it runs.
 *
 * RECONSTRUCTION FIDELITY (documented divergence): the on-screen prompt text and the
 * F-key→mode mapping are reproduced 1:1 from the binary's DOS strings; the original's
 * non-decompilable BIOS hardware-probe / mode-flip machinery (which greys out absent
 * adapters) is replaced by a host text-screen that simply accepts the key.  Because the
 * host render path is the validated EGA/VGA planar pipeline, the graphics screen maps
 * F2→palette_mode 1 (EGA) and every other choice→2 (VGA, the validated path); the sound
 * screen is cosmetic (host audio is Tier-2 / silent) but present + interactive.  Input
 * is read via BIOS INT 16h (the host INT9 ISR is not installed until
 * init_game_session_state, which runs after these screens).
 * Recorded in docs/reconstruction-fidelity.md ("playable host" section).
 * ════════════════════════════════════════════════════════════════════════════ */
#ifdef BUMPY_PLAYABLE

#include "../bumpy.h"
#include "../screens.h"   /* palette_mode (DGROUP 0x541d) */
#include <dos.h>          /* int86, union REGS, MK_FP */

/* Scancodes for F1..F8 (BIOS INT 16h returns the scancode in AH for function keys). */
#define HC_F1 0x3Bu
#define HC_F2 0x3Cu
#define HC_F3 0x3Du
#define HC_F4 0x3Eu
#define HC_F5 0x3Fu
#define HC_F6 0x40u
#define HC_F7 0x41u
#define HC_F8 0x42u

static void hc_set_text_mode(void)
{
    union REGS r;
    r.w.ax = 0x0003u;          /* AH=00 set video mode, AL=03 = 80x25 16-colour text */
    int86(0x10, &r, &r);
}

/* Write a string to the 80x25 colour text buffer at B800:(row*80+col), attr 0x07. */
static void hc_puts_at(u8 row, u8 col, const char *s)
{
    u8 __far *v = (u8 __far *)MK_FP(0xB800u, (u16)(((u16)row * 80u + col) * 2u));
    while (*s != '\0') {
        *v++ = (u8)*s++;
        *v++ = 0x07u;
    }
}

/* Block on a BIOS keystroke; return its scancode (AH). */
static u8 hc_read_scancode(void)
{
    union REGS r;
    r.h.ah = 0x00u;
    int86(0x16, &r, &r);
    return r.h.ah;
}

/* Graphics-adapter select.  Sets palette_mode; F2=EGA→1, anything else→2 (VGA). */
void host_gfx_select(void)
{
    hc_set_text_mode();
    hc_puts_at(2, 2, "BUMPY (C) LORICIEL 1992");
    hc_puts_at(5, 2, "< F1 >: CGA    < F2 >: EGA    < F3 >: VGA");
    hc_puts_at(6, 2, "< F4 >: TANDY  < F5 >: MCGA   < F6 >: VGA256");
    hc_puts_at(9, 2, "Press a function key to select the display adapter...");
    for (;;) {
        u8 sc = hc_read_scancode();
        if (sc == HC_F2) { palette_mode = 1; return; }            /* EGA */
        if (sc >= HC_F1 && sc <= HC_F6) { palette_mode = 2; return; } /* VGA (validated path) */
    }
}

/* Sound-device select.  Cosmetic in the host build (Tier-2 silent); present + interactive. */
void host_audio_select(void)
{
    hc_set_text_mode();
    hc_puts_at(2, 2, "BUMPY (C) LORICIEL 1992");
    hc_puts_at(5, 2, "< F5 >: NO SOUND");
    hc_puts_at(6, 2, "< F6 >: PC BASE");
    hc_puts_at(7, 2, "< F7 >: ADLIB");
    hc_puts_at(8, 2, "< F8 >: MT32");
    hc_puts_at(10, 2, "Press a function key to select the sound device...");
    for (;;) {
        u8 sc = hc_read_scancode();
        if (sc >= HC_F5 && sc <= HC_F8) { return; }
    }
}

#endif /* BUMPY_PLAYABLE */
