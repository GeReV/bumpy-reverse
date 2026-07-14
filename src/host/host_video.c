#ifdef BUMPY_PLAYABLE
#include <i86.h>       /* int86, union REGS, MK_FP, FP_OFF, FP_SEG */
#include <conio.h>     /* inp, outp */
#include <string.h>    /* _fmemset */
#include "host.h"
#include "host_gfx.h"   /* host_gfx_stage_image_palette / host_gfx_upload_palette_to_dac */
#include "../screens.h" /* palette_mode, wait_vretrace_thunk */

/* ============================================================================
 * host_video.c — VGA mode 0x0D + CRTC double-buffer + DAC palette init
 *                (Plan A, Task 3 + Task 4: present_frame double-buffer upgrade)
 * ============================================================================
 *
 * Implements the hardware init leaves that were skeletons in the Task-1 stub:
 *   sound_device_select_screen_thunk — int 10h VGA mode 0x0D set (see its own note:
 *                          real engine target is the sound-device menu, not video)
 *   gfx_draw_sequence_thunk  — CRTC window + palette init (post mode-set fixup)
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
 * host picks the standard EGA/VGA two-page layout (byte-addressed CRTC start in
 * mode 0x0D — see CRTC_PAGE1_ADDR note):
 *   page 0 → CRTC start address 0x0000 (A000:0000, byte offset 0x0000)
 *   page 1 → CRTC start address 0x2000 (A200:0000, byte offset 0x2000)
 * These start values match the ORIGINAL engine's (runtime-captured); the deviation
 * is in *mechanism* (host off-screen-copy + flip vs the engine's overlay flip), not
 * in the displayed result.  Recorded in
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
 * CRTC Start Address (regs 0x0C/0x0D) is BYTE-addressed in mode 0x0D as the BIOS
 * configures it: the start value EQUALS the page's plane byte offset.
 *   page 0: A000:0000 → byte offset 0x0000 → CRTC start 0x0000
 *   page 1: A200:0000 → byte offset 0x2000 → CRTC start 0x2000
 *
 * RECONSTRUCTION FIDELITY — CORRECTED 2026-06-27 (was 0x1000):
 * The earlier value assumed WORD-addressed start units (byte/2 → 0x1000 for page 1).
 * That was wrong: mode 0x0D's CRTC start is byte-addressed here, so 0x1000 made the
 * CRTC display from byte 0x1000 — half a page early — while page-1 content lives at
 * byte 0x2000.  Symptom (seen in interactive play): the title/menu were shifted by
 * ~half the screen, and because the double-buffer alternates page 0 (start 0x0000,
 * correct in either interpretation) with page 1 (offset), the image JITTERED between
 * an aligned and a half-offset frame.  The correct value is the one the ORIGINAL
 * engine uses, confirmed by runtime capture (original CRTC start = 0x2000 for its
 * page 1) and by the original's page-flip primitive, which XORs CRTC reg 0x0C (start
 * HIGH byte) with 0x20 → toggles bit 13 = ±0x2000 (start 0x0000 ↔ 0x2000).  The prior
 * frame-compare gate missed this because it compared page CONTENT (byte 0x2000)
 * without checking the CRTC-selected DISPLAY window.
 */
#define CRTC_PAGE0_ADDR  0x0000u
#define CRTC_PAGE1_ADDR  0x2000u

