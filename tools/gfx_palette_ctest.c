/* Host unit test for src/gfx_palette.c — Task 3 (the 6 graphics-overlay palette
 * handlers, 1ab9:0605/0606/0620/0661/0662/0677 + gfx_page_slot_offset 1ab9:05b6).
 *
 * Compiles the REAL reconstructed handlers (src/gfx_palette.c) on the host — the
 * Open Watcom 16-bit environment shimmed out (__far/__huge/__cdecl16near erased,
 * exact-width typedefs, BUMPY_H so headers don't pull <dos.h>/<conio.h>) — and
 * asserts the PURE-MEMORY logic: the per-page draw-object slot offset
 * (gfx_page_slot_offset) and the two STAGE handlers (gfx_stage_palette_ega /
 * _vga), which copy a fixed-size block from a source buffer into
 * `*gfx_draw_object + page*99 + {0x23,0x33}` — keyed by page, so also checks the
 * OTHER page's slot is left untouched.
 *
 * The UPLOAD handlers (gfx_upload_palette_ega/_vga/_cga) issue real INT 10h /
 * DAC port I/O — there is no hardware on the host to assert against.  They are
 * smoke-called under the no-op I/O shims below (proves they compile, link, and
 * run to completion under the host int86x/outp/inp stand-ins) but their output
 * is NOT asserted here; Task 7's capture-based comparator is the real gate for
 * their (port,value) sequences.  Faking a port-I/O assertion here would violate
 * the "adhere to the binary — never invent" rule as surely as inventing the
 * handler bodies would.
 *
 * Build/run:
 *     cc -O2 -Wall -o /tmp/claude/gfxpal tools/gfx_palette_ctest.c && \
 *       /tmp/claude/gfxpal
 * Exit 0 iff every assert passes; prints "gfx_palette_ctest: PASS".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ── shim the Watcom 16-bit environment for host compilation ─────────────────── */
#define BUMPY_H            /* gfx_palette.h/screens.h's "bumpy.h" include becomes a no-op */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge
#define __cdecl16near

/* MK_FP / FP_OFF / FP_SEG host model (mirrors tools/int8_ctest.c): a >256 KB linear
 * "far memory" shadow indexed by the real-mode linear address, so obj + offset
 * pointer arithmetic inside the handlers lands in host-observable memory. */
#define FAR_MEM_SIZE 0x110000UL
static unsigned char far_mem[FAR_MEM_SIZE];
#define MK_FP(seg, off) ((void *)(far_mem + (((u32)(seg) << 4) + (u16)(off))))
static u16 host_fp_seg(const void *p) { return (u16)(((u32)((const unsigned char *)p - far_mem)) >> 4); }
static u16 host_fp_off(const void *p) { return (u16)(((u32)((const unsigned char *)p - far_mem)) & 0xF); }
#define FP_SEG(p) host_fp_seg((const void *)(p))
#define FP_OFF(p) host_fp_off((const void *)(p))

/* ── HOST I/O SHIMS — the UPLOAD handlers' port-I/O + INT10h primitives ──────────
 * No hardware on the host: outp/inp are no-ops (still valid expressions, in case a
 * future body uses their return value); int86x/segread are minimal stand-ins that
 * do nothing but let the call resolve.  union REGS / struct SREGS mirror just the
 * fields gfx_upload_palette_ega touches (x.ax/x.dx, SREGS.es). */
#define outp(port, val) ((void)(port), (void)(val))
#define inp(port)       ((void)(port), 0u)

struct gfx_pal_wordregs { u16 ax, bx, cx, dx, si, di, cflag, flags; };
struct gfx_pal_byteregs { u8 al, ah, bl, bh, cl, ch, dl, dh; };
union REGS { struct gfx_pal_wordregs x; struct gfx_pal_wordregs w; struct gfx_pal_byteregs h; };
struct SREGS { u16 es, cs, ss, ds; };
static void segread(struct SREGS *sr) { sr->es = sr->cs = sr->ss = sr->ds = 0u; }
static int int86x(int intr, const union REGS *in, union REGS *out, struct SREGS *sr)
{
    (void)intr; (void)in; (void)out; (void)sr;
    return 0;
}

/* screens.h declares `extern u16 palette_mode;` (DGROUP 0x541d) — gfx_stage_palette_vga's
 * +0x30 branch reads it.  We own the storage here (screens.c is not pulled in). */
u16 palette_mode;

/* ── compile the real reconstructed handlers ──────────────────────────────────── */
#include "../src/gfx_palette.c"

/* ── test scaffolding ─────────────────────────────────────────────────────────── */
#define OBJ_SEG   0x2000u   /* fake draw-object: far_mem linear 0x20000 */
#define OBJ_OFF   0x0000u
#define OBJ_LIN   0x20000UL
#define SRCBUF_LEN 0x70u    /* >= 0x33 + 48 = 0x63, room for the VGA block */

static void fill_src(u8 *buf, u8 seed)
{
    unsigned i;
    for (i = 0; i < SRCBUF_LEN; i++) { buf[i] = (u8)(seed + i * 7u + 3u); }
}

