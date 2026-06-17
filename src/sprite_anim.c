#include "sprite_anim.h"

/* See sprite_anim.h.  Field offsets / arithmetic mirror the Ghidra decomp of
   prepare_sprite_frames (1cec:2ded), non-expansion path. */

static u16 obj_rd16(const u8 __far *p) { return (u16)(p[0] | (p[1] << 8)); }
static void obj_wr16(u8 __far *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

static u16 bank_rd16(u8 __huge *bank, u32 off)
{
    return (u16)(bank[off] | (bank[off + 1] << 8));
}

/* linear address of a far ptr (seg:off) stored little-endian at obj+`at` */
static u32 far_lin(const u8 __far *obj, u16 at)
{
    u16 off = obj_rd16(obj + at);
    u16 seg = obj_rd16(obj + at + 2);
    return ((u32)seg << 4) + off;
}

void sprite_prepare_frame(u8 __far *obj, u8 __huge *bank, u32 bank_base_lin)
{
    u16 frame_idx = obj_rd16(obj + 4);
    u32 table_lin = far_lin(obj, 6);                  /* frame_table far ptr */
    u32 ent_off = (table_lin - bank_base_lin) + (u32)frame_idx * 4u;
    u32 frame_lin;
    u32 fb;                                            /* frame bank offset */
    u16 count;
    u16 i;
    u8 ctrl;

    /* select: copy the 4-byte frame far ptr from the table into obj+0xc */
    obj[0x0c] = bank[ent_off];
    obj[0x0d] = bank[ent_off + 1];
    obj[0x0e] = bank[ent_off + 2];
    obj[0x0f] = bank[ent_off + 3];

    frame_lin = ((u32)obj_rd16(obj + 0x0e) << 4) + obj_rd16(obj + 0x0c);
    if (frame_lin == 0) {
        obj_wr16(obj + 0x0a, 0);
        return;
    }
    fb = frame_lin - bank_base_lin;

    /* copy the frame header (BE-swapped at load time, so read straight) */
    obj_wr16(obj + 0x12, bank_rd16(bank, fb - 2));    /* height */
    obj_wr16(obj + 0x10, bank_rd16(bank, fb - 4));    /* width  */
    obj_wr16(obj + 0x14, bank_rd16(bank, fb - 6));    /* X anchor */
    obj_wr16(obj + 0x16, bank_rd16(bank, fb - 8));    /* Y anchor */

    count = bank_rd16(bank, fb - 0x0c);
    if (count > 2) {
        count = 3;
    }
    obj_wr16(obj + 0x18, count);

    /* copy `count` sub-header entries (3 words each), source walking backwards */
    {
        u32 src = fb - 0x0c;
        u8 __far *dst = obj + 0x1a;
        for (i = 0; i < count; i++) {
            obj_wr16(dst + 0, bank_rd16(bank, src - 2));
            obj_wr16(dst + 2, bank_rd16(bank, src - 4));
            obj_wr16(dst + 4, bank_rd16(bank, src - 6));
            dst += 6;
            src -= 6;
        }
    }

    ctrl = bank[fb - 0x0a];
    obj[0x0b] = ctrl;
    /* ctrl & 0x40 expansion path intentionally omitted (never taken here) */
}
