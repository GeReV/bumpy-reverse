#ifdef BUMPY_PLAYABLE
#include <i86.h>       /* MK_FP */
#include <conio.h>     /* inp, outp */
#include "host.h"
#include "host_gfx.h"
#include "../screens.h"   /* palette_mode (DGROUP 0x541d) */

/* ============================================================================
 * host_gfx.c — graphics-overlay primitive host reimplementation (BUMPY_PLAYABLE)
 * ============================================================================
 *
 * RECONSTRUCTION FIDELITY — GRAPHICS-OVERLAY PRIMITIVES (BEHAVIORAL SIDE-STORE)
 * The original Bumpy engine calls graphics primitives through thunks in segment
 * 1000 that dispatch into the graphics overlay loaded at segment 1AB9.  The
 * graphics overlay implements per-mode handlers (CGA/EGA/VGA) selected via
 * vector tables at palette_mode*2+0x5435 (stage) / +0x5441 (upload) / +0x4dda
 * (viewport).  The overlay is Loriciel-custom (not Borland BGI).
 *
 * CORRECTED (2026-07-11): the palette STAGE/UPLOAD handlers (1ab9:0605-0677)
 * WERE carved and DO decompile — see src/gfx_palette.c/.h for the faithful 1:1
 * mirror keyed off the engine's own *0x5311 draw-object descriptor. This file
 * models the SAME handlers again, but behaviorally: instead of wiring a live
 * *0x5311 descriptor, it keeps a per-page side-store (host_gfx_page_palette /
 * host_gfx_page_ac below) that the playable render path actually reads/writes
 * at runtime. EGA (palette_mode==1) and VGA (palette_mode==2) are modeled at
 * the same fidelity — side-store instead of descriptor heap — for both; only
 * the graphics overlay's self-modifying BLIT cores (sprite/bg composite)
 * remain genuinely non-decompiling and are the ones that cannot be
 * reconstructed 1:1 (see host_render.c / sprite_blit.c / bg_render.c).
 *
 * Each function replaces the corresponding graphics-overlay dispatch thunk
 * under #ifdef BUMPY_PLAYABLE while the default BUMPY.EXE build retains the
 * faithful-signature NOP stubs in screens.c. Deviation recorded in
 * docs/reconstruction-fidelity.md ("playable host: graphics overlay
 * primitives").
 *
 * Ghidra provenance — palette pipeline:
 *   1000:7b93  gfx_stage_image_palette_thunk(buf_off, buf_seg, page)
 *              thunk → 1ab9:0606 EGA / 0620 VGA palette-STAGE handler
 *              (palette_mode-dispatched via cmdvec_stage_palette_modes 0x5435).
 *              EGA: DS:SI = buf+0x23, rep movsw (8 words = 16 bytes, AC-index
 *              palette).  VGA: DS:SI = buf+0x33, ES:DI = *0x5311 + page*99 +
 *              0x33, rep movsw (0x18 words = 48 bytes).  Copies the
 *              decoded-image's graphics-overlay palette into the per-page slot.
 *   1000:7bca  gfx_upload_palette_to_dac_thunk(page)
 *              thunk → 1ab9:0662 EGA / 0677 VGA DAC/AC-UPLOAD handler
 *              (palette_mode-dispatched via cmdvec_upload_palette_modes 0x5441).
 *              EGA: INT 10h AX=1002h programs the 16 Attribute Controller
 *              palette registers from the staged +0x23 table.
 *              VGA: reads *0x5311 + page*99 + 0x33; OUT 0x3c8=0, 8×RGB,
 *              OUT 0x3c8=0x10, 8×RGB to port 0x3c9.  Writes DAC from staged slot.
 *
 * RECONSTRUCTION FIDELITY — PALETTE SIDE-STORE DEVIATION
 * The engine stores staged palettes in *0x5311 + page*99 + 0x23 (EGA, 16-byte
 * AC-index table) / +0x33 (VGA, 48-byte 6-bit RGB table) — a slot in the
 * descriptor heap keyed by page. The host keeps two-entry side-stores,
 * host_gfx_page_ac[2][16] (EGA) and host_gfx_page_palette[2][48] (VGA),
 * instead — behaviorally equivalent (stage writes it, upload reads it, same
 * content) but does not model the full *0x5311 descriptor heap layout.
 * Deviation cited for handler addresses 1ab9:0606/0662 (EGA) and 1ab9:0620/
 * 0677 (VGA).
 * ============================================================================ */

