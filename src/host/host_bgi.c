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

#endif /* BUMPY_PLAYABLE */
