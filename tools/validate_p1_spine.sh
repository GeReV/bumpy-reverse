#!/usr/bin/env bash
# Validate the reconstructed Player-1 per-tick SPINE (Phase-9 T3) against the capture
# local/build/render/p1_spine_trace.bin (magic P1SPINE1) via tools/p1_spine_ctest.c.
#
#   1. player.c / player2.c / level.c / game.c compile clean under the Open Watcom
#      16-bit DOS toolchain (wcc -ml -bt=dos -zq -wx);
#   2. the host replay harness extracts the level.c + game.c T3 functions verbatim
#      (all_entries_flag_set / init_view_anim_descriptors / game_post_present /
#      game_post_input) into ${TMPDIR}/p1_spine_extracted.h — NO source copy, the gate
#      runs the REAL reconstructed bodies — then host-compiles + runs the differential:
#        (A) SEMANTIC-STATE diff for the scalar fns (grid / bbox / pending / all_entries);
#        (B) VIEW-DESCRIPTOR diff for render/erase/draw/pending over the plane-exact
#            Phase-0 blitter;
#        (C) INIT-DESCRIPTOR diff for init_view_anim_descriptors (15 view structs).
#   3. a PERTURBATION run (--perturb) corrupts a seeded field per record and confirms
#      the gate then FAILS — proving it is a genuine differential.
#
# Exit 0 iff all four modules build clean AND the differential has ZERO failures AND
# the perturbation run correctly FAILS.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
TMP="${TMPDIR:-/tmp}"

TRACE="${1:-local/build/render/p1_spine_trace.bin}"
if [ ! -f "$TRACE" ]; then
    echo "ERROR: P1 spine trace not found: $TRACE" >&2
    echo "  (Phase-9 T3 capture; regenerate via tools/p1_spine_oracle.py if missing)" >&2
    exit 2
fi

echo "== Open Watcom 16-bit compile check (player/player2/level/game) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && for m in player player2 level game; do
         wcc -ml -bt=dos -zq -wx $m.c -fo="$TMP/$m.obj"
       done )
echo "   player.c / player2.c / level.c / game.c build clean (wcc -ml -bt=dos -zq -wx)"

# ── Extract the level.c + game.c T3 functions verbatim into a host header ──────────
# (these two TUs pull <dos.h> + the render/loop pipeline and cannot host-include
#  wholesale; extracting just the four T3 fn BODIES keeps the gate on the REAL source.)
EXTRACT="$TMP/p1_spine_extracted.h"
{
    echo "/* AUTO-EXTRACTED by tools/validate_p1_spine.sh — DO NOT EDIT.  The verbatim"
    echo "   level.c + game.c Phase-9 T3 function bodies (real source, sliced for the"
    echo "   host harness which cannot include those TUs wholesale). */"
    # all_entries_flag_set from level.c (from its signature line to the closing brace).
    awk '/^u8 all_entries_flag_set\(void\)$/{p=1} p{print} p&&/^}$/{exit}' src/level.c
    echo ""
    # init_view_anim_descriptors from game.c.
    awk '/^void init_view_anim_descriptors\(void\)$/{p=1} p{print} p&&/^}$/{exit}' src/game.c
    echo ""
    # game_post_present from game.c.
    awk '/^void game_post_present\(void\)$/{p=1} p{print} p&&/^}$/{exit}' src/game.c
    echo ""
    # game_post_input from game.c.
    awk '/^void game_post_input\(void\)$/{p=1} p{print} p&&/^}$/{exit}' src/game.c
} > "$EXTRACT"
cp "$EXTRACT" tools/p1_spine_extracted.h   # the ctest #includes it from tools/
echo "   extracted level.c/game.c T3 fns -> tools/p1_spine_extracted.h"

echo "== host replay harness: P1 spine semantic-state + descriptor differential =="
POUT="$TMP/p1_spine_ctest"
cc -O2 -Wall -o "$POUT" tools/p1_spine_ctest.c
echo "-- main differential --"
"$POUT" "$TRACE"
echo
echo "-- perturbation proof (corrupt a seeded field per record -> expect FAIL caught) --"
"$POUT" --perturb "$TRACE"

# clean up the in-tree extracted header (it is a build artifact, not source).
rm -f tools/p1_spine_extracted.h
echo
echo "validate_p1_spine: PASS (differential clean + perturbation caught)"
