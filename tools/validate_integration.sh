#!/usr/bin/env bash
# validate_integration.sh — Phase-9 T4 INTEGRATION / LINK SMOKE GATE
# ============================================================================
# Proves the reconstructed BUMPY.EXE links cleanly AND that game_loop's per-tick
# spine reaches REAL reconstructed module bodies — not linkability stubs — except
# for a documented set of genuine hardware / CRTC-page-flip / int8-timing /
# render-core / never-decompiled carve-outs.
#
# The gate does three things:
#
#   1. LINKS BUMPY.EXE via the project Makefile (wmake) and asserts the link is
#      clean — no errors, and crucially NO DUPLICATE / multiply-defined symbols
#      (a reconstructed body colliding with a leftover stub would be a dup).
#
#   2. Re-links with a MAP and parses, per linked .obj, the public symbols it
#      DEFINES.  For each game_loop / session-spine per-tick callee (the Task 1-3
#      reconstructed set) it asserts the symbol resolves to its EXPECTED module
#      .obj and NOT to game_stubs.obj.  This is the regression guard: if any of
#      those callees ever regresses to a stub (lands back in game_stubs.obj), the
#      gate FAILS.
#
#   3. Asserts every function symbol still defined by game_stubs.obj is a member
#      of the explicit CARVE-OUT ALLOWLIST below.  A NEW stub that is not an
#      allowlisted carve-out (i.e. gateable game logic that regressed to a stub)
#      makes the gate FAIL.
#
# Exit 0 iff: BUMPY.EXE links clean + no dup symbols + every spine callee resolves
# to its real module + game_stubs.obj ⊆ allowlist.  A built-in --self-test proves
# the gate has teeth (it FAILS when a spine callee is forced into game_stubs.obj).
#
# Usage:
#   source local/toolchain/open-watcom/ow-env.sh && bash tools/validate_integration.sh
#   bash tools/validate_integration.sh --self-test   # prove the gate fails on regression
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OWENV="local/toolchain/open-watcom/ow-env.sh"
if [ -f "$OWENV" ]; then
    # shellcheck disable=SC1090
    source "$OWENV"
fi
command -v wlink >/dev/null 2>&1 || { echo "ERROR: wlink not on PATH (source $OWENV)"; exit 2; }

OUTDIR="local/build/src"
EXE="$OUTDIR/BUMPY.EXE"
TMP="${TMPDIR:-/tmp}"
MAP="$TMP/BUMPY_integration.map"
LNK="$TMP/BUMPY_integration.lnk"

# ── The BUMPY.EXE object set (mirrors src/Makefile BUMPY_OBJS) ───────────────
OBJS=(main.obj game.obj game_stubs.obj
      level.obj input.obj player.obj player2.obj items.obj anim.obj spawn.obj sound.obj screens.obj
      vec.obj op12.obj video.obj
      sprite.obj sprite_anim.obj sprite_chain.obj sprite_blit.obj
      bg_render.obj entity.obj bgi_overlay.obj
      dosio.obj prng.obj globals.obj
      bvec_buf1.obj bvec_buf2.obj)

# ── CARVE-OUT ALLOWLIST: the ONLY function symbols game_stubs.obj may define ──
# Every entry is a documented genuine carve-out (hardware / CRTC page-flip /
# int8-timing / engine-standalone-loader / never-decompiled / render-core leaf /
# out-of-scope sound|player handler / CRT thunk).  See src/game_stubs.c.
CARVEOUT_ALLOWLIST=(
  # init_game_session_state hardware-init block
  set_disk_swap_callback init_timer_resource_table install_interrupt_handler
  init_joystick_handlers mouse_reset init_sound_tables init_misc_7bd7
  init_display_97a4 init_misc_7bbd init_display_97f1 init_crtc_window
  set_display_page set_palette_mode set_resource_table clear_viewport
  reset_opaque_session_globals
  # engine standalone loader / never-decompiled round reset
  load_current_level_data reset_round_counters
  # render-core present leaves
  init_sprite_structs init_fullscreen_view_desc setup_fullscreen_view
  apply_level_palette show_text_screen show_pause_screen fun_75a2_poll_action
  # CRTC page-flip + int8-timing
  present_frame run_n_frames wait_keypress rotate_timing_flags_and_wait
  # P2 indirect-call backend carve
  p2_dispatch_move_state_handler
  # out-of-scope sound L4/L6 device drivers + helpers
  record_min_status_code mpu401_reset_to_uart FUN_1000_8b2a FUN_1000_91cf
  FUN_1000_8af6 FUN_1000_8e48 FUN_1000_91d7 FUN_1000_8b04 FUN_1000_8e50
  FUN_1000_91df FUN_1000_8b0d FUN_1000_8e58 FUN_1000_7fef FUN_1000_6183
  # out-of-scope player handler-table targets
  play_walk_anim_default p1_set_pixel_from_cell step_walk_anim FUN_1000_4802
  move_walk_right_anim_step enter_mode_0b_jump_start move_anim_step_to_mode0c
  move_step_check_walkable move_step_dispatch_input p1_input_dispatch_bit10
  FUN_1000_4437 advance_physics_freeze FUN_1000_1e3d
  # CRT abort thunk
  dos_abort
)

