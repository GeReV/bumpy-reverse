/* Host REPLAY HARNESS for src/spawn.c — Phase-8 Task 2.
 *
 * Compiles the REAL reconstructed orchestrator (src/spawn.c) on the host (Open
 * Watcom 16-bit environment shimmed out: __far/__huge/__cdecl16near erased,
 * exact-width typedefs, BUMPY_H so spawn.h/anim.h do not pull <dos.h>), then
 * validates spawn_and_draw_level_entities (1000:2a78) against the Phase-8 T1
 * multi-level capture local/build/render/spawn_trace.bin (magic "SPWNTRC1",
 * version 2 — layout frozen in tools/spawn_oracle.py §"TRACE LAYOUT").
 *
 * ── WHAT THE ORCHESTRATOR DIRECTLY PRODUCES (validated here) ────────────────────
 *   (1) the channel-A/B RECORD POPULATION (the headline): per scanned A/B cell it
 *       writes the active slot-0 record's cell(+1) + frame-data far ptr (+8..+11),
 *       and at entry/exit it sets/clears the record active(+0) + cmd(+6) bytes;
 *   (2) the SPAWN GLOBALS read from the BUM header (p1_cell, level_exit_cell,
 *       items_remaining, p2_cell, p2_move_state, p2_ai_threshold, p2_frame_base);
 *   (3) the layer-C p1_sprite blit descriptor (x/y/frame) per C cell.
 *
 * ── THE NESTED-blit_sprite WRINKLE (carry-forward from T1) ──────────────────────
 *   The engine's draw_anim_channels_a/b each NEST a blit_sprite per active entity,
 *   so the T1 oracle's depth-gated blit_sprite hook captured BOTH the layer-C static
 *   blits AND the nested-A/B blits (all tagged layer=2, cell=0xFF — indistinguishable
 *   by cell).  We resolve this FAITHFULLY via option (b) of the brief: validate
 *   spawn's DIRECT output and separate the nested-A/B blits from spawn's own layer-C
 *   blits using the trace's per-fill LAYER tag + call STRUCTURE.  A layer-2 fill is a
 *   NESTED-A/B blit iff it immediately follows a layer-0 (draw_a) or layer-1 (draw_b)
 *   fill in the run's fill sequence; otherwise it is spawn's OWN layer-C blit.  This
 *   rule is exact for the engine's per-cell order (A-draw -> nested-A-blit ; B-draw ->
 *   nested-B-blit ; C -> direct-blit), and is asserted self-consistent against the
 *   tilemap layer-C non-zero count (the spawn-own-C fills MUST equal it).  The
 *   nested-A/B blit descriptors are draw_anim_channels_a/b's behavior (Phase-5
 *   descriptor-validated), NOT re-validated here.
 *
 *   The host stubs for draw_anim_channels_a/b do NOT nest a blit (they only emit
 *   their own layer-0/1 fill), so the host fill sequence = spawn's DIRECT output:
 *   one layer-0 fill per A cell, one layer-1 per B cell, one layer-2 per C cell.
 *   We compare it against the trace's DIRECT subset (all layer-0, all layer-1, and
 *   the spawn-own layer-2 fills per the rule above), in order.
 *
 * ── COMPARATORS ─────────────────────────────────────────────────────────────────
 *   (A) SEMANTIC-STATE DIFFERENTIAL.  Seed the run's ENTRY snapshot (the channel
 *       records + spawn globals + tilemap/header/tables from the level SEED block),
 *       call spawn_and_draw_level_entities(), then assert the 7 channel records (3 A
 *       + 4 B, 12 bytes each) AND the spawn globals == the run's EXIT snapshot.
 *   (B) DESCRIPTOR-LEVEL DIFFERENTIAL.  Assert the host's captured per-cell fills
 *       (layer-0 A record / layer-1 B record / layer-2 spawn-own-C blit descriptor),
 *       in order, == the trace's DIRECT-subset fills (same layer, cell, row/col, and
 *       the 12-byte record / 8-byte descriptor bytes).
 *
 * Build/run (also wrapped by tools/validate_spawn.sh):
 *     cc -O2 -Wall -Werror -o /tmp/spawn_ctest tools/spawn_ctest.c && \
 *       /tmp/spawn_ctest local/build/render/spawn_trace.bin
 * Exit 0 iff the harness parses the trace, runs, and BOTH differentials have ZERO
 * failures across every captured level (incl. the B-firing levels 2/3/4/5/6/8/9).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── shim the Watcom 16-bit environment for host compilation ─────────────────── */
