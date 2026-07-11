#!/usr/bin/env bash
# DGROUP-sweep harness: build N BUMPYP variants whose only difference is the link
# DGROUP base, capture each at world-2, and fingerprint the platform region -- to
# bisect the DGROUP-layout-sensitive platform-erase bug (renders fine at 0x4f20,
# broken at 0x43c9; see docs / memory bumpy-w2-platform-erase-rootcause).
#
# How a variant is made: we compile a tiny far-data "pad" translation unit of a chosen
# size and link it FIRST, ahead of the real objects.  A far-data segment lands before
# DGROUP, so it translates the whole data image up by ceil(size/16) paragraphs -- the
# DGROUP base moves by exactly that, everything else is otherwise identical.  The real
# objects are REUSED from the normal build (only the pad recompiles + a relink per
# variant), so a sweep is fast.
#
# All layout-dependent capture offsets are re-derived from each variant's own map via
# map_offsets.py, so nothing is stale across the shift.
#
# Usage:
#   dgroup_sweep.sh [--bases "0x43c9 0x4500 ..."] [--frame N] [--secs N] [--out DIR]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
SRC="$ROOT/src"
WROOT="$ROOT/local/toolchain/open-watcom"
export WATCOM="$WROOT" INCLUDE="$WROOT/h" PATH="$WROOT/binl64:$PATH"
WCC="$WROOT/binl64/wcc"
WCL="$WROOT/binl64/wcl"

BASE_DGROUP=0x43c9   # clean-build link DGROUP (pad=0); other bases are >= this
DEFAULT_BASES="0x43c9 0x4500 0x4700 0x4900 0x4b00 0x4d00 0x4f20 0x5100"
BASES="$DEFAULT_BASES"
FRAME=1600
SECS=70
OUTDIR="$ROOT/local/build/dgroup-sweep"
REUSE=0
CALIB_IN=""   # if set, score against this existing mask instead of recalibrating

while [ "$#" -gt 0 ]; do
    case "$1" in
        --bases) BASES="$2"; shift 2 ;;
        --frame) FRAME="$2"; shift 2 ;;
        --secs)  SECS="$2"; shift 2 ;;
        --out)   OUTDIR="$2"; shift 2 ;;
        --reuse) REUSE=1; shift ;;
        --calib) CALIB_IN="$2"; shift 2 ;;
        *) echo "dgroup_sweep: unknown arg $1" >&2; exit 2 ;;
    esac
done

# OUTDIR must be absolute: the per-variant link runs inside a (cd "$SRC") subshell, so
# a relative --out would not resolve there.
case "$OUTDIR" in
    /*) ;;
    *)  OUTDIR="$ROOT/$OUTDIR" ;;
esac
mkdir -p "$OUTDIR"

# --- 1. build the real objects once (also produces the baseline link) ---
echo "== building play objects (once) =="
bash "$SRC/build.sh" play >"$OUTDIR/build.log" 2>&1 || {
    echo "dgroup_sweep: baseline build failed; see $OUTDIR/build.log" >&2; exit 1; }

# canonical object order (matches Makefile PLAY_OBJS)
OBJLIST="play/main.obj play/game.obj play/game_stubs.obj play/host_video.obj \
play/host_render.obj play/host_timer.obj play/host_input.obj play/view_setup.obj \
play/host_boot.obj play/host_resource.obj play/host_gfx.obj play/config_screens.obj \
play/level.obj play/input.obj play/player.obj play/player2.obj play/items.obj \
play/anim.obj play/spawn.obj play/sound.obj play/screens.obj play/vec.obj \
play/op12.obj play/video.obj play/sprite.obj play/sprite_anim.obj \
play/sprite_chain.obj play/sprite_blit.obj play/bg_render.obj play/entity.obj \
play/gfx_overlay.obj play/dosio.obj play/prng.obj play/globals.obj \
play/move_scripts.obj play/worldmap_data.obj play/anim_data.obj play/bvec_buf1.obj \
play/bvec_buf2.obj"

link_dgroup_of() { # <map> -> hex link DGROUP paragraph (no 0x)
    grep -E "^DGROUP" "$1" | head -1 | sed -E 's/^DGROUP[[:space:]]+([0-9a-fA-F]+):.*/\1/'
}

