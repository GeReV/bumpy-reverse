/*
 * vec.c — VEC opcode interpreter: op4 path, op0 raw path, plane-major planar output.
 *
 * Faithfully reconstructed from the engine's real vec_run (1c28:0000).
 *
 * ── Jump-table resolution (DGROUP 0x4e37, index = opcode-1) ──────────────
 *   Extracted from DGROUP:0x4e37 in the unpacked BUMPY.EXE image and
 *   confirmed against the runtime op4_seed_mem.bin full-memory snapshot:
 *
 *     op  0     : raw / uncompressed (SCORE.VEC — file IS the decoded buffer)
 *     op  1..3  : 1c28:0193  (single RET — no-op; not used by TITRE.VEC)
 *     op  4     : 1c28:0194  (RLE decompressor + in-place decode loop)
 *     op  5..11 : 1c28:0193  (no-op)
 *     op 12     : 1c28:04b0  (masked-blit compositor — deferred to Plan 4)
 *     op 13..16 : 1c28:0193  (no-op)
 *
 * ── op4 handler summary (1c28:0194) ──────────────────────────────────────
 *   The handler reads the first byte of the inline payload as the escape byte,
 *   then walks the compressed stream, writing decoded bytes forward into the
 *   decode buffer (the engine uses a far-call to vec_xform for the in-place
 *   sliding-window addressing, but the byte ordering is strictly sequential and
 *   byte-exact to a simple forward pass).  Algorithm:
 *
 *     escape = stream[0];
 *     for each byte b from stream[1..]:
 *       if b != escape  → emit b
 *       else:
 *         x = next byte;
 *         if x == escape → emit escape
 *         else:
 *           count = next byte; if count == 0 count = 256;
 *           emit x repeated count times
 *
 *   This is byte-identical to the oracle (tools/extract/vec_render.py:rle_decode)
 *   and the existing vec_rle_decode implementation below.  Equivalence confirmed:
 *   both produce the same 32099-byte decoded buffer for TITRE.VEC.
 *
 * ── op0 raw path (SCORE.VEC) ─────────────────────────────────────────────
 *   SCORE.VEC has all-zero record header (w0=w1=w2=w3=w4=w5=0, opcode=0).
 *   The file is the decoded buffer directly — no RLE, no record indirection:
 *   [0..51) meta, [51..99) 48-byte palette, [99..32099) four planes.
 *   Spike verification: de-planing SCORE.VEC[99:] with embedded palette ==
 *   results/images/score.png (0/64000 pixel diff). CONFIRMED.
 *
 * ── vec_run structure ─────────────────────────────────────────────────────
 *   vec_run reads records in a loop via vec_read_record(); each 12-byte record
 *   has six big-endian words [w0..w5], where w4 (low 15 bits) is the opcode
 *   and w5 is the XOR checksum.  Termination when vec_read_record sets CF=1
 *   (w0 > 0x0f, bad high bits in w4, or checksum mismatch).  For TITRE.VEC
 *   there is exactly one record (op4) carrying the entire compressed payload.
 *
 * ── Record loop (this implementation) ────────────────────────────────────
 *   We decode only the leading op4 or op0 record, which carries the full image
 *   for all currently targeted screens.  Trailing records (if any) that cannot
 *   be handled produce a clean early return.  This is sufficient for TITRE
 *   (op4) and SCORE (op0).  Screens whose op4 payload decompresses into an
 *   inner op12 record stream (DESSFIN, MASKBUMP, BUMPRESE) are not yet
 *   supported — op12 is deferred to Plan 4.
 *
 * ── Output buffer layout ─────────────────────────────────────────────────
 *   The decoded buffer is 0x7d63 bytes = 99-byte header + 32000-byte planar.
 *   Plane p starts at decoded[99 + p*8000], 40 bytes/row, 200 rows.
 *   We copy each plane's 8000 bytes to planar[p*8000] for video_blit_planar().
 */

#include "vec.h"
#include "op12.h"
#include <string.h>

/* op12 decode arena (declared in bvec_buf2.c) — holds the inner record stream
   and where op12_vec_run builds the decoded image for op12 / inner-op4 screens. */
