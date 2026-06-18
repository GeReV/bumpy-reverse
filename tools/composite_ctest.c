/* Host composite driver — Task 3+4 of Plan 6b.
   Parses frame_oracle.bin (FRM3), renders the full background into a fresh
   4-plane buffer using src/bg_render.c bg_render_grid(), then draws layer-C
   static sprites using src/entity.c entity_draw_layer_c().  Diffs against
   the captured engine frame at both stages.

   Far/huge qualifiers are #define'd away for the host build (gcc/cc), exactly
   as tools/bg_ctest.c does it.

   Build/run:
     cc -O2 -o /tmp/composite_ctest tools/composite_ctest.c && /tmp/composite_ctest
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Shim the DOS-specific qualifiers and types so the src/ headers compile on
   the host.  Must appear before any src/ include. */
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
#include "../src/sprite_blit.c"
#include "../src/sprite_chain.c"
#include "../src/sprite_anim.c"
#include "../src/entity.c"

/* -------------------------------------------------------------------------
   FRM3 parse helpers
   ------------------------------------------------------------------------- */
#define PLANE_SZ  0x10000UL           /* bytes per VGA plane */
#define PLANES_SZ (4UL * PLANE_SZ)    /* total 4-plane buffer */

static u32 rd32(const u8 *p)
{
    return (u32)p[0]
         | ((u32)p[1] <<  8)
         | ((u32)p[2] << 16)
         | ((u32)p[3] << 24);
}

/* -------------------------------------------------------------------------
   Pixel comparison — mirrors composite_check.idx_at exactly:
     off = y*40 + x/8;  m = 0x80 >> (x & 7);
     idx = bit0(plane0[off]&m) | bit1(plane1[off]&m)
         | bit2(plane2[off]&m) | bit3(plane3[off]&m)
   ------------------------------------------------------------------------- */
static int idx_at(const u8 *planes, int x, int y)
{
    u32 off = (u32)y * 40 + (u32)(x / 8);
    u8  m   = (u8)(0x80 >> (x & 7));

    return ((planes[0 * PLANE_SZ + off] & m) ? 1 : 0)
         | ((planes[1 * PLANE_SZ + off] & m) ? 2 : 0)
         | ((planes[2 * PLANE_SZ + off] & m) ? 4 : 0)
         | ((planes[3 * PLANE_SZ + off] & m) ? 8 : 0);
}

/* -------------------------------------------------------------------------
   4-plane work buffer for bg_render_grid output
   ------------------------------------------------------------------------- */
static u8 work_planes[4 * PLANE_SZ];

