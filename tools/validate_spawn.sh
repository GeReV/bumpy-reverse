#!/usr/bin/env bash
# Validate the reconstructed level-load entity-placement orchestrator (src/spawn.c,
# spawn_and_draw_level_entities 1000:2a78) against the Phase-8 T1 multi-level capture
# (local/build/render/spawn_trace.bin, magic SPWNTRC1 v2) via the REPLAY HARNESS
# tools/spawn_ctest.c:
#   1. src/spawn.c compiles clean under the Open Watcom 16-bit DOS toolchain
#      (wcc -ml -bt=dos -zq -wx);
#   2. the host replay harness builds (cc -O2 -Wall -Werror) and runs over every
#      captured level (incl. the layer-B-firing levels 2/3/4/5/6/8/9).  Per run it:
#        (A) SEMANTIC-STATE DIFFERENTIAL — seeds the run's ENTRY snapshot (the 7
#            channel records + spawn globals + tilemap/header/spawn tables from the
#            level SEED block), calls spawn_and_draw_level_entities(), and asserts the
#            3 A + 4 B channel records AND the spawn globals (p1_cell, level_exit_cell,
#            items_remaining, p2_cell, p2_move_state, p2_ai_threshold, p2_frame_base)
#            == the run's EXIT snapshot;
#        (B) DESCRIPTOR-LEVEL DIFFERENTIAL — asserts the host's per-cell fills
#            (layer-0 A record / layer-1 B record / layer-2 spawn-own-C blit
#            descriptor) == the trace's DIRECT-subset fills, in order.  The nested
#            blit_sprite calls inside draw_anim_channels_a/b (which the T1 oracle also
#            captured as layer-2) are SEPARATED from spawn's own layer-C blits via the
#            trace's per-fill layer tag + call structure (a layer-2 fill is nested iff
#            it immediately follows a layer-0/1 draw); the spawn-own-C count is also
#            asserted == the tilemap layer-C non-zero count.  Those nested descriptors
#            are draw_anim_channels_a/b's behavior (Phase-5 descriptor-validated), not
#            re-validated here.  See tools/spawn_ctest.c header for the full rationale.
#
# Then PERTURBATION-PROVES the gate: corrupt a seeded layer-A tilemap cell and assert
# the harness now FAILS (exit 1) — proving the comparator has teeth.
#
# Exit 0 iff spawn.c builds clean AND the differential has ZERO failures AND the
# perturbation run fails as expected.  Expected: FAIL=0, 9 runs, 7 with layer-B.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TRACE="${1:-local/build/render/spawn_trace.bin}"
if [ ! -f "$TRACE" ]; then
    echo "ERROR: spawn trace not found: $TRACE" >&2
    echo "  (Phase-8 T1 capture; regenerate via tools/spawn_oracle.py if missing)" >&2
    exit 2
fi

echo "== Open Watcom 16-bit compile check (src/spawn.c) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh \
    && wcc -ml -bt=dos -zq -wx spawn.c -fo="${TMPDIR:-/tmp}/spawn.obj" )
echo "   spawn.c builds clean (wcc -ml -bt=dos -zq -wx)"

echo "== host replay harness: semantic-state + descriptor differential =="
AOUT="${TMPDIR:-/tmp}/spawn_ctest"
cc -O2 -Wall -Werror -o "$AOUT" tools/spawn_ctest.c
"$AOUT" "$TRACE"

echo "== perturbation proof: corrupt a seeded layer-A cell -> MUST fail =="
if "$AOUT" "$TRACE" --perturb >/dev/null 2>&1; then
    echo "   ERROR: perturbation did NOT fail — the comparator is not gating!" >&2
    exit 1
fi
echo "   perturbation correctly FAILED (gate has teeth)"

echo "== validate_spawn: PASS =="
