#!/usr/bin/env bash
# validate_host_compose.sh — Plan A Task 2 keystone gate.
#
# Proves the playable host render wiring composes byte-exact:
#   1. Host-compile tools/composite_ctest.c and dump its validated 4-plane
#      reference (the already-trusted composite oracle output).
#   2. Host-compile tools/host_compose_ctest.c — which links the REAL host render
#      layer (src/host/host_render.c, -DBUMPY_PLAYABLE): host_fb_init allocates the
#      flat 4-plane framebuffer, registers the page table, and routes the P1 sprite
#      draw through the real p1_blit_sprite_leaf into host_framebuffer.
#   3. Assert host_framebuffer == the composite reference, plane-for-plane.
#   4. Watcom link-check both EXEs (default BUMPY byte-unchanged is covered by
#      validate_integration.sh; here we only confirm the play target links).
#
# Requires: local/build/render/frame_oracle.bin + bank_inmem.bin (sprite_oracle.py).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ORACLE="local/build/render/frame_oracle.bin"
BANK="local/build/render/bank_inmem.bin"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ ! -f "$ORACLE" ] || [ ! -f "$BANK" ]; then
  echo "missing oracle/bank — run: FRAME_ORACLE=1 uv run python tools/sprite_oracle.py" >&2
  exit 1
fi

echo "== [1/3] dump composite reference planes =="
cc -O2 -o "$TMP/composite_ctest" tools/composite_ctest.c
"$TMP/composite_ctest" "$ORACLE" "$BANK" --dump-planes "$TMP/ref.planes" >/dev/null
echo "   reference dumped ($(stat -c%s "$TMP/ref.planes") bytes)"

echo "== [2/3] host compose through the REAL render leaves =="
cc -O2 -Isrc -o "$TMP/hcc" tools/host_compose_ctest.c
"$TMP/hcc" "$ORACLE" "$BANK" "$TMP/ref.planes"

echo "== [3/3] play target links clean (host render layer) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh >/dev/null 2>&1 && wmake play >/dev/null 2>&1 )
echo "   BUMPYP.EXE links clean."

echo "== validate_host_compose: PASS =="
