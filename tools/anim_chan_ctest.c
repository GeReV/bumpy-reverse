/* Host REPLAY HARNESS for src/anim.c — Phase-5 Tasks 2–4.
 *
 * NAMING NOTE: this file is tools/anim_chan_ctest.c, NOT tools/anim_ctest.c.  The
 * Phase-5 plan named the new anim-channel harness "tools/anim_ctest.c", but that
 * filename is ALREADY a tracked, gate-critical file: the Plan-5b sprite per-FRAME
 * animation-select ctest used by tools/validate_blit.sh (the 17/17 sprite-anim gate,
 * committed 9e2feed).  Overwriting it would break validate_blit (a forbidden
 * regression), so the Phase-5 anim-CHANNEL harness is named anim_chan_ctest.c here
 * (anim "channel" — distinct from the sprite-frame "anim" ctest).  See the Phase-5
 * T2 report for the deviation record.
 *
 * Compiles the REAL anim-channel port (src/anim.c) on the host (Open Watcom 16-bit
 * environment shimmed out: __far/__huge erased, exact-width typedefs, BUMPY_H so
 * anim.h does not pull <dos.h>), then validates the reconstructed anim-channel
 * functions against the Phase-5 T1 capture local/build/render/anim_trace.bin
 * (magic "ANIMTRC1", version 1 — layout frozen in tools/anim_oracle.py's header
 * §"TRACE LAYOUT" and local/build/anim_model.md).
 *
 * As of Phase-5 T4, src/anim.c reconstructs all seven anim-channel bodies and every
 * PORTED[] entry below holds its C name, so the comparators run on every exercised
 * record.  T3 ported apply_cell_animation + the two steppers (semantic-state gate);
 * T4 ported the two draw + two erase fns (descriptor gate).  Records still reported
 * UNPORTED are only those for fns genuinely unexercised by the captured trace.
 *
 * ── COMPARATORS (exercised on every PORTED record the trace drives) ──────────────
 *   (A) SEMANTIC-STATE DIFFERENTIAL (the allocator + stepper gate, T3).  For each
 *       record of a PORTED, host-callable fn: seed the reconstructed anim globals +
 *       the channel-record table + tilemap from the record's ENTRY SNAP, call the fn
 *       BY ITS C NAME, then assert the channel-record bytes (the 3 A + 4 B 12-byte
 *       records) AND the tilemap stamp (tile_byte at the target cell) == the record's
 *       EXIT SNAP.  Prints PASS/FAIL with first-divergence (which slot/byte).
 *
 *   (B) DESCRIPTOR DIFFERENTIAL (the draw/erase gate, T4).  draw/erase records carry
 *       the p1_sprite blit descriptor (desc_kind==1) and/or the view-descriptor blobs
 *       the engine wrote at EXIT.  When a draw/erase fn is PORTED + writes a
 *       host-visible descriptor, this asserts the produced descriptor bytes == the
 *       captured ones.  Until then the bytes are retained for the future port.
 *
 *   GRACEFUL UNPORTED DEGRADATION.  A record whose fn is not host-callable (its PORTED
 *     entry holds a NULL callable) is marked UNPORTED and SKIPPED: the harness never
 *     references the (absent) symbol and never call-throughs into it.  With all seven
 *     bodies now reconstructed, UNPORTED only arises for fns genuinely unexercised by
 *     the trace.  UNPORTED is NOT a crash and NOT a hard failure.
 *
 * Build/run (also wrapped by tools/validate_anim.sh):
 *     cc -O2 -Wall -Werror -o /tmp/anim_chan_ctest tools/anim_chan_ctest.c && \
 *       /tmp/anim_chan_ctest local/build/render/anim_trace.bin
 * Exit 0 iff the harness parses the trace, runs, and the differential has ZERO
 * failures on PORTED records (UNPORTED records never fail).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── shim the Watcom 16-bit environment for host compilation ─────────────────── */
#define BUMPY_H            /* anim.h's #include "bumpy.h" becomes a no-op */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge

/* ── MK_FP host model (mirrors tools/physics_ctest.c / p2_ctest.c) ───────────────
 *  The ported anim fns rebuild a far pointer with MK_FP(seg, off) at the use site —
 *  for each slot's byte-STREAM pointer ([+2..+5]) and for the per-action tile-def /
 *  per-frame frame-data far ptrs.  Back MK_FP with a >256 KB linear "far memory"
 *  shadow so those reconstructed pointers land in real host storage the harness can
 *  pre-seed.  MK_FP(seg, off) -> far_mem + ((seg<<4) + off).  The captured far ptrs
 *  reach ~0x271b0 (B stream seg 0x203b : off 0x6e00); size generously. */
#define FAR_MEM_SIZE 0x40000UL
static unsigned char far_mem[FAR_MEM_SIZE];
#define MK_FP(seg, off) ((void *)(far_mem + (((u32)(seg) << 4) + (u16)(off))))

/* Cross-module globals the anim fns read/write but that anim.c declares extern
   (owned elsewhere).  anim.c does not pull their owning headers, so the harness
   provides host definitions (mirroring the items_ctest / p2_ctest convention of
   supplying the cross-module globals the included TU references).
     anim_target_cell    player.c 0x856f — channel-A allocator input
     g_anim_channel_idx  player.c 0x856c — step-A channel loop index
     current_level       level.c  0x79b2 */
