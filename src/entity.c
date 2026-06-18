#include <string.h>
#include "entity.h"
#include "sprite_anim.h"
#include "sprite_chain.h"
#include "sprite_blit.h"

/* See entity.h.  Field offsets / arithmetic mirror the Ghidra decomp of the
   layer-C section inlined in spawn_and_draw_level_entities (1000:2a78, lines
   829-838 of the decomp body) and draw_p1_sprite (1000:1cb2) /
   draw_p2_sprite (1000:1cea).

   The engine layer-C loop:
     for grid_row 0..5, grid_col 0..7:
       cell = grid_row*8 + grid_col
       cv = *(byte *)((int)tilemap + 0x60 + cell)
       if (cv == 0) continue
       *p1_sprite         = posC_X[cell]       // dg[0x274 + cell*4]
       ((u16*)p1_sprite)[1] = posC_Y[cell]     // dg[0x276 + cell*4]
       p1_sprite[2]       = cv + 0x179         // frame index
       blit_sprite(0x792e, 0x203b)             // -> prepare -> build_desc -> blit

   draw_p1_sprite (1000:1cb2):
     if p1_move_anim == 100: return
     p1_sprite[0] = p1_pixel_x
     p1_sprite[1] = p1_pixel_y
     p1_sprite[2] = p1_move_anim
     blit_sprite(0x792e, 0x203b)

   draw_p2_sprite (1000:1cea):
     if p2_cell == -1: return     (P2 absent)
     p2_sprite[0] = p2_pixel_x
     p2_sprite[1] = p2_pixel_y
     p2_sprite[2] = p2_frame_base + p2_move_anim
     blit_sprite(0x795a, 0x203b)

   Here blit_sprite is expanded into the three validated pipeline stages.
   The frame-table far ptr (obj+6/obj+8) is read from the engine's sprite obj
   struct (p1 @ DGROUP:0x792e, p2 @ DGROUP:0x795a) as populated at level init.
   We seed the object's frametable far ptr from the captured dg snapshot.
*/

/* DG offsets for posC table and sprite obj structs */
#define DG_POSC_BASE  0x0274u   /* first XY pair; cell n at 0x274 + n*4 */
#define DG_P1_OBJ     0x792eu   /* p1_sprite obj struct start in DGROUP */
#define DG_P2_OBJ     0x795au   /* p2_sprite obj struct start in DGROUP */

/* DG offsets for posA/posB tables (draw_anim_channels_a/b: 0xf4/0xf6 and 0x3f4/0x3f6) */
#define DG_POSA_X_BASE 0x00f4u  /* posA X: dg[0xf4 + cell*4]  (interleaved; Y at 0xf6+cell*4) */
#define DG_POSA_Y_BASE 0x00f6u  /* posA Y */
#define DG_POSB_X_BASE 0x03f4u  /* posB X: dg[0x3f4 + cell*4] */
#define DG_POSB_Y_BASE 0x03f6u  /* posB Y */

/* p1_sprite obj field offsets (bytes within the 0x18-byte struct) */
#define OBJ_X         0x00u     /* s16 pixel X */
#define OBJ_Y         0x02u     /* s16 pixel Y */
#define OBJ_FRAME_IDX 0x04u     /* u16 frame index */
#define OBJ_FTBL_OFF  0x06u     /* u16 frametable far ptr: offset half */
#define OBJ_FTBL_SEG  0x08u     /* u16 frametable far ptr: segment half */
#define OBJ_FLAGS     0x0au     /* u8 flags: 0x80=visible 0x20=hflip 0x01=align */

#define OBJ_VISIBLE   0x80u     /* flags bit: visible */

/* P1 hidden-sentinel: move_anim == 100 → player hidden, skip draw */
#define P1_HIDDEN_SENTINEL  100u

/* Size of the sprite object work buffer.  sprite_prepare_frame writes up to
   obj+0x2c (count + 3 sub-header entries * 6 bytes starting at obj+0x1a);
   use 0x40 to match the work buffer size in anim_ctest.c. */
#define OBJ_SIZE      0x40u

