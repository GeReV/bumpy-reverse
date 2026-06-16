#!/usr/bin/env bash
# Batch differential validation of the reconstructed VEC renderer.
#
# Builds BVEC.EXE, then for each oracle-backed screen runs it under the
# pure-Python DOS emulator (tools/run_bvec.py), captures the planar frame, and
# diffs it pixel-for-pixel against the oracle PNG via tools/vec_diff.py --planar.
# Prints PASS/FAIL per screen and exits non-zero if any screen mismatches.
#
# Scope: the op12-free screens only — TITRE (op4 -> direct planar) and SCORE
# (op0 -> raw planar). DESSFIN / MASKBUMP / BUMPRESE decompress to nested
# op12 record streams and are added here once op12 lands (Plan 4).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Screens to check: "<VEC name> <oracle png>"
SCREENS=(
  "TITRE titre.png"
  "SCORE score.png"
)

# shellcheck disable=SC1091
source local/toolchain/open-watcom/ow-env.sh

echo "== building BVEC.EXE =="
( cd src && wmake ) >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 2; }

fails=0
for entry in "${SCREENS[@]}"; do
  name="${entry%% *}"
  oracle="${entry##* }"
  rm -f "local/build/render/${name}.PLN"
  timeout 300 python3 tools/run_bvec.py --args "${name}.VEC ${name}.PLN" >/dev/null 2>&1
  verdict="$(python3 tools/vec_diff.py --planar "local/build/render/${name}.PLN" \
              "results/images/${oracle}" --vec "local/build/capture/game/${name}.VEC" 2>&1)"
  rc=$?
  if [ "$rc" -eq 0 ]; then
    printf '%-9s %-22s PASS\n' "$name" "$verdict"
  else
    printf '%-9s %-22s FAIL\n' "$name" "$verdict"
    fails=$((fails + 1))
  fi
done

echo
if [ "$fails" -eq 0 ]; then
  echo "all ${#SCREENS[@]} screens pixel-exact"
  exit 0
fi
echo "${fails} screen(s) FAILED"
exit 1
