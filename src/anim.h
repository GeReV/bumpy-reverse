#ifndef ANIM_H
#define ANIM_H

#include "bumpy.h"

/* ────────────────────────────────────────────────────────────────────────────
 *  anim.h — animation-channel FX module (Phase-5 reconstruction).
 *
 *  RECONSTRUCTED (Phase-5 Tasks 2–4): this header declares the anim-channel
 *  module's globals + the seven anim-channel function prototypes, all of which now
 *  have 1:1 reconstructed bodies in src/anim.c.  The globals landed in T2; the
 *  bodies landed across T3/T4 — apply_cell_animation (69aa) + the two steppers
 *  step_anim_channels_a/b (14e4/15a1) are validated on semantic state (allocator +
 *  channel-record table), and the two draw + two erase fns draw_anim_channels_a/b
 *  (165e/17c7) + erase_anim_channels_a/b (1a67/1b2b) are validated at descriptor
 *  level.  anim.obj contributes both its globals and these seven bodies to
 *  BUMPY.EXE (game_stubs.c no longer stubs them — no dup).  The host replay harness
 *  tools/anim_chan_ctest.c #includes src/anim.c and gates every body.
 *
 *  The anim subsystem drives the two per-cell animated overlay layers (channel A:
 *  3 slots; channel B: 4 slots).  Channel A has an allocator (apply_cell_animation,
 *  keyed on anim_target_cell + an action code); channel B has no allocator (it is
 *  spawn-populated by spawn_and_draw_level_entities, out of scope this phase).  Each
 *  channel's step fn advances its active slots along a byte-stream; the draw fn
 *  erases the old cell, blits the current frame, and saves under; the erase fn
 *  restores the background at the slot's current cell.
 *
 *  Provenance for every address: tools/anim_oracle.py header + local/build/
 *  anim_model.md §"Resolved anim DGROUP addresses" (Ghidra DGROUP 0x203b offsets,
 *  read live via the Ghidra MCP from the disassembly operands of the seven anim
 *  functions — see the Phase-5 T1 progress entry).
 *
 *  ── 12-byte channel-record layout (confirmed from the steppers' decomp) ────────
 *      [0]    active flag (0 free / 1 active / 0xff end-of-table terminator)
 *      [1]    cell
 *      [2..5] stream ptr (far: off@+2, seg@+4)
 *      [6]    cur frame byte
 *      [7]    pad
 *      [8..11] frame-data ptr (far: off@+8, seg@+10)
 *
 *  ── OWNERSHIP (the careful part; grep-verified — see src/anim.c per-symbol block)─
 *    OWNED BY anim.c (defined there; genuinely new — no other TU owns them):
 *      anim_channels_a_tbl/_seg_tbl   0x4c70/0x4c72  far-ptr slot table (3 slots)
 *      anim_channels_b_tbl/_seg_tbl   0x4cbc/0x4cbe  far-ptr slot table (4 slots)
 *      the 3 A + 4 B 12-byte channel records the slot tables point at
 *      anim_a_tiledef_tbl   0x2ede/0x2ee0  action*4 -> far-ptr tile-def (alloc)
 *      anim_a_frame_tbl     0x3d6a/0x3d6c  cmd*4   -> far-ptr frame data (step A)
 *      anim_b_frame_tbl     0x40a6/0x40a8  frame*4 -> far-ptr frame data (step B)
 *      anim_a_grid_tbl      0x32be/0x32c0  cell*4 grid-coord tbl (draw/erase A)
 *      anim_b_grid_tbl      0x343e/0x3440  cell*4 grid-coord tbl (draw/erase B)
 *      posA / posB          0xf4/0xf6 / 0x3f4/0x3f6  cell*4 pos tbls (draw A/B)
 *      the seven view descriptors (anim_a_erase/draw/clear_view, anim_b_*)
 *      p1_sprite (0x8884)   blit-descriptor far ptr (draw fns)
 *      the step-state scalars/working ptrs (g_anim_stream_ptr 0xa0be,
 *      g_anim_cur_cmd_byte 0x8578, anim_b_loop_idx 0x8566, anim_b_stream_ptr
 *      0xa0c2, anim_b_cur_frame_byte 0x8579)
 *
 *    OWNED ELSEWHERE (declared extern here, defined in their owning module — the
 *    anim functions read/write them but must NOT redefine them, to avoid a future
 *    dup-symbol when anim.obj links):
 *      anim_target_cell    0x856f  u8   — player.c (player.c:230; alloc input)
 *      g_anim_channel_idx  0x856c  u8   — player.c (player.c:1664; step-A loop idx)
 *      tilemap             0xa0d8  far  — game.c   (game.c:79; base tilemap layer)
 *      current_level       0x79b2  u8   — level.c  (level.c:106)
 * ──────────────────────────────────────────────────────────────────────────── */

