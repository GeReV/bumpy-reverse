#!/usr/bin/env bash
# Validate the reconstructed front-end MODULE (src/screens.c — menus / title /
# highscore / level-intro screens + HUD/number formatters + the DAC palette upload)
# against the Phase-7 T1 capture (local/build/render/screens_trace.bin, magic SCRTRC01)
# via the REPLAY HARNESS tools/screens_ctest.c.
#
# NAMING NOTE (deviation, documented): the natural name `tools/validate_screens.sh`
# is ALREADY TAKEN by an UNRELATED, committed gate (the VEC-renderer pixel-diff for
# TITRE/SCORE/MASKBUMP/DESSFIN/BUMPRESE, commit bebf586).  The Phase-7 T2 brief asked
# for `validate_screens.sh`, but the HARD CONSTRAINT forbids touching any existing
# validate script.  To avoid clobbering that distinct gate, this front-end-MODULE gate
# is named `validate_screen_fns.sh` instead (the "screen FUNCTIONS" replay harness, vs
# the "screen IMAGES" VEC pixel gate).  See .git/sdd/task-2-report.md.
#
#   1. src/screens.c compiles clean under the Open Watcom 16-bit DOS toolchain
#      (wcc -ml -bt=dos -zq -wx);
#   2. the host replay harness builds (cc -O2 -Wall -Werror) and runs over every
#      scenario in the trace.  For each PORTED, host-callable screen fn it runs one of
#      THREE comparators:
#        (A) SEMANTIC-STATE DIFFERENTIAL (menus / title / highscore / level-intro) —
#            seeds each record's ENTRY SCRSNAP into the reconstructed screen globals,
#            calls the ported fn by C name, asserts the screen-global SCRSNAP (level /
#            palette_mode / menu_option2_setting / input_state / score / timing /
#            frame_abort_flag / the highscore name row + the AX return) == the EXIT SNAP;
#        (B) DESCRIPTOR-LEVEL DIFFERENTIAL (text/number/HUD builders + screen draws) —
#            points render_descriptor_ptr / p1_sprite at host buffers, seeds the
#            fullscreen_buf header, calls the ported builder, asserts the bytes it wrote
#            into the host view-struct + p1_sprite descriptors == the record's captured
#            EXIT descriptors;
#        (C) PORT-WRITE-SEQUENCE DIFFERENTIAL (the DAC upload + iris-wipe) — primes the
#            host in() shim with the record's recorded IN sequence, clears the out()
#            capture, calls the ported code, asserts the host OUT-capture (DAC 0x3c8/0x3c9
#            writes) == the record's captured OUT events.
#      Functions with no reconstructed body yet are reported UNPORTED — NOT a hard
#      failure, and never referenced as a symbol.
#
# Phase-7 T2 SKELETON STATE: src/screens.c defines ONLY the screen globals (no bodies;
# they remain stubbed in game_stubs.c).  So PORTED[] is all-NULL and EVERY record is
# UNPORTED — expected PASS=0 FAIL=0 UNPORTED=<the full T1 record count>.  The T3–T5 ports
# progressively fill PORTED[]; this gate then exercises the comparators on every reached
# fn.
#
# Exit 0 iff screens.c builds clean AND the per-function differential has ZERO failures
# on the PORTED fns (UNPORTED records never fail).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TRACE="${1:-local/build/render/screens_trace.bin}"
if [ ! -f "$TRACE" ]; then
    echo "ERROR: screens trace not found: $TRACE" >&2
    echo "  (Phase-7 T1 capture; regenerate via tools/screens_oracle.py if missing)" >&2
    exit 2
fi

echo "== Open Watcom 16-bit compile check (src/screens.c) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx screens.c -fo="${TMPDIR:-/tmp}/screens.obj" )
echo "   screens.c builds clean (wcc -ml -bt=dos -zq -wx)"

echo "== host replay harness: semantic-state + descriptor-level + port-write-sequence differential =="
AOUT="${TMPDIR:-/tmp}/screens_ctest"
cc -O2 -Wall -Werror -o "$AOUT" tools/screens_ctest.c
"$AOUT" "$TRACE"