/* ── sound_device_select_screen_thunk (1000:97a4) ──────────────────────────────
 * Set VGA video mode 0x0D (320×200×16, 4-plane EGA/VGA) via BIOS int 10h.
 * AH=0x00, AL=0x0D → mode set; clears display + resets CRTC.
 *
 * RECONSTRUCTION FIDELITY — ATTRIBUTION CORRECTED (audit 2026-06-28; NAME
 * CORRECTED 2026-07-14):
 * 1000:97a4 is NOT a BIOS mode-set — it is a near-thunk to sound_device_select_screen
 * (202c:0000), the F5..F8 "NO SOUND / PC BASE / ADLIB / MT32" boot menu that stores
 * the chosen sound_device_state (0x8000/0/1/4).  The 2026-06-28 audit correctly
 * identified this as a thunk to something other than a BIOS mode-set, but
 * misattributed the target as a "detect_video_adapter" adapter probe — it is the
 * sound-device menu (same {0x8000,0,1,4} value set the old note misread as a video
 * adapter/mode code); config_screens.c's sound_device_select_screen is the real 1:1
 * reconstruction, called directly from main.c, not through this thunk slot.
 * Video-adapter/palette_mode selection is a SEPARATE engine screen, gfx_driver_init
 * (1ab9:02ce, sound_device_select_screen's "twin"), also called from main.c — this
 * thunk slot never reaches it.
 * The host folds the platform mode-0x0D set into this boot slot as a NECESSITY
 * (there is no live BIOS/graphics-overlay init to do it); the real sound-device
 * menu is a no-op on this slot (it already ran earlier via main.c).  UPDATED
 * 2026-07-11 (Task 6): palette_mode=2 is no longer forced unconditionally here —
 * the playable's gfx_driver_init() (config_screens.c) now runs earlier in boot and
 * sets a live palette_mode from the player's F2/F3 choice (1=EGA, 2=VGA); this slot
 * only supplies 2 as the DEFAULT when nothing selected it yet, so it never
 * overwrites that choice.  (palette_mode was previously set — to 14! — by the
 * misnomered set_palette_mode/97c5, which is really the graphics-overlay
 * set-text-colour op; see set_text_color below.)  NOTE: this INT 10h mode set
 * resets the CRTC start address to 0 — the boot page parity is programmed
 * AFTER it, by gfx_draw_sequence_thunk (host_crtc_set_start). */
void sound_device_select_screen_thunk(void)
{
    union REGS r;
    r.h.ah = 0x00u;
    r.h.al = 0x0Du;
    int86(0x10, &r, &r);
    /* RECONSTRUCTION FIDELITY — REVERSED 2026-07-11 (was: unconditional palette_mode=2):
     * this previously forced VGA outright, which clobbered the F2/EGA selection
     * gfx_driver_init() (config_screens.c) makes earlier in boot — making the EGA
     * path unreachable regardless of the player's graphics-select choice.  Now the
     * live selection survives; VGA is only the default when nothing selected it
     * (palette_mode left at its pre-boot sentinel).  VGA remains the default +
     * validated path. */
    if (palette_mode != 1u && palette_mode != 2u) {
        palette_mode = 2u;   /* default VGA */
    }
}

/* ── init_crtc_window (1000:9821 → overlay 1ab9:1422 — CLIP-WINDOW STORE) ──────
 * GROUNDED (2026-07-03, runtime-relocated overlay disasm): 1000:9821 thunks to
 * 1ab9:1422, which merely stores its four args into the graphics-overlay clip-window record
 * (DGROUP 0x6936/0x6938/0x693a/0x693c = x0,y0,x1,y1).  It does NOT touch the
 * CRTC.  A prior host body programmed the CRTC Start Address here — that was an
 * invention, and FATAL for the boot page parity: init_game_session_state calls
 * this with (0, 0, 0x13f, 199) AFTER gfx_draw_sequence_thunk, so the invented
 * `start = arg b = 0` write clobbered the 0x2000 boot-parity start and inverted
 * `displayed == page[table[0]]` for the whole session (the §8.1 menu-arrow
 * hidden-page bug).  Host: the clip window is not modelled (the reconstructed
 * blitters clip against the fixed 0x14×0x19 screen themselves) → faithful
 * store-only leaf, folded to a NOP.  RECONSTRUCTION FIDELITY note recorded. */
void init_crtc_window(u16 a, u16 b, u16 c, u16 d)
{
    (void)a; (void)b; (void)c; (void)d;   /* engine: DGROUP 0x6936..0x693c := a,b,c,d */
}

