#ifndef VEC_H_INCLUDED
#define VEC_H_INCLUDED

#include "bumpy.h"

/* Full-screen .VEC raster dimensions and buffer sizes. */
#define VEC_W          320
#define VEC_H          200
#define VEC_PLANAR     32000u                /* 4 planes x 8000 bytes         */
#define VEC_CHUNKY     64000u                /* one colour index/pixel        */
#define VEC_DECODE_MAX 0x7d63u               /* 51 hdr + 48 palette + 32000 planar = 32099 */

/* Read big-endian 16-bit word at offset o. */
u16 vec_be16(const u8 *b, u16 o);

/* RLE-decode (op4 scheme) from data[start], producing up to `limit` bytes into
   `out`. Returns the number of decoded bytes. `n` is the length of `data`. */
u16 vec_rle_decode(const u8 *data, u16 n, u16 start, u16 limit, u8 *out);

/* Decode an EGA-planar buffer (seq layout, 4 sequential planes) starting at
   `hdr` into a 320x200 chunky buffer (one byte = colour index 0..15). */
void vec_planar_to_chunky(const u8 *buf, u16 buflen, u16 hdr, u8 *chunky);

/* Full pipeline: decode a loaded .VEC image into `chunky` (VEC_CHUNKY bytes).
   `data`/`n` is the raw .VEC file. `scratch` is a VEC_DECODE_MAX work buffer.
   Returns the planar-data offset (header length) used, or 0xffff on failure. */
u16 vec_decode_image(const u8 *data, u16 n, u8 *scratch, u8 *chunky);

#endif /* VEC_H_INCLUDED */
