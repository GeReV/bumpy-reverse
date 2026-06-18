#include "sprite_chain.h"

/* See sprite_chain.h.  Field offsets/arithmetic mirror the Ghidra decomp of
   sprite_blit_object_list (0e48) / _clip (0f50) / _setup (103d).  Signed (s16)
   arithmetic is significant: cell col/row go negative for off-screen sprites and
   the >>3 is an arithmetic shift. */

static s16 rd_s16(const u8 __far *p) { return (s16)(p[0] | (p[1] << 8)); }
static u16 rd_u16(const u8 __far *p) { return (u16)(p[0] | (p[1] << 8)); }

static void wr16(u8 *d, u16 v) { d[0] = (u8)v; d[1] = (u8)(v >> 8); }

/* Intermediate state that mirrors the DGROUP globals used by _clip and _setup.
   In the original engine these are ds-relative word/byte RAM cells; here they are
   locals threaded through the three reconstructed functions.

   DGROUP symbol -> field name -> desc[] slot (where applicable):
     iRam00026bbd (view->left)    — passed in via sprite_view
     iRam00026bbf (view->right)   — passed in via sprite_view
     iRam00026bc1 (view->top)     — passed in via sprite_view
     iRam00026bc3 (view->bottom)  — passed in via sprite_view
     iRam00026bc7 (view->height)  — passed in via sprite_view
     cur_sprite_data_off          — view->data_off
     cur_sprite_data_seg          — view->data_seg
     uRam00013216 (shift)         — computed in object_list, passed to setup
     iRam00026bd1 (cell_x / col)  — computed in object_list, used in clip
     iRam00026bd3 (cell_y / row)  — computed in object_list, used in clip
     iRam00026bc9 (bc9)           — clip output: left col-skip (cells)
     iRam00026bcf (bcf)           — clip output: left col margin to skip in dst
     iRam00026bcb (bcb)           — clip output: top row-skip (rows)
     iRam00026bcd (bcd)           — clip output: top row margin to skip in dst
     bRam00026bec (bec)           — clip output -> desc[0x17] clip flags
     iRam00026be5 (be5)           — clip output -> desc[0x10] cols drawn
     uRam00026be7 (be7)           — clip output -> desc[0x12] rows drawn
     iRam00025a98 (dst_off)       — clip output -> desc[0x08]
     iRam00025a96 (src_off_add)   — setup local  -> added to src ptr
*/
typedef struct {
    /* object_list outputs consumed by clip */
    s16 col;     /* iRam00026bd1 */
    s16 row;     /* iRam00026bd3 */
    u8  shift;   /* uRam00013216 */
    /* clip outputs consumed by setup */
    s16 bc9;     /* iRam00026bc9  left skip (cols)  */
    s16 bcf;     /* iRam00026bcf  left dst margin   */
    s16 bcb;     /* iRam00026bcb  top skip (rows)   */
    s16 bcd;     /* iRam00026bcd  top dst margin    */
    u8  bec;     /* bRam00026bec  clip flags        */
    s16 be5;     /* iRam00026be5  visible cols      */
    s16 be7;     /* uRam00026be7  visible rows      */
    u16 dst_off; /* iRam00025a98                    */
} blit_state;

/* sprite_blit_clip (1cec:0f50)
   Compute left/right/top/bottom clip margins + visible width/height vs the view
   rect; return (visible_height | 1) when any column visible, else 0.
   unaff_DI in the original = the object pointer passed here explicitly. */
static int sprite_blit_clip(const u8 __far *obj, const sprite_view *view,
                             blit_state *st)
{
    s16 iVar1;
    s16 width  = (s16)rd_u16(obj + 0x10);
    s16 height = (s16)rd_u16(obj + 0x12);
    s16 half_w = (s16)((u16)width >> 1);

    st->bec = 0;
    st->bc9 = 0;
    st->bcf = (s16)(view->left - st->col);
    if (st->bcf < 0) {
        st->bc9 = (s16)-st->bcf;
        st->bcf = 0;
    }
    if (st->bcf != 0) {
        st->bec = 2;
    }
    if ((s16)(view->right - st->col) < 0 ||
        (s16)((view->right - st->col) - (half_w + 1)) < 0) {
        st->be5 = (s16)((view->right - st->bcf) - st->col);
        st->bec |= 1;
    } else {
        st->be5 = (s16)(half_w - st->bcf);
        if (st->be5 < 0) {
            st->be5 = 0;
        }
    }
    st->bcb = 0;
    st->bcd = (s16)(view->top - st->row);
    if (st->bcd < 0) {
        st->bcb = (s16)-st->bcd;
        st->bcd = 0;
    }
    iVar1 = (s16)((view->top + view->height) - st->row);
    if (iVar1 < 0 || (s16)(iVar1 - height) < 0) {
        st->be7 = (s16)(((view->top - st->bcd) + view->height) - st->row);
    } else {
        st->be7 = (s16)(height - st->bcd);
    }
    iVar1 = 0;
    if ((s16)(st->bcb + view->top) != 0) {
        iVar1 = (s16)(((st->bcb + view->top) & 0xff) * 0x28);
    }
    st->dst_off = (u16)(iVar1 + view->data_off + st->bc9 + view->left);

    if (st->be5 != 0 && st->be5 >= 0 && st->be7 != 0 &&
        st->be7 >= 0 && st->be7 < 100) {
        return (int)(st->be7 | 1);
    }
    return 0;
}

