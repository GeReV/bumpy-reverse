#include "bumpy.h"
#include "dosio.h"
#include "sprite.h"
#include <stdio.h>
#include <stdlib.h>

/* BSPRITE — validation harness for the sprite bank load transform.
   Loads BUMSPJEU.BIN, runs sprite_bank_load_transform for the given palette_mode
   (default 2 = VGA), and writes the transformed DATA region ([0x800, eof)) so it
   can be diffed byte-exact against the engine oracle's in-memory bank dump.

   Usage: BSPRITE [in.BIN] [out] [palette_mode] */

extern u8 __huge g_spr_bank[];

#define BANK_CAP   SPR_BANK_CAP
#define DATA_OFF   0x800UL
#define CHUNK      0x4000u

int main(int argc, char **argv)
{
    const char *in_path  = (argc > 1) ? argv[1] : "BUMSPJEU.BIN";
    /* Output is a FIXED name distinct from argv[1] (the run_bvec harness reuses
       argv[1] as its .PLN capture name and would clobber a colliding file). */
    const char *out_path = "SPROUT.BIN";
    u8 pm = (u8)((argc > 2) ? atoi(argv[2]) : 2);
    int fd;
    int n;
    u32 total;
    u32 off;
    u32 rem;
    u16 chunk;

    /* chunked load (bank > 64 KB) */
    fd = dosio_open_read(in_path);
    if (fd < 0) {
        printf("ERR: cannot open %s\n", in_path);
        return 1;
    }
    total = 0;
    while (total < BANK_CAP) {
        rem = BANK_CAP - total;
        chunk = (rem > (u32)CHUNK) ? CHUNK : (u16)rem;
        n = dosio_read(fd, g_spr_bank + total, chunk);
        if (n <= 0) {
            break;
        }
        total += (u32)n;
        if ((u16)n < chunk) {
            break;                          /* short read = EOF */
        }
    }
    dosio_close(fd);
    printf("loaded %lu bytes from %s (pm=%u)\n", total, in_path, (unsigned)pm);

    /* transform the whole bank in place */
    sprite_bank_load_transform((u8 __far *)g_spr_bank, pm);

    /* dump the transformed data region [0x800, total) in chunks */
    fd = dosio_create(out_path);
    if (fd < 0) {
        printf("ERR: cannot create %s\n", out_path);
        return 2;
    }
    off = DATA_OFF;
    while (off < total) {
        rem = total - off;
        chunk = (rem > (u32)CHUNK) ? CHUNK : (u16)rem;
        if (dosio_write(fd, g_spr_bank + off, chunk) != (int)chunk) {
            printf("ERR: write failed at %lu\n", off);
            dosio_close(fd);
            return 3;
        }
        off += chunk;
    }
    dosio_close(fd);
    printf("wrote transformed data [%lu, %lu) -> %s\n", DATA_OFF, total, out_path);
    return 0;
}
