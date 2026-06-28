/*
 * level.c — start_level() and level-1 render pipeline.
 *
 * Port of start_level (1000:2d14) from the Ghidra decomp
 * (local/build/slice_decomp.txt lines 2162-2214).
 *
 * Phase-1 Task 4 deliverable: COMPILE + LINK.  Render correctness (the decode
 * byte-output and the final composited frame) is DEFERRED to Task 7 — see the
 * decode-path note (#2) below.  This module is structurally faithful and wires
 * the validated Phase-0 render API, but its runtime output is NOT validated here.
 *
 * ── RECONSTRUCTION FIDELITY NOTES ────────────────────────────────────────
 *
 * 1. PARAMETERISATION: original start_level() is parameterless and reads the
 *    global current_level directly.  We add (world, level) for clarity and
 *    write current_level = level at entry, keeping the guard logic identical.
 *
 * 2. FILE LOADING + DECODE (the part the Task-4 brief corrects): the original
 *    uses open_resource(n, 4) + read_chunked + c_close + vec_decode with
 *    far-pointer buffer pairs (buf/seg).  The clean Phase-0 API exposes no
 *    `vec_decode_to_arena` wrapper (it does NOT exist).  Instead we mirror, in
 *    a local level_decode_file(), exactly what vec.c's op12 path does: stream
 *    the raw file into the shared g_op12_arena, parse the 12-byte big-endian
 *    record header faithfully, then run op12_vec_run(g_op12_arena,
 *    VEC_DECODE_MAX, n) (op12_port DECLARED_LEN / file-len payload bound).
 *    The decoded image is left at g_op12_arena[0..]; the caller copies out the
 *    bytes it needs.  RECONSTRUCTION FIDELITY: this decode is structurally
 *    faithful to vec_decode's inner-stream path but is NOT byte-validated in
 *    Task 4 (no DOS file I/O in the Phase-1 boot harness) — runtime decode
 *    validation is Task 7.  The header-parse mirrors vec_decode_planar exactly.
 *
 * 3. RESOURCE-TABLE PATCHING: the original calls set_resource_table(0x90,
 *    0x203b) and patches level digits in DAT_203b_0090/009a/00e0.  This
 *    table maps resource indices to filenames.  We bypass it and construct
 *    filenames directly ("D1.PAV" etc.) using the level number.
 *
 * 4. MOVE-DESCRIPTOR TABLE: the original loads move_descriptor_table ptrs
 *    from a code-segment table at 0x10c8..0x10cb and clears the list until
 *    sentinel -1.  This is runtime player-movement state, irrelevant to the
 *    initial render.  Stubbed here.
 *
 * 5. ANIM COORD TABLE: similarly, anim_coord_table_ptr and DAT_203b_8556
 *    are loaded from per-level tables in the code segment.  Stubbed here.
 *
 * 6. DGROUP SHADOW: entity_draw_* read `dg` (a u8 __far * treated as the
 *    DGROUP snapshot) at fixed offsets (0xf4, 0x3f4, 0x274, 0x792e, etc.)
 *    for posA/posB/posC tables and sprite-object structs.  In the DOS target
 *    these fields live at those exact DGROUP offsets.  In this Open Watcom
 *    build the DGROUP layout is uncontrolled, so we use a __far shadow buffer
 *    g_entity_dg[LEVEL_DG_SIZE] and populate only the fields entity.c reads.
 *
 * 7. PLANES BUFFER: bg_render_grid and entity_draw_* write into a 4-plane
 *    buffer with plane p at planes[p * 0x10000].  We allocate a single huge
 *    0x40000-byte buffer to provide the required layout.  video_blit_planar
 *    takes a 32000-byte page-0 buffer; we pack page-0 (offsets 0..7999 of
 *    each plane) before blitting.
 *
 * 8. FRAMETABLE PTR: the engine seeds p1_sprite[+6..+9] (frametable far ptr)
 *    at level init.  We set it to point into g_bank_buf at the standard
 *    bank_base_lin offset by computing the far pointer from g_bank_buf's
 *    address.  This mirrors what the engine does when it loads BUMSPJEU.BIN.
 *
 * 9. SPRITE BANK LOADING: BUMSPJEU.BIN is ~87 KB, too large for a single
 *    dosio_load() call (limited to u16).  We stream it via dosio_open_read /
 *    dosio_read in 0x4000-byte chunks into g_bank_buf.
 *
 * 10. PAV DECODE BUFFER COPY: after level_decode_file the decoded PAV image
 *     lives in g_op12_arena[0..LEVEL_PAV_BUF_SIZE-1].  We copy it directly
 *     into g_pav_buf.  The atlas pointer passed to bg_render_grid is
 *     g_pav_buf + 6 (skipping the 6-byte PAV header per bg_render.h).
 *
 * 11. DEC / BUM BUFFER COPY: similarly for DEC (level grid map) and BUM
 *     (entity spawn data).
 *
 * 12. P1 POSITION: p1_start_x/y are set to 0x1f/0x1f (level 1).  The
 *     pixel position for the initial render is these values scaled to pixels.
 *     p1_move_anim = 0 (standing frame) at level load.
 *
 * 13. BANK BASE LINEAR: the engine's sprite_blit uses a linear address for
 *     the bank: bank_base_lin = (FP_SEG(g_bank_buf) << 4) + FP_OFF(g_bank_buf).
 *     For entity.c calls we compute this from the far pointer to g_bank_buf.
 *
 * STATUS: compiles and links; runtime validation against the composite oracle
 * (tools/composite_ctest.c) is deferred to Task 7 (project-owner decision).
 */

#include "level.h"

/* ── HARNESS BUILD GUARD (BUMPY_COPYPROT_HARNESS) ─────────────────────────────
   tools/copyprot_ctest.c #includes this TU to compile ONLY copyprotect_challenge
   (1000:4015) on the host.  The rest of level.c (start_level + the level-1 render
   pipeline) pulls the DOS render/op12/dosio/malloc surface, which the host cannot
   provide.  When BUMPY_COPYPROT_HARNESS is defined (ONLY by the ctest, NEVER by the
   real wcc/wmake build), the heavy includes and every non-challenge function are
   compiled out, leaving the DGROUP globals + the challenge body the harness needs.
   The DEFAULT build never sees this macro, so its output is byte-unchanged. */
#ifndef BUMPY_COPYPROT_HARNESS
#include "vec.h"          /* VEC_DECODE_MAX, vec_decode_planar */
#include "op12.h"         /* OP12_ARENA_SIZE, op12_vec_run */
#include "dosio.h"
#include "bg_render.h"
#include "entity.h"
#include "video.h"
#include "sprite_chain.h"
#include "sprite.h"
#include <string.h>
#include <dos.h>
#include <malloc.h>    /* _fmalloc, _ffree, halloc, hfree */
#ifdef BUMPY_PLAYABLE
#include "host/host.h"  /* host_fb_init / host_framebuffer / host_render_bind */
void host_render_bind(u8 __huge *bank, u32 bank_base_lin, const u8 __far *dg);
/* host_view.c needs a pointer into the entity-DG shadow at the sprite-obj offsets
 * (DG_P1_OBJ=0x792e, DG_P2_OBJ=0x795a) so that init_sprite_structs can set
 * p1_sprite/p2_sprite to point there (draw_p1/p2_sprite writes obj+0/2/4, and
 * hr_blit_obj in host_render.c reads hr_dg+0x792e — they must be the same memory).
 * g_entity_dg is static so we expose it via this minimal accessor, compiled only
 * under BUMPY_PLAYABLE (no faithful-default-build exposure). */