/* ── host_crtc_set_start — program the CRTC Start Address (boot page parity) ───
 * BOOT PAGE PARITY (2026-07-02, relocated here 2026-07-03): display starts on
 * PAGE 1 (a200, CRTC 0x2000) — the invariant is `displayed page ==
 * page[sprite_table[0]]` (the UI slot the set_sprite_table_ptr(0)…(1) brackets
 * draw to, and the mode-11 sync's SOURCE).  Booting the display on page 0
 * inverts the invariant: the menu cursor draws on the sync's DESTINATION page
 * and every UI draw lands on the hidden page.  Each present flips the CRTC
 * start and the page table together, so the parity set here is preserved for
 * the whole session.  (The original's boot CRTC value was never directly
 * captured — its graphics-overlay init populates the page table AND the CRTC start
 * out of the corpus; 0x2000 is functionally forced by the engine's own
 * menu/gameplay coherence.  See docs/reconstruction-fidelity.md.)
 *
 * RECONSTRUCTION FIDELITY — UNGUARDED CRTC WRITE (DELIBERATE): the HI/LO pair
 * is two non-atomic outp calls with no interrupt guard; the CRTC latches the
 * start address only at vertical retrace, so the window is benign. */
static void host_crtc_set_start(u16 start)
{
    outp(CRTC_INDEX, CRTC_START_HI);
    outp(CRTC_DATA,  (u8)((start >> 8u) & 0xFFu));
    outp(CRTC_INDEX, CRTC_START_LO);
    outp(CRTC_DATA,  (u8)(start & 0xFFu));
}

/* ── set_text_color (1000:97c5 → overlay 1ab9:1311 — GFX SET TEXT COLOUR) ──────
 * GROUNDED (2026-07-03, unpacked-EXE + runtime-relocated overlay disasm): the
 * 1000:97c5 thunk loads AX=fg, DX=bg from its two stack args and far-calls
 * 1ab9:1311, which dispatches [DGROUP 0x6946 + palette_mode*2] → the pm-2
 * handler 1ab9:14ef: expand fg into the 4-plane mask block at DGROUP 0x68a6
 * (plane p byte = 0xff iff (fg>>p)&1) and bg into 0x68ae likewise.  The glyph
 * blitter (1ab9:1607) paints every glyph cell OPAQUELY from these expansions.
 * Its single engine caller is init_game_session_state (1000:02f5):
 * set_text_color(0x0e, 0x01) — fg = colour 14 (medium grey in the level
 * palettes), bg = colour 1 — the session-wide text colours (pause/world-map
 * score, GAME OVER, highscores).
 *
 * MISNOMER CORRECTED: this function was previously named `set_palette_mode`
 * and its host body wrote `palette_mode = mode` (= 14!) — 97c5 never touches
 * palette_mode (DGROUP 0x541d; runtime capture shows 2, written by the graphics-overlay
 * init — see sound_device_select_screen_thunk).  That mismodel is what made the host
 * glyph blitter assume white (15) — near-BLACK in the world-1 level palette —
 * for the pause-overlay score (§8.2).  Host: store the colours for the
 * host_text_* glyph path (host_render.c). */
void set_text_color(u8 fg, u8 bg)
{
    host_text_set_color(fg, bg);
}

/* level_packed_palette (level.c, BUMPY_PLAYABLE): far ptr to the level's PACKED
 * 16-colour palette (cur_level_ptr[0..0x1f] = g_dec_buf+2: 16 big-endian 12-bit-RGB
 * words), or NULL if no level loaded.  The faithful host load_palette byte-swaps and
 * decodes it into the DAC — see load_palette below. */
extern const u8 __far *level_packed_palette(void);

