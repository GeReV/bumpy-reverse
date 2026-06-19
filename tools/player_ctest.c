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

/* ── Task 6b: src/player.c now DEFINES game_mode_handlers, move_step_dispatch_tbl,
 * the handler bodies and the game-state globals.  We only stub the still-extern
 * TILE-COLLISION leaves + helper leaves (→ T6c) so the TU links.  These stubs
 * record that they were called, for the dispatch-wiring assertions below. */
u8         mode_script_tbl[64 * 4];   /* still forward-declared (populated elsewhere) */
u8 __far  *tilemap;                   /* level tilemap far pointer (→ T6c) */
u8         contact_transition_tbl[0x40];
u8         contact_transition_tbl_b[0x40];
u8         contact_action_tbl_left[0x40];
u8         collision_mode_table_right[0x40];
u8         down_action_lut[0x100];

/* Leaf-call trace: each stub bumps its counter so tests can assert routing. */
static int n_play_sound, n_step_walk_anim, n_read_tile_contact, n_read_tile_at_cell;
static int n_apply_contact_action, n_play_action_sound, n_play_walk_anim_default;
static int n_land_on_tile_below, n_begin_move;
static int n_p1_move_right, n_p1_move_left, n_p1_handle_move_input;
static int n_check_tile, n_fun_4802, n_walk_right_mode, n_walk_left_mode;

void play_sound(u8 id) { (void)id; n_play_sound++; }
void play_action_sound(void) { n_play_action_sound++; }
void apply_contact_action(u8 c) { (void)c; n_apply_contact_action++; }
void play_walk_anim_default(void) { n_play_walk_anim_default++; }
void step_walk_anim(u8 a, u8 p, u16 fo, u16 fs) { (void)a;(void)p;(void)fo;(void)fs; n_step_walk_anim++; }
void read_tile_layer_contact(u8 c) { (void)c; n_read_tile_contact++; }
void read_tile_at_cell(u8 c) { (void)c; n_read_tile_at_cell++; }
void p1_enter_walk_right_mode(void) { n_walk_right_mode++; }
void p1_enter_walk_left_mode(void) { n_walk_left_mode++; }
void p1_handle_move_input(void) { n_p1_handle_move_input++; }
void p1_move_right(void) { n_p1_move_right++; }
void p1_move_left(void) { n_p1_move_left++; }
void check_tile_below_ladder_or_land(void) { n_check_tile++; }
void p1_begin_move(u8 a) { (void)a; n_begin_move++; }
void land_on_tile_below(void) { n_land_on_tile_below++; }
void FUN_1000_4802(void) { n_fun_4802++; }
/* NOTE: move_down is a SCOPE handler DEFINED in player.c — not stubbed here. */

/* Out-of-scope handler-table targets (→ T6c) — host stubs so the table links. */
void move_walk_right_anim_step(void) { }
void enter_mode_0b_jump_start(void) { }
void move_anim_step_to_mode0c(void) { }
void move_step_check_walkable(void) { }
void move_step_dispatch_input(void) { }
void teleport_to_next_exit_tile(void) { }
void FUN_1000_22b0(void) { }
void p1_input_dispatch_bit10(void) { }
void FUN_1000_4437(void) { }
void FUN_1000_22c1(void) { }
void advance_physics_freeze(void) { }
void FUN_1000_1e3d(void) { }

#include "../src/player.c"

/* ── test harness ─────────────────────────────────────────────────────────── */
static int failures = 0;

/* Routing probe: handlers/step-slots are pointed at this to observe dispatch. */
static int g_routed;
static void route_probe(void) { g_routed = 1; }
static void step_noop(void) { }   /* safe filler for dispatch_move_step slots */

/* dispatch_move_step reads a pointer-sized value at move_step_dispatch_tbl +
   mode*0x22 + step*2 and calls it.  The real table holds 2-byte near offsets
   (not host-valid pointers), so before any test that reaches dispatch_move_step
   we overwrite the exact slot with a full host function pointer.
   (Writing sizeof(ptr) bytes at a 2-byte stride overlaps neighbours; harmless for
   isolated single-slot tests.)  `probe` selects an observing vs a no-op stub. */
