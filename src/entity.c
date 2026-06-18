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
