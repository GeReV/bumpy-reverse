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
/* MK_FP host model: a 1 MB+ linear "far memory" shadow indexed by the real-mode
   linear address (seg<<4 + off).  Task 6a/6b only needed MK_FP to compile (the
   spine tests never dereferenced a built far pointer); Task 6c DOES drive
   enter_game_mode for many modes, which loads mode_script_tbl[mode] as a far
   pointer and dereferences script[0..5].  Backing MK_FP with this shadow makes
   those reads land in valid (zeroed) host memory instead of a near-null deref. */
static unsigned char far_mem[0x110000];   /* covers any seg<<4+off (seg,off 16-bit) */
#define MK_FP(seg, off) ((void *)(far_mem + (((u32)(seg) << 4) + (u16)(off))))

/* input_state is normally owned by input.c; provide a host definition. */
u8 input_state;

/* ── Task 6b/6c: src/player.c now DEFINES game_mode_handlers, move_step_dispatch_tbl,
 * the handler bodies, the game-state globals, AND (6c) the tile-collision leaves +
 * the contact/collision/action DATA tables.  We provide a host backing for the
 * cross-module `tilemap` far pointer (level data) and still-extern helper leaves +
 * the two boundary leaves deferred to T7 (land/ladder).  These stubs record that
 * they were called, for the dispatch-wiring + routing assertions below. */
u8         mode_script_tbl[64 * 4];   /* still forward-declared (populated elsewhere) */

/* tilemap is the cross-module level data far pointer (owned by level.c → T7).  The
 * host points it at a synthetic SYNTH_TILES byte array; on the host __far is erased
 * so `u8 *` and `u8 __far *` are the same.  read_tile_layer_contact reads
 * tilemap[cell+0x30], so the backing array must cover cell+0x30. */
#define TILEMAP_SIZE 0x200
static u8 synth_tilemap[TILEMAP_SIZE];
u8 __far  *tilemap = synth_tilemap;

/* Leaf-call trace: each stub bumps its counter so tests can assert routing. */
static int n_play_sound, n_step_walk_anim;
static int n_play_action_sound, n_play_walk_anim_default;
static int n_fun_4802;

void play_sound(u8 id) { (void)id; n_play_sound++; }
void play_action_sound(void) { n_play_action_sound++; }
/* apply_contact_action (1000:6a89) is now RECONSTRUCTED in src/player.c (Phase-9 T1)
   and pulled in via the #include below — no host stub (would be a dup symbol).  The
   E5b routing check asserts it fired by observing anim_b_loop_idx (last_contact_action
   @ DGROUP 0x8566), which the real fn latches to its action arg on entry. */
void play_walk_anim_default(void) { n_play_walk_anim_default++; }
void step_walk_anim(u8 a, u8 p, u16 fo, u16 fs) { (void)a;(void)p;(void)fo;(void)fs; n_step_walk_anim++; }
void FUN_1000_4802(void) { n_fun_4802++; }
/* Phase-2 T3: land_on_tile_below / check_tile_below_ladder_or_land are DEFINED in
 * player.c.  Phase-2 T4: the move-step substates + their two delegates
 * (p1_exec_pending_action / move_down_step) and the settle wrappers
 * (run_physics_settle_wrap / FUN_1000_22b0) are now DEFINED in player.c too — no
 * longer stubbed here.  Only the FX allocator remains a stub. */
void apply_cell_animation(u8 fx) { (void)fx; }
/* run_physics_settle (player.c) reads these cross-module DGROUP bytes (game.c). */
u8 session_continue_flag, frame_abort_flag, settle_countdown;
/* NOTE: move_down, p1_move_left/right, p1_handle_move_input, read_tile_*,
 * p1_enter_walk_*_mode, p1_begin_move, exec_move_action and the *_step_resolve /
 * *_walk_contact leaves are now SCOPE functions DEFINED in player.c (6c) — NOT
 * stubbed here. */

/* Out-of-scope handler-table targets (→ T6c) — host stubs so the table links. */
void move_walk_right_anim_step(void) { }
void enter_mode_0b_jump_start(void) { }
void move_anim_step_to_mode0c(void) { }
void move_step_check_walkable(void) { }
void move_step_dispatch_input(void) { }
void teleport_to_next_exit_tile(void) { }
void p1_input_dispatch_bit10(void) { }
void FUN_1000_4437(void) { }
void advance_physics_freeze(void) { }
void FUN_1000_1e3d(void) { }

/* channel-B anim globals apply_contact_action (player.c, Phase-9 T1) references
   (OWNED by anim.c in the real build; defined here for this host harness).  anim.h
   is pulled in by player.c; its include guard makes a re-include a no-op. */
