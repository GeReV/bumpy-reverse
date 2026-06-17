/*
 * op12.c — op12 masked-blit compositor + inner vec_run record loop.
 *
 * Faithful C transliteration of tools/extract/op12_port.py.  See op12.h for the
 * architecture (arena/window/state mapping).  Every function below mirrors the
 * like-named op12_port method line-for-line; op12_port's inline comments
 * (0x20-alignment je 0x52f skip, the dstend/dst2 branch direction at 0x11a, the
 * once-only relocation gate) are preserved as behaviour and noted inline.
 */

#include "op12.h"
#include <string.h>

/* ── Linear-address model (matches op12_port) ───────────────────────────── */
#define DG_LIN        0x114b0UL          /* DGROUP runtime linear base        */
#define ARENA_BASE    0x67bf0UL          /* op12_port STREAM load base        */
#define WIN_BASE      (DG_LIN + 0x4e97UL)/* op12_port DG:0x4e97 window base    */
#define WIN_SIZE      0x400u

/* DGROUP state window: op12_port uses offsets 0x4df6..0x4e35.  We mirror it as
   a byte array based at ST_BASE so gv(o)=word at st[o-ST_BASE]. */
#define ST_BASE       0x4df6u
#define ST_SIZE       0x44u

/* ── Module state (one op12 run at a time; matches op12_port's instance) ──── */
static u8 __far *g_arena;                /* arena[lin - ARENA_BASE]           */
static u8        g_win[WIN_SIZE];        /* window[lin - WIN_BASE]            */
static u8        g_st[ST_SIZE];          /* DGROUP state (0x4df6..)           */

/* Explicit op-stack mirroring op12_port's self.stack (push/pop of 16-bit words).
   Max depth observed is small (phase1 pushes 6, run pushes 6, never both
   live simultaneously beyond that); 32 is ample. */
static u16 g_stk[32];
static u16 g_sp;

static void stk_push(u16 v) { g_stk[g_sp++] = v; }
static u16  stk_pop(void)   { return g_stk[--g_sp]; }

/* ── Memory access by linear address (route arena vs window) ────────────── */
static u8 mrd(u32 lin)
{
    if (lin >= ARENA_BASE) {
        return g_arena[(u16)(lin - ARENA_BASE)];
    }
    return g_win[(u16)(lin - WIN_BASE)];
}

static void mwr(u32 lin, u8 v)
{
    if (lin >= ARENA_BASE) {
        g_arena[(u16)(lin - ARENA_BASE)] = v;
    } else {
        g_win[(u16)(lin - WIN_BASE)] = v;
    }
}

/* ── DGROUP state word/byte access (gv/sv/byte) ─────────────────────────── */
static u16 gv(u16 o)
{
    u16 i = (u16)(o - ST_BASE);
    return (u16)(g_st[i] | ((u16)g_st[i + 1] << 8));
}

static void sv(u16 o, u16 v)
{
    u16 i = (u16)(o - ST_BASE);
    g_st[i]     = (u8)(v & 0xFF);
    g_st[i + 1] = (u8)((v >> 8) & 0xFF);
}

static u8 gb(u16 o)        { return g_st[(u16)(o - ST_BASE)]; }
static void sb(u16 o, u8 v){ g_st[(u16)(o - ST_BASE)] = v; }

/* getlin/setlin: linear = (seg<<4)+off (both 16-bit DG words). */
static u32 getlin(u16 off_o, u16 seg_o)
{
    return ((u32)gv(seg_o) << 4) + (u32)gv(off_o);
}

static void setlin(u16 off_o, u16 seg_o, u32 lin)
{
    sv(off_o, (u16)(lin & 0xF));
    sv(seg_o, (u16)((lin >> 4) & 0xFFFF));
}

static void norm(u16 off_o, u16 seg_o)
{
    u32 lin = ((u32)gv(seg_o) << 4) + (u32)gv(off_o);
    sv(off_o, (u16)(lin & 0xF));
    sv(seg_o, (u16)((lin >> 4) & 0xFFFF));
}

/* ── forward decls ──────────────────────────────────────────────────────── */
static int  phase1(void);
static void vec_run_7e(void);
static void run(void);
static void plot_loop(void);
static void do_plot(void);
static void do_fill(void);
static void advance_dst_check_wrap(void);
static void phase3_relocate(void);
static void finalize(void);
static void op4_handler(u16 payload_len);

