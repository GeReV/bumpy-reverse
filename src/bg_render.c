#include "bg_render.h"
#ifdef BUMPY_PLAYABLE
#include "host/host.h"          /* host_vga_rmw4 / put4 / clear4 / blit_end */
#endif

/* See bg_render.h.  Mirrors restore_bg_tile_run's run-loop + the reconstructed
   masked planar tile blit (validated byte-exact by tools/bg_blit_ref.py). */

/* Plane stride — must match the host framebuffer (HOST_PLANE_SIZE).  HOST_FB_16K
   (playable EXE) → 16 KB/plane; default build + ctests → full 64 KB (byte-unchanged). */
#ifdef HOST_FB_16K
#define PLANE_SIZE 0x4000UL
#else
#define PLANE_SIZE 0x10000UL
#endif
#define ATLAS_PSZ  (40UL * 192UL)      /* atlas plane size (320/8 * 192) */
#define ROW_BYTES  40                  /* VGA bytes per scanline (320 px) */

static void blit_tile(u8 __huge *planes, const u8 __huge *atlas,
                      u8 tile_id, u16 cell_x, u16 cell_y, int masked)
{
    u16 acol = (u16)(tile_id % 20);
    u16 arow = (u16)(tile_id / 20);
    u16 ry;
    u8 bx;
#ifndef BUMPY_PLAYABLE
    u8 p;   /* plane index — flat-buffer store only; VGA path uses host_vga_* */
#endif
    /* The last grid row (cell_y==24) is the engine's restore_bg_tile_run
       descriptor +0x20==1 case: the 16-row tile is clipped to the 200-row screen
       (rows 200..207 are off-screen), so only 200 - cell_y*8 rows are drawn. */
    u16 rows_drawn = 16;

    if ((u16)(cell_y * 8 + 16) > 200) {
        rows_drawn = (u16)(200 - cell_y * 8);
    }
    for (ry = 0; ry < rows_drawn; ry++) {
        u16 srow = (u16)(arow * 16 + ry);
        u16 drow = (u16)(cell_y * 8 + ry);
        u32 drow_off = (u32)drow * ROW_BYTES;
        for (bx = 0; bx < 2; bx++) {
            u32 doff = drow_off + (u32)cell_x * 2 + bx;
            u32 soff = (u32)srow * ROW_BYTES + (u32)acol * 2 + bx;
            u8 v0 = atlas[soff];
            u8 v1 = atlas[soff + ATLAS_PSZ];
            u8 v2 = atlas[soff + 2 * ATLAS_PSZ];
            u8 v3 = atlas[soff + 3 * ATLAS_PSZ];
            if (masked) {
                u8 mask = (u8)(v0 | v1 | v2 | v3);
#ifdef BUMPY_PLAYABLE
                /* Faithful real-VGA masked store (mode-01 tile blit, 1ab9:0aa0):
                   bit-mask = coverage so set pixels overwrite, clear keep the bg.
                   Target the CURRENT draw page (live table; flips per present). */
                host_vga_rmw4((u16)(host_draw_page_off() + (u16)doff), v0, v1, v2, v3, mask);
#else
                u8 inv = (u8)~mask;
                planes[0 * PLANE_SIZE + doff] = (u8)((v0 & mask) | (planes[0 * PLANE_SIZE + doff] & inv));
                planes[1 * PLANE_SIZE + doff] = (u8)((v1 & mask) | (planes[1 * PLANE_SIZE + doff] & inv));
                planes[2 * PLANE_SIZE + doff] = (u8)((v2 & mask) | (planes[2 * PLANE_SIZE + doff] & inv));
                planes[3 * PLANE_SIZE + doff] = (u8)((v3 & mask) | (planes[3 * PLANE_SIZE + doff] & inv));
#endif
            } else {
#ifdef BUMPY_PLAYABLE
                host_vga_put4((u16)(host_draw_page_off() + (u16)doff),
                              v0, v1, v2, v3);              /* opaque base tile */
#else
                planes[0 * PLANE_SIZE + doff] = v0;
                planes[1 * PLANE_SIZE + doff] = v1;
                planes[2 * PLANE_SIZE + doff] = v2;
                planes[3 * PLANE_SIZE + doff] = v3;
#endif
            }
        }
        if (!masked) {
            /* opaque base also clears 2 bytes past the tile (clipped at edge) */
            for (bx = 2; bx < 4; bx++) {
                if (cell_x * 2 + bx >= ROW_BYTES) {
                    continue;
                }
                {
                    u32 doff = drow_off + (u32)cell_x * 2 + bx;
#ifdef BUMPY_PLAYABLE
                    host_vga_clear4((u16)(host_draw_page_off() + (u16)doff));
#else
                    for (p = 0; p < 4; p++) {
                        planes[(u32)p * PLANE_SIZE + doff] = 0;
                    }
#endif
                }
            }
        }
    }
#ifdef BUMPY_PLAYABLE
    (void)planes;   /* VGA path writes a000 directly */
#endif
}

void bg_tile_run(u8 __huge *planes, const u8 __huge *atlas, const u8 __far *map,
                 u8 run_code, u16 cell_x, u16 cell_y)
{
    u8 col_idx;
    u8 col_count;
    u32 base = (u32)cell_x * 0x27 + (u32)(cell_y >> 1) * 3 + 0x20;
    int sub = 0;

    if (run_code < 0xf1) {
        col_idx = 0;
        col_count = 1;
    } else {
        col_idx = 1;
        col_count = (u8)(((u8)-run_code) - 5);
    }
    for (; col_idx < col_count; col_idx++) {
        u8 code = map[base + col_idx];
        u8 tile_id = (u8)(code - 1);
        blit_tile(planes, atlas, tile_id, cell_x, cell_y, sub > 0);
        sub++;
    }
}

void bg_render_grid(u8 __huge *planes, const u8 __huge *atlas, const u8 __far *map)
{
    u16 cell_x;
    u16 cell_y;

    for (cell_y = 0; cell_y <= 24; cell_y += 2) {
        for (cell_x = 0; cell_x < 20; cell_x++) {
            u8 run_code = map[(u32)cell_x * 0x27 + (u32)(cell_y >> 1) * 3 + 0x20];
            /* redraw_level_background_tiles@1000_2a0a guards the tile-run on a
               nonzero run_code; run_code==0 (empty cell) is left untouched.
               Without this guard bg_tile_run reads code=map[base]=0 -> tile_id
               0xFF and opaquely blits a bogus out-of-atlas tile (col15/row12). */
            if (run_code != 0) {
                bg_tile_run(planes, atlas, map, run_code, cell_x, cell_y);
            }
        }
    }
#ifdef BUMPY_PLAYABLE
    host_vga_blit_end();   /* restore default VGA write state after the grid */
#endif
}
