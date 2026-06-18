#include <string.h>
#include "entity.h"
#include "sprite_anim.h"
#include "sprite_chain.h"
#include "sprite_blit.h"

/* See entity.h.  Field offsets / arithmetic mirror the Ghidra decomp of the
   layer-C section inlined in spawn_and_draw_level_entities (1000:2a78, lines
   829-838 of the decomp body).

   The engine loop:
     for grid_row 0..5, grid_col 0..7:
       cell = grid_row*8 + grid_col
       cv = *(byte *)((int)tilemap + 0x60 + cell)
       if (cv == 0) continue
       *p1_sprite         = posC_X[cell]       // dg[0x274 + cell*4]
       ((u16*)p1_sprite)[1] = posC_Y[cell]     // dg[0x276 + cell*4]
       p1_sprite[2]       = cv + 0x179         // frame index
       blit_sprite(0x792e, 0x203b)             // -> prepare -> build_desc -> blit

   Here blit_sprite is expanded into the three validated pipeline stages.
   The frame-table far ptr (obj+6/obj+8) is read from the engine's p1_sprite
   struct as populated by sprite_prepare_frame — sprite_prepare_frame sets obj+6/8
   to the global frametable pointer before looking up the frame.  In the engine,
   the frametable far ptr at obj+6 is pre-loaded during level init; for the
   host reconstruction we read it from the captured p1_sprite obj in the oracle.

   HOWEVER: sprite_prepare_frame reads obj[6..9] to find the frame table far ptr
   (frametable linear = seg<<4 + off), and the frame table IS in the bank.  The
   engine's p1_sprite obj struct (captured in FRM3) has obj[6..9] already set to
   the correct frametable far ptr.  We therefore seed the object's frametable far
   ptr (obj+6) from the dg snapshot's p1_sprite value for correctness.

   The dg snapshot includes the full DGROUP; the p1_sprite obj struct lives at
   DGROUP offset 0x792e.  We copy obj[6..9] (4 bytes: off+seg of frametable) from
   dg[0x792e+6..9] into our per-cell work object before calling sprite_prepare_frame.
*/

/* DG offsets for posC table and p1_sprite obj struct */
#define DG_POSC_BASE  0x0274u   /* first XY pair; cell n at 0x274 + n*4 */
#define DG_P1_OBJ     0x792eu   /* p1_sprite obj struct start in DGROUP */

/* p1_sprite obj field offsets (bytes within the 0x18-byte struct) */
#define OBJ_X         0x00u     /* s16 pixel X */
#define OBJ_Y         0x02u     /* s16 pixel Y */
#define OBJ_FRAME_IDX 0x04u     /* u16 frame index */
#define OBJ_FTBL_OFF  0x06u     /* u16 frametable far ptr: offset half */
#define OBJ_FTBL_SEG  0x08u     /* u16 frametable far ptr: segment half */
#define OBJ_FLAGS     0x0au     /* u8 flags: 0x80=visible 0x20=hflip 0x01=align */

#define OBJ_VISIBLE   0x80u     /* flags bit: visible */

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

void entity_draw_layer_c(u8 __huge *planes, const u8 __far *bum,
                         const u8 __far *dg, u8 __huge *bank,
                         u32 bank_base_lin, const sprite_view *view)
{
    u8  desc[0x18];
    u8  obj[OBJ_SIZE];
    u16 row;
    u16 col;

    for (row = 0; row < 6u; row++) {
        for (col = 0; col < 8u; col++) {
            u16 cell = row * 8u + col;
            u8  cv;
            u16 voff;
            u16 dst_stride;
            u16 full_w;
            u16 cols;
            u16 rows;
            u8  shift;
            u8  clip_flags;

            /* Zero the work buffer each iteration (matches anim_ctest.c pattern:
               memset(work,0,sizeof(work)) before populating). */
            memset(obj, 0, sizeof(obj));

            /* Seed obj[6..9] (frametable far ptr) from the captured p1_sprite in dg.
               This is constant across all cells — the frametable ptr is set at level
               init and doesn't change per-cell.  Mirrors what blit_sprite does: it
               blits using the p1_sprite struct at 0x792e which already carries the
               frametable ptr from level init.  The prepare step needs it to resolve
               the frame index into a frame far ptr.  Must be re-seeded after each
               memset since the zero wipes bytes 6..9. */
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

            /* --- Build sprite object (mirrors engine: write X, Y, frame, flags) --- */
            ent_wr16(obj, OBJ_X,         dg_rd16(dg, (u16)(DG_POSC_BASE + cell * 4u)));
            ent_wr16(obj, OBJ_Y,         dg_rd16(dg, (u16)(DG_POSC_BASE + cell * 4u + 2u)));
            ent_wr16(obj, OBJ_FRAME_IDX, (u16)(cv + 0x179u));
            obj[OBJ_FLAGS] = OBJ_VISIBLE;

            /* --- Stage 1: sprite_prepare_frame (mirrors anim_ctest.c pattern) ---
               Fills obj+0xb (ctrl), obj+0xc/0xe (prepared ptr), obj+0x10 (width),
               obj+0x12 (height), obj+0x14/0x16 (anchors) from the bank frame data. */
            sprite_prepare_frame(obj, bank, bank_base_lin);

            /* --- Stage 2: sprite_blit_build_desc (mirrors chain_ctest.c pattern) ---
               Returns 1 if visible and overlaps the view, 0 if culled. */
            if (!sprite_blit_build_desc(obj, view, desc)) {
                continue;
            }

            /* --- Stage 3: sprite_blit_planar_vga (mirrors blit_ctest.c unpacking) ---
               voff   = desc dst seg:off converted to a plane-buffer byte offset:
                        (seg<<4 + off) - 0xA0000.  With data_seg=0xa000 and data_off=0,
                        the dst_off built by sprite_blit_build_desc is already relative
                        to 0xa0000 (the VGA base), so voff == desc[0x08..0x09] directly.
               Unpack remaining fields from descriptor per chain_ctest.c / blit_ctest.c. */
            {
                u32 dlin = (u32)dg_rd16((const u8 __far *)desc, 0x0au) * 16u
                         + (u32)dg_rd16((const u8 __far *)desc, 0x08u);
                voff       = (u16)(dlin - 0xA0000UL);
            }
            dst_stride = (u16)((u16)desc[0x0eu] | ((u16)desc[0x0fu] << 8));
            full_w     = (u16)((u16)desc[0x0cu] | ((u16)desc[0x0du] << 8));
            cols       = (u16)((u16)desc[0x10u] | ((u16)desc[0x11u] << 8));
            rows       = (u16)((u16)desc[0x12u] | ((u16)desc[0x13u] << 8));
            shift      = desc[0x16u];
            clip_flags = desc[0x17u];

            {
                /* src far ptr from descriptor [0..3]: off=desc[0..1], seg=desc[2..3] */
                u32 src_lin = (u32)((u16)desc[2] | ((u16)desc[3] << 8)) * 16u
                            + (u32)((u16)desc[0] | ((u16)desc[1] << 8));
                const u8 __far *src = (const u8 __far *)(bank + (src_lin - bank_base_lin));
                sprite_blit_planar_vga(planes, src, voff, dst_stride, full_w,
                                       cols, rows, shift, clip_flags);
            }
        }
    }
}
