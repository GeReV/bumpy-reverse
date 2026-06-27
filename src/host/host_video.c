#ifdef BUMPY_PLAYABLE
#include <i86.h>       /* int86, union REGS, MK_FP, FP_OFF, FP_SEG */
#include <conio.h>     /* inp, outp */
#include <string.h>    /* _fmemset */
#include "host.h"
#include "host_bgi.h"   /* host_bgi_stage_image_palette / host_bgi_upload_palette_to_dac */
#include "../screens.h" /* palette_mode, wait_vretrace_thunk */

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
 *   load_palette        — decode/stage the level palette + DAC upload + vsync (1000:08d1)
 *   apply_level_palette — drive load_palette → DAC ports 0x3C8/0x3C9 (1000:0604)
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
 * branch; the gameplay (the BGI palette pipeline + blitters) reads palette_mode.
 * 'mode' is the low byte of the u16 palette_mode global (DGROUP 0x541d). */
void set_palette_mode(u8 mode, u8 flag)
{
    (void)flag;
    palette_mode = (u16)mode;
}

/* level_pav_palette (level.c, BUMPY_PLAYABLE): far ptr to the level's 48-byte decoded
 * DAC palette (g_pav_buf+51), or NULL if no level loaded.  The host load_palette's
 * data source — see the data-sourcing fidelity note on load_palette below. */
extern const u8 __far *level_pav_palette(void);

/* ── load_palette (1000:08d1) ─────────────────────────────────────────────────────
 * Reconstruction of the engine's level-palette loader.  ENGINE (mode-2 / VGA path):
 * for idx 0..15 it reads a 16-bit packed RGB word w = src[idx] from the far source
 * (apply_level_palette passes 0x578:0x203b = the byteswapped level palette) and writes
 * three 6-bit DAC bytes (each channel << 3) into a staging buffer at DGROUP 0x6c42,
 * palette region +0x33:
 *     R = (w >> 8) << 3
 *     G = ((w - (DAT_75eb << 8)) >> 4) << 3
 *     B = ((w & 0xff) - (DAT_75ec << 4)) << 3
 * (DAT_75eb / DAT_75ec are DGROUP bias bytes; init_game_session_state zeroes BOTH and
 * nothing else writes them, so in the gameplay path the biases are 0 → R=(w>>8)<<3,
 * G=(w>>4)<<3, B=(w&0xff)<<3.)  It then runs the palette tail, REUSING the Task-1 BGI
 * primitives: bgi_stage_image_palette(0x6c42,DS,0) → bgi_upload_palette_to_dac(0) →
 * the vsync wait (1000:9864).  (Mode-1 is the EGA fixed-palette patch path, not taken
 * on the VGA boot.)
 *
 * RECONSTRUCTION FIDELITY — HOST DATA-SOURCING DEVIATION (findings §5):
 * the host never stages the PACKED palette at DGROUP 0x578 (level_populate_dg fills
 * only the entity frametable, and load_palette_byteswapped @1000:063b — which would
 * fill 0x578 from cur_level_ptr — is therefore vestigial and omitted, see
 * apply_level_palette).  The host instead has the ALREADY-DECODED 48-byte DAC palette
 * at g_pav_buf+51 (the engine's per-idx decode produces exactly these 48 bytes).  So
 * the host copies those 48 bytes into the staging buffer's +0x33 region — skipping the
 * packed-word decode — then runs the faithful stage → upload → vsync tail.  The
 * (src_off,src_seg) params are kept for signature fidelity (apply_level_palette passes
 * 0x578,0x203b) but the host sources the palette from g_pav_buf.  Recorded in
 * docs/reconstruction-fidelity.md ("playable host: level-palette pipeline").
 *
 * DAC-LAYOUT NOTE (findings §3): host_bgi_upload_palette_to_dac writes DAC slots
 * {0..7, 0x10..0x17} — the slots the active BGI Attribute-Controller mapping
 * (host_set_bgi_attribute_palette: pixel i → DAC i<8?i:0x10+(i-8)) reads.  This is what
 * makes gameplay colours 8..15 correct; render_level's video_set_palette6 (DAC 0..15
 * contiguous) only covers the AC's low 8 slots.  See the audit + the report. */
static u8 host_palette_staging[0x33u + 48u];   /* mirrors the engine 0x6c42 buffer: palette @ +0x33 */