/* vec_read_record return: opcode in *op_out, function returns 1 to continue,
   0 to terminate (op12_port returns None). */
static int vec_read_record(u16 *op_out)
{
    u32 sp = getlin(0x4e0e, 0x4e10);
    u16 w0 = (u16)(((u16)mrd(sp) << 8) | mrd(sp + 1));
    u16 w1 = (u16)(((u16)mrd(sp + 2) << 8) | mrd(sp + 3));
    u16 w2 = (u16)(((u16)mrd(sp + 4) << 8) | mrd(sp + 5));
    u16 w3 = (u16)(((u16)mrd(sp + 6) << 8) | mrd(sp + 7));
    u16 w4 = (u16)(((u16)mrd(sp + 8) << 8) | mrd(sp + 9));
    u16 w5 = (u16)(((u16)mrd(sp + 10) << 8) | mrd(sp + 11));

    sv(0x4e26, w0); sv(0x4e24, w1);
    sv(0x4e20, w3); sv(0x4e1e, w2);
    sv(0x4e31, w4);
    sv(0x4e35, w0); sv(0x4e33, w1);

    if (w0 > 0x0Fu) {
        return 0;
    }
    if ((w4 & 0x7F00u) != 0u) {
        return 0;
    }
    if ((u16)(w0 ^ w1 ^ w2 ^ w3 ^ w4) != w5) {
        return 0;
    }
    *op_out = (u16)(w4 & 0x7FFFu);
    return 1;
}

/* vec_run: dispatch op12/op4 by opcode until opcode<=0.
   On entry the current record has already been read by the caller's
   vec_read_record (dispatch_current=False in op12_port's outer driver means we
   read the first record here). */
void op12_vec_run(u8 __far *arena, u16 declared_len, u16 payload_len)
{
    u16 op;

    g_arena = arena;
    g_sp = 0;
    memset(g_win, 0, sizeof(g_win));
    memset(g_st, 0, sizeof(g_st));

    /* Seed the DGROUP pointers the way vec_to_png.decode_vec_to_framebuffer
       does before calling vec_run(dispatch_current=False):
         vec_stream = ARENA_BASE
         vec_end    = ARENA_BASE + declared_len
         vsav       = payload_len (op4 input-end bound) */
    setlin(0x4e0e, 0x4e10, ARENA_BASE);
    setlin(0x4e0a, 0x4e0c, ARENA_BASE + (u32)declared_len);
    sv(0x4e28, payload_len);
    sv(0x4e2a, 0);

    if (!vec_read_record(&op)) {
        return;
    }

    while (op > 0u) {
        if (op == 12u) {
            if (!phase1()) {
                break;
            }
            run();
        } else if (op == 4u) {
            op4_handler(payload_len);
        }
        /* vsav update (0x60): vsav = w0:w1 of the record just processed. */
        sv(0x4e2a, gv(0x4e35));
        sv(0x4e28, gv(0x4e33));

        if (!vec_read_record(&op)) {
            break;
        }
    }
}

