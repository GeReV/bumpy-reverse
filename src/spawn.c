/* ════════════════════════════════════════════════════════════════════════════
 *  spawn.c — spawn_and_draw_level_entities (1000:2a78)
 *
 *  Phase-8 Task 2.  Strictly structure-faithful 1:1 port of the level-load
 *  entity-placement orchestrator, decompiled fresh from the live Ghidra decomp +
 *  raw disassembly of 1000:2a78 (verified 2026-06).  This is the channel-A/B
 *  RECORD POPULATOR + the layer-C static-sprite blitter + the P1/P2 BUM-header
 *  spawn-field reader.  It runs once per level load (from reset_game_state, the
 *  engine spine), placing the level's static entities and seeding the per-frame
 *  animation channels.
 *
 *  Source of truth: Ghidra BumpyDecomp decompile_function_by_address(1000:2a78) +
 *  disassemble_function(1000:2a78) + tools/spawn_oracle.py + local/build/
 *  spawn_model.md (the Phase-8 T1 multi-level capture).
 *
 *  ── ENGINE FLOW (decomp 1000:2a78) ────────────────────────────────────────────
 *   1. CHANNEL RESET/ACTIVATE.  Zero the active byte of the 3 A records
 *      (0x4c40/4c4c/4c58) + the 4 B records (0x4c80/4c8c/4c98/4ca4).  Load slot-0's
 *      far ptr from the A/B slot tables, then set slot-0 active=1 + cmd byte(+6)=1
 *      for both channels (and the two secondary mirrors 0x8e8b/0x8e8c + cmd-byte
 *      scalars 0x8578/0x8579).  (asm 2a89..2af1.)
 *   2. BUM-HEADER SPAWN READS.  Via tilemap (0xa0d8): p1_cell = hdr[+0x90] (-1 if
 *      nonzero), level_exit_cell = hdr[+0x91] (-1 if nonzero), items_remaining =
 *      hdr[+0x92].  Via level_src_ptr (0x75d0): p2_cell = hdr[+0x93]-1,
 *      p2_ai_threshold = hdr[+0x95], p2_move_state = hdr[+0x94], p2_frame_base =
 *      spawn_p2_frame_tbl[hdr[+0x96]] (0x2546).  (asm 2af4..2b5b.)
 *   3. setup_fullscreen_view() (1000:483c).  (asm 2b5e.)
 *   4. 6x8 GRID SCAN (cell = row*8 + col), per cell:
 *        layer A: cv = tilemap[+0x00 + cell]; if cv != 0: type = spawn_a_type_tbl
 *          [cv]; descfar = MK_FP(anim_a_frame_tbl seg/off at type*4); record-A[+1] =
 *          cell, [+8] = descfar[0], [+10] = descfar[1]; draw_anim_channels_a();
 *          erase_anim_channels_a().  (asm 2b9a..2be8.)
 *        layer B: cv = tilemap[+0x30 + cell]; if cv != 0 AND col != 7: type =
 *          spawn_b_type_tbl[cv]; descfar = MK_FP(anim_b_frame_tbl seg/off at
 *          type*4); record-B[+1] = cell, [+8] = descfar[0], [+10] = descfar[1];
 *          draw_anim_channels_b(); erase_anim_channels_b().  (asm 2c03..2c58.)
 *        layer C: cv = tilemap[+0x60 + cell]; if cv != 0: p1_sprite[+0] =
 *          posC_x[cell], p1_sprite[+2] = posC_y[cell], p1_sprite[+4] = cv + 0x179;
 *          blit_sprite(0x792e, 0x203b).  (asm 2c73..2cdb.)
 *   5. DEACTIVATE: record-A/B slot-0 cmd byte(+6) = 0, active(+0) = 0.  (asm 2cf6.)
 *
 *  ── RECONSTRUCTION FIDELITY ────────────────────────────────────────────────────
 *   • The render-core LEAVES (setup_fullscreen_view, draw/erase_anim_channels_a/b,
 *     blit_sprite) are the already-reconstructed / faithful-signature primitives
 *     owned by their modules (anim.c / level.c declares blit_sprite; bgi_overlay.c
 *     models setup_fullscreen_view's fullscreen restore).  spawn.c calls them by
 *     name and does not re-implement them.  draw_anim_channels_a/b each NEST a
 *     blit_sprite per active entity (Phase-5-validated behavior); the host replay
 *     harness (tools/spawn_ctest.c) separates those nested blits from spawn's own
 *     layer-C blits via the trace's per-fill layer tag (see the harness header).
 *   • The slot-table far-ptr split (0x4c70 off / 0x4c72 seg) the decomp CONCATs to
 *     reach slot-0's record is modelled by anim.c's anim_channels_a/b_tbl as a
 *     `anim_chan_rec __far *[]` array (off+seg in one far ptr); spawn.c reaches the
 *     active record as anim_channels_X_tbl[0].  The byte-offset writes the decomp
 *     renders as puVar2[1]/[6]/+8/+10 (table-base relative) are, in the asm, all
 *     done through the slot-0 record far ptr (LES BX,[BP-0x10] / [-0x14]); mirrored
 *     here as the typed anim_chan_rec fields (cell/+1, frame/+6, data_off/+8,
 *     data_seg/+10) of anim_channels_X_tbl[0].
 *   • The near DGROUP tables the engine reads DS-relative (spawn_a/b_type_tbl @
 *     0x3d3a/0x4086, spawn_p2_frame_tbl @0x2546, the A/B far-ptr frame tables @
 *     0x3d6a/0x40a6, the layer-C pos table @0x274) are reached through their
 *     module-owned C symbols; the indexing arithmetic is 1:1 with the asm.  The
 *     descriptor far ptr (anim_X_frame_tbl[type*4]) is rebuilt with MK_FP at the use
 *     site (the engine LES'es it), same as anim.c / player.c.
 *  See docs/reconstruction-fidelity.md + docs/rendering-pipeline.md.
 * ════════════════════════════════════════════════════════════════════════════ */
