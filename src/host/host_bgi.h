/* host_bgi.h — BGI primitive host reimplementation (BUMPY_PLAYABLE).
 *
 * RECONSTRUCTION FIDELITY — BGI OVERLAY PRIMITIVES (HOST REIMPLEMENTATION)
 * The original Bumpy engine's graphics primitives route through a Borland BGI
 * driver overlay loaded at segment 1AB9.  The overlay implements per-mode
 * handlers (CGA/EGA/VGA) dispatched via a vector table at palette_mode*2+0x5441.
 * That overlay is third-party (BGI) and is absent from the Ghidra decompilation
 * corpus; its internal structure cannot be reconstructed 1:1.  This host module
 * provides *functional equivalents* for VGA (palette_mode==2) only — the mode
 * the playable build runs in.  Each host primitive replaces the corresponding
 * BGI dispatch thunk (fun_7bca_flip etc.) under #ifdef BUMPY_PLAYABLE; the
 * default BUMPY.EXE build keeps the faithful-signature NOP stubs in screens.c.
 * Deviation recorded in docs/reconstruction-fidelity.md ("playable host: BGI
 * overlay primitives"). */
#ifndef HOST_BGI_H
#define HOST_BGI_H
#ifdef BUMPY_PLAYABLE
#include "bumpy.h"

/* host_bgi_page_flip — functional equivalent of the BGI page-flip dispatch.
 *
 * In the original engine, bgi_page_flip_thunk (1000:7bca) calls
 * bgi_page_flip_dispatch (1ab9:02b1), which dispatches through the BGI vector
 * table to the per-mode VGA page-flip handler.  The 'page' argument passed to
 * the thunk is NOT forwarded to the dispatch fn (the thunk discards it); the
 * original BGI handler determines the target page internally.  The engine always
 * calls fun_7bca_flip(0).
 *
 * The host routes this to present_frame (host_video.c), which copies the
 * host_framebuffer RAM image to the off-screen VGA page, waits for vblank, and
 * performs the CRTC start-address flip — producing a tear-free double-buffer
 * present equivalent to the original BGI page-flip effect. */
void host_bgi_page_flip(u8 page);

#endif /* BUMPY_PLAYABLE */
#endif /* HOST_BGI_H */
