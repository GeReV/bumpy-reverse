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

/* vec_resource_table (203b:0932) load order — see docs/data-files.md.  Slots 5–6 are
 * the documented empty (type 0x7a) entries; 3/4 are the BNK/MID audio resources (not
 * images, but kept so the index→file mapping is 1:1 with the engine table). */
static const char * const hr_vec_files[18] = {
    "MASKBUMP.VEC", "BUMPRESE.VEC", "SCORE.VEC", "BUMPY.BNK", "BUMPY.MID",
    (const char *)0, (const char *)0,
    "MONDE1.VEC", "MONDE2.VEC", "MONDE3.VEC", "MONDE4.VEC", "MONDE5.VEC",
    "MONDE6.VEC", "MONDE7.VEC", "MONDE8.VEC", "MONDE9.VEC",
    "DESSFIN.VEC", "TITRE.VEC"
};

/* The screen-image far buffer (DGROUP 0x7926/0x7928).  The engine allocates this at
 * boot; the reconstruction only READS it (fullscreen_buf/_seg are never assigned), so
 * in the playable build it was a null/garbage far pointer — fine while the resource
 * loader was a NOP, but once read_chunked/vec_decode actually write into it they
 * smashed low memory (corrupt MCB → triple fault).  Back it with a host static buffer.
 * Size: the raw .VEC (≤ VEC_FILE_MAX) loads at +0 and the decoded plane-major raster is
 * laid back at +99 (..+99+32000), so the buffer must hold max(VEC_FILE_MAX, 99+32000). */
extern u16 fullscreen_buf;       /* DGROUP 0x7926 — image buffer offset */
extern u16 fullscreen_buf_seg;   /* DGROUP 0x7928 — image buffer segment */
#define HR_FSBUF_BYTES 0x8800UL   /* 34816 ≥ max(VEC_FILE_MAX 33792, 99+32000 32099) */

void host_screens_buf_init(void)
{
    /* SEGMENT-ALIGNED allocation (offset 0): DOS INT 21h/3Fh wraps/truncates a read
     * whose buffer offset + length crosses the 64 KB segment boundary, so a high
     * static-array offset silently corrupts the load.  halloc returns a paragraph-
     * aligned huge block (offset 0), so a single ≤64000-byte read into it never wraps. */
    u8 __huge *p = (u8 __huge *)halloc(HR_FSBUF_BYTES, 1u);
    if (p != (u8 __huge *)0) {
        fullscreen_buf     = 0u;                                /* offset 0 (aligned) */
        fullscreen_buf_seg = (u16)((u32)((void __far *)p) >> 16);
    }
}

/* host: the descriptor table is the static filename array above; nothing to point at. */
void set_resource_table(u16 off, u16 seg)
{
    (void)off; (void)seg;
}

/* Open a shared .VEC/.BNK resource by its vec_resource_table index, via real DOS I/O. */
int open_resource(u16 res_idx, u16 mode)
{
    (void)mode;
    if (res_idx < 18u && hr_vec_files[res_idx] != (const char *)0) {
        return dosio_open_read(hr_vec_files[res_idx]);
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