/* PLANE_SIZE must match sprite_blit.c's PLANE_SIZE */
#define ENTITY_PLANE_SIZE  0x10000UL

/* -----------------------------------------------------------------------
   Layer A/B descriptor lookup tables.
   Derived from tools/extract/anim_tables.json (decomp-derived; validated by
   render_levels.py).  The engine resolves {yoff, frame} per cell value cv via:
     remap   = dg[cv + 0x3d3a]      (layer A) or dg[cv + 0x4086] (layer B)
     descfar = far ptr at dg[0x3d6a + remap*4]  (or 0x40a6 for B)
     word0   = yoff, word1 = frame
   The descriptor far ptr lands at seg 0x114b (outside DGROUP, in a code-area
   data segment), so it cannot be dereferenced from the captured dg snapshot.
   These tables resolve the same mapping from the decomp-derived JSON.
   cv==0 or a zero-frame entry => remap would be 0 (null descriptor) => skip draw.
   ----------------------------------------------------------------------- */

typedef struct {
    s16 yoff;
    u16 frame;
} anim_desc_t;

/* Layer A: indexed by cv (0..63).  frame==0 && cv!=0 is not possible for valid
   entries; zero-frame rows are "draw nothing" (remap==0) slots. */
static const anim_desc_t anim_a_desc[64] = {
    /* [ 0] */ { 0,   0 }, /* cv==0: always skip */
    /* [ 1] */ { 5,  64 },
    /* [ 2] */ { 5, 204 },
    /* [ 3] */ { 5,  70 },
    /* [ 4] */ { 0,   0 }, /* remap==0: draw nothing */
    /* [ 5] */ { 3,  79 },
    /* [ 6] */ { 3,  81 },
    /* [ 7] */ { 5,  83 },
    /* [ 8] */ { 5,  92 },
    /* [ 9] */ { 5, 101 },
    /* [10] */ { 5, 113 },
    /* [11] */ {-26, 63 },
    /* [12] */ { 5, 132 },
    /* [13] */ { 6, 110 },
    /* [14] */ { 5, 155 },
    /* [15] */ { 2, 159 },
    /* [16] */ { 5, 163 },
    /* [17] */ { 4, 168 },
    /* [18] */ { 1, 179 },
    /* [19] */ { 0, 180 },
    /* [20] */ { 0, 196 },
    /* [21] */ { 0, 202 },
    /* [22] */ { 2, 203 },
    /* [23] */ { 2, 137 },
    /* [24] */ { 2, 136 },
    /* [25] */ { 5, 138 },
    /* [26] */ { 5, 212 },
    /* [27] */ { 5, 211 },
    /* [28] */ { 5, 210 },
    /* [29] */ { 5, 209 },
    /* [30] */ { 4, 216 },
    /* [31] */ { 2, 223 },
    /* [32] */ { 0, 190 },
    /* [33] */ { 2, 189 },
    /* [34] */ { 0,   0 }, /* remap==0 */
    /* [35] */ { 0,   0 }, /* remap==0 */
    /* [36] */ { 0,   0 }, /* remap==0 */
    /* [37] */ { 0,   0 }, /* remap==0 */
    /* [38] */ { 0,   0 }, /* remap==0 */
    /* [39] */ { 0,   0 }, /* remap==0 */
    /* [40] */ { 0,   0 }, /* remap==0 */
    /* [41] */ { 0,   0 }, /* remap==0 */
    /* [42] */ { 0,   0 }, /* remap==0 */
    /* [43] */ { 0,   0 }, /* remap==0 */
    /* [44] */ { 0,   0 }, /* remap==0 */
    /* [45] */ { 0,   0 }, /* remap==0 */
    /* [46] */ { 0,   0 }, /* remap==0 */
    /* [47] */ { 0,   0 }, /* remap==0 */
    /* [48] */ { 0,   0 }, /* remap==0 */
    /* [49] */ { 0,   0 }, /* remap==0 */
    /* [50] */ { 0,   0 }, /* remap==0 */
    /* [51] */ { 0,   0 }, /* remap==0 */
    /* [52] */ { 3,  92 },
    /* [53] */ { 2, 118 },
    /* [54] */ { 2, 122 },
    /* [55] */ { 3,  79 },
    /* [56] */ { 0, 203 },
    /* [57] */ { 2, 118 },
    /* [58] */ { 2, 122 },
    /* [59] */ { 3,  79 },
    /* [60] */ { 0,   0 }, /* remap==0 */
    /* [61] */ { 2, 118 },
    /* [62] */ { 2, 122 },
    /* [63] */ { 3,  79 }
};

