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
 * BGI dispatch thunk under #ifdef BUMPY_PLAYABLE; the default BUMPY.EXE build
 * keeps the faithful-signature NOP stubs in screens.c.
 * Deviation recorded in docs/reconstruction-fidelity.md ("playable host: BGI
 * overlay primitives"). */
#ifndef HOST_BGI_H
#define HOST_BGI_H
#ifdef BUMPY_PLAYABLE
#include "bumpy.h"

/* host_bgi_stage_image_palette — functional equivalent of BGI VGA stage handler
 * (1ab9:0620): copies 48 bytes (16-colour 6-bit palette) from [buf_seg:buf_off]
 * +0x33 into the per-page side-store host_bgi_page_palette[page & 1].
 *
 * Called from fun_7b93_present_blank (screens.c) under #ifdef BUMPY_PLAYABLE.
 * RECONSTRUCTION FIDELITY: the engine slots the palette into *0x5311+page*99+
 * 0x33; the host uses a two-entry side-store instead (see host_bgi.c). */
void host_bgi_stage_image_palette(u16 buf_off, u16 buf_seg, u16 page);

/* host_bgi_upload_palette_to_dac — functional equivalent of BGI VGA DAC-upload
 * handler (1ab9:0677): reads host_bgi_page_palette[page & 1] and emits the
 * canonical VGA-DAC write sequence (OUT 0x3c8=0, 8×RGB; OUT 0x3c8=0x10, 8×RGB).
 *
 * Called from fun_7bca_flip (screens.c) under #ifdef BUMPY_PLAYABLE.
 * The (port,value) sequence is the hard contract the DAC gate validates. */
void host_bgi_upload_palette_to_dac(u16 page);

#endif /* BUMPY_PLAYABLE */
#endif /* HOST_BGI_H */
