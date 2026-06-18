#ifndef ENTITY_H
#define ENTITY_H

#include "bumpy.h"
#include "sprite_chain.h"   /* sprite_view */

/* Faithful C ports of the entity draw functions from spawn_and_draw_level_entities
   (1000:2a78), draw_p1_sprite (1000:1cb2) / draw_p2_sprite (1000:1cea), and the
   anim-channel draw helpers draw_anim_channels_a (1000:165e) /
   draw_anim_channels_b (1000:17c7).

   All functions drive the VALIDATED pipeline:
     sprite_prepare_frame -> sprite_blit_build_desc -> sprite_blit_planar_vga
   via the shared entity_blit_object helper (static in entity.c).

   --- LAYER A (entity_draw_layer_a) ---
   Port of the layer-A static placement loop in spawn_and_draw_level_entities +
   draw_anim_channels_a (draw side only).  For each of the 48 grid cells
   (6 rows × 8 cols), bum[0x00+cell] gives the cell value cv.  cv==0 or a null
   descriptor (remap==0) → skip.  Otherwise: posA X/Y from dg[0xf4+cell*4] /
   dg[0xf6+cell*4]; yoff/frame from anim_a_desc[cv] (see sourcing note below);
   Y += yoff; frame guard (frame & 0x200) == 0; blit via entity_blit_object.
   Validated on level 1 (27 layer-A cells, cv=1 → frame=64).

   --- LAYER B (entity_draw_layer_b) ---
   Port of the layer-B static placement loop + draw_anim_channels_b (draw side).
   Same structure as layer A but: bum[0x30+cell]; col==7 skipped; posB at
   dg[0x3f4+cell*4] / dg[0x3f6+cell*4]; yoff/frame from anim_b_desc[cv].
   NOTE: anim_b_desc[cv].frame stores the FINAL frame index — the 0xf1 bias is
   baked into anim_tables.json B[cv].frame by dump_anim_tables.py (raw + 0xf1).
   LAYER_B_FRAME_BIAS in entity.c is therefore 0 (no additional bias applied).
   Validated on world 8 level 1 (12 B cells, DOSEMU_LEVEL=8, Task 7 Phase 3).

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
   to a P2-present level (Task 7).

   --- RECONSTRUCTION FIDELITY ---
   * STRUCTURE-faithful: loop bounds (0..5 rows, 0..7 cols), cell index formula
     (row*8+col), BUM offsets (A:0x00, B:0x30, C:0x60), frame bias (C:+0x179),
     hidden sentinels (P1:100, P2:-1), col-7 guard (layer B), and the three
     pipeline stages mirror the engine's inlined / standalone code exactly.
   * Layer-B frame bias: anim_b_desc[cv].frame stores the FINAL frame index
     (dump_anim_tables.py bakes raw+0xf1 into the JSON; entity.c uses bias=0).
     Earlier versions incorrectly applied +0xf1 again, causing crash on worlds
     with high cv values (frames 551/573 exceed the 510-entry table).  Fixed in
     Task 7.  Validated on world 8 (12 B cells, DOSEMU_LEVEL=8).
   * Object layout: the engine reuses the shared p1_sprite struct at
     DGROUP:0x792e (0x18 bytes) for layers A/B/C and P1; P2 uses p2_sprite at
     DGROUP:0x795a.  Here we allocate a 0x40-byte host stack buffer (OBJ_SIZE)
     because sprite_prepare_frame writes a sub-header up to obj+0x2c (count
     byte + up to 3 entries × 6 bytes at obj+0x1a..+0x2b); using only 0x18
     bytes causes a stack smash on the host.  The first 0x18 bytes mirror the
     engine's struct layout exactly.
   * Frametable far ptr: seeded from dg[obj_base+6..9] (engine's sprite obj
     struct captured in FRM3 DGROUP snapshot), exactly as the engine which
     sets the ptr at level init before any draw calls.
   * posA/B/C tables: read from the captured dg snapshot (dg[0xf4+cell*4],
     dg[0xf6+cell*4] for A; dg[0x3f4+cell*4], dg[0x3f6+cell*4] for B;
     dg[0x274+cell*4] for C).  All verified to match anim_tables.json posA/B/C.
   * Layer A/B descriptor sourcing: the engine resolves {yoff, frame} per cv via
     a far-ptr deref (dg[0x3d6a+remap*4] for A, dg[0x40a6+remap*4] for B).  The
     far ptr lands at seg 0x114b (code-area data segment), NOT within the captured
     DGROUP snapshot — cannot be dereferenced at host time.  FALLBACK: decomp-
     derived anim_tables.json A/B maps (embedded as anim_a_desc[]/anim_b_desc[]
     static tables in entity.c).  FRM3 chan_a records confirm anim_tables values
     for cv=1 (yoff=5, frame=64).  posA from dg matches anim_tables.json for all
     48 cells.  This is the faithful reconstructible path.
   * Erase omitted: draw_anim_channels_a/b call restore_bg_view (bg-tile erase)
     before each blit.  Omitted in the composite: bg is built first; there is no
     "old" cell to erase.  This is a valid composite-only deviation.
   * render_player_view OMITTED: draw_anim_channels_a/b call render_player_view
     (FUN_1000_93b8) after blit_sprite for a second draw through the player's
     viewport, producing alternating 8-pixel-wide column patterns at all entity
     positions.  This function is OUT OF SCOPE for Plan 6b: it is NOT called.
     This produces the residual footprint mismatches (~100-600 px per entity)
     visible in the footprint_check output.  The first-draw (entity_blit_object)
     pixels ARE plane-exact — the mismatches are solely from render_player_view's
     second pass.  Evidence: left 8px of each entity match exactly; right columns
     show alternating bg/sprite pattern (render_player_view artifact).
   * Items and level-exit OMITTED: bum[0x91] (level-exit cell) and items
     (bum[0x92] count) drive additional sprite draws not ported in Plan 6b.
     These are the primary sources of out-of-scope residue (~600px exit +
     ~288px items on world 1 level 1).
   * Per-frame animation step: step_anim_channels_a/b are OUT OF SCOPE.  Layers
     A/B are placed STATICALLY at level load by spawn_and_draw_level_entities;
     the anim channels are INACTIVE at the captured settle instant (confirmed on
     world 1, world 3, world 8: chan_a[i].active=0 / chan_b[i].active=0).
     The static placement reproduces the captured frame exactly.
   * P2 AI and physics: P2's AI step, movement state machine, and physics are
     OUT OF SCOPE.  entity_draw_p2 takes the pre-captured draw-time P2 state
     (pixel_x, pixel_y, move_anim, frame_base) from the FRM3 oracle.  Validated
     on world 8 (p2 obj assert x/frame MATCH; composite planes changed by P2).
   * entity_blit_object: shared static helper drives the three pipeline stages for
     all layers (A, B, C, P1, P2) without code duplication.
   * PLAN 6b COMPLETE (Task 7): bg + layer C + P1 + layer A + layer B + P2 all
     exercise their positive draw paths.  Full residue documented above.
*/

