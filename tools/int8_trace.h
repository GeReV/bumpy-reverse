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
#define INT8_VERSION 3
#define INT8_TILEMAP_SIZE 0x300   /* matches p1_spine_ctest.c TILEMAP_SIZE */

/* Low-DGROUP static-data window (INT8_VERSION 2/3 read-set extension).
 * The per-tick P1 spine reads two families of static, loader-relocated DGROUP data
 * through far pointers that are NOT in the scalar union:
 *   (a) the MOVE-SCRIPT system — enter_game_mode() reads mode_script_tbl[mode]
 *       (DGROUP 0x2252) -> a [steps,facing,off,seg] header -> p1_move_script (the
 *       [anim,dx,dy] step array), and p1_step_scripted_move derefs it every tick;
 *   (b) the CELL-ANIMATION system — apply_cell_animation()/the anim-channel steppers
 *       read anim_a_tiledef_tbl (0x2ede), anim_a_frame_tbl (0x3d6a), anim_b_frame_tbl
 *       (0x40a6), the grid/pos tables (0xf4/0x32be/0x343e/0x3f4) and the tile-def /
 *       frame-data / byte-stream blobs they point at — which drive the tilemap
 *       cell-animation writes and the slot stream pointers.
 * Both families are static data the loader relocated to the runtime DGROUP segment.
 * Without them a real replay read NULL/zero far ptrs (crash) or zeroed tile-defs
 * (spurious tilemap writes -> downstream move_override/grid divergence).  We capture
 * ONE contiguous low-DGROUP window [MOVE_DATA_OFF, +MOVE_DATA_LEN) covering the whole
 * region (measured extent from the unpacked image: 0x137c..0x41a6), plus the live
 * p1_move_script far ptr, and seed it into the host far-memory at the captured
 * runtime DGROUP linear base so every far hop resolves 1:1. */
#define INT8_MOVE_DATA_OFF 0x0000   /* DGROUP offset of the captured window start  */
#define INT8_MOVE_DATA_LEN 0x4600   /* covers 0x0000..0x4600 (move + anim static;
                                       last table anim_b_frame_tbl@0x40a6 ends 0x44a6) */

/* Channel-record table geometry — must match src/anim.h (3 A + 4 B slots, each a
 * 12-byte anim_chan_rec) and tools/spawn_ctest.c / anim_chan_ctest.c's 7*12 SNAP.
 * INIT carries the 7 records verbatim (3 A then 4 B, 12 raw bytes each). */
#define INT8_ANIM_A_SLOTS  3
#define INT8_ANIM_B_SLOTS  4
#define INT8_ANIM_N_SLOTS  (INT8_ANIM_A_SLOTS + INT8_ANIM_B_SLOTS)   /* 7  */
#define INT8_ANIM_REC_LEN  12
#define INT8_ANIM_RECS_LEN (INT8_ANIM_N_SLOTS * INT8_ANIM_REC_LEN)   /* 84 */

#pragma pack(push, 1)

struct int8_header {
    char     magic[4];      /* "BINT" */
    uint16_t version;       /* == INT8_VERSION */
    uint16_t dgroup_seg;    /* DGROUP calibration seg used at capture (0x185f) */
    uint16_t frame_count;   /* N FRAME records following INIT */
    uint16_t init_size;     /* sizeof(struct int8_init) */
    uint16_t frame_stride;  /* sizeof(struct int8_frame) */
};

/* INIT scalar register-set — the UNION of seed_globals across the per-function
 * gates (Task 3 Step 1).  Each field is annotated with the gate it came from:
 *   [phys]  tools/physics_ctest.c     seed_globals (:202-214)
 *   [spine] tools/p1_spine_ctest.c    seed_globals (:245-258)
 *   [items] tools/items_ctest.c       seed_globals (:239-253)
 *   [anim]  tools/anim_chan_ctest.c   ANIMSNAP scalars (:154-159)
 *   [spawn] tools/spawn_ctest.c       SPAWNSNAP globals (:191-199)
 *   [sess]  brief Step 1 session/level set
 * Fields shared by several gates are listed once under their primary gate.
 * Order is layout-stable: s16 first, then u16 pairs, then u8. */
