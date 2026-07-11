/* Host REPLAY HARNESS for copyprotect_challenge (src/level.c, 1000:4015) — Phase-7b T2.
 *
 * Compiles the REAL reconstructed challenge body (src/level.c, behind
 * #define BUMPY_COPY_PROTECTION) on the host — the Open Watcom 16-bit environment
 * shimmed out (__far/__huge/__cdecl16near erased, exact-width typedefs, BUMPY_H so the
 * headers do not pull <dos.h>) — and validates it against the Phase-7b T1 capture
 * local/build/render/copyprot_trace.bin (magic "CPTRC01", version 1; layout frozen in
 * tools/copyprot_oracle.py §"TRACE LAYOUT").
 *
 * TWO comparator flavors (both run; neither weakened):
 *
 *  (a) PRESENT-PARTS DIFFERENTIAL vs the T1 trace — every part the cracked binary
 *      ACTUALLY contains, reproduced by the ported body:
 *        • the two table copies (sprite_id_tbl 16 words / answer_tbl 16 bytes) seeded
 *          from the trace and read back through the body's copy primitives;
 *        • the random sprite index ∈ 2..15, reproduced by SEEDING src/prng.c from the
 *          captured LIVE prng state (0x5192,0,0) and running the same reject loop → 12;
 *        • the entered_number trajectory for each scripted dial sequence, captured from
 *          the body's draw_number() calls (the 4×-poll-per-action sampling) and matched
 *          against the trace traj;
 *        • the p1_sprite display descriptor (x=0x90, y=100, frame=sprite_id_tbl[12]).
 *
 *  (b) UN-CRACK LOGIC — the documented original compare the crack removed:
 *        • entered == answer_tbl[index]  ⇒  copyprotect_flag stays 0   (PASS)
 *        • entered != answer_tbl[index]  ⇒  copyprotect_flag = -1 (0xff)(FAIL)
 *        • the 0x62 plus-CEILING clamp (NOT exercised by T1) — a "+"-to-ceiling script
 *          drives entered_number up and asserts it clamps at 0x62.
 *
 * The challenge calls render/input/resource primitives that live in other modules; on
 * the host we provide our OWN definitions (below) that capture the dial input + the
 * display descriptor and replay the scripted input.  The +/-/FIRE dial is driven by a
 * scripted input queue expanded exactly as the oracle does ([action,0,0,0] pulses + a
 * terminating FIRE pulse): each poll_input() consumes one queue byte into input_state.
 *
 * Build/run (also wrapped by tools/validate_copyprot.sh):
 *     cc -O2 -Wall -o /tmp/copyprot_ctest tools/copyprot_ctest.c && \
 *       /tmp/copyprot_ctest local/build/render/copyprot_trace.bin
 * Exit 0 iff present-parts PASS + un-crack logic PASS, FAIL=0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── shim the Watcom 16-bit environment for host compilation ─────────────────── */
#define BUMPY_H            /* level.h's #include "bumpy.h" becomes a no-op */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge
#define __cdecl16near
/* The body uses FP_OFF/FP_SEG only to pass descriptor far-ptr halves into shims that
 * read the descriptor global directly; the host values are immaterial. */
#define FP_OFF(p) ((u16)0)
#define FP_SEG(p) ((u16)0)
#define BUMPY_COPY_PROTECTION   /* compile the challenge body in src/level.c */
#define BUMPY_COPYPROT_HARNESS  /* compile ONLY the challenge (skip start_level + render) */

/* ── bumpy.h surrogate (BUMPY_H above suppresses the real one) ────────────────── */
/* prng state + step — owned by globals.c/prng.c in the real build; defined here. */
u16 prng_state0, prng_state1, prng_state2;
void prng_seed(u16 seed);
void prng_step(void);

