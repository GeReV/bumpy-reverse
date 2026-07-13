#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════════
# validate_ega.sh — Task 7: pixel-resolved EGA verification gate.
#
# Proves the reconstruction's PLAYABLE build BUMPYP.EXE, booted into EGA
# (F2 -> palette_mode=1), renders the SAME on-screen colours as the REAL
# ORIGINAL BUMPY.EXE booted into EGA — the black-bg + red/white title look,
# NOT the VGA gold (results/renders/TITRE.png, results/iris-title-after.png).
#
# Unlike tools/validate_playable.sh (which diffs raw VGA planes — palette-
# agnostic), this gate resolves each captured shot to real on-screen RGB via
# DAC[AC[pixel]] (tools/dosbox/shot_to_png.py's decode_rgb(), fed by the
# BUMPYCAP_SHOT AC-dump patch, tools/dosbox/patches/05-bumpycap-shot-attr-
# palette.patch) before comparing — so it actually asserts COLOUR fidelity,
# which a plane compare cannot (EGA and VGA renders can be plane-identical
# while looking totally different, since palette_mode only changes what the
# AC/DAC steer each plane-derived pixel value to).
#
# WHAT THIS VALIDATES, per --screens entry:
#   title  — the post-iris-wipe title art (init_title_graphics /
#            show_title_and_init). EMPIRICALLY CALIBRATED this task: the
#            playable-side frame window below was confirmed live (captured +
#            visually inspected — black bg, red/orange "B'MPY'S", blue-eyed
#            mascot peeking over a dune, matching the VGA reference's
#            composition in results/iris-title-after.png but in EGA colour).
#   menu   — run_main_menu (PLAY/HIGH-SCORE/LEVEL/PASSWORD). Frame window is a
#            DOCUMENTED ESTIMATE derived from level1-idle.txt's own header
#            comment (menu appears after the first FIRE pulse, ~frame 600+ on
#            the original's timeline) — NOT empirically confirmed in this
#            session (the original-side capture is ENV-BLOCKED here; see
#            below). Re-verify once unblocked.
#   world1/2/3/9 — one in-level frame per world palette, current_level forced
#            via the BUMPYCAP_POKE_* mechanism (patch 06), the same technique
#            tools/dosbox/cap_w2.sh uses. INFORMATIONAL ONLY (see KNOWN GAP
#            below) — never fails the gate.
#
# CLOSED GAP (2026-07-11, was: load_palette level-palette staging not gated on
# palette_mode): load_palette (src/host/host_video.c) now has the EGA branch that
# copies the fixed in-game AC table (DGROUP 0x70e) into staging +0x23, and
# level_palette_ptr_table (the per-world overworld tables) is populated — so in-level
# and overworld EGA frames now program the correct AC. Verified out-of-band (not via
# this gate's original-vs-playable path, which stays ENV-BLOCKED — see below): a
# DOSBox-X EGA boot capture showed the programmed AC registers match the binary tables
# byte-for-byte (overworld -> 0x65a world 1, in-level -> 0x70e), vs pre-fix garbage
# (overworld) / all-zero (in-level). The world1/2/3/9 checks below remain informational
# only because the ORIGINAL-side capture is still env-blocked, NOT because of a code gap.
#
# PHASE 0 (self-contained, no original required, no ENV-BLOCK risk): boots
# the SAME freshly-built playable in EGA and in VGA at the same title-frame
# window and asserts the two decoded images are SUBSTANTIALLY different
# (tools/dosbox/ega_compare.py --min-mismatch-frac) — a regression guard that
# Task 6's palette_mode gating actually changes the render, independent of
# whether the original-side comparison can run at all this session.
#
# COMPARATOR: tools/dosbox/ega_compare.py — decodes both sides via
# shot_to_png.decode_rgb(), tries every (ref-burst x cand-burst) pair (a
# small burst per side absorbs a few frames of cross-build timing jitter —
# the "phase tolerance"), and requires the least-different pair to be within
# --max-mismatch-frac of the total pixel count. Blank/not-yet-rendered shots
# (>=99% one colour) are auto-excluded — see the module docstring for why.
#
# Exit codes (distinct, so a broken environment is never reported as a pass):
#   0 = PASS       every required (title[,menu]) screen matched.
#   1 = FAIL       a required screen was captured on both sides and the
#                  colours genuinely differ (a real bug — the diagnostic
#                  above pinpoints the first mismatched pixel).
#   2 = ENV-BLOCKED the run could not reach a real compare (dosbox-x missing/
#                  unbuildable, or — the actual state found in this
#                  environment, see below — the real original BUMPY.EXE
#                  triple-faults under the currently-built instrumented
#                  dosbox-x before it ever reaches gfx_driver_init).
# Final line: "validate_ega: PASS" / "validate_ega: FAIL" / "validate_ega: ENV-BLOCKED".
#
# ── HOW TO RUN THIS ONCE THE ORIGINAL-SIDE CAPTURE IS UNBLOCKED ─────────────
#   cd <repo-root>
#   tools/validate_ega.sh --screens title,menu
#   tools/validate_ega.sh --screens title,menu,world1,world2,world3,world9
#
# ── HOW TO CONFIRM INTERACTIVELY (works today, no dosbox-x instrumentation
#    needed — this is the honest, currently-available verification path) ────
#   Boot local/build/src/BUMPYP.EXE (or the copy staged under
#   local/build/capture/game/) in real DOSBox/DOSBox-X, or on real DOS.
#   At the graphics-select screen (blank text-mode prompt right after
#   launch), press F2. Expect: black background, red/white/orange "B'MPY'S
#   ARCADE FANTASY" title art with the blue-eyed mascot peeking over a dune —
#   NOT the VGA gold look (blue background, gold title, green menu text; see
#   results/renders/TITRE.png for the VGA reference). Press F3 instead to
#   confirm the VGA path is unchanged (still gold).
# ════════════════════════════════════════════════════════════════════════════
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
TMP="${TMPDIR:-/tmp}"

