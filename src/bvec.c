#include "bumpy.h"
#include "dosio.h"
#include "vec.h"
#include "video.h"
#include <stdio.h>
#include <string.h>

/* VEC render core — faithful planar-VGA pipeline.
   Usage:
     BVEC [in.VEC] [out.bin]  — decode .VEC, blit to VGA, write planar
     BVEC --selftest           — write a synthetic pattern to VGA and exit

   Capture route (2): blit planar buffer plane-by-plane via VGA sequencer;
   the harness captures from host.plane[].

   Note on segment layout: the large static buffers are split across
   bvec_buf1.c (g_file + g_scratch) and bvec_buf2.c (g_chunky + g_planar)
   to stay within the 64 KB per-segment limit of the large memory model. */

#define VEC_FILE_MAX 0x4000u   /* 16 KB: largest .VEC (TITRE ~12.6 KB) */
#define PLANAR_BYTES 32000u    /* 4 planes * 8000 bytes */

/* Large buffers declared __far in bvec_buf1.c / bvec_buf2.c to keep each
   segment under the 64 KB DGROUP limit. */
extern u8 __far g_file[];    /* VEC_FILE_MAX bytes */
extern u8 __far g_scratch[]; /* VEC_DECODE_MAX bytes */
extern u8 __far g_chunky[];  /* VEC_CHUNKY bytes */
extern u8 __far g_planar[];  /* PLANAR_BYTES bytes */

/* Synthesise a test pattern: pat[p*8000 + i] = (u8)((i + p*37) & 0xFF) */
static void selftest_fill(u8 *planar)
{
    u16 p;
    u16 i;

    for (p = 0; p < 4u; p++) {
        for (i = 0; i < 8000u; i++) {
            planar[p * 8000u + i] = (u8)((i + p * 37u) & 0xFFu);
        }
    }
}

int main(int argc, char **argv)
{
    const char *in_path;
    const char *out_path;
    s16 n;
    u16 hdr;

    /* --- selftest mode ---- */
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
        selftest_fill(g_planar);
        video_set_mode_0d();
        video_blit_planar(g_planar);
        video_restore_text();
        printf("selftest: blit done\n");
        return 0;
    }

    /* --- normal decode mode --- */
    in_path  = (argc > 1) ? argv[1] : "TITRE.VEC";
    out_path = (argc > 2) ? argv[2] : "TITRE.PLN";

    n = dosio_load(in_path, g_file, VEC_FILE_MAX);
    if (n <= 0) {
        printf("ERR: cannot read %s\n", in_path);
        return 1;
    }
    printf("loaded %d bytes from %s\n", (int)n, in_path);

    hdr = vec_decode_image(g_file, (u16)n, g_scratch, g_chunky);
    if (hdr == 0xffffu) {
        printf("ERR: not a full-screen raster .VEC\n");
        return 2;
    }
    printf("decoded image (header %u bytes)\n", hdr);

    /* The planar buffer lives at g_scratch + hdr (past the header). */
    memcpy(g_planar, g_scratch + hdr, PLANAR_BYTES);

    video_set_mode_0d();
    video_blit_planar(g_planar);
    video_restore_text();
    printf("blit complete\n");

    if (dosio_save(out_path, g_planar, PLANAR_BYTES) != 0) {
        printf("ERR: cannot write %s\n", out_path);
        return 3;
    }
    printf("wrote %u-byte planar image -> %s\n", PLANAR_BYTES, out_path);
    return 0;
}
