/* ════════════════════════════════════════════════════════════════════════════
 * config_screens.c — the boot graphics + sound device select screens.
 *
 * These are reconstructions of two REAL engine functions (not host platform glue):
 *   - gfx_driver_init           (1ab9:02ce)  graphics-adapter / palette select
 *   - sound_device_select_screen (202c:0000) sound-device select
 * (The sound screen was Ghidra-mislabeled "detect_video_adapter"; it is NOT a video
 * probe — it draws the F5..F8 sound menu and sets sound_device_state.)  Both are 1:1
 * reconstructions of inline-INT-10h/21h disassembly that does not cleanly decompile.
 * They live in src/ proper (the faithful decompilation), not src/host/ — they are
 * engine menu logic, only built into the playable image (BUMPY_PLAYABLE); the default
 * BUMPY.EXE boot does not call them.  The host platform leaves (timer/keyboard ISRs,
 * framebuffer, DAC, file I/O) stay under src/host/.
 *
 * gfx_driver_init (1ab9:02ce) disassembly:
 *
 *   1ab9:02d8  int 10h AX=0002h          set 80x25 text mode 0x02
 *   1ab9:02dd  si=529c; cursor (1,1);    print header "BUMPY (C) LORICIEL 1992$"
 *              int 21h AH=09h            (DOS print-string, '$'-terminated)
 *   1ab9:02ef  si=52b4; di=548b; cx=6;   loop the 6-entry adapter table at 0x548b,
 *              dx=0a21h (row 10, col 33) printing the menu line for each present
 *              adapter (table byte == 1), one row down (inc dh) per line printed.
 *   1ab9:0316  poll get_key_state(F2 0x3c) / get_key_state(F3 0x3d): F2 -> palette_mode
 *              (DAT_203b_541d) = 1, F3 -> = 2; loop until one is pressed.  Both arms
 *              also set DAT_203b_530e = 0x40.
 *
 * The shipped adapter table is { 00 01 01 00 00 00 } — only entries 1 (EGA) and 2
 * (VGA) are present — so the real screen shows exactly two lines, "< F2 >: EGA" and
 * "< F3 >: VGA", at column 33, rows 10-11.  The other four lines (CGA/TANDY/MCGA/
 * VGA256) are in the table but not printed.  All six strings + the table are
 * reproduced verbatim from the binary (DGROUP 0x529c / 0x52b4 / 0x548b).
 *
 * ── RECONSTRUCTION FIDELITY (two documented host divergences) ──
 *  (1) Input path: the original reads keys via get_key_state(scancode) over the
 *      engine's g_key_state_table (DGROUP 0x4d42), which the game's INT9 ISR
 *      populates.  In the host boot order this screen runs BEFORE
 *      init_game_session_state installs that ISR, so the table is never filled.  The
 *      host therefore reads the same F2/F3 gate via BIOS INT 16h instead.  The
 *      on-screen result (mode, strings, layout, F2->EGA / F3->VGA) is identical.
 *  (2) Adapter table: the host uses the shipped static table { 0,1,1,0,0,0 } rather
 *      than running the live hardware probe (detect_video_adapter, 202c:0000) that
 *      would overwrite it on real hardware.  EGA+VGA-present is the correct result
 *      for the validated VGA path the host targets.
 *  DAT_203b_530e = 0x40 is an engine graphics-overlay flag with no host effect; not reproduced.
 * Recorded in docs/reconstruction-fidelity.md ("playable host" section).
 * ════════════════════════════════════════════════════════════════════════════ */
#ifdef BUMPY_PLAYABLE

#include "bumpy.h"
#include "screens.h"      /* palette_mode (DGROUP 0x541d) */
#include "config_screens.h"
#include <dos.h>          /* int86, int86x, segread, union REGS, struct SREGS */

/* Scancodes the gates accept (BIOS INT 16h returns the scancode in AH). */
#define HC_F2 0x3Cu       /* gfx:   -> palette_mode = 1 (EGA) */
#define HC_F3 0x3Du       /* gfx:   -> palette_mode = 2 (VGA) */
#define HC_F5 0x3Fu       /* sound: -> sound_device_state = 0x8000 (no sound) */
#define HC_F6 0x40u       /* sound: -> sound_device_state = 0      (PC base) */
#define HC_F7 0x41u       /* sound: -> sound_device_state = 1      (AdLib)   */
#define HC_F8 0x42u       /* sound: -> sound_device_state = 4      (MT-32)   */

/* Menu cursor position (row 10, col 33) — shared by gfx_driver_init and
 * sound_device_select_screen. */
#define HC_MENU_ROW 0x0Au
#define HC_MENU_COL 0x21u

/* DGROUP 0x689c — set by the sound-select screen, consumed by sound_select_device
 * (1000:6de3): 0x8000 == force-muted, else a device id fed to snddrv_init's mask. */
extern s16 sound_device_state;

/* The header + 6 adapter menu lines, verbatim from DGROUP 0x529c / 0x52b4 (each a
 * 14-char field, '$'-terminated for DOS INT 21h AH=09h, matching the 15-byte stride). */
static const char hc_header[]   = "BUMPY (C) LORICIEL 1992$";
static const char hc_lines[6][16] = {
    "< F1 >: CGA   $",
    "< F2 >: EGA   $",
    "< F3 >: VGA   $",
    "< F4 >: TANDY $",
    "< F5 >: MCGA  $",
    "< F6 >: VGA256$",
};
/* Shipped adapter-present table @ DGROUP 0x548b: only EGA (1) and VGA (2) present. */
static const u8 hc_adapter_present[6] = { 0u, 1u, 1u, 0u, 0u, 0u };

