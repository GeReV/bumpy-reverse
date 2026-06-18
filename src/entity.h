#ifndef ENTITY_H
#define ENTITY_H

#include "bumpy.h"
#include "sprite_chain.h"   /* sprite_view */

/* Faithful C ports of the entity draw functions from spawn_and_draw_level_entities
   (1000:2a78) and draw_p1_sprite (1000:1cb2) / draw_p2_sprite (1000:1cea).

   All three functions drive the VALIDATED pipeline:
     sprite_prepare_frame -> sprite_blit_build_desc -> sprite_blit_planar_vga
   via the shared entity_blit_object helper (static in entity.c).

   --- LAYER C (entity_draw_layer_c) ---
   Layer C is purely BUM-data-sourced: for each of the 48 grid cells
   (6 rows × 8 cols), if the cell byte bum[0x60+cell] is nonzero, the engine
   builds a temporary sprite object and blits it.  No runtime animation state
   is involved — every cell is fully determined by the level header.

   --- P1 DRAW (entity_draw_p1) ---
   Port of draw_p1_sprite (1000:1cb2).  Guard: move_anim == 100 → hidden,
   draw nothing.  Otherwise: obj[+0]=pixel_x, obj[+2]=pixel_y,
   obj[+4]=move_anim; blit via the p1_sprite obj (DGROUP:0x792e).
   Validated: P1 object construction asserted vs captured engine obj (x and
   frame exact match; y off by 2 due to oracle-capture timing — see Task 5
   report §3 for explanation).  bg+C+P1 composite checked vs captured frame.

   --- P2 DRAW (entity_draw_p2) ---
   Port of draw_p2_sprite (1000:1cea).  Guard: p2_cell == -1 → P2 absent,
   draw nothing.  Otherwise: obj[+0]=pixel_x, obj[+2]=pixel_y,
   obj[+4]=frame_base+move_anim; blit via the p2_sprite obj (DGROUP:0x795a).

   DEAD/GUARDED PATH (level 1): p2_cell == -1 on level 1 (single-player
   only), so the positive draw path is UNVALIDATED on level 1.
   The guard (p2_cell == -1 → no draw) IS validated: planes are unchanged
   across the call.  Full positive-path composite + object assert is DEFERRED
   to a P2-present level (Task 6 switches levels).

   --- RECONSTRUCTION FIDELITY ---
   * STRUCTURE-faithful: the loop bounds (0..5 rows, 0..7 cols), cell index
     formula (row*8+col), BUM offset (0x60), frame bias (0x179), the hidden
     sentinels (P1: move_anim==100, P2: p2_cell==-1), and the three pipeline
     stages mirror the engine's inlined / standalone code exactly.
   * Object layout: the engine reuses the shared p1_sprite struct at
     DGROUP:0x792e (0x18 bytes) for layer-C and P1; P2 uses p2_sprite at
     DGROUP:0x795a.  Here we allocate a 0x40-byte host stack buffer (OBJ_SIZE)
     because sprite_prepare_frame writes a sub-header up to obj+0x2c (count
     byte + up to 3 entries × 6 bytes at obj+0x1a..+0x2b); using only 0x18
     bytes causes a stack smash on the host.  The first 0x18 bytes mirror the
     engine's struct layout exactly; only the caller-set fields (+0, +2, +4,
     +0xa) differ between sprites; sprite_prepare_frame fills the rest
     (+6/8 frame-table ptr seeds from dg, +0b ctrl, +0c/0e prepared ptr,
     +10 width, +12 height, +14/16 anchors).
   * Frametable far ptr: seeded from dg[obj_base+6..9] (engine's sprite obj
     struct captured in FRM3 DGROUP snapshot), exactly as the engine which
     sets the ptr at level init before any draw calls.
   * posC table (layer-C only): the engine reads from DGROUP:0x274
     (interleaved XY pairs, 4 bytes/cell, X at 0x274+cell*4, Y at
     0x276+cell*4).  The `dg` parameter carries the captured full DGROUP
     snapshot; reads are done from that to preserve exact engine values.
   * entity_blit_object refactor: the prepare→build_desc→blit sequence was
     factored out from entity_draw_layer_c into a shared static helper so
     entity_draw_p1, entity_draw_p2 (and future layer A/B ports) can reuse
     the pipeline without duplication.  Layer-C behavior is UNCHANGED.
*/