struct int8_scalars {
    /* ── s16 player/physics + grid + bbox state ── */
    int16_t  p1_pixel_x;        /* [phys] 0x9290 */
    int16_t  p1_pixel_y;        /* [phys] 0x9292 */
    int16_t  p1_grid_x_new;     /* [spine] */
    int16_t  p1_grid_y_new;     /* [spine] */
    int16_t  p1_grid_x;         /* [spine] */
    int16_t  p1_grid_y;         /* [spine] */
    int16_t  p1_grid_x_prev;    /* [spine] */
    int16_t  p1_grid_y_prev;    /* [spine] */
    int16_t  p1_scroll_x;       /* [spine] */
    int16_t  p1_scroll_y;       /* [spine] */
    int16_t  pvp_p1_x0;         /* [spine] bbox left  */
    int16_t  pvp_p1_x1;         /* [spine] bbox right */
    int16_t  pvp_p1_y0;         /* [spine] bbox top   */
    int16_t  pvp_p1_y1;         /* [spine] bbox bottom */
    int16_t  pending_erase_x;   /* [spine] */
    int16_t  pending_erase_y;   /* [spine] */
    int16_t  sound_device_state;/* [items] 0x689c (==4 -> OPL ids) */

    /* ── u16 score + spawn frame base + anim stream far ptrs ── */
    uint16_t score_lo;          /* [items] 0xa0d4 */
    uint16_t score_hi;          /* [items] 0xa0d6 */
    uint16_t p2_frame_base;     /* [spawn] */
    uint16_t g_anim_stream_off; /* [anim] channel-A stream far off */
    uint16_t g_anim_stream_seg; /* [anim] channel-A stream far seg */
    uint16_t anim_b_stream_off; /* [anim] channel-B stream far off */
    uint16_t anim_b_stream_seg; /* [anim] channel-B stream far seg */
    uint16_t p1_move_script_off;/* [move] live p1_move_script far off (DGROUP 0xa1ac) */
    uint16_t p1_move_script_seg;/* [move] live p1_move_script far seg (DGROUP 0xa1ae) */

    /* ── u8 player/physics flags + cells ── */
    uint8_t  p1_move_anim;      /* [phys] 0x792a-ish move-anim state */
    uint8_t  game_mode;         /* [phys] 0x792c */
    uint8_t  p1_move_step_idx;  /* [phys] */
    uint8_t  p1_facing_left;    /* [phys] */
    uint8_t  p1_move_steps_left;/* [phys] */
    uint8_t  input_state;       /* [phys] 0x8244 */
    uint8_t  physics_frozen;    /* [phys] 0xa0ce */
    uint8_t  move_override;     /* [phys] */
    uint8_t  p1_cell;           /* [phys] 0x856e */
    uint8_t  move_locked;       /* [phys] */
    uint8_t  prev_game_mode;    /* [phys] 0x8552 */
    uint8_t  p1_step_col_count; /* [phys] 0x855e */
    uint8_t  pending_erase_count;/* [spine] */
    uint8_t  level_complete_flag;/* [spine] 0xa1b1 */

    /* ── u8 item/exit/level semantic state ── */
    uint8_t  items_remaining;   /* [items] */
    uint8_t  level_exit_cell;   /* [items] */
    uint8_t  level_complete_anim_counter; /* [items] */
    uint8_t  p1_item_code;      /* [items] */
    uint8_t  anim_target_cell;  /* [items]/[anim] 0x856f */
    uint8_t  current_level;     /* [items] 0x79b2 */
    uint8_t  move_step_count;   /* [items] 0x824c */

    /* ── u8 anim-channel scalars ── */
    uint8_t  g_anim_channel_idx;  /* [anim] 0x856c */
    uint8_t  g_anim_cur_cmd_byte; /* [anim] */
    uint8_t  anim_b_loop_idx;     /* [anim] 0x8566 */
    uint8_t  anim_b_cur_frame_byte;/* [anim] */

    /* ── s8/u8 player-2 spawn state ── */
    int8_t   p2_cell;           /* [spawn] */
    uint8_t  p2_move_state;     /* [spawn] */
    uint8_t  p2_ai_threshold;   /* [spawn] */

    /* ── u8 session / level / frame-boundary control ── */
    uint8_t  round_continue_flag;   /* [sess] 0x9d30 */
    uint8_t  session_continue_flag; /* [sess] 0x856d */
    uint8_t  frame_abort_flag;      /* [sess] 0x928d */
    uint8_t  settle_countdown;      /* [sess] */
    uint8_t  rng_frame;             /* [sess] FRAME[0] mirror of the seed rng */
};