/* int 10h AH=00h — set video mode AL. */
static void hc_set_mode(u8 mode)
{
    union REGS r;
    r.h.ah = 0x00u;
    r.h.al = mode;
    int86(0x10, &r, &r);
}

/* int 10h AH=02h — set cursor position (page 0, row, col). */
static void hc_set_cursor(u8 row, u8 col)
{
    union REGS r;
    r.h.ah = 0x02u;
    r.h.bh = 0x00u;
    r.h.dh = row;
    r.h.dl = col;
    int86(0x10, &r, &r);
}

/* int 21h AH=09h — print a '$'-terminated string at the current cursor (DS:DX). */
static void hc_dos_print(const char *s)
{
    union REGS r;
    struct SREGS sr;
    segread(&sr);
    sr.ds  = FP_SEG(s);
    r.h.ah = 0x09u;
    r.x.dx = FP_OFF(s);
    int86x(0x21, &r, &r, &sr);
}

/* Block on a BIOS keystroke; return its scancode (AH). */
static u8 hc_read_scancode(void)
{
    union REGS r;
    r.h.ah = 0x00u;
    int86(0x16, &r, &r);
    return r.h.ah;
}

/* ── gfx_driver_init (1ab9:02ce) — graphics-adapter / palette select ──────────────
 * Sets palette_mode: F2 -> 1 (EGA), F3 -> 2 (VGA).  See file header for fidelity. */
void gfx_driver_init(void)
{
    u8 i;
    u8 row;

    hc_set_mode(0x02u);                    /* 1ab9:02d8  int 10h AX=0002h */

    hc_set_cursor(1u, 1u);                 /* 1ab9:02e2  dx=0101h (row 1, col 1) */
    hc_dos_print(hc_header);               /* 1ab9:02e9  int 21h AH=09h @ 529c */

    /* 1ab9:02ef  loop the 6 adapter-table entries, printing present ones at col 33,
     *            rows starting at 10 (dx=0a21h), one row down per printed line. */
    row = HC_MENU_ROW;
    for (i = 0u; i < 6u; i = i + 1u) {
        if (hc_adapter_present[i] == 1u) {
            hc_set_cursor(row, HC_MENU_COL);
            hc_dos_print(hc_lines[i]);
            row = row + 1u;                /* 1ab9:030e  inc dh */
        }
    }

    /* 1ab9:0316  poll F2 / F3 until one is pressed (host: BIOS INT 16h). */
    for (;;) {
        u8 sc = hc_read_scancode();
        if (sc == HC_F2) { palette_mode = 1; return; }   /* 1ab9:032a  EGA */
        if (sc == HC_F3) { palette_mode = 2; return; }   /* 1ab9:0338  VGA */
    }
}

/* The sound screen's header + 4 menu lines, verbatim from DGROUP 0x6840 / 0x6858
 * (16-char fields, '$'-terminated; 17-byte stride). */
static const char hc_snd_header[]   = "BUMPY (C) LORICIEL 1992$";
static const char hc_snd_lines[4][18] = {
    "< F5 >: NO SOUND$",
    "< F6 >: PC BASE $",
    "< F7 >: ADLIB   $",
    "< F8 >: MT32    $",
};
/* Present table @ DGROUP 0x689e: all four devices offered. */
static const u8 hc_snd_present[4] = { 1u, 1u, 1u, 1u };

/* ── sound_device_select_screen (202c:0000) — sound-device select ─────────────────
 * Twin of gfx_driver_init; sets sound_device_state (consumed by sound_select_device).
 * F5 -> 0x8000 (no sound), F6 -> 0 (PC base), F7 -> 1 (AdLib), F8 -> 4 (MT-32).
 * Same two host divergences as host_gfx_select (BIOS INT 16h input; static present
 * table instead of the live probe). */
void sound_device_select_screen(void)
{
    u8 i;
    u8 row;

    hc_set_mode(0x02u);                    /* 202c:000a  int 10h AX=0002h */

    hc_set_cursor(1u, 1u);                 /* 202c:0014  dx=0101h (row 1, col 1) */
    hc_dos_print(hc_snd_header);           /* 202c:001b  int 21h AH=09h @ 6840 */

    /* 202c:0021  print present devices at col 33, rows from 10 (dx=0a21h). */
    row = HC_MENU_ROW;
    for (i = 0u; i < 4u; i = i + 1u) {
        if (hc_snd_present[i] == 1u) {
            hc_set_cursor(row, HC_MENU_COL);
            hc_dos_print(hc_snd_lines[i]);
            row = row + 1u;                /* 202c:0040  inc dh */
        }
    }

    /* 202c:0048  poll F5..F8 until one is pressed (host: BIOS INT 16h). */
    for (;;) {
        u8 sc = hc_read_scancode();
        if (sc == HC_F5) { sound_device_state = (s16)0x8000; return; }  /* no sound */
        if (sc == HC_F6) { sound_device_state = 0;           return; }  /* PC base  */
        if (sc == HC_F7) { sound_device_state = 1;           return; }  /* AdLib    */
        if (sc == HC_F8) { sound_device_state = 4;           return; }  /* MT-32    */
    }
}

#endif /* BUMPY_PLAYABLE */
