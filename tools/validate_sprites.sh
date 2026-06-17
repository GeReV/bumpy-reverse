#!/usr/bin/env bash
# Validate the reconstructed sprite-bank load transform byte-exact against the
# real engine.  Builds BSPRITE.EXE, runs it under the Unicorn host over
# BUMSPJEU.BIN, and diffs the transformed data region against the engine oracle
# (bank_inmem.bin, captured by tools/sprite_oracle.py).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"

ORACLE="local/build/render/bank_inmem.bin"
GAME="local/originals/old-games/bumpy"
if [ ! -f "$ORACLE" ]; then
  echo "missing oracle $ORACLE -- generate it first: uv run python tools/sprite_oracle.py"
  exit 2
fi

# shellcheck disable=SC1091
source local/toolchain/open-watcom/ow-env.sh
echo "== building BSPRITE.EXE =="
( cd src && wmake ) >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 2; }

rm -f local/build/render/SPROUT.BIN
uv run python tools/run_bvec.py --exe local/build/src/BSPRITE.EXE --cpu unicorn \
  --in-dir "$GAME" --out-dir local/build/render \
  --args "BUMSPJEU.BIN 2" --max-instr 120000000 >/dev/null 2>&1

verdict="$(python3 tools/sprite_diff.py local/build/render/SPROUT.BIN "$ORACLE")"
rc=$?
printf 'sprite bank transform  %-26s %s\n' "$verdict" "$([ "$rc" -eq 0 ] && echo PASS || echo FAIL)"
exit "$rc"
