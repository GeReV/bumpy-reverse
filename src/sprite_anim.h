#ifndef SPRITE_ANIM_H
#define SPRITE_ANIM_H

#include "bumpy.h"

/* Faithful C port of the per-frame animation SELECT in prepare_sprite_frames
   (1cec:2ded) — the engine routine that, per sprite object, picks the current
   animation frame and copies the frame header into the object so the blit chain
   (sprite_chain.h) can consume it.

   Per object it: looks up frame_table[frame_idx] (a relocated far ptr into the
   sprite bank), stores it as the object's prepared-frame ptr, then copies the
   frame's 12-byte header (height/width/X-anchor/Y-anchor at frame[-2/-4/-6/-8],
   a sub-header count at frame[-0xc] clamped to 3 with `count` 3-word entries, and
   the ctrl byte at frame[-10]).  The ctrl&0x40 packed-pixel EXPANSION branch is
   not ported: every BUMSPJEU frame has ctrl=0x03 (bit 0x40 clear), so it never
   runs.

   Addressing mirrors src/sprite.c: the bank is a flat huge buffer and the frame
   table's far pointers are resolved to bank offsets relative to `bank_base_lin`
   (the runtime linear address of bank[0]).  Frame table is at the object's
   frame_table far ptr (obj+6); frame index at obj+4.  Validated descriptor-/
   field-exact against the engine's post-select objects by tools/anim_ctest.c. */
/* --- RECONSTRUCTION FIDELITY (deviates from the engine) ---
   * Faithful transcription of prepare_sprite_frames' per-object select logic, BUT
     the frame-table far pointers are resolved to bank OFFSETS relative to
     bank_base_lin (mirroring src/sprite.c's bank_ptr) instead of the engine's direct
     far-pointer dereference.  Functionally identical; chosen for host testability.
   * The ctrl&0x40 packed-pixel EXPANSION branch is omitted (dead for these sprites). */
void sprite_prepare_frame(u8 __far *obj, u8 __huge *bank, u32 bank_base_lin);

/* UNVALIDATED reconstruction of the ctrl&0x40 packed-pixel EXPANSION path of
   prepare_sprite_frames (engine 1cec:2ea9).  DEAD for BUMSPJEU (every frame is
   ctrl=0x03), present for completeness only — a near-literal transcription of the
   raw disassembly, with no oracle to validate against.  `frame` points at the
   control-byte stream (header at frame[-2..-0xc]); `scratch` is the decode-scratch
   cursor (header copied to scratch[0..0xb], expanded frame at scratch+0xc); `bitrev`
   is the 256-byte pixel_bitrev_lut; `path` = obj[0x0a] (bit 0x20 selects the path);
   `mode` is the engine flag iRam00010ded.  Returns the advanced scratch cursor. */
u8 __far *sprite_expand_frame(u8 __far *frame, u8 __far *scratch,
                              const u8 __far *bitrev, u8 path, u16 mode);

#endif /* SPRITE_ANIM_H */
