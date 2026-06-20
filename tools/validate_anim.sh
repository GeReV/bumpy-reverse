#!/usr/bin/env bash
# Validate the reconstructed anim-channel module (src/anim.c) against the Phase-5 T1
# capture (local/build/render/anim_trace.bin, magic ANIMTRC1) via the REPLAY HARNESS
# tools/anim_chan_ctest.c:
#   1. src/anim.c compiles clean under the Open Watcom 16-bit DOS toolchain
#      (wcc -ml -bt=dos -zq -wx);
#   2. the host replay harness builds (cc -O2 -Wall -Werror) and runs over every
#      scenario in the trace.  It runs, for each PORTED, host-callable anim fn:
#        (A) SEMANTIC-STATE DIFFERENTIAL — seeds each record's ENTRY snapshot (the
#            channel-record table + tilemap), calls the ported fn by C name, and
#            asserts the channel-record bytes (3 A + 4 B slots) AND the tilemap stamp
#            == the record's EXIT snapshot;
#        (B) DESCRIPTOR DIFFERENTIAL — for draw/erase records, asserts the produced
#            p1_sprite blit descriptor / view-descriptor bytes match the captured ones.
#      Functions with no reconstructed body yet are reported UNPORTED (expected until
#      Phase-5 T3/T4) — NOT a hard failure, and never referenced as a symbol.
#
# NAMING NOTE: the harness is tools/anim_chan_ctest.c (anim "channel"), NOT
# tools/anim_ctest.c — that filename is already the tracked Plan-5b sprite-frame
# anim-select ctest used by tools/validate_blit.sh.  See the harness header + the
# Phase-5 T2 report for the deviation record.
#
# Exit 0 iff anim.c builds clean AND the per-function differential has ZERO failures
# on the PORTED fns.  This is the Task-2 self-check: as of this task ALL SEVEN anim
# functions are UNPORTED (src/anim.c is the globals-only skeleton; the bodies port in
# Phase-5 T3/T4), so the expected result is every record UNPORTED with zero FAIL.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TRACE="${1:-local/build/render/anim_trace.bin}"
if [ ! -f "$TRACE" ]; then
    echo "ERROR: anim trace not found: $TRACE" >&2
    echo "  (Phase-5 T1 capture; regenerate via tools/anim_oracle.py if missing)" >&2
    exit 2
fi

echo "== Open Watcom 16-bit compile check (src/anim.c) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx anim.c -fo="${TMPDIR:-/tmp}/anim.obj" )
echo "   anim.c builds clean (wcc -ml -bt=dos -zq -wx)"

echo "== host replay harness: per-function semantic-state + descriptor differential =="
AOUT="${TMPDIR:-/tmp}/anim_chan_ctest"
cc -O2 -Wall -Werror -o "$AOUT" tools/anim_chan_ctest.c
"$AOUT" "$TRACE"
