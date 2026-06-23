#!/usr/bin/env bash
# Validate the reconstructed game_loop per-tick body (game_tick) END-TO-END,
# tick-for-tick, against a REAL DOSBox capture of BUMPY.EXE — the int8-synced
# end-to-end gate (Phase-9.x / int8-snap, Task 7).  This is the project's last
# composition gate: per-function gates prove each function is faithful in isolation;
# this proves they COMPOSE into a faithful running game (call order, inter-function
# state flow, loop timing) by replaying game_tick() against the captured frame trace.
#
# Pipeline (mirrors tools/validate_p1_spine.sh's structure):
#   1. build the instrumented dosbox-x if missing (applies tools/dosbox/patches/01+02);
#   2. capture the level-1 scripted-gameplay trace if missing (boot+gameplay script,
#      env BUMPYCAP_INT8_OUT/_FRAMES; the 02 patch emits header+INIT+(N+1)*FRAME);
#   3. Open Watcom 16-bit compile-check the reconstructed src/ modules;
#   4. awk-extract game_tick (+ game_post_present/game_post_input) verbatim from
#      src/game.c into tools/int8_extracted.h and cc -O2 build tools/int8_ctest.c
#      with -DINT8_WITH_GAME_TICK (the REAL reconstructed bodies, no source copy);
#   5. run tools/int8_oracle_check.py — the calibration TRUST ANCHOR (the DOSBox
#      capture must agree with the Unicorn per-function oracle on INIT + first frames);
#   6. run int8_ctest <trace> — the REAL per-tick replay (seed once from INIT, then
#      feed each FRAME's rng/input, call game_tick(), assert evolved state == captured);
#   7. run int8_ctest --perturb <trace> — assert it FAILS (genuine differential).
#
# Exit 0 iff: the src modules build clean AND the oracle anchor agrees AND the replay
# matches every scalar gameplay field for the full captured scenario AND the
# perturbation run correctly FAILS.
#
# EXCLUSION (one precise, documented carve-out — NOT a tolerance): the per-frame
# tilemap_hash is excluded from the compare.  The replay reproduces every
# GAMEPLAY-COLLISION tilemap write 1:1; the only residual full-tilemap divergence is
# the ANIMATED-TILE FX-GRAPHICS layer (cell+0xa0), written inside the carved-out BGI
# render core (render_player_view -> the un-analyzed EGAVGA overlay handler), which no
# gameplay-collision callee reads.  See int8_ctest.c run_replay + the int8 row of
# docs/reconstruction-fidelity.md.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
TMP="${TMPDIR:-/tmp}"

TRACE="${1:-local/build/render/int8_trace.bin}"
DBX_BIN="local/toolchain/dosbox-x-src/src/dosbox-x"
GAME_DIR="local/build/capture/game"
SCRIPT="tools/dosbox/scripts/level1-scripted.txt"
CONF_TMPL="tools/dosbox/bumpy-capture.conf"
FRAMES="${BUMPYCAP_INT8_FRAMES:-150}"

# ── 1. Build the instrumented dosbox-x if missing ────────────────────────────────
if [ ! -x "$DBX_BIN" ]; then
    echo "== instrumented dosbox-x missing -> building (tools/dosbox/build-dosbox-x.sh) =="
    tools/dosbox/build-dosbox-x.sh
fi

