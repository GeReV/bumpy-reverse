#!/usr/bin/env bash
# Validate the reconstructed item/exit module (src/items.c) against the Phase-3 T1
# capture (local/build/render/items_trace.bin, magic ITEMTRC1) via the REPLAY
# HARNESS tools/items_ctest.c:
#   1. src/items.c compiles clean under the Open Watcom 16-bit DOS toolchain
#      (wcc -ml -bt=dos -zq -wx);
#   2. the host replay harness builds (cc -O2 -Wall -Werror) and runs over every
#      scenario in the trace.  It runs ONE comparator:
#        PER-FUNCTION DIFFERENTIAL — seeds each record's ENTRY snapshot + a
#                 synthetic tilemap (the captured layer-C item byte at
#                 tilemap[cell+0x60]), calls the ported fn by C name, and asserts
#                 the output semantic-state globals (+ the tilemap item byte) ==
#                 the record's EXIT snapshot.  Functions with no reconstructed
#                 body yet are reported UNPORTED (expected until T3/T4) — NOT a
#                 hard failure, and never referenced as a symbol.
#
# Exit 0 iff items.c builds clean AND the per-function differential has ZERO
# failures on the PORTED fns.  This is the Task-2 self-check; as of this task ALL
# FIVE item/exit functions are UNPORTED (their bodies port in Phase-3 T3/T4), so
# the expected result is every record UNPORTED with zero FAIL.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TRACE="${1:-local/build/render/items_trace.bin}"
if [ ! -f "$TRACE" ]; then
    echo "ERROR: items trace not found: $TRACE" >&2
    echo "  (Phase-3 T1 capture; regenerate via tools/items_oracle.py if missing)" >&2
    exit 2
fi

echo "== Open Watcom 16-bit compile check (src/items.c) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx items.c -fo="${TMPDIR:-/tmp}/items.obj" )
echo "   items.c builds clean (wcc -ml -bt=dos -zq -wx)"

echo "== host replay harness: per-function semantic-state differential =="
IOUT="${TMPDIR:-/tmp}/items_ctest"
cc -O2 -Wall -Werror -o "$IOUT" tools/items_ctest.c
"$IOUT" "$TRACE"