/* phase1: op12 0x4b0..0x66b. Returns 0 to terminate op12. */
static int phase1(void)
{
    u32 stream;
    u32 vsrc;
    u32 probe;
    u32 p;
    u16 raw_hi, raw_lo;
    u32 val;
    u32 vend;
    u32 crd;
    u32 d2;
    u32 dst2;
    u32 cnt;
    u32 si, di;

    stream = getlin(0x4e0e, 0x4e10);
    vsrc = (u32)gv(0x4e24) | ((u32)gv(0x4e26) << 16);
    probe = (stream + vsrc) & 0xFFFFFUL;
    if (probe > getlin(0x4e0a, 0x4e0c)) {          /* past vec_end -> terminate */
        return 0;
    }
    /* 0x4d8: fill word (BE) at stream+0xc; dst = stream+0xe */
    p = stream + 0xCUL;
    setlin(0x4df6, 0x4df8, p);
    sv(0x4e22, (u16)(((u16)mrd(p) << 8) | mrd(p + 1)));
    sv(0x4df6, (u16)(gv(0x4df6) + 2));

    /* 0x503: crd = round-up(vec_src, 0x20) >> 3.  Clear low 5 bits; if that
       CHANGED the value round up by +0x20 (0x529), else use as-is (the 0x520
       je 0x52f skip).  See op12_port comment: must NOT always add 0x20. */
    raw_hi = gv(0x4e26); raw_lo = gv(0x4e24);
    sv(0x4e20, raw_hi); sv(0x4e1e, raw_lo);
    sb(0x4e1e, (u8)(gb(0x4e1e) & 0xE0));
    if (raw_hi == gv(0x4e20) && raw_lo == gv(0x4e1e)) {   /* already 0x20-aligned */
        val = (((u32)raw_hi << 16) | (u32)raw_lo);
    } else {                                              /* masked -> round up */
        val = ((((u32)gv(0x4e20) << 16) | (u32)gv(0x4e1e)) + 0x20UL);
    }
    val >>= 3;
    sv(0x4e20, (u16)((val >> 16) & 0xFFFF));
    sv(0x4e1e, (u16)(val & 0xFFFF));

    /* 0x542: dst2 = (vend - crd) & ~1 */
    vend = getlin(0x4e0a, 0x4e0c);
    crd = (u32)gv(0x4e1e) | ((u32)gv(0x4e20) << 16);
    d2 = (vend - crd) & 0xFFFFFUL;
    sv(0x4e18, (u16)((d2 >> 4) & 0xFFFF));
    sv(0x4e16, (u16)(d2 & 0xF));
    sb(0x4e16, (u8)(gb(0x4e16) & 0xFE));
    dst2 = ((u32)gv(0x4e18) << 4) + (u32)gv(0x4e16);
    setlin(0x4dfa, 0x4dfc, dst2);
    setlin(0x4dfe, 0x4e00, dst2);

    /* 0x583: coord-copy crd bytes from dst(=stream+0xe) -> dst2 (4 at a time) */
    cnt = crd;
    si = getlin(0x4df6, 0x4df8);
    di = dst2;
    while (cnt != 0u) {
        mwr(di + 0, mrd(si + 0));
        mwr(di + 1, mrd(si + 1));
        mwr(di + 2, mrd(si + 2));
        mwr(di + 3, mrd(si + 3));
        si += 4; di += 4;
        cnt = (cnt - 4) & 0xFFFFFFFFUL;
        if (cnt == 0u) {
            break;
        }
    }
    setlin(0x4df6, 0x4df8, si);
    setlin(0x4dfe, 0x4e00, di);

    /* 0x60f: save 6 words, vend=dst2, vec_src=vsav, recurse. */
    stk_push(gv(0x4e26)); stk_push(gv(0x4e24));
    stk_push(gv(0x4dfc)); stk_push(gv(0x4dfa));
    stk_push(gv(0x4e0c)); stk_push(gv(0x4e0a));
    sv(0x4e0c, gv(0x4dfc)); sv(0x4e0a, gv(0x4dfa));
    sv(0x4e26, gv(0x4e2a)); sv(0x4e24, gv(0x4e28));
    vec_run_7e();
    sv(0x4e0a, stk_pop()); sv(0x4e0c, stk_pop());
    sv(0x4dfa, stk_pop()); sv(0x4dfc, stk_pop());
    sv(0x4e24, stk_pop()); sv(0x4e26, stk_pop());

    if (gv(0x4e14) == 0u && gv(0x4e12) == 2u) {
        return 0;
    }
    return 1;
}