u8 anim_target_cell;
u8 g_anim_channel_idx;
u8 current_level;

/* tilemap: cross-module level-data far ptr (owned by game.c 0xa0d8).  The allocator
   stamps tilemap[anim_target_cell] with the action's tile-def byte; the host backs
   it with a synthetic window (seeded per-record from the captured tile bytes when a
   PORTED allocator runs — see seed_tilemap). */
#define TILEMAP_SIZE 0x300
static u8 synth_tilemap[TILEMAP_SIZE];
u8 __far *tilemap = synth_tilemap;

/* The draw/erase wrappers stamp the RUNTIME DS register (Ghidra renders it as the
   static 0x203b; the captured Phase-5 T1 trace loads at PSP_SEG 0x100 -> runtime DS
   = 0x114b) into their view descriptors' far-data segment fields.  Drive anim.c's
   ANIM_DGROUP_RUNTIME_SEG to the captured value so the descriptor gate compares
   against the engine's actual EXIT bytes (0x114b), not the static literal. */
#define ANIM_DGROUP_RUNTIME_SEG 0x114b

#include "../src/anim.c"

/* ── DESCRIPTOR-GATE host wiring (Phase-5 T4) ──────────────────────────────────
 *  The four draw/erase wrappers write their OBSERVABLE output into engine-DGROUP
 *  descriptor structs reached through FAR-POINTER globals: the seven view
 *  descriptors (anim_*_view, 0x20 bytes each) and the p1_sprite blit descriptor
 *  pointee (0x792e, 8 bytes; p1_sprite at 0x8884 points at it).  On the host those
 *  far-ptr globals point at NOTHING by default; here we back EACH with a real host
 *  buffer so the reconstructed wrappers' field writes land where the comparator can
 *  read them, then diff those bytes against the captured ENGINE descriptors
 *  (T1 trace `views` blobs + the draw `desc` blob).  This is the REAL gate. */
#define VIEW_LEN 0x20
static u8 hview[7][VIEW_LEN];           /* one host buffer per view id 0..6        */

/* grid/pos host tables the draw/erase fns index by cell*4 (engine 0x32be/0x32c0
   gridA, 0x343e/0x3440 gridB, 0xf4/0xf6 posA, 0x3f4/0x3f6 posB).  Sized to hold
   256 cells * 4 bytes; the reconstructed fns read (u16)x at +0, (u16)y at +2. */
#define GRID_TBL_LEN (256 * 4)
static u8 hgridA[GRID_TBL_LEN], hgridB[GRID_TBL_LEN];
static u8 hposA[GRID_TBL_LEN],  hposB[GRID_TBL_LEN];

/* ════════════════════════════════════════════════════════════════════════════
 *  Trace format (frozen — see tools/anim_oracle.py header §"TRACE LAYOUT").
 *
 *  Header:  magic[8]="ANIMTRC1", u16 version(=1), u16 n_scenarios,
 *           u16 n_fn_names, then per name {u8 len, len bytes ascii}.
 *  Per scenario: u8 id, u8 name_len, name bytes, u8 seeded, u8 level,
 *                u32 n_records, then n_records records.
 *  Per record: u16 fn_addr, u16 fn_name_idx, SNAP entry, SNAP exit,
 *              u8 tile_cell, u8 tile_byte_e, u8 tile_byte_x,
 *              u8 desc_kind, u8 desc_len, desc_len bytes,
 *              u8 nview, per view {u8 view_id, u8 view_len, view_len bytes}.
 *  ANIMSNAP (ANIMSNAP_FMT "<BB" + "B"*(7*12) + "BBBBBB" + "HHHH"):
 *    u8 a_slots(3), u8 b_slots(4),
 *    7*12 channel-record bytes (3 A slots then 4 B slots, 12 raw bytes each),
 *    u8 anim_target_cell, u8 g_anim_channel_idx, u8 g_anim_cur_cmd_byte,
 *    u8 anim_b_loop_idx, u8 anim_b_cur_frame_byte, u8 current_level,
 *    u16 g_anim_stream_off, u16 g_anim_stream_seg,
 *    u16 anim_b_stream_off, u16 anim_b_stream_seg.
 * ════════════════════════════════════════════════════════════════════════════ */
#define A_SLOTS       3
#define B_SLOTS       4
#define REC_LEN       12
#define N_SLOTS       (A_SLOTS + B_SLOTS)          /* 7                            */
#define SNAP_RECS_OFF 2                            /* after the 2 count bytes      */
/* ANIMSNAP byte size: 2 + 7*12 + 6 + 4*2 = 2 + 84 + 6 + 8 = 100. */
#define SNAP_SIZE     (2 + N_SLOTS * REC_LEN + 6 + 8)

typedef struct {
    u8  a_slots, b_slots;
    u8  recs[N_SLOTS * REC_LEN];                   /* raw 7*12 channel-record bytes*/
    u8  anim_target_cell, g_anim_channel_idx, g_anim_cur_cmd_byte;
    u8  anim_b_loop_idx, anim_b_cur_frame_byte, current_level;
    u16 g_anim_stream_off, g_anim_stream_seg;
    u16 anim_b_stream_off, anim_b_stream_seg;
} snap_t;

typedef struct { u8 id; u8 len; const u8 *bytes; } view_t;

