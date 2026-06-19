#!/usr/bin/env bash
# Validate the keyboard input layer port (src/input.c) — Phase-1 Task 5.
#
#   1. Open Watcom 16-bit DOS compile check of src/input.c (zero warnings, -wx).
#   2. Host per-function differential: gcc-compiles tools/game_ctest.c (which
#      #includes the real src/input.c with the 16-bit env shimmed) and replays
#      the scripted key stream through poll_input, comparing per-tick input_state
#      to the golden trace's input_state column (100/100).
#
# The golden trace (local/build/render/slice_goldentrace.bin) is the resolved
# input SPEC captured by tools/game_oracle.py; see game_ctest.c for the honest
# note on what this proves (the interpreter PLUMBING end-to-end vs the spec).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

GOLDEN="local/build/render/slice_goldentrace.bin"
if [ ! -f "$GOLDEN" ]; then
  echo "missing $GOLDEN" >&2
  exit 1
fi

echo "== Open Watcom 16-bit compile check =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx input.c -fo="${TMPDIR:-/tmp}/input.obj" )
echo "   input.c builds clean (wcc -ml -wx)"

echo "== host per-function differential (key table -> poll_input -> input_state) =="
COUT="${TMPDIR:-/tmp}/game_ctest"
cc -O2 -Wall -o "$COUT" tools/game_ctest.c
"$COUT" "$GOLDEN"