#define BUMPY_H            /* spawn.h/anim.h's #include "bumpy.h" becomes a no-op */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge
#define __cdecl16near

/* ── MK_FP / FP_OFF / FP_SEG host model (mirrors anim_chan_ctest.c) ──────────────
 *  spawn.c rebuilds a far pointer with MK_FP(seg, off) for the A/B frame-data
 *  descriptor and for the layer-C pos table, and reads FP_OFF/FP_SEG(p1_sprite).
 *  Back MK_FP with a >256 KB linear "far memory" shadow so reconstructed pointers
 *  land in real host storage the harness can pre-seed; FP_OFF/FP_SEG invert it. */
#define FAR_MEM_SIZE 0x80000UL
static unsigned char far_mem[FAR_MEM_SIZE];
#define MK_FP(seg, off) ((void *)(far_mem + (((u32)(seg) << 4) + (u16)(off))))
static u16 host_fp_seg(const void *p) { return (u16)(((u32)((const unsigned char *)p - far_mem)) >> 4); }
static u16 host_fp_off(const void *p) { return (u16)(((u32)((const unsigned char *)p - far_mem)) & 0xF); }
#define FP_SEG(p) host_fp_seg((const void *)(p))
#define FP_OFF(p) host_fp_off((const void *)(p))

/* ── the anim_chan_rec type comes from anim.h (pulled by spawn.h via spawn.c).
 *    Provide the cross-module + anim-owned globals spawn.c references as host
 *    definitions (we do NOT include anim.c — its draw/erase bodies are STUBBED here
 *    so the host fill sequence is spawn's DIRECT output; the nested-blit behavior is
 *    Phase-5 territory, separated out of the trace per the header). ─────────────── */

/* Anim-owned globals (anim.c 0x4c70/0x4cbc slot tables, 0x3d6a/0x40a6 frame tbls,
   0x8578/0x8579 cmd bytes, 0x8884 p1_sprite, tilemap).  We define them here and
   wire the slot tables to real record storage so spawn's record writes land where
   the comparator reads. */
#include "../src/anim.h"   /* anim_chan_rec, ANIM_A_SLOTS, ANIM_B_SLOTS, externs */

/* Record storage the slot tables point at (3 A + 4 B). */
static anim_chan_rec host_a_recs[ANIM_A_SLOTS];
static anim_chan_rec host_b_recs[ANIM_B_SLOTS];
anim_chan_rec *anim_channels_a_tbl[ANIM_A_SLOTS + 1];
anim_chan_rec *anim_channels_b_tbl[ANIM_B_SLOTS + 1];  /* 4 slots + 0xFF terminator (anim.h) */

/* anim frame-data far-ptr tables (off@N*4+0, seg@N*4+2). */
u8 anim_a_frame_tbl[256 * 4];
u8 anim_b_frame_tbl[256 * 4];
u8 g_anim_cur_cmd_byte;
u8 anim_b_cur_frame_byte;

/* p1_sprite blit-descriptor far ptr (0x8884 -> the 0x792e 8-byte pointee). */
static unsigned char *p1_sprite_buf;   /* points into far_mem so FP_OFF/SEG invert */
u8 __far *p1_sprite;

/* tilemap (game.c 0xa0d8) — base tilemap layer; spawn reads layers A/B/C + header. */
#define TILEMAP_SIZE 0x200
static u8 synth_tilemap[TILEMAP_SIZE];
u8 __far *tilemap = synth_tilemap;

/* cross-module spawn globals (owned elsewhere; spawn.c reads/writes them). */
u8 __far *level_src_ptr;
u8  p1_cell;
u8  level_exit_cell;
u8  items_remaining;
s8  p2_cell;
u8  p2_ai_threshold;
u8  p2_move_state;
u16 p2_frame_base;
u8 __far *p2_cell_coord_tbl;

/* ── per-run fill capture (filled by the leaf stubs, in call order) ─────────────── */
#define MAX_FILLS 256
typedef struct { u8 layer; u8 cell; u8 desc_len; u8 desc[12]; } hfill_t;
static hfill_t hfills[MAX_FILLS];
static int     n_hfills;

