#ifdef BUMPY_PLAYABLE
#include "host.h"
#include "host_bgi.h"
#include "../game.h"   /* present_frame */

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
 * Ghidra provenance:
 *   1000:7bca  bgi_page_flip_thunk(unsigned char page)  — the thunk (fun_7bca_flip)
 *              calls bgi_page_flip_dispatch() without forwarding 'page'.
 *   1ab9:02b1  bgi_page_flip_dispatch(void)             — the BGI dispatch fn;
 *              calls (*(code*)*(u16*)(palette_mode*2 + 0x5441))() to reach the
 *              per-mode VGA page-flip handler in the BGI overlay.
 * ============================================================================ */

/* ── host_bgi_page_flip ──────────────────────────────────────────────────────
 * Functional equivalent of the BGI VGA page-flip path (1000:7bca → 1ab9:02b1
 * → VGA vector table handler).
 *
 * Routes to present_frame (host_video.c), which:
 *   1. Copies host_framebuffer (4-plane RAM image) to the off-screen VGA page.
 *   2. Waits for vertical retrace (port 0x3DA bit 3).
 *   3. Flips the CRTC Start Address to make the written page visible.
 *
 * The 'page' argument mirrors the thunk's signature but the original thunk does
 * NOT forward it to the dispatch fn; it is passed through here for signature
 * compatibility and forwarded to present_frame, which also ignores it (the
 * double-buffer selects the off-screen page automatically via s_display_page).
 *
 * RECONSTRUCTION FIDELITY: the original BGI VGA handler's exact mechanism
 * (whether it syncs to vblank, which CRTC registers it touches) is unknown —
 * the BGI overlay is absent from the Ghidra corpus.  The host uses the standard
 * EGA/VGA double-buffer pattern (framebuffer copy + vblank sync + CRTC flip)
 * which produces the same observable effect (tear-free page flip). */
void host_bgi_page_flip(u8 page)
{
    present_frame(page);
}

#endif /* BUMPY_PLAYABLE */