/* ── load_palette (1000:08d1) [+ inlined load_palette_byteswapped 1000:063b] ───────
 * Reconstruction of the engine's level-palette loader.  ENGINE (mode-2 / VGA path):
 * apply_level_palette first calls load_palette_byteswapped (1000:063b), which copies
 * 16 words from cur_level_ptr[0..0x1f] into DGROUP 0x578, byte-swapping each
 * (0x578[i] = bswap(cur_level_ptr[i])).  load_palette then reads w = *(0x578 + idx*2)
 * and writes three DAC bytes (each channel << 3) into the staging buffer at DGROUP
 * 0x6c42, palette region +0x33:
 *     R = (w >> 8) << 3
 *     G = ((w - (DAT_75eb << 8)) >> 4) << 3
 *     B = ((w & 0xff) - (DAT_75ec << 4)) << 3
 * (DAT_75eb / DAT_75ec are DGROUP bias bytes; init_game_session_state zeroes BOTH and
 * nothing else writes them, so in the gameplay path the biases are 0 → R=(w>>8)<<3,
 * G=(w>>4)<<3, B=(w&0xff)<<3.)  The VGA DAC latches only the low 6 bits on upload
 * (host_gfx_upload_palette_to_dac → port 0x3c9), so e.g. packed word 0x0750 →
 * (R,G,B bytes) (0x38,0xa8,0x80) → DAC (56,40,0).  It then runs the palette tail,
 * REUSING the Task-1 graphics-overlay primitives: gfx_stage_image_palette(0x6c42,DS,0) →
 * gfx_upload_palette_to_dac(0) → the vsync wait (1000:9864).  Mode-1 is the EGA
 * fixed-palette path (copies the fixed AC table at DGROUP 0x70e into +0x23 instead of the
 * RGB decode; see the palette_mode==1 branch below) — reconstructed 2026-07-11.
 *
 * RECONSTRUCTION FIDELITY — HOST DATA SOURCE (RE'd 2026-06-27):
 * the host sources cur_level_ptr from g_dec_buf (the decoded .DEC) via
 * level_packed_palette() — cur_level_ptr = g_dec_buf + 2 + level_index*0x32c — rather
 * than the engine's own DGROUP level archive, and inlines load_palette_byteswapped's
 * byte-swap here (the engine's 0x578 staging buffer is not modelled).  This is the
 * SAME source the engine reads; verified offline that the byte-swap+decode of
 * g_dec_buf+2 for D{1,2,3,9}.DEC reproduces local/build/render/bum/world{n}.pal.json
 * byte-for-byte.  Earlier the host wrongly sourced g_pav_buf+51 (PAV background raster
 * — NOT a palette → all-black DAC); corrected here.  Recorded in
 * docs/reconstruction-fidelity.md ("playable host: level-palette pipeline").
 *
 * DAC-LAYOUT NOTE (findings §3): host_gfx_upload_palette_to_dac writes DAC slots
 * {0..7, 0x10..0x17} — the slots the active overlay Attribute-Controller mapping
 * (host_set_gfx_attribute_palette: pixel i → DAC i<8?i:0x10+(i-8)) reads.  This is what
 * makes gameplay colours 8..15 correct; render_level's video_set_palette6 (DAC 0..15
 * contiguous) only covers the AC's low 8 slots.  See the audit + the report. */
static u8 host_palette_staging[0x33u + 48u];   /* mirrors the engine 0x6c42 buffer: EGA AC @ +0x23, VGA RGB @ +0x33 */

/* ingame_ega_ac_70e — the FIXED 16-byte in-game EGA Attribute-Controller palette load_palette
 * (1000:08d1) copies over the staging buffer at +0x23 when palette_mode==1.  Unlike the
 * per-world overworld tables (level_palette_ptr_table, screens.c), this ONE table is used for
 * ALL in-level gameplay frames in every world — in EGA the playfield uses a single fixed
 * 16-colour assignment onto the BIOS mode-0Dh EGA DAC ramp.
 * RECONSTRUCTION FIDELITY: the binary's DGROUP 0x70e static data (extracted by
 * tools/extract/ega_palette_patch.py — NOT invented). */