/* Read the active slot-0 record (12 bytes) for the given channel into out. */
static void rec_bytes(const anim_chan_rec *r, u8 *out)
{
    out[0]  = r->active; out[1] = r->cell;
    out[2]  = (u8)(r->stream_off & 0xFF); out[3] = (u8)(r->stream_off >> 8);
    out[4]  = (u8)(r->stream_seg & 0xFF); out[5] = (u8)(r->stream_seg >> 8);
    out[6]  = r->frame; out[7] = r->pad;
    out[8]  = (u8)(r->data_off & 0xFF); out[9]  = (u8)(r->data_off >> 8);
    out[10] = (u8)(r->data_seg & 0xFF); out[11] = (u8)(r->data_seg >> 8);
}

/* ── leaf stubs spawn.c calls.  draw_a/b emit the active slot-0 record as a layer-0/1
 *    fill (spawn populated it just before the call).  anim_blit_sprite_leaf emits the
 *    8-byte p1_sprite descriptor as a layer-2 fill.  erase/setup are no-ops.  None
 *    NESTS a blit — so the host fills are spawn's DIRECT output only. ────────────── */
void draw_anim_channels_a(void)
{
    if (n_hfills < MAX_FILLS) {
        hfill_t *f = &hfills[n_hfills++];
        f->layer = 0; f->cell = anim_channels_a_tbl[0]->cell; f->desc_len = 12;
        rec_bytes(anim_channels_a_tbl[0], f->desc);
    }
}
void draw_anim_channels_b(void)
{
    if (n_hfills < MAX_FILLS) {
        hfill_t *f = &hfills[n_hfills++];
        f->layer = 1; f->cell = anim_channels_b_tbl[0]->cell; f->desc_len = 12;
        rec_bytes(anim_channels_b_tbl[0], f->desc);
    }
}
void erase_anim_channels_a(void) {}
void erase_anim_channels_b(void) {}
void setup_fullscreen_view(void) {}
void anim_blit_sprite_leaf(u16 obj_off, u16 obj_seg)
{
    (void)obj_off; (void)obj_seg;
    if (n_hfills < MAX_FILLS) {
        hfill_t *f = &hfills[n_hfills++];
        f->layer = 2; f->cell = 0xFF; f->desc_len = 8;
        memcpy(f->desc, p1_sprite, 8);   /* the x/y/frame descriptor spawn just wrote */
    }
}

#include "../src/spawn.c"

/* ════════════════════════════════════════════════════════════════════════════
 *  Trace format (frozen — tools/spawn_oracle.py §"TRACE LAYOUT", v2 multi-level).
 * ════════════════════════════════════════════════════════════════════════════ */
#define A_SLOTS 3
#define B_SLOTS 4
#define N_SLOTS (A_SLOTS + B_SLOTS)         /* 7  */
#define REC_LEN 12
/* SPAWNSNAP "<BB" + "B"*(7*12) + "BBBbBB" + "H" + "B" + "HHHH" = 2+84+6+2+1+8 = 103 */
#define SNAP_SIZE (2 + N_SLOTS * REC_LEN + 6 + 2 + 1 + 8)

typedef struct {
    u8  recs[N_SLOTS * REC_LEN];            /* 3 A then 4 B, 12 raw bytes each       */
    u8  p1_cell, level_exit_cell, items_remaining;
    s8  p2_cell;
    u8  p2_move_state, p2_ai_rng_threshold;
    u16 p2_frame_base;
    u8  current_level;
    u16 tilemap_off, tilemap_seg, level_src_off, level_src_seg;
} snap_t;

typedef struct { u8 layer, cell, row, col, desc_len; const u8 *desc; } fill_t;
typedef struct { snap_t ent, ex; int n_fills; fill_t fills[MAX_FILLS]; u8 seeded; } run_t;

typedef struct {
    u8  level;
    const u8 *seed; u32 seed_len;
    int n_runs; run_t runs[4];
} level_t;

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