/* Per-page staged palette store — 2 pages × 16 colours × 3 bytes (6-bit RGB).
 * Filled by host_gfx_stage_image_palette (mirrors 1ab9:0620 rep-movsw); read by
 * host_gfx_upload_palette_to_dac (mirrors 1ab9:0677 DAC write loop). */
static u8 host_gfx_page_palette[2][48];

/* Per-page staged AC-index palette store (EGA) — 2 pages × 16 bytes. Filled by
 * host_gfx_stage_image_palette (mirrors 1ab9:0606 rep-movsw); read by
 * host_gfx_upload_palette_to_dac (mirrors 1ab9:0662 INT 10h AX=1002h). */
static u8 host_gfx_page_ac[2][16];

/* gfx_write_mode_flag_a / gfx_write_mode_flag_b — DGROUP 0x541f / 0x5420.
 * Engine globals set by gfx_init_viewport (1ab9:0179), restore_bg_view (1ab9:0d77),
 * and render_player_view (1ab9:1028) to select source/dest addressing mode for the
 * subsequent blit handler.  In the VGA playable path only gfx_init_viewport writes
 * them (a=2, b=1); the overlay blit handlers that READ them are not reconstructed
 * (VGA slot = null handler; see host_gfx_set_viewport).
 *
 * Declared here because these globals are only needed under #ifdef BUMPY_PLAYABLE:
 * the default BUMPY.EXE NOP stub for gfx_set_viewport_thunk never reaches them. */
u8 gfx_write_mode_flag_a;   /* DGROUP 0x541f */
u8 gfx_write_mode_flag_b;   /* DGROUP 0x5420 */

/* ── host_gfx_stage_image_palette ───────────────────────────────────────────
 * Functional equivalent of the graphics-overlay palette-stage handler
 * (1ab9:0606 EGA / 0620 VGA), dispatched by palette_mode.
 *
 * EGA: the original handler copies 8 words (16 bytes, AC-index palette) from
 * DS:SI = [buf_seg:buf_off]+0x23 via "rep movsw".  The host copies to
 * host_gfx_page_ac[page & 1] instead of the full descriptor slot — see the
 * fidelity note above.
 *
 * VGA (existing): the original handler copies 0x18 words (48 bytes = 16
 * colours × 3 bytes) from DS:SI = [buf_seg:buf_off]+0x33 to ES:DI =
 * *0x5311 + page*99 + 0x33 via "rep movsw".  The host copies to
 * host_gfx_page_palette[page & 1] instead of the full descriptor slot — see
 * the fidelity note above.
 *
 * Called from gfx_stage_image_palette_thunk (screens.c) under #ifdef BUMPY_PLAYABLE. */
void host_gfx_stage_image_palette(u16 buf_off, u16 buf_seg, u16 page)
{
    if (palette_mode == 1u) {                      /* EGA: 16 bytes from buf+0x23 */
        u8 __far *src = (u8 __far *)MK_FP(buf_seg, buf_off) + 0x23;
        u8 *dst = host_gfx_page_ac[page & 1u];
        u8 i;
        for (i = 0u; i < 16u; i++) {
            dst[i] = src[i];
        }
        return;
    }
    {                                                /* VGA (existing): 48 bytes from buf+0x33 */
        u8 __far *src = (u8 __far *)MK_FP(buf_seg, buf_off) + 0x33;
        u8 *dst = host_gfx_page_palette[page & 1u];
        u8 i;
        for (i = 0u; i < 48u; i++) {
            dst[i] = src[i];
        }
    }
}