#include "spawn.h"
#include "anim.h"   /* anim_channels_a/b_tbl, anim_a/b_frame_tbl, g_anim_cur_cmd_byte,
                       anim_b_cur_frame_byte, p1_sprite, tilemap (all OWNED elsewhere) */

/* ── globals OWNED BY spawn.c (DEFINED here; no other TU owns them; grep-verified
 *    against the src tree — see spawn.h ownership block) ─────────────────────── */
u8  spawn_a_type_tbl[SPAWN_TYPE_TBL_LEN];      /* DGROUP 0x3d3a */
u8  spawn_b_type_tbl[SPAWN_TYPE_TBL_LEN];      /* DGROUP 0x4086 */
u16 spawn_p2_frame_tbl[SPAWN_P2_FRAME_TBL_LEN];/* DGROUP 0x2546 */
u8  g_anim_a_active_mirror;                    /* DGROUP 0x8e8b */
u8  g_anim_b_active_mirror;                    /* DGROUP 0x8e8c */

/* ── globals OWNED ELSEWHERE (extern; spawn.c reads/writes, must NOT redefine) ───
 *  anim_channels_a/b_tbl, anim_a/b_frame_tbl, g_anim_cur_cmd_byte,
 *  anim_b_cur_frame_byte, p1_sprite, tilemap come from anim.h above. */
