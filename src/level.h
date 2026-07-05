#ifndef LEVEL_H_INCLUDED
#define LEVEL_H_INCLUDED

/*
 * level.h — start_level() and level rendering declarations.
 *
 * Port of start_level (1000:2d14): loads D{n}.PAV / D{n}.DEC / D{n}.BUM via
 * the op4+op12 VEC pipeline, then renders the background and entity layers
 * into the VGA planes and presents to screen.
 *
 * See level.c for full RECONSTRUCTION FIDELITY notes.
 *
 * Phase-1 Task 4 STATUS: compile/link target.  Render correctness (the decode
 * byte-output and the final frame) is DEFERRED to Task 7 — the decode path and
 * render pipeline are structurally faithful but NOT yet runtime-validated here.
 */

#include "bumpy.h"

/* ── Decoded level buffer sizes ──────────────────────────────────────────────
   Sizes are the OUTER decoded sizes after op4+op12 processing.  The inner
   op12 stream leaves the final atlas/map/BUM image starting at arena[0].

   PAV atlas:  op4 decodes to w1=0x3e7f inner bytes; after op12 the raster
               occupies 0x7806 bytes (6 planes × 40×192 = 7680; +0x186 header).
   DEC map:    w1=0x19ea inner bytes.
   BUM objects: w1=0x369 inner bytes.

   The "decoded size" here refers to the PADDED buffer size we allocate; the
   op12 arena holds up to OP12_ARENA_SIZE (0x8000) bytes which covers all three.
   We copy the portion we actually need out of the arena into per-file buffers.

   RECONSTRUCTION FIDELITY: the engine uses far-segmented level_pav_buf /
   level_dec_buf / level_bum_buf in DGROUP (at fixed offsets set by the linker
   at Turbo C link time).  Here we use named __far globals declared in level.c
   to keep them out of DGROUP.  The logical content is identical.
*/
#define LEVEL_PAV_BUF_SIZE  0x7806u   /* PAV decoded atlas size */
#define LEVEL_DEC_BUF_SIZE  0x2f96u   /* DEC decoded size — ENGINE value (start_level
                                         1000:2d14 dec_len, DGROUP word @0x00a0 =
                                         2 + 15*0x32c).  Was 0x2000 "conservative":
                                         blocks 10+ truncated → broken bg on nodes
                                         11-15 (fixed 2026-07-03). */
#define LEVEL_BUM_BUF_SIZE  0x0b60u   /* BUM decoded size — ENGINE value (bum_len,
                                         DGROUP word @0x00e6 = 2 + 15*0xc2).  Was
                                         0x400: blocks 5+ read heap garbage past the
                                         copy-out → node 6 partial / node 7 broken
                                         entity layers (fixed 2026-07-03). */
#define LEVEL_BANK_BUF_SIZE 0x15c20u  /* BUMSPJEU.BIN sprite bank (~87 KB) */

/* ── Level resource file load sizes (raw file sizes, ≤ 0x8000 = OP12_ARENA_SIZE) */
#define LEVEL_PAV_FILE_MAX  0x3c00u   /* D1.PAV = 15071 B; 16384 covers it  */
#define LEVEL_DEC_FILE_MAX  0x1a00u   /* D1.DEC = 6436 B                    */
#define LEVEL_BUM_FILE_MAX  0x0400u   /* D1.BUM = 686 B                     */
#define LEVEL_BANK_FILE_MAX 0x4000u   /* BUMSPJEU.BIN chunks (stream-loaded) */

/* ── DGROUP entity shadow buffer size ────────────────────────────────────────
   entity_draw_* read from a `dg` pointer (u8 __far *) at fixed DGROUP offsets
   up to 0xa0de+2 = 0xa0e0.  We allocate a 0xa200-byte shadow buffer in the far
   heap, populate the relevant fields at render time, and pass it as `dg`.
   This avoids requiring the real DGROUP to have a fixed layout.

   RECONSTRUCTION FIDELITY: in the DOS target these fields ARE in the real DGROUP
   at the specified offsets (placed there by the Turbo C linker at the same
   addresses the decomp shows).  In this Open Watcom build the DGROUP layout
   is not controlled to those exact offsets, so we use the shadow buffer.
*/
#define LEVEL_DG_SIZE       0xa200u

