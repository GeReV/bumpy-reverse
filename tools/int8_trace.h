/* int8_trace.h — canonical binary layout for the int8-synced end-to-end trace.
 * Shared by tools/int8_ctest.c (replay) and tools/int8_oracle_check.py logic.
 * The DOSBox capture patch (tools/dosbox/patches/02-int8-snap-capture.patch)
 * MIRRORS this layout (it cannot include this header); keep them in lockstep and
 * bump INT8_VERSION on ANY layout change so stale traces hard-fail at load.
 *
 * All little-endian (x86 capture host == x86 replay host). See
 * docs/superpowers/specs/2026-06-23-int8-snap-capture-design.md. */
#ifndef INT8_TRACE_H
#define INT8_TRACE_H
#include <stdint.h>
#include <stddef.h>

#define INT8_MAGIC   "BINT"
#define INT8_VERSION 1
#define INT8_TILEMAP_SIZE 0x300   /* matches p1_spine_ctest.c TILEMAP_SIZE */

#pragma pack(push, 1)

struct int8_header {
    char     magic[4];      /* "BINT" */
    uint16_t version;       /* == INT8_VERSION */
    uint16_t dgroup_seg;    /* DGROUP calibration seg used at capture (0x185f) */
    uint16_t frame_count;   /* N FRAME records following INIT */
    uint16_t init_size;     /* sizeof(struct int8_init) */
    uint16_t frame_stride;  /* sizeof(struct int8_frame) */
};

/* INIT — the per-tick loop's full READ-SET, seeded once.  The scalar block is
 * the union of the per-function gates' seed sets (see Task 3 Step 1 for the
 * exact field assembly); the large arrays carry the world state the loop reads
 * AND mutates (so the host can evolve without disk/seed-per-frame). */
struct int8_init {
    uint8_t  tilemap[INT8_TILEMAP_SIZE];   /* DAT_a0d8 grid */
    uint8_t  anim_channels[0x200];         /* channel A/B/C records (size set in Task 3) */
    uint8_t  entity_state[0x200];          /* spawn/entity arrays (size set in Task 3) */
    /* scalar register-set: the union of seed_globals across the gates.  Laid out
     * field-by-field in Task 3; placeholder size asserted at compile time there. */
    uint8_t  scalars[256];
};

/* FRAME — captured at the innermost-loop top.  Carries the comparison STATE plus
 * the TRAILING rng/input that drove the just-completed tick (see Global
 * Constraints "Frame-boundary timing").  FRAME[0] mirrors INIT's scalars; for
 * k>=1, replay tick k-1 by feeding rng+input then asserting state. */
struct int8_frame {
    uint8_t  rng;            /* rng_frame value that drove the just-completed tick */
    uint8_t  input;          /* input_state value that drove the just-completed tick */
    uint32_t tilemap_hash;   /* int8_tilemap_hash over the live tilemap */
    /* state assert-set: the union of the gates' cmp_* fields.  Field-by-field in
     * Task 3; placeholder size asserted at compile time there. */
    uint8_t  state[128];
};

#pragma pack(pop)

/* FNV-1a over the tilemap — cheap, order-sensitive mutation check. */
static inline uint32_t int8_tilemap_hash(const uint8_t *tm) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < INT8_TILEMAP_SIZE; i++) { h ^= tm[i]; h *= 16777619u; }
    return h;
}

#endif /* INT8_TRACE_H */