extern u8 __far *level_src_ptr;   /* game.c   0x75d0 — level-archive source cursor   */
extern u8  p1_cell;               /* player.c 0x856e — P1 grid cell                  */
extern u8  level_exit_cell;       /* items.c  0x8572 — grid cell of the level exit   */
extern u8  items_remaining;       /* items.c  0xa0cf — items left on the level        */
extern s8  p2_cell;               /* player2.c 0x8571 — P2 grid cell; -1 = absent     */
extern u8  p2_ai_threshold;       /* player2.c 0x7920 — AI rng-branch threshold       */
extern u8  p2_move_state;         /* game.c   0x8562 — P2 launch move state          */
extern u16 p2_frame_base;         /* player2.c 0xa0de — P2 frame-table base index     */
extern u8 __far *p2_cell_coord_tbl;/* player2.c 0x0274 — layer-C pos tbl (X@+0,Y@+2)  */

/* ── render-core LEAVES (declared here; bodies owned by their modules) ───────────
 *  setup_fullscreen_view 1000:483c, draw/erase_anim_channels_a/b (anim.c), and
 *  blit_sprite 1000:942a (declared exactly as level.c declares it). */
extern void setup_fullscreen_view(void);          /* 1000:483c (faithful leaf, game_stubs)*/
extern void draw_anim_channels_a(void);           /* 1000:165e (anim.c)               */
extern void erase_anim_channels_a(void);          /* 1000:1a67 (anim.c)               */
extern void draw_anim_channels_b(void);           /* 1000:17c7 (anim.c)               */
extern void erase_anim_channels_b(void);          /* 1000:1b2b (anim.c)               */
/* The engine's blit_sprite (1000:942a) is the BGI-overlay self-modifying leaf that
   does not decompile; the project reconstructs it as anim.c's anim_blit_sprite_leaf
   (same address, faithful signature) and EVERY caller (anim.c, player2.c, screens.c)
   routes through it.  spawn.c follows that convention so the layer-C call is the same
   1:1 leaf — anim_blit_sprite_leaf(0x792e, DS), the decomp's blit_sprite(&sprite_obj_
   203b_792e, 0x203b).  See docs/reconstruction-fidelity.md. */
extern void anim_blit_sprite_leaf(u16 obj_off, u16 obj_seg); /* blit_sprite 1000:942a */

/* DGROUP segment of the literal blit object (decomp: blit_sprite(&sprite_obj_203b_792e,
   0x203b)).  0x203b in the real build; the ctest harness may override to the runtime seg —
   same guarded convention as anim.c. */
#ifndef ANIM_DGROUP_RUNTIME_SEG
#ifdef BUMPY_PLAYABLE
extern u16 host_dgroup_seg(void);   /* host_render.c — loaded image's real DGROUP seg */
#define ANIM_DGROUP_RUNTIME_SEG host_dgroup_seg()
#else
#define ANIM_DGROUP_RUNTIME_SEG 0x203b
#endif
#endif

#ifdef BUMPY_PLAYABLE
extern void host_render_set_spawn(u8 active);   /* host_render.c — suppress the anim
                                                   clean-bg repaint during the spawn scan
                                                   (world-2 platform over-paint fix, F1) */
#endif

/* NOTE: the decomp opens with the Borland stack-overflow-check prologue
   (CMP SP,stack_check_limit; CALL FUN_1000_ab83).  That is a non-semantic
   compiler-emitted guard, omitted here as throughout the reconstruction. */