/* ════════════════════════════════════════════════════════════════════════════
 *  HOST SHIMS for the copy-protection callees + DGROUP globals.
 *  These mirror the prototypes src/level.c declares (extern) for the challenge.
 * ════════════════════════════════════════════════════════════════════════════ */

/* DGROUP globals reached by the challenge. */
u8  input_state;
u16 palette_mode;
u16 copyprot_seed_src;          /* live prng seed (0x5192 captured from the trace) */

/* Far buffers (host: plain pointers).  Each shim reads these globals directly. */
static u8  host_dec_buf[256];   /* level_dec_buf_fp target                         */
static u8  host_p1_desc[0x20];  /* p1_sprite descriptor target                     */
static u8  host_cur_level[64];  /* cur_level_ptr target (16 words)                 */
static u8  host_patch[256];     /* copyprot_patch_ptr target                       */
static u8  host_palette_src[16];/* code-seg palette patch table @0x65a            */
static u16 host_levelptr_src[16];/* code-seg level table @0x73e                    */

u8  __far *level_dec_buf_fp     = host_dec_buf;
u8  __far *p1_sprite            = host_p1_desc;
u8  __far *cur_level_ptr        = host_cur_level;
u8  __far *copyprot_patch_ptr   = host_patch;
u8  __far *copyprot_palette_src = host_palette_src;
u16 __far *copyprot_levelptr_src= host_levelptr_src;

/* The two fixed DGROUP tables, seeded by the harness from the trace before each call.
 * The copy primitives below copy from these into the challenge's SS-local arrays. */
static u16 seed_sprite_id_tbl[16];   /* DS:0x11b6 — sprite frame ids */
static u8  seed_answer_tbl[16];      /* DS:0x11d6 — correct answers  */

/* ── capture state (filled while the ported challenge runs) ──────────────────── */
static u8  cap_dial_traj[512];   /* draw_number() first-arg sequence (entered_number) */
static int cap_dial_n;
static u8  cap_entered_last;     /* last draw_number first-arg (always updated)        */
static u16 cap_desc_x, cap_desc_y, cap_desc_frame; /* p1_sprite descriptor at blit */
static int cap_desc_seen;

/* scripted dial input queue ([action,0,0,0] pulses + terminating [FIRE,0,0,0]).
   Sized for the ceiling case: up to ~200 plus actions * 4 + 4 = ~804 entries. */
static u8  in_queue[1024];
static int in_queue_n, in_queue_i;

/* ── copy primitives (fmemcpy 1000:a9f5) ─────────────────────────────────────── */
void copyprot_copy_words(u16 __far *dst, u16 src_off, u16 nwords)
{
    u16 i;
    (void)src_off;   /* the host seeds from the captured table, not a DGROUP offset */
    for (i = 0u; i < nwords; i++) dst[i] = seed_sprite_id_tbl[i];
}
void copyprot_copy_bytes(u8 __far *dst, u16 src_off, u16 nbytes)
{
    u16 i;
    (void)src_off;
    for (i = 0u; i < nbytes; i++) dst[i] = seed_answer_tbl[i];
}

/* ── setup / resource callees — inert on the host (no DOS I/O) ─────────────────── */
void set_sprite_table_ptr(void)            { }
void set_active_display_page(void)         { }
void set_resource_table(u16 r, u16 s)      { (void)r; (void)s; }
void prng_seed_thunk(u16 seed)             { prng_seed(seed); }
int  open_resource(void)                   { return 7; }      /* sentinel handle */
void read_chunked(int h, u16 bo, u16 bs, u16 len, u16 a)
{ (void)h; (void)bo; (void)bs; (void)len; (void)a; }          /* seeded buffer  */
void c_close(int h)                        { (void)h; }
void load_palette_byteswapped(void)        { }
void play_iris_wipe_transition(void)       { }
void load_palette(u16 so, u16 ss)          { (void)so; (void)ss; }  /* engine 1000:08d1 (2-arg) */