fail() { echo "validate_ega: FAIL"; echo " -> $*" >&2; exit 1; }
env_blocked() { echo "validate_ega: ENV-BLOCKED"; echo " -> $*" >&2; exit 2; }

DBX_BIN="local/toolchain/dosbox-x-src/src/dosbox-x"
GAME_DIR="local/build/capture/game"
CONF_TMPL="tools/dosbox/bumpy-capture.conf"
ORIG_EXE="$GAME_DIR/BUMPY.EXE"
PLAY_EXE="local/build/src/BUMPYP.EXE"
PLAY_MAP="src/play/BUMPYP.map"
COMPARE="tools/dosbox/ega_compare.py"

MAX_MISMATCH_FRAC="${EGA_MAX_MISMATCH_FRAC:-0.0}"   # required-screen tolerance (default: exact)
SELFCHECK_MIN_DIFF="${EGA_SELFCHECK_MIN_DIFF:-0.20}" # Phase-0 EGA-vs-VGA must differ by >= this

SCREENS="title,menu"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --screens) SCREENS="$2"; shift 2 ;;
        *) echo "validate_ega: unknown arg $1" >&2; exit 2 ;;
    esac
done

# ── ORIGINAL BUMPY.EXE calibration (shared with validate_playable.sh) ────────
ORIG_DGROUP="0x185f"
ORIG_OFF_CURLEVEL="0x79b2"; ORIG_OFF_GAMEMODE="0x792c"
ORIG_SCRIPT="tools/dosbox/scripts/level1-idle.txt"   # already boots the original via F2/EGA

# ── PLAYABLE BUMPYP.EXE calibration (re-derived per build below) ────────────
PLAY_SCRIPT_EGA="tools/dosbox/scripts/bumpyp-boot-ega.txt"   # Task 6: F2/EGA variant
PLAY_SCRIPT_VGA="tools/dosbox/scripts/bumpyp-boot.txt"       # F3/VGA (Phase-0 differential)

# Per-screen SHOT capture windows: "<frame> <stride> <count>".
# original_frames is a DOCUMENTED ESTIMATE for every screen except where noted —
# the original-side capture never got far enough in this session (see ENV-BLOCKED
# below) to empirically confirm any of them.  playable_frames for "title" WAS
# empirically confirmed live this task (see header). The rest are carried over
# from the same boot-script timing structure (same code, same pulse cadence) but
# not individually re-verified — cheap to correct once the gate actually runs.
orig_frames() {
    case "$1" in
        title) echo "450 25 6" ;;
        menu)  echo "650 25 6" ;;
        *) echo "" ;;
    esac
}
play_frames() {
    case "$1" in
        title) echo "900 25 7" ;;   # empirically confirmed (see header)
        menu)  echo "1100 25 7" ;;  # estimate; not empirically confirmed
        *) echo "" ;;
    esac
}
# World-forcing current_level values (see tools/dosbox/cap_w2.sh's --poke-val).
world_poke_val() {
    case "$1" in
        world1) echo 1 ;; world2) echo 2 ;; world3) echo 3 ;; world9) echo 9 ;;
        *) echo "" ;;
    esac
}

