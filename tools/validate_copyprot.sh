#!/usr/bin/env bash
# Validate the reconstructed copy-protection challenge (copyprotect_challenge,
# 1000:4015, in src/level.c — behind #ifdef BUMPY_COPY_PROTECTION) against the
# Phase-7b T1 capture (local/build/render/copyprot_trace.bin, magic CPTRC01) via the
# REPLAY HARNESS tools/copyprot_ctest.c.
#
#   1. src/level.c compiles clean under the Open Watcom 16-bit DOS toolchain in BOTH
#      modes — the DEFAULT build (BUMPY_COPY_PROTECTION OFF: the whole challenge + hook
#      compile OUT, byte-unchanged behaviour) AND the protection-ON build
#      (-dBUMPY_COPY_PROTECTION: the challenge body compiles).
#   2. the host replay harness builds (cc -O2 -Wall) and runs over the trace.  It runs
#      TWO comparators:
#        (a) PRESENT-PARTS DIFFERENTIAL vs the T1 capture — the two table copies
#            (sprite_id_tbl 16 words / answer_tbl 16 bytes), the random sprite index in
#            2..15 reproduced by SEEDING src/prng.c from the captured LIVE prng state
#            (0x5192,0,0) → 12, the entered_number trajectory for each scripted dial
#            sequence (the 4×-poll-per-action sampling), and the p1_sprite display
#            descriptor (x=0x90, y=100, frame=sprite_id_tbl[12]).
#        (b) UN-CRACK LOGIC — the documented original compare the crack removed:
#            entered == answer ⇒ copyprotect_flag stays 0 (PASS); entered != answer ⇒
#            copyprotect_flag = -1 (FAIL); plus the plus-CEILING clamp (saturates at
#            0x63), which T1 did NOT exercise.
#
# Exit 0 iff level.c builds clean in BOTH modes AND both comparators report ZERO
# failures (present-parts PASS + un-crack logic PASS, FAIL=0).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TRACE="${1:-local/build/render/copyprot_trace.bin}"
if [ ! -f "$TRACE" ]; then
    echo "ERROR: copyprot trace not found: $TRACE" >&2
    echo "  (Phase-7b T1 capture; regenerate via tools/copyprot_oracle.py if missing)" >&2
    exit 2
fi

TMP="${TMPDIR:-/tmp}"

echo "== Open Watcom 16-bit compile check (src/level.c) — DEFAULT build (macro OFF) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx level.c -fo="$TMP/level_off.obj" )
echo "   level.c builds clean (default — challenge + hook compiled OUT)"

echo "== Open Watcom 16-bit compile check (src/level.c) — PROTECTION ON =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx -dBUMPY_COPY_PROTECTION level.c -fo="$TMP/level_on.obj" )
echo "   level.c builds clean (-dBUMPY_COPY_PROTECTION — challenge body compiled)"

echo "== host replay harness: present-parts differential + un-crack logic =="
COUT="$TMP/copyprot_ctest"
cc -O2 -Wall -Werror -o "$COUT" tools/copyprot_ctest.c
"$COUT" "$TRACE"
