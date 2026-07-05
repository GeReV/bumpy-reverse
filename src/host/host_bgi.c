#ifdef BUMPY_PLAYABLE
#include <i86.h>       /* MK_FP */
#include <conio.h>     /* outp */
#include "host.h"
#include "host_bgi.h"

/* ============================================================================
 * host_bgi.c — BGI overlay primitive host reimplementation (BUMPY_PLAYABLE)
 * ============================================================================
 *
 * RECONSTRUCTION FIDELITY — BGI OVERLAY PRIMITIVES (FUNCTIONAL EQUIVALENCE)
 * The original Bumpy engine calls graphics primitives through thunks in segment
 * 1000 that dispatch into the Borland BGI overlay loaded at segment 1AB9.  The
 * BGI overlay implements per-mode handlers (CGA mode 0 / VGA mode 2) selected
 * via a vector table at palette_mode*2+0x5441.  The overlay is third-party
 * (Borland BGI library) and is NOT in the Ghidra decompilation corpus; 1:1
 * reconstruction of its internals is impossible.
 *
 * This module provides functional equivalents for the VGA (palette_mode==2)
 * path — the only path exercised by the playable build.  Each function replaces
 * the corresponding BGI dispatch thunk under #ifdef BUMPY_PLAYABLE while the
 * default BUMPY.EXE build retains the faithful-signature NOP stubs in screens.c.
 * Deviation recorded in docs/reconstruction-fidelity.md ("playable host: BGI
 * overlay primitives").
 *
 * Ghidra provenance — palette pipeline:
 *   1000:7b93  fun_7b93_present_blank(buf_off, buf_seg, page)
 *              thunk → 1ab9:0620  VGA palette-STAGE handler.
 *              Disassembly: DS:SI = buf+0x33, ES:DI = *0x5311 + page*99 + 0x33,
 *              rep movsw (0x18 words = 48 bytes).  Copies the decoded-image's
 *              16-colour BGI palette into the per-page palette slot.
 *   1000:7bca  fun_7bca_flip(page)
 *              thunk → 1ab9:0677  VGA DAC-UPLOAD handler.
 *              Disassembly: reads *0x5311 + page*99 + 0x33; OUT 0x3c8=0, 8×RGB,
 *              OUT 0x3c8=0x10, 8×RGB to port 0x3c9.  Writes DAC from staged slot.
 *
 * RECONSTRUCTION FIDELITY — PALETTE SIDE-STORE DEVIATION
 * The engine stores staged palettes in *0x5311 + page*99 + 0x33 (a slot in the
 * descriptor heap keyed by page).  The host keeps a two-entry side-store
 * host_bgi_page_palette[2][48] instead — behaviorally equivalent (stage writes
 * it, upload reads it, same 48-byte content) but does not model the full
 * *0x5311 descriptor heap layout.  Deviation cited for handler addresses
 * 1ab9:0620 (stage) and 1ab9:0677 (DAC upload).
 * ============================================================================ */

/* Per-page staged palette store — 2 pages × 16 colours × 3 bytes (6-bit RGB).
 * Filled by host_bgi_stage_image_palette (mirrors 1ab9:0620 rep-movsw); read by
 * host_bgi_upload_palette_to_dac (mirrors 1ab9:0677 DAC write loop). */
static u8 host_bgi_page_palette[2][48];

/* bgi_write_mode_flag_a / bgi_write_mode_flag_b — DGROUP 0x541f / 0x5420.
 * Engine globals set by bgi_init_viewport (1ab9:0179), restore_bg_view (1ab9:0d77),
 * and render_player_view (1ab9:1028) to select source/dest addressing mode for the
 * subsequent blit handler.  In the VGA playable path only bgi_init_viewport writes
 * them (a=2, b=1); the overlay blit handlers that READ them are not reconstructed
 * (VGA slot = null handler; see host_bgi_set_viewport).
 *
 * Declared here because these globals are only needed under #ifdef BUMPY_PLAYABLE:
 * the default BUMPY.EXE NOP stub for fun_7b4a never reaches them. */
u8 bgi_write_mode_flag_a;   /* DGROUP 0x541f */
u8 bgi_write_mode_flag_b;   /* DGROUP 0x5420 */

/* ── host_bgi_stage_image_palette ───────────────────────────────────────────
 * Functional equivalent of the BGI VGA palette-stage handler (1ab9:0620).
 *
 * The original handler copies 0x18 words (48 bytes = 16 colours × 3 bytes) from
 * DS:SI = [buf_seg:buf_off]+0x33 to ES:DI = *0x5311 + page*99 + 0x33 via
 * "rep movsw".  The host copies to host_bgi_page_palette[page & 1] instead of
 * the full descriptor slot — see the fidelity note above.
 *
 * Called from fun_7b93_present_blank (screens.c) under #ifdef BUMPY_PLAYABLE. */
