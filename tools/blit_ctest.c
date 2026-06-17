/* Host unit test for src/sprite_blit.c: compiles the real port with __far/__huge
   shimmed out and exact 16-bit types, then replays it against the engine plane
   capture (blit_oracle.bin, BLT2) and checks byte-exact.  Build/run:
       cc -O2 -o /tmp/blit_ctest tools/blit_ctest.c && \
         /tmp/blit_ctest local/build/render/blit_oracle.bin
   Exit 0 iff every distinct blit matches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* shim the Watcom 16-bit environment for host compilation */
#define BUMPY_H
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge

#include "../src/sprite_blit.c"

#define PLANE 0x10000UL

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "local/build/render/blit_oracle.bin";
    FILE *f = fopen(path, "rb");
    long sz;
    u8 *b;
    u32 o;
    int n, i, ok = 0, total = 0;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz);
    if (fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
    fclose(f);
    if (memcmp(b, "BLT3", 4) != 0) { fprintf(stderr, "bad magic (want BLT3)\n"); return 2; }
    n = rd16(b + 4);
    /* header: DGWIN_OFF, DGWIN_LEN */
    {
        u16 dgwin_len_hdr = rd16(b + 8);
        o = 10;
        (void)dgwin_len_hdr;
    }

    u8 *work = malloc(4 * PLANE);
    for (i = 0; i < n; i++) {
        const u8 *desc, *src, *before, *after;
        u32 src_len, plen, dlin;
        u16 voff, dst_stride, full_w, cols, rows, dgwin_len;
        u8 shift;
        long diffs = 0, p;

        o += 4;                       /* ds, si */
        desc = b + o; o += 0x20;
        o += 4;                       /* setup_ds, setup_di */
        o += 0x20;                    /* object struct */
        dgwin_len = rd16(b + 8);
        o += dgwin_len;               /* DGROUP window */
        /* src_lin */ o += 4;
        src_len = *(u32 *)(b + o); o += 4;
        src = b + o; o += src_len;
        plen = *(u32 *)(b + o); o += 4;
        before = b + o; o += plen;
        after = b + o; o += plen;

        dlin = (u32)rd16(desc + 0x0a) * 16u + rd16(desc + 0x08);  /* dst seg:off */
        voff = (u16)(dlin - 0xA0000UL);
        full_w = rd16(desc + 0x0c);
        dst_stride = rd16(desc + 0x0e);
        cols = rd16(desc + 0x10);
        rows = rd16(desc + 0x12);
        shift = desc[0x16];

        memcpy(work, before, 4 * PLANE);
        sprite_blit_planar_vga(work, src, voff, dst_stride, full_w, cols, rows,
                               shift, desc[0x17]);
        for (p = 0; p < (long)(4 * PLANE); p++) {
            if (work[p] != after[p]) diffs++;
        }
        total++;
        if (diffs == 0) {
            ok++;
        } else {
            printf("  blit %d: DIFF %ld bytes (cols=%u rows=%u shift=%u full_w=%u)\n",
                   i, diffs, cols, rows, shift, full_w);
        }
    }
    printf("%d/%d blits byte-exact (C port)\n", ok, total);
    return (ok == total) ? 0 : 1;
}