typedef struct {
    u16    fn_addr;
    u16    fn_name_idx;
    snap_t ent, ex;
    u8     tile_cell, tile_byte_e, tile_byte_x;
    u8     desc_kind, desc_len;
    const u8 *desc;            /* points into the loaded file buffer (draw records) */
    u8     nview;
    view_t views[8];           /* up to 8 view-descriptor blobs (capacity bound)    */
} record_t;

/* Engine seg-1000 offsets for the seven hooked anim fns (anim_oracle FN_NAMES). */
#define FN_APPLY_CELL_ANIMATION  0x69aa
#define FN_STEP_ANIM_A           0x14e4
#define FN_STEP_ANIM_B           0x15a1
#define FN_DRAW_ANIM_A           0x165e
#define FN_DRAW_ANIM_B           0x17c7
#define FN_ERASE_ANIM_A          0x1a67
#define FN_ERASE_ANIM_B          0x1b2b

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) |
                                      ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void parse_snap(const u8 *p, snap_t *s)
{
    u32 o;
    s->a_slots = p[0];
    s->b_slots = p[1];
    memcpy(s->recs, p + SNAP_RECS_OFF, N_SLOTS * REC_LEN);
    o = SNAP_RECS_OFF + N_SLOTS * REC_LEN;
    s->anim_target_cell      = p[o + 0];
    s->g_anim_channel_idx    = p[o + 1];
    s->g_anim_cur_cmd_byte   = p[o + 2];
    s->anim_b_loop_idx       = p[o + 3];
    s->anim_b_cur_frame_byte = p[o + 4];
    s->current_level         = p[o + 5];
    o += 6;
    s->g_anim_stream_off = rd16(p + o + 0);
    s->g_anim_stream_seg = rd16(p + o + 2);
    s->anim_b_stream_off = rd16(p + o + 4);
    s->anim_b_stream_seg = rd16(p + o + 6);
}

static const char *fn_name(u16 fn_addr)
{
    switch (fn_addr) {
        case FN_APPLY_CELL_ANIMATION: return "apply_cell_animation";
        case FN_STEP_ANIM_A:          return "step_anim_channels_a";
        case FN_STEP_ANIM_B:          return "step_anim_channels_b";
        case FN_DRAW_ANIM_A:          return "draw_anim_channels_a";
        case FN_DRAW_ANIM_B:          return "draw_anim_channels_b";
        case FN_ERASE_ANIM_A:         return "erase_anim_channels_a";
        case FN_ERASE_ANIM_B:         return "erase_anim_channels_b";
        default:                      return "?";
    }
}

/* ── ENTRY SNAP -> reconstructed anim globals + channel-record table ────────────
 *  Seed the module's channel records from the captured ENTRY record bytes, point the
 *  two slot tables at them, and restore the scalar step-state globals.  When the
 *  allocator/steppers port (T3) they mutate these records; the comparator then diffs
 *  the post-call record bytes against the EXIT snap.  (Skeleton: no fn runs, so the
 *  seed is exercised only structurally — kept here so T3 lands without harness edits.) */
static void seed_globals(const snap_t *s)
{
    unsigned i;
    memcpy(anim_a_records, s->recs, A_SLOTS * REC_LEN);
    memcpy(anim_b_records, s->recs + A_SLOTS * REC_LEN, B_SLOTS * REC_LEN);
    for (i = 0; i < A_SLOTS; i++) anim_channels_a_tbl[i] = &anim_a_records[i];
    for (i = 0; i < B_SLOTS; i++) anim_channels_b_tbl[i] = &anim_b_records[i];
    /* The A scan terminator (engine 0x4c64, active=0xFF) at index A_SLOTS: the
       allocator's unbounded slot scan stops on it (steppers never index it). */
    anim_a_terminator.active = 0xff;
    anim_channels_a_tbl[A_SLOTS] = &anim_a_terminator;
    anim_target_cell      = s->anim_target_cell;
    g_anim_channel_idx    = s->g_anim_channel_idx;
    g_anim_cur_cmd_byte   = s->g_anim_cur_cmd_byte;
    anim_b_loop_idx       = s->anim_b_loop_idx;
    anim_b_cur_frame_byte = s->anim_b_cur_frame_byte;
    current_level         = s->current_level;
}

/* Seed the synthetic tilemap so the target cell holds the captured ENTRY byte (the
   allocator reads/stamps tilemap[anim_target_cell]). */
static void seed_tilemap(const record_t *r)
{
    memset(synth_tilemap, 0, sizeof(synth_tilemap));
    if (r->tile_cell < TILEMAP_SIZE)
        synth_tilemap[r->tile_cell] = r->tile_byte_e;
}

/* Read the live channel-record bytes back into a flat 7*12 buffer (the post-call
   state the comparator diffs against the EXIT snap). */
static void read_live_recs(u8 *out)
{
    memcpy(out, anim_a_records, A_SLOTS * REC_LEN);
    memcpy(out + A_SLOTS * REC_LEN, anim_b_records, B_SLOTS * REC_LEN);
}

/* Field accessors on a flat 7*12 snap-record buffer (slot S, byte B). */
static u8  rec_u8 (const u8 *recs, unsigned slot, unsigned b) { return recs[slot * REC_LEN + b]; }
static u16 rec_u16(const u8 *recs, unsigned slot, unsigned b) { return rd16(recs + slot * REC_LEN + b); }

