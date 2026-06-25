/* ════════════════════════════════════════════════════════════════════════════
 * host_resource.c — playable host: resource loader + .VEC decode (Plan A, render).
 *
 * The default build stubs the whole resource pipeline (open_resource / read_chunked /
 * c_close / set_resource_table / vec_decode are faithful-signature NOPs in screens.c
 * + game_stubs.c), so the playable build composed an UNINITIALISED fullscreen_buf into
 * the framebuffer → RGB noise on every title/menu/text screen.  This file un-stubs the
 * pipeline for the playable build so those .VEC backgrounds actually load + decode.
 *
 * RECONSTRUCTION FIDELITY (documented divergence):
 *   - The engine's open_resource (1000:…) walks a DGROUP `vec_res` descriptor table
 *     (resource_table_ptr + index*10 → name_off/name_seg far-ptr filename + a disk_id
 *     byte at +4 for the QUELDISK floppy copy-protection), then c_open()s the file.
 *     The host shortcuts the descriptor-table + disk-swap indirection to a static
 *     filename table (the documented vec_resource_table @203b:0932 load order,
 *     docs/data-files.md) and the real DOS file I/O in dosio.c.  No copy-protection
 *     prompt (the data is mounted, present).
 *   - vec_decode is the engine's .VEC interpreter; for the fullscreen raster
 *     backgrounds it RLE-decodes to a 16-colour planar image + palette.  The host
 *     drives the already-reconstructed vec_decode_planar (vec.c) and lays the result
 *     out where the screen builders expect it: palette at buf+0x33 (51, read by
 *     upload_vga_dac_palette) and the 4×8000 plane-major raster at buf+99 (the source
 *     the title/menu restore_bg_view descriptor points at, via host_compose_bg_view).
 *   - read_chunked mirrors the engine's xfer_chunked (≤64000-byte chunks to EOF).
 *
 * Recorded in docs/reconstruction-fidelity.md ("playable host" section).
 * ════════════════════════════════════════════════════════════════════════════ */
#ifdef BUMPY_PLAYABLE

#include "../bumpy.h"
#include "../dosio.h"
#include "../vec.h"
#include <dos.h>        /* MK_FP, FP_SEG */
#include <malloc.h>     /* halloc */
#include <string.h>     /* _fmemcpy */

/* The engine keeps ONE contiguous resource descriptor array in DGROUP and switches
 * which BASE entry open_resource indexes from via set_resource_table(off, seg):
 *   off 0x932 = vec_resource_table   — base entry = MASKBUMP.VEC  (level/shared images)
 *   off 0x928 = the title/menu table — base entry = BUMSPJEU.BIN  (= 0x932 minus ONE
 *               10-byte entry), so e.g. init_title_graphics open_resource(2)=BUMPRESE
 *               and run_main_menu open_resource(0x12)=TITRE.VEC.
 * hr_full_files models that array from its earliest base (BUMSPJEU at the 0x928 slot 0);
 * the 0x932 base is the same array indexed one entry later.  See docs/data-files.md.
 * Slots are 1:1 with the engine table (the two NULLs are the documented empty type-0x7a
 * entries; the BNK/MID audio resources are kept so the index map stays exact). */
static const char * const hr_full_files[19] = {
    /* 0x928[0] */ "BUMSPJEU.BIN",
    /* 0x932[0] */ "MASKBUMP.VEC", "BUMPRESE.VEC", "SCORE.VEC", "BUMPY.BNK", "BUMPY.MID",
                   (const char *)0, (const char *)0,
                   "MONDE1.VEC", "MONDE2.VEC", "MONDE3.VEC", "MONDE4.VEC", "MONDE5.VEC",
                   "MONDE6.VEC", "MONDE7.VEC", "MONDE8.VEC", "MONDE9.VEC",
                   "DESSFIN.VEC", "TITRE.VEC"
};
/* Base index into hr_full_files for the active table.  Default = the 0x932 vec table
 * (base 1); set_resource_table(0x928,..) selects the title/menu base (0). */
#define HR_BASE_VEC   1   /* off 0x932 */
#define HR_BASE_TITLE 0   /* off 0x928 */
static u16 hr_base_idx = HR_BASE_VEC;

