#!/usr/bin/env bash
# Validate the planar-VGA sprite blitter port (src/sprite_blit.c) byte-exact
# against the engine plane capture (local/build/render/blit_oracle.bin), and
# confirm it builds under the Open Watcom 16-bit DOS toolchain.
#
# The oracle is produced by:  BLIT_ORACLE=1 uv run python tools/sprite_oracle.py
# (boots the real BUMPY.EXE under the Unicorn DOS harness, snapshots the VGA
# planes before/after each sprite_blit_planar_vga (1cec:10e1) call).
#
# Validation runs the ACTUAL src/sprite_blit.c on the host (16-bit types + far/huge
# shimmed) and replays it over every captured blit — testing identical source.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ORACLE="local/build/render/blit_oracle.bin"
if [ ! -f "$ORACLE" ]; then
  echo "missing $ORACLE — run: BLIT_ORACLE=1 uv run python tools/sprite_oracle.py" >&2
  exit 1
fi

echo "== Open Watcom 16-bit compile check =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx sprite_blit.c -fo="${TMPDIR:-/tmp}/sprite_blit.obj" )
echo "   sprite_blit.c builds clean (wcc -ml -wx)"

echo "== host byte-exact replay vs engine =="
OUT="${TMPDIR:-/tmp}/blit_ctest"
cc -O2 -Wall -o "$OUT" tools/blit_ctest.c
"$OUT" "$ORACLE"
