#include "bumpy.h"
#include "dosio.h"
#include "vec.h"
#include <stdio.h>

/* VEC render core — first end-to-end slice.
   Usage: BVEC [in.VEC] [out.bin]
   Loads a .VEC, decodes it to a 320x200 16-colour chunky buffer (one byte =
   colour index 0..15 per pixel), and writes that buffer to a file. The chunky
   artifact is diffed pixel-for-pixel against the Python oracle (vec_render.py).

   Capture route (1): write the decoded chunky buffer via DOS INT 21h. This
   sidesteps planar-VGA-under-emulator while still validating the hard part
   (the RLE + planar decode). */

#define VEC_FILE_MAX 0x4000u   /* 16 KB: largest .VEC (TITRE ~12.6 KB) */

static u8 g_file[VEC_FILE_MAX];
static u8 g_scratch[VEC_DECODE_MAX];
static u8 g_chunky[VEC_CHUNKY];

int main(int argc, char **argv)
{
    const char *in_path;
    const char *out_path;
    s16 n;
    u16 hdr;

    in_path  = (argc > 1) ? argv[1] : "TITRE.VEC";
    out_path = (argc > 2) ? argv[2] : "TITRE.RAW";

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
    printf("decoded image (header %u bytes, planar @%u)\n", hdr, hdr);

    if (dosio_save(out_path, g_chunky, VEC_CHUNKY) != 0) {
        printf("ERR: cannot write %s\n", out_path);
        return 3;
    }
    printf("wrote %u-byte chunky image -> %s\n", (u16)VEC_CHUNKY, out_path);
    return 0;
}