#include "../src/anim.h"
anim_chan_rec       anim_b_records[ANIM_B_SLOTS];
anim_chan_rec       anim_b_terminator = { 0xff, 0, 0, 0, 0, 0, 0, 0 };
anim_chan_rec __far *anim_channels_b_tbl[ANIM_B_SLOTS + 1];
u8                  anim_b_loop_idx;   /* DGROUP 0x8566 = last_contact_action alias */

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
    /* wire the channel-B slot table (4 free slots + 0xFF terminator) so the real
       apply_contact_action (player.c, Phase-9 T1) can claim a slot without crashing. */
    {
        int i;
        memset(anim_b_records, 0, sizeof(anim_b_records));
        for (i = 0; i < ANIM_B_SLOTS; i++) anim_channels_b_tbl[i] = &anim_b_records[i];
        anim_b_terminator.active = 0xff;
        anim_channels_b_tbl[ANIM_B_SLOTS] = &anim_b_terminator;
        anim_b_loop_idx = 0;
    }
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
       so pre-load a safe host stub into that slot.

       Phase-2 T3: land_on_tile_below is now the REAL ported leaf (no counter stub).
       With p1_cell == 0 (< 8) its faithful path is enter_game_mode(6), so we assert
       the OBSERVABLE land result (game_mode == 6) instead of a stub-call counter. */
    {
        void (*saved)(void) = game_mode_handlers[0x10];
        g_routed = 0;
        game_mode_handlers[0x10] = route_probe;
        reset_state();
        game_mode = 0x10;              /* within the reproduced move_step rows (0..0x11) */
        physics_frozen = 0;
        move_override = 1;
        p1_cell = 0;                   /* < 8 -> land_on_tile_below enters mode 6 */
        p1_pending_action = 0;         /* != 0x11 -> land_on_tile_below path */
        /* move_settle -> land_on_tile_below sets game_mode=6, THEN dispatch_move_step
           runs on mode 6's slot — install a safe host stub there (not mode 0x10). */
        install_step_slot(6, 0, step_noop);
        p1_movement_dispatch();
        CHECK(g_routed == 0, "E3 override bypassed game_mode_handlers");
        CHECK(game_mode == 6, "E3 override ran move_settle->land (mode->6): got 0x%x",
              game_mode);
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
        n_step_walk_anim = 0;
        game_mode = 0x23;
        input_state = 0;
        gamemode_23_walk();
        CHECK(n_step_walk_anim == 1, "E5a walk anim advanced (idle)");
        reset_state();
        game_mode = 0x23;
        input_state = 2;               /* right held */
        anim_b_loop_idx = 0;
        install_step_slot(1, 9, step_noop);   /* p1_begin_walk_right uses step_idx 9, mode 1 */
        gamemode_23_walk();
        CHECK(game_mode == 1, "E5b right-held -> begin_walk_right set mode 1: got 0x%x",
              game_mode);
        CHECK(p1_facing_left == 9, "E5b facing_left=9 per engine literal");
        /* the real apply_contact_action latches its action arg into last_contact_action
           (anim_b_loop_idx @ 0x8566) on entry; p1_begin_walk_right calls it with 0x16. */
        CHECK(anim_b_loop_idx == 0x16, "E5b apply_contact_action(0x16) fired: got 0x%x",
              anim_b_loop_idx);
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

    /* ══ F. TILE-COLLISION cell-resolution (Task 6c) ═════════════════════════
     * Build a SYNTHETIC tilemap and drive the ported tile leaves with a set
     * p1_cell, asserting p1_contact_code / p1_current_tile / resolved game_mode
     * match the decomp logic AND the dumped contact/collision tables.
     *
     * read_tile_layer_contact reads tilemap[cell+0x30]; read_tile_at_cell reads
     * tilemap[cell].  The resolvers end in dispatch_move_step, so we install safe
     * step slots for any game_mode they can enter before calling them. */

    /* Install safe step-slots for EVERY mode (the dispatch table is the full 0x40
       rows after the 6c bound-correction), so the resolvers' tail dispatch_move_step
       never jumps to a raw near offset regardless of which mode they enter. */
    {
        unsigned m;
        for (m = 0; m < 0x40; m++) {
            install_step_slot(m, 0, step_noop);
        }
    }
    /* enter_game_mode reads mode_script_tbl[mode*4] far ptr for modes not in
       {5,0xb,0x1c}; the host MK_FP yields a host pointer, and script[0/1] reads
       index into the zeroed mode_script_tbl backing — harmless (sets steps/facing
       to 0).  We only assert game_mode, which enter_game_mode sets directly. */

    /* F1: read_tile_layer_contact — p1_contact_code = tilemap[cell+0x30]. */
    {
        memset(synth_tilemap, 0, TILEMAP_SIZE);
        synth_tilemap[0x10 + 0x30] = 0x07;   /* contact-layer byte for cell 0x10 */
        p1_contact_code = 0xaa;
        read_tile_layer_contact(0x10);
        CHECK(p1_contact_code == 0x07,
              "F1 read_tile_layer_contact: got 0x%02x want 0x07", p1_contact_code);
    }

    /* F2: read_tile_at_cell — p1_current_tile = tilemap[cell]. */
    {
        memset(synth_tilemap, 0, TILEMAP_SIZE);
        synth_tilemap[0x0b] = 0x0b;          /* teleport tile at cell 0x0b */
        p1_current_tile = 0xaa;
        read_tile_at_cell(0x0b);
        CHECK(p1_current_tile == 0x0b,
              "F2 read_tile_at_cell: got 0x%02x want 0x0b", p1_current_tile);
    }

    /* F3: move_left (6b) routes contact_code -> contact_action_tbl_left -> mode.
       Set the cell's contact-layer byte so read_tile_layer_contact latches a
       contact code, then assert move_left enters contact_action_tbl_left[code].
       contact_code 1 in that table maps to mode 1 (then teleport-tile check). */
    {
        memset(synth_tilemap, 0, TILEMAP_SIZE);
        reset_state();
        move_step_count = 1;                  /* != 0, take the resolve path */
        p1_cell = 0x20;
        /* read_tile_layer_contact(cell-1=0x1f) reads tilemap[0x1f+0x30]. */
        synth_tilemap[0x1f + 0x30] = 0x08;    /* contact code 0x08 */
        /* contact_action_tbl_left[0x08] == 0x21 (from the dumped table). */
        synth_tilemap[0x1f] = 0x00;           /* base tile not 0x0b */
        move_left();
        CHECK(contact_action_tbl_left[0x08] == 0x21, "F3 table[0x08]==0x21");
        CHECK(game_mode == 0x21,
              "F3 move_left contact8 -> mode 0x21: got 0x%02x", game_mode);
        CHECK(p1_cell_prev == 0x1f, "F3 cell_prev = cell-1: got 0x%02x", p1_cell_prev);
    }

    /* F4: move_left contact code 1 -> table maps to 1 -> teleport-tile probe.
       contact_action_tbl_left[1] == 0x12 (NOT 1), so no teleport branch; assert
       the table-driven mode is taken directly. */
    {
        memset(synth_tilemap, 0, TILEMAP_SIZE);
        reset_state();
        move_step_count = 1;
        p1_cell = 0x20;
        synth_tilemap[0x1f + 0x30] = 0x01;    /* contact code 1 */
        move_left();
        CHECK(contact_action_tbl_left[0x01] == 0x12, "F4 table[1]==0x12");
        CHECK(game_mode == 0x12,
              "F4 move_left contact1 -> mode 0x12: got 0x%02x", game_mode);
    }

    /* F5: move_right routes via collision_mode_table_right.  contact code 0 ->
       table[0]==0x02 -> resolved_mode 2 -> teleport probe at cell+1.  With base
       tile 0x0b at cell+1, mode becomes 0x17; else 2. */
    {
        memset(synth_tilemap, 0, TILEMAP_SIZE);
        reset_state();
        move_step_count = 1;                  /* != 7, resolve path */
        p1_cell = 0x20;
        synth_tilemap[0x20 + 0x30] = 0x00;    /* contact code 0 */
        synth_tilemap[0x21] = 0x0b;           /* teleport tile at cell+1 */
        move_right();
        CHECK(collision_mode_table_right[0x00] == 0x02, "F5 table[0]==0x02");
        CHECK(p1_current_tile == 0x0b, "F5 read cell+1 tile == 0x0b");
        CHECK(game_mode == 0x17,
              "F5 move_right code0+teleport -> mode 0x17: got 0x%02x", game_mode);
    }

    /* F6: move_right same contact, base tile NOT 0x0b -> resolved mode stays 2. */
    {
        memset(synth_tilemap, 0, TILEMAP_SIZE);
        reset_state();
        move_step_count = 1;
        p1_cell = 0x20;
        synth_tilemap[0x20 + 0x30] = 0x00;
        synth_tilemap[0x21] = 0x05;           /* not the teleport tile */
        move_right();
        CHECK(game_mode == 0x02,
              "F6 move_right code0 no-teleport -> mode 0x02: got 0x%02x", game_mode);
    }

    /* F7: p1_enter_walk_right_mode probes cell+1 -> mode 0x2a (tile 0x0b) / 0x26. */
    {
        memset(synth_tilemap, 0, TILEMAP_SIZE);
        reset_state();
        p1_cell = 0x30;
        synth_tilemap[0x31] = 0x0b;
        p1_enter_walk_right_mode();
        CHECK(game_mode == 0x2a, "F7a tile0x0b -> mode 0x2a: got 0x%02x", game_mode);
        memset(synth_tilemap, 0, TILEMAP_SIZE);
        reset_state();
        p1_cell = 0x30;
        synth_tilemap[0x31] = 0x03;
        p1_enter_walk_right_mode();
        CHECK(game_mode == 0x26, "F7b tile!=0x0b -> mode 0x26: got 0x%02x", game_mode);
    }

    /* F8: p1_enter_walk_left_mode probes cell-1 -> mode 0x29 (tile 0x0b) / 0x25. */
    {
        memset(synth_tilemap, 0, TILEMAP_SIZE);
        reset_state();
        p1_cell = 0x30;
        synth_tilemap[0x2f] = 0x0b;
        p1_enter_walk_left_mode();
        CHECK(game_mode == 0x29, "F8a tile0x0b -> mode 0x29: got 0x%02x", game_mode);
        memset(synth_tilemap, 0, TILEMAP_SIZE);
        reset_state();
        p1_cell = 0x30;
        synth_tilemap[0x2f] = 0x03;
        p1_enter_walk_left_mode();
        CHECK(game_mode == 0x25, "F8b tile!=0x0b -> mode 0x25: got 0x%02x", game_mode);
    }

    /* F9: exec_move_action dispatch — action 0->move_down, 1->move_left,
       2->move_right, 3->move_settle, default->p1_begin_move(action).  Probe the
       default arm: action 0x07 (not a special) enters mode 0x07 via p1_begin_move
       (= enter_game_mode(7)+dispatch).  install step slot for mode 7. */
    {
        reset_state();
        install_step_slot(0x07, 0, step_noop);
        exec_move_action(0x07);
        CHECK(game_mode == 0x07,
              "F9 exec_move_action(0x07) -> p1_begin_move mode 7: got 0x%02x",
              game_mode);
    }

    /* F10: p1_move_left maps pending action via action_tbl_left then exec_move_action.
       action_tbl_left[0x12] == 0x10 (from the dump) -> exec_move_action(0x10) ->
       default arm -> p1_begin_move(0x10) -> mode 0x10. */
    {
        reset_state();
        install_step_slot(0x10, 0, step_noop);
        p1_pending_action = 0x12;
        CHECK(action_tbl_left[0x12] == 0x10, "F10 action_tbl_left[0x12]==0x10");
        p1_move_left();
        CHECK(game_mode == 0x10,
              "F10 p1_move_left(pending0x12) -> mode 0x10: got 0x%02x", game_mode);
    }

    /* F11: move_down (6b) indexes down_action_lut[tilemap[cell-8]] (6c table).
       tile-below value 0x05 -> down_action_lut[0x05]==0x11 -> p1_begin_move(0x11). */
    {
        memset(synth_tilemap, 0, TILEMAP_SIZE);
        reset_state();
        install_step_slot(0x11, 0, step_noop);
        p1_cell = 0x40;
        synth_tilemap[0x40 - 8] = 0x05;       /* tile below */
        CHECK(down_action_lut[0x05] == 0x11, "F11 down_action_lut[5]==0x11");
        move_down();
        CHECK(game_mode == 0x11,
              "F11 move_down tile5 -> action 0x11 -> mode 0x11: got 0x%02x",
              game_mode);
    }

    /* F12: p1_resolve_walk_right_contact — step!=7, contact code routes the
       right_walk_contact_tbl_39 table.  With contact code whose table entry ==0x39
       it forces mode 0x39.  contact code 1 -> table[1]==0x39. */
    {
        memset(synth_tilemap, 0, TILEMAP_SIZE);
        reset_state();
        move_step_count = 1;                  /* != 7 */
        p1_cell = 0x20;
        synth_tilemap[0x20 + 0x30] = 0x01;    /* contact code 1 */
        CHECK(right_walk_contact_tbl_39[0x01] == 0x39, "F12 tbl39[1]==0x39");
        p1_resolve_walk_right_contact();
        CHECK(game_mode == 0x39,
              "F12 resolve_walk_right contact1 -> mode 0x39: got 0x%02x", game_mode);
    }

    if (failures == 0) {
        printf("PASS: p1_step_scripted_move spine (right/left/guards) + Task 6b "
               "dispatch wiring (game_mode_handlers index map, p1_movement_dispatch "
               "table/override routing, dispatch_move_step slot arithmetic, "
               "gamemode_22/23 handler routing) + Task 6c tile-collision "
               "(read_tile_layer_contact/read_tile_at_cell tilemap reads, "
               "move_left/move_right collision-table routing + teleport-tile probe, "
               "p1_enter_walk_{left,right}_mode, exec_move_action dispatch, "
               "p1_move_left action LUT, move_down down_action_lut, "
               "p1_resolve_walk_right_contact walk-contact table) "
               "[synthetic tilemap + dumped-real tables]\n");
        return 0;
    }
    printf("FAIL: %d assertion(s) failed\n", failures);
    return 1;
}