/* Layer B: indexed by cv (0..63).  draw_anim_channels_b adds 0xf1 to frame
   at draw time; stored here pre-bias, bias applied in entity_draw_layer_b. */
static const anim_desc_t anim_b_desc[64] = {
    /* [ 0] */ { 0,   0 }, /* cv==0: always skip */
    /* [ 1] */ { 2, 241 },
    /* [ 2] */ { 2, 243 },
    /* [ 3] */ { 2, 254 },
    /* [ 4] */ { 2, 264 },
    /* [ 5] */ { 2, 311 },
    /* [ 6] */ { 2, 274 },
    /* [ 7] */ { 2, 280 },
    /* [ 8] */ { 4, 287 },
    /* [ 9] */ { 1, 302 },
    /* [10] */ { 1, 303 },
    /* [11] */ { 1, 304 },
    /* [12] */ { 2, 310 },
    /* [13] */ { 2, 317 },
    /* [14] */ { 2, 332 },
    /* [15] */ { 2, 335 },
    /* [16] */ { 2, 337 },
    /* [17] */ { 2, 339 },
    /* [18] */ { 8, 341 },
    /* [19] */ { 2, 345 },
    /* [20] */ { 0,   0 }, /* remap==0 */
    /* [21] */ { 0,   0 }, /* remap==0 */
    /* [22] */ { 0,   0 }, /* remap==0 */
    /* [23] */ { 0,   0 }, /* remap==0 */
    /* [24] */ { 0,   0 }, /* remap==0 */
    /* [25] */ { 0,   0 }, /* remap==0 */
    /* [26] */ { 0,   0 }, /* remap==0 */
    /* [27] */ { 0,   0 }, /* remap==0 */
    /* [28] */ { 0,   0 }, /* remap==0 */
    /* [29] */ { 0,   0 }, /* remap==0 */
    /* [30] */ { 0,   0 }, /* remap==0 */
    /* [31] */ { 0,   0 }, /* remap==0 */
    /* [32] */ { 0,   0 }, /* remap==0 */
    /* [33] */ { 0,   0 }, /* remap==0 */
    /* [34] */ { 0,   0 }, /* remap==0 */
    /* [35] */ { 0,   0 }, /* remap==0 */
    /* [36] */ { 0,   0 }, /* remap==0 */
    /* [37] */ { 2, 298 },
    /* [38] */ { 2, 299 },
    /* [39] */ { 2, 256 },
    /* [40] */ { 0,   0 }, /* remap==0 */
    /* [41] */ { 2, 298 },
    /* [42] */ { 2, 299 },
    /* [43] */ { 2, 256 },
    /* [44] */ { 0,   0 }, /* remap==0 */
    /* [45] */ { 2, 298 },
    /* [46] */ { 2, 299 },
    /* [47] */ { 2, 256 },
    /* [48] */ { 0,   0 }, /* remap==0 */
    /* [49] */ { 2, 298 },
    /* [50] */ { 2, 299 },
    /* [51] */ { 2, 256 },
    /* [52] */ { 0,   0 }, /* remap==0 */
    /* [53] */ { 2, 298 },
    /* [54] */ { 2, 299 },
    /* [55] */ { 2, 256 },
    /* [56] */ { 0,   0 }, /* remap==0 */
    /* [57] */ { 2, 298 },
    /* [58] */ { 2, 299 },
    /* [59] */ { 2, 256 },
    /* [60] */ { 0,   0 }, /* remap==0 */
    /* [61] */ { 2, 298 },
    /* [62] */ { 2, 299 },
    /* [63] */ { 2, 256 }
};

