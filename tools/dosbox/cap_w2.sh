#!/usr/bin/env bash
# Drive a BUMPYP.EXE build to a world-2 level headlessly and SHOT the render.
#
# Unlike the older run_w2.sh / shot.sh, EVERY layout-dependent BUMPYCAP_* value
# (runtime DGROUP, _current_level / _game_mode offsets, the world-forcing poke target)
# is DERIVED from the build's own linker map via tools/dosbox/map_offsets.py -- nothing
# is hardcoded.  That makes this safe to point at any build, including the deliberately
# DGROUP-shifted variants produced by dgroup_sweep.sh.
#
# Usage:
#   cap_w2.sh --exe <BUMPYP.EXE> --map <BUMPYP.map> --bin <out.bin> --png <out.png> \
#             [--frame N] [--poke-val V] [--boot SCRIPT] [--secs N] [--cycles N] \
#             [--stride N] [--count N]
#
# Defaults reproduce the b3fedee clean-build w2 capture.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
TMP="${TMPDIR:-/tmp}"

DBX_BIN="local/toolchain/dosbox-x-src/src/dosbox-x"
GAME_DIR="local/build/capture/game"
CONF_TMPL="tools/dosbox/bumpy-capture.conf"

# --- defaults ---
EXE="local/build/src/BUMPYP.EXE"
MAP="src/play/BUMPYP.map"
OUT_BIN=""
OUT_PNG=""
FRAME=1600
POKE_VAL=2
BOOT="tools/dosbox/scripts/bumpyp-w2.txt"
SECS=120
CYCLES=40000
STRIDE=1
COUNT=1
MEMDUMP=""   # optional OFF:LEN:OUT[:SEG] — dump guest memory at the shot frame
EXTRA_ENV="" # optional space-separated K=V passthrough (e.g. BUMPYCAP_WWATCH=...)
LOGCOPY=""   # optional path to copy the dosbox run log to

while [ "$#" -gt 0 ]; do
    case "$1" in
        --exe)      EXE="$2"; shift 2 ;;
        --map)      MAP="$2"; shift 2 ;;
        --bin)      OUT_BIN="$2"; shift 2 ;;
        --png)      OUT_PNG="$2"; shift 2 ;;
        --frame)    FRAME="$2"; shift 2 ;;
        --poke-val) POKE_VAL="$2"; shift 2 ;;
        --boot)     BOOT="$2"; shift 2 ;;
        --secs)     SECS="$2"; shift 2 ;;
        --cycles)   CYCLES="$2"; shift 2 ;;
        --stride)   STRIDE="$2"; shift 2 ;;
        --count)    COUNT="$2"; shift 2 ;;
        --memdump)  MEMDUMP="$2"; shift 2 ;;
        --env)      EXTRA_ENV="$2"; shift 2 ;;
        --log)      LOGCOPY="$2"; shift 2 ;;
        *) echo "cap_w2: unknown arg $1" >&2; exit 2 ;;
    esac
done

[ -n "$OUT_BIN" ] || { echo "cap_w2: --bin required" >&2; exit 2; }
[ -s "$EXE" ] || { echo "cap_w2: EXE not found: $EXE" >&2; exit 2; }
[ -s "$MAP" ] || { echo "cap_w2: MAP not found: $MAP" >&2; exit 2; }

# --- derive all offsets from the linker map (no hardcoded constants) ---
OFFS="$(python3 tools/dosbox/map_offsets.py "$MAP" --env)" || {
    echo "cap_w2: map_offsets failed on $MAP" >&2; exit 2; }
set -a
eval "$OFFS"
set +a
echo "cap_w2: derived DGROUP=$BUMPYCAP_DGROUP curlevel=$BUMPYCAP_OFF_CURLEVEL gamemode=$BUMPYCAP_OFF_GAMEMODE" >&2

# --- stage the freshly-built EXE into the mounted game dir ---
\cp -f "$EXE" "$GAME_DIR/BUMPYP.EXE"

