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
u16 bgi_write_mode_flag_a;   /* DGROUP 0x541f */
u16 bgi_write_mode_flag_b;   /* DGROUP 0x5420 */

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
 * via main-segment thunk 1000:7b4a (Ghidra: bgi_set_viewport_thunk, formerly
 * blit_view_step).
 *
 * Engine disassembly (1ab9:0179):
 *   *(u16*)(view + 0x18) = 0x14;         -- clip extent width  = 20 (constant)
 *   *(u16*)(view + 0x1a) = 0x19;         -- clip extent height = 25 (constant)
 *   bgi_write_mode_flag_a = 2;           -- DGROUP 0x541f
 *   bgi_write_mode_flag_b = 1;           -- DGROUP 0x5420
 *   (*(code*)table[palette_mode*2+0x4dda])(); -- VGA slot (mode 2) = 0x0000 -> NULL -> no blit
 *
 * VGA iris degeneration — CRITICAL (see findings §2, docs/faithfulness-gap-audit.md §1):
 *   On VGA (palette_mode==2) the dispatch table 0x4dda[2] is NULL -> NO pixel blit.
 *   The iris loop (play_iris_wipe_transition, screens.c:657) passes a per-step rect
 *   in fields +0x14/+0x16/+0x1e/+0x20, but bgi_init_viewport IGNORES them: it always
 *   writes the CONSTANTS 0x14/0x19 to +0x18/+0x1a.  The compose path reads clip from
 *   +0x0a/+0x0c (different fields), so no geometric shrink occurs on VGA.
 *   The visible VGA iris = the vsync-timed hold (4x wait_vretrace_thunk/step, 10 steps)
 *   + the final blank-palette upload (fun_7b93 -> fun_7bca -> DAC zeroed).
 *   This is a TIMED-HOLD -> BLANK-TO-BLACK, not a shrinking rectangle.
 *   The geometric iris is an EGA/CGA effect (non-null blit handler for modes 0/1);
 *   on VGA (mode 2) it degenerates.
 *
 * RECONSTRUCTION FIDELITY:
 *   This host function mirrors bgi_init_viewport for the VGA path: sets view[+0x18]/
 *   [+0x1a] + the two mode flags exactly as the original, then returns (null dispatch).
 *   The per-step iris rect (+0x14/+0x16/+0x1e/+0x20) is UNCONSUMED on VGA — neither
 *   here nor in the compose path; the visible iris is the timed hold + Task-1/2 palette
 *   pipeline, not a geometric redraw.  Deviation recorded in docs/reconstruction-
 *   fidelity.md ("playable host: BGI overlay primitives") and docs/faithfulness-gap-
 *   audit.md §1.
 *
 * Called from fun_7b4a_view_blit (screens.c) under #ifdef BUMPY_PLAYABLE. */
void host_bgi_set_viewport(u8 __far *view, u16 seg)
{
    (void)seg;
    *(u16 __far *)(view + 0x18) = 0x14u;   /* clip extent width  = 20 (constant) */
    *(u16 __far *)(view + 0x1a) = 0x19u;   /* clip extent height = 25 (constant) */
    bgi_write_mode_flag_a = 2u;             /* DGROUP 0x541f */
    bgi_write_mode_flag_b = 1u;             /* DGROUP 0x5420 */
    /* VGA dispatch: table[palette_mode*2 + 0x4dda] for mode 2 = 0x0000 -> NULL -> no blit */
}

#endif /* BUMPY_PLAYABLE */