int main(void)
{
    u8 srcbuf[SRCBUF_LEN];
    unsigned char *obj;
    int off0, off1;

    memset(far_mem, 0, sizeof(far_mem));
    gfx_draw_object_off = OBJ_OFF;
    gfx_draw_object_seg = OBJ_SEG;
    obj = far_mem + OBJ_LIN;

    /* gfx_page_slot_offset — 1ab9:05b6: page * 99. */
    off0 = gfx_page_slot_offset(0u);
    off1 = gfx_page_slot_offset(1u);
    assert(off0 == 0);
    assert(off1 == 99);

    /* ── gfx_stage_palette_ega (1ab9:0606) ────────────────────────────────────
     * stage src[0x23..0x32] (16B) into page-1's slot; page-0's slot (still all
     * zero) must be untouched — proves the copy is genuinely keyed by page*99,
     * not a hard-coded offset. */
    fill_src(srcbuf, 0x11u);
    gfx_stage_palette_ega(srcbuf, 1u);
    assert(memcmp(obj + off1 + 0x23, srcbuf + 0x23, 16) == 0);
    {
        static const u8 zero16[16] = { 0 };
        assert(memcmp(obj + off0 + 0x23, zero16, 16) == 0);
    }

    /* ── gfx_stage_palette_vga (1ab9:0620), palette_mode != 5 ──────────────────
     * stage src[0x33..0x62] (48B) into page-0's slot at +0x33 (not +0x63). */
    palette_mode = 2u;   /* the boot VGA mode; != 5, so the +0x30 patch is inert */
    fill_src(srcbuf, 0x55u);
    gfx_stage_palette_vga(srcbuf, 0u);
    assert(memcmp(obj + off0 + 0x33, srcbuf + 0x33, 48) == 0);

    /* ── gfx_stage_palette_vga (1ab9:0620), palette_mode == 5 ──────────────────
     * the +0x30 patch: lands at +0x63, NOT +0x33 (fresh page-1 slot, untouched by
     * the palette_mode!=5 case above since that used page 0). */
    palette_mode = 5u;
    fill_src(srcbuf, 0x99u);
    gfx_stage_palette_vga(srcbuf, 1u);
    assert(memcmp(obj + off1 + 0x33 + 0x30, srcbuf + 0x33, 48) == 0);
    {
        static const u8 zero48[48] = { 0 };
        /* the un-patched +0x33 slot for page 1 must still be zero (never written
           by the palette_mode==5 call — it wrote +0x63, not +0x33). */
        assert(memcmp(obj + off1 + 0x33, zero48, 48) == 0);
    }

    /* ── gfx_stage_palette_cga (1ab9:0605) — bare RET: must NOT touch memory ──── */
    {
        unsigned char snapshot[256];
        memcpy(snapshot, obj, sizeof(snapshot));
        gfx_stage_palette_cga(srcbuf, 0u);
        assert(memcmp(obj, snapshot, sizeof(snapshot)) == 0);
    }

    /* ── UPLOAD handlers — smoke only (no hardware to assert against; see the
     * file header comment + task-3-report.md).  Proves they compile, link, and
     * run to completion under the host int86x/outp/inp shims. */
    gfx_upload_palette_cga(0u);
    gfx_upload_palette_ega(0u);
    gfx_upload_palette_vga(0u);

    /* ── Task 4: gfx_stage_palette_dispatch — the palette_mode-keyed switch that
     * mirrors cmdvec_stage_palette_modes[palette_mode] (1ab9:0605/0606/0620). ─────
     * Uses fresh pages (2/3/4) so these asserts can't be satisfied by residue
     * left behind by the direct-handler tests above. */
    {
        int off2 = gfx_page_slot_offset(2u);   /* 198 */
        int off3 = gfx_page_slot_offset(3u);   /* 297 */
        int off4 = gfx_page_slot_offset(4u);   /* 396 */

        /* mode 0 -> CGA no-op: dispatch must NOT touch the page-2 slot at all. */
        {
            unsigned char snapshot[256];
            memcpy(snapshot, obj + off2, sizeof(snapshot));
            palette_mode = 0u;
            fill_src(srcbuf, 0xaau);
            gfx_stage_palette_dispatch(srcbuf, 2u);
            assert(memcmp(obj + off2, snapshot, sizeof(snapshot)) == 0);
        }

        /* mode 1 -> EGA: dispatch must stage the 16B AC block at page-2's +0x23. */
        palette_mode = 1u;
        fill_src(srcbuf, 0x77u);
        gfx_stage_palette_dispatch(srcbuf, 2u);
        assert(memcmp(obj + off2 + 0x23, srcbuf + 0x23, 16) == 0);

        /* mode 2 -> VGA: dispatch must stage the 48B RGB block at page-3's +0x33. */
        palette_mode = 2u;
        fill_src(srcbuf, 0x33u);
        gfx_stage_palette_dispatch(srcbuf, 3u);
        assert(memcmp(obj + off3 + 0x33, srcbuf + 0x33, 48) == 0);

        /* default (mode 5, which is also gfx_stage_palette_vga's +0x30 special
         * case) -> VGA: proves the switch's `default:` arm, not just case 2u. */
        palette_mode = 5u;
        fill_src(srcbuf, 0xccu);
        gfx_stage_palette_dispatch(srcbuf, 4u);
        assert(memcmp(obj + off4 + 0x33 + 0x30, srcbuf + 0x33, 48) == 0);
    }

    /* ── Task 4: gfx_upload_palette_dispatch — smoke-call every mode (no hardware
     * to assert against; same rationale as the direct UPLOAD handlers above). */
    palette_mode = 0u; gfx_upload_palette_dispatch(0u);
    palette_mode = 1u; gfx_upload_palette_dispatch(0u);
    palette_mode = 2u; gfx_upload_palette_dispatch(0u);
    palette_mode = 5u; gfx_upload_palette_dispatch(0u);

    printf("gfx_palette_ctest: PASS\n");
    return 0;
}
