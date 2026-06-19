/* Host unit test for src/player.c — the P1 MOVE-EXECUTION SPINE (Phase 1, Task 6a).
 *
 * Compiles the REAL port (src/player.c) on the host with the Watcom 16-bit
 * environment shimmed out (__far/__huge erased, exact-width typedefs, BUMPY_H
 * defined so player.h does not pull in <dos.h>), then drives the core move-step
 * executor p1_step_scripted_move() over a known [anim,dx,dy] move script and
 * asserts that p1_pixel_x/y, p1_move_anim, p1_move_steps_left, p1_move_step_idx
 * and the return value evolve EXACTLY per the script semantics — including the
 * facing-left dx-negation and all three no-op guards (move_locked,
 * steps_left==0, game_mode in {5,0xb,0x1c}).
 *
 * Build/run (also wrapped by tools/validate_player.sh):
 *     cc -O2 -o /tmp/player_ctest tools/player_ctest.c && /tmp/player_ctest
 * Exit 0 iff every assertion passes.
 *
 * MOVE-SCRIPT SOURCE: SYNTHETIC.  The engine's real move scripts are reached via
 * mode_script_tbl (DGROUP 0x2252) far pointers that are POPULATED AT RUNTIME — a
 * bounded probe of the unpacked-image DGROUP (local/build/unpack/
 * bumpy_unpacked.exe.fullmem, DG_LIN=0x114b0) showed the table holds scattered
 * uninitialised far pointers whose targets are zero (not statically resolvable),
 * so per the Task-6a brief we fall back to a documented synthetic [anim,dx,dy]
 * script.  The function LOGIC under test is identical regardless of script source.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── shim the Watcom 16-bit environment for host compilation ─────────────────── */
#define BUMPY_H            /* player.h's #include "bumpy.h" becomes a no-op */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define __far
#define __huge
/* player.c never executes the MK_FP path in this test (enter_game_mode is not
   driven), but the symbol must exist for the translation unit to compile. */
#define MK_FP(seg, off) ((void *)(uintptr_t)(((u32)(seg) << 4) + (u16)(off)))

/* input_state is normally owned by input.c; provide a host definition. */
u8 input_state;

/* Forward-declared (Task 6b) externs that src/player.c references — host stubs so
   the translation unit links.  p1_step_scripted_move() never calls through these. */
void (*game_mode_handlers[64])(void);
u8 move_step_dispatch_tbl[64 * 0x22];
u8 mode_script_tbl[64 * 4];
void move_settle(void) { }

#include "../src/player.c"

/* ── test harness ─────────────────────────────────────────────────────────── */
static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* Reset all spine globals to a known idle state. */
static void reset_state(void)
{
    p1_pixel_x = 0; p1_pixel_y = 0; p1_move_anim = 0;
    game_mode = 0; prev_game_mode = 0; p1_move_step_idx = 0;
    move_locked = 0; p1_move_steps_left = 0; p1_facing_left = 0;
    p1_move_anim_frame_idx = 0; p1_queued_action_code = 0;
    physics_frozen = 0; move_override = 0; p1_move_script = NULL;
}