/* The screen-image far buffer (DGROUP 0x7926/0x7928).  The engine allocates this at
 * boot; the reconstruction only READS it (fullscreen_buf/_seg are never assigned), so
 * in the playable build it was a null/garbage far pointer — fine while the resource
 * loader was a NOP, but once read_chunked/vec_decode actually write into it they
 * smashed low memory (corrupt MCB → triple fault).  Back it with real memory.
 * Size: the raw .VEC (≤ VEC_FILE_MAX) loads at +0 and the decoded plane-major raster is
 * laid back at +99 (..+99+32000), so the buffer must hold max(VEC_FILE_MAX, 99+32000). */
extern u16 fullscreen_buf;       /* DGROUP 0x7926 — image buffer offset */
extern u16 fullscreen_buf_seg;   /* DGROUP 0x7928 — image buffer segment */
extern u8 __huge *host_framebuffer;   /* host_render.c — 4 × 64 KB plane image */
#define HR_FSBUF_BYTES 0x8800UL   /* 34816 ≥ max(VEC_FILE_MAX 33792, 99+32000 32099) */

/* The framebuffer is 4 × 0x10000 (256 KB) but only ~16 KB of each 64 KB plane is used:
 * page 0 at [0..0x1f40] and page 1 at [0x2000..0x3f40], so [0x4000..0x10000) (48 KB) of
 * plane 0 is permanently unused.  fullscreen_buf lives there — a paragraph- (and here
 * 0x4000-byte / segment-) aligned window inside plane 0, NO extra heap allocation.  The
 * 8 KB of conventional memory left over the framebuffer cannot fit a separate 34 KB
 * halloc, so reusing the slack is what makes both fit.  clear_viewport is bounded to the
 * display extent [0..0x4000) per plane so it never touches this window, and the
 * compose/present paths only read/write [0..0x3f40], so fullscreen_buf is never clobbered. */
#define HR_FSBUF_PLANE0_OFF_PARA 0x400u   /* 0x4000 bytes into plane 0 (in paragraphs) */

/* The sprite-bank read target.  init_title_graphics does
 *   read_chunked(handle, 0xa0c6, 0xa0c8, ...)  // the engine's screen_sprite_buf
 * passing the DGROUP literals 0xa0c6/0xa0c8 as the buffer off/seg.  In the engine those
 * name a DGROUP-resident far pointer to a real buffer; as a literal seg:off they land in
 * VGA memory (0xa0c8:0xa0c6), so an 89 KB BUMSPJEU.BIN read there smashes memory.
 * process_sprites is a NOP host-side, so the loaded sprite bank is never consumed — the
 * host therefore DRAINS the file into a tiny reusable discard buffer (see read_chunked)
 * instead of backing a real ≥89 KB block.  A real backing block (96 KB) starves
 * host_fb_init's 256 KB framebuffer halloc (conventional memory is tight — see main.c),
 * which would NULL the framebuffer and make present_frame a silent NOP → blank screen. */
#define HR_SPRITE_BUF_SEG_MARK 0xa0c8u   /* the literal buf_seg the sprite read passes */
static u8 __far hr_discard[0x1000];      /* 4 KB: drains the unused sprite-bank read */

void host_screens_buf_init(void)
{
    /* Point fullscreen_buf at plane 0's unused slack inside the (already allocated)
     * framebuffer — offset 0 in a segment 0x400 paragraphs (0x4000 bytes) above the
     * framebuffer base.  SEGMENT-ALIGNED (offset 0): DOS INT 21h/3Fh wraps/truncates a
     * read whose buffer offset + length crosses the 64 KB segment boundary; a 0x8800-byte
     * read at offset 0 stays within plane 0's 64 KB span (ends at 0xc800 < 0x10000). */
    if (host_framebuffer != (u8 __huge *)0) {
        u16 base_seg = (u16)((u32)((void __far *)host_framebuffer) >> 16);
        fullscreen_buf     = 0u;
        fullscreen_buf_seg = (u16)(base_seg + HR_FSBUF_PLANE0_OFF_PARA);
    } else {
        /* Fallback (framebuffer alloc failed): a separate huge block, so the resource
         * loader still has a valid target instead of writing to a null far pointer. */
        u8 __huge *p = (u8 __huge *)halloc(HR_FSBUF_BYTES, 1u);
        if (p != (u8 __huge *)0) {
            fullscreen_buf     = 0u;
            fullscreen_buf_seg = (u16)((u32)((void __far *)p) >> 16);
        }
    }
}