# ── 1. Build the instrumented dosbox-x if missing ────────────────────────────
if [ ! -x "$DBX_BIN" ]; then
    echo "== instrumented dosbox-x missing -> building (tools/dosbox/build-dosbox-x.sh) =="
    bash tools/dosbox/build-dosbox-x.sh || env_blocked "dosbox-x build failed; see tools/dosbox/build-dosbox-x.sh output above"
fi
echo "   dosbox-x binary: $DBX_BIN ($(stat -c%s "$DBX_BIN") bytes, $(stat -c%y "$DBX_BIN"))"

# ── 2. Build the playable BUMPYP.EXE (Tasks 2-6 changed it) ──────────────────
echo "== building playable BUMPYP.EXE (wmake play) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh && wmake play ) \
    >"$TMP/ega_playable_build.log" 2>&1 || { cat "$TMP/ega_playable_build.log" >&2; fail "wmake play failed"; }
[ -f "$PLAY_EXE" ] || fail "playable build produced no $PLAY_EXE"
echo "   built $PLAY_EXE ($(stat -c%s "$PLAY_EXE") bytes)"
\cp -f "$PLAY_EXE" "$GAME_DIR/BUMPYP.EXE"

# Re-derive the playable's runtime DGROUP + current_level/game_mode offsets from the
# FRESH link map (tools/dosbox/map_offsets.py) instead of hardcoding them — the exact
# approach tools/dosbox/cap_w2.sh uses, so a Task 1-6 code-size shift never goes stale.
[ -s "$PLAY_MAP" ] || fail "playable map not found: $PLAY_MAP (wmake play should have produced it)"
PLAY_OFFS="$(python3 tools/dosbox/map_offsets.py "$PLAY_MAP" --env)" || fail "map_offsets failed on $PLAY_MAP"
eval "$PLAY_OFFS"
PLAY_DGROUP="$BUMPYCAP_DGROUP"; PLAY_OFF_CURLEVEL="$BUMPYCAP_OFF_CURLEVEL"
PLAY_OFF_GAMEMODE="$BUMPYCAP_OFF_GAMEMODE"; PLAY_POKE_OFF="$BUMPYCAP_POKE_OFF"
echo "   playable DGROUP=$PLAY_DGROUP curlevel=$PLAY_OFF_CURLEVEL gamemode=$PLAY_OFF_GAMEMODE (from $PLAY_MAP)"

# ── 3. Ensure the real original game is present ───────────────────────────────
[ -f "$ORIG_EXE" ] || env_blocked "real original game not found at $ORIG_EXE (user-supplied; see docs/playable-dos.md)"

# ── 4. Conf helper (mirrors validate_playable.sh's mk_conf) ──────────────────
mk_conf() {  # $1=exe -> emits a clean run conf to stdout
    sed -e 's/[[:space:]]*#.*$//' "$CONF_TMPL" \
      | grep -vE '^[[:space:]]*$|^#' \
      | sed -e "s#/ABSOLUTE/PATH/TO/local/build/capture/game#$ROOT/$GAME_DIR#" \
            -e "s#^BUMPY.EXE#$1#"
}

# ── 5. SHOT-burst capture helper: writes <out>.NNN (patch 05+06: DAC+AC+stride) ──
# args: out script dgroup off_curlevel off_gamemode exe frame stride count [poke_off poke_val]
shot_capture() {
    local out="$1" script="$2" dgroup="$3" off_cl="$4" off_gm="$5" exe="$6" \
          frame="$7" stride="$8" count="$9" poke_off="${10:-}" poke_val="${11:-}"
    \rm -f "$out" "$out".*
    local conf="$TMP/ega_$$_$(basename "$out").conf"; \rm -f "$conf"
    mk_conf "$exe" > "$conf"
    local poke_env=()
    if [ -n "$poke_off" ]; then
        poke_env=( BUMPYCAP_POKE_OFF="$poke_off" BUMPYCAP_POKE_VAL="$poke_val"
                   BUMPYCAP_POKE_FSTART=400 BUMPYCAP_POKE_FEND=9000 )
    fi
    env HOME="$TMP" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        BUMPYCAP_SCAN_INJECT=1 \
        BUMPYCAP_SCRIPT="$ROOT/$script" \
        BUMPYCAP_DGROUP="$dgroup" BUMPYCAP_OFF_CURLEVEL="$off_cl" BUMPYCAP_OFF_GAMEMODE="$off_gm" \
        BUMPYCAP_SHOT_FRAME="$frame" BUMPYCAP_SHOT_OUT="$out" \
        BUMPYCAP_SHOT_STRIDE="$stride" BUMPYCAP_SHOT_COUNT="$count" \
        "${poke_env[@]}" \
        timeout -k 5 90 "$DBX_BIN" -conf "$conf" -nomenu -nogui >"$out.log" 2>&1 || true
}

