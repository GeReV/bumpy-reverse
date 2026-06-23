/* tools/fb_compare.c — the Plan-A Task-11 frame-compare comparator.
 *
 * Reads two raw VGA-framebuffer dump files produced by the BUMPYCAP FB capture
 * (tools/dosbox/patches/03-framebuffer-capture.patch): a flat stream of
 *   N frames x 4 planes x 0x1F40 bytes,  plane order 0,1,2,3
 * (0x1F40 = 8000 = 320*200/8, the visible area of VGA mode 0x0D).  File A is the
 * REFERENCE (the real original BUMPY.EXE), file B is the candidate (the playable
 * BUMPYP.EXE).  Both are captured under the same in-level arm + per-tick trigger
 * so frame k of each is the k-th per-tick "just-presented" displayed page.
 *
 * It compares the two streams plane-for-plane and reports the FIRST mismatch as
 * (frame, plane, byte-offset) with the two differing bytes and the decoded
 * screen pixel coordinates — this is the gate's diagnostic value: it isolates a
 * host-glue divergence (present/flip/binding) since the compose + per-tick state
 * are independently gated (Task 2 byte-exact compose; the int8 gate for state).
 *
 * Phase tolerance: page-flip phase between the two builds can differ, so frame k
 * of one may equal frame k+/-PHASE of the other.  With --phase P, the comparator
 * first finds the single best whole-frame alignment shift s in [-P, +P] (the s
 * that maximises identical frames), then requires EXACT plane-for-plane equality
 * for every overlapping frame at that shift.  Default P=0 (strict, no shift).
 * The chosen shift is reported.  This is a documented, bounded relaxation — NOT a
 * per-byte tolerance: at the chosen shift the compare is byte-exact.
 *
 * Exit 0 iff every overlapping frame matches plane-for-plane at the best shift;
 * non-zero on any residual mismatch (printing the first one) or on bad input.
 *
 * Build: cc -O2 -Wall -o fb_compare tools/fb_compare.c
 * Usage: fb_compare <ref.bin> <cand.bin> [--phase P] [--min-frames M]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PLANE_BYTES 0x1F40u            /* 8000 = 320*200/8 (visible area)      */
#define FRAME_BYTES (4u * PLANE_BYTES) /* one captured frame: 4 planes         */
#define SCR_W       320
#define ROW_BYTES   40                 /* 320/8 bytes per scanline per plane   */

static uint8_t *slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "fb_compare: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); fprintf(stderr, "fb_compare: OOM\n"); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(buf); fprintf(stderr, "fb_compare: short read %s\n", path); return NULL;
    }
    fclose(f);
    *len_out = (size_t)n;
    return buf;
}

/* Count whole frames that are byte-identical when stream B is shifted by `shift`
 * frames relative to A (cand frame j compared against ref frame j+shift). */
static unsigned count_matches(const uint8_t *a, unsigned na,
                              const uint8_t *b, unsigned nb, int shift, unsigned *overlap_out) {
    unsigned matches = 0, overlap = 0;
    for (unsigned j = 0; j < nb; j++) {
        long ai = (long)j + shift;
        if (ai < 0 || ai >= (long)na) continue;
        overlap++;
        if (memcmp(a + (size_t)ai * FRAME_BYTES, b + (size_t)j * FRAME_BYTES, FRAME_BYTES) == 0)
            matches++;
    }
    *overlap_out = overlap;
    return matches;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <ref.bin> <cand.bin> [--phase P] [--min-frames M]\n", argv[0]);
        return 2;
    }
    int phase = 0;
    unsigned min_frames = 1;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--phase") == 0 && i + 1 < argc) phase = atoi(argv[++i]);
        else if (strcmp(argv[i], "--min-frames") == 0 && i + 1 < argc) min_frames = (unsigned)strtoul(argv[++i], NULL, 0);
    }

    size_t la = 0, lb = 0;
    uint8_t *A = slurp(argv[1], &la);
    uint8_t *B = slurp(argv[2], &lb);
    if (!A || !B) return 2;

    if (la % FRAME_BYTES != 0 || lb % FRAME_BYTES != 0) {
        fprintf(stderr, "fb_compare: file size not a whole number of frames (A=%zu B=%zu, frame=%u)\n",
                la, lb, FRAME_BYTES);
        return 2;
    }
    unsigned na = (unsigned)(la / FRAME_BYTES);
    unsigned nb = (unsigned)(lb / FRAME_BYTES);
    printf("fb_compare: ref=%s (%u frames)  cand=%s (%u frames)  phase=+/-%d\n",
           argv[1], na, argv[2], nb, phase);
    if (na == 0 || nb == 0) { fprintf(stderr, "fb_compare: empty capture\n"); return 2; }

    /* find best whole-frame alignment shift in [-phase, +phase] */
    int best_shift = 0;
    unsigned best_matches = 0, best_overlap = 0;
    for (int s = -phase; s <= phase; s++) {
        unsigned ov = 0, m = count_matches(A, na, B, nb, s, &ov);
        if (m > best_matches || (m == best_matches && abs(s) < abs(best_shift))) {
            best_matches = m; best_shift = s; best_overlap = ov;
        }
    }
    printf("fb_compare: best alignment shift = %d  (matched %u / %u overlapping frames)\n",
           best_shift, best_matches, best_overlap);

    /* require EXACT plane-for-plane equality for every overlapping frame at the
     * chosen shift; report the first mismatch (frame, plane, offset). */
    for (unsigned j = 0; j < nb; j++) {
        long ai = (long)j + best_shift;
        if (ai < 0 || ai >= (long)na) continue;
        const uint8_t *fa = A + (size_t)ai * FRAME_BYTES;
        const uint8_t *fb = B + (size_t)j * FRAME_BYTES;
        for (unsigned plane = 0; plane < 4u; plane++) {
            const uint8_t *pa = fa + plane * PLANE_BYTES;
            const uint8_t *pb = fb + plane * PLANE_BYTES;
            for (unsigned off = 0; off < PLANE_BYTES; off++) {
                if (pa[off] != pb[off]) {
                    unsigned row = off / ROW_BYTES;
                    unsigned col = (off % ROW_BYTES) * 8;  /* leftmost pixel of byte */
                    printf("fb_compare: MISMATCH at cand-frame=%u (ref-frame=%ld) "
                           "plane=%u offset=0x%04x  ref=0x%02x cand=0x%02x  "
                           "(screen y=%u x=%u..%u)\n",
                           j, ai, plane, off, pa[off], pb[off], row, col, col + 7);
                    free(A); free(B);
                    return 1;
                }
            }
        }
    }

    unsigned compared = best_overlap;
    if (compared < min_frames) {
        fprintf(stderr, "fb_compare: only %u overlapping frames compared (< required %u)\n",
                compared, min_frames);
        free(A); free(B);
        return 1;
    }
    printf("fb_compare: MATCH — %u overlapping frames identical plane-for-plane "
           "(shift=%d)\n", compared, best_shift);
    free(A); free(B);
    return 0;
}
