#!/usr/bin/env bash
# Validate the P1 move-execution spine port (src/player.c, Phase 1 Task 6a):
#   1. compiles clean under the Open Watcom 16-bit DOS toolchain (wcc -ml -wx);
#   2. the host per-function differential ctest (tools/player_ctest.c) builds and
#      passes — it drives the REAL p1_step_scripted_move() over a known
#      [anim,dx,dy] move script (with __far/__huge shimmed and exact 16-bit types)
#      and asserts px/py/anim/steps/step_idx/return evolve exactly per the script
#      semantics, including the facing-left dx-negation and the move_locked /
#      steps_left==0 / game_mode-in-{5,0xb,0x1c} no-op guards.
#
# Move-script source is SYNTHETIC: the engine's real scripts live behind
# mode_script_tbl (DGROUP 0x2252) far pointers populated at runtime (a bounded
# DGROUP probe showed them uninitialised in the capture), so the brief's
# synthetic-script fallback applies; the function logic under test is unchanged.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "== Open Watcom 16-bit compile check =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx player.c -fo="${TMPDIR:-/tmp}/player.obj" )
echo "   player.c builds clean (wcc -ml -bt=dos -zq -wx)"

echo "== host per-function differential: p1_step_scripted_move =="
POUT="${TMPDIR:-/tmp}/player_ctest"
cc -O2 -Wall -o "$POUT" tools/player_ctest.c
"$POUT"