static void parse_snap(const u8 *p, snap_t *s)
{
    u32 o = 2;                              /* after a_slots,b_slots count bytes     */
    memcpy(s->recs, p + o, N_SLOTS * REC_LEN); o += N_SLOTS * REC_LEN;
    s->p1_cell             = p[o + 0];
    s->level_exit_cell     = p[o + 1];
    s->items_remaining     = p[o + 2];
    s->p2_cell             = (s8)p[o + 3];
    s->p2_move_state       = p[o + 4];
    s->p2_ai_rng_threshold = p[o + 5];
    o += 6;
    s->p2_frame_base = rd16(p + o); o += 2;
    s->current_level = p[o]; o += 1;
    s->tilemap_off   = rd16(p + o); s->tilemap_seg   = rd16(p + o + 2);
    s->level_src_off = rd16(p + o + 4); s->level_src_seg = rd16(p + o + 6);
}

/* ── SEED block layout (tools/spawn_oracle.py build_seed) — offsets within seed ──── */
#define SEED_OFF_PTRS    0
#define SEED_OFF_HDR     8           /* 8 ptr bytes, then 0xC2 BUM header            */
#define SEED_OFF_LAYERA  (SEED_OFF_HDR + 0xC2)
#define SEED_OFF_LAYERB  (SEED_OFF_LAYERA + 0x30)
#define SEED_OFF_LAYERC  (SEED_OFF_LAYERB + 0x30)
#define SEED_OFF_ATYPE   (SEED_OFF_LAYERC + 0x30)
#define SEED_OFF_AFOFF   (SEED_OFF_ATYPE + 0x100)
#define SEED_OFF_AFSEG   (SEED_OFF_AFOFF + 0x400)
#define SEED_OFF_BTYPE   (SEED_OFF_AFSEG + 0x400)
#define SEED_OFF_BFOFF   (SEED_OFF_BTYPE + 0x100)
#define SEED_OFF_BFSEG   (SEED_OFF_BFOFF + 0x400)
#define SEED_OFF_POSX    (SEED_OFF_BFSEG + 0x400)
#define SEED_OFF_POSY    (SEED_OFF_POSX + 0x180)
#define SEED_OFF_P2FB    (SEED_OFF_POSY + 0x180)
#define SEED_TOTAL       (SEED_OFF_P2FB + 0x40)

/* ── host buffers backing the far/near tables the orchestrator reads ────────────── */
static u8 host_bum_header[0xC2];
static unsigned char *posc_buf;    /* layer-C X/Y coord tbl (0x274), in far_mem      */

/* ── THE DESCRIPTOR-TARGET SEED GAP (documented carry-forward of the T1 capture) ──
 *  spawn populates record[+8/+10] by DEREFERENCING the far ptr it builds from the
 *  A/B frame tables: descfar = MK_FP(anim_X_frame_tbl seg/off at type*4); record
 *  data_off/data_seg = descfar[0]/descfar[1].  The T1 oracle captured the frame-table
 *  POINTERS (seeded above) but NOT the 2-word DESCRIPTOR they point at (it lives in
 *  the level's decoded frame-data buffer, e.g. seg 0x37be — outside the captured
 *  tables).  To exercise spawn's deref with the REAL computed address, we seed the
 *  descriptor target IN far_mem at exactly MK_FP(fseg, foff) — the address spawn
 *  independently computes from the seed tables — with the 2 words the engine read
 *  (recovered from the trace's per-cell record bytes +8/+10).  This validates the
 *  full chain spawn OWNS: pick the right type-table entry -> build the right far ptr
 *  -> deref it -> store the words at +8/+10 in the right order, only for the right
 *  cells (A vs B).  The descriptor CONTENT is level input, not spawn logic; it is the
 *  one narrow value borrowed from the capture (the T1 seed lacks the target). */
static void seed_one_descriptor(const u8 *frame_tbl, u8 type, const u8 *rec)
{
    u16 foff = (u16)(frame_tbl[type*4+0] | (frame_tbl[type*4+1] << 8));
    u16 fseg = (u16)(frame_tbl[type*4+2] | (frame_tbl[type*4+3] << 8));
    unsigned char *tgt = (unsigned char *)MK_FP(fseg, foff);
    tgt[0] = rec[8]; tgt[1] = rec[9];     /* descriptor[+0] = record data_off (+8)   */
    tgt[2] = rec[10]; tgt[3] = rec[11];   /* descriptor[+2] = record data_seg (+10)  */
}