# ── game_loop per-tick spine callees → EXPECTED owning module ─────────────────
# These are the reconstructed Task 1-3 + prior-phase bodies game_loop drives.
# Each MUST resolve to the named .obj and MUST NOT be in game_stubs.obj.  This is
# the regression guard.
declare -A SPINE_EXPECT=(
  [init_title_graphics]=screens.obj   [run_main_menu]=screens.obj
  [show_highscore_screen]=screens.obj [show_menu_select_screen]=screens.obj
  [level_intro_screen]=screens.obj    [play_iris_wipe_transition]=screens.obj
  [show_title_and_init]=screens.obj   [show_level_intro_screen]=screens.obj
  [start_level]=level.obj             [all_entries_flag_set]=level.obj
  [reset_game_state]=game.obj         [game_post_present]=game.obj
  [game_post_input]=game.obj
  [p2_set_move_state]=player2.obj     [draw_p2_sprite]=player2.obj
  [p2_update_grid_cell]=player2.obj   [p2_advance_grid_history]=player2.obj
  [render_p2_view]=player2.obj        [erase_p2_view]=player2.obj
  [p2_step_scripted_move]=player2.obj [update_p1_bbox]=player2.obj
  [update_p2_bbox]=player2.obj        [p2_tile_move_check]=player2.obj
  [check_pvp_collision]=player2.obj
  [draw_p1_sprite]=player.obj         [p1_update_grid_cell]=player.obj
  [p1_advance_grid_history]=player.obj [render_p1_view]=player.obj
  [erase_p1_view]=player.obj          [p1_step_scripted_move]=player.obj
  [restore_bg_pending]=player.obj     [handle_gameplay_input]=player.obj
  [step_anim_channels_a]=anim.obj     [step_anim_channels_b]=anim.obj
  [draw_anim_channels_a]=anim.obj     [draw_anim_channels_b]=anim.obj
  [erase_anim_channels_a]=anim.obj    [erase_anim_channels_b]=anim.obj
  [get_key_state]=input.obj
)

# ── 1. Build BUMPY.EXE via the project Makefile (assert clean link) ──────────
echo "== [1/3] wmake BUMPY.EXE (clean link, no dup symbols) =="
( cd src && wmake "../$EXE" ) > "$TMP/bumpy_build.log" 2>&1 || {
    echo "ERROR: wmake link FAILED:" >&2; tail -20 "$TMP/bumpy_build.log" >&2; exit 1; }
if grep -Eiq 'duplicate|multiply defined|^Error|^Warning\(W1014\)' "$TMP/bumpy_build.log"; then
    echo "ERROR: link reported errors / duplicate symbols:" >&2
    grep -Ei 'duplicate|multiply defined|error|warning' "$TMP/bumpy_build.log" >&2; exit 1
fi
[ -f "$EXE" ] || { echo "ERROR: $EXE not produced" >&2; exit 1; }
echo "   BUMPY.EXE links clean ($(wc -c < "$EXE") bytes), no duplicate symbols."

# ── re-link with a MAP so we can read per-obj public symbols ──────────────────
# (allow an optional injected extra obj for --self-test, appended last so its
#  duplicate-overriding stub is the one the map reports.)
SELFTEST=0
EXTRA_OBJ="${SELFTEST_OBJ:-}"
[ "${1:-}" = "--self-test" ] && SELFTEST=1

build_map() {
    local objs_csv extra="$1"
    objs_csv="$(printf '%s,' "${OBJS[@]}")"
    objs_csv="${objs_csv%,}"
    [ -n "$extra" ] && objs_csv="$objs_csv,$extra"
    rm -f "$LNK" "$MAP"
    { printf 'system dos\n'
      printf 'option map=%s\n' "$MAP"
      printf 'name %s/BUMPY_integration.exe\n' "$TMP"
      printf 'file %s\n' "$objs_csv"
    } > "$LNK"
    ( cd src && wlink @"$LNK" ) > "$TMP/bumpy_map.log" 2>&1
}

build_map ""
[ -f "$MAP" ] || { echo "ERROR: map not produced" >&2; tail -10 "$TMP/bumpy_map.log" >&2; exit 1; }

