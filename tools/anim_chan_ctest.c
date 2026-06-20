/* Host REPLAY HARNESS for src/anim.c — Phase-5 Task 2.
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
 * As of Phase-5 T2, src/anim.c is the GLOBALS-ONLY skeleton (no fn bodies): every
 * PORTED[] entry below is NULL, so every record is reported UNPORTED and the
 * comparators do not run.  Phase-5 T3 fills in apply_cell_animation + the two
 * steppers (semantic-state gate); T4 fills in the two draw + two erase fns
 * (descriptor gate); each then swaps NULL for its C name in PORTED[] and its
 * UNPORTED count converts to a per-function diff with no other harness change.
 *
 * ── COMPARATORS (present now; exercised only on PORTED records — none this task) ─
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
 *   GRACEFUL UNPORTED DEGRADATION.  A function with no reconstructed C body yet (ALL
 *     seven this task) is marked UNPORTED and SKIPPED: it is NEVER referenced as a
 *     symbol (its PORTED entry holds a NULL callable), so the harness never
 *     link-depends on the missing body and never call-throughs into it.  UNPORTED is
 *     NOT a crash and NOT a hard failure.
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

#include "../src/anim.c"

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
 *  When a draw/erase fn is PORTED and writes a host-visible descriptor (T4), this
 *  compares the produced bytes against the captured ones.  For the skeleton (no fn
 *  PORTED) it never runs; it is wired so T4 lands without harness edits. */
static u8 host_desc[8];                    /* p1_sprite blit descriptor target      */
static const char *cmp_descriptor(const record_t *r, long *got, long *want)
{
    unsigned b;
    if (r->desc_kind == 0 || r->desc_len == 0) return NULL;
    for (b = 0; b < r->desc_len && b < sizeof(host_desc); b++) {
        if (host_desc[b] != r->desc[b]) {
            *got = host_desc[b]; *want = r->desc[b];
            return "desc";
        }
    }
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  PORTED REGISTRY — engine seg-1000 offset -> reconstructed-C callable.
 *
 *  Each anim function is host-callable iff it has a reconstructed body in
 *  src/anim.c.  This task (the GLOBALS-ONLY skeleton) ports NONE, so every entry's
 *  callable is NULL -> the harness marks the record UNPORTED and never references
 *  the (absent) symbol.  Phase-5 T3 fills in a body in anim.c AND swaps NULL for
 *  the function's C name here (e.g. { FN_APPLY_CELL_ANIMATION, NULL } becomes
 *  { FN_APPLY_CELL_ANIMATION, (void(*)(void))apply_cell_animation }); that record's
 *  UNPORTED count then converts to a per-function diff with no other harness change.
 *
 *  apply_cell_animation takes a cdecl arg (the action code); that is handled in the
 *  run loop (recovered from the scenario), not the registry, which holds void(*)(void).
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct { u16 off; void (*fn)(void); } ported_t;

static const ported_t PORTED[] = {
    { FN_APPLY_CELL_ANIMATION, NULL },   /* T3 — channel-A allocator                */
    { FN_STEP_ANIM_A,          NULL },   /* T3 — advance 3 A channels               */
    { FN_STEP_ANIM_B,          NULL },   /* T3 — advance 4 B channels               */
    { FN_DRAW_ANIM_A,          NULL },   /* T4 — erase+blit+save-under (A)          */
    { FN_DRAW_ANIM_B,          NULL },   /* T4 — erase+blit+save-under (B)          */
    { FN_ERASE_ANIM_A,         NULL },   /* T4 — restore_bg_view current (A)        */
    { FN_ERASE_ANIM_B,         NULL },   /* T4 — restore_bg_view current (B)        */
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

        /* ── PORTED: seed entry + tilemap, call, assert ──────────────────────── */
        {
            const char *bad; long got = 0, want = 0;
            seed_globals(&r->ent);
            seed_tilemap(r);
            memset(host_desc, 0, sizeof(host_desc));
            if (fn_is_draw_or_erase(r->fn_addr))
                p1_sprite = host_desc;       /* draw fns write the blit descriptor */

            if (r->fn_addr == FN_APPLY_CELL_ANIMATION) {
                /* apply_cell_animation(action) — the cdecl action code.  The trace
                   does not carry the action word directly; the future port recovers
                   it from the scenario setup (the alloc scenarios fix it).  Until the
                   fn is PORTED this branch is dead code (fn==NULL above), so a 0 arg
                   is a safe placeholder the T3 port will replace with the recovered
                   action. */
                void (*f1)(u8) = (void (*)(u8))fn;
                f1(0);
            } else {
                fn();
            }

            bad = cmp_semantic(r, &got, &want);
            if (bad == NULL && fn_is_draw_or_erase(r->fn_addr) && r->desc_len) {
                bad = cmp_descriptor(r, &got, &want);
                if (bad == NULL && r->desc_kind) st->desc_checked++;
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
    printf("  src/anim.c is the GLOBALS-ONLY skeleton (Phase-5 T2): all seven anim "
           "fns UNPORTED (bodies port in T3/T4) — expected result is every record "
           "UNPORTED, FAIL=0.\n");

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
    printf("PASS: FAIL=0.  %ld records UNPORTED (all seven anim fns are unported in "
           "the Phase-5 T2 globals-only skeleton; %ld matched).\n",
           st.unported, st.pass);
    return 0;
}