/* Frameguard bias for layer B (draw_anim_channels_b: frame += 0xf1) */
#define LAYER_B_FRAME_BIAS 0x00f1u

/* -----------------------------------------------------------------------
   anim_desc_valid — guard helper mirroring the engine's remap==0 check.
   Returns nonzero if the descriptor entry is a valid draw (cv != 0 and the
   table entry has a nonzero frame; cv==0 is always skip per the engine loop
   which only enters the branch when uVar6 != 0).
   ----------------------------------------------------------------------- */
static int anim_desc_valid_a(u8 cv)
{
    if (cv == 0u || cv >= 64u) {
        return 0;
    }
    return (anim_a_desc[cv].frame != 0u);
}

static int anim_desc_valid_b(u8 cv)
{
    if (cv == 0u || cv >= 64u) {
        return 0;
    }
    return (anim_b_desc[cv].frame != 0u);
}

static u16 dg_rd16(const u8 __far *dg, u16 off)
{
    return (u16)((u16)dg[off] | ((u16)dg[off + 1u] << 8));
}

static void ent_wr16(u8 *obj, u16 off, u16 val)
{
    obj[off]      = (u8)val;
    obj[off + 1u] = (u8)(val >> 8);
}

/* -----------------------------------------------------------------------
   entity_blit_object — shared pipeline driver
   Executes the three validated pipeline stages for a fully-populated 0x40-byte
   sprite object:
     sprite_prepare_frame -> sprite_blit_build_desc -> sprite_blit_planar_vga
   Caller must have already set obj[+0,+2,+4,+6/8,+0xa] (X, Y, frame_idx,
   frametable far ptr, flags) and zeroed the rest.  Returns without blitting
   if sprite_blit_build_desc culls the sprite.

   Corresponds to blit_sprite(obj_seg_off, 0x203b) in the engine, expanded
   into the three constituent validated stages.
   ----------------------------------------------------------------------- */
static void entity_blit_object(u8 __huge *planes, u8 *obj,
                                u8 __huge *bank, u32 bank_base_lin,
                                const sprite_view *view)
{
    u8  desc[0x18];
    u16 voff;
    u16 dst_stride;
    u16 full_w;
    u16 cols;
    u16 rows;
    u8  shift;
    u8  clip_flags;

    /* Stage 1: sprite_prepare_frame */
    sprite_prepare_frame(obj, bank, bank_base_lin);

    /* Stage 2: sprite_blit_build_desc — returns 0 if culled.
       On cull this helper returns early; the caller's loop continues as before
       (mirrors the engine's original `continue` in the layer-C sprite loop). */
    if (!sprite_blit_build_desc(obj, view, desc)) {
        return;
    }

    /* Stage 3: sprite_blit_planar_vga
       Unpack descriptor fields per chain_ctest.c / blit_ctest.c. */
    {
        u32 dlin = (u32)dg_rd16((const u8 __far *)desc, 0x0au) * 16u
                 + (u32)dg_rd16((const u8 __far *)desc, 0x08u);
        voff = (u16)(dlin - 0xA0000UL);
    }
    dst_stride = (u16)((u16)desc[0x0eu] | ((u16)desc[0x0fu] << 8));
    full_w     = (u16)((u16)desc[0x0cu] | ((u16)desc[0x0du] << 8));
    cols       = (u16)((u16)desc[0x10u] | ((u16)desc[0x11u] << 8));
    rows       = (u16)((u16)desc[0x12u] | ((u16)desc[0x13u] << 8));
    shift      = desc[0x16u];
    clip_flags = desc[0x17u];

    {
        u32 src_lin = (u32)((u16)desc[2] | ((u16)desc[3] << 8)) * 16u
                    + (u32)((u16)desc[0] | ((u16)desc[1] << 8));
        const u8 __far *src = (const u8 __far *)(bank + (src_lin - bank_base_lin));
        sprite_blit_planar_vga(planes, src, voff, dst_stride, full_w,
                               cols, rows, shift, clip_flags);
    }
}