static void install_step_slot(unsigned mode, unsigned step, void (*fn)(void))
{
    unsigned off = mode * 0x22 + step * 2;
    memcpy(&move_step_dispatch_tbl[off], &fn, sizeof(fn));
}

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

    /* ══ E. dispatch-table WIRING (Task 6b) ══════════════════════════════════
     * Assert game_mode_handlers[] routes game_mode → the correct handler and that
     * the p1_movement_dispatch override path bypasses the table.  We install test
     * stubs into table slots (the array is non-const) so we observe routing
     * without invoking the real tile-collision leaves.  dispatch_move_step() reads
     * move_step_dispatch_tbl raw near offsets (not host-valid), so any path that
     * reaches it gets a safe host pointer written into its exact slot first. */

    /* E1: game_mode_handlers index→handler map matches the dumped table (a few
       representative SCOPE entries + the default fill). */
    CHECK(game_mode_handlers[0x00] == gamemode_default_idle, "E1 idx0x00->idle");
    CHECK(game_mode_handlers[0x03] == gamemode_03_move,      "E1 idx0x03->move");
    CHECK(game_mode_handlers[0x0f] == gamemode_03_move,      "E1 idx0x0f->move");
    CHECK(game_mode_handlers[0x21] == gamemode_21_start,     "E1 idx0x21->start");
    CHECK(game_mode_handlers[0x22] == gamemode_22,           "E1 idx0x22->22");
    CHECK(game_mode_handlers[0x23] == gamemode_23_walk,      "E1 idx0x23->walk_r");
    CHECK(game_mode_handlers[0x24] == gamemode_24_walk,      "E1 idx0x24->walk_l");
    CHECK(game_mode_handlers[0x25] == gamemode_25_contact,   "E1 idx0x25->contact_l");
    CHECK(game_mode_handlers[0x26] == gamemode_26_contact,   "E1 idx0x26->contact_r");
    CHECK(game_mode_handlers[0x27] == gamemode_default_idle, "E1 idx0x27->idle(fill)");
    CHECK(game_mode_handlers[0x3f] == gamemode_default_idle, "E1 idx0x3f->idle(fill)");

    /* E2: p1_movement_dispatch routes through game_mode_handlers[game_mode].
       Install a recording stub into a normally-idle slot and confirm it fires,
       and that prev_game_mode / p1_queued_action_code are set per the spine. */
    {
        void (*saved)(void) = game_mode_handlers[0x11];
        g_routed = 0;
        game_mode_handlers[0x11] = route_probe;
        reset_state();
        game_mode = 0x11;
        prev_game_mode = 0x99;
        p1_queued_action_code = 0x55;
        physics_frozen = 1;            /* force the table path (not the override) */
        move_override = 1;
        p1_movement_dispatch();
        CHECK(g_routed == 1, "E2 dispatch routed to game_mode_handlers[0x11]");
        CHECK(prev_game_mode == 0x11, "E2 prev_game_mode saved: got 0x%x", prev_game_mode);
        CHECK(p1_queued_action_code == 0, "E2 queued action cleared: got 0x%x",
              p1_queued_action_code);
        game_mode_handlers[0x11] = saved;
    }

    /* E3: the override path — physics_frozen==0 && move_override!=0 → move_settle,
       NOT the table.  Probe by routing the current-mode slot AND confirming the
       move_settle land path ran instead.  move_settle ends in dispatch_move_step,
       so pre-load a safe host stub into that slot. */
    {
        void (*saved)(void) = game_mode_handlers[0x10];
        g_routed = 0;
        n_land_on_tile_below = 0;
        game_mode_handlers[0x10] = route_probe;
        reset_state();
        game_mode = 0x10;              /* within the reproduced move_step rows (0..0x11) */
        physics_frozen = 0;
        move_override = 1;
        p1_pending_action = 0;         /* != 0x11 -> land_on_tile_below path */
        install_step_slot(0x10, 0, step_noop);   /* safe stub for dispatch_move_step */
        p1_movement_dispatch();
        CHECK(g_routed == 0, "E3 override bypassed game_mode_handlers");
        CHECK(n_land_on_tile_below == 1, "E3 override ran move_settle->land");
        CHECK(move_override == 0, "E3 move_settle cleared override");
        game_mode_handlers[0x10] = saved;
    }

    /* E4: a real SCOPE handler with NO tile dependency — gamemode_22 with
       p1_contact_code != 8 just sets game_mode = 0x23 (pure transition). */
    {
        reset_state();
        game_mode = 0x22;
        p1_contact_code = 0;
        gamemode_22();
        CHECK(game_mode == 0x23, "E4 gamemode_22(no-contact) -> mode 0x23: got 0x%x",
              game_mode);
    }

    /* E5: gamemode_23_walk with right NOT held — advances walk anim, no walk-begin.
       With right held (input_state&2) it begins a rightward walk; that path reaches
       dispatch_move_step, so pre-install a safe step slot for game_mode 1. */
    {
        reset_state();
        n_step_walk_anim = 0; n_apply_contact_action = 0;
        game_mode = 0x23;
        input_state = 0;
        gamemode_23_walk();
        CHECK(n_step_walk_anim == 1, "E5a walk anim advanced (idle)");
        reset_state();
        game_mode = 0x23;
        input_state = 2;               /* right held */
        install_step_slot(1, 9, step_noop);   /* p1_begin_walk_right uses step_idx 9, mode 1 */
        gamemode_23_walk();
        CHECK(game_mode == 1, "E5b right-held -> begin_walk_right set mode 1: got 0x%x",
              game_mode);
        CHECK(p1_facing_left == 9, "E5b facing_left=9 per engine literal");
        CHECK(n_apply_contact_action == 1, "E5b apply_contact_action(0x16) fired");
    }

    /* E6: dispatch_move_step index arithmetic lands on the exact table slot.
       Install a probe at [mode=2][step=3] and confirm dispatch calls it. */
    {
        g_routed = 0;
        install_step_slot(2, 3, route_probe);
        reset_state();
        game_mode = 2;
        p1_move_step_idx = 3;
        dispatch_move_step();
        CHECK(g_routed == 1, "E6 dispatch_move_step[2][3] routed to probe");
    }

    if (failures == 0) {
        printf("PASS: p1_step_scripted_move spine (right/left/guards) + Task 6b "
               "dispatch wiring (game_mode_handlers index map, p1_movement_dispatch "
               "table/override routing, dispatch_move_step slot arithmetic, "
               "gamemode_22/23 handler routing) [synthetic move-script]\n");
        return 0;
    }
    printf("FAIL: %d assertion(s) failed\n", failures);
    return 1;
}
