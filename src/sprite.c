/* sprite.c — BUMSPJEU bank load transform.

   Faithful C port of the engine's sprite_bank_relocate_frames (1cec:0c34) and
   sprite_frame_transform (1cec:0c77).  See sprite.h.  The original works on
   normalized far pointers (seg:off) reconstructed from the bank's big-endian
   frame-offset table; this port mirrors that with normalized far pointers into
   a flat far bank buffer. */

#include "sprite.h"

/* Normalize bank+off to a far pointer whose offset has >=0x10 of head-room, so
   the 12-byte header access at pix[-12..-1] (and forward pixel walk) stay within
   the segment without 16-bit offset wrap. */
static u8 __far *bank_ptr(u8 __far *bank, u32 off)
{
    u32 lin = ((u32)FP_SEG(bank) << 4) + (u32)FP_OFF(bank) + off;
    return (u8 __far *)MK_FP((u16)((lin >> 4) - 1u), (u16)((lin & 0xFu) + 0x10u));
}

/* 16-bit interleaved bit-reverse used by the CGA (palette_mode==0) pixel path:
   literal transliteration of the sprite_frame_transform expression over the two
   block bytes (lo,hi).  NOTE: the CGA path is not exercised by the DOS/VGA build
   (palette_mode==2) and is not covered by the engine oracle. */
static u16 sprite_bitrev_block(u8 lo, u8 hi)
{
    s8  cl = (s8)lo;
    s8  ch = (s8)hi;
    u16 v;
    v = (u16)((cl & 0x80) != 0);
    v = (v << 1) | (u16)((hi & 0x80) != 0);   /* (iVar3<0) = top bit of the word = hi top */
    v = (v << 1) | (u16)((s8)(cl << 1) < 0);
    v = (v << 1) | (u16)((s8)(ch << 1) < 0);
    v = (v << 1) | (u16)((s8)(cl << 2) < 0);
    v = (v << 1) | (u16)((s8)(ch << 2) < 0);
    v = (v << 1) | (u16)((s8)(cl << 3) < 0);
    v = (v << 1) | (u16)((s8)(ch << 3) < 0);
    v = (v << 1) | (u16)((s8)(cl << 4) < 0);
    v = (v << 1) | (u16)((s8)(ch << 4) < 0);
    v = (v << 1) | (u16)((s8)(cl << 5) < 0);
    v = (v << 1) | (u16)((s8)(ch << 5) < 0);
    v = (v << 1) | (u16)((s8)(cl << 6) < 0);
    v = (v << 1) | (u16)((s8)(ch << 6) < 0);
    v = (v << 1) | (u16)((s8)(cl << 7) < 0);
    v = (u16)(v << 1);
    /* low bit folds in (ch<<7) top bit (CONCAT11 ... | (cVar6<<7)<0). */
    return (u16)(v | (u16)((u8)(hi << 7) >> 7));
}

/* Transform one frame in place: pix points at the pixel data; the 12-byte header
   is at pix[-12..-1].  Mirrors sprite_frame_transform (1cec:0c77). */
static void sprite_frame_transform(u8 __far *pix, u8 palette_mode)
{
    u8  b0, b1;
    u16 w, h;
    u16 hdr5;
    u8 __far *p;
    u16 blocks, rows;
    u8  o1, o2, o3, o4, o5, o6;

    /* word[-1] (h): byte-swap; gate on original high byte and the swapped value. */
    b0 = pix[-2];                 /* low byte of the LE word = BE high byte of h    */
    b1 = pix[-1];
    pix[-2] = b1; pix[-1] = b0;    /* swap -> proper value                          */
    h = (u16)(((u16)b0 << 8) | b1);
    if ((s8)b0 >= 0 && h != 0) {
        /* byte-swap header words [-2],[-3],[-4],[-6] and [-5] */
        b0 = pix[-4];  b1 = pix[-3];  pix[-4] = b1;  pix[-3] = b0;   /* w */
        b0 = pix[-6];  b1 = pix[-5];  pix[-6] = b1;  pix[-5] = b0;
        b0 = pix[-8];  b1 = pix[-7];  pix[-8] = b1;  pix[-7] = b0;
        b0 = pix[-12]; b1 = pix[-11]; pix[-12] = b1; pix[-11] = b0;
        b0 = pix[-10]; b1 = pix[-9];                                 /* word[-5] */
        hdr5 = (u16)((u16)b0 | ((u16)b1 << 8));                      /* original value */
        pix[-10] = b1; pix[-9] = b0;
        if ((hdr5 & 0xC000u) == 0u) {
            w = (u16)((u16)pix[-4] | ((u16)pix[-3] << 8));           /* swapped LE w */
            rows = h;
            p = pix;
            while (rows != 0u) {
                blocks = (u16)(w >> 2);
                if (blocks == 0u) {
                    return;
                }
                while (blocks != 0u) {
                    /* de-interleave the 8-byte block: [o0,o2,o4,o6,o1,o3,o5,o7] */
                    o1 = p[1]; o2 = p[2]; o3 = p[3];
                    o4 = p[4]; o5 = p[5]; o6 = p[6];
                    p[1] = o2; p[2] = o4; p[3] = o6;
                    p[4] = o1; p[5] = o3; p[6] = o5;
                    if (palette_mode == 0u) {
                        /* CGA: bit-reverse words 0,2; zero words 1,3 (unvalidated). */
                        u16 v0 = sprite_bitrev_block(p[0], p[1]);
                        u16 v2 = sprite_bitrev_block(p[4], p[5]);
                        p[0] = (u8)(v0 & 0xFF); p[1] = (u8)(v0 >> 8);
                        p[4] = (u8)(v2 & 0xFF); p[5] = (u8)(v2 >> 8);
                        p[2] = 0; p[3] = 0;     /* piVar8[1] = 0 */
                        p[6] = 0; p[7] = 0;     /* piVar8[3] = 0 */
                    }
                    p += 8;
                    blocks--;
                }
                rows--;
            }
        }
    }
}

void sprite_bank_load_transform(u8 __far *bank, u8 palette_mode)
{
    u16 i;
    u8 __far *e;
    u32 off;
    u8 __far *pix;

    for (i = 0; i < 512u; i++) {
        e = bank_ptr(bank, (u32)i * 4u);
        off = ((u32)e[0] << 24) | ((u32)e[1] << 16) | ((u32)e[2] << 8) | (u32)e[3];
        if (off == 0u) {
            break;                 /* 0 entry terminates the table */
        }
        pix = bank_ptr(bank, 0x800UL + off);
        sprite_frame_transform(pix, palette_mode);
    }
}
