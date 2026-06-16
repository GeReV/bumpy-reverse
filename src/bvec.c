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

   Normal path:
     1. Load .VEC into g_file.
     2. vec_decode_planar() → plane-major g_planar + 48-byte g_pal.
     3. video_set_mode_0d() → INT 10h mode 0x0D (320x200x16 EGA planar).
     4. video_set_palette6(g_pal) → upload 16-colour DAC.
     5. video_blit_planar(g_planar) → write 4 planes via sequencer map-mask.
     6. video_restore_text() → return to text mode.
     7. harness captures plane[] + DAC state → TITRE.PLN (32000+768 bytes).

   Note on segment layout: g_file is declared in bvec_buf1.c; g_planar is
   declared in bvec_buf2.c.  Both use __far so the linker places them outside
   the 64 KB DGROUP limit of the large memory model. */

#define VEC_FILE_MAX 0x8400u   /* 33792 B — covers SCORE.VEC (32099 B), the largest .VEC */
#define PLANAR_BYTES 32000u    /* 4 planes * 8000 bytes */

/* Large buffers declared __far in bvec_buf1.c / bvec_buf2.c to keep each
   segment under the 64 KB DGROUP limit. */
extern u8 __far g_file[];    /* VEC_FILE_MAX bytes */
extern u8 __far g_planar[];  /* PLANAR_BYTES bytes */

/* 16-colour 6-bit DAC palette (48 bytes: 16 x R,G,B). */
static u8 g_pal[VEC_PAL_BYTES];

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
    u16 rc;

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

    /* vec_decode_planar: runs the real vec_run op4 loop, writes plane-major
       planar into g_planar, and extracts the 16-colour 6-bit DAC palette
       into g_pal (48 bytes). */
    rc = vec_decode_planar(g_file, (u16)n, g_planar, g_pal);
    if (rc != 0u) {
        printf("ERR: vec_decode_planar failed (rc=%u)\n", (unsigned)rc);
        return 2;
    }
    printf("decoded planar image ok\n");

    /* Faithful planar-VGA pipeline. */
    video_set_mode_0d();
    video_set_palette6(g_pal);
    video_blit_planar(g_planar);
    video_restore_text();
    printf("blit complete\n");

    /* The harness (run_bvec.py) captures the VGA plane state + DAC into .PLN.
       The dosio_save here is a secondary on-disk copy (optional for harness). */
    if (dosio_save(out_path, g_planar, PLANAR_BYTES) != 0) {
        printf("ERR: cannot write %s\n", out_path);
        return 3;
    }
    printf("wrote %u-byte planar image -> %s\n", PLANAR_BYTES, out_path);
    return 0;
}
