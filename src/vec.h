#ifndef VEC_H_INCLUDED
#define VEC_H_INCLUDED

#include "bumpy.h"

/* Full-screen .VEC raster dimensions and buffer sizes. */
#define VEC_W          320
#define VEC_H          200
#define VEC_PLANAR     32000u                /* 4 planes x 8000 bytes         */
#define VEC_CHUNKY     64000u                /* one colour index/pixel        */
#define VEC_DECODE_MAX 0x7d63u               /* 51 hdr + 48 palette + 32000 planar = 32099 */

/* Decoded VEC buffer layout constants (per docs/formats/VEC.md):
   offset 0  .. 50  : leading metadata header (51 bytes)
   offset 51 .. 98  : 16-colour palette, 16 x 3 bytes of 6-bit DAC R,G,B
   offset 99 .. 32098: 320x200 planar image, 4 sequential bitplanes x 8000 bytes */
#define VEC_HDR_BYTES   99u     /* total non-planar header = 51 + 48             */
#define VEC_PAL_OFF     51u     /* palette starts at decoded_buf[51]             */
#define VEC_PAL_BYTES   48u     /* 16 colours x 3 bytes                          */
#define VEC_PLANE_BYTES 8000u   /* bytes per plane (40 bytes/row x 200 rows)     */

/* Decode a .VEC op4 full-screen image into a PLANE-MAJOR planar buffer.
   Layout: plane p at planar[p*8000 .. p*8000+8000), 40 bytes/row, 200 rows.
   This matches the layout expected by video_blit_planar() and vec_diff --planar.

   data/n  : raw .VEC file bytes.
   planar  : caller-provided 32000-byte output buffer (plane-major).
   pal_out : if non-NULL, receives 48 bytes of 6-bit DAC palette
             (16 x 3 bytes of R,G,B, values 0..63).

   Returns 0 on success, 0xffff on failure (stream too short, decode underflow,
   or op4-only path not satisfied — op12 record-stream screens are not yet
   implemented and will also return 0xffff). */
u16 vec_decode_planar(const u8 *data, u16 n, u8 *planar, u8 *pal_out);

#endif /* VEC_H_INCLUDED */
