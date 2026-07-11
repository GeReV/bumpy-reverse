/* host_gfx.h — graphics-overlay primitive host reimplementation (BUMPY_PLAYABLE).
 *
 * RECONSTRUCTION FIDELITY — GRAPHICS-OVERLAY PRIMITIVES (HOST REIMPLEMENTATION)
 * The original Bumpy engine's graphics primitives route through a Loriciel-custom
 * graphics overlay loaded at segment 1AB9.  The overlay implements per-mode
 * handlers (CGA/EGA/VGA) dispatched via a vector table at palette_mode*2+0x5441.
 * That overlay is Loriciel-custom (not Borland BGI) and is absent from the Ghidra decompilation
 * corpus; its internal structure cannot be reconstructed 1:1.  This host module
 * provides *functional equivalents* for VGA (palette_mode==2) only — the mode
 * the playable build runs in.  Each host primitive replaces the corresponding
 * graphics-overlay dispatch thunk under #ifdef BUMPY_PLAYABLE; the default BUMPY.EXE build
 * keeps the faithful-signature NOP stubs in screens.c.
 * Deviation recorded in docs/reconstruction-fidelity.md ("playable host: graphics
 * overlay primitives"). */
#ifndef HOST_GFX_H
#define HOST_GFX_H
#ifdef BUMPY_PLAYABLE
#include "bumpy.h"

/* host_gfx_stage_image_palette — functional equivalent of graphics-overlay VGA stage handler
 * (1ab9:0620): copies 48 bytes (16-colour 6-bit palette) from [buf_seg:buf_off]
 * +0x33 into the per-page side-store host_gfx_page_palette[page & 1].
 *
 * Called from fun_7b93_present_blank (screens.c) under #ifdef BUMPY_PLAYABLE.
 * RECONSTRUCTION FIDELITY: the engine slots the palette into *0x5311+page*99+
 * 0x33; the host uses a two-entry side-store instead (see host_gfx.c). */
void host_gfx_stage_image_palette(u16 buf_off, u16 buf_seg, u16 page);

/* host_gfx_upload_palette_to_dac — functional equivalent of graphics-overlay VGA DAC-upload
 * handler (1ab9:0677): reads host_gfx_page_palette[page & 1] and emits the
 * canonical VGA-DAC write sequence (OUT 0x3c8=0, 8×RGB; OUT 0x3c8=0x10, 8×RGB).
 *
 * Called from fun_7bca_flip (screens.c) under #ifdef BUMPY_PLAYABLE.
 * The (port,value) sequence is the hard contract the DAC gate validates. */
void host_gfx_upload_palette_to_dac(u16 page);

/* gfx_write_mode_flag_a / gfx_write_mode_flag_b — DGROUP 0x541f / 0x5420.
 * Engine globals set by gfx_init_viewport (1ab9:0179) to select addressing mode
 * for the subsequent blit dispatch.  Written here (a=2, b=1) by
 * host_gfx_set_viewport; the VGA blit slot is null so they are never read by a
 * live handler.  Exposed for reference / future overlay reconstruction. */
extern u8 gfx_write_mode_flag_a;   /* DGROUP 0x541f */
extern u8 gfx_write_mode_flag_b;   /* DGROUP 0x5420 */

/* host_gfx_set_viewport — functional equivalent of graphics-overlay gfx_init_viewport
 * (1ab9:0179), called via thunk 1000:7b4a (Ghidra: gfx_set_viewport_thunk).
 *
 * Sets clip-extent constants view[+0x18]=0x14, view[+0x1a]=0x19 and the two mode
 * flags (gfx_write_mode_flag_a=2, gfx_write_mode_flag_b=1), then returns with NO
 * pixel blit (VGA dispatch slot 0x4dda[2]=0x0000 is null).
 *
 * VGA iris: the iris loop passes a per-step rect in +0x14/+0x16/+0x1e/+0x20 but
 * gfx_init_viewport IGNORES it (writes CONSTANTS to +0x18/+0x1a).  The visible
 * VGA iris = the vsync-timed hold (4x wait_vretrace_thunk/step, 10 steps) + the
 * final blank-palette upload (Task-1/2 palette path) → TIMED-HOLD -> BLANK-TO-BLACK,
 * not a geometric shrink (the geometric iris is an EGA/CGA effect; on VGA it
 * degenerates).  Do NOT invent a geometric wipe here.
 *
 * RECONSTRUCTION FIDELITY: host clip-state reconstruction; VGA blit handler null;
 * per-step iris rect unconsumed on VGA; iris visible via timed hold + palette path.
 * Deviation recorded in docs/reconstruction-fidelity.md + faithfulness-gap-audit.md §1.
 *
 * Called from fun_7b4a_view_blit (screens.c) under #ifdef BUMPY_PLAYABLE.
 * `seg` is the DGROUP segment (already encoded in the far ptr `view`; unused here). */
void host_gfx_set_viewport(u8 __far *view, u16 seg);

#endif /* BUMPY_PLAYABLE */
#endif /* HOST_GFX_H */