/* Select the active resource-table base from the engine's DGROUP table offset, so
 * open_resource's index maps to the right file (the title/menu screens switch to the
 * 0x928 base, where the indices are shifted one entry from the 0x932 vec table). */
void set_resource_table(u16 off, u16 seg)
{
    (void)seg;
    if (off == 0x928u) {
        hr_base_idx = HR_BASE_TITLE;
    } else if (off == 0x932u) {
        hr_base_idx = HR_BASE_VEC;
    }
    /* Other offsets (e.g. 0x90 copy-protection level-header table) are not used on the
     * playable host's image-load path; leave the current base unchanged. */
}

/* Open a shared .VEC/.BNK resource by its index into the ACTIVE table, via real DOS I/O. */
int open_resource(u16 res_idx, u16 mode)
{
    u16 idx = (u16)(hr_base_idx + res_idx);

    (void)mode;
    if (idx < 19u && hr_full_files[idx] != (const char *)0) {
        return dosio_open_read(hr_full_files[idx]);
    }
    return -1;
}

/* Read the whole file into the far buffer in ≤64000-byte chunks; return the total.
 * (.VEC files are all < 64 KB so this is a single chunk in practice.) */
u32 read_chunked(int handle, u16 buf_off, u16 buf_seg, u16 len_lo, u16 len_hi)
{
    u8 __far *buf = (u8 __far *)MK_FP(buf_seg, buf_off);
    u32 total = 0;
    int n;

    (void)len_lo; (void)len_hi;   /* engine length cap — host reads to EOF */

    /* Sprite-bank read (init_title_graphics): the literal seg 0xa0c8 target is VGA
     * memory and process_sprites is a NOP, so drain the file to EOF into the small
     * discard buffer (no advance) rather than writing it anywhere live. */
    if (buf_seg == HR_SPRITE_BUF_SEG_MARK) {
        for (;;) {
            n = dosio_read(handle, (u8 __far *)hr_discard, (u16)sizeof(hr_discard));
            if (n <= 0) {
                break;
            }
            total += (u32)n;
            if (n < (int)sizeof(hr_discard)) {
                break;
            }
        }
        return total;
    }

    for (;;) {
        n = dosio_read(handle, buf, 0xFA00u);   /* 64000 */
        if (n <= 0) {
            break;
        }
        total += (u32)n;
        if (n < 0xFA00) {
            break;
        }
        /* advance the far pointer by 64000 bytes (segment carry) for >64KB reads. */
        buf = (u8 __far *)(buf + 0xFA00u);
    }
    return total;
}

void c_close(int handle)
{
    (void)dosio_close(handle);
}

/* Scratch decode targets (kept out of DGROUP; large model places them in their own
 * segment).  Decode to scratch first, then lay the result into the caller's buffer. */
static u8 __far hr_planar[VEC_PLANAR];     /* 32000 — 4 plane-major planes */
static u8       hr_pal[VEC_PAL_BYTES];     /* 48 — 16×RGB-6bit */

/* RLE-decode the loaded .VEC raster in `buf` into planar + palette, written back at
 * the engine-expected offsets (palette @ +0x33, plane-major raster @ +99). */
void vec_decode(u16 buf_off, u16 buf_seg, u32 size, u16 arg, u16 flag)
{
    const u8 *raw = (const u8 *)MK_FP(buf_seg, buf_off);

    (void)arg; (void)flag;
    if (vec_decode_planar(raw, (u16)size, hr_planar, hr_pal) != 0xffffu) {
        _fmemcpy((u8 __far *)MK_FP(buf_seg, (u16)(buf_off + VEC_PAL_OFF)),
                 (u8 __far *)hr_pal, VEC_PAL_BYTES);
        _fmemcpy((u8 __far *)MK_FP(buf_seg, (u16)(buf_off + VEC_HDR_BYTES)),
                 (u8 __far *)hr_planar, VEC_PLANAR);
    }
}

#endif /* BUMPY_PLAYABLE */