/* ── channel-record table sizing ─────────────────────────────────────────────── */
#define ANIM_A_SLOTS     3
#define ANIM_B_SLOTS     4
#define ANIM_SLOT_REC_LEN 12

/* One 12-byte animation-channel record (slot).  The slot tables hold far ptrs to
   records of this layout; the steppers advance [2..5] (stream) and [6] (frame). */
typedef struct anim_chan_rec {
    u8  active;            /* +0  0 free / 1 active / 0xff terminator             */
    u8  cell;             /* +1  grid cell this slot animates                     */
    u16 stream_off;       /* +2  byte-stream ptr (far off)                        */
    u16 stream_seg;       /* +4  byte-stream ptr (far seg)                        */
    u8  frame;            /* +6  current frame byte                               */
    u8  pad;              /* +7  pad                                              */
    u16 data_off;         /* +8  frame-data ptr (far off)                         */
    u16 data_seg;         /* +10 frame-data ptr (far seg)                         */
} anim_chan_rec;

/* ── slot tables — OWNED BY anim.c (far-ptr tables; each entry -> a record) ──────
   A table = ANIM_A_SLOTS+1 entries: 3 usable slots + a 0xFF-terminator record
   (engine 0x4c64) the allocator scan stops on (see anim.c). */
extern anim_chan_rec __far *anim_channels_a_tbl[ANIM_A_SLOTS + 1]; /* DGROUP 0x4c70/0x4c72 (3 slots + 0xFF term) */
extern anim_chan_rec __far *anim_channels_b_tbl[ANIM_B_SLOTS + 1]; /* DGROUP 0x4cbc/0x4cbe (4 slots + 0xFF term) */

/* The records the slot tables point at (one fixed record per slot) + the A
   scan-terminator record (active=0xFF) the allocator stops on. */
extern anim_chan_rec anim_a_records[ANIM_A_SLOTS];
extern anim_chan_rec anim_b_records[ANIM_B_SLOTS];
extern anim_chan_rec anim_a_terminator;
extern anim_chan_rec anim_b_terminator;

/* ── per-action / per-frame far-ptr tables — OWNED BY anim.c ─────────────────────
   Tables of 4-byte FAR POINTERS (off-half @ N*4+0, seg-half @ N*4+2), indexed by
   action/cmd/frame byte; an entry's far ptr is rebuilt with MK_FP(seg, off) at the
   use site (same byte-blob representation as player.c's mode_script_tbl). */
#define ANIM_FARPTR_TBL_LEN  (256 * 4)
extern u8 anim_a_tiledef_tbl[ANIM_FARPTR_TBL_LEN]; /* DGROUP 0x2ede/0x2ee0 action*4 */
extern u8 anim_a_frame_tbl[ANIM_FARPTR_TBL_LEN];   /* DGROUP 0x3d6a/0x3d6c cmd*4     */
extern u8 anim_b_frame_tbl[ANIM_FARPTR_TBL_LEN];   /* DGROUP 0x40a6/0x40a8 frame*4   */