/* vec_run_7e: recursion leaf 1c28:007e. */
static void vec_run_7e(void)
{
    u16 v16;
    u32 rounded;
    u32 vend;
    u32 stream;
    u32 srcp;
    u32 dst;
    u32 dstend;
    u32 dst2;
    u32 di, si, n;

    /* rounded = vec_src rounded up to 0x10 */
    sv(0x4e14, gv(0x4e26)); sv(0x4e12, gv(0x4e24));
    sb(0x4e12, (u8)(gb(0x4e12) & 0xF0));
    if (!(gv(0x4e26) == gv(0x4e14) && gv(0x4e24) == gv(0x4e12))) {
        v16 = (u16)(gv(0x4e12) + 0x10);
        /* op12_port: if v > 0xFFFF carry into hi.  v16 is 16-bit; detect the
           carry as wrap (sum < 0x10 means it overflowed). */
        if ((u32)gv(0x4e12) + 0x10UL > 0xFFFFUL) {
            sv(0x4e14, (u16)(gv(0x4e14) + 1));
        }
        sv(0x4e12, v16);
    }
    rounded = (u32)gv(0x4e12) | ((u32)gv(0x4e14) << 16);
    vend = getlin(0x4e0a, 0x4e0c);
    stream = getlin(0x4e0e, 0x4e10);
    srcp = (vend - rounded) & 0xFFFFFUL;
    sv(0x4e08, (u16)((srcp >> 4) & 0xFFFF));
    sv(0x4e06, (u16)((srcp & 0xF) & 0xFE));
    srcp = getlin(0x4e06, 0x4e08);
    setlin(0x4dfa, 0x4dfc, srcp + rounded);
    setlin(0x4df6, 0x4df8, stream + rounded);
    dst = getlin(0x4df6, 0x4df8);
    setlin(0x4dfe, 0x4e00, dst + 0x10);
    dstend = getlin(0x4dfe, 0x4e00);
    dst2 = getlin(0x4dfa, 0x4dfc);

    /* 0x11a: only strict dstend > dst2 skips the copy and returns marker 2. */
    if (dstend > dst2) {
        sv(0x4e14, 0); sv(0x4e12, 2);
        return;
    }
    /* 0x135: backward word-copy `rounded` bytes dst -> dst2 (dstend <= dst2) */
    di = dst2; si = dst; n = rounded;
    while (n != 0u) {
        si -= 2; di -= 2;
        mwr(di + 0, mrd(si + 0));
        mwr(di + 1, mrd(si + 1));
        n = (n - 2) & 0xFFFFFFFFUL;
    }
    sv(0x4e14, 0); sv(0x4e12, 1);
}

/* run: phase 1b (0x66b) setup + plot_loop. */
static void run(void)
{
    u32 srcp;
    u32 vsav;
    u32 src;
    u32 crd;
    u32 t;
    u32 srcp2;
    u32 vend;
    u32 dstend;

    stk_push(gv(0x4e26)); stk_push(gv(0x4e24));
    stk_push(gv(0x4e2a)); stk_push(gv(0x4e28));
    stk_push(gv(0x4e0c)); stk_push(gv(0x4e0a));

    srcp = ((u32)gv(0x4e08) << 4) + (u32)gv(0x4e06);
    vsav = (u32)gv(0x4e28) | ((u32)gv(0x4e2a) << 16);
    src = srcp + vsav;
    sv(0x4e04, (u16)((src >> 4) & 0xFFFF));
    sv(0x4e02, (u16)(src & 0xF));
    crd = (u32)gv(0x4e1e) | ((u32)gv(0x4e20) << 16);
    t = srcp + crd;
    srcp2 = t + 0xEUL;
    sv(0x4e08, (u16)((srcp2 >> 4) & 0xFFFF));
    sv(0x4e06, (u16)(srcp2 & 0xF));
    vend = ((u32)gv(0x4e0c) << 4) + (u32)gv(0x4e0a);
    dstend = vend - 0x400UL;
    sv(0x4e00, (u16)((dstend >> 4) & 0xFFFF));
    sv(0x4dfe, (u16)(dstend & 0xF));
    sv(0x4e0c, gv(0x4dfc)); sv(0x4e0a, gv(0x4dfa));   /* mask ptr = dst2 */
    sv(0x4df8, gv(0x4e10)); sv(0x4df6, gv(0x4e0e));   /* dst = stream */
    sv(0x4e1c, 0); sv(0x4e1a, 0);
    sv(0x4e14, 0); sv(0x4e12, 0);
    sv(0x4e18, 0); sv(0x4e16, 0);
    norm(0x4df6, 0x4df8);
    norm(0x4e02, 0x4e04);
    plot_loop();
}

