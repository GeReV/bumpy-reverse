#!/usr/bin/env bash
# Phase 1 (coarse oracle): drive the from-scratch emulator to each of the 9 WORLDS and
# capture its first puzzle level (default current_level_index) as ground truth.
# NOTE: this is ONE puzzle level per world, not all of them. The full set of puzzle
# levels per world is reversed out of the .BUM/.DEC tables in Phase 2 (pure Python).
# DOSEMU_LEVEL here selects the WORLD (D<n>), patched into the filename by the game.
# ONE emulator at a time.
# Usage: tools/render/render_levels_emu.sh [PAGE]   (PAGE = 0000 or 2000, default 2000)
set -euo pipefail
cd "$(dirname "$0")/../.."
PY=tools/venv-emu/bin/python
PAGE="${1:-2000}"
OUT=results/levels_png
mkdir -p "$OUT" build/render/lvl
for n in 1 2 3 4 5 6 7 8 9; do
  echo "=== world $n ==="
  DOSEMU_LEVEL="$n" "$PY" tools/render/dosemu.py > "local/build/render/lvl/emu_run$n.log" 2>&1 || {
    echo "  world $n FAILED (see build/render/lvl/emu_run$n.log)"; continue;
  }
  cp -f "local/build/render/dosemu_vga_p${PAGE}.png" "$OUT/world${n}_lvl1.png"
  echo "  -> $OUT/world${n}_lvl1.png"
done
echo "done"