u8 __far *level_get_entity_dg(void);
/* host_video.c: faithful level-palette upload (load_palette → DAC).  render_level
 * calls it in the playable build so the initial level frame (and the world-map /
 * iris that precede game_loop's own apply_level_palette) use the real palette. */
void apply_level_palette(void);
#endif

/* ── extern: op12 arena (declared in bvec_buf2.c) ─────────────────────────────
   Shared 0x8000-byte decode arena.  We extern it (do NOT redefine); op12.obj
   and bvec_buf2.obj are already in the BUMPY link set. */
extern u8 __far g_op12_arena[];
#endif /* !BUMPY_COPYPROT_HARNESS */

/* ── DGROUP globals (engine originals; defined here) ─────────────────────── */
u8 current_level        = 1u;
u8 copyprotect_flag     = 0u;
u8 p1_start_x           = 0x1fu;
u8 p1_start_y           = 0x1fu;
u8 current_entity_index = 1u;

#ifndef BUMPY_COPYPROT_HARNESS   /* { heavy level pipeline — excluded from the host harness */

/* ── Level-file read buffer (small; __far to avoid DGROUP pressure) ───────────
   Raw file bytes for PAV/DEC/BUM.  OP12_ARENA_SIZE = 0x8000; all three files
   fit (max is PAV at 15071 B < 0x3c00).  We reuse a single static buffer
   across the three loads. */
static u8 __far g_filebuf[LEVEL_PAV_FILE_MAX];

/* ── Dynamically-allocated level buffers ─────────────────────────────────────
   The large buffers (planes 256 KB, bank 87 KB, dg shadow 41 KB, PAV 30 KB)
   are allocated at runtime via _fmalloc / halloc to keep the EXE small.
   Static allocation of ~470 KB would cause the EXE to exceed DOS 640 KB.

   RECONSTRUCTION FIDELITY: the original engine used fixed DGROUP/far-segment
   addresses for these buffers (placed by the TC/BC linker).  We use runtime
   allocation since Open Watcom's linker cannot guarantee the same fixed
   addresses, and static large-BSS allocation inflates the EXE beyond 640 KB. */
static u8 __huge *g_planes    = (u8 __huge *)0;  /* 4 * 0x10000 = 256 KB     */
static u8 __huge *g_bank_buf  = (u8 __huge *)0;  /* BUMSPJEU.BIN ~87 KB      */
static u8 __far  *g_pav_buf   = (u8 __far  *)0;  /* decoded PAV atlas 0x7806 */
static u8 __far  *g_dec_buf   = (u8 __far  *)0;  /* decoded DEC map  0x2000  */
static u8 __far  *g_bum_buf   = (u8 __far  *)0;  /* decoded BUM data 0x0400  */
static u8 __far  *g_entity_dg = (u8 __far  *)0;  /* entity dg shadow 0xa200  */

/* ── Page-0 pack buffer for video_blit_planar ─────────────────────────────────
   video_blit_planar takes a 32000-byte plane-major buffer: plane p at [p*8000].
   We copy page-0 (offsets 0..7999 in each plane of g_planes) into this buffer
   before the final blit to VGA hardware. */
static u8 g_page0[32000u];

/* ── big-endian word read (mirrors vec.c's be16) ──────────────────────────── */
static u16 level_be16(const u8 __far *p, u16 off)
{
    return (u16)(((u16)p[off] << 8) | (u16)p[off + 1u]);
}

/* ────────────────────────────────────────────────────────────────────────────
   level_alloc_buffers — allocate all large level buffers at runtime.

   Called once from start_level before any file loading.
   Returns 0 on success, -1 on allocation failure.

   RECONSTRUCTION FIDELITY: the original engine had these buffers at fixed
   DGROUP/far-segment addresses (placed by TC linker).  We allocate at runtime
   because static large BSS inflates the EXE beyond DOS 640 KB.
   ─────────────────────────────────────────────────────────────────────────── */
static int level_alloc_buffers(void)
{
    /* VGA planes shadow: 4 * 0x10000 = 0x40000 bytes = 256 KB.
       halloc(count, size) allocates huge memory via DOS INT 21h AH=48h. */
#ifdef BUMPY_PLAYABLE
    /* Playable host build (Plan A Task 2): the flat 4-plane framebuffer is owned by
       the host layer (host_render.c).  g_planes aliases host_framebuffer so the
       static level compose and the per-tick render-leaf draws (host_render.c blit
       leaves) share one RAM image, and present blits that image to real VGA. */
    if (g_planes == (u8 __huge *)0) {
        host_fb_init();
        g_planes = host_framebuffer;
        if (g_planes == (u8 __huge *)0) { return -1; }
    }
#else
    if (g_planes == (u8 __huge *)0) {
        g_planes = (u8 __huge *)halloc(0x40000UL, 1);
        if (g_planes == (u8 __huge *)0) { return -1; }
    }
#endif

    /* Sprite bank: 0x15c20 bytes = ~87 KB. */
    if (g_bank_buf == (u8 __huge *)0) {
        g_bank_buf = (u8 __huge *)halloc((u32)LEVEL_BANK_BUF_SIZE, 1);
        if (g_bank_buf == (u8 __huge *)0) { return -1; }
    }

    /* PAV atlas: 0x7806 bytes. */
    if (g_pav_buf == (u8 __far *)0) {
        g_pav_buf = (u8 __far *)_fmalloc(LEVEL_PAV_BUF_SIZE);
        if (g_pav_buf == (u8 __far *)0) { return -1; }
    }

    /* DEC level map: 0x2000 bytes. */
    if (g_dec_buf == (u8 __far *)0) {
        g_dec_buf = (u8 __far *)_fmalloc(LEVEL_DEC_BUF_SIZE);
        if (g_dec_buf == (u8 __far *)0) { return -1; }
    }

    /* BUM entity data: 0x0400 bytes. */
    if (g_bum_buf == (u8 __far *)0) {
        g_bum_buf = (u8 __far *)_fmalloc(LEVEL_BUM_BUF_SIZE);
        if (g_bum_buf == (u8 __far *)0) { return -1; }
    }

    /* Entity DG shadow: 0xa200 bytes = ~41 KB. */
    if (g_entity_dg == (u8 __far *)0) {
        g_entity_dg = (u8 __far *)_fmalloc(LEVEL_DG_SIZE);
        if (g_entity_dg == (u8 __far *)0) { return -1; }
    }

    return 0;
}