/* ── render callees that the present-parts comparator inspects ─────────────────── */
void blit_sprite(u16 desc_off, u16 desc_seg)
{
    (void)desc_off; (void)desc_seg;
    /* capture the p1_sprite descriptor the body wrote (x@0, y@2, frame@4). */
    cap_desc_x     = (u16)host_p1_desc[0] | ((u16)host_p1_desc[1] << 8);
    cap_desc_y     = (u16)host_p1_desc[2] | ((u16)host_p1_desc[3] << 8);
    cap_desc_frame = (u16)host_p1_desc[4] | ((u16)host_p1_desc[5] << 8);
    cap_desc_seen  = 1;
}
void draw_text_at(u16 str_off, u16 str_seg, u16 x, u16 y)
{ (void)str_off; (void)str_seg; (void)x; (void)y; }

void draw_number(u16 val_lo, u16 val_hi, u8 width, u16 x, u16 y)
{
    (void)val_hi; (void)width; (void)x; (void)y;
    /* The body calls draw_number(entered_number,...) once before the loop and once at
       LAB_redraw each iteration.  Record the first-arg sequence; the poll-entry traj is
       this sequence minus its final element (see tools/copyprot_oracle.py sampling). */
    cap_entered_last = (u8)val_lo;   /* always track the latest (final) entered_number */
    if (cap_dial_n < (int)sizeof(cap_dial_traj)) {
        cap_dial_traj[cap_dial_n++] = (u8)val_lo;
    }
}

/* poll_input (1000:1dde) — replay the next scripted action byte into input_state. */
void poll_input(void)
{
    if (in_queue_i < in_queue_n) {
        input_state = in_queue[in_queue_i++];
    } else {
        input_state = 0x10u;   /* safety: confirm if the script runs dry (never normally) */
    }
}
void run_n_frames(u8 n)                     { (void)n; }

/* ════════════════════════════════════════════════════════════════════════════
 *  Pull in the reconstructed PRNG + the challenge body itself.
 * ════════════════════════════════════════════════════════════════════════════ */
/* src/prng.c is a VERBATIM transcription of the Ghidra decomp (the operator-precedence
   idioms are intentional, matching the asm); silence -Wparentheses only for its body so
   the harness can keep -Werror for the harness's own code (the p2_ctest.c convention). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"
#include "../src/prng.c"
#pragma GCC diagnostic pop
#include "../src/level.c"

/* ════════════════════════════════════════════════════════════════════════════
 *  Trace parsing (CPTRC01 v1) — mirrors tools/copyprot_oracle.py's writer exactly.
 * ════════════════════════════════════════════════════════════════════════════ */
#define MAX_SC 16
#define MAX_TRAJ 256
typedef struct {
    int   id;
    char  name[32];
    int   n_acts; u8 acts[64];
    u16   rand_ret; u8 masked; u8 accepted;   /* first (accepted) draw */
    u8    accepted_index;
    u8    expected_answer;
    int   n_traj; u8 traj[MAX_TRAJ];
    u8    en_final, ed_final, flag;
    u16   dx, dy, dframe;
    int   n_desc; u8 desc[64];
} scen_t;

static u16 prng_seed_hdr;
static u16 prng_state_hdr[3];
static u8  pos_n; static u16 pos_tbl[16];
static u8  ans_n; static u8  ans_tbl[16];
static int n_scen;
static scen_t scen[MAX_SC];

static u8 *g_d; static size_t g_o, g_len;
static u8  rd8(void)  { return g_d[g_o++]; }
static u16 rd16(void) { u16 v = (u16)g_d[g_o] | ((u16)g_d[g_o+1] << 8); g_o += 2; return v; }