# ── 2. Capture the trace if missing ──────────────────────────────────────────────
if [ ! -f "$TRACE" ]; then
    echo "== trace missing -> capturing $FRAMES frames via the scripted level-1 run =="
    if [ ! -f "$GAME_DIR/BUMPY.EXE" ]; then
        echo "ERROR: game install not found at $GAME_DIR (user-supplied; see docs)" >&2
        exit 2
    fi
    mkdir -p "$(dirname "$TRACE")"
    # Generate a run-specific conf with the absolute mount path (the template ships a
    # placeholder).  Determinism: frozen conf + scripted input + pinned emulator.
    RUNCONF="$TMP/bumpy-capture-run.conf"
    \rm -f "$RUNCONF"
    sed "s#/ABSOLUTE/PATH/TO/local/build/capture/game#$ROOT/$GAME_DIR#" "$CONF_TMPL" > "$RUNCONF"
    # Headless capture; HOME=$TMP + dummy SDL drivers; the emulator can linger after
    # its kill-switch, so bound it with a hard wall-clock timeout.
    HOME="$TMP" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
      BUMPYCAP_SCRIPT="$ROOT/$SCRIPT" \
      BUMPYCAP_INT8_OUT="$ROOT/$TRACE" BUMPYCAP_INT8_FRAMES="$FRAMES" \
      timeout 200 "$DBX_BIN" -conf "$RUNCONF" -nomenu -nogui >"$TMP/int8_capture.log" 2>&1 || true
    if [ ! -f "$TRACE" ]; then
        echo "ERROR: capture produced no trace (see $TMP/int8_capture.log)" >&2
        exit 2
    fi
    echo "   captured $(stat -c%s "$TRACE") bytes -> $TRACE"
fi

# ── 3. Open Watcom 16-bit compile check (the modules game_tick composes) ─────────
echo "== Open Watcom 16-bit compile check (game/player/player2/level/anim/items/spawn/input) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && for m in game player player2 level anim items spawn input; do
         wcc -ml -bt=dos -zq -wx $m.c -fo="$TMP/$m.obj"
       done )
echo "   src modules build clean (wcc -ml -bt=dos -zq -wx)"

# ── 4. awk-extract game_tick + helpers verbatim and build the replay harness ─────
# (game.c pulls <dos.h> + the render/loop pipeline and cannot host-include wholesale;
#  extract just the three real game.c BODIES game_tick needs — game_tick itself and
#  the two state-mutating helpers it calls — keeping the gate on the REAL source.)
EXTRACT="tools/int8_extracted.h"
{
    echo "/* AUTO-EXTRACTED by tools/validate_int8.sh — DO NOT EDIT / DO NOT COMMIT."
    echo "   The verbatim src/game.c game_tick + game_post_present + game_post_input"
    echo "   bodies (real source, sliced for the host harness which cannot include the"
    echo "   game.c TU wholesale). */"
    awk '/^void game_tick\(void\)$/         {p=1} p{print} p&&/^}$/{exit}' src/game.c
    echo ""
    awk '/^void game_post_present\(void\)$/ {p=1} p{print} p&&/^}$/{exit}' src/game.c
    echo ""
    awk '/^void game_post_input\(void\)$/   {p=1} p{print} p&&/^}$/{exit}' src/game.c
} > "$EXTRACT"
echo "   extracted game.c game_tick/_post_present/_post_input -> $EXTRACT"

CTEST="$TMP/int8_ctest"
cc -O2 -Wall -DINT8_WITH_GAME_TICK -Itools -o "$CTEST" tools/int8_ctest.c
echo "   built int8_ctest (cc -O2 -Wall -DINT8_WITH_GAME_TICK -Itools)"

# ── 5. Calibration trust anchor (DOSBox capture vs Unicorn per-function oracle) ──
echo
echo "== calibration trust anchor: int8_oracle_check.py (DOSBox capture vs Unicorn oracle) =="
uv run python tools/int8_oracle_check.py "$TRACE"

# ── 6. The REAL per-tick replay ──────────────────────────────────────────────────
echo
echo "== int8 per-tick replay: reconstructed game_tick() vs the real captured trace =="
"$CTEST" "$TRACE"

# ── 7. Perturbation proof (genuine differential) ─────────────────────────────────
echo
echo "== perturbation proof (corrupt a seeded field -> expect the replay to FAIL) =="
if "$CTEST" --perturb "$TRACE"; then
    echo "ERROR: --perturb run PASSED — the gate is not a genuine differential!" >&2
    rm -f "$EXTRACT"
    exit 1
fi
echo "   --perturb correctly diverged (the gate is a genuine differential)"

# clean up the transient extracted header (build artifact, not source).
rm -f "$EXTRACT"
echo
echo "validate_int8: PASS (oracle anchor agrees + 150-frame replay matched + perturb caught)"