/* ────────────────────────────────────────────────────────────────────────────
   level_decode_file — load and op4+op12 decode one level resource file.

   Reads the file at `path` into g_filebuf (must be <= LEVEL_PAV_FILE_MAX bytes),
   then mirrors vec.c's inner-record-stream decode: stream the raw bytes into
   g_op12_arena, parse the 12-byte big-endian record header faithfully, and run
   op12_vec_run on the arena.  After return, g_op12_arena[0..] holds the decoded
   image; the caller copies the portion it needs into a persistent buffer.

   RECONSTRUCTION FIDELITY: this replaces the engine's vec_decode (which the
   clean Phase-0 API exposes only as the lower-level op12_vec_run + the
   public vec_decode_planar).  Structure mirrors vec_decode_planar's op12 path
   (vec.c lines ~228-251) byte-for-byte for the header parse and op12_vec_run
   invocation; NOT runtime-validated in Task 4 (no file I/O in the Phase-1 boot
   harness) — decode validation is Task 7.

   Returns the number of raw bytes read on success, 0 on error.
   ─────────────────────────────────────────────────────────────────────────── */
static u16 level_decode_file(const char *path)
{
    int fd;
    int n;
    u16 nb;
    u16 w0;
    u16 i;

    fd = dosio_open_read(path);
    if (fd < 0) {
        return 0u;
    }
    n = dosio_read(fd, (u8 __far *)g_filebuf, LEVEL_PAV_FILE_MAX);
    dosio_close(fd);
    if (n <= 0) {
        return 0u;
    }
    nb = (u16)n;

    /* The decoded image must fit the shared arena. */
    if (nb > OP12_ARENA_SIZE) {
        return 0u;
    }

    /* Faithful header parse (mirrors vec_decode_planar): the 12-byte record
       header is six big-endian words.  w0 > 0x0f terminates the stream; the
       level resources are op4/op12 inner-stream files (w0 small).  We do not
       reject on opcode here — the level files are always inner-stream records
       fed to op12_vec_run, matching the engine's vec_decode dispatch. */
    if (nb < 12u) {
        return 0u;
    }
    w0 = level_be16((const u8 __far *)g_filebuf, 0u);
    (void)w0;  /* parsed for fidelity; op12_vec_run consumes the full record */

    /* Stream raw bytes into the arena, then run the inner record loop.
       declared_len = VEC_DECODE_MAX (op12_port DECLARED_LEN);
       payload_len  = nb (op12_port's vsav seed = file size). */
    for (i = 0u; i < nb; i++) {
        g_op12_arena[i] = g_filebuf[i];
    }
    op12_vec_run(g_op12_arena, VEC_DECODE_MAX, nb);

    return nb;
}

/* ────────────────────────────────────────────────────────────────────────────
   level_copy_arena — copy the decoded arena result into a far buffer.

   Copies `bufsz` bytes from g_op12_arena into `dst`.

   RECONSTRUCTION FIDELITY: the engine places the decoded image directly in
   level_pav_buf/level_dec_buf/level_bum_buf via vec_decode + the far buffer
   pair.  Here we copy out of the shared arena into our per-file buffers.
   ─────────────────────────────────────────────────────────────────────────── */
static void level_copy_arena(u8 __far *dst, u16 bufsz)
{
    u16 i;
    for (i = 0u; i < bufsz; i++) {
        dst[i] = g_op12_arena[i];
    }
}

/* ────────────────────────────────────────────────────────────────────────────
   level_load_bank — stream BUMSPJEU.BIN into g_bank_buf.

   The file is ~87 KB, read in 0x4000-byte (16 KB) chunks via dosio_read.
   Returns total bytes read on success, 0 on error.
   ─────────────────────────────────────────────────────────────────────────── */
static u32 level_load_bank(const char *path)
{
    int fd;
    int chunk;
    u32 total;
    u8 __huge *dst;

    fd = dosio_open_read(path);
    if (fd < 0) {
        return 0UL;
    }
    dst   = g_bank_buf;
    total = 0UL;
    do {
        /* dosio_read takes a u8 __far * buf and u16 len.  Cast the __huge ptr
           to __far for each 0x4000-byte chunk; this stays within a single
           64 KB segment per chunk (0x4000 < 0x10000). */
        chunk = dosio_read(fd, (u8 __far *)dst, (u16)0x4000u);
        if (chunk <= 0) {
            break;
        }
        dst   = dst + (u16)chunk;
        total = total + (u32)(u16)chunk;
    } while (total < (u32)LEVEL_BANK_BUF_SIZE);
    dosio_close(fd);
    return total;
}

/* ────────────────────────────────────────────────────────────────────────────
   level_make_filename — build "D1.PAV" style path into buf (caller supplies
   a char[16]).  suffix is "PAV", "DEC", or "BUM".
   ─────────────────────────────────────────────────────────────────────────── */
static void level_make_filename(char *buf, u8 lev, const char *suffix)
{
    /* "D" + digit + "." + suffix NUL-terminated */
    buf[0] = 'D';
    buf[1] = (char)('0' + (int)lev);
    buf[2] = '.';
    buf[3] = suffix[0];
    buf[4] = suffix[1];
    buf[5] = suffix[2];
    buf[6] = '\0';
}

/* ────────────────────────────────────────────────────────────────────────────
   level_populate_dg — fill the entity-dg shadow with posA/B/C tables and
   sprite-object structs before a render call.

   posA:  cell X = col*40 pixels, Y = row*32+24 pixels  (layer A)
   posC:  cell X = col*40+8 pixels, Y = row*32+8 pixels (layer C)
   posB:  cell X = col*40+32 pixels, Y = row*32 pixels  (layer B)

   RECONSTRUCTION FIDELITY: the engine computes these from the per-cell
   animation descriptor tables at level init and stores them in DGROUP at
   the offsets documented in entity.c.  Here we compute them analytically
   from the grid geometry, which matches (verified in composite_ctest.c).
   The frametable far ptr for p1/p2 sprite objects is set to point at
   g_bank_buf (the loaded sprite bank).
   ─────────────────────────────────────────────────────────────────────────── */