/* -----------------------------------------------------------------------
   entity_draw_layer_c — faithful port of the layer-C static-sprite loop.
   Refactored to call entity_blit_object; behavior is unchanged vs the
   pre-refactor inline version (validated: bg+C == 51397/64000).
   ----------------------------------------------------------------------- */
void entity_draw_layer_c(u8 __huge *planes, const u8 __far *bum,
                         const u8 __far *dg, u8 __huge *bank,
                         u32 bank_base_lin, const sprite_view *view)
{
    u8  obj[OBJ_SIZE];
    u16 row;
    u16 col;

    for (row = 0; row < 6u; row++) {
        for (col = 0; col < 8u; col++) {
            u16 cell = row * 8u + col;
            u8  cv;

            /* Zero the work buffer each iteration. */
            memset(obj, 0, sizeof(obj));

            /* Seed obj[6..9] (frametable far ptr) from the captured p1_sprite obj in dg.
               This is constant across all cells — set at level init.  Re-seeded after
               each memset since the zero wipes bytes 6..9. */
            {
                u16 ftbl_off = dg_rd16(dg, (u16)(DG_P1_OBJ + OBJ_FTBL_OFF));
                u16 ftbl_seg = dg_rd16(dg, (u16)(DG_P1_OBJ + OBJ_FTBL_SEG));
                ent_wr16(obj, OBJ_FTBL_OFF, ftbl_off);
                ent_wr16(obj, OBJ_FTBL_SEG, ftbl_seg);
            }

            cv = bum[0x60u + cell];

            if (cv == 0) {
                continue;
            }

            /* Build sprite object: write X, Y, frame, flags. */
            ent_wr16(obj, OBJ_X,         dg_rd16(dg, (u16)(DG_POSC_BASE + cell * 4u)));
            ent_wr16(obj, OBJ_Y,         dg_rd16(dg, (u16)(DG_POSC_BASE + cell * 4u + 2u)));
            ent_wr16(obj, OBJ_FRAME_IDX, (u16)(cv + 0x179u));
            obj[OBJ_FLAGS] = OBJ_VISIBLE;

            entity_blit_object(planes, obj, bank, bank_base_lin, view);
        }
    }
}

/* -----------------------------------------------------------------------
   entity_draw_p1 — faithful port of draw_p1_sprite (1000:1cb2).
   Guard: if p1_move_anim == 100, player is hidden — draw nothing.
   Otherwise: obj[+0]=pixel_x, obj[+2]=pixel_y, obj[+4]=move_anim; blit.
   Frametable far ptr seeded from dg[0x792e+6..9] (engine's p1_sprite obj).
   ----------------------------------------------------------------------- */
void entity_draw_p1(u8 __huge *planes, const u8 __far *dg,
                    u16 pixel_x, u16 pixel_y, u16 move_anim,
                    u8 __huge *bank, u32 bank_base_lin,
                    const sprite_view *view)
{
    u8  obj[OBJ_SIZE];

    /* draw_p1_sprite (1000:1cb2): if p1_move_anim == 100, return (hidden). */
    if (move_anim == P1_HIDDEN_SENTINEL) {
        return;
    }

    memset(obj, 0, sizeof(obj));

    /* Seed frametable far ptr from engine's p1_sprite obj at DGROUP:0x792e[+6..9].
       This mirrors the engine where the frametable ptr is set at level init and
       reused by every draw_p1_sprite call. */
    {
        u16 ftbl_off = dg_rd16(dg, (u16)(DG_P1_OBJ + OBJ_FTBL_OFF));
        u16 ftbl_seg = dg_rd16(dg, (u16)(DG_P1_OBJ + OBJ_FTBL_SEG));
        ent_wr16(obj, OBJ_FTBL_OFF, ftbl_off);
        ent_wr16(obj, OBJ_FTBL_SEG, ftbl_seg);
    }

    /* draw_p1_sprite field writes:
         *puVar1 = p1_pixel_x;           obj[+0] = pixel X
         sprite_fields[1] = p1_pixel_y;  obj[+2] = pixel Y
         sprite_fields[2] = p1_move_anim; obj[+4] = frame index */
    ent_wr16(obj, OBJ_X,         pixel_x);
    ent_wr16(obj, OBJ_Y,         pixel_y);
    ent_wr16(obj, OBJ_FRAME_IDX, move_anim);
    obj[OBJ_FLAGS] = OBJ_VISIBLE;

    entity_blit_object(planes, obj, bank, bank_base_lin, view);
}