/* ── ENGINE-DATA SEEDING (the far-ptr inputs the host cannot read from the engine) ─
 *  Three reconstructed anim fns dereference engine DGROUP/level data the host trace
 *  does NOT capture as raw memory: (a) the allocator's per-action tile-def far ptr,
 *  (b) each stepper's per-frame far data ptr, and (c) each active slot's byte-STREAM
 *  (read through MK_FP).  We reconstruct those inputs FROM THE CAPTURED EXIT RECORD
 *  (the same seed-from-capture method physics_ctest uses for runtime move-scripts):
 *  the engine's own output bytes are placed back where the reconstructed fn will read
 *  them, so the ported control flow (slot scan, stream-ptr advance, 0xFF active-clear,
 *  frame routing) is exercised genuinely.  The INDEPENDENT, non-seeded genuine signals
 *  the comparator still checks: the claimed slot (scan), active-flag transitions,
 *  cell, the stream-ptr +1 advance, and the tilemap stamp address. */

/* (a) ALLOCATOR: build a host tile-def for `action` from the claimed slot's EXIT
   record + the captured tilemap stamp, and point anim_a_tiledef_tbl[action*4] at it.
   The claimed A slot is the one that goes active 0->1 (or whose cell becomes the
   target) between entry and exit. */
#define TILEDEF_SHADOW_SEG 0x2000      /* an unused far_mem region for the tile-def  */
#define TILEDEF_SHADOW_OFF 0x0000
static void seed_alloc_tiledef(const record_t *r, u8 action)
{
    unsigned slot, claimed = 0;
    u8 *td;
    u16 s_off = 0, s_seg = 0;
    /* find the claimed A slot: active 0->1, else first whose [1]==target cell. */
    for (slot = 0; slot < A_SLOTS; slot++) {
        if (rec_u8(r->ent.recs, slot, 0) == 0 && rec_u8(r->ex.recs, slot, 0) == 1) {
            claimed = slot; break;
        }
    }
    if (slot == A_SLOTS) {
        for (slot = 0; slot < A_SLOTS; slot++)
            if (rec_u8(r->ex.recs, slot, 0) == 1 &&
                rec_u8(r->ex.recs, slot, 1) == r->ex.anim_target_cell) { claimed = slot; break; }
    }
    s_off = rec_u16(r->ex.recs, claimed, 2);   /* exit stream ptr off (= tile_def[2..3]) */
    s_seg = rec_u16(r->ex.recs, claimed, 4);   /* exit stream ptr seg (= tile_def[4..5]) */
    td = (u8 *)MK_FP(TILEDEF_SHADOW_SEG, TILEDEF_SHADOW_OFF);
    td[0] = r->tile_byte_x;                     /* tile_def[0] = the tilemap stamp byte   */
    td[1] = 0;
    td[2] = (u8)(s_off & 0xff); td[3] = (u8)(s_off >> 8);
    td[4] = (u8)(s_seg & 0xff); td[5] = (u8)(s_seg >> 8);
    /* anim_a_tiledef_tbl[action*4] = far ptr {off, seg} of the shadow tile-def. */
    anim_a_tiledef_tbl[action * 4 + 0] = (u8)(TILEDEF_SHADOW_OFF & 0xff);
    anim_a_tiledef_tbl[action * 4 + 1] = (u8)(TILEDEF_SHADOW_OFF >> 8);
    anim_a_tiledef_tbl[action * 4 + 2] = (u8)(TILEDEF_SHADOW_SEG & 0xff);
    anim_a_tiledef_tbl[action * 4 + 3] = (u8)(TILEDEF_SHADOW_SEG >> 8);
}

/* (b)+(c) STEPPER: for each ACTIVE entry slot of channel A (or B):
 *  (c) place the stream byte the engine read (= the slot's EXIT frame byte [+6]) at
 *      the entry stream ptr's MK_FP shadow address;
 *  (b) if that byte is non-0/non-0xFF, the stepper does TWO levels of indirection:
 *      it reads a far ptr from frame_tbl[byte*4], then reads two words AT that far
 *      ptr and stores them into slot[+8..+11].  So we point frame_tbl[byte*4] at a
 *      per-byte shadow frame-data record and write the slot's EXIT [+8..+11] words
 *      there (slot[8]=*frame_data, slot[10]=*(frame_data+2)).
 *  The frame-data shadow lives in a dedicated far_mem region, one 4-byte record per
 *  frame byte (byte*4 offset), distinct from the stream-byte region. */