static const u8 ingame_ega_ac_70e[16] = {
    0x00u,0x01u,0x09u,0x0eu,0x0au,0x05u,0x04u,0x06u,0x0cu,0x02u,0x0au,0x09u,0x0bu,0x05u,0x07u,0x00u
};

void load_palette(u16 src_off, u16 src_seg)
{
    const u8 __far *cur;      /* cur_level_ptr: 16 packed big-endian 12-bit-RGB words */
    u8 __far       *stage_fp;
    u8              i;
    u16             le;
    u16             w;

    (void)src_off; (void)src_seg;   /* engine far-ptr params; host sources cur_level_ptr (see note) */

    cur = level_packed_palette();
    if (cur == (const u8 __far *)0) {
        return;   /* no level loaded yet (e.g. boot-time gfx_draw_sequence_thunk) — faithful NOP */
    }

    if (palette_mode == 1u) {
        /* EGA (1000:08d1, palette_mode==1 branch): copy the FIXED in-game 16-byte AC-index
         * palette (DGROUP 0x70e) into the staging buffer's AC region (+0x23).  The packed
         * level palette (cur) is NOT used in EGA — the DAC stays the BIOS mode-0Dh EGA ramp
         * and only the Attribute-Controller indices change.  The host stage/upload
         * (host_gfx_*) read +0x23 and program the 16 AC regs when palette_mode==1. */
        for (i = 0u; i < 16u; i++) {
            host_palette_staging[0x23u + (u16)i] = ingame_ega_ac_70e[i];
        }
    } else {
        /* VGA/default (1000:08d1, else branch): load_palette_byteswapped + load_palette
         * decode, idx 0..15.  Read each packed word little-endian from cur_level_ptr,
         * byte-swap (→ engine 0x578 word), then split each nibble << 3 into the staging
         * palette region (+0x33).  Full bytes are stored; the DAC masks to 6 bits on upload
         * (biases DAT_75eb/75ec = 0). */
        for (i = 0u; i < 16u; i++) {
            le = (u16)cur[(u16)i * 2u] | ((u16)cur[(u16)i * 2u + 1u] << 8);
            w  = (u16)((le << 8) | (le >> 8));
            host_palette_staging[0x33u + (u16)i * 3u + 0u] = (u8)((u16)(w >> 8) << 3);
            host_palette_staging[0x33u + (u16)i * 3u + 1u] = (u8)((u16)(w >> 4) << 3);
            host_palette_staging[0x33u + (u16)i * 3u + 2u] = (u8)((u16)(w & 0xffu) << 3);
        }
    }

    /* Engine tail (1000:09e9): stage the staged palette into the per-page graphics-overlay slot
     * (EGA: +0x23 AC / VGA: +0x33 RGB, per palette_mode), upload it (EGA: program 16 AC regs /
     * VGA: DAC 3c8/3c9), then wait for vertical retrace. */
    stage_fp = (u8 __far *)host_palette_staging;
    host_gfx_stage_image_palette(FP_OFF(stage_fp), FP_SEG(stage_fp), 0u);
    host_gfx_upload_palette_to_dac(0u);
    wait_vretrace_thunk();
}

/* ── apply_level_palette (1000:0604) ──────────────────────────────────────────────
 * Engine:  if (!palette_loaded && cur_level_ptr[0x1c] != 0) load_palette_byteswapped();
 *          else palette_loaded = 1;
 *          load_palette(0x578, 0x203b);
 * Loads the level's 16-colour 6-bit-RGB palette into the DAC (via load_palette).
 *
 * RECONSTRUCTION FIDELITY: the host's load_palette inlines load_palette_byteswapped's
 * byte-swap and sources cur_level_ptr from g_dec_buf (see load_palette note), so the
 * separate pre-load + its palette_loaded gate are folded into the single call below.
 * load_palette(0x578,0x203b) keeps the engine signature. */
