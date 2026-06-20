#!/usr/bin/env bash
# Validate the reconstructed sound module (src/sound.c) against the Phase-6 T1 capture
# (local/build/render/sound_trace.bin, magic SNDTRC01) via the REPLAY HARNESS
# tools/sound_ctest.c:
#   1. src/sound.c compiles clean under the Open Watcom 16-bit DOS toolchain
#      (wcc -ml -bt=dos -zq -wx);
#   2. the host replay harness builds (cc -O2 -Wall -Werror) and runs over every
#      scenario in the trace.  For each PORTED, host-callable sound fn it runs one of
#      two comparators:
#        (A) SEMANTIC-STATE DIFFERENTIAL (L1 dispatch / L2 device / L3 tone) — seeds
#            each record's ENTRY SND_SNAP into the reconstructed sound globals, calls
#            the ported fn by C name, asserts the sound-global SNAP (device/driver
#            state + tone param frame + timer-cb ptr) == the EXIT SNAP;
#        (B) PORT-WRITE-SEQUENCE DIFFERENTIAL (L4 hardware drivers) — primes the host
#            in() shim with the record's recorded IN sequence, clears the out()
#            capture, calls the ported driver, and asserts the host OUT-capture ==
#            the record's captured OUT events.
#      Functions with no reconstructed body yet are reported UNPORTED — NOT a hard
#      failure, and never referenced as a symbol.
#
# Phase-6 T2 SKELETON STATE: src/sound.c defines ONLY the sound globals (no bodies;
# they remain stubbed in game_stubs.c).  So PORTED[] is all-NULL and EVERY record is
# UNPORTED — expected PASS=0 FAIL=0 UNPORTED=872 (the full T1 record count).  The
# L1–L4 ports (T3–T6) progressively fill PORTED[]; this gate then exercises the
# comparators on every reached fn.
#
# Exit 0 iff sound.c builds clean AND the per-function differential has ZERO failures
# on the PORTED fns (UNPORTED records never fail).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TRACE="${1:-local/build/render/sound_trace.bin}"
if [ ! -f "$TRACE" ]; then
    echo "ERROR: sound trace not found: $TRACE" >&2
    echo "  (Phase-6 T1 capture; regenerate via tools/sound_oracle.py if missing)" >&2
    exit 2
fi

echo "== Open Watcom 16-bit compile check (src/sound.c) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx sound.c -fo="${TMPDIR:-/tmp}/sound.obj" )
echo "   sound.c builds clean (wcc -ml -bt=dos -zq -wx)"

echo "== host replay harness: per-function semantic-state + port-write-sequence differential =="
AOUT="${TMPDIR:-/tmp}/sound_ctest"
cc -O2 -Wall -Werror -o "$AOUT" tools/sound_ctest.c
"$AOUT" "$TRACE"
