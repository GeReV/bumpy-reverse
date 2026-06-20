#!/usr/bin/env bash
# Validate the reconstructed Player-2 module (src/player2.c) against the Phase-4 T1
# capture (local/build/render/p2_trace.bin, magic P2TRACE1) via the REPLAY HARNESS
# tools/p2_ctest.c:
#   1. src/player2.c compiles clean under the Open Watcom 16-bit DOS toolchain
#      (wcc -ml -bt=dos -zq -wx);
#   2. the host replay harness builds (cc -O2 -Wall -Werror) and runs over every
#      scenario in the trace.  It runs:
#        (A) PER-FUNCTION TRAJECTORY + AI DIFFERENTIAL — seeds each record's ENTRY
#            snapshot + the captured move-script bytes + rng_frame, calls the ported
#            fn by C name, and asserts the output P2 trajectory + AI-decision globals
#            (px/py/anim/grid/cell/move_state/steps_left/step_idx/facing/toggle/pvp)
#            == the record's EXIT snapshot;
#        (B) RENDER-DESCRIPTOR COMPARATOR — for draw_p2_sprite records, asserts the
#            produced P2 object descriptor (x, y, frame=frame_base+anim) matches the
#            captured bytes.
#      Functions with no reconstructed body yet are reported UNPORTED (expected
#      until T3/T4/T5) — NOT a hard failure, and never referenced as a symbol.
#
# Exit 0 iff player2.c builds clean AND the per-function differential has ZERO
# failures on the PORTED fns.  This is the Task-2 self-check: as of this task ALL
# ELEVEN P2 functions are UNPORTED (src/player2.c is the globals-only skeleton; the
# bodies port in Phase-4 T3/T4/T5), so the expected result is every record UNPORTED
# with zero FAIL.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TRACE="${1:-local/build/render/p2_trace.bin}"
if [ ! -f "$TRACE" ]; then
    echo "ERROR: P2 trace not found: $TRACE" >&2
    echo "  (Phase-4 T1 capture; regenerate via tools/p2_oracle.py if missing)" >&2
    exit 2
fi

echo "== Open Watcom 16-bit compile check (src/player2.c) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx player2.c -fo="${TMPDIR:-/tmp}/player2.obj" )
echo "   player2.c builds clean (wcc -ml -bt=dos -zq -wx)"

echo "== host replay harness: per-function trajectory + AI + descriptor differential =="
POUT="${TMPDIR:-/tmp}/p2_ctest"
cc -O2 -Wall -Werror -o "$POUT" tools/p2_ctest.c
"$POUT" "$TRACE"