/* ── host_gfx_upload_palette_to_dac ─────────────────────────────────────────
 * Functional equivalent of the graphics-overlay palette-upload handler
 * (1ab9:0662 EGA / 0677 VGA), dispatched by palette_mode.
 *
 * EGA: the original handler issues INT 10h AX=1002h (ES:DX -> the staged
 * +0x23 AC table) to program the 16 Attribute Controller palette registers.
 * RECONSTRUCTION FIDELITY: modeled here as the equivalent direct 0x3c0 AC-port
 * sequence (reset the AC index/data flip-flop via a read of 0x3DA, then for
 * each of the 16 regs OUT 0x3C0=index, OUT 0x3C0=value, then OUT 0x3C0=0x20 to
 * re-enable video output) so it runs under the host's real VGA in EGA-register
 * mode. Same per-page side-store deviation as the VGA path below
 * (host_gfx_page_ac vs the *0x5311 descriptor slot). The DAC itself is left
 * alone here (stays whatever the BIOS EGA ramp / boot init programmed it to —
 * see Task 6).
 *
 * VGA (existing): the original handler reads *0x5311 + page*99 + 0x33 (the
 * staged palette slot) and emits the canonical VGA-DAC write sequence: OUT
 * 0x3c8=0x00, 8×RGB to 0x3c9, OUT 0x3c8=0x10, 8×RGB to 0x3c9 (graphics-overlay
 * colour indices 0..7 at DAC 0..7, indices 8..15 at DAC 0x10..0x17).  The host
 * reads from host_gfx_page_palette[page & 1] and emits the identical port
 * sequence.
 *
 * The VGA (port,value) sequence is the hard contract the DAC port-write gate
 * validates; it must match vga_dac_upload_from_buffer (screens.c) byte-for-byte.
 *
 * Called from gfx_upload_palette_to_dac_thunk (screens.c) under #ifdef BUMPY_PLAYABLE. */
void host_gfx_upload_palette_to_dac(u16 page)
{
    if (palette_mode == 1u) {
        const u8 *ac = host_gfx_page_ac[page & 1u];
        u8 i;
        (void)inp(VGA_INPUT_STATUS1);              /* reset AC flip-flop */
        for (i = 0u; i < 16u; i++) {
            outp(ATTR_PORT, i);                    /* AC palette reg index */
            outp(ATTR_PORT, ac[i]);                /* value = image AC index (0..15) */
        }
        outp(ATTR_PORT, 0x20u);                     /* re-enable video */
        return;
    }
    {                                                /* VGA (existing) */
        const u8 *pal = host_gfx_page_palette[page & 1u];
        u8 i;
        outp(DAC_INDEX, 0x00u);            /* DAC write index 0 */
        for (i = 0u; i < 8u; i++) {
            outp(DAC_DATA, pal[0]);        /* R */
            outp(DAC_DATA, pal[1]);        /* G */
            outp(DAC_DATA, pal[2]);        /* B */
            pal += 3;
        }
        outp(DAC_INDEX, 0x10u);            /* DAC write index 0x10 (graphics-overlay colours 8..15) */
        for (i = 0u; i < 8u; i++) {
            outp(DAC_DATA, pal[0]);
            outp(DAC_DATA, pal[1]);
            outp(DAC_DATA, pal[2]);
            pal += 3;
        }
    }
}

/* ── host_gfx_set_viewport ───────────────────────────────────────────────────
 * Functional equivalent of graphics-overlay gfx_init_viewport (1ab9:0179), called
 * via main-segment thunk 1000:7b4a (Ghidra: gfx_set_viewport_thunk).
 *
 * Engine disassembly (1ab9:0179):
 *   *(u16*)(view + 0x18) = 0x14;         -- clip extent width  = 20 (constant)
 *   *(u16*)(view + 0x1a) = 0x19;         -- clip extent height = 25 (constant)
 *   gfx_write_mode_flag_a = 2;           -- DGROUP 0x541f
 *   gfx_write_mode_flag_b = 1;           -- DGROUP 0x5420
 *   CALL [palette_mode*2 + 0x4dda]       -- EGA/VGA slot = 0x0000
 *
 * ── CORRECTION (2026-07-05): the 0x4dda EGA/VGA slot is a RECT FILL, not a no-op ──
 * The prior reconstruction here (and faithfulness-gap-audit.md §1) called the
 * 0x4dda[1]/[2]==0 slot a "null → no pixel blit".  That was WRONG: `CALL word ptr
 * [BX+0x4dda]` with the entry value 0 does NOT no-op — it calls the near address 0,
 * i.e. 1ab9:0000, which is a REAL secondary dispatcher.  Fully disassembled
 * (2026-07-05, all in the non-decompiling overlay seg 1ab9):
 *   1ab9:0000  runs GC/SEQ setup (0x64d) + geometry setup (0x427) from the descriptor,
 *              reads sub-mode `view[+0x1c]`, dispatches table `0x4dcc[view+0x1c]`.
 *   0x4dcc[0] = 1ab9:002b  = a SOLID RECTANGLE FILL across all 4 planes (per-plane
 *              SEQ map-mask + `rep stosw`).  Fill value = the per-plane colour built
 *              from `view[+0x22..+0x25]` (1ab9:05cf); for the iris and the name-entry
 *              cursor those bytes are 0 → BLACK.
 *   Geometry (1ab9:0427, with gfx_write_mode_flag_a==2 → dest-only, no source copy):
 *     dest byte-x = view[+0x14] * 2   (0x5421)
 *     dest row    = view[+0x16] * 8   (0x542d)     stride = 40 (0x5429, flag_b==1)
 *     width bytes = view[+0x1e] * 2   (0x5431)
 *     height rows = view[+0x20] * 8   (0x5433)
 *     dest offset = row*40 + byte-x + current-draw-page  (1ab9:052d)
 *
 * This one primitive is BOTH long-standing gaps:
 *   - the IRIS (play_iris_wipe_transition drives shrinking black rects → the geometric
 *     "closing square"; the recon's prior timed-hold+palette-blank was an approximation),
 *   - the NAME-ENTRY cursor ERASE (draw_name_entry_cursor fills the cursor cell black
 *     BEFORE re-blitting the glyph → no letter trail on cycling).
 * It also clears the code/password screen (show_menu_select composes no bg).
 *
 * RECONSTRUCTION FIDELITY: only sub-mode 0 (the rect fill) with a black fill colour is
 * reconstructed — that is the only path the playable build's callers (iris + name entry)
 * exercise; other `view[+0x1c]` sub-handlers and non-black fills are not reached and are
 * left unimplemented (early return).  The self-modifying per-plane inner loop is replaced
 * by an equivalent all-planes zero fill (Map-Mask=0x0F, write 0).  Recorded in docs/
 * reconstruction-fidelity.md ("playable host: graphics-overlay primitives").
 *
 * Called from gfx_set_viewport_thunk (screens.c) under #ifdef BUMPY_PLAYABLE. */