/* Seed all host state from a level SEED block + a run's ENTRY snapshot, then return
   the layer-C non-zero count (for the spawn-own-C self-consistency assertion). */
static int seed_state(const u8 *seed, const snap_t *ent)
{
    int i, cnz;

    /* tilemap: header bytes 0x90.. live in the BUM header copy; layers A/B/C from the
       seed.  spawn reads tilemap[+0x00/+0x30/+0x60] and header[+0x90..+0x92] via the
       SAME tilemap far ptr (engine: load_current_level_data copies the 0xC2 slice
       into tilemap, so header+layers are contiguous).  Lay them out the same way. */
    memset(synth_tilemap, 0, TILEMAP_SIZE);
    memcpy(synth_tilemap + 0x00, seed + SEED_OFF_LAYERA, 0x30);
    memcpy(synth_tilemap + 0x30, seed + SEED_OFF_LAYERB, 0x30);
    memcpy(synth_tilemap + 0x60, seed + SEED_OFF_LAYERC, 0x30);
    memcpy(host_bum_header, seed + SEED_OFF_HDR, 0xC2);
    memcpy(synth_tilemap + 0x90, host_bum_header + 0x90, 0xC2 - 0x90); /* header tail */
    tilemap = synth_tilemap;

    /* level_src_ptr: spawn reads header[+0x93..+0x96] via it; back it with the full
       BUM header so those reads are correct. */
    level_src_ptr = host_bum_header;

    /* spawn type tables (0x3d3a / 0x4086). */
    memcpy(spawn_a_type_tbl, seed + SEED_OFF_ATYPE, 0x100);
    memcpy(spawn_b_type_tbl, seed + SEED_OFF_BTYPE, 0x100);

    /* anim frame-data far-ptr tables (0x3d6a/0x3d6c, 0x40a6/0x40a8): the seed stores
       off and seg halves SEPARATELY (0x400 each); the engine interleaves them as
       off@N*4+0 / seg@N*4+2.  Re-interleave into anim_a/b_frame_tbl. */
    for (i = 0; i < 256; i++) {
        u16 ao = rd16(seed + SEED_OFF_AFOFF + i * 2);
        u16 as = rd16(seed + SEED_OFF_AFSEG + i * 2);
        u16 bo = rd16(seed + SEED_OFF_BFOFF + i * 2);
        u16 bs = rd16(seed + SEED_OFF_BFSEG + i * 2);
        anim_a_frame_tbl[i*4+0] = (u8)ao; anim_a_frame_tbl[i*4+1] = (u8)(ao>>8);
        anim_a_frame_tbl[i*4+2] = (u8)as; anim_a_frame_tbl[i*4+3] = (u8)(as>>8);
        anim_b_frame_tbl[i*4+0] = (u8)bo; anim_b_frame_tbl[i*4+1] = (u8)(bo>>8);
        anim_b_frame_tbl[i*4+2] = (u8)bs; anim_b_frame_tbl[i*4+3] = (u8)(bs>>8);
    }

    /* p2_frame_base lookup (0x2546, 0x20 u16 entries). */
    for (i = 0; i < (int)SPAWN_P2_FRAME_TBL_LEN; i++) {
        spawn_p2_frame_tbl[i] = rd16(seed + SEED_OFF_P2FB + i * 2);
    }

    /* layer-C pos table (0x274): a FLAT u16 table.  The seed's POSX (0x180 B @0x274)
       and POSY (0x180 B @0x276) are the SAME table sampled 2 bytes apart (the oracle
       notes "0x276; overlapping +2"), so POSX already holds X and Y interleaved for
       adjacent cells.  Engine: X = tbl[0x274 + idx], Y = tbl[0x274 + idx + 2] with
       idx = (col*2+row*16)*2.  Copy POSX verbatim (+2 tail word from POSY's last) and
       point p2_cell_coord_tbl (0x274) at it. */
    memcpy(posc_buf, seed + SEED_OFF_POSX, 0x180);
    posc_buf[0x180] = seed[SEED_OFF_POSY + 0x17e];      /* the +2 tail word (Y of last) */
    posc_buf[0x181] = seed[SEED_OFF_POSY + 0x17f];
    p2_cell_coord_tbl = (u8 __far *)posc_buf;

    /* channel records (3 A + 4 B) from the ENTRY snapshot raw bytes. */
    for (i = 0; i < A_SLOTS; i++) {
        const u8 *r = ent->recs + i * REC_LEN;
        host_a_recs[i].active = r[0]; host_a_recs[i].cell = r[1];
        host_a_recs[i].stream_off = rd16(r+2); host_a_recs[i].stream_seg = rd16(r+4);
        host_a_recs[i].frame = r[6]; host_a_recs[i].pad = r[7];
        host_a_recs[i].data_off = rd16(r+8); host_a_recs[i].data_seg = rd16(r+10);
        anim_channels_a_tbl[i] = &host_a_recs[i];
    }
    anim_channels_a_tbl[A_SLOTS] = &host_a_recs[0];  /* terminator slot (unused here) */
    for (i = 0; i < B_SLOTS; i++) {
        const u8 *r = ent->recs + (A_SLOTS + i) * REC_LEN;
        host_b_recs[i].active = r[0]; host_b_recs[i].cell = r[1];
        host_b_recs[i].stream_off = rd16(r+2); host_b_recs[i].stream_seg = rd16(r+4);
        host_b_recs[i].frame = r[6]; host_b_recs[i].pad = r[7];
        host_b_recs[i].data_off = rd16(r+8); host_b_recs[i].data_seg = rd16(r+10);
        anim_channels_b_tbl[i] = &host_b_recs[i];
    }
    {   /* B scan terminator slot (engine 0x4cb0, active=0xFF); unused here (draw/erase
           B are stubbed in this harness) but wired for type/layout consistency with
           src/anim.c's B_SLOTS+1 table. */
        static anim_chan_rec b_term;
        b_term.active = 0xff;
        anim_channels_b_tbl[B_SLOTS] = &b_term;
    }

    /* spawn globals from the ENTRY snapshot (they are overwritten by spawn, but seed
       them so any guard reads start from the captured state). */
    p1_cell = ent->p1_cell; level_exit_cell = ent->level_exit_cell;
    items_remaining = ent->items_remaining; p2_cell = ent->p2_cell;
    p2_move_state = ent->p2_move_state; p2_ai_threshold = ent->p2_ai_rng_threshold;
    p2_frame_base = ent->p2_frame_base;

    /* p1_sprite descriptor pointee (8 bytes), backed in far_mem. */
    memset(p1_sprite_buf, 0, 8);
    p1_sprite = (u8 __far *)p1_sprite_buf;

    cnz = 0;
    for (i = 0; i < 0x30; i++) { if (synth_tilemap[0x60 + i] != 0) cnz++; }
    return cnz;
}

