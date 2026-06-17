/* Host unit test for src/sprite_chain.c: builds each captured sprite's blit
   descriptor from its object + view globals and checks it byte-exact against the
   descriptor the engine actually passed to sprite_blit_planar_vga (blit_oracle.bin
   BLT3, the chain caps where setup fired).  Build/run:
       cc -O2 -o /tmp/chain_ctest tools/chain_ctest.c && \
         /tmp/chain_ctest local/build/render/blit_oracle.bin
   Exit 0 iff every chain blit's descriptor matches (bytes 0..0x17). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define BUMPY_H
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge

#include "../src/sprite_chain.c"

#define PLANE 0x10000UL
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

/* DGROUP window runtime offsets (== Ghidra 203b offsets) of the chain globals */
#define DG_LEFT   0x680d
#define DG_RIGHT  0x680f
#define DG_TOP    0x6811
#define DG_BOTTOM 0x6813
#define DG_VHGT   0x6817
#define DG_CSDOFF 0x56e2
#define DG_CSDSEG 0x56e4

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "local/build/render/blit_oracle.bin";
    FILE *f = fopen(path, "rb");
    long sz; u8 *b; u32 o; int n, i, dgwin_off, dgwin_len, ok = 0, total = 0;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz);
    if (fread(b, 1, sz, f) != (size_t)sz) { return 2; }
    fclose(f);
    if (memcmp(b, "BLT3", 4) != 0) { fprintf(stderr, "want BLT3\n"); return 2; }
    n = rd16(b + 4);
    dgwin_off = rd16(b + 6);
    dgwin_len = rd16(b + 8);
    o = 10;

    for (i = 0; i < n; i++) {
        const u8 *desc, *obj, *dgwin;
        u16 setup_di;
        u32 src_len, plen;
        u8 got[0x18];
        sprite_view view;
        int drawn, j;

        o += 4;                          /* ds, si */
        desc = b + o; o += 0x20;
        setup_di = rd16(b + o); o += 4;  /* setup_ds, setup_di */
        obj = b + o; o += 0x20;
        dgwin = b + o; o += dgwin_len;
        o += 4;                          /* src_lin */
        src_len = *(u32 *)(b + o); o += 4 + src_len;
        plen = *(u32 *)(b + o); o += 4 + 2 * plen;

        if (setup_di == 0) {
            continue;                    /* direct (non-chain) blit; no setup */
        }
        view.left   = (s16)rd16(dgwin + DG_LEFT   - dgwin_off);
        view.right  = (s16)rd16(dgwin + DG_RIGHT  - dgwin_off);
        view.top    = (s16)rd16(dgwin + DG_TOP    - dgwin_off);
        view.bottom = (s16)rd16(dgwin + DG_BOTTOM - dgwin_off);
        view.height = (s16)rd16(dgwin + DG_VHGT   - dgwin_off);
        view.data_off = rd16(dgwin + DG_CSDOFF - dgwin_off);
        view.data_seg = rd16(dgwin + DG_CSDSEG - dgwin_off);

        memset(got, 0, sizeof(got));
        drawn = sprite_blit_build_desc(obj, &view, got);
        total++;
        if (drawn && memcmp(got, desc, 0x18) == 0) {
            ok++;
        } else {
            printf("  cap %d: MISMATCH (drawn=%d)\n", i, drawn);
            printf("    got =");
            for (j = 0; j < 0x18; j++) printf("%02x", got[j]);
            printf("\n    want=");
            for (j = 0; j < 0x18; j++) printf("%02x", desc[j]);
            printf("\n");
        }
    }
    printf("%d/%d chain descriptors byte-exact\n", ok, total);
    return (ok == total && total > 0) ? 0 : 1;
}
