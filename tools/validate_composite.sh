#!/usr/bin/env bash
# Validate the composite host harness (tools/composite_ctest.c):
#   1. Watcom 16-bit compile-check of src/bg_render.c (same as validate_bg.sh)
#   2. gcc compile + run tools/composite_ctest.c
#   3. Assert the C bg-match count == the Python composite_check.py count
#
# Requires: local/build/render/frame_oracle.bin (run FRAME_ORACLE=1 uv run python tools/sprite_oracle.py)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ORACLE="local/build/render/frame_oracle.bin"
if [ ! -f "$ORACLE" ]; then
  echo "missing $ORACLE — run: FRAME_ORACLE=1 uv run python tools/sprite_oracle.py" >&2
  exit 1
fi

echo "== Open Watcom 16-bit compile check =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx bg_render.c -fo="${TMPDIR:-/tmp}/bg_render.obj" )
echo "   bg_render.c builds clean (wcc -ml -wx)"

echo "== host bg composite render + plane diff =="
OUT="${TMPDIR:-/tmp}/composite_ctest"
cc -O2 -Wall -o "$OUT" tools/composite_ctest.c
C_OUTPUT=$(timeout 60 "$OUT" "$ORACLE")
echo "   $C_OUTPUT"

# Extract the C match count (the integer before the first '/')
C_COUNT=$(echo "$C_OUTPUT" | grep -oE '^bg: [0-9]+' | grep -oE '[0-9]+')
if [ -z "$C_COUNT" ]; then
  echo "ERROR: could not parse match count from C harness output: $C_OUTPUT" >&2
  exit 1
fi

echo "== Python composite_check.py reference =="
PY_OUTPUT=$(timeout 120 uv run python tools/composite_check.py "$ORACLE" 2>/dev/null | head -1)
echo "   $PY_OUTPUT"

# Extract the Python match count (the integer before '/64000')
PY_COUNT=$(echo "$PY_OUTPUT" | grep -oE '[0-9]+/64000' | grep -oE '^[0-9]+')
if [ -z "$PY_COUNT" ]; then
  echo "ERROR: could not parse match count from Python output: $PY_OUTPUT" >&2
  exit 1
fi

echo "== Assertion: C count == Python count =="
if [ "$C_COUNT" != "$PY_COUNT" ]; then
  echo "FAIL: C bg match ($C_COUNT) != Python bg match ($PY_COUNT)" >&2
  exit 1
fi
echo "   PASS: both report $C_COUNT/64000 matching pixels"
