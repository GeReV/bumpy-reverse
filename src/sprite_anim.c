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

    /* ctrl & 0x40 -> packed-pixel expansion (see sprite_expand_frame). DEAD for
       BUMSPJEU (all frames ctrl=0x03), so not invoked here; the routine exists for
       completeness. In the engine this is where prepare_sprite_frames would expand
       the frame into the decode scratch and repoint obj+0xc/0xe at it. */
    if ((ctrl & 0xc0) != 0 && (ctrl & 0x40) != 0) {
        /* obj[0x0a] selects the path; the engine passes scratch / bitrev / mode
           globals that this port leaves to the caller (see sprite_expand_frame). */
        (void)0;
    }
}

/* -------------------------------------------------------------------------------
   sprite_expand_frame — UNVALIDATED reconstruction of the ctrl&0x40 packed-pixel
   EXPANSION path of prepare_sprite_frames (engine 1cec:2ea9).

   This path NEVER executes for Bumpy's Arcade Fantasy: every BUMSPJEU frame has
   ctrl=0x03 (bit 0x40 clear).  It is reconstructed here for completeness only, as a
   near-literal transcription of the raw disassembly (the decompiler output is
   register-conflated and unusable).  There is NO oracle to validate it against, and
   Path A's control-byte bit test reads a word (count_hi:ctrl) whose semantics are
   opaque in the original — so treat this as DOCUMENTATION-IN-CODE, not proven logic.

   Inputs (mirroring the engine):
     frame   -> the control-byte stream; the 12-byte header is at frame[-2..-0xc],
                width=frame[-4], height=frame[-2]; packed pixels follow the control
                bytes at frame + (width>>2)*height.
     scratch -> the decode scratch write cursor (DGROUP 0x56ee in the engine).  The
                12-byte header is copied to scratch[0..0xb] and the expanded frame
                begins at scratch+0xc (the engine repoints obj+0xc/0xe there).
     bitrev  -> the 256-byte pixel_bitrev_lut (DGROUP 0x66f0).
     path    -> obj[0x0a] (bit 0x20 selects Path B vs Path A).
     mode    -> the engine's mode flag iRam00010ded (selects a byte-swap in Path B).
   Returns the advanced scratch cursor (engine writes it back to DGROUP 0x56ee).
   ------------------------------------------------------------------------------- */
u8 __far *sprite_expand_frame(u8 __far *frame, u8 __far *scratch,
                              const u8 __far *bitrev, u8 path, u16 mode)
{
    u8 width = frame[-4];
    u8 height = frame[-2];
    u16 count = (u16)((u16)(width >> 2) * (u16)height);
    const u8 __far *ctrl_p = frame;                 /* bx: control bytes      */
    const u8 __far *src_p = frame + count;          /* si: packed pixel bytes */
    u8 __far *dst;                                   /* di: scratch write cursor */
    u16 i;
    u16 outer;
    u16 inner;

    /* header copy: scratch[0..0xb] = frame[-0xc..-1]; expanded frame at scratch+0xc */
    for (i = 0; i < 12; i++) {
        scratch[i] = frame[(s16)i - 0xc];
    }
    dst = scratch + 12;

    if (path & 0x20) {
        /* ---- Path B (engine 2ee9): bitrev de-interleave with column transpose ---- */
        dst -= (u16)(width * 2 + 8);
        for (outer = 0; outer < height; outer++) {
            dst += (u16)(width * 4);
            for (inner = 0; inner < (u16)(width >> 2); inner++) {
                u8 cl = *ctrl_p++;
                u8 ch, dl, dh, ah, cv;
                ch = (cl & 0x80) ? bitrev[*src_p++] : 0;
                dst[0] = (cl & 0x40) ? bitrev[*src_p++] : 0;       /* stosb */
                dl = (cl & 0x20) ? bitrev[*src_p++] : 0;
                dst[1] = (cl & 0x10) ? bitrev[*src_p++] : 0;
                dh = (cl & 0x08) ? bitrev[*src_p++] : 0;
                dst[2] = (cl & 0x04) ? bitrev[*src_p++] : 0;
                ah = (cl & 0x02) ? bitrev[*src_p++] : 0;
                dst[3] = (cl & 0x01) ? bitrev[*src_p++] : 0;
                cv = ah;
                if (mode == 0) {                       /* 2fb6: byte-swap de-interleave */
                    u8 t;
                    t = dst[2]; dst[2] = dst[3]; dst[3] = t;       /* es:[di-2] swap */
                    t = dst[0]; dst[0] = dst[1]; dst[1] = t;       /* es:[di-4] swap */
                    t = dl; dl = ch; ch = t;                       /* xchg dl,ch */
                    t = cv; cv = dh; dh = t;                       /* xchg cl,dh */
                }
                dst[4] = ch; dst[5] = dl; dst[6] = dh; dst[7] = cv; /* 2fd0: 4x stosb */
                dst -= 8;                              /* di += 8 (8 stosb) then -0x10 */
            }
            dst += (u16)(width * 2 + 8);               /* 2ff2: bp<<1; di += bp + 8 */
        }
    } else {
        /* ---- Path A (engine 3003): raw-byte path. The control test shifts the word
           (count_hi:ctrl); its exact bit order is opaque in the original — mirrored
           here literally for faithfulness, NOT semantic certainty. ---- */
        u16 outer_n = count;
        do {
            /* cx = count_hi:ctrl (the original keeps the count's high byte in ch and
               loads the control byte into cl, then tests bits MSB-first via shl). */
            u16 cx = (u16)((count & 0xff00) | *ctrl_p);
            u8 al, ah2, ch2, dl2;
            ctrl_p++;
            al = (u8)((cx & 0x8000) ? *src_p++ : 0); cx <<= 1; dst[0] = al;
            ah2 = (u8)((cx & 0x8000) ? *src_p++ : 0); cx <<= 1;
            al = (u8)((cx & 0x8000) ? *src_p++ : 0); cx <<= 1; dst[1] = al;
            ch2 = (u8)((cx & 0x8000) ? *src_p++ : 0); cx <<= 1;
            al = (u8)((cx & 0x8000) ? *src_p++ : 0); cx <<= 1; dst[2] = al;
            dl2 = (u8)((cx & 0x8000) ? *src_p++ : 0); cx <<= 1;
            al = (u8)((cx & 0x8000) ? *src_p++ : 0); cx <<= 1; dst[3] = al;
            dst[4] = ah2; dst[5] = ch2; dst[6] = dl2;
            al = (u8)((cx & 0x8000) ? *src_p++ : 0); dst[7] = al;
            dst += 8;
        } while (--outer_n != 0);
    }
    return dst;
}