static int parse_trace(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz; size_t got; int s, i;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    g_d = (u8 *)malloc((size_t)sz); got = fread(g_d, 1, (size_t)sz, f); fclose(f);
    if (got != (size_t)sz) { fprintf(stderr, "short read\n"); return -1; }
    g_len = (size_t)sz; g_o = 0;
    if (memcmp(g_d, "CPTRC01\0", 8) != 0) { fprintf(stderr, "bad magic\n"); return -1; }
    g_o = 8;
    { u16 ver = rd16(); n_scen = rd16();
      if (ver != 1) { fprintf(stderr, "bad version %u\n", ver); return -1; } }
    prng_seed_hdr = rd16();
    prng_state_hdr[0] = rd16(); prng_state_hdr[1] = rd16(); prng_state_hdr[2] = rd16();
    pos_n = rd8(); for (i = 0; i < pos_n; i++) pos_tbl[i] = rd16();
    ans_n = rd8(); for (i = 0; i < ans_n; i++) ans_tbl[i] = rd8();
    if (n_scen > MAX_SC) { fprintf(stderr, "too many scenarios\n"); return -1; }
    for (s = 0; s < n_scen; s++) {
        scen_t *r = &scen[s];
        int nl, nd, nt;
        r->id = rd8();
        nl = rd8(); memcpy(r->name, g_d + g_o, nl); r->name[nl] = 0; g_o += nl;
        r->n_acts = rd8(); memcpy(r->acts, g_d + g_o, r->n_acts); g_o += r->n_acts;
        nd = rd8();
        for (i = 0; i < nd; i++) {
            u16 ret = rd16(); u8 m = rd8(); u8 acc = rd8();
            if (i == 0) { r->rand_ret = ret; r->masked = m; r->accepted = acc; }
        }
        r->accepted_index  = rd8();
        r->expected_answer = rd8();
        nt = rd8(); r->n_traj = nt; memcpy(r->traj, g_d + g_o, nt); g_o += nt;
        r->en_final = rd8(); r->ed_final = rd8(); r->flag = rd8();
        r->dx = rd16(); r->dy = rd16(); r->dframe = rd16();
        r->n_desc = rd8(); memcpy(r->desc, g_d + g_o, r->n_desc); g_o += r->n_desc;
    }
    if (g_o != g_len) {
        fprintf(stderr, "trailing bytes: parsed %zu of %zu\n", g_o, g_len);
        return -1;
    }
    return 0;
}

/* ── expand a high-level action list into the FUN_75a2 return stream ──────────── */
static void build_queue(const u8 *acts, int n)
{
    int i;
    in_queue_n = 0;
    for (i = 0; i < n; i++) {
        in_queue[in_queue_n++] = acts[i];
        in_queue[in_queue_n++] = 0;
        in_queue[in_queue_n++] = 0;
        in_queue[in_queue_n++] = 0;
    }
    in_queue[in_queue_n++] = 0x10u;     /* terminating FIRE pulse */
    in_queue[in_queue_n++] = 0;
    in_queue[in_queue_n++] = 0;
    in_queue[in_queue_n++] = 0;
    in_queue_i = 0;
}

/* ── seed the two fixed DGROUP tables from the trace's captured tables ─────────── */
static void seed_tables(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        seed_sprite_id_tbl[i] = (i < pos_n) ? pos_tbl[i] : 0u;
        seed_answer_tbl[i]    = (i < ans_n) ? ans_tbl[i] : 0u;
    }
}