/* -----------------------------------------------------------------------
   entity_draw_layer_a — static placement port of the layer-A loop inside
   spawn_and_draw_level_entities (1000:2a78), draw side from draw_anim_channels_a
   (1000:165e).

   Engine flow (abbreviated, per-cell):
     cv = tilemap[0x00 + cell]     (bum[0x00 + cell] in our naming)
     if cv == 0: continue
     remap = dg[cv + 0x3d3a]
     if remap == 0: continue       (null descriptor; draw nothing)
     descfar = far ptr at dg[0x3d6a + remap*4]    -> {word0=yoff, word1=frame}
     // populate chan_a_rec[+1]=cell, [+8]=yoff, [+10]=frame; call draw_anim_channels_a
     // which does:
     obj[+0] = dg[0xf4 + cell*4]  (posA X)
     obj[+2] = dg[0xf6 + cell*4] + yoff  (posA Y + yoff)
     obj[+4] = frame
     if ((frame & 0x200) == 0): blit_sprite(p1_sprite obj @0x792e)
     // render_player_view: second draw (BGI overlay / double-buffer) — modeled
     //   as the same single blit_sprite call (P1 validated with blit_sprite alone;
     //   same approach here). See RECONSTRUCTION FIDELITY note in entity.h.

   DESCRIPTOR SOURCING: the far ptr at dg[0x3d6a + remap*4] lands at seg 0x114b
   (code-area data segment), NOT within the captured DGROUP snapshot.  It cannot
   be dereferenced from `dg` at host time.  We fall back to the decomp-derived
   anim_tables.json A-map (embedded as anim_a_desc[]).  The channel records captured
   in FRM3 confirm: chan_a[0] cell=40 yoff=5 frame=64, matching anim_a_desc[1].
   posA X/Y from dg[0xf4+cell*4] / dg[0xf6+cell*4] matches anim_tables.json posA
   exactly (verified for all 48 cells).

   ERASE omitted: draw_anim_channels_a calls restore_bg_view (bg-tile erase) before
   each blit.  In our composite, bg is built first and never needs erasing; skip it.

   render_player_view: modeled as the same blit_sprite (entity_blit_object) call —
   identical to the P1 approach (Task 5, bg+C+P1 validated).  VALIDATE EMPIRICALLY:
   if layer A does not match with blit_sprite alone, investigate further.
   ----------------------------------------------------------------------- */
