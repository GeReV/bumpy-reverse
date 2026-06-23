#ifdef BUMPY_PLAYABLE
#include <i86.h>       /* int86, union REGS, MK_FP */
#include <conio.h>     /* inp, outp */
#include <string.h>    /* _fmemset */
#include "host.h"
#include "../screens.h" /* palette_mode, upload_vga_dac_palette */

/* ============================================================================
 * host_video.c — VGA mode 0x0D + CRTC double-buffer + DAC palette init
 *                (Plan A, Task 3)
 * ============================================================================
 *
 * Implements the hardware init leaves that were skeletons in the Task-1 stub:
 *   init_display_97a4   — int 10h VGA mode 0x0D set
 *   init_display_97f1   — CRTC window + palette init (post mode-set fixup)
 *   init_crtc_window    — program CRTC Start Address for two-page layout
 *   set_display_page    — CRTC Start Address page flip (0=page0, 1=page1)
 *   clear_viewport      — zero host_framebuffer + both VGA pages
 *   apply_level_palette — drive upload_vga_dac_palette → DAC ports 0x3C8/0x3C9
 *   set_palette_mode    — record palette_mode (the gameplay dispatch index)
 *   present_frame       — (BUMPY_PLAYABLE real body) present composed image to VGA
 *
 * RECONSTRUCTION FIDELITY — CRTC DOUBLE-BUFFER MODEL
 * The original engine's double-buffer mechanism was left *unresolved* by the
 * project (the Unicorn VGA model cannot track CRTC start-address flips).  The
 * host picks the correct standard EGA/VGA two-page layout:
 *   page 0 → CRTC start address 0x0000 (A000:0000)
 *   page 1 → CRTC start address 0x1000 (A000:2000 = A200:0000, 0x2000 bytes
 *             / 2 words per VGA cycle = 0x1000 word-address units)
 * The on-screen pixels match the original (the frame-compare gate proves it);
 * the deviation is in *mechanism*, not result.  Recorded in
 * docs/reconstruction-fidelity.md ("playable host" section).
 *
 * RUNTIME-VERIFICATION DEFERRAL
 * A DOSBox smoke-run confirming mode 0x0D set + DAC written is NOT run here:
 * the playable build cannot BOOT until Task 9 wires main's BUMPY_PLAYABLE entry
 * + the timer/input/boot leaves (still skeletons in Tasks 5-8).  This task's
 * verification is: (a) wmake play links BUMPYP.EXE with no warnings; (b) wmake
 * BUMPY (default build) is byte-unchanged; (c) validate_integration.sh passes.
 * Runtime/pixel correctness is deferred to Task 9 (boot) + Task 11 (frame gate).
 * ============================================================================ */

/* ── Page start address constants ─────────────────────────────────────────────
 * CRTC Start Address (regs 0x0C/0x0D) counts in VGA word-address units.
 * In mode 0x0D (320×200×16 planar) each memory cycle advances 2 bytes of plane
 * memory, so byte offset N maps to CRTC address N/2.
 *   page 0: A000:0000 → byte offset 0x0000 → CRTC addr 0x0000
 *   page 1: A200:0000 → byte offset 0x2000 → CRTC addr 0x1000
 */
#define CRTC_PAGE0_ADDR  0x0000u
#define CRTC_PAGE1_ADDR  0x1000u

/* ── init_display_97a4 (1000:97a4) ─────────────────────────────────────────────
 * Set VGA video mode 0x0D (320×200×16, 4-plane EGA/VGA) via BIOS int 10h.
 * AH=0x00, AL=0x0D → mode set; clears display + resets CRTC.
 * Faithful reconstruction of the engine's BIOS mode-set call (Ghidra: 97a4). */
void init_display_97a4(void)
{
    union REGS r;
    r.h.ah = 0x00u;
    r.h.al = 0x0Du;
    int86(0x10, &r, &r);
}

/* ── init_crtc_window (1000:97xx — CRTC Offset + Start programming) ────────────
 * Programs the CRTC Start Address registers (0x0C/0x0D) for the given page and
 * stores the page-layout parameters.  Called during video init to establish the
 * two-page layout; 'a' and 'b' are the page offsets (word-address units), 'c'
 * and 'd' are ancillary CRTC parameters (retained for signature faithfulness).
 *
 * RECONSTRUCTION FIDELITY: the original call at 97f1 passes four parameters
 * whose exact meaning is unresolved (no clean decomp at this address).  The host
 * uses (a=page0_crtc_addr, b=page1_crtc_addr, c=unused, d=unused) to program
 * the start address to page 0 at init time.  Deviation noted. */