static void level_populate_dg(void)
{
    u8  col;
    u8  row;
    u8  cell;
    u16 ftbl_off;
    u16 ftbl_seg;

    /* Zero the dg shadow before populating. */
    {
        u16 i;
        for (i = 0u; i < LEVEL_DG_SIZE; i++) {
            g_entity_dg[i] = 0u;
        }
    }

    /* posA, posC, posB tables: 6 rows × 8 cols = 48 cells. */
    for (row = 0u; row < 6u; row++) {
        for (col = 0u; col < 8u; col++) {
            u16 posa_x;
            u16 posa_y;
            u16 posc_x;
            u16 posc_y;
            u16 posb_x;
            u16 posb_y;
            u16 off;

            cell   = (u8)(row * 8u + col);
            posa_x = (u16)((u16)col * 40u);
            posa_y = (u16)((u16)row * 32u + 24u);
            posc_x = (u16)((u16)col * 40u + 8u);
            posc_y = (u16)((u16)row * 32u + 8u);
            posb_x = (u16)((u16)col * 40u + 32u);
            posb_y = (u16)((u16)row * 32u);

            off = (u16)(DG_POSA_X_BASE + (u16)cell * 4u);
            g_entity_dg[off]     = (u8)(posa_x & 0xffu);
            g_entity_dg[off + 1] = (u8)(posa_x >> 8u);
            off = (u16)(DG_POSA_Y_BASE + (u16)cell * 4u);
            g_entity_dg[off]     = (u8)(posa_y & 0xffu);
            g_entity_dg[off + 1] = (u8)(posa_y >> 8u);

            off = (u16)(DG_POSC_X_BASE + (u16)cell * 4u);
            g_entity_dg[off]     = (u8)(posc_x & 0xffu);
            g_entity_dg[off + 1] = (u8)(posc_x >> 8u);
            off = (u16)(DG_POSC_Y_BASE + (u16)cell * 4u);
            g_entity_dg[off]     = (u8)(posc_y & 0xffu);
            g_entity_dg[off + 1] = (u8)(posc_y >> 8u);

            off = (u16)(DG_POSB_X_BASE + (u16)cell * 4u);
            g_entity_dg[off]     = (u8)(posb_x & 0xffu);
            g_entity_dg[off + 1] = (u8)(posb_x >> 8u);
            off = (u16)(DG_POSB_Y_BASE + (u16)cell * 4u);
            g_entity_dg[off]     = (u8)(posb_y & 0xffu);
            g_entity_dg[off + 1] = (u8)(posb_y >> 8u);
        }
    }

    /* Frametable far ptr for p1_sprite and p2_sprite objects.
       The engine seeds obj[+6..+9] with the far ptr to the in-memory sprite
       frametable (bank_inmem.bin offset 0 = start of the table).
       We point it at g_bank_buf directly. */
    ftbl_off = FP_OFF(g_bank_buf);
    ftbl_seg = FP_SEG(g_bank_buf);

    /* p1_sprite obj at DG_P1_OBJ */
    g_entity_dg[DG_P1_OBJ + OBJ_FTBL_OFF]     = (u8)(ftbl_off & 0xffu);
    g_entity_dg[DG_P1_OBJ + OBJ_FTBL_OFF + 1] = (u8)(ftbl_off >> 8u);
    g_entity_dg[DG_P1_OBJ + OBJ_FTBL_SEG]     = (u8)(ftbl_seg & 0xffu);
    g_entity_dg[DG_P1_OBJ + OBJ_FTBL_SEG + 1] = (u8)(ftbl_seg >> 8u);

    /* p2_sprite obj at DG_P2_OBJ */
    g_entity_dg[DG_P2_OBJ + OBJ_FTBL_OFF]     = (u8)(ftbl_off & 0xffu);
    g_entity_dg[DG_P2_OBJ + OBJ_FTBL_OFF + 1] = (u8)(ftbl_off >> 8u);
    g_entity_dg[DG_P2_OBJ + OBJ_FTBL_SEG]     = (u8)(ftbl_seg & 0xffu);
    g_entity_dg[DG_P2_OBJ + OBJ_FTBL_SEG + 1] = (u8)(ftbl_seg >> 8u);

    /* p2_cell = -1 (P2 absent on level 1) */
    g_entity_dg[DG_P2_CELL] = (u8)0xffu;  /* -1 as unsigned byte */

    /* p2_frame_base = 0 */
    g_entity_dg[DG_P2_FRAME_BASE]     = 0u;
    g_entity_dg[DG_P2_FRAME_BASE + 1] = 0u;
}

/* ────────────────────────────────────────────────────────────────────────────
   render_level — render background + entity layers into g_planes and blit.

   RECONSTRUCTION FIDELITY: the original start_level does NOT render directly —
   the per-tick render is driven by game_loop via spawn_and_draw_level_entities
   + draw_p1/p2_sprite.  We invoke the validated bg_render_grid + entity_draw_*
   path here at level load to exercise the pipeline structurally.  Runtime
   pixel-correctness is deferred to Task 7.
   ─────────────────────────────────────────────────────────────────────────── */
static void render_level(void)
{
    const u8 __huge *atlas;
    const u8 __far  *map;
    const u8 __far  *bum;
    const u8 __far  *dg;
    u8 __huge       *bank;
    u32              bank_base_lin;
    sprite_view      view;
    u16              p;
    u16              i;

    /* Atlas pointer: PAV decoded data starts at g_pav_buf[6] (6-byte header
       per bg_render.h notes on the atlas layout). */
    atlas = (const u8 __huge *)g_pav_buf + 6u;

    /* Level grid map pointer: decoded DEC data at g_dec_buf. */
    map = (const u8 __far *)g_dec_buf;

    /* BUM entity data pointer: decoded BUM data at g_bum_buf. */
    bum = (const u8 __far *)g_bum_buf;

    /* DGROUP shadow (populated by level_populate_dg). */
    dg = (const u8 __far *)g_entity_dg;

    /* Sprite bank. */
    bank = g_bank_buf;
    bank_base_lin = (u32)((u32)FP_SEG(g_bank_buf) << 4u) +
                    (u32)FP_OFF(g_bank_buf);

#ifdef BUMPY_PLAYABLE
    /* Register the render context for the per-tick blit leaves (host_render.c):
       the engine's blit leaf reads bank / dg / view from globals; we hand it the
       same three inputs so anim_blit_sprite_leaf / p1_blit_sprite_leaf can re-run
       the validated blit into host_framebuffer's current draw page. */
    host_render_bind(bank, bank_base_lin, dg);
#endif

    /* Full-screen sprite viewport (left=0,right=40,top=0,bottom=199,
       height=199, data_off=0, data_seg=0xa000).
       RECONSTRUCTION FIDELITY: these globals are set by the engine before
       draw calls.  We hardcode the full-screen values here. */
    view.left     = 0;
    view.right    = 40;
    view.top      = 0;
    view.bottom   = 199;
    view.height   = 199;
    view.data_off = 0u;
    view.data_seg = 0xa000u;

    /* Clear the planes buffer before rendering. */
    {
        u32 j;
        for (j = 0UL; j < (4UL * 0x10000UL); j++) {
            g_planes[j] = 0u;
        }
    }

    /* Background grid */
    bg_render_grid(g_planes, atlas, map);

    /* Entity layers */
    entity_draw_layer_c(g_planes, bum, dg, bank, bank_base_lin, &view);
    entity_draw_layer_a(g_planes, bum, dg, bank, bank_base_lin, &view);
    entity_draw_layer_b(g_planes, bum, dg, bank, bank_base_lin, &view);

    /* Player 1: starting position, standing frame (move_anim=0) */
    entity_draw_p1(g_planes, dg,
                   (u16)p1_start_x, (u16)p1_start_y, 0u,
                   bank, bank_base_lin, &view);

    /* Player 2: absent on level 1 (p2_cell == -1; guard in entity_draw_p2) */
    entity_draw_p2(g_planes, dg,
                   0u, 0u, 0u,  /* pixel_x/y/move_anim irrelevant (P2 absent) */
                   0u,          /* frame_base */
                   (s8)-1,      /* p2_cell = -1 → no draw */
                   bank, bank_base_lin, &view);

    /* Pack page-0 (plane offsets 0..7999) into g_page0 for video_blit_planar.
       Layout: plane p at g_page0[p * 8000 .. p*8000+8000). */
    for (p = 0u; p < 4u; p++) {
        for (i = 0u; i < 8000u; i++) {
            g_page0[(u16)(p * 8000u) + i] = g_planes[(u32)p * 0x10000UL + (u32)i];
        }
    }

    /* Upload the level palette, then blit planes to VGA hardware.
       RECONSTRUCTION FIDELITY: the engine loads the palette at level init via
       load_palette_byteswapped() + load_palette() from cur_level_ptr (the decoded
       .DEC, see level_packed_palette / host_video.c).  In the playable build we run
       that faithful path (apply_level_palette → load_palette → DAC slots {0..7,
       0x10..0x17}, the BGI AC mapping).  The differential harness (non-playable)
       has no DAC/BGI model and apply_level_palette is a no-op stub, so it keeps the
       direct video_set_palette6 upload; its frame compare uses the captured palette,
       not this DAC write. */
#ifdef BUMPY_PLAYABLE
    apply_level_palette();
#else
    video_set_palette6((const u8 *)(g_pav_buf + 51u));
#endif
    video_blit_planar(g_page0);
}

