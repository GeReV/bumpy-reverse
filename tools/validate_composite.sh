#!/usr/bin/env bash
# Validate the composite host harness (tools/composite_ctest.c):
#   1. Watcom 16-bit compile-check of src/bg_render.c and src/entity.c
#   2. gcc compile + run tools/composite_ctest.c
#   3. Assert the C bg-match count == the Python composite_check.py count
#   4. Assert bg+C match count > bg match count (monotonic progress)
#   5. Assert P1 object construction: x and frame match captured obj (y skew documented)
#   6. Assert bg+C+P1 match count >= bg+C match count (no regression)
#   7. Assert P2 absent on level 1: planes unchanged across entity_draw_p2
#
# Requires: local/build/render/frame_oracle.bin (run FRAME_ORACLE=1 uv run python tools/sprite_oracle.py)
# Requires: local/build/render/bank_inmem.bin (run uv run python tools/sprite_oracle.py)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ORACLE="local/build/render/frame_oracle.bin"
BANK="local/build/render/bank_inmem.bin"
if [ ! -f "$ORACLE" ]; then
  echo "missing $ORACLE — run: FRAME_ORACLE=1 uv run python tools/sprite_oracle.py" >&2
  exit 1
fi
if [ ! -f "$BANK" ]; then
  echo "missing $BANK — run: uv run python tools/sprite_oracle.py" >&2
  exit 1
fi

echo "== Open Watcom 16-bit compile check =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx bg_render.c -fo="${TMPDIR:-/tmp}/bg_render.obj" \
    && wcc -ml -bt=dos -zq -wx entity.c  -fo="${TMPDIR:-/tmp}/entity.obj" )
echo "   bg_render.c + entity.c build clean (wcc -ml -wx)"

echo "== host bg+C+P1+P2 composite render + plane diff =="
OUT="${TMPDIR:-/tmp}/composite_ctest"
# -Wno-unused-function suppresses the warning for sprite_expand_frame in
# src/sprite_anim.c — that function reconstructs the ctrl&0x40 packed-pixel
# expansion path which is dead code for all BUMSPJEU frames (ctrl always 0x03).
# It is intentionally kept UNVALIDATED; the suppression is intentional.
cc -O2 -Wall -Wno-unused-function -o "$OUT" tools/composite_ctest.c
C_OUTPUT=$(timeout 60 "$OUT" "$ORACLE" "$BANK")
echo "$C_OUTPUT" | sed 's/^/   /'

# Extract match counts
C_BG_COUNT=$(echo "$C_OUTPUT" | grep -oE '^bg: [0-9]+' | grep -oE '[0-9]+')
if [ -z "$C_BG_COUNT" ]; then
  echo "ERROR: could not parse bg match count from C harness output" >&2
  exit 1
fi

C_BGC_COUNT=$(echo "$C_OUTPUT" | grep -oE '^bg\+C: [0-9]+' | grep -oE '[0-9]+')
if [ -z "$C_BGC_COUNT" ]; then
  echo "ERROR: could not parse bg+C match count from C harness output" >&2
  exit 1
fi

C_BGCP1_COUNT=$(echo "$C_OUTPUT" | grep -oE '^bg\+C\+P1: [0-9]+' | sed 's/.*: //')
if [ -z "$C_BGCP1_COUNT" ]; then
  echo "ERROR: could not parse bg+C+P1 match count from C harness output" >&2
  exit 1
fi

echo "== Python composite_check.py reference (bg-only) =="
PY_OUTPUT=$(timeout 120 uv run python tools/composite_check.py "$ORACLE" 2>/dev/null | head -1)
echo "   $PY_OUTPUT"

# Extract the Python match count (the integer before '/64000')
PY_COUNT=$(echo "$PY_OUTPUT" | grep -oE '[0-9]+/64000' | grep -oE '^[0-9]+')
if [ -z "$PY_COUNT" ]; then
  echo "ERROR: could not parse match count from Python output: $PY_OUTPUT" >&2
  exit 1
fi

echo "== Assertion: C bg count == Python bg count =="
if [ "$C_BG_COUNT" != "$PY_COUNT" ]; then
  echo "FAIL: C bg match ($C_BG_COUNT) != Python bg match ($PY_COUNT)" >&2
  exit 1
fi
echo "   PASS: bg both report $C_BG_COUNT/64000 matching pixels"

echo "== Assertion: bg+C match > bg match (monotonic progress) =="
if [ "$C_BGC_COUNT" -le "$C_BG_COUNT" ]; then
  echo "FAIL: bg+C ($C_BGC_COUNT) <= bg ($C_BG_COUNT) — layer-C regressed!" >&2
  exit 1
fi
echo "   PASS: bg+C ($C_BGC_COUNT) > bg ($C_BG_COUNT) — layer-C improved composite"

echo "== Assertion: P1 obj construction x+frame match (in C harness output) =="
if echo "$C_OUTPUT" | grep -q "p1 obj assert:.*x=MATCH.*frame=MATCH"; then
  echo "   PASS: p1 obj x and frame match captured engine obj"
else
  if echo "$C_OUTPUT" | grep -q "p1 obj assert: SKIP"; then
    echo "   SKIP: p1 move_anim==100 (hidden sentinel)"
  else
    echo "FAIL: p1 obj assert did not confirm x=MATCH and frame=MATCH" >&2
    exit 1
  fi
fi

echo "== Assertion: bg+C+P1 match >= bg+C match (no regression) =="
if [ "$C_BGCP1_COUNT" -lt "$C_BGC_COUNT" ]; then
  echo "FAIL: bg+C+P1 ($C_BGCP1_COUNT) < bg+C ($C_BGC_COUNT) — P1 draw regressed!" >&2
  exit 1
fi
echo "   PASS: bg+C+P1 ($C_BGCP1_COUNT) >= bg+C ($C_BGC_COUNT)"

echo "== Assertion: P2 absent on level 1 — planes unchanged =="
if echo "$C_OUTPUT" | grep -q "P2 absent.*UNCHANGED"; then
  echo "   PASS: entity_draw_p2 no-op when p2_cell==-1"
else
  echo "FAIL: p2 absent assertion not found in C harness output" >&2
  exit 1
fi

echo ""
echo "== Summary =="
echo "   bg:      $C_BG_COUNT/64000"
echo "   bg+C:    $C_BGC_COUNT/64000"
echo "   bg+C+P1: $C_BGCP1_COUNT/64000"
echo "   P2: absent on level 1 (deferred to Task 6)"