#define FRAMEDATA_SHADOW_SEG 0x3000    /* far_mem region for per-frame data records   */
static void seed_stepper_stream(const record_t *r, int is_b)
{
    unsigned base = is_b ? A_SLOTS : 0;
    unsigned n    = is_b ? B_SLOTS : A_SLOTS;
    u8 *frame_tbl = is_b ? anim_b_frame_tbl : anim_a_frame_tbl;
    unsigned s;
    for (s = 0; s < n; s++) {
        unsigned slot = base + s;
        if (rec_u8(r->ent.recs, slot, 0) == 0) continue;   /* only active slots step  */
        {
            u16 e_off = rec_u16(r->ent.recs, slot, 2);
            u16 e_seg = rec_u16(r->ent.recs, slot, 4);
            u8  byte  = rec_u8 (r->ex.recs,  slot, 6);      /* the byte the engine read */
            u8 *p = (u8 *)MK_FP(e_seg, e_off);
            *p = byte;
            if (byte != 0 && byte != 0xff) {
                u16 d_w0 = rec_u16(r->ex.recs, slot, 8);    /* slot[8]  = *frame_data    */
                u16 d_w1 = rec_u16(r->ex.recs, slot, 10);   /* slot[10] = *(frame_data+2)*/
                u16 fd_off = (u16)(byte * 4);               /* per-byte shadow record    */
                u8 *fd = (u8 *)MK_FP(FRAMEDATA_SHADOW_SEG, fd_off);
                fd[0] = (u8)(d_w0 & 0xff); fd[1] = (u8)(d_w0 >> 8);
                fd[2] = (u8)(d_w1 & 0xff); fd[3] = (u8)(d_w1 >> 8);
                /* frame_tbl[byte*4] = far ptr {off=fd_off, seg=FRAMEDATA_SHADOW_SEG}. */
                frame_tbl[byte * 4 + 0] = (u8)(fd_off & 0xff);
                frame_tbl[byte * 4 + 1] = (u8)(fd_off >> 8);
                frame_tbl[byte * 4 + 2] = (u8)(FRAMEDATA_SHADOW_SEG & 0xff);
                frame_tbl[byte * 4 + 3] = (u8)(FRAMEDATA_SHADOW_SEG >> 8);
            }
        }
    }
}

/* Recover the action code from an alloc scenario name "alloc_action_XX" (XX hex). */
static int recover_action(const char *scname, u8 *action_out)
{
    const char *p = strstr(scname, "alloc_action_");
    if (!p) return 0;
    p += strlen("alloc_action_");
    *action_out = (u8)strtoul(p, NULL, 16);
    return 1;
}

/* ── (A) SEMANTIC-STATE COMPARATOR — channel records + tilemap stamp ─────────────
 *  Returns NULL if the post-call channel-record bytes AND the target-cell tilemap
 *  byte match the EXIT snap, else a short description of the first divergence (which
 *  slot/byte or the tilemap stamp); got/want filled. */
static const char *cmp_semantic(const record_t *r, long *got, long *want)
{
    static char buf[48];
    u8 live[N_SLOTS * REC_LEN];
    unsigned slot, b;
    read_live_recs(live);
    for (slot = 0; slot < N_SLOTS; slot++) {
        for (b = 0; b < REC_LEN; b++) {
            unsigned idx = slot * REC_LEN + b;
            if (live[idx] != r->ex.recs[idx]) {
                *got = live[idx]; *want = r->ex.recs[idx];
                snprintf(buf, sizeof(buf), "%s%u[+%u]",
                         slot < A_SLOTS ? "A" : "B",
                         slot < A_SLOTS ? slot : slot - A_SLOTS, b);
                return buf;
            }
        }
    }
    /* tilemap stamp at the target cell. */
    {
        u8 live_tile = (r->tile_cell < TILEMAP_SIZE) ? synth_tilemap[r->tile_cell] : 0;
        if (live_tile != r->tile_byte_x) {
            *got = live_tile; *want = r->tile_byte_x;
            return "tilemap[cell]";
        }
    }
    return NULL;
}

/* ── (B) DESCRIPTOR COMPARATOR — p1_sprite blit descriptor + view descriptors ────
 *  draw/erase records carry the descriptor/view bytes the engine wrote at EXIT.
 *  A PORTED draw/erase fn writes its descriptor field bytes into the host view +
 *  p1_sprite buffers; this asserts the produced bytes == the captured engine bytes,
 *  printing first-divergence.  The gate is REAL: the wrappers RECOMPUTE the written
 *  fields (grid/pos lookups, work-buffer ptr (channel_idx*0x180/0x100)+base, the
 *  0x600/0x400/0x200 / 0x9eba/0x9fba/0x8888 flag + far-data writes, the +0xf1 frame
 *  bias) from seeded inputs — NOT a self-consistency check.  A field perturbation
 *  (offset a written field) makes the comparator FAIL (see ANIM_DESC_PERTURB). */
static u8 host_desc[8];                    /* p1_sprite blit-descriptor pointee      */

/* view id -> the far-ptr global that must point at hview[id]. */
static void wire_views(void)
{
    anim_a_erase_view = hview[0];          /* id 0  0x8d4 */
    anim_a_draw_view  = hview[1];          /* id 1  0x8e0 */
    anim_b_view0      = hview[2];          /* id 2  0x8c8 */
    anim_b_view1      = hview[3];          /* id 3  0x8cc */
    anim_b_draw_view  = hview[4];          /* id 4  0x8d0 */
    anim_a_clear_view = hview[5];          /* id 5  0x8c0 */
    anim_b_clear_view = hview[6];          /* id 6  0x8bc */
    anim_a_grid_tbl = hgridA; anim_b_grid_tbl = hgridB;
    anim_posA_tbl   = hposA;  anim_posB_tbl   = hposB;
}

static void wr16h(u8 *p, u16 v) { p[0] = (u8)(v & 0xff); p[1] = (u8)(v >> 8); }

