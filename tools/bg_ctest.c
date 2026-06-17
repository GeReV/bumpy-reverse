/* Host unit test for src/bg_render.c: replays the per-cell background tile build
   on the engine plane capture (bg_oracle.bin BG02) and checks the real C port
   byte-exact (in the cell's footprint) against the planes the engine produced.
   Consecutive snapshots give before/after; row-last cells (cx=19) wrap their
   delta into the next row, so the comparison is restricted to each cell's own
   footprint (cols [cx*2, cx*2+4) clipped to 40, rows [cy*8, cy*8+16)).
   Build/run: cc -O2 -o /tmp/bg_ctest tools/bg_ctest.c && /tmp/bg_ctest */
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

#include "../src/bg_render.c"

#define PLANE 0x10000UL
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "local/build/render/bg_oracle.bin";
    FILE *f = fopen(path, "rb");
    long sz;
    u8 *b;
    const u8 *atlas, *bmap;
    u32 o;
    int n, i, ok = 0, total = 0;
    u32 alen, mlen, plen = 0;
    /* per-cell: run_code,cx,cy,frame + planes pointer */
    typedef struct { u16 rc, cx, cy, fr; const u8 *planes; } cell_t;
    cell_t *cells;
    u8 *work;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz);
    if (fread(b, 1, sz, f) != (size_t)sz) return 2;
    fclose(f);
    if (memcmp(b, "BG02", 4) != 0) { fprintf(stderr, "want BG02\n"); return 2; }
    n = rd16(b + 4);
    o = 6;
    /* atlas_lin, atlas_len, atlas */
    o += 4; alen = *(u32 *)(b + o); o += 4; atlas = b + o; o += alen;
    /* map_lin, map_len, map */
    o += 4; mlen = *(u32 *)(b + o); o += 4; bmap = b + o; o += mlen;
    (void)mlen;

    cells = malloc(sizeof(cell_t) * n);
    for (i = 0; i < n; i++) {
        cells[i].rc = rd16(b + o); cells[i].cx = rd16(b + o + 2);
        cells[i].cy = rd16(b + o + 4); cells[i].fr = rd16(b + o + 6); o += 8;
        plen = *(u32 *)(b + o); o += 4;
        cells[i].planes = b + o; o += plen;
    }

    work = malloc(4 * PLANE);
    for (i = 0; i < n - 1; i++) {     /* consecutive: before=cells[i], after=cells[i+1] */
        cell_t *c = &cells[i];
        const u8 *after = cells[i + 1].planes;
        int c0 = c->cx * 2, c1 = c->cx * 2 + 4;
        int r0 = c->cy * 8, r1 = c->cy * 8 + 16;
        long diffs = 0;
        int p, row, col;

        if (c1 > 40) c1 = 40;
        memcpy(work, c->planes, 4 * PLANE);
        bg_tile_run(work, atlas + 6, bmap, (u8)c->rc, c->cx, c->cy);
        for (p = 0; p < 4; p++) {
            for (row = r0; row < r1; row++) {
                for (col = c0; col < c1; col++) {
                    u32 x = (u32)p * PLANE + (u32)row * 40 + col;
                    if (work[x] != after[x]) diffs++;
                }
            }
        }
        total++;
        if (diffs == 0) {
            ok++;
        } else {
            printf("  cell %d (cx=%u cy=%u rc=%#x): %ld in-region diffs\n",
                   i, c->cx, c->cy, c->rc, diffs);
        }
    }
    printf("%d/%d bg cells byte-exact (in-region, C port)\n", ok, total);
    return (ok == total && total > 0) ? 0 : 1;
}
