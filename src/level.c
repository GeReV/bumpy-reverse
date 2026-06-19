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

/* ── extern: op12 arena (declared in bvec_buf2.c) ─────────────────────────────
   Shared 0x8000-byte decode arena.  We extern it (do NOT redefine); op12.obj
   and bvec_buf2.obj are already in the BUMPY link set. */
extern u8 __far g_op12_arena[];

/* ── DGROUP globals (engine originals; defined here) ─────────────────────── */
u8 current_level        = 1u;
u8 copyprotect_flag     = 0u;
u8 p1_start_x           = 0x1fu;
u8 p1_start_y           = 0x1fu;
u8 current_entity_index = 1u;

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
    if (g_planes == (u8 __huge *)0) {
        g_planes = (u8 __huge *)halloc(0x40000UL, 1);
        if (g_planes == (u8 __huge *)0) { return -1; }
    }

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

    /* Write palette from PAV header (offset 51 = 48 bytes of DAC values)
       and blit planes to VGA hardware.
       RECONSTRUCTION FIDELITY: the engine loads the palette via
       load_palette_byteswapped() at level init.  We use video_set_palette6
       with the palette embedded in the PAV decoded header at offset 51. */
    video_set_palette6((const u8 *)(g_pav_buf + 51u));
    video_blit_planar(g_page0);
}

/* ────────────────────────────────────────────────────────────────────────────
   copyprotect_challenge_stub — dormant copy-protection stub.
   ─────────────────────────────────────────────────────────────────────────── */
void copyprotect_challenge_stub(void)
{
    /* STUB: the original (1000:4015) displays a sprite-identification quiz.
       Only called when current_level > 1 AND copyprotect_flag == 0.
       For level-1 boot this path is never reached.  Left unimplemented;
       the cracked build had this logic bypassed anyway.
       RECONSTRUCTION FIDELITY: the hook STRUCTURE is carried faithfully
       (the guarded call in start_level) but the challenge body is DORMANT. */
}

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

    /* ── 1. Copy-protection guard (1000:2d14, decomp line 2177) ───────────
       In the original: "if ((1 < current_level) && (copyprotect_flag == '\0'))
         copyprotect_challenge();"
       DORMANT at level 1: the condition (1 < 1) is FALSE, so it never fires.
       copyprotect_challenge() is fully stubbed. */
    if ((1u < (u16)current_level) && (copyprotect_flag == 0u)) {
        copyprotect_challenge_stub();
    }
    if (copyprotect_flag == (u8)0xffu) {  /* == -1 as byte */
        current_level = 1u;
    }

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