/* sprite_blit_setup (1cec:103d)
   Compute source row offset, then fill the blit descriptor.
   In the original engine this ends with a tail-call to sprite_blit_planar_vga;
   in this reconstruction it fills `desc` instead (the composite calls the blitter
   separately from the filled descriptor — see RECONSTRUCTION FIDELITY in
   sprite_chain.h).
   unaff_DI in the original = the object pointer passed here explicitly. */
static void sprite_blit_setup(const u8 __far *obj, const sprite_view *view,
                               blit_state *st, u8 *desc)
{
    u16 src_off_add; /* iRam00025a96 */
    s16 width  = (s16)rd_u16(obj + 0x10);
    s16 half_w = (s16)((u16)width >> 1);

    sprite_blit_clip(obj, view, st);

    src_off_add = 0;
    if (st->bcd != 0) {
        src_off_add = (u16)((st->bcd & 0xff) * (u8)(width * 2));
    }
    src_off_add = (u16)(src_off_add + st->bcf * 4);

    /* Fill the 0x18-byte descriptor (DGROUP block 0x26bd5):
         iRam00026bd5 (src off)   -> desc[0x00]   uRam00026bd7 (src seg)  -> desc[0x02]
         iRam00026bdd (dst off)   -> desc[0x08]   uRam00026bdf (dst seg)  -> desc[0x0a]
         uRam00026be1 (cols full) -> desc[0x0c]   uRam00026be3 (stride)   -> desc[0x0e]
         iRam00026be5 (vis cols)  -> desc[0x10]   uRam00026be7 (vis rows) -> desc[0x12]
                                                  bRam00026bea (sel)      -> desc[0x15]
                                                  bRam00026beb (shift)    -> desc[0x16]
                                                  bRam00026bec (clip)     -> desc[0x17]  */
    desc[0x00] = 0; desc[0x01] = 0;
    wr16(desc + 0x00, (u16)(rd_u16(obj + 0x0c) + src_off_add));  /* src off */
    wr16(desc + 0x02, rd_u16(obj + 0x0e));                       /* src seg */
    desc[0x04] = 0; desc[0x05] = 0; desc[0x06] = 0; desc[0x07] = 0;
    wr16(desc + 0x08, st->dst_off);                              /* dst off */
    wr16(desc + 0x0a, view->data_seg);                           /* dst seg */
    wr16(desc + 0x0c, (u16)((u16)half_w & 0xff));                /* full width cols */
    wr16(desc + 0x0e, 0x28);                                     /* dst stride */
    wr16(desc + 0x10, (u16)st->be5);                             /* cols drawn */
    wr16(desc + 0x12, (u16)st->be7);                             /* rows drawn */
    desc[0x14] = 0;
    desc[0x15] = 0;                                              /* sel */
    desc[0x16] = st->shift;                                      /* shift */
    desc[0x17] = st->bec;                                        /* clip flags */
    /* Original: sprite_blit_planar_vga(); — called by composite separately */
}

/* sprite_blit_object_list (1cec:0e48) — single-object body.
   In the engine this iterates the far-pointer object list (puRam00026bb9) and calls
   sprite_blit_setup per visible, in-bounds object.  In this reconstruction the list
   walk is handled externally (composite/entity layer); this function encodes the
   per-object body: flags decode, screen cell x/y, view-bounds cull, then setup.
   Returns 1 if the descriptor was filled, 0 if the object was hidden or culled. */
static int sprite_blit_object_list(const u8 __far *obj, const sprite_view *view,
                                    u8 *desc)
{
    u8 flags = obj[0x0a];
    s16 x;
    s16 width  = (s16)rd_u16(obj + 0x10);
    s16 height = (s16)rd_u16(obj + 0x12);
    s16 half_w = (s16)((u16)width >> 1);
    s16 iv4;
    blit_state st;

    if ((flags & 0x80) == 0) {
        return 0;                              /* not visible */
    }

    /* Compute screen pixel X, then derive cell column + sub-cell shift */
    if ((flags & 0x20) == 0) {
        x = (s16)(rd_s16(obj + 0x00) - rd_s16(obj + 0x14));
    } else {
        x = (s16)(rd_s16(obj + 0x00) + width * -4 + rd_s16(obj + 0x14));
    }
    if ((flags & 1) != 0) {
        x = (s16)((x + 4) & 0xfff8);
    }
    st.shift = (u8)(x & 7);
    st.col   = (s16)(x >> 3);
    st.row   = (s16)(rd_s16(obj + 0x02) - rd_s16(obj + 0x16));

    /* View-bounds cull (iRam00026bbd/bbf/bc1/bc3/bc7) */
    iv4 = (s16)(st.col + half_w);
    if (!(st.row < view->bottom && st.col < view->right && iv4 >= 0 &&
          view->left <= iv4 && (s16)(st.row + height) >= 0 &&
          view->top <= (s16)(st.row + height))) {
        return 0;                              /* culled */
    }

    sprite_blit_setup(obj, view, &st, desc);
    return 1;
}

/* Public entry — thin wrapper that runs the single-object body of object_list
   then setup (via clip).  Preserves the validated public signature so callers
   (composite_ctest.c, entity.c via entity_blit_object) are unaffected. */
int sprite_blit_build_desc(const u8 __far *obj, const sprite_view *view, u8 *desc)
{
    return sprite_blit_object_list(obj, view, desc);
}