/* ── comparators ────────────────────────────────────────────────────────────────*/
static int fails;

static void cmp_recs(const char *tag, int lvl, const snap_t *ex)
{
    u8 got[N_SLOTS * REC_LEN];
    int i;
    for (i = 0; i < A_SLOTS; i++) rec_bytes(&host_a_recs[i], got + i * REC_LEN);
    for (i = 0; i < B_SLOTS; i++) rec_bytes(&host_b_recs[i], got + (A_SLOTS + i) * REC_LEN);
    for (i = 0; i < N_SLOTS * REC_LEN; i++) {
        if (got[i] != ex->recs[i]) {
            printf("  FAIL %s L%d: record byte %d (slot %d off %d) host=%02x trace=%02x\n",
                   tag, lvl, i, i / REC_LEN, i % REC_LEN, got[i], ex->recs[i]);
            fails++;
            return;   /* first divergence per run is enough */
        }
    }
}

static void cmp_globals(const char *tag, int lvl, const snap_t *ex)
{
    int bad = 0;
#define CK(name, hostv, exv) do { if ((u16)(hostv) != (u16)(exv)) { \
        printf("  FAIL %s L%d: %s host=%04x trace=%04x\n", tag, lvl, name, \
               (u16)(hostv), (u16)(exv)); bad = 1; } } while (0)
    CK("p1_cell", p1_cell, ex->p1_cell);
    CK("level_exit_cell", level_exit_cell, ex->level_exit_cell);
    CK("items_remaining", items_remaining, ex->items_remaining);
    CK("p2_cell", (u8)(s8)p2_cell, (u8)ex->p2_cell);
    CK("p2_move_state", p2_move_state, ex->p2_move_state);
    CK("p2_ai_threshold", p2_ai_threshold, ex->p2_ai_rng_threshold);
    CK("p2_frame_base", p2_frame_base, ex->p2_frame_base);