/* Draw all nonzero layer-A cells from `bum` into `planes`.
   Parameters:
     planes        — 4-plane VGA buffer (4 * 0x10000 B), plane p at p*0x10000
     bum           — pointer to the level block at tilemap base (offset 0);
                     layer-A grid at bum[0x00+cell].  Pass the FRM3 bum pointer
                     as-is (same base as layer C).
     dg            — 0x10000-byte DGROUP snapshot; posA X at dg[0xf4+cell*4],
                     Y at dg[0xf6+cell*4].
     bank          — sprite bank (bank_inmem.bin), flat huge buffer
     bank_base_lin — runtime linear address of bank[0] (0x4eae0 for bank_inmem.bin)
     view          — sprite viewport (full-screen: left=0,right=40,top=0,
                     bottom=199,height=199,data_off=0,data_seg=0xa000)

   NOTE: layer-B positive path validated on world 8 (Task 7 Phase 3).
*/
void entity_draw_layer_a(u8 __huge *planes, const u8 __far *bum,
                         const u8 __far *dg, u8 __huge *bank,
                         u32 bank_base_lin, const sprite_view *view);

/* Draw all nonzero layer-B cells from `bum` into `planes`.
   Parameters:
     planes        — 4-plane VGA buffer (4 * 0x10000 B)
     bum           — pointer to the level block at tilemap base;
                     layer-B grid at bum[0x30+cell].
     dg            — 0x10000-byte DGROUP snapshot; posB X at dg[0x3f4+cell*4],
                     Y at dg[0x3f6+cell*4].
     bank          — sprite bank
     bank_base_lin — runtime linear address of bank[0]
     view          — sprite viewport

   GUARD: col==7 cells are skipped (engine guard in draw_anim_channels_b).
   FRAME INDEX: anim_b_desc[cv].frame is the FINAL frame (bias pre-baked in
   anim_tables.json; LAYER_B_FRAME_BIAS = 0 in entity.c).

   VALIDATED on world 8 (DOSEMU_LEVEL=8, 12 B cells, Task 7 Phase 3).
   On level 1: 0 B cells, call is a structural no-op.
*/
void entity_draw_layer_b(u8 __huge *planes, const u8 __far *bum,
                         const u8 __far *dg, u8 __huge *bank,
                         u32 bank_base_lin, const sprite_view *view);

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

   NOTE: P2 positive draw path validated on world 8 (DOSEMU_LEVEL=8, Task 7
   Phase 3): p2 obj assert x/frame MATCH, composite planes changed by P2 draw.
*/
void entity_draw_p2(u8 __huge *planes, const u8 __far *dg,
                    u16 pixel_x, u16 pixel_y, u16 move_anim,
                    u16 frame_base, s8 p2_cell,
                    u8 __huge *bank, u32 bank_base_lin,
                    const sprite_view *view);

#endif /* ENTITY_H */
