/* host_compose_ctest.c — Plan A Task 2 compose check (the keystone gate).
 *
 * Drives one level background + entity compose through the REAL host render-leaf
 * wrappers (src/host/host_render.c, compiled with -DBUMPY_PLAYABLE on the host) and
 * asserts the resulting host_framebuffer matches, plane-for-plane, the validated
 * composite reference produced by tools/composite_ctest.c for the SAME inputs.
 *
 * This proves the keystone wiring: host_fb_init allocates the flat 4-plane RAM
 * framebuffer; the page table (sprite_table_base / cur_sprite_data) is registered
 * into it; and the blit leaf (p1_blit_sprite_leaf → host_render.c) routes the P1
 * sprite draw through the already-validated blitter into the framebuffer — the
 * exact call shape composite_ctest uses, but reached via the gameplay leaf rather
 * than a direct entity_draw_p1 call.
 *
 * Method: replicate composite_ctest's compose (bg → layerC → P1 → P2 → layerA →
 * layerB) byte-for-byte, EXCEPT the P1 sprite is drawn by writing the captured P1
 * obj fields into the dg shadow at 0x792e and calling p1_blit_sprite_leaf(0x792e),
 * which reads them back and re-runs entity_draw_p1 into host_framebuffer.  The
 * result must equal composite_ctest's --dump-planes output exactly.
 *
 * Build/run:
 *   tools/composite_ctest --dump-planes <ref>     (produce the reference once)
 *   cc -O2 -o /tmp/hcc tools/host_compose_ctest.c && /tmp/hcc <oracle> <bank> <ref>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* DOS-qualifier + type shims (must precede any src/ include) — same as
   composite_ctest.c. */
#define BUMPY_H
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge

/* host.h hardware constants the host layer references (we are not including the
   real dos.h-backed bumpy.h, so provide what host_render.c needs). */
#define halloc(count, size)  calloc((size_t)(count), (size_t)(size))
/* DOS intrinsics host_render.c's playable block uses (BUMPY_H guards the real
   <i86.h>/<conio.h>/<malloc.h> out): a flat >1 MB arena backs MK_FP so a000-window
   far writes land in valid host memory; port I/O is a no-op. */
#define HC_FARMEM_BYTES 0x110000UL
static u8 hc_far_mem[HC_FARMEM_BYTES];
#define MK_FP(seg, off) ((void *)(hc_far_mem + (((u32)(u16)(seg) << 4) + (u16)(off))))
#define _fmemset(p, v, n) memset((void *)(p), (v), (size_t)(n))
static void outp(u16 port, unsigned val) { (void)port; (void)val; }

#include "../src/bg_render.c"
#include "../src/sprite_blit.c"
#include "../src/sprite_chain.c"
#include "../src/sprite_anim.c"
#include "../src/gfx_overlay.c"
#include "../src/entity.c"

/* Cross-module symbols host_render.c's playable block references but this harness's
   compose scope never exercises: the anim-erase leaf inputs (anim.c/view_setup.c) and
   the playable-only cursor screen-sprite leaf (entity.c compiles WITHOUT the flag
   here, so its playable-gated body is absent).  Link-only stubs. */
u8 __far *anim_a_erase_view;
const u8 __far *host_clean_bg(void) { return (const u8 __far *)0; }
const u8 __far *host_font_ptr(void) { return (const u8 __far *)0; }  /* text path: NOP (host_resource.c out of scope) */
struct sprite_view_fwd;   /* matches entity.c's sprite_view by pointer only */
void entity_draw_screen_sprite(u8 __huge *planes, u16 pixel_x, u16 pixel_y, u16 frame,
                               u16 ftbl_off, u16 ftbl_seg,
                               u8 __huge *bank, u32 bank_base_lin,
                               const sprite_view *view)
{ (void)planes;(void)pixel_x;(void)pixel_y;(void)frame;(void)ftbl_off;(void)ftbl_seg;
  (void)bank;(void)bank_base_lin;(void)view; }

/* Pull in the real host render layer under the playable flag.  host.h is included
   by host_render.c; with BUMPY_H defined its #include "bumpy.h" is a no-op, and the
   shims above satisfy its decls.  We must NOT redefine the host.h symbols, so let
   host.h provide them. */