void entity_draw_layer_a(u8 __huge *planes, const u8 __far *bum,
                         const u8 __far *dg, u8 __huge *bank,
                         u32 bank_base_lin, const sprite_view *view)
{
    u8  obj[OBJ_SIZE];
    u16 row;
    u16 col;

    for (row = 0; row < 6u; row++) {
        for (col = 0; col < 8u; col++) {
            u16 cell = row * 8u + col;
            u8  cv;
            u16 frame;
            s16 yoff;
            u16 pos_x;
            u16 pos_y;

            /* Layer-A grid at bum[0x00 + cell] (tilemap[0x00+cell] in the engine). */
            cv = bum[0x00u + cell];

            /* Engine: if cv==0 continue; if remap==0 continue. */
            if (!anim_desc_valid_a(cv)) {
                continue;
            }

            frame = anim_a_desc[cv].frame;
            yoff  = anim_a_desc[cv].yoff;

            /* draw_anim_channels_a (1000:165e) frame guard. */
            if ((frame & 0x200u) != 0u) {
                continue;
            }

            memset(obj, 0, sizeof(obj));

            /* Seed frametable far ptr from engine's p1_sprite obj (same obj used
               for layer A as for layer C and P1). */
            {
                u16 ftbl_off = dg_rd16(dg, (u16)(DG_P1_OBJ + OBJ_FTBL_OFF));
                u16 ftbl_seg = dg_rd16(dg, (u16)(DG_P1_OBJ + OBJ_FTBL_SEG));
                ent_wr16(obj, OBJ_FTBL_OFF, ftbl_off);
                ent_wr16(obj, OBJ_FTBL_SEG, ftbl_seg);
            }

            /* posA position from dg (draw_anim_channels_a: 0xf4/0xf6 tables). */
            pos_x = dg_rd16(dg, (u16)(DG_POSA_X_BASE + cell * 4u));
            pos_y = dg_rd16(dg, (u16)(DG_POSA_Y_BASE + cell * 4u));

            ent_wr16(obj, OBJ_X,         pos_x);
            ent_wr16(obj, OBJ_Y,         (u16)((s16)pos_y + yoff));
            ent_wr16(obj, OBJ_FRAME_IDX, frame);
            obj[OBJ_FLAGS] = OBJ_VISIBLE;

            entity_blit_object(planes, obj, bank, bank_base_lin, view);
        }
    }
}

/* -----------------------------------------------------------------------
   entity_draw_layer_b — static placement port of the layer-B loop inside
   spawn_and_draw_level_entities (1000:2a78), draw side from draw_anim_channels_b
   (1000:17c7).

   Engine flow (abbreviated, per-cell):
     cv = tilemap[0x30 + cell]     (bum[0x30 + cell])
     if cv == 0 OR grid_col == 7: continue
     remap = dg[cv + 0x4086]
     if remap == 0: continue
     descfar = far ptr at dg[0x40a6 + remap*4] -> {word0=yoff, word1=frame}
     obj[+0] = dg[0x3f4 + cell*4]  (posB X)
     obj[+2] = dg[0x3f6 + cell*4] + yoff  (posB Y + yoff)
     if ((frame & 0x200) == 0):
       obj[+4] = frame + 0xf1      (layer-B frame bias)
       blit_sprite(p1_sprite @0x792e)
     render_player_view(...)       (modeled as same blit, see layer-A note)

   DESCRIPTOR SOURCING: same reasoning as layer A — far ptr outside dg; fall back
   to anim_b_desc[] from anim_tables.json.

   ERASE: draw_anim_channels_b additionally erases a shadow/mask (multiple
   restore_bg_view calls with separate data ptrs).  All erase paths omitted for
   the same reason as layer A: composite builds bg first.

   UNVALIDATED on level 1: level 1 has 0 layer-B cells (bum[0x30..0x5f] all zero),
   so entity_draw_layer_b is a structural no-op.  The positive blit path (including
   the 0xf1 frame bias and the col==7 guard) is UNVALIDATED until Task 7 (richer level).
   ----------------------------------------------------------------------- */
