#!/usr/bin/env bash
# Validate the reconstructed P1 physics (src/player.c) against the Phase-2 T1
# capture (local/build/render/physics_trace.bin, magic PHYSTRC1) via the REPLAY
# HARNESS tools/physics_ctest.c:
#   1. src/player.c compiles clean under the Open Watcom 16-bit DOS toolchain
#      (wcc -ml -bt=dos -zq -wx);
#   2. the host replay harness builds (cc -O2 -Wall -Werror) and runs over all 5
#      scenarios in the trace.  It runs TWO comparators:
#        PRIMARY  per-function differential — seeds each record's ENTRY snapshot +
#                 move-script + tilemap window, calls the ported fn by C name, and
#                 asserts the output globals == the record's EXIT snapshot.  The
#                 host-callable ported fns (p1_step_scripted_move, enter_game_mode)
#                 plus dispatch_move_step's slot-address arithmetic must PASS on
#                 every record; the landing leaves (0x2810/0x29a6) and the
#                 dispatch call-through (p1_movement_dispatch) are reported
#                 UNPORTED (expected until T3/T4) — NOT a hard failure.
#        SECONDARY trajectory-stitch — host re-implementation of the per-tick call
#                 order, dispatched via an explicit engine-offset -> reconstructed-C
#                 host map; asserts the (px,py,move_anim,game_mode) SEQUENCE matches
#                 the capture up to the first UNPORTED stop.  (Validation tooling.)
#
# Exit 0 iff player.c builds clean AND the per-function differential has ZERO
# failures on the ported fns.  This is the Task-2 self-check; a full physics pass
# is T3-T5 (as more substates port, their UNPORTED counts convert to PASS).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TRACE="${1:-local/build/render/physics_trace.bin}"
if [ ! -f "$TRACE" ]; then
    echo "ERROR: physics trace not found: $TRACE" >&2
    echo "  (Phase-2 T1 capture; regenerate via tools/physics_oracle.py if missing)" >&2
    exit 2
fi

echo "== Open Watcom 16-bit compile check (src/player.c) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx player.c -fo="${TMPDIR:-/tmp}/player.obj" )
echo "   player.c builds clean (wcc -ml -bt=dos -zq -wx)"

echo "== host replay harness: per-function differential + trajectory-stitch =="
POUT="${TMPDIR:-/tmp}/physics_ctest"
cc -O2 -Wall -Werror -o "$POUT" tools/physics_ctest.c
"$POUT" "$TRACE"