#undef CK
    if (bad) fails++;
}

/* Descriptor differential: host fills (spawn's DIRECT output) vs the trace's DIRECT
   subset.  A trace layer-2 fill is spawn-own-C iff the preceding trace fill is NOT a
   layer-0/1 draw (i.e. it is the first fill, or the previous fill is also layer-2). */
static int trace_fill_is_direct(const run_t *run, int idx)
{
    if (run->fills[idx].layer != 2) return 1;             /* all A/B draws are direct */
    if (idx == 0) return 1;                               /* leading C blit is direct */
    return run->fills[idx - 1].layer == 2;                /* nested iff prev was A/B   */
}

static void cmp_fills(const char *tag, int lvl, const run_t *run, int c_nz)
{
    int ti = 0, hi = 0, spawn_own_c = 0;
    /* count spawn-own-C in the trace for the self-consistency check */
    for (ti = 0; ti < run->n_fills; ti++) {
        if (run->fills[ti].layer == 2 && trace_fill_is_direct(run, ti)) spawn_own_c++;
    }
    if (spawn_own_c != c_nz) {
        printf("  FAIL %s L%d: spawn-own-C count %d != tilemap layer-C nz %d\n",
               tag, lvl, spawn_own_c, c_nz);
        fails++;
    }
    ti = 0;
    while (ti < run->n_fills || hi < n_hfills) {
        /* advance the trace cursor past nested-A/B blits (not spawn's direct output) */
        while (ti < run->n_fills && !trace_fill_is_direct(run, ti)) ti++;
        if (ti >= run->n_fills && hi >= n_hfills) break;
        if (ti >= run->n_fills || hi >= n_hfills) {
            printf("  FAIL %s L%d: fill count mismatch (host=%d trace-direct exhausted=%d)\n",
                   tag, lvl, n_hfills, hi);
            fails++;
            return;
        }
        {
            const fill_t *t = &run->fills[ti];
            const hfill_t *h = &hfills[hi];
            int j, mism = 0;
            if (t->layer != h->layer) mism = 1;
            else if (t->layer != 2 && t->cell != h->cell) mism = 1;
            else if (t->desc_len != h->desc_len) mism = 1;
            else for (j = 0; j < t->desc_len; j++) if (t->desc[j] != h->desc[j]) { mism = 1; break; }
            if (mism) {
                printf("  FAIL %s L%d: fill[h=%d,t=%d] layer h%d/t%d cell h%02x/t%02x desc:\n",
                       tag, lvl, hi, ti, h->layer, t->layer, h->cell, t->cell);
                printf("        host="); for (j=0;j<h->desc_len;j++) printf("%02x", h->desc[j]);
                printf("\n        trace="); for (j=0;j<t->desc_len;j++) printf("%02x", t->desc[j]);
                printf("\n");
                fails++;
                return;
            }
        }
        ti++; hi++;
    }
}

/* PERTURBATION: corrupt the seeded layer-A tilemap byte of the first fired A cell.
   That MUST change spawn's record-A population (cell/data ptr) AND the fill sequence,
   so the comparator MUST report FAIL!=0.  Proves the gate has teeth. */
static int g_perturb;