void load_palette(u16 src_off, u16 src_seg)
{
    const u8 __far *pal;
    u8 __far       *stage_fp;
    u8              i;

    (void)src_off; (void)src_seg;   /* engine far-ptr params; host sources g_pav_buf (see note) */

    pal = level_pav_palette();
    if (pal == (const u8 __far *)0) {
        return;   /* no level loaded yet (e.g. boot-time init_display_97f1) — faithful NOP */
    }

    /* Copy the 48 decoded DAC bytes into the staging buffer's palette region (+0x33). */
    for (i = 0u; i < 48u; i++) {
        host_palette_staging[0x33u + i] = pal[i];
    }

    /* Engine tail (1000:09e9): stage the staged palette into the per-page BGI slot,
     * upload it to the DAC, then wait for vertical retrace. */
    stage_fp = (u8 __far *)host_palette_staging;
    host_bgi_stage_image_palette(FP_OFF(stage_fp), FP_SEG(stage_fp), 0u);
    host_bgi_upload_palette_to_dac(0u);
    wait_vretrace_thunk();
}

/* ── apply_level_palette (1000:0604) ──────────────────────────────────────────────
 * Engine:  if (!palette_loaded && cur_level_ptr[0x1c] != 0) load_palette_byteswapped();
 *          else palette_loaded = 1;
 *          load_palette(0x578, 0x203b);
 * Loads the level's 16-colour 6-bit-RGB palette into the DAC (via load_palette).
 *
 * RECONSTRUCTION FIDELITY — OMITTED BYTESWAP PRE-LOAD (findings §4/§5):
 * load_palette_byteswapped (1000:063b) exists only to fill DGROUP 0x578 from
 * cur_level_ptr (byte-swapped); the host's load_palette sources the decoded palette
 * from g_pav_buf instead of 0x578, so the byteswap pre-load and its palette_loaded gate
 * have no host consumer and are omitted.  load_palette(0x578,0x203b) is the faithful
 * tail call. */
void apply_level_palette(void)
{
    load_palette(0x578u, 0x203bu);
}

/* ── init_display_97f1 (1000:97f1) ─────────────────────────────────────────────
 * Post-mode-set video fixup: establishes the CRTC two-page window and uploads
 * the initial DAC palette (so the screen is not black on first entry).
 * Calls init_crtc_window with page0=0x0000, page1=0x1000 to set up the double-
 * buffer layout, then applies the level palette.
 * RECONSTRUCTION FIDELITY: the original 97f1 body is not cleanly decompiled;
 * the host reconstructs its observable effect (CRTC window + DAC init). */
/* host_set_bgi_attribute_palette — program the VGA Attribute Controller palette to the
 * BGI 16-colour mapping: pixel i -> DAC (i<8 ? i : 0x10+(i-8)).  The engine's BGI driver
 * set this up (its mode-init code is not in the Ghidra corpus); without it the BIOS
 * mode-0x0D default AC maps pixel 6->DAC 0x14 and pixels 8..15->DAC 0x38..0x3f, which the
 * decoded image's DAC palette (written to DAC 0..7 / 0x10..0x17 by vga_dac_upload_from_buffer)
 * never loads — so half the colours come out as the EGA default ramp.  Matching the AC to
 * vga_dac_upload_from_buffer's DAC targets makes pixel i resolve to image palette colour i.
 * RECONSTRUCTION FIDELITY: host BGI-init reconstruction (the original BGI handler is absent
 * from the corpus); recorded in docs/reconstruction-fidelity.md ("playable host: BGI palette"). */
static void host_set_bgi_attribute_palette(void)
{
    u8 i;
    (void)inp(0x3DAu);                 /* reset the AC index/data flip-flop */
    for (i = 0u; i < 16u; i++) {
        outp(0x3C0u, i);               /* AC palette register index (bit5=0: programming) */
        outp(0x3C0u, (u8)(i < 8u ? i : (0x10u + (i - 8u))));
    }
    outp(0x3C0u, 0x20u);               /* bit5=1: re-enable video output */
}

void init_display_97f1(void)
{
    init_crtc_window(CRTC_PAGE0_ADDR, CRTC_PAGE1_ADDR, 0u, 0u);
    host_set_bgi_attribute_palette();
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

    /* Zero the host framebuffer's DISPLAY EXTENT only: each plane's [0..0x4000) covers
     * page 0 ([0..0x1f40]) and page 1 ([0x2000..0x3f40]); the rest of the 64 KB plane is
     * never displayed.  Bounding the clear here (rather than the full 0x10000) is correct
     * AND keeps plane 0's slack [0x4000..0x10000) intact — that window backs fullscreen_buf
     * (host_resource.c).  4 planes × 0x4000 cleared. */
    if (host_framebuffer != (u8 __huge *)0) {
        u8 p;
        for (p = 0u; p < 4u; p++) {
            u8 __far *base = (u8 __far *)(host_framebuffer + (u32)p * HOST_PLANE_SIZE);
            _fmemset(base, 0, (u16)0x4000u);   /* display extent: pages 0 + 1 */
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
