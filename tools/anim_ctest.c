/* Host unit test for src/sprite_anim.c: replays the per-frame animation select on
   each captured sprite object (using the transformed bank bank_inmem.bin from 5a)
   and checks the frame ptr + copied header fields byte-exact against the object the
   engine actually produced (blit_oracle.bin BLT3 captures the post-select object).
   Build/run:
       cc -O2 -o /tmp/anim_ctest tools/anim_ctest.c && /tmp/anim_ctest
   Exit 0 iff every chain sprite's selected frame + header matches. */
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

#include "../src/sprite_anim.c"

#define PLANE 0x10000UL
#define BANK_BASE_LIN 0x4eae0UL          /* runtime linear of bank_inmem[0] (5a) */
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "local/build/render/blit_oracle.bin";
    const char *bankpath = (argc > 2) ? argv[2] : "local/build/render/bank_inmem.bin";
    FILE *f = fopen(path, "rb"), *bf = fopen(bankpath, "rb");
    long sz, bsz;
    u8 *b, *bank;
    u32 o;
    int n, i, dgwin_len, ok = 0, total = 0;

    if (!f || !bf) { fprintf(stderr, "cannot open inputs\n"); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz); if (fread(b, 1, sz, f) != (size_t)sz) return 2; fclose(f);
    fseek(bf, 0, SEEK_END); bsz = ftell(bf); fseek(bf, 0, SEEK_SET);
    bank = malloc(bsz); if (fread(bank, 1, bsz, bf) != (size_t)bsz) return 2; fclose(bf);

    if (memcmp(b, "BLT3", 4) != 0) { fprintf(stderr, "want BLT3\n"); return 2; }
    n = rd16(b + 4);
    dgwin_len = rd16(b + 8);
    o = 10;

    for (i = 0; i < n; i++) {
        const u8 *obj;
        u16 setup_di;
        u32 src_len, plen;
        u8 work[0x40];

        o += 4;                          /* ds, si */
        o += 0x20;                       /* desc */
        setup_di = rd16(b + o); o += 4;  /* setup_ds, setup_di */
        obj = b + o; o += 0x20;
        o += dgwin_len;
        o += 4;                          /* src_lin */
        src_len = *(u32 *)(b + o); o += 4 + src_len;
        plen = *(u32 *)(b + o); o += 4 + 2 * plen;

        if (setup_di == 0) {
            continue;                    /* direct blit; never went through prepare */
        }
        memset(work, 0, sizeof(work));
        memcpy(work, obj, 0x20);         /* input fields (idx@4, table@6) preserved */
        sprite_prepare_frame(work, bank, BANK_BASE_LIN);

        total++;
        /* compare ctrl + frame ptr + header + count + sub-header entry 0 (0x0b..0x1f) */
        if (memcmp(work + 0x0b, obj + 0x0b, 0x20 - 0x0b) == 0) {
            ok++;
        } else {
            int j;
            printf("  cap %d: MISMATCH (fidx=%u)\n", i, rd16(obj + 4));
            printf("    got =");
            for (j = 0x0b; j < 0x20; j++) printf("%02x", work[j]);
            printf("\n    want=");
            for (j = 0x0b; j < 0x20; j++) printf("%02x", obj[j]);
            printf("\n");
        }
    }
    printf("%d/%d animation selects byte-exact\n", ok, total);
    return (ok == total && total > 0) ? 0 : 1;
}