/* ── DGROUP field offsets (mirrors entity.c / entity.h definitions) ──────── */
#define DG_POSA_X_BASE  0x00f4u   /* posA X: dg[0xf4 + cell*4]            */
#define DG_POSA_Y_BASE  0x00f6u   /* posA Y: dg[0xf6 + cell*4]            */
#define DG_POSC_X_BASE  0x0274u   /* posC X: dg[0x274 + cell*4]           */
#define DG_POSC_Y_BASE  0x0276u   /* posC Y: dg[0x276 + cell*4]           */
#define DG_POSB_X_BASE  0x03f4u   /* posB X: dg[0x3f4 + cell*4]           */
#define DG_POSB_Y_BASE  0x03f6u   /* posB Y: dg[0x3f6 + cell*4]           */
#define DG_P1_OBJ       0x792eu   /* p1_sprite struct (0x18 bytes)         */
#define DG_P2_OBJ       0x795au   /* p2_sprite struct (0x18 bytes)         */
#define DG_P2_CELL      0x8571u   /* p2_cell (s8): -1 = P2 absent          */
#define DG_P2_FRAME_BASE 0xa0deu  /* p2_frame_base (u16)                   */

/* Sprite obj field offsets within the 0x18-byte struct */
#define OBJ_FTBL_OFF    0x06u     /* frametable far ptr: offset half       */
#define OBJ_FTBL_SEG    0x08u     /* frametable far ptr: segment half      */

/* ── Engine globals ──────────────────────────────────────────────────────────
   Declared here; defined in level.c.  These mirror the engine's DGROUP globals
   as documented in the decomp.  current_level is a u8 (the engine uses a byte
   at DGROUP:0x79b2).

   RECONSTRUCTION FIDELITY: the engine stores current_level as a 1-byte global;
   start_level is parameterless and reads it directly.  We add (world, level)
   parameters for clarity but also keep current_level so the copy-protection
   guard (1 < current_level) is directly reconstructable.
*/
extern u8 current_level;    /* DGROUP:0x79b2 — current level index (1-based) */
extern u8 copyprotect_flag; /* DGROUP:?  — copy-protection state: 0=ok, -1=fail */

extern u16 p1_start_x;      /* DGROUP 0x791c — p1 start pixel X (WORD: 1000:2d83/2d97) */
extern u16 p1_start_y;      /* DGROUP 0x791e — p1 start pixel Y (WORD: 1000:2d80)      */
extern u8 current_entity_index; /* DGROUP:? — next entity slot index       */

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Port of start_level (1000:2d14).
   `world` and `level` are added parameters for clarity (original reads
   current_level from DGROUP).  Internally sets current_level = level.
   Loads D{level}.PAV, D{level}.DEC, D{level}.BUM from the level resource
   directory.  Then renders bg + entity layers and calls video_blit_planar. */
void start_level(u8 world, u8 level);

/* Copy-protection interactive challenge (1000:4015), gated by the compile-time
   #ifdef BUMPY_COPY_PROTECTION (NOT defined in the default build).  In the
   original, start_level calls this (levels > 1 only, when copyprotect_flag == 0)
   to display a sprite-identification quiz; the shipped cracked build defeats it
   (copyprotect_flag = 1 unconditional).  With the macro OFF the whole hook —
   declaration and call — compiles out.  Faithful un-cracked body → Phase 7b. */
#ifdef BUMPY_COPY_PROTECTION
void copyprotect_challenge(void);
#endif

/* all_entries_flag_set (1000:3e8a, Phase-9 T3) — level-complete predicate the
   game_loop round exit polls.  Walks the per-level move-descriptor table (the
   runtime-populated far ptr move_descriptor_table @ DGROUP 0x8246) ANDing each
   record's [0] flag; returns 1 iff all set. */
extern u8 __far *move_descriptor_table;  /* DGROUP 0x8246/0x8248 */
u8 all_entries_flag_set(void);

#ifdef BUMPY_PLAYABLE
/* level_get_entity_dg — expose the entity-DG shadow pointer (g_entity_dg) to
 * host_view.c so init_sprite_structs can point p1_sprite / p2_sprite into the
 * same buffer that hr_blit_obj reads from (host_render.c: hr_dg = g_entity_dg
 * via host_render_bind).  ONLY available under BUMPY_PLAYABLE. */
u8 __far *level_get_entity_dg(void);
#endif /* BUMPY_PLAYABLE */

#endif /* LEVEL_H_INCLUDED */
