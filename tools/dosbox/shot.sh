#!/usr/bin/env bash
# One-off PNG screenshot of BUMPYP.EXE at a given frame, driven by an input script.
# Usage: shot.sh <input-script> <shot-frame> <out.png> [extra dbx logging env...]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
TMP="${TMPDIR:-/tmp}"

DBX_BIN="local/toolchain/dosbox-x-src/src/dosbox-x"
GAME_DIR="local/build/capture/game"
CONF_TMPL="tools/dosbox/bumpy-capture.conf"
PLAY_EXE="local/build/src/BUMPYP.EXE"

SCRIPT="$1"; SHOT_FRAME="$2"; OUT_PNG="$3"

# Stage the freshly-built playable EXE into the capture game dir.
\cp -f "$ROOT/$PLAY_EXE" "$ROOT/$GAME_DIR/BUMPYP.EXE"

CONF="$TMP/shot.conf"; \rm -f "$CONF"
sed -e 's/[[:space:]]*#.*$//' "$CONF_TMPL" \
  | grep -vE '^[[:space:]]*$|^#' \
  | sed -e "s#/ABSOLUTE/PATH/TO/local/build/capture/game#$ROOT/$GAME_DIR#" \
        -e "s#^BUMPY.EXE#BUMPYP.EXE#" > "$CONF"

SHOT_BIN="$TMP/shot_${SHOT_FRAME}.bin"; \rm -f "$SHOT_BIN"
HOME="$TMP" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  UV_CACHE_DIR="$TMP/uvcache" \
  BUMPYCAP_SCAN_INJECT=1 \
  BUMPYCAP_SCRIPT="$ROOT/$SCRIPT" \
  BUMPYCAP_DGROUP=0x3fe4 \
  BUMPYCAP_OFF_GAMEMODE=0xaca4 \
  BUMPYCAP_OFF_CURLEVEL=0x05da \
  BUMPYCAP_SHOT_FRAME="$SHOT_FRAME" \
  BUMPYCAP_SHOT_OUT="$SHOT_BIN" \
  timeout -k 5 60 "$DBX_BIN" -conf "$CONF" -nomenu -nogui >"$TMP/shot.log" 2>&1 || true

if [ ! -s "$SHOT_BIN" ]; then
    echo "NO SHOT produced at frame $SHOT_FRAME"; tail -8 "$TMP/shot.log"; exit 1
fi
uv run python tools/dosbox/shot_to_png.py "$SHOT_BIN" "$OUT_PNG" 2>>"$TMP/shot.log" \
  || { echo "PNG decode failed"; tail -8 "$TMP/shot.log"; exit 1; }
echo "wrote $OUT_PNG ($(stat -c%s "$OUT_PNG") bytes)"
# tail the mode/level log lines
grep -iE "mode=|level=" "$TMP/shot.log" | tail -6