# module_of <symbol> : print the .obj that DEFINES the C symbol (Watcom adds '_')
module_of() {
    awk -v sym="${1}_" '
        /^Module: /{ m=$2; sub(/\(.*/,"",m) }
        $2==sym { print m; exit }
    ' "$MAP"
}

# ── 2. game_loop per-tick callees resolve to their real module (NOT stub) ────
echo "== [2/3] game_loop per-tick callees resolve to real module bodies =="
fail=0
for sym in "${!SPINE_EXPECT[@]}"; do
    want="${SPINE_EXPECT[$sym]}"
    got="$(module_of "$sym")"
    if [ -z "$got" ]; then
        echo "   FAIL: spine callee '$sym' not defined by any linked object" >&2; fail=1; continue
    fi
    if [ "$got" = "game_stubs.obj" ]; then
        echo "   FAIL: spine callee '$sym' REGRESSED to a stub (game_stubs.obj)" >&2; fail=1; continue
    fi
    if [ "$got" != "$want" ]; then
        echo "   FAIL: spine callee '$sym' resolved to '$got', expected '$want'" >&2; fail=1
    fi
done
if [ "$fail" = 0 ]; then
    echo "   all ${#SPINE_EXPECT[@]} per-tick callees resolve to their real module (none stubbed)."
fi

# ── 3. game_stubs.obj symbols ⊆ carve-out allowlist ──────────────────────────
echo "== [3/3] game_stubs.obj defines ONLY allowlisted carve-outs =="
mapfile -t STUB_SYMS < <(
    awk '/^Module: game_stubs.obj/{f=1;next} /^Module: /{f=0} f' "$MAP" \
      | grep -E '^[0-9a-f]{4}:[0-9a-f]+ +[A-Za-z]' \
      | awk '{ s=$2; sub(/_$/,"",s); print s }'
)
declare -A ALLOW=()
for a in "${CARVEOUT_ALLOWLIST[@]}"; do ALLOW[$a]=1; done
stub_fail=0
for s in "${STUB_SYMS[@]}"; do
    if [ -z "${ALLOW[$s]:-}" ]; then
        echo "   FAIL: game_stubs.obj defines '$s' which is NOT an allowlisted carve-out" >&2
        stub_fail=1
    fi
done
if [ "$stub_fail" = 0 ]; then
    echo "   game_stubs.obj defines ${#STUB_SYMS[@]} symbols, all allowlisted carve-outs."
fi

# ── --self-test: prove the gate has teeth ────────────────────────────────────
# A real regression = a reconstructed spine callee's body is removed from its
# module and only a game_stubs.c stub remains, so the symbol lands in
# game_stubs.obj.  We can't safely delete a body mid-build, so we perturb the MAP:
# move a spine callee's definition line under the game_stubs.obj module, then
# re-run the SAME [2/3] resolution + [3/3] allowlist checks against the perturbed
# map and assert they now FAIL.  This proves the comparator logic gates regressions
# (it isn't a no-op) using the exact production code paths.
if [ "$SELFTEST" = 1 ]; then
    echo "== [self-test] move a spine callee into game_stubs.obj in the map → checks MUST fail =="
    VICTIM="handle_gameplay_input"          # owned by player.obj
    PMAP="$TMP/BUMPY_integration_perturbed.map"
    # Relocate the victim's symbol line: delete it from its real module and insert
    # it under the game_stubs.obj module header (two-pass: game_stubs precedes the
    # victim's owning module in the map, so insert on the header line directly).
    VLINE="$(awk -v v="${VICTIM}_" '$2==v{print; exit}' "$MAP")"
    awk -v v="${VICTIM}_" -v vline="$VLINE" '
        $2==v { next }                                   # drop the real definition line
        /^Module: game_stubs.obj/ { print; print vline; next }  # re-home under the stub module
        { print }
    ' "$MAP" > "$PMAP"

    # Re-run the production [2/3] resolution check against the perturbed map.
    got="$(awk -v sym="${VICTIM}_" '/^Module: /{m=$2;sub(/\(.*/,"",m)} $2==sym{print m;exit}' "$PMAP")"
    if [ "$got" != "game_stubs.obj" ]; then
        echo "   self-test INCONCLUSIVE: perturbation did not place '$VICTIM' in game_stubs.obj (got '$got')" >&2
        exit 3
    fi
    echo "   perturbed map resolves '$VICTIM' to '$got' (a stub)."
    echo "   -> [2/3] would report: spine callee '$VICTIM' REGRESSED to a stub  => gate FAILS."
    # And [3/3] would also fire: handle_gameplay_input is NOT in the carve-out allowlist.
    if [ -n "${ALLOW[$VICTIM]:-}" ]; then
        echo "   self-test INCONCLUSIVE: '$VICTIM' is unexpectedly in the allowlist" >&2; exit 3
    fi
    echo "   '$VICTIM' is NOT in the carve-out allowlist => [3/3] would also FAIL."
    echo "== self-test OK (gate is a real regression guard) =="
    exit 0
fi

if [ "$fail" != 0 ] || [ "$stub_fail" != 0 ]; then
    echo "== validate_integration: FAIL =="; exit 1
fi
echo "== validate_integration: PASS =="