static void plot_loop(void)
{
    u32 mp;
    u32 w32;
    u32 v;
    u16 cnt;
    u16 cf;

    for (;;) {
        /* 0x71d: decrement bit counter; reload mask when it goes negative */
        cnt = (u16)(gv(0x4e16) - 1);
        sv(0x4e16, cnt);
        if (cnt & 0x8000u) {                 /* counter < 0 -> reload BE32 word */
            mp = getlin(0x4e0a, 0x4e0c);
            w32 = ((u32)mrd(mp) << 24) | ((u32)mrd(mp + 1) << 16)
                | ((u32)mrd(mp + 2) << 8) | (u32)mrd(mp + 3);
            setlin(0x4e0a, 0x4e0c, mp + 4);
            sv(0x4e16, 0x1f);
            sv(0x4e26, (u16)((w32 >> 16) & 0xFFFF));
            sv(0x4e24, (u16)(w32 & 0xFFFF));
        }
        /* 0x772: shift mask word left, top bit -> cf */
        v = ((u32)gv(0x4e26) << 16) | (u32)gv(0x4e24);
        cf = (u16)((v >> 31) & 1u);
        v = (v << 1) & 0xFFFFFFFFUL;
        sv(0x4e26, (u16)((v >> 16) & 0xFFFF));
        sv(0x4e24, (u16)(v & 0xFFFF));
        if (cf == 0u) {
            do_plot();                       /* 0x789 */
            /* 0x812: done-check only after a plot */
            if (gv(0x4e08) == gv(0x4e04) && gv(0x4e06) == gv(0x4e02)) {
                finalize();
                return;
            }
        } else {
            do_fill();                       /* 0x8d7 */
        }
        /* 0x828: relocation gate (fires at most once, then resumes) */
        phase3_relocate();
    }
}

static void advance_dst_check_wrap(void)
{
    u16 c = (u16)(gv(0x4e12) + 1);
    sv(0x4e12, c);
    if (c == 0u) {
        sv(0x4e14, (u16)(gv(0x4e14) + 1));
    }
    if (getlin(0x4dfe, 0x4e00) == getlin(0x4df6, 0x4df8)) {
        setlin(0x4df6, 0x4df8, WIN_BASE);
        sv(0x4e1c, 0); sv(0x4e1a, 1);
    }
}

static void do_plot(void)                    /* 0x789 */
{
    u32 dst = getlin(0x4df6, 0x4df8);
    u32 src = getlin(0x4e06, 0x4e08);
    mwr(dst, mrd(src));
    setlin(0x4e06, 0x4e08, src + 1);
    setlin(0x4df6, 0x4df8, dst + 1);
    advance_dst_check_wrap();
}

static void do_fill(void)                    /* 0x8d7 */
{
    u32 dst = getlin(0x4df6, 0x4df8);
    mwr(dst, (u8)(gv(0x4e22) & 0xFF));
    setlin(0x4df6, 0x4df8, dst + 1);
    advance_dst_check_wrap();
}

static void phase3_relocate(void)            /* 0x828 .. 0x8d4 */
{
    u32 srcp;
    u32 dst;
    u32 di, si;
    u32 srcp_lin;

    /* 0x828: srcp > dst -> resume plotting */
    srcp = getlin(0x4e06, 0x4e08);
    dst = getlin(0x4df6, 0x4df8);
    if (srcp > dst) {
        return;
    }
    /* 0x843/0x84d: if dst has wrapped into the window, don't relocate */
    if (gv(0x4e1c) != 0u || gv(0x4e1a) != 0u) {
        return;
    }
    /* 0x857: backward memmove of [srcp..src) so it ends at vec_end (=mask ptr) */
    sv(0x4dfc, gv(0x4e0c)); sv(0x4dfa, gv(0x4e0a));   /* dst2 = vec_end */
    di = getlin(0x4dfa, 0x4dfc);
    si = getlin(0x4e02, 0x4e04);
    srcp_lin = srcp;
    for (;;) {
        si -= 1; di -= 1;
        mwr(di, mrd(si));
        if (si == srcp_lin) {
            break;
        }
    }
    /* 0x8ab..0x8d1: dst2 = di; srcp = dst2; src = vec_end */
    setlin(0x4dfa, 0x4dfc, di);
    setlin(0x4e06, 0x4e08, di);
    sv(0x4e04, gv(0x4e0c)); sv(0x4e02, gv(0x4e0a));
}