extern u8 __far g_op12_arena[];

/* ── Internal decode scratch buffer ──────────────────────────────────────── */
/* The RLE decoder needs a VEC_DECODE_MAX work buffer.  It is declared as the
   static array vec_decode_scratch[] below; no external symbol is required. */

/* ── Big-endian word read ─────────────────────────────────────────────────── */

static u16 be16(const u8 *b, u16 o)
{
    return (u16)(((u16)b[o] << 8) | (u16)b[o + 1]);
}

/* ── op4 RLE decode (1c28:0194, byte-identical to oracle rle_decode) ───────
   Decodes from data[start] into out[], producing up to `limit` bytes.
   `n` = len(data).  Returns number of bytes written. */
static u16 op4_rle_decode(const u8 *data, u16 n, u16 start, u16 limit, u8 *out)
{
    u8  escape;
    u8  b;
    u8  x;
    u16 count;
    u16 k;
    u16 i;
    u16 outlen;

    escape = data[start];
    i      = (u16)(start + 1);
    outlen = 0;

    while (outlen < limit && i < n) {
        b = data[i]; i++;
        if (b != escape) {
            out[outlen++] = b;
            continue;
        }
        if (i >= n) { break; }
        x = data[i]; i++;
        if (x == escape) {
            out[outlen++] = escape;
            continue;
        }
        if (i >= n) { break; }
        count = data[i]; i++;
        if (count == 0) { count = 256; }
        for (k = 0; k < count && outlen < limit; k++) {
            out[outlen++] = x;
        }
    }
    return outlen;
}

/* ── Planar extraction: decoded buffer → plane-major planar ─────────────────
   The decoded buffer is laid out as:
       [51 bytes metadata] [48 bytes palette] [32000 bytes: 4 planes x 8000 bytes]
   Plane p in the decoded buffer starts at decoded[VEC_HDR_BYTES + p*VEC_PLANE_BYTES].
   We memcpy each plane into planar[p*VEC_PLANE_BYTES] for video_blit_planar(). */
static void copy_planes(const u8 *decoded, u8 *planar)
{
    u16 p;

    for (p = 0; p < 4u; p++) {
        memcpy(planar + (u16)(p * VEC_PLANE_BYTES),
               decoded + VEC_HDR_BYTES + (u16)(p * VEC_PLANE_BYTES),
               VEC_PLANE_BYTES);
    }
}

/* ── vec_decode_planar ───────────────────────────────────────────────────────
   Structured after vec_run's opcode loop.  For a full-screen op4-only file
   (TITRE.VEC etc.) there is one record: the RLE-compressed planar image.
   We read the record header, dispatch to op4 (= RLE decode into a local
   decode scratch), then copy the four planes into the caller's buffer. */

/* Static decode scratch: VEC_DECODE_MAX bytes.  Kept in vec.c to avoid
   adding another large buffer object in bvec.  For the 16-bit target this is
   ~32 KB; in the large memory model it sits in its own far data segment. */
static u8 __far vec_decode_scratch[VEC_DECODE_MAX];

