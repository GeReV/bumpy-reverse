#!/usr/bin/env bash
# Capture each world's freshly-decoded .BUM level table from the emulator (ground truth),
# bypassing the pure-Python .BUM decoder which has a data-dependent op12 bug.
# Each D<n>.BUM holds that world's full ~15-level table, so 9 runs cover all 126 levels.
# ONE emulator at a time; each run hard-bounded by `timeout`. Output: build/render/bum/world<n>.bum
set -uo pipefail
cd "$(dirname "$0")/../.."
PY=tools/venv-emu/bin/python
mkdir -p build/render/bum
for n in 1 2 3 4 5 6 7 8 9; do
  out="local/build/render/bum/world${n}.bum"; pal="local/build/render/bum/world${n}.pal.json"
  if [ -f "$out" ] && [ -f "$pal" ]; then echo "world $n: already captured (.bum + palette)"; continue; fi
  echo "=== world $n: emulating (bounded 400s) ... ==="
  \rm -f "local/build/render/lvl/bum_run${n}.log"
  env DOSEMU_LEVEL="$n" timeout 400 "$PY" tools/render/dosemu.py > "local/build/render/lvl/bum_run${n}.log" 2>&1
  rc=$?
  if [ -f "$out" ] && [ -f "$pal" ]; then
    echo "world $n: OK (.bum $(wc -c <"$out") B + palette)"
  else
    echo "world $n: FAILED (rc=$rc, bum=$([ -f "$out" ] && echo y || echo n) pal=$([ -f "$pal" ] && echo y || echo n) — see build/render/lvl/bum_run${n}.log)"
  fi
done
echo "=== capture done: $(ls build/render/bum/*.bum 2>/dev/null|wc -l) .bum, $(ls build/render/bum/*.pal.json 2>/dev/null|wc -l) palettes ==="