#ifdef BUMPY_PLAYABLE
/* ── level_packed_palette (host accessor for cur_level_ptr) ─────────────────────
 * Returns a far pointer to the level's PACKED 16-colour palette — 16 big-endian
 * 12-bit-RGB words (32 bytes) — i.e. the engine's cur_level_ptr[0..0x1f], or NULL
 * if no level has been loaded yet.  This is the source the faithful host
 * load_palette (host_video.c) byte-swaps and decodes into the DAC, exactly mirroring
 * the engine's load_palette_byteswapped (1000:063b) + load_palette (1000:08d1).
 *
 * SOURCE LOCATION (RE'd 2026-06-27): load_current_level_data (1000:32b0) sets
 *   cur_level_ptr = DAT_203b_6bd2 + current_level_index * 0x32c;
 * load_palette_byteswapped then reads 16 words from cur_level_ptr[0..0x1f].
 * DAT_203b_6bd2 is the decoded D{n}.DEC buffer past its 2-byte prefix, so the
 * per-level palette is the first 32 bytes of each level's 0x32c block:
 *   cur_level_ptr = g_dec_buf + 2 + level_index*0x32c.
 * Verified offline: g_dec_buf decode of D{1,2,3,9}.DEC at +2 decodes byte-for-byte
 * to local/build/render/bum/world{n}.pal.json (the emulator-captured gameplay
 * palette).  The host renders level-block 0 of D{current_level} (render_level →
 * bg_render_grid(map=g_dec_buf)), so level_index is 0 → cur_level_ptr = g_dec_buf+2.
 *
 * RECONSTRUCTION FIDELITY: the engine's cur_level_ptr is a far ptr into its own
 * DGROUP level archive; the host's equivalent is g_dec_buf (the decoded .DEC).  The
 * 2-byte prefix + 0x32c per-level stride match the engine layout 1:1.  Earlier the
 * host wrongly sourced g_pav_buf+51 (PAV background raster, not a palette → all-black
 * DAC); corrected here.  BUMPY_PLAYABLE only. */
const u8 __far *level_packed_palette(void)
{
    if (g_dec_buf == (u8 __far *)0) {
        return (const u8 __far *)0;
    }
    return (const u8 __far *)(g_dec_buf + 2u);   /* cur_level_ptr, level_index 0 */
}
#endif /* BUMPY_PLAYABLE */

#endif /* } !BUMPY_COPYPROT_HARNESS — end heavy level pipeline */

/* ────────────────────────────────────────────────────────────────────────────
   copy-protection challenge — 1000:4015, gated by #ifdef BUMPY_COPY_PROTECTION.

   RECONSTRUCTION FIDELITY (copy protection): start_level (1000:2d14) opens with a
   copy-protection hook:

       if (1 < current_level && copyprotect_flag == 0) copyprotect_challenge();
       if (copyprotect_flag == -1)                     current_level = 1;

   copyprotect_challenge() (1000:4015) is a sprite-identification quiz: it draws a
   randomly-chosen Bumpy sprite, reads a number the player types, and — in the
   ORIGINAL un-cracked build — compares it against the answer table, setting
   copyprotect_flag = -1 on a MISMATCH (which the second guard above then turns
   into a forced reset to level 1).  The shipped DOS English release is a CRACKED
   build: copyprotect_challenge() sets `copyprotect_flag = 1` (pass) UNCONDITIONALLY
   (1000:412e), before input is even read, then executes a DEAD `expected_answer =
   entered_number` store (1000:4219) in place of the compare — so the challenge is
   defeated and copyprotect_flag is never written -1 anywhere in the binary.

   The body below is the FRESH Ghidra decompile of 1000:4015 ported 1:1 for every
   part present in the cracked binary, PLUS the documented UN-CRACK: the original
   `if (entered_number != answer_tbl[index]) copyprotect_flag = -1;` compare
   recovered from docs/copy-protection.md replaces the crack's unconditional pass +
   dead store (see the in-body RECONSTRUCTION FIDELITY note).  The whole thing is
   gated behind `#ifdef BUMPY_COPY_PROTECTION` (NOT defined in the default build),
   so the default build still compiles OUT the entire hook + body, exactly matching
   the cracked-build runtime (copyprotect_flag stays 0; nothing writes -1).
   ─────────────────────────────────────────────────────────────────────────── */
#ifdef BUMPY_COPY_PROTECTION

/* ── Copy-protection callees + DGROUP globals reached by copyprotect_challenge ──
   These are engine primitives that live in OTHER translation units (input.c,
   anim.c, game_stubs.c, and the resource/render core); none are owned by level.c.
   The whole challenge — and therefore every reference below — is gated behind
   #ifdef BUMPY_COPY_PROTECTION, so the DEFAULT build (macro OFF) never sees them.
   We declare them locally (mirroring the engine prototypes the decomp shows at the
   call sites) so the challenge body compiles standalone; the replay harness
   (tools/copyprot_ctest.c) supplies host definitions, and a fully-linked
   protection-on BUMPY.EXE would resolve them against their owning modules.

   RECONSTRUCTION FIDELITY: fmemcpy is the engine's far block-copy (1000:a9f5).
   The two challenge call sites copy fixed DGROUP tables into SS-local arrays:
     fmemcpy(SS,&sprite_id_tbl, DS,0x11b6) CX=0x20 → 16 WORDS  (sprite frame ids)
     fmemcpy(SS,&answer_tbl,    DS,0x11d6) CX=0x10 → 16 BYTES  (correct answers)
   We model each as one typed copy primitive taking (dst, dgroup_src_off, count). */