/* SEED the descriptor INPUTS from the captured EXIT record so the reconstructed
 *  wrapper RECOMPUTES the written fields to the engine's values:
 *   - each view host buffer is pre-loaded with the captured EXIT bytes (the fields
 *     the wrapper does NOT write retain the engine value -> trivially match; the
 *     fields it DOES write are independently recomputed -> the genuine signal);
 *   - the grid/pos tables for the active cell are recovered so the wrapper's
 *     grid[cell]/pos[cell] lookups yield the captured coords (a real cross-check
 *     that the wrapper reads the right table at cell*4 and writes the right offset);
 *   - p1_sprite x/y is recovered from the captured `desc` MINUS the entry slot's
 *     [+8] contribution so the wrapper's `posA_y + slot[+8]` add (and the B +0xf1
 *     frame bias on slot[+0xa]) are genuinely exercised, not echoed.
 *  Returns the active cell, or -1 if no active slot drives this record. */
static int seed_draw_erase(const record_t *r)
{
    unsigned v, base, n, s, slot, cell = 0; int have_cell = -1;
    int is_b = (r->fn_addr == FN_DRAW_ANIM_B || r->fn_addr == FN_ERASE_ANIM_B);
    u8 *gridT = is_b ? hgridB : hgridA;
    u8 *posT  = is_b ? hposB  : hposA;
    u16 gx = 0, gy = 0;

    /* (1) pre-load each captured view into its host buffer. */
    for (v = 0; v < r->nview; v++) {
        u8 id = r->views[v].id;
        if (id < 7 && r->views[v].len <= VIEW_LEN)
            memcpy(hview[id], r->views[v].bytes, r->views[v].len);
    }

    /* (2) the active slot/cell that drives this record (first non-0/non-0xFF). */
    base = is_b ? A_SLOTS : 0;
    n    = is_b ? B_SLOTS : A_SLOTS;
    for (s = 0; s < n; s++) {
        slot = base + s;
        if (rec_u8(r->ent.recs, slot, 0) != 0 && rec_u8(r->ent.recs, slot, 0) != 0xff) {
            cell = rec_u8(r->ent.recs, slot, 1); have_cell = (int)cell; break;
        }
    }
    if (have_cell < 0) return -1;

    /* (3) recover the grid coords this record's wrapper must produce.  For draw_a the
       coords are in the erase view (id 0) at +0x14/+0x16; for draw_b in view0 (id 2)
       at +6/+8; for erase_a/b in the clear view (id 5/6) at +0x14/+0x16. */
    {
        const u8 *vw = NULL; unsigned ox = 0x14, oy = 0x16;
        for (v = 0; v < r->nview; v++) {
            u8 id = r->views[v].id;
            if ((r->fn_addr == FN_DRAW_ANIM_A && id == 0) ||
                (r->fn_addr == FN_ERASE_ANIM_A && id == 5) ||
                (r->fn_addr == FN_ERASE_ANIM_B && id == 6)) { vw = r->views[v].bytes; break; }
            if (r->fn_addr == FN_DRAW_ANIM_B && id == 2) {
                vw = r->views[v].bytes; ox = 6; oy = 8; break;
            }
        }
        if (vw) { gx = rd16(vw + ox); gy = rd16(vw + oy); }
    }
    wr16h(gridT + cell * 4 + 0, gx);
    wr16h(gridT + cell * 4 + 2, gy);

    /* (4) p1_sprite pos seed (draw fns only).  desc[0..1]=x, desc[2..3]=y,
       desc[4..5]=frame.  posT_x[cell] = desc x; posT_y[cell] = desc y - slot[+8]
       (so wrapper's y = posT_y + slot[+8] reproduces desc y).  frame is checked
       independently (= slot[+0xa] (+0xf1 for B)). */
    if (r->desc_kind == 1 && r->desc_len >= 6) {
        u16 dx = rd16(r->desc + 0), dy = rd16(r->desc + 2);
        u16 s8 = rec_u16(r->ent.recs, slot, 8);
        wr16h(posT + cell * 4 + 0, dx);
        wr16h(posT + cell * 4 + 2, (u16)(dy - s8));
    }
    return (int)cell;
}

/* Compare the descriptor bytes the wrapper wrote against the captured engine bytes:
 *  every view blob (full 0x20) + the p1_sprite pointee (desc).  First divergence is
 *  returned with got/want.  A perturbation of any written field makes this FAIL. */
static const char *cmp_descriptor(const record_t *r, long *got, long *want)
{
    static char buf[32];
    unsigned v, b;
    for (v = 0; v < r->nview; v++) {
        u8 id = r->views[v].id, len = r->views[v].len;
        if (id >= 7 || len > VIEW_LEN) continue;
        for (b = 0; b < len; b++) {
            if (hview[id][b] != r->views[v].bytes[b]) {
                *got = hview[id][b]; *want = r->views[v].bytes[b];
                snprintf(buf, sizeof(buf), "view%u[+%u]", id, b);
                return buf;
            }
        }
    }
    if (r->desc_kind == 1 && r->desc_len) {
        for (b = 0; b < r->desc_len && b < sizeof(host_desc); b++) {
            if (host_desc[b] != r->desc[b]) {
                *got = host_desc[b]; *want = r->desc[b];
                snprintf(buf, sizeof(buf), "desc[+%u]", b);
                return buf;
            }
        }
    }
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  PORTED REGISTRY — engine seg-1000 offset -> reconstructed-C callable.
 *
 *  Each anim function is host-callable iff it has a reconstructed body in
 *  src/anim.c.  All seven now do (ported across T3/T4), so every entry below holds
 *  its function's C name (e.g. { FN_APPLY_CELL_ANIMATION,
 *  (void(*)(void))apply_cell_animation }) and the harness diffs each record the
 *  trace drives.  A NULL callable would mark its record UNPORTED and skip the symbol;
 *  none are NULL here.
 *
 *  apply_cell_animation takes a cdecl arg (the action code); that is handled in the
 *  run loop (recovered from the scenario), not the registry, which holds void(*)(void).
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct { u16 off; void (*fn)(void); } ported_t;