void spawn_and_draw_level_entities(void)
{
    anim_chan_rec __far *rec_a;   /* local_12 — A slot-0 record (active slot)         */
    anim_chan_rec __far *rec_b;   /* local_16 — B slot-0 record (active slot)         */
    u8 __far  *bum_hdr;           /* via tilemap (0xa0d8) — BUM header (p1/exit/items)*/
    u8 __far  *src;               /* via level_src_ptr (0x75d0) — p2 fields           */
    u8         grid_row;
    u8         grid_col;
    u8         cell_index;
    u8         cv;                /* uVar6 — the runtime tilemap byte at the cell     */
    u8         type;             /* the 0x3d3a / 0x4086 remap of cv                  */
    u16 __far *descfar;          /* local_a / local_e — the A/B far-ptr descriptor   */
    u8  __far *p1d;              /* p1_sprite layer-C blit descriptor (0x792e pointee)*/

#ifdef BUMPY_PLAYABLE
    /* HOST-ONLY (F1): gate the anim clean-bg repaint OFF for the whole spawn scan so a
       neighbour cell's erase-before-blit cannot over-paint a freshly-blitted layer-A
       structure (world-2 platform).  Faithful build unaffected.  See host_render.c. */
    host_render_set_spawn(1u);
#endif

    /* ── 1. CHANNEL RESET / ACTIVATE (asm 2a89..2af1) ──────────────────────────────
       Zero the active byte of the 3 A + 4 B records, then activate slot 0 of each
       channel.  rec_a/rec_b are the slot-0 record far ptrs the decomp CONCATs from
       the (off,seg) slot-table halves — i.e. anim_channels_X_tbl[0]. */
    anim_channels_a_tbl[0]->active = 0;   /* DAT_203b_4c40 = 0 */
    anim_channels_a_tbl[1]->active = 0;   /* DAT_203b_4c4c = 0 */
    anim_channels_a_tbl[2]->active = 0;   /* DAT_203b_4c58 = 0 */
    anim_channels_b_tbl[0]->active = 0;   /* DAT_203b_4c80 = 0 */
    anim_channels_b_tbl[1]->active = 0;   /* DAT_203b_4c8c = 0 */
    anim_channels_b_tbl[2]->active = 0;   /* DAT_203b_4c98 = 0 */
    anim_channels_b_tbl[3]->active = 0;   /* DAT_203b_4ca4 = 0 */

    rec_a = anim_channels_a_tbl[0];       /* local_12 = MK_FP(seg_tbl[0], tbl[0])     */
    rec_b = anim_channels_b_tbl[0];       /* local_16 = MK_FP(seg_tbl[0], tbl[0])     */

    rec_a->active = 1;                    /* *local_12 = 1                            */
    g_anim_a_active_mirror = 1;           /* DAT_203b_8e8b = 1                        */
    rec_b->active = 1;                    /* *local_16 = 1                            */
    g_anim_b_active_mirror = 1;           /* DAT_203b_8e8c = 1                        */
    rec_a->frame = 1;                     /* puVar2[6] = 1 (record-A cmd byte +6)     */
    g_anim_cur_cmd_byte = 1;              /* DGROUP 0x8578                            */
    rec_b->frame = 1;                     /* puVar4[6] = 1 (record-B cmd byte +6)     */
    anim_b_cur_frame_byte = 1;            /* DGROUP 0x8579                            */

    /* ── 2. BUM-HEADER SPAWN READS (asm 2af4..2b5b) ───────────────────────────────
       p1_cell / level_exit_cell / items_remaining read via tilemap (0xa0d8);
       p2_* read via level_src_ptr (0x75d0). */
    bum_hdr = tilemap;
    p1_cell = bum_hdr[0x90];
    if (p1_cell != 0) {
        p1_cell = p1_cell - 1;
    }
    level_exit_cell = bum_hdr[0x91];
    if (level_exit_cell != 0) {
        level_exit_cell = level_exit_cell - 1;
    }
    items_remaining = bum_hdr[0x92];

    src = level_src_ptr;
    p2_cell = (s8)(src[0x93] - 1);
    p2_ai_threshold = src[0x95];
    p2_move_state = src[0x94];
    p2_frame_base = spawn_p2_frame_tbl[src[0x96]];

    /* ── 3. setup_fullscreen_view (asm 2b5e) ─────────────────────────────────────── */
    setup_fullscreen_view();

    /* ── 4. 6x8 GRID SCAN (asm 2b61..2cf3) ───────────────────────────────────────── */
    for (grid_row = 0; grid_row < 6; grid_row = grid_row + 1) {
        for (grid_col = 0; grid_col < 8; grid_col = grid_col + 1) {
            cell_index = grid_row * 8 + grid_col;

            /* ── layer A (tilemap[+0x00 + cell]) — asm 2b9a..2be8 ──────────────────── */
            cv = tilemap[(u16)grid_row * 8 + (u16)grid_col];
            if (cv != 0) {
                type = spawn_a_type_tbl[cv];
                descfar = (u16 __far *)MK_FP(
                    *(u16 *)(anim_a_frame_tbl + (u16)type * 4 + 2),   /* 0x3d6c seg */
                    *(u16 *)(anim_a_frame_tbl + (u16)type * 4 + 0));  /* 0x3d6a off */
                rec_a->cell     = cell_index;       /* puVar2[1] = cell_index          */
                rec_a->data_off = descfar[0];       /* *(puVar2+8)  = descriptor[+0]    */
                rec_a->data_seg = descfar[1];       /* *(puVar2+10) = descriptor[+2]    */
                draw_anim_channels_a();
                erase_anim_channels_a();
            }

            /* ── layer B (tilemap[+0x30 + cell], skip col 7) — asm 2c03..2c58 ─────── */
            cv = tilemap[(u16)grid_row * 8 + (u16)grid_col + 0x30];
            if ((cv != 0) && (grid_col != 7)) {
                type = spawn_b_type_tbl[cv];
                descfar = (u16 __far *)MK_FP(
                    *(u16 *)(anim_b_frame_tbl + (u16)type * 4 + 2),   /* 0x40a8 seg */
                    *(u16 *)(anim_b_frame_tbl + (u16)type * 4 + 0));  /* 0x40a6 off */
                rec_b->cell     = cell_index;       /* puVar4[1] = cell_index          */
                rec_b->data_off = descfar[0];       /* *(puVar4+8)  = descriptor[+0]    */
                rec_b->data_seg = descfar[1];       /* *(puVar4+10) = descriptor[+2]    */
                draw_anim_channels_b();
                erase_anim_channels_b();
            }

            /* ── layer C (tilemap[+0x60 + cell]) — asm 2c73..2cdb ──────────────────── */
            cv = tilemap[(u16)grid_row * 8 + (u16)grid_col + 0x60];
            if (cv != 0) {
                /* posC X/Y from the layer-C coord table (0x274): each cell entry is
                   X@+0, Y@+2; index = (col*2 + row*0x10)*2 = cell*4 (== cell stride 4).
                   1:1 with asm: word ptr [BX+0x274] / [BX+0x276], BX = (col*2+row*16)*2. */
                u16 idx = ((u16)grid_col * 2 + (u16)grid_row * 0x10) * 2;
                u16 __far *posc = (u16 __far *)MK_FP(
                    FP_SEG(p2_cell_coord_tbl),
                    (u16)(FP_OFF(p2_cell_coord_tbl) + idx));
                p1d = p1_sprite;
                *(u16 __far *)(p1d + 0) = posc[0];                         /* x @0x274 */
                *(u16 __far *)(p1d + 2) = posc[1];                         /* y @0x276 */
                *(u16 __far *)(p1d + 4) = (u16)(cv + 0x179);              /* frame    */
                anim_blit_sprite_leaf(0x792e, ANIM_DGROUP_RUNTIME_SEG);  /* blit_sprite(&sprite_obj_203b_792e, 0x203b) — literal DGROUP obj addr per the decomp/asm, not a p1_sprite deref */
            }
        }
    }

    /* ── 5. DEACTIVATE channel slot-0 records (asm 2cf6..2d0b) ────────────────────── */
    rec_a->frame  = 0;   /* puVar2[6] = 0 */
    rec_a->active = 0;   /* *local_12 = 0 */
    rec_b->frame  = 0;   /* puVar4[6] = 0 */
    rec_b->active = 0;   /* *local_16 = 0 */

#ifdef BUMPY_PLAYABLE
    host_render_set_spawn(0u);   /* re-enable the per-tick anim erase (trail-fix) */
#endif
}
