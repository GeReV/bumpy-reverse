/* Host composite driver — Task 3+4+5+6+7 of Plan 6b + Task 3 of Plan 6c.
   Parses frame_oracle.bin (FRM3 or FRM4), renders the full background into a
   fresh 4-plane buffer using src/bg_render.c bg_render_grid(), then draws
   layer-C static sprites using src/entity.c entity_draw_layer_c(), then draws
   P1 using entity_draw_p1(), P2 using entity_draw_p2(), then layer A using
   entity_draw_layer_a(), and layer B using entity_draw_layer_b().
   Diffs against the captured engine frame at each stage.

   *** DOUBLE-BUFFER MODEL (Plan 6c Task 3) ***

   The engine double-buffers across two VGA pages within the 64KB VGA plane:
     page0 = a000:0000  — plane byte offset 0x0000 within each plane
     page1 = a200:0000  — plane byte offset 0x2000 within each plane

   Each plane's 64KB buffer holds BOTH pages; the visible scanlines
   (200 rows × 40 bytes = 8000 B) start at the page's base offset.

   Page selection: set_sprite_table_ptr (1cec:2dd2) indexes sprite_table_base
   (DGROUP:0x5415) to set cur_sprite_data_ptr (DGROUP:0x56de).
   dispatch_palette_mode_with_src_ptr (1cec:2d6d) then splits the far-ptr value
   into cur_sprite_data_off (DGROUP:0x56e2) and cur_sprite_data_seg (0x56e4).
   sprite_table_base[0] = a200:0000 (page1), sprite_table_base[1] = a000:0000
   (page0); the engine alternates the index per-sprite blit (~50/50 split,
   confirmed from 888 csd_log entries at level 8).

   Xref evidence (local/build/xrefs_csd.txt):
     1cec:2d81 WRITE cur_sprite_data_off  in dispatch_palette_mode_with_src_ptr
     1cec:2d85 WRITE cur_sprite_data_seg  in dispatch_palette_mode_with_src_ptr
   Only one writer: dispatch_palette_mode_with_src_ptr @ 1cec:2d6d.

   LIVE PAGE AT CAPTURE: at the FRAME_ORACLE capture instant, the captured
   DGROUP holds cur_sprite_data seg = dg[0x56e4], off = dg[0x56e2].
   For the level-8 oracle: seg = 0xa200 → live page = page1, plane offset 0x2000.
   (For any oracle where seg = 0xa000, live page = page0, plane offset 0x0000.)

   VALIDATION TARGET: we compare the composite against the LIVE PAGE
   (offset = 0x2000 when seg=0xa200, else 0x0000), because the last complete
   entity draws happened to that page.  The composite match improves from
   ~53858 (offset 0 / page0) to ~54152 (offset 0x2000 / page1) on the level-8
   oracle (the two pages share all background pixels and differ only in ~328
   entity-draw pixels that landed on the last-blitted page).

   LIMITATION: The OTHER page (the non-live page) contains entity draws from
   the PREVIOUS animation tick.  Reproducing it would require capturing that
   prior tick's entity state — out of scope for Plan 6b/6c.  Residue vs the
   live page = the non-live page's previous-tick entity draws + the engine's
   VGA double-buffer save-under deltas (the double-buffer is not modelled here);
   see docs/rendering-pipeline.md.  NOTE: render_player_view is a planar
   save-under/read-back copy, NOT a visible "second pass"; and level-exit
   (bum+0x91) / items (bum+0x92) are game-state, not separate sprites — earlier
   notes calling those "residue" were misattributions, corrected.

   Far/huge qualifiers are #define'd away for the host build (gcc/cc), exactly
   as tools/bg_ctest.c does it.

   Build/run:
     cc -O2 -o /tmp/composite_ctest tools/composite_ctest.c && /tmp/composite_ctest

   Optional --dump-planes <path>: write the final composite planes (4*0x10000 B)
   to <path> for per-entity footprint analysis by tools/footprint_check.py.
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
#include "../src/bgi_overlay.c"
#include "../src/entity.c"

/* -------------------------------------------------------------------------
   FRM3/FRM4 parse helpers.
   FRM4 is a strict superset of FRM3 (identical byte layout up through DGROUP;
   FRM4 appends fullscreen_buf + present-log + csd-log after DGROUP).
   This driver reads only the FRM3-compatible portion and accepts both magics.
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

static u16 rd16(const u8 *p)
{
    return (u16)p[0] | ((u16)p[1] << 8);
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
   4-plane work buffer for composite output
   ------------------------------------------------------------------------- */