#define BUMPY_PLAYABLE
#include "../src/host/host_render.c"
#undef BUMPY_PLAYABLE

#define PLANE_SZ  0x10000UL
#define BANK_BASE_LIN  0x4eae0UL

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u16 rd16c(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }

int main(int argc, char **argv)
{
    const char *path     = (argc > 1) ? argv[1] : "local/build/render/frame_oracle.bin";
    const char *bankpath = (argc > 2) ? argv[2] : "local/build/render/bank_inmem.bin";
    const char *refpath  = (argc > 3) ? argv[3] : NULL;
    FILE *fh;
    long sz, bsz, rsz;
    u8 *b, *bank, *ref = NULL;
    u32 o, plen, alen, mlen;
    const u8 *atlas_raster, *bmap, *bum, *p1_obj_cap, *p2_obj_cap, *p2_glob, *dg_src;
    u8 *dg;                          /* mutable dg copy (leaf reads obj fields) */
    sprite_view view;
    u16 p2_pixel_x, p2_pixel_y, p2_move_anim, p2_frame_base;
    s8  p2_cell;
    int rc = 0;

    if (!refpath) {
        fprintf(stderr, "usage: %s <oracle> <bank> <reference.planes>\n", argv[0]);
        return 2;
    }

    /* Load oracle. */
    fh = fopen(path, "rb");
    if (!fh) { fprintf(stderr, "open %s\n", path); return 2; }
    fseek(fh, 0, SEEK_END); sz = ftell(fh); fseek(fh, 0, SEEK_SET);
    b = malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, fh) != (size_t)sz) { fprintf(stderr,"short\n"); return 2; }
    fclose(fh);

    /* Load bank. */
    fh = fopen(bankpath, "rb");
    if (!fh) { fprintf(stderr, "open bank %s\n", bankpath); return 2; }
    fseek(fh, 0, SEEK_END); bsz = ftell(fh); fseek(fh, 0, SEEK_SET);
    bank = malloc((size_t)bsz);
    if (fread(bank, 1, (size_t)bsz, fh) != (size_t)bsz) { fprintf(stderr,"short bank\n"); return 2; }
    fclose(fh);

    /* Load reference planes. */
    fh = fopen(refpath, "rb");
    if (!fh) { fprintf(stderr, "open ref %s\n", refpath); return 2; }
    fseek(fh, 0, SEEK_END); rsz = ftell(fh); fseek(fh, 0, SEEK_SET);
    ref = malloc((size_t)rsz);
    if (fread(ref, 1, (size_t)rsz, fh) != (size_t)rsz) { fprintf(stderr,"short ref\n"); return 2; }
    fclose(fh);
    if (rsz != (long)(4UL * PLANE_SZ)) {
        fprintf(stderr, "ref size %ld != %lu\n", rsz, 4UL * PLANE_SZ);
        return 2;
    }

    if (memcmp(b, "FRM3", 4) != 0 && memcmp(b, "FRM4", 4) != 0) {
        fprintf(stderr, "bad magic\n"); return 2;
    }

    /* Parse FRM3/FRM4 (same offsets composite_ctest uses). */
    o = 4;
    plen = rd32(b + o); o += 4;
    o += plen;                       /* skip captured planes */
    o += 256 * 3;                    /* DAC */
    alen = rd32(b + o); o += 4;
    atlas_raster = b + o + 6; o += alen;
    mlen = rd32(b + o); o += 4;
    bmap = b + o; o += mlen;
    o += 2;                          /* level */
    bum = b + o; o += 0xc2;
    p1_obj_cap = b + o; o += 0x18;
    p2_obj_cap = b + o; o += 0x18;
    o += 6;                          /* p1_glob */
    p2_glob = b + o; o += 6;
    o += 3 * 0xc; o += 4 * 0xc; o += 8;
    dg_src = b + o;                  /* full DGROUP snapshot */

    /* Mutable dg copy: the blit leaf reads the P1 obj fields back from dg[0x792e]. */
    dg = malloc(0x10000u);
    memcpy(dg, dg_src, 0x10000u);

    p2_pixel_x   = rd16c(p2_glob + 0);
    p2_pixel_y   = rd16c(p2_glob + 2);
    p2_move_anim = rd16c(p2_glob + 4);
    p2_frame_base = rd16c(dg + 0xa0de);
    p2_cell = (s8)dg[0x8571];

    /* ── KEYSTONE: allocate the framebuffer + register the page table. ── */
    host_fb_init();
    if (host_framebuffer == NULL) { fprintf(stderr, "fb alloc failed\n"); return 2; }
    host_render_bind((u8 *)bank, BANK_BASE_LIN, (const u8 *)dg);

    /* Sanity: page table mirrors the engine's sprite_table_base (DGROUP 0x5415):
       [0] = a200:0000, [1] = a000:0000 — check via the LINEAR page offsets
       (seg<<4 + off − 0xA0000), the representation the 2026-07-02 double-buffer
       fix normalized (the old off[0]==0x2000 pairing with seg A200 double-counted). */
    if (host_page_off_of(1) != 0x0000u || host_page_off_of(0) != 0x2000u) {
        fprintf(stderr, "ASSERT FAIL: page table offsets wrong\n"); rc = 1;
    }
    if (host_draw_page_off() != 0x0000u) {
        fprintf(stderr, "ASSERT FAIL: cur_sprite_data not page0\n"); rc = 1;
    }

    /* The composite reference renders to a page-0 view (data_seg=0xa000, off=0).
       host_fb_init defaults cur_sprite_data to page0, so the leaf-built view
       matches the reference's view exactly. */
    view.left = 0; view.right = 40; view.top = 0; view.bottom = 199;
    view.height = 199; view.data_off = 0x0000u; view.data_seg = 0xa000u;

    /* Clear + bg + layer C (direct, exactly as composite_ctest). */
    memset((void *)host_framebuffer, 0, (size_t)(4UL * PLANE_SZ));
    bg_render_grid(host_framebuffer, atlas_raster, bmap);
    entity_draw_layer_c(host_framebuffer, bum, dg, (u8 *)bank, BANK_BASE_LIN, &view);

    /* ── P1 via the REAL leaf ──
       Write the captured P1 draw-time fields into dg[0x792e] (x/y/frame), exactly
       the values composite_ctest passes to entity_draw_p1, then dispatch through
       p1_blit_sprite_leaf — the gameplay leaf that the host build links. */
    {
        u16 draw_x     = rd16c(p1_obj_cap + 0);
        u16 draw_y     = rd16c(p1_obj_cap + 2);
        u16 draw_frame = rd16c(p1_obj_cap + 4);
        dg[0x792e + 0] = (u8)draw_x;        dg[0x792e + 1] = (u8)(draw_x >> 8);
        dg[0x792e + 2] = (u8)draw_y;        dg[0x792e + 3] = (u8)(draw_y >> 8);
        dg[0x792e + 4] = (u8)draw_frame;    dg[0x792e + 5] = (u8)(draw_frame >> 8);
        p1_blit_sprite_leaf(0x792e, 0x203b);
    }

    /* P2 (direct; absent on level 1, present on level 8 — composite_ctest draws it
       directly with the same args). */
    entity_draw_p2(host_framebuffer, dg, p2_pixel_x, p2_pixel_y, p2_move_anim,
                   p2_frame_base, p2_cell, (u8 *)bank, BANK_BASE_LIN, &view);
    (void)p2_obj_cap;

    /* layer A + layer B (direct, exactly as composite_ctest). */
    entity_draw_layer_a(host_framebuffer, bum, dg, (u8 *)bank, BANK_BASE_LIN, &view);
    entity_draw_layer_b(host_framebuffer, bum, dg, (u8 *)bank, BANK_BASE_LIN, &view);

    /* ── Assert host_framebuffer == composite reference, plane-for-plane. ── */
    {
        u32 i, diff = 0, first = 0xffffffffUL;
        for (i = 0; i < 4UL * PLANE_SZ; i++) {
            if (((u8 *)host_framebuffer)[i] != ref[i]) {
                if (first == 0xffffffffUL) { first = i; }
                diff++;
            }
        }
        if (diff == 0) {
            printf("host compose == composite reference: EXACT MATCH (%lu bytes)\n",
                   4UL * PLANE_SZ);
        } else {
            printf("host compose MISMATCH: %lu/%lu bytes differ (first @ 0x%lx)\n",
                   (unsigned long)diff, 4UL * PLANE_SZ, (unsigned long)first);
            rc = 1;
        }
    }

    return rc;
}