void entity_draw_layer_b(u8 __huge *planes, const u8 __far *bum,
                         const u8 __far *dg, u8 __huge *bank,
                         u32 bank_base_lin, const sprite_view *view)
{
    u8  obj[OBJ_SIZE];
    u16 row;
    u16 col;

    for (row = 0; row < 6u; row++) {
        for (col = 0; col < 8u; col++) {
            u16 cell = row * 8u + col;
            u8  cv;
            u16 frame;
            s16 yoff;
            u16 pos_x;
            u16 pos_y;

            /* Engine guard: skip col 7 for layer B. */
            if (col == 7u) {
                continue;
            }

            /* Layer-B grid at bum[0x30 + cell]. */
            cv = bum[0x30u + cell];

            if (!anim_desc_valid_b(cv)) {
                continue;
            }

            frame = anim_b_desc[cv].frame;
            yoff  = anim_b_desc[cv].yoff;

            /* draw_anim_channels_b (1000:17c7) frame guard (same 0x200 bit). */
            if ((frame & 0x200u) != 0u) {
                continue;
            }

            memset(obj, 0, sizeof(obj));

            /* Seed frametable far ptr from p1_sprite obj (same obj for all layers). */
            {
                u16 ftbl_off = dg_rd16(dg, (u16)(DG_P1_OBJ + OBJ_FTBL_OFF));
                u16 ftbl_seg = dg_rd16(dg, (u16)(DG_P1_OBJ + OBJ_FTBL_SEG));
                ent_wr16(obj, OBJ_FTBL_OFF, ftbl_off);
                ent_wr16(obj, OBJ_FTBL_SEG, ftbl_seg);
            }

            /* posB position from dg (draw_anim_channels_b: 0x3f4/0x3f6 tables). */
            pos_x = dg_rd16(dg, (u16)(DG_POSB_X_BASE + cell * 4u));
            pos_y = dg_rd16(dg, (u16)(DG_POSB_Y_BASE + cell * 4u));

            /* Layer-B frame bias: draw_anim_channels_b assigns frame + 0xf1. */
            ent_wr16(obj, OBJ_X,         pos_x);
            ent_wr16(obj, OBJ_Y,         (u16)((s16)pos_y + yoff));
            ent_wr16(obj, OBJ_FRAME_IDX, (u16)(frame + LAYER_B_FRAME_BIAS));
            obj[OBJ_FLAGS] = OBJ_VISIBLE;

            entity_blit_object(planes, obj, bank, bank_base_lin, view);
        }
    }
}

/* -----------------------------------------------------------------------
   entity_draw_p2 — faithful port of draw_p2_sprite (1000:1cea).
   Guard: if p2_cell == -1, P2 is absent — draw nothing.
   Otherwise: obj[+0]=pixel_x, obj[+2]=pixel_y, obj[+4]=frame_base+move_anim;
   blit via the p2_sprite obj (DGROUP 0x795a).
   Frametable far ptr seeded from dg[0x795a+6..9] (engine's p2_sprite obj).

   DEAD/GUARDED PATH (level 1): p2_cell == -1 on level 1 (single-player),
   so the positive draw path is UNVALIDATED on level 1 (P2 absent).
   The guard (p2_cell == -1 → return) IS validated: planes unchanged.
   Full positive-path validation is deferred to a P2-present level (Task 6).
   ----------------------------------------------------------------------- */
void entity_draw_p2(u8 __huge *planes, const u8 __far *dg,
                    u16 pixel_x, u16 pixel_y, u16 move_anim,
                    u16 frame_base, s8 p2_cell,
                    u8 __huge *bank, u32 bank_base_lin,
                    const sprite_view *view)
{
    u8  obj[OBJ_SIZE];

    /* draw_p2_sprite (1000:1cea): if p2_cell == -1, P2 absent — return. */
    if (p2_cell == (s8)(-1)) {
        return;
    }

    memset(obj, 0, sizeof(obj));

    /* Seed frametable far ptr from engine's p2_sprite obj at DGROUP:0x795a[+6..9].
       P2 uses its own sprite obj struct (distinct from p1_sprite at 0x792e). */
    {
        u16 ftbl_off = dg_rd16(dg, (u16)(DG_P2_OBJ + OBJ_FTBL_OFF));
        u16 ftbl_seg = dg_rd16(dg, (u16)(DG_P2_OBJ + OBJ_FTBL_SEG));
        ent_wr16(obj, OBJ_FTBL_OFF, ftbl_off);
        ent_wr16(obj, OBJ_FTBL_SEG, ftbl_seg);
    }

    /* draw_p2_sprite field writes:
         *puVar1 = p2_pixel_x;
         sprite_fields[1] = p2_pixel_y;
         sprite_fields[2] = p2_frame_base + p2_move_anim; */
    ent_wr16(obj, OBJ_X,         pixel_x);
    ent_wr16(obj, OBJ_Y,         pixel_y);
    ent_wr16(obj, OBJ_FRAME_IDX, (u16)(frame_base + move_anim));
    obj[OBJ_FLAGS] = OBJ_VISIBLE;

    entity_blit_object(planes, obj, bank, bank_base_lin, view);
}
