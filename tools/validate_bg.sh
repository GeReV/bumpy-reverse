#!/usr/bin/env bash
# Validate the background tile-build port (src/bg_render.c) byte-exact against the
# engine plane capture (local/build/render/bg_oracle.bin), and confirm it builds
# under the Open Watcom 16-bit DOS toolchain.
#
# The oracle is produced by: BG_ORACLE=1 uv run python tools/sprite_oracle.py
# (boots the real BUMPY.EXE, hooks restore_bg_tile_run during start_level, and
# snapshots the VGA planes per playfield cell + the PAV atlas + the level map).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ORACLE="local/build/render/bg_oracle.bin"
if [ ! -f "$ORACLE" ]; then
  echo "missing $ORACLE — run: BG_ORACLE=1 uv run python tools/sprite_oracle.py" >&2
  exit 1
fi

echo "== Open Watcom 16-bit compile check =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx bg_render.c -fo="${TMPDIR:-/tmp}/bg_render.obj" )
echo "   bg_render.c builds clean (wcc -ml -wx)"

echo "== host byte-exact replay vs engine (tile blit) =="
OUT="${TMPDIR:-/tmp}/bg_ctest"
cc -O2 -Wall -o "$OUT" tools/bg_ctest.c
"$OUT" "$ORACLE"