void host_bgi_stage_image_palette(u16 buf_off, u16 buf_seg, u16 page)
{
    u8 __far *src = (u8 __far *)MK_FP(buf_seg, buf_off) + 0x33;
    u8 *dst = host_bgi_page_palette[page & 1u];
    u8 i;
    for (i = 0u; i < 48u; i++) {
        dst[i] = src[i];
    }
}

/* ── host_bgi_upload_palette_to_dac ─────────────────────────────────────────
 * Functional equivalent of the BGI VGA DAC-upload handler (1ab9:0677).
 *
 * The original handler reads *0x5311 + page*99 + 0x33 (the staged palette slot)
 * and emits the canonical VGA-DAC write sequence: OUT 0x3c8=0x00, 8×RGB to
 * 0x3c9, OUT 0x3c8=0x10, 8×RGB to 0x3c9 (BGI colour indices 0..7 at DAC 0..7,
 * indices 8..15 at DAC 0x10..0x17).  The host reads from host_bgi_page_palette
 * [page & 1] and emits the identical port sequence.
 *
 * The (port,value) sequence is the hard contract the DAC port-write gate
 * validates; it must match vga_dac_upload_from_buffer (screens.c) byte-for-byte.
 *
 * Called from fun_7bca_flip (screens.c) under #ifdef BUMPY_PLAYABLE. */
void host_bgi_upload_palette_to_dac(u16 page)
{
    const u8 *pal = host_bgi_page_palette[page & 1u];
    u8 i;
    outp(0x3c8, 0x00);                 /* DAC write index 0 */
    for (i = 0u; i < 8u; i++) {
        outp(0x3c9, pal[0]);           /* R */
        outp(0x3c9, pal[1]);           /* G */
        outp(0x3c9, pal[2]);           /* B */
        pal += 3;
    }
    outp(0x3c8, 0x10);                 /* DAC write index 0x10 (BGI colours 8..15) */
    for (i = 0u; i < 8u; i++) {
        outp(0x3c9, pal[0]);
        outp(0x3c9, pal[1]);
        outp(0x3c9, pal[2]);
        pal += 3;
    }
}

/* ── host_bgi_set_viewport ───────────────────────────────────────────────────
 * Functional equivalent of BGI overlay bgi_init_viewport (1ab9:0179), called
 * via main-segment thunk 1000:7b4a (Ghidra: bgi_set_viewport_thunk).
 *
 * Engine disassembly (1ab9:0179):
 *   *(u16*)(view + 0x18) = 0x14;         -- clip extent width  = 20 (constant)
 *   *(u16*)(view + 0x1a) = 0x19;         -- clip extent height = 25 (constant)
 *   bgi_write_mode_flag_a = 2;           -- DGROUP 0x541f
 *   bgi_write_mode_flag_b = 1;           -- DGROUP 0x5420
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
 *   Geometry (1ab9:0427, with bgi_write_mode_flag_a==2 → dest-only, no source copy):
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
 * reconstruction-fidelity.md ("playable host: BGI overlay primitives").
 *
 * Called from fun_7b4a_view_blit (screens.c) under #ifdef BUMPY_PLAYABLE. */
void host_bgi_set_viewport(u8 __far *view, u16 seg)
{
    u16 dx_t, dy_t, w_t, h_t;
    u16 wb, hr, base, r, c;
    u8 __far *vga;

    (void)seg;
    *(u16 __far *)(view + 0x18) = 0x14u;   /* clip extent width  = 20 (constant) */
    *(u16 __far *)(view + 0x1a) = 0x19u;   /* clip extent height = 25 (constant) */
    bgi_write_mode_flag_a = 2u;             /* DGROUP 0x541f */
    bgi_write_mode_flag_b = 1u;             /* DGROUP 0x5420 */

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
    base = (u16)(host_draw_page_off() + (u16)((u16)(dy_t * 8u) * 40u) + (u16)(dx_t * 2u));

    vga = (u8 __far *)MK_FP(VGA_SEG_PAGE0, 0u);
    outp(GC_INDEX, GC_BIT_MASK); outp(GC_DATA, 0xFFu);   /* all bits writable */
    outp(SEQ_INDEX, SEQ_MAP_MASK); outp(SEQ_DATA, 0x0Fu);/* all 4 planes: one write = black */
    for (r = 0u; r < hr; r++) {
        u16 off = (u16)(base + (u16)(r * 40u));
        for (c = 0u; c < wb; c++) {
            vga[off + c] = 0u;
        }
    }
    host_vga_blit_end();                    /* restore default Map-Mask/Bit-Mask */
}

#endif /* BUMPY_PLAYABLE */