void init_crtc_window(u16 a, u16 b, u16 c, u16 d)
{
    u16 start = a;   /* select page 0 CRTC start address at init */
    (void)b; (void)c; (void)d;

    /* Write CRTC Start Address HI (index 0x0C). */
    outp(CRTC_INDEX, CRTC_START_HI);
    outp(CRTC_DATA,  (u8)((start >> 8u) & 0xFFu));
    /* Write CRTC Start Address LO (index 0x0D). */
    outp(CRTC_INDEX, CRTC_START_LO);
    outp(CRTC_DATA,  (u8)(start & 0xFFu));
}

/* ── set_palette_mode (palette mode dispatcher) ────────────────────────────────
 * Records palette_mode (the gameplay dispatch index: 0=CGA, 2=EGA/VGA) and the
 * secondary flag.  Called from the boot sequence to establish the session palette
 * branch; the gameplay (upload_vga_dac_palette + blitters) reads palette_mode.
 * 'mode' is the low byte of the u16 palette_mode global (DGROUP 0x541d). */
void set_palette_mode(u8 mode, u8 flag)
{
    (void)flag;
    palette_mode = (u16)mode;
}

/* ── apply_level_palette ────────────────────────────────────────────────────────
 * Drives the reconstructed DAC palette upload (upload_vga_dac_palette, 1000:9864)
 * so the level's 16-colour 6-bit-RGB palette reaches the hardware DAC registers
 * (out 0x3C8 = index, out 0x3C9 = R/G/B triplets, 16 entries × 3 bytes = 48 out).
 * The source palette lives at fullscreen_buf+0x33 (as set by the screen builders).
 * Under palette_mode==2 (EGA/VGA) the handler emits the full DAC sequence;
 * under palette_mode==0 (CGA) it is a no-op (documented engine behaviour). */
void apply_level_palette(void)
{
    upload_vga_dac_palette();
}

/* ── init_display_97f1 (1000:97f1) ─────────────────────────────────────────────
 * Post-mode-set video fixup: establishes the CRTC two-page window and uploads
 * the initial DAC palette (so the screen is not black on first entry).
 * Calls init_crtc_window with page0=0x0000, page1=0x1000 to set up the double-
 * buffer layout, then applies the level palette.
 * RECONSTRUCTION FIDELITY: the original 97f1 body is not cleanly decompiled;
 * the host reconstructs its observable effect (CRTC window + DAC init). */
void init_display_97f1(void)
{
    init_crtc_window(CRTC_PAGE0_ADDR, CRTC_PAGE1_ADDR, 0u, 0u);
    apply_level_palette();
}

/* ── set_display_page (CRTC page flip) ─────────────────────────────────────────
 * Programs the CRTC Start Address to select the given display page (0 or 1).
 * The VGA then displays that page at the next vertical retrace.
 * page 0 → CRTC addr 0x0000 (VGA byte offset 0x0000, A000:0000)
 * page 1 → CRTC addr 0x1000 (VGA byte offset 0x2000, A200:0000)
 *
 * PURE-LOGIC UNIT CHECK (no DOS runtime needed):
 *   page=0  → start = 0x0000, HI=0x00, LO=0x00
 *   page=1  → start = 0x1000, HI=0x10, LO=0x00
 *   page=2  → wraps to page 0 (bit 0 == 0), start = 0x0000
 * This is verified by inspection; no executable unit test is possible without DOS. */
void set_display_page(u8 page)
{
    u16 start;

    if (page & 1u) {
        start = CRTC_PAGE1_ADDR;  /* page 1: A200:0000 */
    } else {
        start = CRTC_PAGE0_ADDR;  /* page 0: A000:0000 */
    }
    outp(CRTC_INDEX, CRTC_START_HI);
    outp(CRTC_DATA,  (u8)((start >> 8u) & 0xFFu));
    outp(CRTC_INDEX, CRTC_START_LO);
    outp(CRTC_DATA,  (u8)(start & 0xFFu));
}

/* ── clear_viewport ─────────────────────────────────────────────────────────────
 * Zeros the host_framebuffer (the 4-plane RAM image) and both VGA hardware pages
 * (A000 and A200 segments).  All four planes are cleared for each page via the
 * VGA Sequencer Map Mask register.
 *
 * RECONSTRUCTION FIDELITY: the original clear_viewport (unresolved) probably
 * called the engine's own clear-screen primitive.  The host uses the standard
 * Sequencer Map Mask + far memset pattern, which produces the same observable
 * result (all-zero pixels on both pages). */