int main(void)
{
    /* Synthetic 3-step move script: [anim, dx, dy] per 6-byte entry.
       Chosen with distinct, signed, non-trivial deltas to exercise +x/-x/+y/-y. */
    static u16 script[] = {
        /* anim   dx      dy   */
        10,       3,      0,        /* step 1: +3x,  0y, anim 10 */
        11,       4,    (u16)-2,    /* step 2: +4x, -2y, anim 11 */
        12,     (u16)-1,  5         /* step 3: -1x, +5y, anim 12 */
    };
    char r;

    printf("player_ctest: p1_step_scripted_move spine differential\n");

    /* ── A. facing RIGHT: dx applied as-is, full 3-step run ─────────────────── */
    reset_state();
    p1_facing_left = 0;
    p1_move_steps_left = 3;
    game_mode = 0x21;                 /* a normal (non-skipped) move mode */
    p1_pixel_x = 100; p1_pixel_y = 50;
    p1_move_script = script;

    /* step 1 */
    r = p1_step_scripted_move();
    CHECK(p1_move_anim == 10, "A1 anim: got %u want 10", p1_move_anim);
    CHECK(p1_pixel_x == 103, "A1 px: got %d want 103", p1_pixel_x);
    CHECK(p1_pixel_y == 50,  "A1 py: got %d want 50", p1_pixel_y);
    CHECK(p1_move_steps_left == 2, "A1 steps: got %u want 2", p1_move_steps_left);
    CHECK(p1_move_step_idx == 1, "A1 idx: got %u want 1", p1_move_step_idx);
    CHECK(r == 1, "A1 ret: got %d want 1", r);
    CHECK(p1_move_script == script + 3, "A1 script advance");

    /* step 2 */
    r = p1_step_scripted_move();
    CHECK(p1_move_anim == 11, "A2 anim: got %u want 11", p1_move_anim);
    CHECK(p1_pixel_x == 107, "A2 px: got %d want 107", p1_pixel_x);
    CHECK(p1_pixel_y == 48,  "A2 py: got %d want 48", p1_pixel_y);
    CHECK(p1_move_steps_left == 1, "A2 steps: got %u want 1", p1_move_steps_left);
    CHECK(p1_move_step_idx == 2, "A2 idx: got %u want 2", p1_move_step_idx);
    CHECK(r == 2, "A2 ret: got %d want 2", r);

    /* step 3 — finishes: returns 0 and resets step_idx */
    r = p1_step_scripted_move();
    CHECK(p1_move_anim == 12, "A3 anim: got %u want 12", p1_move_anim);
    CHECK(p1_pixel_x == 106, "A3 px: got %d want 106", p1_pixel_x);
    CHECK(p1_pixel_y == 53,  "A3 py: got %d want 53", p1_pixel_y);
    CHECK(p1_move_steps_left == 0, "A3 steps: got %u want 0", p1_move_steps_left);
    CHECK(p1_move_step_idx == 0, "A3 idx (reset): got %u want 0", p1_move_step_idx);
    CHECK(r == 0, "A3 ret (finished): got %d want 0", r);

    /* after finish, steps_left==0 guard makes a further call a no-op */
    p1_pixel_x = 999;
    r = p1_step_scripted_move();
    CHECK(p1_pixel_x == 999, "A4 steps==0 guard: px changed (got %d)", p1_pixel_x);
    CHECK(r == 0, "A4 steps==0 guard ret: got %d want 0", r);

    /* ── B. facing LEFT: dx negated ─────────────────────────────────────────── */
    reset_state();
    p1_facing_left = 1;
    p1_move_steps_left = 3;
    game_mode = 0x21;
    p1_pixel_x = 100; p1_pixel_y = 50;
    p1_move_script = script;

    r = p1_step_scripted_move();               /* dx=+3 -> negated -> -3 */
    CHECK(p1_pixel_x == 97, "B1 px (neg): got %d want 97", p1_pixel_x);
    CHECK(p1_pixel_y == 50, "B1 py: got %d want 50", p1_pixel_y);
    r = p1_step_scripted_move();               /* dx=+4 -> -4 */
    CHECK(p1_pixel_x == 93, "B2 px (neg): got %d want 93", p1_pixel_x);
    CHECK(p1_pixel_y == 48, "B2 py: got %d want 48", p1_pixel_y);
    r = p1_step_scripted_move();               /* dx=-1 -> +1 */
    CHECK(p1_pixel_x == 94, "B3 px (neg): got %d want 94", p1_pixel_x);
    CHECK(p1_pixel_y == 53, "B3 py: got %d want 53", p1_pixel_y);

    /* ── C. move_locked guard: no-op, returns move_locked ───────────────────── */
    reset_state();
    move_locked = 7;
    p1_move_steps_left = 3;
    game_mode = 0x21;
    p1_pixel_x = 200; p1_pixel_y = 60;
    p1_move_script = script;
    r = p1_step_scripted_move();
    CHECK(p1_pixel_x == 200 && p1_pixel_y == 60, "C px/py unchanged: %d,%d",
          p1_pixel_x, p1_pixel_y);
    CHECK(p1_move_steps_left == 3, "C steps unchanged: got %u", p1_move_steps_left);
    CHECK(p1_move_script == script, "C script not advanced");
    CHECK((u8)r == 7, "C ret==move_locked: got %d want 7", (u8)r);

    /* ── D. game_mode skip guard for each of {5, 0xb, 0x1c} ──────────────────── */
    {
        u8 skip_modes[3] = { 0x05, 0x0b, 0x1c };
        int i;
        for (i = 0; i < 3; i++) {
            reset_state();
            p1_move_steps_left = 3;
            game_mode = skip_modes[i];
            p1_pixel_x = 300; p1_pixel_y = 70;
            p1_move_script = script;
            r = p1_step_scripted_move();
            CHECK(p1_pixel_x == 300 && p1_pixel_y == 70,
                  "D mode 0x%02x px/py unchanged: %d,%d", skip_modes[i],
                  p1_pixel_x, p1_pixel_y);
            CHECK(p1_move_steps_left == 3, "D mode 0x%02x steps unchanged: %u",
                  skip_modes[i], p1_move_steps_left);
            CHECK(r == 0, "D mode 0x%02x ret==move_locked(0): got %d", skip_modes[i], r);
        }
    }

    if (failures == 0) {
        printf("PASS: p1_step_scripted_move — 3-step run (facing right & left), "
               "steps==0 / move_locked / game_mode in {5,0xb,0x1c} guards "
               "[synthetic move-script]\n");
        return 0;
    }
    printf("FAIL: %d assertion(s) failed\n", failures);
    return 1;
}