/* ── grid-coord / pos tables — OWNED BY anim.c (draw/erase A/B) ──────────────── */
extern u8 __far *anim_a_grid_tbl;     /* DGROUP 0x32be/0x32c0 — cell*4 grid-coord (A) */
extern u8 __far *anim_b_grid_tbl;     /* DGROUP 0x343e/0x3440 — cell*4 grid-coord (B) */
extern u8 __far *anim_posA_tbl;       /* DGROUP 0xf4/0xf6     — cell*4 pos tbl (A)    */
extern u8 __far *anim_posB_tbl;       /* DGROUP 0x3f4/0x3f6   — cell*4 pos tbl (B)    */

/* ── view descriptors — OWNED BY anim.c (the draw/erase gfx-overlay passes) ───── */
extern u8 __far *anim_a_erase_view;   /* DGROUP 0x8d4 — draw A erase pass  (->0x80bc) */
extern u8 __far *anim_a_draw_view;    /* DGROUP 0x8e0 — draw A save-under  (->0x93b8) */
extern u8 __far *anim_a_clear_view;   /* DGROUP 0x8c0 — erase A (restore_bg_view)     */
extern u8 __far *anim_b_view0;        /* DGROUP 0x8c8 — draw B pass 0      (->0x80ac) */
extern u8 __far *anim_b_view1;        /* DGROUP 0x8cc — draw B passes  (->0x80ac/bc)  */
extern u8 __far *anim_b_draw_view;    /* DGROUP 0x8d0 — draw B save-under  (->0x93b8) */
extern u8 __far *anim_b_clear_view;   /* DGROUP 0x8bc — erase B (restore_bg_view)     */

/* ── blit descriptor — OWNED BY anim.c (the far ptr the draw fns pass to blit) ── */
extern u8 __far *p1_sprite;           /* DGROUP 0x8884 — blit descriptor far ptr      */

/* ── step-state scalars / working ptrs — OWNED BY anim.c ────────────────────────*/
extern u8 __far *g_anim_stream_ptr;   /* DGROUP 0xa0be/0xa0c0 — working stream (A)    */
extern u8  g_anim_cur_cmd_byte;       /* DGROUP 0x8578 — cur cmd byte (step A)        */
extern u8  anim_b_loop_idx;           /* DGROUP 0x8566 — loop idx (step B)            */
extern u8 __far *anim_b_stream_ptr;   /* DGROUP 0xa0c2/0xa0c4 — working stream (B)    */
extern u8  anim_b_cur_frame_byte;     /* DGROUP 0x8579 — cur frame byte (step B)      */

/* ── globals OWNED ELSEWHERE (extern — must NOT be redefined in anim.c) ───────── */
extern u8  anim_target_cell;          /* player.c 0x856f — channel-A allocator input  */
extern u8  g_anim_channel_idx;        /* player.c 0x856c — step-A channel loop index  */
extern u8 __far *tilemap;             /* game.c   0xa0d8 — base tilemap layer far ptr */
/* current_level (level.c 0x79b2) is declared extern by level.h; anim.c includes it
   transitively via bumpy.h only if needed — not required by the skeleton. */

/* ════════════════════════════════════════════════════════════════════════════
 *  Anim functions — DECLARED here, BODIES reconstructed in src/anim.c (Phase-5 T3/T4).
 *  All seven are now defined 1:1 in anim.c (game_stubs.c no longer stubs them).
 *  These prototypes match the existing declarations in game.h (the six step/draw/
 *  erase fns) and player.h (apply_cell_animation) — restated here for module
 *  documentation.
 *  Engine seg-1000 offsets are noted for the harness's per-fn registry.
 * ════════════════════════════════════════════════════════════════════════════ */
void apply_cell_animation(u8 fx_code);  /* 1000:69aa — channel-A allocator           */
void step_anim_channels_a(void);        /* 1000:14e4 — advance 3 A channels           */
void step_anim_channels_b(void);        /* 1000:15a1 — advance 4 B channels           */
void draw_anim_channels_a(void);        /* 1000:165e — erase+blit+save-under (A)       */
void draw_anim_channels_b(void);        /* 1000:17c7 — erase+blit+save-under (B)       */
void erase_anim_channels_a(void);       /* 1000:1a67 — restore_bg_view current (A)     */
void erase_anim_channels_b(void);       /* 1000:1b2b — restore_bg_view current (B)     */

#endif /* ANIM_H */