int main(int argc, char **argv)
{
    const char *path = "local/build/render/spawn_trace.bin";
    FILE *fp; long sz; u8 *buf; u32 o;
    u16 ver, n_levels, n_names;
    int li, runs_total = 0, levels_b = 0, ai;

    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--perturb") == 0) g_perturb = 1;
        else path = argv[ai];
    }
    fp = fopen(path, "rb");

    /* back p1_sprite + posc + p2_cell_coord_tbl in far_mem so FP_OFF/SEG invert. */
    p1_sprite_buf = far_mem + 0x10000;
    posc_buf      = far_mem + 0x20000;

    if (!fp) { fprintf(stderr, "ERROR: cannot open %s\n", path); return 2; }
    fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    buf = malloc(sz);
    if (fread(buf, 1, sz, fp) != (size_t)sz) { fprintf(stderr, "ERROR: read\n"); return 2; }
    fclose(fp);

    if (memcmp(buf, "SPWNTRC1", 8) != 0) { fprintf(stderr, "ERROR: bad magic\n"); return 2; }
    o = 8;
    ver = rd16(buf + o); n_levels = rd16(buf + o + 2); o += 4;
    n_names = rd16(buf + o); o += 2;
    { int k; for (k = 0; k < n_names; k++) { u8 ln = buf[o++]; o += ln; } }
    printf("== spawn replay: trace v%u, %u levels ==\n", ver, n_levels);

    for (li = 0; li < n_levels; li++) {
        level_t L;
        int ri;
        L.level = buf[o++];
        L.seed_len = rd16(buf + o); o += 2;
        L.seed = buf + o; o += L.seed_len;
        L.n_runs = rd16(buf + o); o += 2;
        for (ri = 0; ri < L.n_runs && ri < 4; ri++) {
            run_t *R = &L.runs[ri];
            u8 nl; int fi;
            o += 1;                          /* run_id     */
            nl = buf[o++]; o += nl;          /* name       */
            R->seeded = buf[o++];
            parse_snap(buf + o, &R->ent); o += SNAP_SIZE;
            parse_snap(buf + o, &R->ex);  o += SNAP_SIZE;
            R->n_fills = rd16(buf + o); o += 2;
            for (fi = 0; fi < R->n_fills && fi < MAX_FILLS; fi++) {
                fill_t *F = &R->fills[fi];
                F->layer = buf[o++]; F->cell = buf[o++]; F->row = buf[o++]; F->col = buf[o++];
                F->desc_len = buf[o++]; F->desc = buf + o; o += F->desc_len;
            }
        }

        /* run the orchestrator per run, compare. */
        for (ri = 0; ri < L.n_runs && ri < 4; ri++) {
            run_t *R = &L.runs[ri];
            int c_nz;
            int b_in_run = 0, fk;
            for (fk = 0; fk < R->n_fills; fk++) if (R->fills[fk].layer == 1) b_in_run = 1;
            if (b_in_run) levels_b++;
            c_nz = seed_state(L.seed, &R->ent);
            /* Seed the descriptor TARGETS spawn will deref, from the run's captured
               A/B fills (the documented T1 seed-gap carry-forward; see
               seed_one_descriptor).  Each layer-0/1 fill's 12-byte record carries the
               cell (+1) -> recompute its type from the seeded type tables -> place the
               record's +8/+10 words at the spawn-computed far address. */
            {
                int fk2;
                for (fk2 = 0; fk2 < R->n_fills; fk2++) {
                    const fill_t *F = &R->fills[fk2];
                    u8 cell, cv2, type2;
                    if (F->layer == 0 && F->desc_len == 12) {
                        cell = F->desc[1];
                        cv2  = synth_tilemap[cell];
                        type2 = spawn_a_type_tbl[cv2];
                        seed_one_descriptor(anim_a_frame_tbl, type2, F->desc);
                    } else if (F->layer == 1 && F->desc_len == 12) {
                        cell = F->desc[1];
                        cv2  = synth_tilemap[0x30 + cell];
                        type2 = spawn_b_type_tbl[cv2];
                        seed_one_descriptor(anim_b_frame_tbl, type2, F->desc);
                    }
                }
            }
            /* perturbation: flip the seeded layer-A byte of the first fired A cell to
               a different nonzero type so its record-A data ptr + fill diverge. */
            if (g_perturb) {
                int fk3;
                for (fk3 = 0; fk3 < R->n_fills; fk3++) {
                    if (R->fills[fk3].layer == 0 && R->fills[fk3].desc_len == 12) {
                        u8 cell = R->fills[fk3].desc[1];
                        synth_tilemap[cell] = (u8)(synth_tilemap[cell] ^ 0x55) | 1;
                        break;
                    }
                }
            }
            n_hfills = 0;
            spawn_and_draw_level_entities();
            cmp_globals("globals", L.level, &R->ex);
            cmp_recs("records", L.level, &R->ex);
            cmp_fills("fills", L.level, R, c_nz);
            runs_total++;
        }
        printf("  level %u: %d run(s) replayed\n", L.level, L.n_runs);
    }

    printf("== SPAWN VALIDATION: %d run(s), %d with layer-B, FAIL=%d ==\n",
           runs_total, levels_b, fails);
    free(buf);
    return fails ? 1 : 0;
}