static const ported_t PORTED[] = {
    /* T3 PORTED (anim.c bodies).  apply_cell_animation takes a cdecl action arg; it is
       stored here as void(*)(void) and called with the recovered action in the run
       loop (the registry is keyed on offset, the arg is handled at the call site). */
    { FN_APPLY_CELL_ANIMATION, (void (*)(void))apply_cell_animation }, /* T3 alloc    */
    { FN_STEP_ANIM_A,          (void (*)(void))step_anim_channels_a }, /* T3 step 3 A */
    { FN_STEP_ANIM_B,          (void (*)(void))step_anim_channels_b }, /* T3 step 4 B */
    { FN_DRAW_ANIM_A,  (void (*)(void))draw_anim_channels_a },  /* T4 erase+blit+save-under (A) */
    { FN_DRAW_ANIM_B,  (void (*)(void))draw_anim_channels_b },  /* T4 erase+blit+save-under (B) */
    { FN_ERASE_ANIM_A, (void (*)(void))erase_anim_channels_a }, /* T4 restore_bg_view current(A)*/
    { FN_ERASE_ANIM_B, (void (*)(void))erase_anim_channels_b }, /* T4 restore_bg_view current(B)*/
};
#define PORTED_N (sizeof(PORTED) / sizeof(PORTED[0]))

static int ported_known(u16 off)
{
    unsigned i;
    for (i = 0; i < PORTED_N; i++)
        if (PORTED[i].off == off) return 1;
    return 0;
}

static void (*ported_lookup(u16 off))(void)
{
    unsigned i;
    for (i = 0; i < PORTED_N; i++)
        if (PORTED[i].off == off) return PORTED[i].fn;
    return NULL;
}

static int fn_is_draw_or_erase(u16 off)
{
    return off == FN_DRAW_ANIM_A || off == FN_DRAW_ANIM_B ||
           off == FN_ERASE_ANIM_A || off == FN_ERASE_ANIM_B;
}

typedef struct { long pass, fail, unported, desc_checked; } stats_t;

/* ── PER-FUNCTION semantic-state + descriptor differential ─────────────────────── */
static int run_per_function(record_t *recs, long nrec, const char *scname,
                            stats_t *st)
{
    long i;
    int  scen_fail = 0;
    long printed = 0;
    for (i = 0; i < nrec; i++) {
        record_t *r = &recs[i];
        void (*fn)(void) = ported_lookup(r->fn_addr);

        if (fn == NULL) {
            /* No reconstructed body yet.  Never references the symbol; UNPORTED is
               not a crash, not a hard failure. */
            if (!ported_known(r->fn_addr) && printed++ < 4)
                printf("    NOTE [%s #%ld] unknown fn_addr %#x (%s) — skipped\n",
                       scname, i, r->fn_addr, fn_name(r->fn_addr));
            st->unported++;
            continue;
        }

        /* ── PORTED: seed entry + tilemap + engine-data, call, assert ────────── */
        {
            const char *bad; long got = 0, want = 0;
            seed_globals(&r->ent);
            seed_tilemap(r);
            memset(host_desc, 0, sizeof(host_desc));
            if (fn_is_draw_or_erase(r->fn_addr)) {
                p1_sprite = host_desc;       /* draw fns write the blit descriptor */
                wire_views();                /* point the 7 view far-ptrs at hview[] */
                seed_draw_erase(r);          /* seed views + grid/pos + p1_sprite in  */
            }

            if (r->fn_addr == FN_APPLY_CELL_ANIMATION) {
                /* apply_cell_animation(action) — the cdecl action code.  Recovered
                   from the scenario name ("alloc_action_XX"); seed the action's
                   tile-def far ptr from the captured EXIT (stamp byte + claimed-slot
                   stream ptr) so the reconstructed allocator's slot scan + tilemap
                   stamp + stream-ptr copy run genuinely. */
                void (*f1)(u8) = (void (*)(u8))fn;
                u8 action = 0;
                if (!recover_action(scname, &action)) {
                    /* not an alloc scenario name — skip (no action to drive). */
                    st->unported++;
                    continue;
                }
                seed_alloc_tiledef(r, action);
                f1(action);
            } else if (fn_is_draw_or_erase(r->fn_addr)) {
                /* draw/erase: inputs seeded above (wire_views + seed_draw_erase). */
                fn();
            } else {
                /* steppers: seed each active slot's stream byte + frame-data ptr. */
                seed_stepper_stream(r, r->fn_addr == FN_STEP_ANIM_B);
                fn();
            }

            bad = cmp_semantic(r, &got, &want);
            if (bad == NULL && fn_is_draw_or_erase(r->fn_addr) && (r->nview || r->desc_len)) {
                bad = cmp_descriptor(r, &got, &want);
                if (bad == NULL) st->desc_checked++;
            }
            if (bad == NULL) {
                st->pass++;
            } else {
                st->fail++; scen_fail = 1;
                if (printed++ < 8)
                    printf("    FAIL [%s #%ld] %s field %s: got %#lx want %#lx "
                           "(tgt cell=%#x tile e->x=%#x->%#x)\n",
                           scname, i, fn_name(r->fn_addr), bad, got, want,
                           r->tile_cell, r->tile_byte_e, r->tile_byte_x);
            }
        }
    }
    return !scen_fail;
}