# --- build a concrete dosbox conf from the template ---
CONF="$TMP/cap_w2.conf"; \rm -f "$CONF"
sed -e 's/[[:space:]]*#.*$//' "$CONF_TMPL" \
  | grep -vE '^[[:space:]]*$|^#' \
  | sed -e "s#/ABSOLUTE/PATH/TO/local/build/capture/game#$ROOT/$GAME_DIR#" \
        -e "s#^BUMPY.EXE#BUMPYP.EXE#" \
        -e "s#^cycles.*#cycles = fixed $CYCLES#" > "$CONF"

\rm -f "$OUT_BIN" "$OUT_BIN".* 2>/dev/null

# --- optional world-forcing poke (current_level = POKE_VAL over the load window) ---
POKE_ENV=()
if [ "$POKE_VAL" != "0" ]; then
    POKE_ENV=( BUMPYCAP_POKE_VAL="$POKE_VAL"
               BUMPYCAP_POKE_FSTART=400 BUMPYCAP_POKE_FEND=9000 )
    # BUMPYCAP_POKE_OFF already exported from the map (= _current_level dgroup off).
fi

# --- optional guest-memory dump at the shot frame: OFF:LEN:OUT[:SEG] ---
# OFF/LEN are relative to the runtime DGROUP base (derived above) unless SEG is given,
# in which case the base is SEG<<4 (an absolute far segment, e.g. a resolved far ptr).
MEMDUMP_ENV=()
if [ -n "$MEMDUMP" ]; then
    IFS=: read -r MD_OFF MD_LEN MD_OUT MD_SEG <<EOF
$MEMDUMP
EOF
    MEMDUMP_ENV=( BUMPYCAP_MEMDUMP_OFF="$MD_OFF" BUMPYCAP_MEMDUMP_LEN="$MD_LEN"
                  BUMPYCAP_MEMDUMP_OUT="$MD_OUT" )
    [ -n "${MD_SEG:-}" ] && MEMDUMP_ENV+=( BUMPYCAP_MEMDUMP_SEG="$MD_SEG" )
    \rm -f "$MD_OUT" 2>/dev/null
fi

LOG="$TMP/cap_w2.log"
env HOME="$TMP" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    BUMPYCAP_SCAN_INJECT=1 \
    BUMPYCAP_SCRIPT="$ROOT/$BOOT" \
    BUMPYCAP_SHOT_FRAME="$FRAME" \
    BUMPYCAP_SHOT_OUT="$OUT_BIN" \
    BUMPYCAP_SHOT_STRIDE="$STRIDE" \
    BUMPYCAP_SHOT_COUNT="$COUNT" \
    "${POKE_ENV[@]}" \
    "${MEMDUMP_ENV[@]}" \
    $EXTRA_ENV \
    timeout -k 5 "$SECS" "$DBX_BIN" -conf "$CONF" -nomenu -nogui >"$LOG" 2>&1 || true

[ -n "$MEMDUMP" ] && { grep -E 'BUMPYMEMDUMP' "$LOG" | tail -2 >&2 || true; }
[ -n "$LOGCOPY" ] && \cp -f "$LOG" "$LOGCOPY"

if [ ! -s "$OUT_BIN" ]; then
    echo "cap_w2: NO SHOT produced at frame $FRAME" >&2
    grep -iE "mode=|level=|BUMPYCAP frame=" "$LOG" | tail -6 >&2
    exit 1
fi

# --- decode to PNG if requested (pure-stdlib shot_to_png; no uv) ---
if [ -n "$OUT_PNG" ]; then
    python3 tools/dosbox/shot_to_png.py "$OUT_BIN" "$OUT_PNG" 2>>"$LOG" \
      || { echo "cap_w2: PNG decode failed" >&2; tail -6 "$LOG" >&2; exit 1; }
    echo "cap_w2: wrote $OUT_PNG ($(stat -c%s "$OUT_PNG") bytes)"
fi

# --- report the level/mode the capture actually reached ---
echo "cap_w2: reached ->"
grep -E 'BUMPYCAP frame=' "$LOG" \
  | sed -E 's/.*frame=([0-9]+).*level=([0-9]+) mode=([0-9]+) vganz=([0-9]+)/\1 L\2 M\3 nz\4/' \
  | awk '{k=$2" "$3; if(k!=prev){print; prev=k}}' | tail -12
