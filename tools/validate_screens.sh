#!/usr/bin/env bash
# Batch differential validation of the reconstructed VEC renderer.
#
# Builds BVEC.EXE, then for each oracle-backed screen runs it under the DOS
# emulator (tools/run_bvec.py), captures the planar frame, and diffs it
# pixel-for-pixel against the oracle PNG via tools/vec_diff.py --planar.
# Prints PASS/FAIL per screen and exits non-zero if any screen mismatches.
#
# Backend: the Unicorn CPU (--cpu unicorn, native speed via 'uv run python').
# The op12 screens decode through a deep in-place LZ pipeline — DESSFIN's
# record-2 overlap relocation alone copies ~6.9 MB (~634M emulated
# instructions), far beyond what the pure-Python CPU finishes in reasonable
# wall-clock; Unicorn runs each screen in seconds.
#
# Scope: all five oracle-backed screens —
#   TITRE    op4  -> direct planar
#   SCORE    op0  -> raw planar
#   MASKBUMP op4/op12 nested record stream (masked-blit compositor)
#   DESSFIN  op4/op12 nested record stream (heavy overlap relocation)
#   BUMPRESE op12 at record 0
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Screens to check: "<VEC name> <oracle png>"
SCREENS=(
  "TITRE titre.png"
  "SCORE score.png"
  "MASKBUMP maskbump.png"
  "DESSFIN dessfin.png"
  "BUMPRESE bumprese.png"
)

# Instruction cap: DESSFIN needs ~634M (the op12 reloc burst); 1.5B gives margin.
MAX_INSTR=1500000000

# shellcheck disable=SC1091
source local/toolchain/open-watcom/ow-env.sh

echo "== building BVEC.EXE =="
( cd src && wmake ) >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 2; }

fails=0
for entry in "${SCREENS[@]}"; do
  name="${entry%% *}"
  oracle="${entry##* }"
  rm -f "local/build/render/${name}.PLN"
  timeout 120 uv run python tools/run_bvec.py --cpu unicorn \
    --args "${name}.VEC ${name}.PLN" --max-instr "${MAX_INSTR}" >/dev/null 2>&1
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