/* Bank path for entity layer-C: same as anim_ctest.c */
#define BANK_BASE_LIN  0x4eae0UL
#define BANK_PATH      "local/build/render/bank_inmem.bin"

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1]
                                  : "local/build/render/frame_oracle.bin";
    const char *bankpath = (argc > 2) ? argv[2] : BANK_PATH;
    FILE *fh;
    long  sz, bsz;
    u8   *b, *bank;
    u32   o;
    u32   plen, alen, mlen;
    const u8 *cap_planes;   /* captured engine frame planes (from file) */
    const u8 *atlas_raster; /* PAV raster: atlas block data + 6 (skip header) */
    const u8 *bmap;         /* level tile map */
    const u8 *bum;          /* BUM per-level header (0xc2 bytes) */
    const u8 *dg;           /* full DGROUP snapshot (0x10000 bytes) */
    long  match = 0;
    int   x, y;
    sprite_view view;

    /* --- Load oracle file --- */
    fh = fopen(path, "rb");
    if (!fh) {
        fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    fseek(fh, 0, SEEK_END); sz = ftell(fh); fseek(fh, 0, SEEK_SET);
    b = (u8 *)malloc((size_t)sz);
    if (!b) {
        fprintf(stderr, "malloc failed\n");
        fclose(fh);
        return 2;
    }
    if (fread(b, 1, (size_t)sz, fh) != (size_t)sz) {
        fprintf(stderr, "short read\n");
        fclose(fh);
        return 2;
    }
    fclose(fh);

    /* --- Load sprite bank --- */
    fh = fopen(bankpath, "rb");
    if (!fh) {
        fprintf(stderr, "cannot open bank %s\n", bankpath);
        free(b);
        return 2;
    }
    fseek(fh, 0, SEEK_END); bsz = ftell(fh); fseek(fh, 0, SEEK_SET);
    bank = (u8 *)malloc((size_t)bsz);
    if (!bank) {
        fprintf(stderr, "bank malloc failed\n");
        fclose(fh);
        free(b);
        return 2;
    }
    if (fread(bank, 1, (size_t)bsz, fh) != (size_t)bsz) {
        fprintf(stderr, "short bank read\n");
        fclose(fh);
        free(bank);
        free(b);
        return 2;
    }
    fclose(fh);

    /* Verify magic */
    if (memcmp(b, "FRM3", 4) != 0) {
        fprintf(stderr, "expected FRM3 magic, got %.4s\n", b);
        free(bank);
        free(b);
        return 2;
    }

    /* --- Parse FRM3 blocks (all little-endian) ---
       +0x00  4 B  "FRM3"
       +0x04  4 B  u32 planes_len  (== 0x40000)
       +0x08  0x40000 B  captured engine planes (plane0..3)
       next   0x300 B  DAC palette (256*3) — skip
       next   4 B  u32 atlas_len
       next   atlas_len B  PAV atlas raster (raw; first 6 B = raster header)
       next   4 B  u32 map_len
       next   map_len B  level tile map
       --- FRM3 new blocks (Task 2) ---
       next   2 B  u16 level
       next   0xc2 B  BUM per-level header
       next   0x18 B  p1_sprite obj struct
       next   0x18 B  p2_sprite obj struct
       next   6 B  p1_glob
       next   6 B  p2_glob
       next   3*0xc B  layer-A channel records
       next   4*0xc B  layer-B channel records
       next   8 B  chan_tbl_raw
       next   0x10000 B  full DGROUP snapshot
    */
    o = 4;
    plen = rd32(b + o); o += 4;
    cap_planes = b + o; o += plen;
    o += 256 * 3;                   /* skip DAC */

    alen = rd32(b + o); o += 4;
    /* atlas block starts with a 6-byte raster header; bg_render_grid expects
       the pointer past that header — same as bg_ctest.c's "atlas + 6" usage */
    atlas_raster = b + o + 6; o += alen;

    mlen = rd32(b + o); o += 4;
    bmap = b + o; o += mlen;

    /* --- FRM3 new blocks (Task 2 additions) --- */
    o += 2;                         /* skip u16 level */
    bum = b + o; o += 0xc2;        /* BUM per-level header */
    o += 0x18;                      /* skip p1_sprite obj */
    o += 0x18;                      /* skip p2_sprite obj */
    o += 6;                         /* skip p1_glob */
    o += 6;                         /* skip p2_glob */
    o += 3 * 0xc;                   /* skip layer-A channel records */
    o += 4 * 0xc;                   /* skip layer-B channel records */
    o += 8;                         /* skip chan_tbl_raw */
    dg = b + o;                     /* full DGROUP snapshot (0x10000 B) */

    /* --- Full-screen sprite view (matches chain_ctest.c / engine globals) ---
       left=0 right=40 top=0 bottom=199 height=199 data_off=0 data_seg=0xa000 */
    view.left     = 0;
    view.right    = 40;
    view.top      = 0;
    view.bottom   = 199;
    view.height   = 199;
    view.data_off = 0x0000;
    view.data_seg = 0xa000;

    /* --- Render background into fresh plane buffer --- */
    memset(work_planes, 0, sizeof(work_planes));
    bg_render_grid(work_planes, atlas_raster, bmap);

    /* --- Pixel-diff after bg only --- */
    match = 0;
    for (y = 0; y < 200; y++) {
        for (x = 0; x < 320; x++) {
            if (idx_at(work_planes, x, y) == idx_at(cap_planes, x, y)) {
                match++;
            }
        }
    }
    printf("bg: %ld/64000 pixels match (%.1f%%)\n",
           match, (double)match / 64000.0 * 100.0);

    /* --- Draw layer-C static sprites into the same plane buffer ---
       FRM3 `bum` block is captured directly from the tilemap base (the oracle
       reads from deref(tilemap_far_ptr) with no offset — the tilemap ptr is
       re-pointed per level at load time).  entity_draw_layer_c uses bum[0x60+cell]
       which mirrors the engine's tilemap[0x60+cell] exactly. */
    entity_draw_layer_c(work_planes, bum, dg, bank, BANK_BASE_LIN, &view);

    /* --- Pixel-diff after bg + layer C --- */
    match = 0;
    for (y = 0; y < 200; y++) {
        for (x = 0; x < 320; x++) {
            if (idx_at(work_planes, x, y) == idx_at(cap_planes, x, y)) {
                match++;
            }
        }
    }
    printf("bg+C: %ld/64000 pixels match (%.1f%%)\n",
           match, (double)match / 64000.0 * 100.0);

    free(bank);
    free(b);
    return 0;
}