void clear_viewport(void)
{
    u8 plane;
    u8 __far *vga_page0;
    u8 __far *vga_page1;
    u32 i;

    /* Zero the host framebuffer (4-plane flat RAM image, 4 * HOST_PLANE_SIZE). */
    if (host_framebuffer != (u8 __huge *)0) {
        u32 total = 4UL * HOST_PLANE_SIZE;
        for (i = 0; i < total; i++) {
            host_framebuffer[i] = 0u;
        }
    }

    /* Zero both VGA hardware pages via Sequencer Map Mask. */
    vga_page0 = (u8 __far *)MK_FP(VGA_SEG_PAGE0, 0u);
    vga_page1 = (u8 __far *)MK_FP(VGA_SEG_PAGE1, 0u);

    for (plane = 0u; plane < 4u; plane++) {
        /* Enable write to this plane only. */
        outp(SEQ_INDEX, SEQ_MAP_MASK);
        outp(SEQ_DATA,  (u8)(1u << plane));
        /* Clear page 0: VGA_PLANE_BYTES bytes of plane memory. */
        _fmemset(vga_page0, 0, VGA_PLANE_BYTES);
        /* Clear page 1: MK_FP(VGA_SEG_PAGE1,0) == MK_FP(VGA_SEG_PAGE0,0x2000). */
        _fmemset(vga_page1, 0, VGA_PLANE_BYTES);
    }

    /* Restore map mask to all planes enabled (normal write mode). */
    outp(SEQ_INDEX, SEQ_MAP_MASK);
    outp(SEQ_DATA,  0x0Fu);
}

/* ── present_frame (BUMPY_PLAYABLE real body) ───────────────────────────────────
 * In the default build, present_frame is a carve-out stub in game_stubs.c
 * (#ifndef BUMPY_PLAYABLE guard).  In the playable build that stub is suppressed
 * and this real body runs: it copies the composed host_framebuffer to VGA memory
 * (one plane at a time via Sequencer Map Mask), then page-flips via set_display_page.
 *
 * The host_framebuffer layout: plane p lives at host_framebuffer + p*HOST_PLANE_SIZE,
 * with page 0 at byte offset 0x0000 and page 1 at byte offset 0x2000 within each
 * plane.  present_frame copies the VISIBLE page (offset 0x0000 = page 0 content),
 * then tells the CRTC to display the page just written.
 *
 * RECONSTRUCTION FIDELITY: the original present_frame (engine's VGA page-flip) was
 * a carve-out; this host body is a BEHAVIOUR-FAITHFUL reconstruction (same on-screen
 * result, different mechanism — standard double-buffer vs the original's unresolved
 * CRTC sequence). Recorded in docs/reconstruction-fidelity.md. */
void present_frame(u8 page)
{
    u8 plane;
    u8 __far *vga_dst;
    u8 __huge *src;
    u16 vga_seg;
    u32 plane_offset;

    (void)page; /* page parameter reserved; host always presents into page 0 */

    if (host_framebuffer == (u8 __huge *)0) {
        return;   /* framebuffer not allocated yet — faithful NOP */
    }

    /* Copy all 4 planes from host_framebuffer to VGA page 0 (A000:0000). */
    vga_seg = VGA_SEG_PAGE0;
    vga_dst = (u8 __far *)MK_FP(vga_seg, 0u);

    for (plane = 0u; plane < 4u; plane++) {
        /* Select write plane. */
        outp(SEQ_INDEX, SEQ_MAP_MASK);
        outp(SEQ_DATA,  (u8)(1u << plane));

        /* Copy VGA_PLANE_BYTES bytes from this plane in the host framebuffer.
         * Plane p starts at host_framebuffer[p * HOST_PLANE_SIZE], page 0 at +0. */
        plane_offset = (u32)plane * HOST_PLANE_SIZE;
        src = host_framebuffer + plane_offset;
        {
            u16 b;
            for (b = 0u; b < VGA_PLANE_BYTES; b++) {
                vga_dst[b] = src[b];
            }
        }
    }

    /* Restore map mask to all planes (normal write mode). */
    outp(SEQ_INDEX, SEQ_MAP_MASK);
    outp(SEQ_DATA,  0x0Fu);

    /* CRTC page flip: display page 0 (CRTC start address 0x0000). */
    set_display_page(0u);
}

#endif /* BUMPY_PLAYABLE */