void apply_level_palette(void)
{
    load_palette(0x578u, ENGINE_STATIC_DGROUP_SEG);
}

/* ── gfx_draw_sequence_thunk (1000:97f1) ─────────────────────────────────────────────
 * Post-mode-set video fixup: establishes the CRTC two-page window and uploads
 * the initial DAC palette (so the screen is not black on first entry).
 * Calls init_crtc_window with page0=0x0000, page1=0x2000 to set up the double-
 * buffer layout, then applies the level palette.
 * RECONSTRUCTION FIDELITY: the original 97f1 body is not cleanly decompiled;
 * the host reconstructs its observable effect (CRTC window + DAC init). */
/* host_set_gfx_attribute_palette — program the VGA Attribute Controller palette to the
 * graphics-overlay 16-colour mapping: pixel i -> DAC (i<8 ? i : 0x10+(i-8)).  The engine's graphics overlay
 * set this up (its mode-init code is not in the Ghidra corpus); without it the BIOS
 * mode-0x0D default AC maps pixel 6->DAC 0x14 and pixels 8..15->DAC 0x38..0x3f, which the
 * decoded image's DAC palette (written to DAC 0..7 / 0x10..0x17 by vga_dac_upload_from_buffer)
 * never loads — so half the colours come out as the EGA default ramp.  Matching the AC to
 * vga_dac_upload_from_buffer's DAC targets makes pixel i resolve to image palette colour i.
 * RECONSTRUCTION FIDELITY: host graphics-overlay-init reconstruction (the original graphics-overlay handler is absent
 * from the corpus); recorded in docs/reconstruction-fidelity.md ("playable host: graphics-overlay palette"). */
static void host_set_gfx_attribute_palette(void)
{
    u8 i;
    (void)inp(VGA_INPUT_STATUS1);      /* reset the AC index/data flip-flop */
    for (i = 0u; i < 16u; i++) {
        outp(ATTR_PORT, i);            /* AC palette register index (bit5=0: programming) */
        outp(ATTR_PORT, (u8)(i < 8u ? i : (0x10u + (i - 8u))));
    }
    outp(ATTR_PORT, 0x20u);            /* bit5=1: re-enable video output */
}

void gfx_draw_sequence_thunk(void)
{
    /* Engine 1000:97f1 → overlay 1ab9:137b gfx_draw_sequence (runtime-relocated
     * disasm, 2026-07-03): text pos = (10,10) [op 1441]; clip window
     * (0,0,0x13f,0xc7) [1422]; active page = 0 [1409]; text line height = 8
     * [1458: DGROUP 0x693e]; TEXT COLOUR fg=0x0f bg=0 [1311 → pm-2 expansion
     * 14ef: fg planes → DGROUP 0x68a6, bg → 0x68ae].  Host models the text pos +
     * colour ops (the clip window / active page / line height have no host
     * consumers — see init_crtc_window / set_display_page notes). */
    host_text_set_pos(10u, 10u);
    set_text_color(0x0fu, 0x00u);
    /* Host boot additions (RECONSTRUCTION FIDELITY — see each helper's note):
     * CRTC boot page parity + the overlay attribute-controller mapping + DAC init,
     * the observable effects of the graphics-overlay init that is absent from the
     * corpus. */
    host_crtc_set_start(CRTC_PAGE1_ADDR);
    /* RECONSTRUCTION FIDELITY (2026-07-11): the fixed VGA pixel->DAC Attribute
     * Controller map only applies under VGA (palette_mode==2).  In EGA
     * (palette_mode==1) the AC is programmed per-image by the Task-5 host_gfx
     * upload path (host_gfx_upload_palette_to_dac); running the fixed VGA map
     * here would clobber that per-image AC state. */
    if (palette_mode != 1u) {
        host_set_gfx_attribute_palette();
    }
    apply_level_palette();
}