static void finalize(void)                   /* 0x934 */
{
    u32 out_len;
    u32 dst;
    u16 c;
    u32 di, si;
    u16 k;

    sv(0x4e0a, stk_pop()); sv(0x4e0c, stk_pop());
    sv(0x4e28, stk_pop()); sv(0x4e2a, stk_pop());
    sv(0x4e24, stk_pop()); sv(0x4e26, stk_pop());
    out_len = (u32)gv(0x4e24) | ((u32)gv(0x4e26) << 16);

    /* 0x94c: fill dst with fill byte until op counter reaches out_len */
    for (;;) {
        dst = getlin(0x4df6, 0x4df8);
        mwr(dst, (u8)(gv(0x4e22) & 0xFF));
        setlin(0x4df6, 0x4df8, dst + 1);
        c = (u16)(gv(0x4e12) + 1);
        sv(0x4e12, c);
        if (c == 0u) {
            sv(0x4e14, (u16)(gv(0x4e14) + 1));
        }
        if (getlin(0x4dfe, 0x4e00) == getlin(0x4df6, 0x4df8)) {
            setlin(0x4df6, 0x4df8, WIN_BASE);
            sv(0x4e1c, 0); sv(0x4e1a, 1);
        }
        if (((u32)gv(0x4e12) | ((u32)gv(0x4e14) << 16)) >= out_len) {
            break;
        }
    }
    /* 0x9a9: final copy 0x100*4 = 1024 bytes window -> dstend */
    di = getlin(0x4dfe, 0x4e00);
    si = WIN_BASE;
    for (k = 0; k < 0x100u; k++) {
        mwr(di + 0, mrd(si + 0));
        mwr(di + 1, mrd(si + 1));
        mwr(di + 2, mrd(si + 2));
        mwr(di + 3, mrd(si + 3));
        si += 4; di += 4;
    }
}

/* Far scratch for the inner op4 payload snapshot (kept out of DGROUP). */
static u8 __far g_op4_scratch[OP12_ARENA_SIZE];

/* op4_handler: inner op4 (1c28:0194) RLE decompressor.  Faithful port of
   op12_port.op4_handler:
     - snapshot the compressed payload [stream+0xD, stream+vsav),
     - forward RLE-decode it into [stream, stream+vec_src) (escape byte =
       byte at stream+0xC),
     - reproduce the 0x400 sliding window at DG:0x4e97, end-aligned from the
       payload (a later op12 finalize copies that window into [dstend,vec_end),
       which the NEXT op12 record reads as its source).
   The snapshot makes the in-place decode overtake-safe and byte-exact. */
static void op4_handler(u16 payload_len)
{
    u32 stream;
    u32 vec_src;
    u32 vsav;
    u8  fill;
    u32 in_base;
    u32 in_end;
    u32 n;
    u32 j;
    u32 ip;
    u32 outlen;
    u8  v0, v1, v2;
    u16 rep;
    u16 i;
    u16 plen;
    u16 pad;

    (void)payload_len;
    stream  = getlin(0x4e0e, 0x4e10);
    vec_src = (u32)gv(0x4e24) | ((u32)gv(0x4e26) << 16);
    vsav    = (u32)gv(0x4e28) | ((u32)gv(0x4e2a) << 16);
    fill    = mrd(stream + 0xCUL);
    in_base = stream + 0xDUL;
    in_end  = stream + vsav;
    n       = in_end - in_base;          /* compressed payload length */

    /* Snapshot the compressed payload before the decode overwrites it. */
    for (j = 0; j < n; j++) {
        g_op4_scratch[(u16)j] = mrd(in_base + j);
    }

    /* Forward RLE decode the snapshot into [stream, stream+vec_src). */
    ip = 0;
    outlen = 0;
    while (outlen < vec_src && ip < n) {
        v0 = g_op4_scratch[(u16)ip]; ip++;
        if (v0 != fill) {
            mwr(stream + outlen, v0);
            outlen++;
            continue;
        }
        if (ip >= n) { break; }
        v1 = g_op4_scratch[(u16)ip]; ip++;
        if (v1 == fill) {
            mwr(stream + outlen, fill);
            outlen++;
        } else {
            if (ip >= n) { break; }
            v2 = g_op4_scratch[(u16)ip]; ip++;
            rep = (u16)(v2 ? v2 : 256);
            for (i = 0; i < rep && outlen < vec_src; i++) {
                mwr(stream + outlen, v1);
                outlen++;
            }
        }
    }

    /* Reproduce the 0x400 window end-aligned from the snapshotted payload. */
    plen = (u16)n;
    if (n <= 0x400UL) {
        pad = (u16)(0x400u - plen);
        for (i = 0; i < pad; i++) {
            g_win[i] = 0;
        }
        for (i = 0; i < plen; i++) {
            g_win[pad + i] = g_op4_scratch[i];
        }
    } else {
        for (i = 0; i < 0x400u; i++) {
            g_win[i] = g_op4_scratch[(u16)(n - 0x400UL) + i];
        }
    }
}