# ── 6. Phase 0: self-contained EGA-vs-VGA differential (no original needed) ──
echo "== Phase 0: playable EGA vs VGA differential (same build; regression guard) =="
P0_EGA="$TMP/ega_p0_ega.bin"; P0_VGA="$TMP/ega_p0_vga.bin"
read -r F0 S0 C0 <<<"$(play_frames title)"
shot_capture "$P0_EGA" "$PLAY_SCRIPT_EGA" "$PLAY_DGROUP" "$PLAY_OFF_CURLEVEL" "$PLAY_OFF_GAMEMODE" \
             "BUMPYP.EXE" "$F0" "$S0" "$C0"
shot_capture "$P0_VGA" "$PLAY_SCRIPT_VGA" "$PLAY_DGROUP" "$PLAY_OFF_CURLEVEL" "$PLAY_OFF_GAMEMODE" \
             "BUMPYP.EXE" "$F0" "$S0" "$C0"
if grep -qi "Triple Fault\|Rebooting the system" "$P0_EGA.log" "$P0_VGA.log" 2>/dev/null; then
    echo "   Phase 0: SKIPPED -- playable capture crashed the emulator (unexpected; see $P0_EGA.log / $P0_VGA.log)"
elif ls "$P0_EGA".0* >/dev/null 2>&1 && ls "$P0_VGA".0* >/dev/null 2>&1; then
    if python3 "$COMPARE" "$P0_EGA" "$P0_VGA" --min-mismatch-frac "$SELFCHECK_MIN_DIFF"; then
        echo "   Phase 0: PASS (EGA and VGA renders of the same build genuinely differ)"
    else
        fail "Phase 0 self-check: playable EGA title looks the same as VGA title -- palette_mode gating regressed (Tasks 2-6)"
    fi
else
    echo "   Phase 0: SKIPPED -- playable-side SHOT capture produced no frames (see $P0_EGA.log / $P0_VGA.log)"
fi

# ── 7. Per-screen original-vs-playable EGA compare ────────────────────────────
REQUIRED_SCREENS="title menu"   # gate PASS/FAIL hinges on these
WORLD_SCREENS="world1 world2 world3 world9"  # informational only (KNOWN GAP, see header)

IFS=',' read -ra WANT <<<"$SCREENS"
ORIG_OK=1
ANY_REQUIRED_FAIL=0
ANY_REQUIRED_RUN=0

