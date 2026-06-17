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

#endif /* SPRITE_ANIM_H */