static u8 work_planes[4 * PLANE_SZ];

/* Snapshot of planes BEFORE a draw call, for verifying no-op paths */
static u8 snap_planes[4 * PLANE_SZ];

/* Bank path for entity draws: same as anim_ctest.c */
#define BANK_BASE_LIN  0x4eae0UL
#define BANK_PATH      "local/build/render/bank_inmem.bin"

/* DG offsets for P1 obj construction assertions */
#define DG_P1_OBJ_OFF  0x792eu

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1]
                                  : "local/build/render/frame_oracle.bin";
    const char *bankpath = (argc > 2) ? argv[2] : BANK_PATH;
    /* Optional 3rd argument: --dump-planes <outpath> */
    const char *dump_planes_path = NULL;
    if (argc > 3 && strcmp(argv[3], "--dump-planes") == 0 && argc > 4) {
        dump_planes_path = argv[4];
    }
    FILE *fh;
    long  sz, bsz;
    u8   *b, *bank;
    u32   o;
    u32   plen, alen, mlen;

    /* Pointers into the oracle file */
    const u8 *cap_planes;   /* captured engine frame planes (both pages) */
    const u8 *atlas_raster; /* PAV raster: atlas block + 6 (skip header) */
    const u8 *bmap;         /* level tile map */
    const u8 *bum;          /* BUM per-level header (0xc2 bytes) */
    const u8 *p1_obj_cap;   /* captured p1_sprite obj struct (0x18 bytes) */
    const u8 *p2_obj_cap;   /* captured p2_sprite obj struct (0x18 bytes) */
    const u8 *p1_glob;      /* p1 globals: pixel_x(u16) pixel_y(u16) move_anim(u16) */
    const u8 *p2_glob;      /* p2 globals: pixel_x(u16) pixel_y(u16) move_anim(u16) */
    const u8 *dg;           /* full DGROUP snapshot (0x10000 bytes) */

    /* Live page selection: determined from the captured cur_sprite_data seg
       (DGROUP:0x56e4).  0xa000 -> page0, plane offset 0x0000.
       0xa200 -> page1, plane offset 0x2000.
       All pixel-diffs use cap_planes + live_plane_off as the reference. */
    u16       live_csd_seg;    /* captured cur_sprite_data_seg */
    u16       live_csd_off;    /* captured cur_sprite_data_off */
    u32       live_plane_off;  /* plane byte offset for the live page */

    long  match = 0;
    long  match_bgC = 0;
    long  match_bgCP1 = 0;
    int   x, y;
    int   assert_fail = 0;
    sprite_view view;

    /* P1/P2 globals extracted from oracle */
    u16 p1_pixel_x, p1_pixel_y, p1_move_anim;
    u16 p2_pixel_x, p2_pixel_y, p2_move_anim;
    u16 p2_frame_base;
    s8  p2_cell;

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

    /* Verify magic: accept FRM3 and FRM4 (FRM4 is a strict superset of FRM3;
       both have identical byte layout for all blocks this driver reads). */
    if (memcmp(b, "FRM3", 4) != 0 && memcmp(b, "FRM4", 4) != 0) {
        fprintf(stderr, "expected FRM3 or FRM4 magic, got %.4s\n", b);
        free(bank);
        free(b);
        return 2;
    }

    /* --- Parse FRM3/FRM4 blocks (all little-endian) ---
       +0x00  4 B  "FRM3" or "FRM4"
       +0x04  4 B  u32 planes_len  (== 0x40000 = 4 * 0x10000)
       +0x08  0x40000 B  captured VGA planes (plane0..3, 0x10000 B each)
                         Each plane holds BOTH VGA pages:
                           page0 (a000:0000) at plane byte offset 0x0000
                           page1 (a200:0000) at plane byte offset 0x2000
                         Visible scanlines: 200 rows × 40 B = 8000 B from page base.
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
       --- FRM4 additions (Plan 6c Task 2) — not read here ---
       next   fullscreen_buf far ptr + len + data
       next   present-call log
       next   csd observations
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
    p1_obj_cap = b + o; o += 0x18; /* captured p1_sprite obj struct */
    p2_obj_cap = b + o; o += 0x18; /* captured p2_sprite obj struct */
    p1_glob    = b + o; o += 6;    /* p1 globals */
    p2_glob    = b + o; o += 6;    /* p2 globals */
    o += 3 * 0xc;                   /* skip layer-A channel records */
    o += 4 * 0xc;                   /* skip layer-B channel records */
    o += 8;                         /* skip chan_tbl_raw */
    dg = b + o;                     /* full DGROUP snapshot (0x10000 B) */

    /* --- Determine live VGA page from captured cur_sprite_data (Plan 6c T3) ---
       cur_sprite_data_off @ DGROUP:0x56e2, cur_sprite_data_seg @ DGROUP:0x56e4.
       These are written ONLY by dispatch_palette_mode_with_src_ptr (1cec:2d6d)
       from sprite_table_base[index] (DGROUP:0x5415):
         index 0 -> a200:0000 (page1, plane byte offset 0x2000)
         index 1 -> a000:0000 (page0, plane byte offset 0x0000)
       The engine alternates index per-sprite blit; at the FRAME_ORACLE capture
       instant, the value in dg[0x56e4] reveals which page received the last draw.
       We validate against THAT page for the highest-fidelity match.
    */
    live_csd_off = rd16(dg + 0x56e2);
    live_csd_seg = rd16(dg + 0x56e4);
    if (live_csd_seg == 0xa200u) {
        live_plane_off = 0x2000UL;   /* page1: a200:0000 */
    } else {
        live_plane_off = 0x0000UL;   /* page0: a000:0000 (or unknown seg → fallback) */
    }
    printf("live page: cur_sprite_data = %04x:%04x -> plane_off=0x%lx (%s)\n",
           (unsigned)live_csd_seg, (unsigned)live_csd_off,
           (unsigned long)live_plane_off,
           (live_csd_seg == 0xa200u) ? "page1/a200" : "page0/a000");

    /* --- Extract P1/P2 globals --- */
    p1_pixel_x  = rd16(p1_glob + 0);
    p1_pixel_y  = rd16(p1_glob + 2);
    p1_move_anim = rd16(p1_glob + 4);

    p2_pixel_x  = rd16(p2_glob + 0);
    p2_pixel_y  = rd16(p2_glob + 2);
    p2_move_anim = rd16(p2_glob + 4);

    /* p2_frame_base: dg[0xa0de] (u16) */
    p2_frame_base = rd16(dg + 0xa0de);

    /* p2_cell: dg[0x8571] (signed byte; -1 = P2 absent) */
    p2_cell = (s8)dg[0x8571];

    /* --- Full-screen sprite view (always targets work_planes at offset 0) ---
       The work_planes buffer is indexed by idx_at() starting at offset 0,
       so view.data_off/seg must anchor the blit to offset 0 within the buffer
       (matching a000:0000).  Entity blits go into work_planes[plane*0x10000+row_off]
       regardless of which live page the captured oracle was on.
       The live page offset (live_plane_off) is applied only to the REFERENCE side
       (cap_planes + live_plane_off) so that each pixel-diff compares our composite
       pixel against the captured pixel from the same live page.
       This is the correct model: composite renders to a fresh page-0-style buffer;
       validates against whichever captured page was live at oracle time. */
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
            if (idx_at(work_planes, x, y) == idx_at(cap_planes + live_plane_off, x, y)) {
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
    match_bgC = 0;
    for (y = 0; y < 200; y++) {
        for (x = 0; x < 320; x++) {
            if (idx_at(work_planes, x, y) == idx_at(cap_planes + live_plane_off, x, y)) {
                match_bgC++;
            }
        }
    }
    printf("bg+C: %ld/64000 pixels match (%.1f%%)\n",
           match_bgC, (double)match_bgC / 64000.0 * 100.0);

    /* =========================================================
       P1 OBJECT CONSTRUCTION ASSERTION (Task 5)
       Build the P1 object from p1_glob and verify fields match
       the captured p1_sprite obj from the FRM3 snapshot.

       draw_p1_sprite (1000:1cb2) writes:
         obj[+0] = p1_pixel_x   (u16)
         obj[+2] = p1_pixel_y   (u16)
         obj[+4] = p1_move_anim (u16)

       Expected: constructed [+0,+2,+4] == captured [+0,+2,+4].
       Note: a y-offset of 2 is known and explained in Task 5 report §3
       (oracle-capture timing: p1_pixel_y global is updated by game physics
       AFTER draw_p1_sprite ran in this frame; the obj struct holds the y from
       the PREVIOUS draw call while the global reflects the updated value).
       The x and frame fields are exact matches.  We assert x and frame and
       REPORT the y discrepancy without aborting.
       ========================================================= */
    if (p1_move_anim != 100u) {
        u16 cap_x     = rd16(p1_obj_cap + 0);
        u16 cap_y     = rd16(p1_obj_cap + 2);
        u16 cap_frame = rd16(p1_obj_cap + 4);

        int x_ok     = (p1_pixel_x   == cap_x);
        int frame_ok = (p1_move_anim == cap_frame);
        int y_ok     = (p1_pixel_y   == cap_y);

        printf("p1 obj assert: x=%s(%u==%u) y=%s(%u==%u) frame=%s(%u==%u)\n",
               x_ok     ? "MATCH" : "MISMATCH", p1_pixel_x,   cap_x,
               y_ok     ? "MATCH" : "MISMATCH(expected-timing-skew)", p1_pixel_y, cap_y,
               frame_ok ? "MATCH" : "MISMATCH", p1_move_anim, cap_frame);

        if (!x_ok || !frame_ok) {
            fprintf(stderr, "ASSERT FAIL: p1 obj x or frame mismatch\n");
            assert_fail = 1;
        }
    } else {
        printf("p1 obj assert: SKIP (move_anim==100 hidden sentinel)\n");
    }

    /* =========================================================
       P1 DRAW: add P1 sprite into the composite
       SOURCE: captured p1_obj fields (draw-time state), NOT the settle-instant
       globals.  draw_p1_sprite copies the globals into the obj just before the
       blit, so p1_obj_cap[+0/+2/+4] ARE the draw-time global values — feeding
       entity_draw_p1 the captured-object fields reproduces the captured planes.
       The p1_glob values are one physics tick AHEAD of p1_obj_cap[+2] (y-lag
       documented above); using them would paint the player ~2px off.
       Snapshot planes before and after to confirm the draw added pixels.
       ========================================================= */
    memcpy(snap_planes, work_planes, sizeof(work_planes));

    {
        u16 draw_x     = rd16(p1_obj_cap + 0);
        u16 draw_y     = rd16(p1_obj_cap + 2);
        u16 draw_frame = rd16(p1_obj_cap + 4);
        entity_draw_p1(work_planes, dg, draw_x, draw_y, draw_frame,
                       bank, BANK_BASE_LIN, &view);
    }

    /* --- Pixel-diff after bg + layer C + P1 --- */
    match_bgCP1 = 0;
    for (y = 0; y < 200; y++) {
        for (x = 0; x < 320; x++) {
            if (idx_at(work_planes, x, y) == idx_at(cap_planes + live_plane_off, x, y)) {
                match_bgCP1++;
            }
        }
    }
    match = match_bgCP1;
    printf("bg+C+P1: %ld/64000 pixels match (%.1f%%)\n",
           match_bgCP1, (double)match_bgCP1 / 64000.0 * 100.0);

    if (match_bgCP1 < match_bgC) {
        fprintf(stderr, "ASSERT FAIL: bg+C+P1 (%ld) < bg+C (%ld) — P1 draw regressed!\n",
                match_bgCP1, match_bgC);
        assert_fail = 1;
    }

    /* =========================================================
       P2 DRAW: add P2 sprite into the composite (absent on level 1)
       Snapshot planes before the call; verify no change when p2_cell==-1.
       ========================================================= */
    memcpy(snap_planes, work_planes, sizeof(work_planes));

    entity_draw_p2(work_planes, dg, p2_pixel_x, p2_pixel_y, p2_move_anim,
                   p2_frame_base, p2_cell, bank, BANK_BASE_LIN, &view);

    if (p2_cell == (s8)(-1)) {
        /* Level 1: P2 absent — assert planes unchanged */
        if (memcmp(work_planes, snap_planes, sizeof(work_planes)) == 0) {
            printf("p2 draw: P2 absent (p2_cell==-1) — planes UNCHANGED (correct)\n");
        } else {
            fprintf(stderr, "ASSERT FAIL: p2_cell==-1 but planes changed after entity_draw_p2!\n");
            assert_fail = 1;
        }
        printf("NOTE: P2 positive-path composite + object assert DEFERRED to P2-present level (Task 7)\n");
    } else {
        /* P2 present: diff and report */
        match = 0;
        for (y = 0; y < 200; y++) {
            for (x = 0; x < 320; x++) {
                if (idx_at(work_planes, x, y) == idx_at(cap_planes + live_plane_off, x, y)) {
                    match++;
                }
            }
        }
        printf("bg+C+P1+P2: %ld/64000 pixels match (%.1f%%)\n",
               match, (double)match / 64000.0 * 100.0);
    }

    /* =========================================================
       P2 OBJECT CONSTRUCTION ASSERTION (skeleton for Task 7)
       On level 1, p2_cell == -1 (P2 absent) so this block is skipped.
       When P2 IS present (future level), verify captured-obj fields match
       the draw-time globals, mirroring the P1 assert above.
       ========================================================= */
    if (p2_cell != (s8)(-1)) {
        u16 cap_x     = rd16(p2_obj_cap + 0);
        u16 cap_frame = rd16(p2_obj_cap + 4);
        int x_ok     = (p2_pixel_x == cap_x);
        int frame_ok = ((u16)(p2_frame_base + p2_move_anim) == cap_frame);
        printf("p2 obj assert: x=%s frame=%s\n",
               x_ok    ? "MATCH" : "MISMATCH",
               frame_ok ? "MATCH" : "MISMATCH");
        if (!x_ok || !frame_ok) {
            fprintf(stderr, "ASSERT FAIL: p2 obj x or frame mismatch\n");
            assert_fail = 1;
        }
    }

    /* =========================================================
       LAYER A DRAW: add layer-A static entities into the composite.
       Sourced from bum[0x00+cell], dg posA tables (0xf4/0xf6).
       Level 1: 27 layer-A cells (all cv=1 → frame=64, yoff=5).
       The match count must RISE substantially above bg+C+P1.
       ========================================================= */
    entity_draw_layer_a(work_planes, bum, dg, bank, BANK_BASE_LIN, &view);

    match = 0;
    for (y = 0; y < 200; y++) {
        for (x = 0; x < 320; x++) {
            if (idx_at(work_planes, x, y) == idx_at(cap_planes + live_plane_off, x, y)) {
                match++;
            }
        }
    }
    printf("bg+C+P1+A: %ld/64000 pixels match (%.1f%%)\n",
           match, (double)match / 64000.0 * 100.0);

    if (match < match_bgCP1) {
        fprintf(stderr, "ASSERT FAIL: bg+C+P1+A (%ld) < bg+C+P1 (%ld) — layer-A draw regressed!\n",
                match, match_bgCP1);
        assert_fail = 1;
    }

    /* =========================================================
       LAYER B DRAW: add layer-B entities into the composite.
       Sourced from bum[0x30+cell], dg posB tables (0x3f4/0x3f6); col==7 skipped.
       Level 1: 0 layer-B cells — structural exercise only; planes UNCHANGED.
       The positive blit path (frame += 0xf1 bias) is UNVALIDATED on level 1.
       Validation deferred to Task 7 (richer level with B-layer entities).
       ========================================================= */
    memcpy(snap_planes, work_planes, sizeof(work_planes));

    entity_draw_layer_b(work_planes, bum, dg, bank, BANK_BASE_LIN, &view);

    {
        long match_b = 0;
        for (y = 0; y < 200; y++) {
            for (x = 0; x < 320; x++) {
                if (idx_at(work_planes, x, y) == idx_at(cap_planes + live_plane_off, x, y)) {
                    match_b++;
                }
            }
        }
        printf("bg+C+P1+A+B: %ld/64000 pixels match (%.1f%%)\n",
               match_b, (double)match_b / 64000.0 * 100.0);

        if (match_b < match) {
            fprintf(stderr, "ASSERT FAIL: bg+C+P1+A+B (%ld) < bg+C+P1+A (%ld) — layer-B draw regressed!\n",
                    match_b, match);
            assert_fail = 1;
        }

        if (memcmp(work_planes, snap_planes, sizeof(work_planes)) == 0) {
            printf("layer-B: 0 cells on level 1 — planes UNCHANGED (correct; UNVALIDATED positive path)\n");
        } else {
            printf("layer-B: planes CHANGED (layer-B cells present — validate positive path)\n");
        }
    }

    /* Optionally dump the final composite planes (after bg+C+P1+A+B) for
       per-entity footprint analysis by tools/footprint_check.py. */
    if (dump_planes_path != NULL) {
        FILE *dfh = fopen(dump_planes_path, "wb");
        if (dfh) {
            fwrite(work_planes, 1, sizeof(work_planes), dfh);
            fclose(dfh);
            printf("composite planes dumped -> %s\n", dump_planes_path);
        } else {
            fprintf(stderr, "WARNING: could not write dump-planes to %s\n", dump_planes_path);
        }
    }

    free(bank);
    free(b);
    return assert_fail ? 1 : 0;
}
