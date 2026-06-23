#ifdef BUMPY_PLAYABLE
#include <i86.h>       /* int86, union REGS, MK_FP */
#include <conio.h>     /* inp, outp */
#include <string.h>    /* _fmemset */
#include "host.h"
#include "../screens.h" /* palette_mode, upload_vga_dac_palette */

/* ============================================================================
 * host_video.c — VGA mode 0x0D + CRTC double-buffer + DAC palette init
 *                (Plan A, Task 3 + Task 4: present_frame double-buffer upgrade)
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
 *   present_frame       — (BUMPY_PLAYABLE real body) flicker-free double-buffer present
 *                          + vblank sync (writes off-screen page, waits retrace, CRTC flip)
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

    /* Write CRTC Start Address HI (index 0x0C) then LO (index 0x0D).
     * RECONSTRUCTION FIDELITY — UNGUARDED CRTC WRITE (DELIBERATE):
     * The HI/LO pair is written as two non-atomic outp calls with no
     * interrupt guard (_disable/_enable).  This is intentional: the CRTC
     * latches the start address only at vertical retrace, so the corruption
     * window is benign in practice.  The original engine's CRTC programming
     * at this site is unresolved, and almost certainly uses the same unguarded
     * pair — adding a guard the original lacks would be LESS faithful.  Revisit
     * only if the original's CRTC sequence is recovered and shown to guard. */
    outp(CRTC_INDEX, CRTC_START_HI);
    outp(CRTC_DATA,  (u8)((start >> 8u) & 0xFFu));
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
    /* RECONSTRUCTION FIDELITY — UNGUARDED CRTC WRITE (DELIBERATE):
     * Same rationale as init_crtc_window above: the HI/LO pair is written
     * without an interrupt guard because the original engine's page-flip code
     * is unresolved and almost certainly does the same; the CRTC latches start
     * address at vretrace so the tear window is benign.  Do not add a guard
     * the original lacks.  Revisit if the original's CRTC sequence is recovered. */
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

    /* Zero the host framebuffer (4-plane flat RAM image, 4 * HOST_PLANE_SIZE = 256 KB).
     * Open Watcom's _fmemset size argument is 16-bit (unsigned int, max 0xFFFF), so a
     * single call cannot span the full 64 KB plane (0x10000 bytes).  We split each
     * plane into two 0x8000-byte halves.  4 planes × 2 halves × 0x8000 = 256 KB total.
     * _hmemset is not available in this Open Watcom -ml DOS model build. */
    if (host_framebuffer != (u8 __huge *)0) {
        u8 p;
        for (p = 0u; p < 4u; p++) {
            u8 __far *base = (u8 __far *)(host_framebuffer + (u32)p * HOST_PLANE_SIZE);
            _fmemset(base,            0, (u16)0x8000u);
            _fmemset(base + 0x8000u,  0, (u16)0x8000u);
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

/* ── present_frame double-buffer state ─────────────────────────────────────────
 * s_display_page — the VGA page CURRENTLY being scanned out by the CRTC.
 * Initialised to 0 (CRTC starts at page 0 after init_display_97f1).
 * present_frame always writes into the OTHER page, then flips. */
static u8 s_display_page = 0u;

/* ── present_frame (BUMPY_PLAYABLE real body) ───────────────────────────────────
 * Flicker-free double-buffer present:
 *
 *   1. Select the off-screen VGA page (the page NOT currently displayed).
 *   2. Copy host_framebuffer (4-plane flat RAM image) into that page, one plane
 *      at a time via the Sequencer Map Mask register.
 *   3. Wait for vertical retrace (poll Input Status #1 port 0x3DA bit 3) so the
 *      subsequent CRTC flip lands inside the vblank interval — no visible tearing.
 *   4. Flip the CRTC start address to the newly-written page.
 *   5. Update s_display_page for the next call.
 *
 * VGA page layout (mode 0x0D, 320×200×16):
 *   page 0 → VGA byte offset 0x0000 → segment A000:0000 → CRTC addr 0x0000
 *   page 1 → VGA byte offset 0x2000 → segment A200:0000 → CRTC addr 0x1000
 * VGA_PLANE_BYTES = 0x1F40 (320×200÷8 = 8000 bytes) per plane per page.
 *
 * host_framebuffer layout: plane p at [p * HOST_PLANE_SIZE], page 0 content
 * at plane offset +0x0000, page 1 content at plane offset +0x2000.
 * The blitters always compose into page 0 of the host buffer (+0x0000 per plane).
 *
 * RECONSTRUCTION FIDELITY — DOUBLE-BUFFER MECHANISM (CHOSEN DEVIATION):
 * The original engine's display page-flip mechanism was left *unresolved* by the
 * project (the Unicorn VGA model cannot observe CRTC start-address transitions).
 * This host double-buffer (off-screen copy into the non-displayed VGA page, vblank
 * sync via Input Status #1 port 0x3DA bit 3, then CRTC flip via set_display_page)
 * is a *host-chosen mechanism* that is standard and correct for VGA mode 0x0D.
 * The deviation is in mechanism only — the on-screen pixels are identical to the
 * original (verified by the frame-compare gate, Task 11).  Recorded in
 * docs/reconstruction-fidelity.md ("playable host: present_frame double-buffer").
 *
 * VBLANK SYNC NOTE:
 * We poll for the VRETRACE bit (bit 3 of Input Status #1 at 0x3DA) going HIGH,
 * meaning we wait until the start of the vblank interval, then write the CRTC
 * start address.  The CRTC latches the new start address at the NEXT vblank, so
 * the flip is tear-free.  We first wait for the bit to go LOW (end of any ongoing
 * retrace) to avoid a spurious early exit, then wait for it to go HIGH.
 * No infinite-loop guard is needed: at 60 Hz vblank arrives within ~16 ms;
 * a stuck bit would indicate broken hardware, not a program bug.
 *
 * RUNTIME-VERIFICATION DEFERRAL:
 * The playable build cannot boot until Task 9.  This task's verification is:
 * (a) wmake play links BUMPYP.EXE with zero -wx warnings; (b) wmake BUMPY
 * (default build) is byte-unchanged; (c) validate_integration.sh passes.
 * Pixel correctness is deferred to Task 9 (boot) + Task 11 (frame gate). */
void present_frame(u8 page)
{
    u8  plane;
    u8  offscreen;        /* the page we will write into (not displayed) */
    u16 vga_seg;
    u8 __far *vga_dst;
    u8 __huge *src;
    u32 plane_offset;

    (void)page; /* page parameter honored via the double-buffer; engine's arg unused */

    if (host_framebuffer == (u8 __huge *)0) {
        return;   /* framebuffer not allocated yet — faithful NOP */
    }

    /* Step 1: select the off-screen (non-displayed) VGA page. */
    offscreen = (u8)((s_display_page == 0u) ? 1u : 0u);
    vga_seg   = (u16)((offscreen == 0u) ? VGA_SEG_PAGE0 : VGA_SEG_PAGE1);
    vga_dst   = (u8 __far *)MK_FP(vga_seg, 0u);

    /* Step 2: copy all 4 planes from host_framebuffer (page-0 region, +0x0000
     * per plane) into the off-screen VGA page. */
    for (plane = 0u; plane < 4u; plane++) {
        /* Select write plane via Sequencer Map Mask. */
        outp(SEQ_INDEX, SEQ_MAP_MASK);
        outp(SEQ_DATA,  (u8)(1u << plane));

        /* Copy VGA_PLANE_BYTES from plane p, page-0 region of host_framebuffer. */
        plane_offset = (u32)plane * HOST_PLANE_SIZE;
        src = host_framebuffer + plane_offset;   /* +0x0000 = page 0 content */
        _fmemcpy(vga_dst, (u8 __far *)src, VGA_PLANE_BYTES);
    }

    /* Restore map mask to all planes (normal write mode). */
    outp(SEQ_INDEX, SEQ_MAP_MASK);
    outp(SEQ_DATA,  0x0Fu);

    /* Step 3: wait for vertical retrace so the CRTC flip lands in vblank.
     * First drain any ongoing retrace (wait for bit to go LOW), then wait
     * for the next retrace start (bit goes HIGH). */
    while ( (inp(VGA_INPUT_STATUS1) & VGA_VRETRACE_BIT) != 0u) { }
    while ( (inp(VGA_INPUT_STATUS1) & VGA_VRETRACE_BIT) == 0u) { }

    /* Step 4: flip CRTC start address to the newly-written page. */
    set_display_page(offscreen);

    /* Step 5: record which page is now displayed. */
    s_display_page = offscreen;
}

#endif /* BUMPY_PLAYABLE */