extern void copyprot_copy_words(u16 __far *dst, u16 src_off, u16 nwords); /* 1000:a9f5 */
extern void copyprot_copy_bytes(u8  __far *dst, u16 src_off, u16 nbytes); /* 1000:a9f5 */

extern void set_sprite_table_ptr(void);          /* 1cec:2dd2 (thunk 1000:9410)   */
extern void set_active_display_page(void);        /* (thunk 1000:9814)             */
extern void set_resource_table(u16 res, u16 seg); /* 1000:7307                     */
extern void prng_seed_thunk(u16 seed);            /* 1000:93a4 → prng_seed         */
extern int  open_resource(void);                  /* 1000:736f → INT 21h c_open    */
extern void read_chunked(int handle, u16 buf_off, u16 buf_seg,
                         u16 len, u16 arg);        /* 1000:745e                     */
extern void c_close(int handle);                  /* 1000:7319                     */
extern void load_palette_byteswapped(void);       /* 1000:063b                     */
extern void play_iris_wipe_transition(void);      /* 1000:3467                     */
extern void load_palette(u16 src_off, u16 src_seg); /* 1000:08d1                     */
extern void blit_sprite(u16 desc_off, u16 desc_seg); /* 1000:942a                  */
extern void draw_text_at(u16 str_off, u16 str_seg, u16 x, u16 y); /* 1000:07f0     */
extern void draw_number(u16 val_lo, u16 val_hi, u8 width, u16 x, u16 y); /* 1000:0816 */
extern void poll_input(void);                     /* 1000:1dde                     */
extern void run_n_frames(u8 n);                   /* 1000:05e7                     */

/* DGROUP globals the challenge reads/writes (owned by other modules / the engine). */
extern u8  input_state;                  /* DGROUP:0x8244 — latched input bits        */
extern u16 palette_mode;                 /* DGROUP:0x541d — BGI/DAC dispatch mode      */
extern u16 copyprot_seed_src;            /* DGROUP:0x119c — live prng seed (0x5192 RT) */
extern u8 __far  *level_dec_buf_fp;      /* DGROUP:0x6be8 far ptr → decoded res buffer */
extern u8 __far  *p1_sprite;             /* DGROUP:0x8884 far ptr → p1 blit descriptor */
extern u8 __far  *cur_level_ptr;         /* DGROUP:0x6bca far ptr → current-level table */
extern u8 __far  *copyprot_patch_ptr;    /* DGROUP:0x9b96 far ptr → res patch target   */
extern u8 __far  *copyprot_palette_src;  /* codeseg ptr → 16-byte palette patch @0x65a */
extern u16 __far *copyprot_levelptr_src; /* codeseg ptr → 16-word level table   @0x73e */

/* copyprot_engine_rand — 1000:93b1 (the engine rand() thunk).
   NOT the CRT rand: it CALLF's prng_step (1ce5:001f) and returns AL = low byte of
   the new prng_state0.  Modelled identically to player2.c's p2_engine_rand so the
   reject loop's draw is driven by the reconstructed src/prng.c state — deterministic
   once prng_state0/1/2 are seeded to the engine's live entry value (0x5192,0,0). */
static u8 copyprot_engine_rand(void)
{
    prng_step();
    return (u8)prng_state0;
}