/* ── set_display_page — engine set_active_display_page (1000:9814) ──────────────
 * This is graphics-overlay setactivepage, NOT setvisualpage.  GROUNDED: 1000:9814 → 1ab9:1409
 * merely clamps the index (≤1) and stores it in DGROUP[0x6940]; it does NOT touch
 * the CRTC.  And [0x6940] is the graphics-overlay's active-page state, which is dead for
 * the gameplay draw path — the gameplay draw page is cur_sprite_data, selected
 * separately (set_sprite_table_ptr).  Per the runtime oracle the engine never
 * reprograms the CRTC start address during gameplay (single page a000); the display
 * is fixed at page0 by init_crtc_window.  So this is a faithful index-store with NO
 * CRTC write — the prior host body programmed the CRTC start address (a page flip),
 * which is unfaithful (1ab9:1409 does no such thing) and would move the display off
 * the drawn a000 page now that present_frame no longer overrides it.
 * RECONSTRUCTION FIDELITY: recorded in docs/reconstruction-fidelity.md. */
u8 host_gfx_active_page = 0u;   /* mirrors DGROUP[0x6940] — inert for gameplay draw */

void set_display_page(u8 page)
{
    host_gfx_active_page = (u8)(page & 1u);   /* setactivepage: store index, no CRTC */
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
            _fmemset(base, 0, (u16)VGA_DISPLAY_EXTENT);   /* display extent: pages 0 + 1 */
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

/* ── present_frame (BUMPY_PLAYABLE real body) — the engine's REAL page flip ─────
 * present_frame is the engine's frame-present hook (1000:7bdd → gfx_present_dispatch
 * 1ab9:0351; pm=2 handler 1ab9:0379 → 1ab9:06c1).  The 06c1 primitive, recovered
 * from the runtime-relocated overlay bytes (2026-07-02 investigation):
 *
 *   cli ; dx=3d4 ; al=0x0c ; out ; inc dx ; in al,dx
 *   xor al,0x20 ; out dx,al          ; CRTC start high 0x00 <-> 0x20 (page flip)
 *   sti ; shl bx,2
 *   mov ax,[si+2] ; xchg [bx+si+2],ax ; mov [si+2],ax   ; swap sprite_table[0]<->[1]
 *
 * Each present = CRTC start flip (display page 0x0000 <-> 0x2000) + a swap of the
 * two sprite_table_base entries, retargeting every subsequent draw through the
 * cur-sprite pointer.  A PRIOR revision made this a NOP based on crtc_page.md — a
 * MISREAD: the single observed CRTC value 0xDF00 is 0xFF ^ 0x20 (the Unicorn VGA
 * model returns 0xFF on the CRTC read), i.e. the flip primitive fired on EVERY
 * present; and "w0=1 always sources a000" ignored the per-present table swap.  The
 * NOP broke the engine invariant "a present separates draw from save-under":
 * level_intro_screen / level entry draw Bumpy BEFORE the first render_p1_view and
 * rely on the flip+swap so the first save reads the clean opposite page — with the
 * NOP that save captured the drawn marker, permanently baking a Bumpy ghost at the
 * overworld start node and poisoning the walk/entry save-unders (trails).
 *
 * Host: the engine's exact port writes against DOSBox's real VGA +
 * host_page_table_swap() (host_render.c).  Both pages hold the composed screen
 * (init_fullscreen_view_desc's mode-11 sync), so alternating draws stay seamless. */
void present_frame(u8 page)
{
    u8 v;
    (void)page;                    /* engine's constant descriptor-index arg */
    outp(CRTC_INDEX, CRTC_START_HI);       /* CRTC index: start address high  */
    v = (u8)inp(CRTC_DATA);
    outp(CRTC_DATA, (u8)(v ^ 0x20u));      /* display page 0x0000 <-> 0x2000  */
    host_page_table_swap();        /* 1ab9:06c1 tail: table[0] <-> table[1]  */
}

#endif /* BUMPY_PLAYABLE */
