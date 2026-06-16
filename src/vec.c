#include "vec.h"

/* Ported from tools/extract/vec_render.py (the authoritative oracle). The decomp
   opcode dispatch (jump table @ DGROUP 0x4e37) is unresolved here; for the
   full-screen op4-only case (TITRE etc.) the single record's RLE payload IS the
   final planar buffer, so we reproduce the oracle's decode directly. See the
   task note on divergence from a literal vec_run transcription. */

u16 vec_be16(const u8 *b, u16 o)
{
    return (u16)(((u16)b[o] << 8) | (u16)b[o + 1]);
}

/* op4 RLE: escape = data[start]; payload follows at start+1.
     b != escape         -> literal b
     escape, escape      -> one literal escape byte
     escape, x, count    -> x repeated `count` (count==0 means 256); x != escape */
u16 vec_rle_decode(const u8 *data, u16 n, u16 start, u16 limit, u8 *out)
{
    u8  escape;
    u16 i;
    u16 outlen;
    u8  b;
    u8  x;
    u16 count;
    u16 k;

    escape = data[start];
    i = (u16)(start + 1);
    outlen = 0;
    while (outlen < limit && i < n) {
        b = data[i]; i++;
        if (b != escape) {
            out[outlen++] = b;
            continue;
        }
        if (i >= n) {
            break;
        }
        x = data[i]; i++;
        if (x == escape) {
            out[outlen++] = escape;
            continue;
        }
        if (i >= n) {
            break;
        }
        count = data[i]; i++;
        if (count == 0) {
            count = 256;
        }
        for (k = 0; k < count && outlen < limit; k++) {
            out[outlen++] = x;
        }
    }
    return outlen;
}

/* seq layout: plane p starts at hdr + p*plane; pixel bit is 7 - (x & 7). */
void vec_planar_to_chunky(const u8 *buf, u16 buflen, u16 hdr, u8 *chunky)
{
    u16 wb;
    u16 plane;
    u16 y;
    u16 x;
    u8  bit;
    u8  v;
    u8  p;
    u32 off;
    u16 o;

    wb = VEC_W / 8;             /* 40 bytes per scanline per plane */
    plane = (u16)(wb * VEC_H);  /* 8000 bytes per plane           */
    for (y = 0; y < VEC_H; y++) {
        for (x = 0; x < VEC_W; x++) {
            bit = (u8)(7 - (x & 7));
            v = 0;
            for (p = 0; p < 4; p++) {
                off = (u32)hdr + (u32)p * plane + (u32)y * wb + (x >> 3);
                if (off < (u32)buflen) {
                    v = (u8)(v | (((buf[off] >> bit) & 1) << p));
                }
            }
            o = (u16)(y * VEC_W + x);
            chunky[o] = v;
        }
    }
}

u16 vec_decode_image(const u8 *data, u16 n, u8 *scratch, u8 *chunky)
{
    u16 decoded_size;
    u16 decoded;
    u16 hdr;

    /* Record 0 header word w1 is the decoded size; payload starts at offset 12. */
    if (n < 12) { return 0xffffu; }
    decoded_size = vec_be16(data, 2);
    if (decoded_size > VEC_DECODE_MAX) {
        decoded_size = VEC_DECODE_MAX;
    }
    decoded = vec_rle_decode(data, n, 12, decoded_size, scratch);
    if (decoded < VEC_PLANAR) {
        return 0xffffu;     /* not a full-screen raster */
    }
    hdr = (u16)(decoded - VEC_PLANAR);
    vec_planar_to_chunky(scratch, decoded, hdr, chunky);
    return hdr;
}