/* copyprotect_challenge — port of 1000:4015 (decompiled fresh via Ghidra MCP).

   A sprite-identification quiz: copy the (sprite-id, answer) tables out of DGROUP,
   load resource 0x90 (the level header) into the decode buffer, optionally patch
   it + the current-level table, then run up to 3 rounds.  Each round draws a
   randomly-chosen Bumpy sprite (frame = sprite_id_tbl[index], index ∈ 2..15),
   prompts "Enter the platform number", and runs the +/-/FIRE input dial.  On
   confirm the typed number is compared against the correct answer.

   RECONSTRUCTION FIDELITY — THE UN-CRACK (recovered from docs/copy-protection.md):
   The shipped DOS English release is a CRACKED build.  At 1000:412e it sets
       copyprotect_flag = 1            (pass, UNCONDITIONALLY, BEFORE any input)
   and at 1000:4219 it executes the now-DEAD store
       expected_answer = entered_number          (the original compare, gutted)
   so the typed answer is never checked and copyprotect_flag is never written -1
   anywhere in the binary (Ghidra's own "COPY PROTECTION DEFEATED HERE" comment).
   The ORIGINAL un-cracked logic — `expected_answer = answer_tbl[index]` survives
   intact at 1000:4120, and the documented compare it fed — is restored below:
       if (entered_number != expected_answer) copyprotect_flag = -1;
   This compare is NOT present in the cracked binary; it is reconstructed from the
   answer table + the original control flow documented in docs/copy-protection.md.

   RECONSTRUCTION FIDELITY — round_state flow INFERRED.  The crack collapsed the
   multi-round/pass machinery: it forces `round_state = 0xff` after one round
   (1000:423e) so `2 < round_state` exits immediately after a single dial entry,
   regardless of pass/fail.  The original's per-round retry/advance on the compare
   result is not present in the cracked image; the single-round exit is kept 1:1
   (it is what the binary does) and the un-cracked pass/fail is expressed purely
   through copyprotect_flag, which start_level (1000:2d14) turns into the level-1
   reset.  Where the crack removed the original loop control we do not invent it.
*/
void copyprotect_challenge(void)
{
    u16 sprite_id_tbl[16];      /* SS [BP-0x26] — sprite frame ids   (DS:0x11b6) */
    u8  answer_tbl[16];         /* SS [BP-0x36] — correct answers     (DS:0x11d6) */
    u8  round_state;            /* SS [BP-0x05]                                    */
    u8  expected_answer;        /* SS [BP-0x04] — answer_tbl[index]               */
    char entry_done;            /* SS [BP-0x03]                                    */
    u8  entered_number;         /* SS [BP-0x02] — the typed platform number       */
    u8  copy_idx;               /* SS [BP-0x01] — patch-loop counter              */
    u16 index;                  /* SI           — random sprite index, 2..15       */
    int handle;
    u8 __far *p1d;              /* p1_sprite blit descriptor                       */

    round_state = 0;

    /* Copy the two fixed tables out of DGROUP into SS-local arrays. */
    copyprot_copy_words(sprite_id_tbl, 0x11b6u, 16u);  /* 16 sprite frame ids */
    copyprot_copy_bytes(answer_tbl,    0x11d6u, 16u);  /* 16 correct answers  */

    /* Setup: sprite table + display page, then point the resource table at the
       challenge's level header (res 0x90 in DGROUP segment 0x203b). */
    set_sprite_table_ptr();
    set_active_display_page();
    set_resource_table(0x90u, 0x203bu);

    /* Seed the prng from the (LIVE) DGROUP seed source, then load resource 0x90
       (99 bytes) into the decode buffer.  RECONSTRUCTION FIDELITY: the static
       image value at 0x119c is 0x1e61, but at runtime after boot it is mutated to
       0x5192 — the seed is taken LIVE.  See docs/copy-protection.md + the replay
       harness, which seeds the captured live value to reproduce index = 12. */
    prng_seed_thunk(copyprot_seed_src);
    handle = open_resource();
    read_chunked(handle, FP_OFF(level_dec_buf_fp), FP_SEG(level_dec_buf_fp),
                 99u, 0u);
    c_close(handle);

    /* palette_mode==1: patch 16 bytes of the decoded header from the code-segment
       table @0x65a (the copy target tracked via copyprot_patch_ptr @0x9b96). */
    if (palette_mode == 1u) {
        copyprot_patch_ptr = level_dec_buf_fp;
        for (copy_idx = 0u; copy_idx < 0x10u; copy_idx = copy_idx + 1u) {
            copyprot_patch_ptr[(u16)copy_idx + 0x23u] =
                copyprot_palette_src[copy_idx];
        }
    }

    /* Copy 16 words of the level-pointer table from the code-segment src @0x73e
       into the current-level table, then load the (byte-swapped) palette. */
    for (copy_idx = 0u; copy_idx < 0x10u; copy_idx = copy_idx + 1u) {
        ((u16 __far *)cur_level_ptr)[copy_idx] = copyprot_levelptr_src[copy_idx];
    }
    load_palette_byteswapped();

    round_state = 0;
    do {
        if (2u < round_state) {
            set_active_display_page();
            set_sprite_table_ptr();
            return;
        }

        /* Random sprite pick: reject 0/1, accept 2..15. */
        do {
            index = (u16)copyprot_engine_rand() & 0xfu;
        } while (index < 2u);

        expected_answer = answer_tbl[index];   /* 1000:4120 — original answer slot */
        entry_done = '\0';
        entered_number = 0u;

        /* ── THE UN-CRACK ──────────────────────────────────────────────────────
           CRACKED binary here: `copyprotect_flag = 1;` (1000:412e) — defeated,
           set BEFORE input.  Restored below: nothing is decided up-front; the
           verdict comes from the answer compare AFTER the dial entry. */

        /* Draw the chosen platform sprite. */
        play_iris_wipe_transition();
        load_palette();
        p1d = p1_sprite;
        *(u16 __far *)(p1d + 4) = sprite_id_tbl[index];  /* frame */
        *(u16 __far *)(p1d + 0) = 0x90u;                 /* x = 144 */
        *(u16 __far *)(p1d + 2) = 100u;                  /* y = 100 */
        blit_sprite(FP_OFF(p1_sprite), FP_SEG(p1_sprite));

        /* "Enter the platform number" prompt + the current entry. */
        draw_text_at(0x1331u, 0x203bu, 0x54u, 0x87u);
        draw_number((u16)entered_number, 0u, 2u, 0x98u, 0x96u);

        /* ── Input dial state machine ──────────────────────────────────────────
           FIRE (0x10) confirms; MINUS (0x04) decrements if > 0; PLUS (0x08)
           increments only while entered_number < 0x63 (CMP 0x63 / JNC @1000:41d3),
           so the value saturates at 0x63.  Each iteration redraws the number and
           advances 4 frames (4× poll sampling per action). */
        while (entry_done == '\0') {
            input_state = 0u;
            poll_input();
            if ((input_state & 0x10u) == 0u) {
                if (((input_state & 4u) == 0u) || (entered_number == 0u)) {
                    if (((input_state & 8u) == 0u) || (0x62u < entered_number)) {
                        goto LAB_redraw;
                    }
                    entered_number = entered_number + 1u;     /* PLUS */
                    draw_text_at(0x1350u, 0x203bu, 0x98u, 0x96u);
                }
                else {
                    entered_number = entered_number - 1u;     /* MINUS */
                    draw_text_at(0x134bu, 0x203bu, 0x98u, 0x96u);
                }
            }
            else {
                entry_done = '\x01';                          /* FIRE: confirm */
            }
LAB_redraw:
            draw_number((u16)entered_number, 0u, 2u, 0x98u, 0x96u);
            run_n_frames(4u);
        }

        /* ── THE UN-CRACK (continued) ──────────────────────────────────────────
           CRACKED binary here: `expected_answer = entered_number;` (1000:4219) —
           a DEAD store that overwrites the loaded answer with the typed value so
           no comparison happens.  Restored to the documented ORIGINAL: compare the
           typed number against the correct answer and FAIL the protection on a
           mismatch (start_level then forces the player back to level 1). */
        if (entered_number != expected_answer) {
            copyprotect_flag = (u8)-1;   /* 0xff — protection FAILED */
        }

        round_state = 0xffu;             /* 1000:423e — single-round exit (crack)  */
    } while (1);
}
#endif /* BUMPY_COPY_PROTECTION */

#ifndef BUMPY_COPYPROT_HARNESS   /* { start_level + render — excluded from the host harness */

/* ────────────────────────────────────────────────────────────────────────────
   start_level — port of 1000:2d14.

   Key structure from decomp lines 2162-2214:
     1. copy-protection guard (dormant at level 1)
     2. set move-descriptor / anim-coord ptrs  (STUBBED)
     3. set p1 start position
     4. clear move-descriptor list sentinel  (STUBBED)
     5. set_resource_table / patch digits    (STUBBED — we compute filenames)
     6. load D{n}.PAV → vec_decode
     7. load D{n}.DEC → vec_decode
     8. load D{n}.BUM → vec_decode
     9. [ADDED] render the loaded level
   ─────────────────────────────────────────────────────────────────────────── */
