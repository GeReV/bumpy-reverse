/* Host per-function differential test for src/input.c (Phase-1 Task 5).
 *
 * Compiles the REAL src/input.c with the Watcom 16-bit environment shimmed out
 * (BUMPY_H suppressed, exact 16-bit typedefs, __far/__huge emptied, BUMPY_CTEST
 * eliding the DOS-only ISR/BIOS bodies), then drives the reconstructed input
 * plumbing — key-state table -> read_input_action (FUN_1000_75a2) -> poll_input
 * -> input_state — over the scripted key stream and compares the resulting
 * per-tick input_state to the golden trace's input_state column.
 *
 * Build/run:
 *   cc -O2 -Wall -o /tmp/game_ctest tools/game_ctest.c && \
 *     /tmp/game_ctest local/build/render/slice_goldentrace.bin
 * Exit 0 iff all 100 ticks match.
 *
 * Nature of the validation (honest): the golden trace's input_state column is the
 * RESOLVED input SPEC (T2 injected input_state directly because the engine's
 * FUN_75a2 path misbehaved under the harness).  The interpreter in src/input.c is
 * a faithful reconstruction; the handler-script DATA seeded below is synthesized
 * from that same resolved spec (the runtime populator of g_joystick_handler_table
 * is a Task-1 UNCERTAIN item).  So this test proves the reconstructed interpreter
 * PLUMBING reproduces the spec end-to-end — it is not an independent engine
 * capture of the script bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── shim the Watcom 16-bit environment for host compilation ─────────────────── */
#define BUMPY_H        /* suppress src/bumpy.h (and <dos.h>) */
#define BUMPY_CTEST    /* elide DOS-only ISR/BIOS bodies in input.c */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge

/* CRT terminate stub: read_input_action calls dos_abort only on an out-of-range
   handler index or null script entry — never on the valid handler 0 path. */
int dos_abort(void) { fprintf(stderr, "dos_abort reached (unexpected)\n"); exit(3); return 0; }

#include "../src/input.c"

/* ── golden-trace layout (see tools/replay_check.py header) ──────────────────── */
#define GT_HEADER      16            /* "GTRACE01" + u32 n_ticks + u32 tick_size */
#define GT_PLANES      (4 * 0x4000)  /* 4 x 16384 VGA planes per tick            */
#define GT_NAMED       12            /* named-state block per tick               */
#define GT_TICK        (GT_PLANES + GT_NAMED)
#define GT_IS_OFFSET   7             /* input_state byte within the named state   */

/* ── scripted key stream (mirrors tools/game_oracle.py TICK_SCRIPT) ──────────────
   Physical scancode -> input_state bit (resolved in T2):
     0x4D right -> 0x08,  0x4B left -> 0x04,  0x48 up/jump -> 0x10,  none -> 0x00 */
#define SC_RIGHT 0x4D
#define SC_LEFT  0x4B
#define SC_JUMP  0x48

/* (n_ticks, scancode held this run; 0 == idle).  Same 100 ticks as TICK_SCRIPT. */
static const struct { int n; u8 sc; } TICK_SCRIPT[] = {
    { 10, 0        },   /* ticks  0.. 9 : idle  */
    { 30, SC_RIGHT },   /* ticks 10..39 : right */
    { 30, SC_LEFT  },   /* ticks 40..69 : left  */
    { 10, SC_JUMP  },   /* ticks 70..79 : jump  */
    { 20, 0        },   /* ticks 80..99 : idle  */
};

/* ── synthesized joystick-handler script (RECONSTRUCTION FIDELITY: data) ─────────
   Encodes the resolved mapping using read_input_action's exact opcode format:
     0xFD  phase-1 delimiter -> skips the joystick loop, opens phase 2
     <out> <scancode...>     per keyboard group; if any scancode is down, OR <out>
     0xFD  separates groups (ends a group's scancode loop, !=0xfe -> continue)
     0xFE  ends the final group's scancode loop -> terminates phase 2
   Groups: 0x08<-0x4D (right), 0x04<-0x4B (left), 0x10<-0x48 (jump). */
static u8 handler_script[] = {
    0xFD,
    0x08, SC_RIGHT,
    0xFD,
    0x04, SC_LEFT,
    0xFD,
    0x10, SC_JUMP,
    0xFE,
};

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "local/build/render/slice_goldentrace.bin";
    FILE *f = fopen(path, "rb");
    long sz;
    u8 *b;
    u32 n_ticks, tick_size;
    int tick, first_div = -1, mismatches = 0, i, k;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz);
    if (!b || fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
    fclose(f);

    if (memcmp(b, "GTRACE01", 8) != 0) { fprintf(stderr, "bad magic\n"); return 2; }
    n_ticks   = b[8]  | (b[9]  << 8) | ((u32)b[10] << 16) | ((u32)b[11] << 24);
    tick_size = b[12] | (b[13] << 8) | ((u32)b[14] << 16) | ((u32)b[15] << 24);
    if (tick_size != GT_TICK) {
        fprintf(stderr, "unexpected tick_size %lu (want %lu)\n",
                (unsigned long)tick_size, (unsigned long)GT_TICK);
        return 2;
    }

    /* Seed handler 0 with the synthesized script, exactly as the runtime populator
       would, then install the ISR (zeros the key table; DOS path elided here). */
    install_keyboard_isr();
    g_joystick_handler_table[0] = handler_script;

    /* Expand the script into a per-tick scancode list and replay. */
    tick = 0;
    for (k = 0; k < (int)(sizeof(TICK_SCRIPT) / sizeof(TICK_SCRIPT[0])); k++) {
        for (i = 0; i < TICK_SCRIPT[k].n; i++, tick++) {
            u8 sc = TICK_SCRIPT[k].sc;
            u8 got, want;

            if ((u32)tick >= n_ticks) { fprintf(stderr, "script longer than trace\n"); return 2; }

            /* Per-tick: clear input_state (clear TIMING owned by game_loop/Task 7),
               set the held key (0 others), poll. */
            input_state_clear();
            memset(g_key_state_table, 0, sizeof(g_key_state_table));
            if (sc != 0) { g_key_state_table[sc & 0x7f] = 1; }
            poll_input();
            got = input_state;

            want = b[GT_HEADER + (u32)tick * tick_size + GT_PLANES + GT_IS_OFFSET];
            if (got != want) {
                mismatches++;
                if (first_div < 0) {
                    first_div = tick;
                    printf("  DIVERGE tick %d: input_state got 0x%02x want 0x%02x (sc=0x%02x)\n",
                           tick, got, want, sc);
                }
            }
        }
    }

    if ((u32)tick != n_ticks) {
        fprintf(stderr, "script ticks %d != trace ticks %lu\n", tick, (unsigned long)n_ticks);
        return 2;
    }

    if (mismatches == 0) {
        printf("game_ctest: input_state %d/%d ticks match golden trace (PASS)\n", tick, tick);
        free(b);
        return 0;
    }
    printf("game_ctest: %d/%d ticks mismatch (first divergence tick %d) (FAIL)\n",
           mismatches, tick, first_div);
    free(b);
    return 1;
}