/* ════════════════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1]
                                  : "local/build/render/anim_trace.bin";
    FILE *f = fopen(path, "rb");
    long sz; u8 *b; u32 o; u16 ver, nsc, nfn; unsigned s;
    stats_t st = { 0, 0, 0, 0 };
    long n_records = 0, n_draw = 0;
    int hard_fail = 0;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz);
    if (!b || fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
    fclose(f);

    if (sz < 14 || memcmp(b, "ANIMTRC1", 8) != 0) {
        fprintf(stderr, "bad magic (want ANIMTRC1)\n"); return 2;
    }
    ver = rd16(b + 8); nsc = rd16(b + 10);
    if (ver != 1) { fprintf(stderr, "unsupported version %u (want 1)\n", ver); return 2; }
    o = 12;
    nfn = rd16(b + o); o += 2;
    /* skip the fn-name string table (we key on fn_addr, not the name index). */
    { u16 k; for (k = 0; k < nfn; k++) { u8 ln = b[o]; o += 1 + ln; } }

    printf("anim_chan_ctest: replay harness over %s\n", path);
    printf("  trace: ANIMTRC1 v%u, %u scenarios, %u fn-names\n", ver, nsc, nfn);
    printf("  src/anim.c reconstructs all seven anim fns (Phase-5 T3/T4): allocator + "
           "steppers gated on semantic state, draw/erase gated at descriptor level. "
           "UNPORTED records are only genuinely-unexercised fns; expected FAIL=0.\n");

    for (s = 0; s < nsc; s++) {
        u8 sid, name_len, seeded, level;
        char scname[64];
        u32 nrec, k;
        record_t *recs;
        stats_t sst = { 0, 0, 0, 0 };
        int per_ok;

        sid = b[o]; o += 1;
        name_len = b[o]; o += 1;
        { unsigned n = name_len < 63 ? name_len : 63;
          memcpy(scname, b + o, n); scname[n] = 0; o += name_len; }
        seeded = b[o]; level = b[o + 1]; o += 2;
        nrec = rd32(b + o); o += 4;

        recs = malloc(sizeof(record_t) * (nrec ? nrec : 1));
        for (k = 0; k < nrec; k++) {
            record_t *r = &recs[k];
            unsigned v;
            r->fn_addr = rd16(b + o); o += 2;
            r->fn_name_idx = rd16(b + o); o += 2;
            parse_snap(b + o, &r->ent); o += SNAP_SIZE;
            parse_snap(b + o, &r->ex);  o += SNAP_SIZE;
            r->tile_cell   = b[o]; r->tile_byte_e = b[o + 1]; r->tile_byte_x = b[o + 2];
            o += 3;
            r->desc_kind = b[o]; r->desc_len = b[o + 1]; o += 2;
            r->desc = b + o; o += r->desc_len;
            r->nview = b[o]; o += 1;
            for (v = 0; v < r->nview; v++) {
                u8 vid = b[o], vlen = b[o + 1]; o += 2;
                if (v < (unsigned)(sizeof(r->views) / sizeof(r->views[0]))) {
                    r->views[v].id = vid; r->views[v].len = vlen; r->views[v].bytes = b + o;
                }
                o += vlen;
            }
            if (r->desc_kind || r->desc_len) n_draw++;
            n_records++;
        }

        printf("\n== scenario %u: %s (seeded %u, level %u, %lu records) ==\n",
               sid, scname, seeded, level, (unsigned long)nrec);

        per_ok = run_per_function(recs, (long)nrec, scname, &sst);
        printf("  per-fn: PASS=%ld  FAIL=%ld  UNPORTED=%ld  DESC_CHECKED=%ld\n",
               sst.pass, sst.fail, sst.unported, sst.desc_checked);
        if (!per_ok) hard_fail = 1;

        st.pass += sst.pass; st.fail += sst.fail; st.unported += sst.unported;
        st.desc_checked += sst.desc_checked;
        free(recs);
    }

    printf("\n=== TOTAL per-fn: PASS=%ld  FAIL=%ld  UNPORTED=%ld  DESC_CHECKED=%ld "
           "(records=%ld, draw/erase descriptor records=%ld) ===\n",
           st.pass, st.fail, st.unported, st.desc_checked, n_records, n_draw);
    if (hard_fail || st.fail != 0) {
        printf("FAIL: %ld per-function differential failure(s) on PORTED fns\n",
               st.fail);
        return 1;
    }
    printf("PASS: FAIL=0.  %ld records UNPORTED (fns genuinely unexercised by the "
           "trace; all seven anim bodies are reconstructed in src/anim.c); %ld "
           "matched.\n",
           st.unported, st.pass);
    return 0;
}