void start_level(u8 world, u8 level)
{
    char fname[16];

    /* Suppress unused-parameter warning for world (not used in original
       parameterless function; kept here for clarity in the reconstructed API). */
    (void)world;

    current_level = level;

    /* ── 0. Allocate large buffers ─────────────────────────────────────────
       ADDED: allocate planes, bank, dg, and level data buffers at runtime
       to keep EXE size within 640 KB DOS memory limit.
       RECONSTRUCTION FIDELITY: original used fixed far-segment addresses. */
    if (level_alloc_buffers() != 0) {
        return;   /* out of memory; cannot continue */
    }

    /* ── 1. Copy-protection guard (1000:2d14, decomp lines 2176-2179) ─────
       Original:
         if ((1 < current_level) && (copyprotect_flag == 0)) copyprotect_challenge();
         if (copyprotect_flag == -1)                         current_level = 1;
       The WHOLE hook is gated by #ifdef BUMPY_COPY_PROTECTION (NOT defined by
       default).  With it OFF the hook compiles OUT — level-advance to levels 2+
       flows with no challenge, matching the cracked-build runtime (copyprotect_flag
       stays 0; nothing ever writes -1, so the reset can never fire).  See the
       copyprotect_challenge() note above for the reconstructed (un-cracked) body. */
#ifdef BUMPY_COPY_PROTECTION
    if ((1u < (u16)current_level) && (copyprotect_flag == 0u)) {
        copyprotect_challenge();
    }
    if (copyprotect_flag == (u8)0xffu) {  /* == -1 as byte */
        current_level = 1u;
    }
#endif /* BUMPY_COPY_PROTECTION */

    /* ── 2. Move-descriptor / anim-coord table ptrs (STUBBED) ─────────────
       Original: read ptrs from code-seg table at 0x10c8..0x10cb / 0x10ec..0x10ef.
       Not needed for the initial render; deferred (RECONSTRUCTION FIDELITY #4/#5). */

    /* ── 3. Player start position ─────────────────────────────────────────
       Original decomp lines 2187-2192. */
    current_entity_index = 1u;
    p1_start_y = 0x1fu;
    p1_start_x = 0x1fu;
    if ((current_level == 2u) || (current_level == 5u)) {
        p1_start_x = 0x6fu;
    }

    /* ── 4. Clear move-descriptor list (STUBBED) ──────────────────────────
       Original: walks sentinel-terminated list at move_descriptor_table,
       clears entries until -1.  Not needed for render; deferred. */

    /* ── 5. set_resource_table + digit-patch (STUBBED) ────────────────────
       Original calls set_resource_table(0x90, 0x203b) and patches DAT bytes.
       We bypass and build filenames directly (RECONSTRUCTION FIDELITY #3). */

    /* ── 6. Load D{n}.PAV ─────────────────────────────────────────────────
       Original: open_resource(0,4) + read_chunked + c_close + vec_decode.
       Here: level_decode_file (dosio stream + op12_vec_run on g_op12_arena),
       then copy arena → g_pav_buf. */
    level_make_filename(fname, current_level, "PAV");
    if (level_decode_file(fname) > 0u) {
        level_copy_arena(g_pav_buf, LEVEL_PAV_BUF_SIZE);
    }

    /* ── 7. Load D{n}.DEC ────────────────────────────────────────────────── */
    level_make_filename(fname, current_level, "DEC");
    if (level_decode_file(fname) > 0u) {
        level_copy_arena(g_dec_buf, LEVEL_DEC_BUF_SIZE);
    }

    /* ── 8. Load D{n}.BUM ────────────────────────────────────────────────── */
    level_make_filename(fname, current_level, "BUM");
    if (level_decode_file(fname) > 0u) {
        level_copy_arena(g_bum_buf, LEVEL_BUM_BUF_SIZE);
    }

    /* ── 9. Load + transform sprite bank (BUMSPJEU.BIN) ─────────────────
       ADDED: not in original start_level (bank is loaded once at session init
       by the engine's resource manager).  We load it here for Task 4 because
       entity_draw_* needs it and session init is still stubbed.
       sprite_bank_load_transform converts the on-disk big-endian table to
       the in-memory LE form the blitter uses (EGA/VGA palette_mode=2).
       RECONSTRUCTION FIDELITY DEVIATION: in the original the bank is loaded
       during init_game_session_state, not start_level.  Deferred to the
       appropriate init function once resource management is reconstructed. */
    if (level_load_bank("BUMSPJEU.BIN") > 0UL) {
        sprite_bank_load_transform((u8 __far *)g_bank_buf, 2u);
    }

    /* ── 10. Populate DGROUP shadow + render ─────────────────────────────
       ADDED: render pipeline not in original start_level; added here to
       exercise the validated bg_render_grid + entity_draw_* path. */
    level_populate_dg();
    render_level();
}

#endif /* } !BUMPY_COPYPROT_HARNESS — end start_level + render */

/* ══ PHASE 9, TASK 3 — LEVEL-COMPLETE PREDICATE ════════════════════════════════
 *
 * all_entries_flag_set (1000:3e8a) — the do/while level-round exit predicate
 * game_loop (game.c) polls: returns 1 iff EVERY entry in the move-descriptor table
 * has its [0] flag byte set (i.e. all required entries cleared), else 0 (stay in
 * the round).  Walks the table at the far ptr move_descriptor_table (DGROUP 0x8246)
 * from record index 1, in 9-byte strides, ANDing each record's [0] byte, until the
 * first record whose [0] byte == 0xff (the sentinel terminator).
 *
 * Ported 1:1 from the disasm 1000:3e8a..3ed3 (IMUL DX=9 stride; LES BX,[0x8246];
 * AND [BP-1],AL; CMP ES:[BX],0xff loop guard).  Index 0 is skipped (loop starts at
 * 1); the accumulator seeds to 1 so an empty table (record 1 is the sentinel)
 * returns 1.
 *
 * Lives in level.c because move_descriptor_table is level state (the per-level
 * entry list the engine loads at start_level).  The table is RUNTIME-POPULATED
 * engine level-data (start_level §4 "clear move-descriptor list" is still stubbed —
 * see the start_level body); move_descriptor_table is the far ptr the engine points
 * at it.  Previously a game_stubs.c no-op returning 0 (round never completes); now
 * the real predicate.
 */
u8 __far *move_descriptor_table;   /* DGROUP 0x8246/0x8248 — per-level entry list far ptr */

u8 all_entries_flag_set(void)
{
    u8 record_idx;
    u8 all_set;

    record_idx = 1;
    all_set = 1;
    while (move_descriptor_table[(u16)record_idx * 9] != 0xff) {
        all_set = (u8)(all_set & move_descriptor_table[(u16)record_idx * 9]);
        record_idx = (u8)(record_idx + 1);
    }
    return all_set;
}

#ifdef BUMPY_PLAYABLE
/* level_get_entity_dg — expose the entity-DG shadow to host_view.c.
 * init_sprite_structs (1000:33c5) in the engine sets p1_sprite / p2_sprite to
 * the literal DGROUP far ptrs (&sprite_obj_203b_792e / &sprite_obj_203b_795a).
 * In the host model the "DGROUP" is the g_entity_dg shadow buffer; init_sprite_structs
 * must point p1_sprite and p2_sprite into that shadow so that draw_p1/p2_sprite's
 * writes to obj+0/2/4 land in the same bytes that hr_blit_obj (host_render.c) reads
 * from hr_dg[0x792e] / hr_dg[0x795a].  Exposed ONLY under BUMPY_PLAYABLE. */
u8 __far *level_get_entity_dg(void)
{
    return g_entity_dg;
}

/* ── load_current_level_data (1000:32b0) — per-round level (re)load ──────────────
 * Engine logic (relocated from src/host/host_boot.c — not host platform glue).
 *
 * Engine body: copies the current level's 0x96-byte header from the in-memory level
 * archive (cur_level_ptr) into the tilemap buffer.  HOST STAND-IN: the playable
 * build never populates that in-memory archive (start_level decodes from files
 * directly), so we reload by calling start_level(current_level, current_level),
 * which re-runs the full INT 21h file-load pipeline for the current level.
 *
 * DEVIATION (DONE_WITH_CONCERNS): start_level does MORE than the engine's 32b0 (it
 * also reloads the sprite bank + re-renders the level to VGA), and game_loop already
 * calls start_level at the top of each round, so it runs twice per round.  Harmless
 * (start_level is idempotent — same static buffers, no recursion) but wasteful.
 * TODO (future faithful pass): implement the real 1000:32b0 lightweight 0x96-byte
 * header copy and drop this start_level call.  See docs/reconstruction-fidelity.md. */
void load_current_level_data(void)
{
    start_level(current_level, current_level);
}
#endif /* BUMPY_PLAYABLE */
