#!/usr/bin/env bash
# Validate the reconstructed MIDI module (src/midi.c) + its OPL2 backend (src/sound.c)
# against the Task C2 capture (local/build/render/midi_trace.bin, magic MIDTRC01) via
# the REPLAY HARNESS tools/midi_ctest.c:
#   1. src/midi.c compiles clean under the Open Watcom 16-bit DOS toolchain
#      (wcc -ml -bt=dos -zq -wx);
#   2. the host replay harness builds (cc -O2 -Wall -Werror) and runs over every
#      scenario in the trace.  For each PORTED, host-callable MIDI/OPL fn it runs one
#      of two comparators:
#        (A) SEMANTIC-STATE DIFFERENTIAL (parser/sequencer fns) — seeds each record's
#            ENTRY MIDI_SNAP into the reconstructed MIDI globals, calls the ported fn
#            by C name, asserts the MIDI sequencer-state SNAP == the EXIT SNAP;
#        (B) PORT-WRITE-SEQUENCE DIFFERENTIAL (the 9 OPL2/emission fns +
#            midi_install_tempo_timer) — primes the host in() shim with the record's
#            recorded IN sequence, clears the out() capture, calls the ported fn, and
#            asserts the host OUT-capture == the record's captured OUT events.
#      Functions with no reconstructed body yet are reported UNPORTED — NOT a hard
#      failure, and never referenced as a symbol.
#
# Task C3 BASELINE: src/midi.c defines ONLY the MIDI module's globals (no bodies yet;
# Phase D/E ports them).  So PORTED[] is EMPTY and EVERY record is UNPORTED — expected
# PASS=0 FAIL=0 UNPORTED=337 (the full Task C2 capture's record count).  Phase D/E
# progressively fill PORTED[]; this gate then exercises the comparators on every
# reached fn.
#
# Exit 0 iff midi.c builds clean AND the per-function differential has ZERO failures
# on the PORTED fns (UNPORTED records never fail).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TRACE="${1:-local/build/render/midi_trace.bin}"
if [ ! -f "$TRACE" ]; then
    echo "ERROR: MIDI trace not found: $TRACE" >&2
    echo "  (Task C2 capture; regenerate via tools/midi_oracle.py if missing)" >&2
    exit 2
fi

echo "== Open Watcom 16-bit compile check (src/midi.c) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx midi.c -fo="${TMPDIR:-/tmp}/midi.obj" )
echo "   midi.c builds clean (wcc -ml -bt=dos -zq -wx)"

echo "== host replay harness: per-function semantic-state + port-write-sequence differential =="
AOUT="${TMPDIR:-/tmp}/midi_ctest"
cc -O2 -Wall -Werror -o "$AOUT" tools/midi_ctest.c
"$AOUT" "$TRACE"