/* INIT — the per-tick loop's full READ-SET, seeded once.  The scalar block is
 * the explicit union above; the large arrays carry the world state the loop reads
 * AND mutates (so the host can evolve without disk/seed-per-frame). */
struct int8_init {
    uint8_t  tilemap[INT8_TILEMAP_SIZE];          /* DAT_a0d8 grid */
    uint8_t  anim_channels[INT8_ANIM_RECS_LEN];   /* 3 A + 4 B records, 12 bytes each */
    uint8_t  entity_state[0x200];                 /* [v2] [0x00..0x40)=p1_sprite pointee,
                                                     [0x40..0x80)=p2_sprite pointee (the
                                                     +0x14/+0x16 origin words p1/p2_update_
                                                     grid_cell read); rest reserved zeros */
    uint8_t  move_data[INT8_MOVE_DATA_LEN];       /* [v2/v3] low-DGROUP move+anim static window */
    struct int8_scalars scalars;
};

/* FRAME state assert-set — the union of the gates' cmp_* output fields.  These are
 * the post-tick comparison points; FRAME[0] mirrors INIT's seed.  Mirrors the
 * scalar union but only the fields a gate actually COMPARES on exit. */
struct int8_frame_state {
    /* [phys] cmp_exit / [spine] cmp_grid+cmp_adv+cmp_bbox */
    int16_t  p1_pixel_x, p1_pixel_y;
    int16_t  p1_grid_x_new, p1_grid_y_new;
    int16_t  p1_grid_x, p1_grid_y;
    int16_t  p1_grid_x_prev, p1_grid_y_prev;
    int16_t  pvp_p1_x0, pvp_p1_x1, pvp_p1_y0, pvp_p1_y1;
    int16_t  p1_pixel_y_dup;          /* [items] p1_pixel_y compare (alias kept explicit) */
    /* [items] score / semantic state */
    uint16_t score_lo, score_hi;
    uint8_t  p1_move_anim, game_mode, p1_move_step_idx, p1_facing_left;
    uint8_t  p1_move_steps_left, input_state, physics_frozen, move_override;
    uint8_t  p1_cell, move_locked, prev_game_mode, p1_step_col_count;
    uint8_t  pending_erase_count, level_complete_flag;
    uint8_t  items_remaining, level_exit_cell, level_complete_anim_counter;
    uint8_t  p1_item_code, anim_target_cell, current_level, move_step_count;
    /* [spawn] p2 state */
    int8_t   p2_cell;
    uint8_t  p2_move_state, p2_frame_base_lo, p2_frame_base_hi;
};

/* FRAME — captured at the innermost-loop top.  Carries the comparison STATE plus
 * the TRAILING rng/input that drove the just-completed tick (see Global
 * Constraints "Frame-boundary timing").  FRAME[0] mirrors INIT's scalars; for
 * k>=1, replay tick k-1 by feeding rng+input then asserting state. */
struct int8_frame {
    uint8_t  rng;            /* rng_frame value that drove the just-completed tick */
    uint8_t  input;          /* input_state value that drove the just-completed tick */
    uint32_t tilemap_hash;   /* int8_tilemap_hash over the live tilemap */
    struct int8_frame_state state;
};

#pragma pack(pop)

/* Layout pins — recompute and update the literals on ANY field change (still v1,
 * pre-release).  Values measured on the x86 host (see Task 3 report). */
_Static_assert(sizeof(struct int8_scalars)     ==    85, "int8_scalars layout pinned");
_Static_assert(sizeof(struct int8_init)        == 19369, "int8_init layout pinned");
_Static_assert(sizeof(struct int8_frame_state) ==   55, "int8_frame_state layout pinned");
_Static_assert(sizeof(struct int8_frame)       ==   61, "int8_frame layout pinned");

/* FNV-1a over the tilemap — cheap, order-sensitive mutation check. */
static inline uint32_t int8_tilemap_hash(const uint8_t *tm) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < INT8_TILEMAP_SIZE; i++) { h ^= tm[i]; h *= 16777619u; }
    return h;
}

#endif /* INT8_TRACE_H */