void host_gfx_set_viewport(u8 __far *view, u16 seg)
{
    u16 dx_t, dy_t, w_t, h_t;
    u16 wb, hr, base, r, c;
    u8 __far *vga;

    (void)seg;
    *(u16 __far *)(view + 0x18) = SCREEN_W_TILES;   /* clip extent width  = 20 (constant) */
    *(u16 __far *)(view + 0x1a) = SCREEN_H_TILES;   /* clip extent height = 25 (constant) */
    gfx_write_mode_flag_a = 2u;             /* DGROUP 0x541f */
    gfx_write_mode_flag_b = 1u;             /* DGROUP 0x5420 */

    /* Secondary dispatch (1ab9:0000 → 0x4dcc[view+0x1c]); only sub-mode 0 = rect fill
       is reached by the recon (iris + name-entry cursor). */
    if ((*(u16 __far *)(view + 0x1c)) != 0u) {
        return;
    }
    /* Fill colour = view[+0x22..+0x25] (per-plane); the recon callers all use black. */
    if ((view[0x22] | view[0x23] | view[0x24] | view[0x25]) != 0u) {
        return;
    }
    dx_t = *(u16 __far *)(view + 0x14);     /* dest x (tiles) */
    dy_t = *(u16 __far *)(view + 0x16);     /* dest y (tiles) */
    w_t  = *(u16 __far *)(view + 0x1e);     /* width  (tiles) */
    h_t  = *(u16 __far *)(view + 0x20);     /* height (tiles) */
    wb   = (u16)(w_t * 2u);                 /* width in VGA bytes  (2 bytes / 16px tile) */
    hr   = (u16)(h_t * 8u);                 /* height in rows      (8 rows / tile)       */
    base = (u16)(host_draw_page_off() + (u16)((u16)(dy_t * 8u) * VGA_ROW_BYTES) + (u16)(dx_t * 2u));

    vga = (u8 __far *)MK_FP(VGA_SEG_PAGE0, 0u);
    outp(GC_INDEX, GC_BIT_MASK); outp(GC_DATA, GC_BIT_MASK_ALL);       /* all bits writable */
    outp(SEQ_INDEX, SEQ_MAP_MASK); outp(SEQ_DATA, SEQ_MAP_ALL_PLANES); /* all 4 planes: one write = black */
    for (r = 0u; r < hr; r++) {
        u16 off = (u16)(base + (u16)(r * VGA_ROW_BYTES));
        for (c = 0u; c < wb; c++) {
            vga[off + c] = 0u;
        }
    }
    host_vga_blit_end();                    /* restore default Map-Mask/Bit-Mask */
}

#endif /* BUMPY_PLAYABLE */
