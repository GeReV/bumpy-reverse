#include "sprite_chain.h"

/* See sprite_chain.h.  Field offsets/arithmetic mirror the Ghidra decomp of
   sprite_blit_object_list (0e48) / _clip (0f50) / _setup (103d).  Signed (s16)
   arithmetic is significant: cell col/row go negative for off-screen sprites and
   the >>3 is an arithmetic shift. */

static s16 rd_s16(const u8 __far *p) { return (s16)(p[0] | (p[1] << 8)); }
static u16 rd_u16(const u8 __far *p) { return (u16)(p[0] | (p[1] << 8)); }

static void wr16(u8 *d, u16 v) { d[0] = (u8)v; d[1] = (u8)(v >> 8); }

int sprite_blit_build_desc(const u8 __far *obj, const sprite_view *view, u8 *desc)
{
    u8 flags = obj[0x0a];
    s16 x;
    s16 col;
    s16 row;
    u8 shift;
    s16 width = (s16)rd_u16(obj + 0x10);      /* width in words */
    s16 height = (s16)rd_u16(obj + 0x12);
    s16 half_w = (s16)((u16)width >> 1);      /* columns = width/2 */
    s16 iv4;

    /* clip state */
    s16 bc9;       /* horizontal source/dest clip (left) */
    s16 bcf;       /* horizontal source skip (cols) */
    s16 bcb;       /* vertical clip (top, rows) */
    s16 bcd;       /* vertical source skip (rows) */
    u8 bec = 0;    /* clip flags */
    s16 be5;       /* visible width (cols) */
    s16 be7;       /* visible height (rows) */
    s16 iVar1;
    u16 dst_off;   /* 25a98 */
    u16 src_off_add; /* 25a96 */

    if ((flags & 0x80) == 0) {
        return 0;                              /* not visible */
    }

    /* --- object_list (0e48): screen cell + cull --- */
    if ((flags & 0x20) == 0) {
        x = (s16)(rd_s16(obj + 0x00) - rd_s16(obj + 0x14));
    } else {
        x = (s16)(rd_s16(obj + 0x00) + width * -4 + rd_s16(obj + 0x14));
    }
    if ((flags & 1) != 0) {
        x = (s16)((x + 4) & 0xfff8);
    }
    shift = (u8)(x & 7);
    col = (s16)(x >> 3);
    row = (s16)(rd_s16(obj + 0x02) - rd_s16(obj + 0x16));

    iv4 = (s16)(col + half_w);
    if (!(row < view->bottom && col < view->right && iv4 >= 0 &&
          view->left <= iv4 && (s16)(row + height) >= 0 &&
          view->top <= (s16)(row + height))) {
        return 0;                              /* culled */
    }

    /* --- clip (0f50) --- */
    bc9 = 0;
    bcf = (s16)(view->left - col);
    if (bcf < 0) {
        bc9 = (s16)-bcf;
        bcf = 0;
    }
    if (bcf != 0) {
        bec = 2;
    }
    if ((s16)(view->right - col) < 0 ||
        (s16)((view->right - col) - (half_w + 1)) < 0) {
        be5 = (s16)((view->right - bcf) - col);
        bec |= 1;
    } else {
        be5 = (s16)(half_w - bcf);
        if (be5 < 0) {
            be5 = 0;
        }
    }
    bcb = 0;
    bcd = (s16)(view->top - row);
    if (bcd < 0) {
        bcb = (s16)-bcd;
        bcd = 0;
    }
    iVar1 = (s16)((view->top + view->height) - row);
    if (iVar1 < 0 || (s16)(iVar1 - height) < 0) {
        be7 = (s16)(((view->top - bcd) + view->height) - row);
    } else {
        be7 = (s16)(height - bcd);
    }
    iVar1 = 0;
    if ((s16)(bcb + view->top) != 0) {
        iVar1 = (s16)(((bcb + view->top) & 0xff) * 0x28);
    }
    dst_off = (u16)(iVar1 + view->data_off + bc9 + view->left);

    /* --- setup (103d): assemble descriptor --- */
    src_off_add = 0;
    if (bcd != 0) {
        src_off_add = (u16)((bcd & 0xff) * (u8)(width * 2));
    }
    src_off_add = (u16)(src_off_add + bcf * 4);

    desc[0x00] = 0; desc[0x01] = 0; /* set below */
    wr16(desc + 0x00, (u16)(rd_u16(obj + 0x0c) + src_off_add));  /* src off */
    wr16(desc + 0x02, rd_u16(obj + 0x0e));                       /* src seg */
    desc[0x04] = 0; desc[0x05] = 0; desc[0x06] = 0; desc[0x07] = 0;
    wr16(desc + 0x08, dst_off);                                  /* dst off */
    wr16(desc + 0x0a, view->data_seg);                          /* dst seg */
    wr16(desc + 0x0c, (u16)((u16)half_w & 0xff));               /* full width */
    wr16(desc + 0x0e, 0x28);                                     /* dst stride */
    wr16(desc + 0x10, (u16)be5);                                /* cols drawn */
    wr16(desc + 0x12, (u16)be7);                                /* rows */
    desc[0x14] = 0;
    desc[0x15] = 0;                                              /* sel */
    desc[0x16] = shift;
    desc[0x17] = bec;                                            /* clip flags */
    return 1;
}