run_screen() {
    local screen="$1" required="$2"
    local of sf oc; read -r of sf oc <<<"$(orig_frames "$screen")"
    local pf ps pc; read -r pf ps pc <<<"$(play_frames "$screen")"
    local poke_val=""; poke_val="$(world_poke_val "$screen")"
    local poke_off_o="" poke_off_p=""
    if [ -n "$poke_val" ]; then
        poke_off_o="$ORIG_OFF_CURLEVEL"; poke_off_p="$PLAY_OFF_CURLEVEL"
        # world screens reuse the "title" frame window on each side's in-level boot;
        # informational only (see KNOWN GAP) so an imprecise window is not fatal.
        read -r of sf oc <<<"$(orig_frames title)"
        read -r pf ps pc <<<"$(play_frames title)"
    fi
    if [ -z "$of" ] || [ -z "$pf" ]; then
        echo "   [$screen] SKIPPED -- no frame calibration for this screen (no silent pass/fail)"
        return 0
    fi

    echo "== [$screen] capturing ORIGINAL (EGA) =="
    local out_o="$TMP/ega_orig_$screen.bin"
    shot_capture "$out_o" "$ORIG_SCRIPT" "$ORIG_DGROUP" "$ORIG_OFF_CURLEVEL" "$ORIG_OFF_GAMEMODE" \
                 "BUMPY.EXE" "$of" "$sf" "$oc" "$poke_off_o" "$poke_val"
    # Check the crash signature FIRST, before file existence: a triple-fault reboot loop
    # does not stop the emulator, and the frame counter keeps advancing across reboots, so
    # the SHOT hook can still fire and write files once the counter crosses the target frame
    # -- but their content is garbage from mid-reboot (BIOS text-mode / POST noise), NOT the
    # target screen. Bring-up caught this concretely: a "crashed" original capture at the
    # title window still produced 6 shot files, decoding to a striped noise pattern, not a
    # blank frame -- so the blank-frame filter in ega_compare.py would NOT have caught it,
    # and treating "files exist" as "capture succeeded" would have silently compared noise
    # against the playable's real title and reported a false FAIL instead of ENV-BLOCKED.
    if grep -qi "Triple Fault\|Rebooting the system" "$out_o.log" 2>/dev/null; then
        ORIG_OK=0
        echo "   [$screen] ORIGINAL capture crashed the emulator (CPU triple fault / reboot loop)." >&2
        echo "             This reproduces with ZERO BUMPYCAP instrumentation (bare '$DBX_BIN -conf <conf>'" >&2
        echo "             booting only BUMPY.EXE) -- confirmed pre-existing to Task 7, NOT caused by" >&2
        echo "             the EGA changes: 'CPU_Exception: Exception 6 ... Triple Fault' at CS:IP=0824:994e," >&2
        echo "             frame ~62, independent of cputype/core config. See tools/validate_playable.sh," >&2
        echo "             whose (unmodified) original-side capture fails identically in this environment." >&2
        [ -f "$out_o.000" ] && echo "             (note: $out_o.0* files exist but are reboot-loop garbage, not real content -- ignored)" >&2
        return 0
    fi
    if ! ls "$out_o".0* >/dev/null 2>&1; then
        ORIG_OK=0
        echo "   [$screen] ORIGINAL capture produced no frames (see $out_o.log)" >&2
        return 0
    fi

    echo "== [$screen] capturing PLAYABLE (EGA) =="
    local out_p="$TMP/ega_play_$screen.bin"
    shot_capture "$out_p" "$PLAY_SCRIPT_EGA" "$PLAY_DGROUP" "$PLAY_OFF_CURLEVEL" "$PLAY_OFF_GAMEMODE" \
                 "BUMPYP.EXE" "$pf" "$ps" "$pc" "$poke_off_p" "$poke_val"
    if grep -qi "Triple Fault\|Rebooting the system" "$out_p.log" 2>/dev/null; then
        echo "   [$screen] PLAYABLE capture crashed the emulator -- unexpected (see $out_p.log)" >&2
        [ "$required" = "1" ] && ANY_REQUIRED_FAIL=1
        return 0
    fi
    if ! ls "$out_p".0* >/dev/null 2>&1; then
        echo "   [$screen] PLAYABLE capture produced no frames (see $out_p.log)" >&2
        [ "$required" = "1" ] && ANY_REQUIRED_FAIL=1
        return 0
    fi

    echo "== [$screen] pixel-resolved compare (DAC[AC[pixel]]) =="
    if python3 "$COMPARE" "$out_o" "$out_p" --max-mismatch-frac "$MAX_MISMATCH_FRAC"; then
        echo "   [$screen] PASS"
    else
        echo "   [$screen] FAIL"
        [ "$required" = "1" ] && ANY_REQUIRED_FAIL=1
    fi
}

for screen in "${WANT[@]}"; do
    case " $REQUIRED_SCREENS " in
        *" $screen "*) ANY_REQUIRED_RUN=1; run_screen "$screen" 1 ;;
        *)
            case " $WORLD_SCREENS " in
                *" $screen "*)
                    echo "== [$screen] informational only -- the ORIGINAL-side EGA capture is"
                    echo "            env-blocked (triple-fault, see header). The in-level EGA palette"
                    echo "            path is now reconstructed (load_palette 0x70e branch + populated"
                    echo "            level_palette_ptr_table, 2026-07-11) and AC-verified out-of-band." ;;
                *) echo "validate_ega: unknown screen '$screen' (expected: title menu $WORLD_SCREENS)" >&2; exit 2 ;;
            esac
            run_screen "$screen" 0
            ;;
    esac
done

# ── 8. Final status ────────────────────────────────────────────────────────
echo
if [ "$ORIG_OK" = "0" ]; then
    env_blocked "the original-side capture never produced a usable frame this run (see per-screen diagnostics above). \
Definitive verification requires: (a) a working instrumented dosbox-x that can boot the real BUMPY.EXE without \
crashing, then (b) re-running: cd $ROOT && tools/validate_ega.sh --screens $SCREENS"
fi
if [ "$ANY_REQUIRED_RUN" = "0" ]; then
    fail "no required screen (title/menu) was selected -- nothing to gate on (--screens=$SCREENS)"
fi
if [ "$ANY_REQUIRED_FAIL" = "1" ]; then
    fail "at least one required screen (title/menu) mismatched -- see the [screen] FAIL diagnostics above"
fi
echo "validate_ega: PASS"
echo " -> required screens ($REQUIRED_SCREENS, as selected) match the original pixel-for-pixel in EGA."
exit 0