/* Draw all nonzero layer-C cells from `bum` into `planes`.
   Parameters:
     planes        — 4-plane VGA buffer (4 * 0x10000 B), plane p at p*0x10000
     bum           — pointer to the level block at tilemap base (offset 0);
                     layer-C grid at bum[0x60+cell].  The FRM3 oracle captures
                     this block directly from deref(tilemap_far_ptr) with no
                     additional offset — pass the FRM3 bum pointer as-is.
     dg            — 0x10000-byte DGROUP snapshot; posC table at dg[0x274..0x393]
     bank          — sprite bank (bank_inmem.bin), flat huge buffer
     bank_base_lin — runtime linear address of bank[0] (0x4eae0 for bank_inmem.bin)
     view          — sprite viewport (full-screen: left=0,right=40,top=0,
                     bottom=199,height=199,data_off=0,data_seg=0xa000)
*/
void entity_draw_layer_c(u8 __huge *planes, const u8 __far *bum,
                         const u8 __far *dg, u8 __huge *bank,
                         u32 bank_base_lin, const sprite_view *view);

/* Draw P1 sprite into `planes` if not hidden.
   Parameters:
     planes        — 4-plane VGA buffer (4 * 0x10000 B)
     dg            — 0x10000-byte DGROUP snapshot (for frametable far ptr)
     pixel_x       — p1_pixel_x (DGROUP:0x9290), sourced from FRM3 p1_glob[0..1]
     pixel_y       — p1_pixel_y (DGROUP:0x9292), sourced from FRM3 p1_glob[2..3]
     move_anim     — p1_move_anim (DGROUP:0x824a), sourced from FRM3 p1_glob[4..5]
                     value 100 = hidden sentinel (player invisible, skip draw)
     bank          — sprite bank (bank_inmem.bin), flat huge buffer
     bank_base_lin — runtime linear address of bank[0]
     view          — sprite viewport
*/
void entity_draw_p1(u8 __huge *planes, const u8 __far *dg,
                    u16 pixel_x, u16 pixel_y, u16 move_anim,
                    u8 __huge *bank, u32 bank_base_lin,
                    const sprite_view *view);

/* Draw P2 (opponent) sprite into `planes` if P2 is present.
   Parameters:
     planes        — 4-plane VGA buffer (4 * 0x10000 B)
     dg            — 0x10000-byte DGROUP snapshot (for frametable far ptr)
     pixel_x       — p2_pixel_x (DGROUP:0x79ba), sourced from FRM3 p2_glob[0..1]
     pixel_y       — p2_pixel_y (DGROUP:0x79bc), sourced from FRM3 p2_glob[2..3]
     move_anim     — p2_move_anim (DGROUP:0x8560), sourced from FRM3 p2_glob[4..5]
     frame_base    — p2_frame_base (dg[0xa0de], u16): loaded from P2_FRAME_TABLE
                     at level init; frame index = frame_base + move_anim
     p2_cell       — p2_cell (dg[0x8571], SIGNED byte): -1 = P2 absent (no draw)
     bank          — sprite bank (bank_inmem.bin), flat huge buffer
     bank_base_lin — runtime linear address of bank[0]
     view          — sprite viewport

   NOTE: P2 positive draw path is UNVALIDATED on level 1 (p2_cell==-1 always).
   Validation deferred to Task 6 (P2-present level).
*/
void entity_draw_p2(u8 __huge *planes, const u8 __far *dg,
                    u16 pixel_x, u16 pixel_y, u16 move_anim,
                    u16 frame_base, s8 p2_cell,
                    u8 __huge *bank, u32 bank_base_lin,
                    const sprite_view *view);

#endif /* ENTITY_H */