/* ── seed the harness state and run one ported challenge() ─────────────────────── */
static void seed_and_run(const u8 *acts, int n_acts, u8 inject_flag_pre)
{
    seed_tables();
    /* seed the LIVE prng seed source (0x5192) so the body's reject loop draws idx 12 */
    copyprot_seed_src = prng_seed_hdr;
    palette_mode = 0u;               /* the trace's runtime path (no patch) */
    copyprotect_flag = inject_flag_pre;
    /* reset capture + scripted input */
    cap_dial_n = 0; cap_desc_seen = 0; cap_entered_last = 0;
    cap_desc_x = cap_desc_y = cap_desc_frame = 0;
    input_state = 0u;
    build_queue(acts, n_acts);
    copyprotect_challenge();
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Comparators
 * ════════════════════════════════════════════════════════════════════════════ */
static int g_pass = 0, g_fail = 0;
#define OK(cond, ...)  do { if (cond) { g_pass++; } else { \
        g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* (a) PRESENT-PARTS differential vs the T1 trace. */
static void cmp_present_parts(void)
{
    int s, i, ok;
    printf("== (a) PRESENT-PARTS differential vs T1 trace ==\n");
    seed_tables();

    /* table copies: seed → body copy primitive → read back the SS-local arrays.
       We re-run the copy primitives into host scratch and verify they mirror the
       seeded (trace-captured) tables exactly. */
    {
        u16 w[16]; u8 b[16];
        copyprot_copy_words(w, 0x11b6u, 16u);
        copyprot_copy_bytes(b, 0x11d6u, 16u);
        ok = 1;
        for (i = 0; i < pos_n; i++) if (w[i] != pos_tbl[i]) ok = 0;
        OK(ok, "sprite_id_tbl copy mismatch");
        ok = 1;
        for (i = 0; i < ans_n; i++) if (b[i] != ans_tbl[i]) ok = 0;
        OK(ok, "answer_tbl copy mismatch");
    }

    /* random index ∈ 2..15 reproduced via the seeded LIVE prng state. */
    {
        u16 idx;
        prng_seed(prng_seed_hdr);
        OK(prng_state0 == prng_state_hdr[0] && prng_state1 == prng_state_hdr[1] &&
           prng_state2 == prng_state_hdr[2],
           "post-seed prng state (%04x,%04x,%04x) != trace (%04x,%04x,%04x)",
           prng_state0, prng_state1, prng_state2,
           prng_state_hdr[0], prng_state_hdr[1], prng_state_hdr[2]);
        do { idx = (u16)copyprot_engine_rand() & 0xfu; } while (idx < 2u);
        OK(idx >= 2u && idx <= 15u, "reproduced index %u not in 2..15", idx);
        OK(idx == scen[0].accepted_index,
           "reproduced index %u != trace accepted_index %u", idx, scen[0].accepted_index);
    }

    /* per-scenario: run the body with the scripted dial, check traj + descriptor. */
    for (s = 0; s < n_scen; s++) {
        scen_t *r = &scen[s];
        seed_and_run(r->acts, r->n_acts, 1u /* mirror cracked pre-set, harmless */);
        /* the poll-entry trajectory == draw_number sequence minus its final element */
        {
            int got_n = cap_dial_n - 1;     /* drop the final LAB_redraw draw */
            if (got_n < 0) got_n = 0;
            ok = (got_n == r->n_traj);
            for (i = 0; ok && i < r->n_traj; i++)
                if (cap_dial_traj[i] != r->traj[i]) ok = 0;
            if (!ok) {
                int firstdiv = -1;
                for (i = 0; i < r->n_traj && i < got_n; i++)
                    if (cap_dial_traj[i] != r->traj[i]) { firstdiv = i; break; }
                printf("  [%s] traj len got=%d want=%d firstdiv=%d\n",
                       r->name, got_n, r->n_traj, firstdiv);
            }
            OK(ok, "[%s] entered_number trajectory mismatch", r->name);
        }
        /* p1_sprite display descriptor (x=0x90, y=100, frame=pos_tbl[index]). */
        OK(cap_desc_seen, "[%s] descriptor never captured", r->name);
        OK(cap_desc_x == r->dx, "[%s] desc x %#x != %#x", r->name, cap_desc_x, r->dx);
        OK(cap_desc_y == r->dy, "[%s] desc y %u != %u", r->name, cap_desc_y, r->dy);
        OK(cap_desc_frame == r->dframe,
           "[%s] desc frame %#x != %#x", r->name, cap_desc_frame, r->dframe);
    }
}

/* (b) UN-CRACK logic — the documented original compare. */
static void cmp_uncrack(void)
{
    u8 acts_zero[1];           /* immediate confirm (entered stays 0)             */
    u8 acts_to_ans[64];        /* "+"-up to the answer                            */
    u8 acts_ceiling[200];      /* "+"-to-ceiling: drive entered_number past 0x62  */
    int i, correct;
    printf("== (b) UN-CRACK logic (pass-on-match / -1-on-mismatch / ceiling clamp) ==\n");

    /* The reproduced index is 12 ⇒ answer_tbl[12] = 25 (0x19).  We confirm against
       the seeded table rather than hardcoding. */
    {
        u16 idx;
        prng_seed(prng_seed_hdr);
        do { idx = (u16)copyprot_engine_rand() & 0xfu; } while (idx < 2u);
        correct = (idx < ans_n) ? ans_tbl[idx] : 0;
        printf("   reproduced index=%u  correct answer=%d\n", idx, correct);
    }

    /* CASE 1 — entered == answer ⇒ PASS (copyprotect_flag stays 0). */
    {
        int n = 0;
        for (i = 0; i < correct; i++) acts_to_ans[n++] = 0x08u;   /* "+" up to answer */
        seed_and_run(acts_to_ans, n, 0u /* start clean: original pre-state */);
        OK(cap_entered_last == (u8)correct,
           "match-case: entered %u != answer %d", cap_entered_last, correct);
        OK(copyprotect_flag == 0u,
           "match-case: copyprotect_flag = %d, expected 0 (PASS)", (s8)copyprotect_flag);
    }

    /* CASE 2 — entered != answer ⇒ FAIL (copyprotect_flag = -1). */
    {
        /* immediate confirm at 0; 0 != 25 ⇒ fail. */
        acts_zero[0] = 0;   /* unused; pass n_acts = 0 for immediate FIRE */
        (void)acts_zero;
        seed_and_run(acts_zero, 0, 0u);
        OK(cap_entered_last == 0u,
           "mismatch-case: entered %u != 0", cap_entered_last);
        OK((s8)copyprotect_flag == -1,
           "mismatch-case: copyprotect_flag = %d, expected -1 (FAIL)", (s8)copyprotect_flag);
    }

    /* CASE 3 — plus-CEILING clamp (NOT exercised by T1).  The dial guard at 1000:41d3
       is `CMP entered_number,0x63 / JNC skip`: it increments only while
       entered_number < 0x63, so the value SATURATES at 0x63 (99).  (The brief's "0x62"
       names the largest value from which a "+" still increments — 0x62 → 0x63 — not the
       saturation point.)  Drive far more "+" than the ceiling allows and assert it
       clamps at 0x63. */
    {
        int n = 0;
        for (i = 0; i < 0x80; i++) acts_ceiling[n++] = 0x08u;   /* 128 "+" presses */
        seed_and_run(acts_ceiling, n, 0u);
        OK(cap_entered_last == 0x63u,
           "ceiling-case: entered_number = %#x, expected saturation at 0x63",
           cap_entered_last);
        /* 0x62 != 25 ⇒ also a protection failure. */
        OK((s8)copyprotect_flag == -1,
           "ceiling-case: copyprotect_flag = %d, expected -1", (s8)copyprotect_flag);
    }
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "local/build/render/copyprot_trace.bin";
    if (parse_trace(path) != 0) { fprintf(stderr, "trace parse failed\n"); return 2; }

    printf("trace: CPTRC01 v1, %d scenarios, seed=%#x state=(%04x,%04x,%04x)\n",
           n_scen, prng_seed_hdr, prng_state_hdr[0], prng_state_hdr[1], prng_state_hdr[2]);
    printf("tables: %u positions, %u answers\n\n", pos_n, ans_n);

    cmp_present_parts();
    printf("\n");
    cmp_uncrack();

    printf("\nTOTAL: PASS=%d FAIL=%d\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