# --- 2. build + capture each variant ---
declare -a ROW_BASE ROW_DG ROW_BIN
i=0
for TARGET in $BASES; do
    paras=$(( TARGET - BASE_DGROUP ))
    if [ "$paras" -lt 0 ]; then
        echo "-- skip $TARGET (< baseline $(printf 0x%x $BASE_DGROUP))" >&2
        continue
    fi
    padbytes=$(( paras * 16 ))
    tag=$(printf "%04x" "$TARGET")
    exe="$OUTDIR/v_$tag.exe"
    map="$OUTDIR/v_$tag.map"
    bin="$OUTDIR/v_$tag.bin"
    png="$OUTDIR/v_$tag.png"

    echo "== variant target=$(printf 0x%04x $TARGET) pad=${padbytes}B =="
    if [ "$REUSE" -eq 1 ] && [ -s "$bin" ] && [ -s "$map" ]; then
        actual=$(link_dgroup_of "$map")
        echo "  reuse existing capture (link DGROUP=0x$actual)"
        ROW_BASE[$i]=$(printf "0x%04x" "$TARGET"); ROW_DG[$i]="0x$actual"; ROW_BIN[$i]="$bin"
        i=$((i+1)); continue
    fi
    ( cd "$SRC"
      if [ "$padbytes" -eq 0 ]; then
          "$WCL" -ml -bt=dos -zq -k0x4000 -fm="$map" -fe="$exe" $OBJLIST \
              >"$OUTDIR/link_$tag.log" 2>&1
      else
          printf 'char __far g_dgroup_pad[%d];\n' "$padbytes" > "$OUTDIR/pad_$tag.c"
          "$WCC" -ml -bt=dos -zq -wx -fo="$OUTDIR/pad_$tag.obj" "$OUTDIR/pad_$tag.c" \
              >"$OUTDIR/link_$tag.log" 2>&1
          "$WCL" -ml -bt=dos -zq -k0x4000 -fm="$map" -fe="$exe" \
              "$OUTDIR/pad_$tag.obj" $OBJLIST >>"$OUTDIR/link_$tag.log" 2>&1
      fi
    ) || { echo "  link failed; see $OUTDIR/link_$tag.log" >&2; continue; }

    actual=$(link_dgroup_of "$map")
    echo "  link DGROUP=0x$actual (target $(printf 0x%04x $TARGET))"

    echo "  capturing world-2 render (frame $FRAME) ..."
    bash "$ROOT/tools/dosbox/cap_w2.sh" \
        --exe "$exe" --map "$map" --bin "$bin" --png "$png" \
        --frame "$FRAME" --poke-val 2 --secs "$SECS" >"$OUTDIR/cap_$tag.log" 2>&1
    if [ ! -s "$bin" ]; then
        echo "  NO capture; see $OUTDIR/cap_$tag.log" >&2
        continue
    fi
    ROW_BASE[$i]=$(printf "0x%04x" "$TARGET")
    ROW_DG[$i]="0x$actual"
    ROW_BIN[$i]="$bin"
    i=$((i+1))
done

NVAR=$i
if [ "$NVAR" -lt 2 ]; then
    echo "dgroup_sweep: need >=2 successful variants to calibrate; got $NVAR" >&2
    exit 1
fi

# --- 3. calibrate from endpoints (first=buggy 0x43c9, last=good high base),
#        or reuse an existing mask if --calib was given ---
if [ -n "$CALIB_IN" ]; then
    CALIB="$CALIB_IN"
    echo "== reusing existing platform mask: $CALIB =="
else
    BUGGY_BIN="${ROW_BIN[0]}"
    GOOD_BIN="${ROW_BIN[$((NVAR-1))]}"
    CALIB="$OUTDIR/calib.json"
    echo "== calibrating platform mask from ${ROW_DG[0]} (buggy) vs ${ROW_DG[$((NVAR-1))]} (good) =="
    python3 "$ROOT/tools/dosbox/platform_fingerprint.py" calibrate "$GOOD_BIN" "$BUGGY_BIN" "$CALIB"
fi

# --- 4. score every variant + print the sweep table ---
echo
echo "=================== DGROUP SWEEP RESULT ==================="
printf "%-10s %-10s %-10s %s\n" "target" "dgroup" "good_frac" "verdict"
CSV="$OUTDIR/sweep.csv"
echo "target,dgroup,good_frac,n_good,n_buggy,n_other" > "$CSV"
j=0
while [ "$j" -lt "$NVAR" ]; do
    js=$(python3 "$ROOT/tools/dosbox/platform_fingerprint.py" score "${ROW_BIN[$j]}" \
            --calib "$CALIB" --json)
    gf=$(printf '%s' "$js" | jq -r '.good_frac')
    ng=$(printf '%s' "$js" | jq -r '.n_good')
    nb=$(printf '%s' "$js" | jq -r '.n_buggy')
    no=$(printf '%s' "$js" | jq -r '.n_other')
    verdict=$(awk -v g="$gf" 'BEGIN{print (g>=0.8?"GOOD":(g<=0.2?"BUGGY":"MIXED"))}')
    printf "%-10s %-10s %-10s %s\n" "${ROW_BASE[$j]}" "${ROW_DG[$j]}" "$gf" "$verdict"
    echo "${ROW_BASE[$j]},${ROW_DG[$j]},$gf,$ng,$nb,$no" >> "$CSV"
    j=$((j+1))
done
echo "=========================================================="
echo "artifacts: $OUTDIR (v_*.png renders, sweep.csv, calib.json)"