u16 vec_decode_planar(const u8 *data, u16 n, u8 *planar, u8 *pal_out)
{
    u16 w0;
    u16 w1_decoded_size;
    u16 w4_opcode_raw;
    u16 opcode;
    u16 checksum_expected;
    u16 checksum_actual;
    u16 payload_start;
    u16 decoded;

    /* ── Record 0 header: 6 big-endian words (12 bytes) ─────────────────── */
    if (n < 12u) { return 0xffffu; }

    w0              = be16(data, 0);
    w1_decoded_size = be16(data, 2);
    w4_opcode_raw   = be16(data, 8);
    opcode          = (u16)(w4_opcode_raw & 0x7fffu);

    /* vec_read_record termination: w0 > 0x0f → end of stream.
       For op4-only full-screen files, w0 == 0. */
    if (w0 > 0x000fu) { return 0xffffu; }

    /* Checksum: w5 = w0 ^ w1 ^ w2 ^ w3 ^ w4. */
    checksum_expected = be16(data, 10);
    checksum_actual   = (u16)(be16(data, 0) ^ be16(data, 2) ^
                              be16(data, 4) ^ be16(data, 6) ^
                              w4_opcode_raw);
    if (checksum_actual != checksum_expected) { return 0xffffu; }

    /* ── Opcode dispatch ─────────────────────────────────────────────────── */

    /* op0: raw / uncompressed (SCORE.VEC).
       The file itself IS the decoded buffer: [0..51) meta, [51..99) palette,
       [99..32099) four planes.  No RLE, no record-stream indirection.
       w0=0,w1=0 → checksum 0^0^0^0^0=0 passes above; opcode=0 detected here
       BEFORE the op4 size checks so the zero w1_decoded_size does not reject it.
       VERIFIED: de-planing SCORE.VEC[99:] with the embedded palette ==
       results/images/score.png (0/64000 pixel diff). */
    if (opcode == 0u) {
        if (n < (u16)(VEC_HDR_BYTES + VEC_PLANAR)) {
            return 0xffffu;
        }
        if (pal_out != (u8 *)0) {
            memcpy(pal_out, data + VEC_PAL_OFF, VEC_PAL_BYTES);
        }
        copy_planes(data, planar);
        return 0u;
    }

    if (opcode == 4u && w1_decoded_size == VEC_DECODE_MAX) {
        /* op4 full-screen direct-planar path (TITRE.VEC).
           Inline payload starts at byte 12 of the record; the first byte of
           the payload is the RLE escape byte, payload data follows from byte 13.
           w1_decoded_size == 0x7d63 means the op4 record decodes straight to a
           full 99+32000 planar image — no inner record stream.  This is the
           verified TITRE path and is kept unchanged. */
        if (w1_decoded_size < VEC_HDR_BYTES || w1_decoded_size > VEC_DECODE_MAX) {
            return 0xffffu;
        }
        payload_start = 12u;  /* escape byte at data[12], rle stream at data[13] */
        decoded = op4_rle_decode(data, n, payload_start, w1_decoded_size,
                                 vec_decode_scratch);
        if (decoded < VEC_HDR_BYTES) { return 0xffffu; }

        /* Extract optional palette before copying planes. */
        if (pal_out != (u8 *)0) {
            memcpy(pal_out, vec_decode_scratch + VEC_PAL_OFF, VEC_PAL_BYTES);
        }

        if (decoded < (u16)(VEC_HDR_BYTES + VEC_PLANAR)) { return 0xffffu; }

        /* Copy the four planes into the caller's plane-major planar buffer. */
        copy_planes(vec_decode_scratch, planar);
        return 0u;
    }

    if (opcode == 4u || opcode == 12u) {
        /* Inner-record-stream path (MASKBUMP / DESSFIN = op4 with a small
           decoded size; BUMPRESE = op12 at record 0).  Faithful to op12_port:
           the raw .VEC bytes are the inner stream; vec_run dispatches op4
           (outer RLE) and op12 (masked-blit) records over the arena until the
           stream terminates, leaving the final 99+32000 decoded image there.

           We load the raw file into g_op12_arena and run op12_vec_run with
           declared_len = VEC_DECODE_MAX (op12_port DECLARED_LEN) and
           payload_len = n (op12_port's vsav seed = file size). */
        u32 i32;

        if (n > OP12_ARENA_SIZE) { return 0xffffu; }
        for (i32 = 0; i32 < (u32)n; i32++) {
            g_op12_arena[(u16)i32] = data[(u16)i32];
        }
        op12_vec_run(g_op12_arena, VEC_DECODE_MAX, (u16)n);

        if (pal_out != (u8 *)0) {
            memcpy(pal_out, g_op12_arena + VEC_PAL_OFF, VEC_PAL_BYTES);
        }
        copy_planes(g_op12_arena, planar);
        return 0u;
    }

    /* All other opcodes (op1-3, 5-11, 13+) are no-ops in the real engine
       (jump table entry = 1c28:0193 = single RET).  For TITRE.VEC these never
       appear, but return success so a pure no-op stream does not fail. */
    return 0u;
}
