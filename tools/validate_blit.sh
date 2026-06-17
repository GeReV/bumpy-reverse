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

BANK="local/build/render/bank_inmem.bin"

echo "== Open Watcom 16-bit compile check =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx sprite_blit.c  -fo="${TMPDIR:-/tmp}/sprite_blit.obj" \
    && wcc -ml -bt=dos -zq -wx sprite_chain.c -fo="${TMPDIR:-/tmp}/sprite_chain.obj" \
    && wcc -ml -bt=dos -zq -wx sprite_anim.c  -fo="${TMPDIR:-/tmp}/sprite_anim.obj" )
echo "   sprite_blit.c + sprite_chain.c + sprite_anim.c build clean (wcc -ml -wx)"

echo "== host byte-exact replay vs engine (full sprite pipeline) =="
BOUT="${TMPDIR:-/tmp}/blit_ctest"
COUT="${TMPDIR:-/tmp}/chain_ctest"
AOUT="${TMPDIR:-/tmp}/anim_ctest"
cc -O2 -Wall -o "$BOUT" tools/blit_ctest.c
cc -O2 -Wall -o "$COUT" tools/chain_ctest.c
cc -O2 -Wall -o "$AOUT" tools/anim_ctest.c
echo "-- anim select (frame index -> object header):"
"$AOUT" "$ORACLE" "$BANK"
echo "-- chain (object -> descriptor):"
"$COUT" "$ORACLE"
echo "-- blitter (descriptor -> VGA planes):"
"$BOUT" "$ORACLE"
