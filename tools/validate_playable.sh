#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════════
# validate_playable.sh — Plan-A FINAL INTEGRATION GATE (Task 11).
#
# Proves the reconstruction's PLAYABLE build BUMPYP.EXE renders level-1
# frames pixel-identically to the REAL ORIGINAL game BUMPY.EXE, by running BOTH
# under the SAME instrumented dosbox-x and diffing the captured VGA A000 4-plane
# framebuffer per per-tick frame.
#
# WHAT THIS VALIDATES (the gate's precise SCOPE — Tier 1, "idle in-level"):
#   Each build is driven (boot input only, PER-BUILD: the original boots THROUGH
#   the F2/F5 palette screens; the playable SKIPS them) to level 1, then the FB
#   capture arms in-level (current_level>=1 && game_mode!=0) and dumps the first
#   N per-tick "just-presented displayed page" frames with NO further gameplay
#   input.  The per-tick game_tick() pipeline still runs FULLY every tick
#   (render_p1/p2_view, draw_p1/p2_sprite, the anim channels, present + page
#   flip) — so an idle frame-compare genuinely exercises the host render /
#   present / flip glue against the original's direct-VGA output, which is
#   exactly Plan A's claim ("renders identical frames").  It SIDESTEPS the
#   input-timeline alignment problem (a full dig/move scenario tick-for-tick
#   across two differently-booting binaries is out of Tier-1 scope).
#
# COMPARATOR (tools/fb_compare.c): plane-for-plane, reports the first
#   (frame, plane, offset).  A bounded whole-frame phase shift (--phase) absorbs
#   any page-flip phase difference between the builds (it picks the single best
#   alignment, then requires byte-exact equality at that shift — NOT a per-byte
#   tolerance).
#
# Determinism notes (documented in docs/playable-dos.md):
#   - core=normal + cycles=fixed + pinned emulator + scripted input => same frames.
#   - RNG: game_tick calls rand() per tick; an idle level-1 scene has no
#     RNG-driven sprites at tick 0, so the early frames are RNG-stable.  The gate
#     compares the leading idle frames; if RNG-driven divergence appears it is
#     reported as a precise (frame,plane,offset) rather than hidden.
#
# Build/run model: reuses the int8 dosbox-x build (tools/dosbox/build-dosbox-x.sh
# + patches 01/02/03); builds the playable EXE via `wmake play`; the default
# BUMPY.EXE reconstruction is NEVER touched (its objects live outside play/).
# The REAL original BUMPY.EXE is user-supplied under local/build/capture/game/.
#
# Exit 0 iff: playable EXE builds AND both captures are produced AND the playable
# frames == the original frames plane-for-plane over the compared window.
# Final line: "validate_playable: PASS" or "validate_playable: FAIL".
# ════════════════════════════════════════════════════════════════════════════
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
TMP="${TMPDIR:-/tmp}"

fail() { echo "validate_playable: FAIL"; echo " -> $*" >&2; exit 1; }

DBX_BIN="local/toolchain/dosbox-x-src/src/dosbox-x"
GAME_DIR="local/build/capture/game"
CONF_TMPL="tools/dosbox/bumpy-capture.conf"
ORIG_EXE="$GAME_DIR/BUMPY.EXE"
PLAY_EXE="local/build/src/BUMPYP.EXE"

# capture parameters
FRAMES="${PLAYABLE_FB_FRAMES:-32}"          # per-tick frames to capture per build
PHASE="${PLAYABLE_FB_PHASE:-2}"             # bounded whole-frame page-flip phase shift
MIN_FRAMES="${PLAYABLE_FB_MIN_FRAMES:-8}"   # minimum overlapping frames the gate must compare

# ── ORIGINAL BUMPY.EXE per-tick trigger + arm (calibrated, Tasks 7-9 / int8) ──
ORIG_TRIG_CS="0x824"; ORIG_TRIG_IP="0xcda"
ORIG_DGROUP="0x185f"
ORIG_OFF_CURLEVEL="0x79b2"; ORIG_OFF_GAMEMODE="0x792c"
# IDLE boot for the original: the sustained FIRE pulse train drives the engine
# THROUGH the title/menu/level-intro into the per-tick gameplay loop (game_mode!=0,
# which ARMS the FB capture) WITHOUT the movement verbs of level1-scripted.txt.
# (boot-to-graphics.txt only triple-taps FIRE and PARKS in the intro wait, mode==0,
# so it never arms — see the script header.)
ORIG_SCRIPT="tools/dosbox/scripts/level1-idle.txt"

# ── PLAYABLE BUMPYP.EXE calibration (Task 11) ─────────────────────────────────
# Runtime DGROUP seg 0x3fe4 (SHIFTS on any code-size change — re-derive after a
# rebuild from the bumpyp-dbx.log DS register; DGROUP-internal offsets are STABLE).
# Per-tick loop-top CS:IP DERIVED from `wdis play/game.obj`: game_loop's per-tick
# while-loop top is L$11 at game_TEXT+0x02B3; game_loop_ sits at game_TEXT+0x01B6
# (obj) / consolidated-CODE offset 0x153c (BUMPYP.MAP), so L$11 is at link offset
# 0x153c + (0x02B3-0x01B6) = 0x1639.  Runtime CODE segment is 0x0824 (same load
# arrangement as the original; confirmed by bumpyp-dbx.log CS=0824 for engine code).
# => playable per-tick trigger = 0824:1639.
PLAY_TRIG_CS="0x824"; PLAY_TRIG_IP="0x1639"
PLAY_DGROUP="0x3fe4"
PLAY_OFF_CURLEVEL="0x05c2"; PLAY_OFF_GAMEMODE="0x9eca"
PLAY_KEYTBL="0x9dbe"
PLAY_SCRIPT="tools/dosbox/scripts/bumpyp-boot.txt"

# ── 1. Build the instrumented dosbox-x if missing ────────────────────────────
if [ ! -x "$DBX_BIN" ]; then
    echo "== instrumented dosbox-x missing -> building (tools/dosbox/build-dosbox-x.sh) =="
    bash tools/dosbox/build-dosbox-x.sh || fail "dosbox-x build failed"
fi

# ── 2. Build the playable BUMPYP.EXE (default BUMPY.EXE objects untouched) ────
echo "== building playable BUMPYP.EXE (wmake play, -dBUMPY_PLAYABLE into play/) =="
( cd src && source ../local/toolchain/open-watcom/ow-env.sh && command wmake play ) \
    >"$TMP/playable_build.log" 2>&1 || { cat "$TMP/playable_build.log" >&2; fail "wmake play failed"; }
[ -f "$PLAY_EXE" ] || fail "playable build produced no $PLAY_EXE"
echo "   built $PLAY_EXE ($(stat -c%s "$PLAY_EXE") bytes)"

# Re-derive the playable DGROUP from the link map so a code-size shift is caught
# (BUMPYP.MAP DGROUP group paragraph + the empirical load delta 0x82a).  Advisory
# only: the run uses $PLAY_DGROUP; if they disagree the run still uses the value
# proven live, but we warn so the calibration can be refreshed.
MAP="local/build/src/BUMPYP.MAP"
if [ -f "$MAP" ]; then
    LINK_DG=$(grep -m1 -iE '^DGROUP' "$MAP" | grep -oE '[0-9a-fA-F]{4}:0000' | head -1 | cut -d: -f1)
    if [ -n "${LINK_DG:-}" ]; then
        # runtime = link DGROUP paragraph + load delta (0x82a, empirically calibrated)
        RT_DG=$(printf '0x%04x' $(( 0x$LINK_DG + 0x82a )))
        if [ "$RT_DG" != "$PLAY_DGROUP" ]; then
            echo "   WARNING: map-derived runtime DGROUP $RT_DG != pinned $PLAY_DGROUP"
            echo "            (code size shifted; verify the live DS in bumpyp-dbx.log)"
        else
            echo "   playable DGROUP $PLAY_DGROUP confirmed (link $LINK_DG + load delta 0x82a)"
        fi
    fi
fi

# ── 3. Ensure the real original game is present ───────────────────────────────
[ -f "$ORIG_EXE" ] || fail "real original game not found at $ORIG_EXE (user-supplied; see docs/playable-dos.md)"

# Default-build byte-identity guard (Task 1 Step 5 / Task 11 Step 3 regression).
DEF_EXE="local/build/src/BUMPY.EXE"
if [ -f "$DEF_EXE" ]; then
    DEF_MD5=$(md5sum "$DEF_EXE" | cut -d' ' -f1)
    echo "   default reconstruction BUMPY.EXE md5 = $DEF_MD5"
    # Baseline updated 2026-07-13 (audio subsystem): the tempo-ISR sequence-advance
    # midi_tempo_tick (midi.c 0x864c), its snd_timer_slot_sweep dispatch (sound.c), the
    # faithful play_intro_animation_loop body (screens.c 0x30dd) and its copyprot_seed_src
    # global (globals.c) are now reconstructed into the default build.  Re-bumped 2026-07-13
    # (silent-intro fix): snddrv_init (sound.c 0x88e5) now accumulates the real device-
    # detection bitmask (|4 MPU, |1 OPL) from its two sub-calls' return-ZF instead of the
    # hard-coded status=0 that Ghidra's fake dead-substep_ok decompile implied — shifting the
    # image from 55b362c6... (itself from the prior e8957fa0/EGA-path baseline).
    # Re-bumped 2026-07-13 (PC-speaker music + MT-32): mpu401_present/midi_seq_step_active
    # static init = 1 (image), snd_busy_delay MPU raw-forward, + the pcspk_music_render
    # (0x9136) reconstruction & freq table land in the default build too.
    # Re-bumped 2026-07-13 (MT-32 SFX): snd_opl_sample_table (sound.c, DGROUP 0x27ae) was a
    # zeroed placeholder that silenced all MT-32 sound effects (emit sent note-on velocity 0);
    # now populated 1:1 from the binary (0x30 real {note,vel} entries) so bounce/land SFX sound.
    # Re-bumped 2026-07-13 (struct introduction pass, Tier 1): pvp_bbox_t (player2.c),
    # sprite_obj_t (entity.c/level.c), blit_desc_t (sprite_chain.c), and screen_view_desc
    # (screens.c) replace flat scalars / raw offset arithmetic with named structs over the
    # SAME bytes — no behavior change (chain_ctest 17/17, blit_ctest 24/24, validate_p2 74/74,
    # validate_screens 5/5 pixel-exact, validate_int8 150-tick replay all unchanged) — but
    # collapsing N separate DGROUP globals into fewer struct instances shifts the Open Watcom
    # linker's internal variable layout (this build's own DGROUP was never byte-matched to the
    # original engine's addresses anyway, see level.h's DG_* shadow-buffer note), hence the hash
    # moves even though no logic changed.  snd_opl_sample_table's struct-array conversion and
    # snd_tone_param_frame's macro-overlay (sound.c) did NOT move the hash (same variable count/
    # position, verified by direct rebuild comparison).
    # Re-bumped 2026-07-14 (Tier 2, item #8 narrow slice + item #9): player_view_geom_t
    # (player.c/player2.c) names the geometry fields shared by render_p1/p2_view +
    # erase_p1/p2_view over the same view-descriptor memory gfx_view_desc already
    # documents the dispatch-guard fields for (did NOT move the hash — retypes a local
    # pointer only). cell_pos_t (entity.h) replaces the posA/posB/posC dg_rd16 offset
    # reads in entity.c + the raw byte-copy loop in level.c with typed {x,y} array
    # access (DID move the hash — code-segment changes, not just data layout; verified
    # via validate_int8.sh 150-tick replay + validate_p1_spine.sh 30/30 descriptor gates).
    [ "$DEF_MD5" = "5fe1db15183aa72c2ee429b13614c6e3" ] \
        || echo "   WARNING: default BUMPY.EXE md5 changed (expected 5fe1db15...)"
fi

# ── 4. Capture helper ─────────────────────────────────────────────────────────
# args: <label> <exe-name> <out.bin> <conf-vars...>
mk_conf() {  # $1=exe  -> emits a CLEAN run conf to stdout
    # DOSBox-X does NOT strip INLINE '#' comments from value lines, so the
    # template's `machine = vgaonly      # Bumpy uses ...` is parsed verbatim,
    # corrupting the machine type (and `nosound`).  The original game tolerates
    # this, but the playable build's INT8/timer-driven mode-0x0D boot STALLS at a
    # BIOS wait (f000:8db4, game_mode stuck 0).  Strip inline comments + comment/
    # blank lines so only clean `key = value` pairs reach DOSBox (matches the
    # hand-clean conf the playable build was brought up under).
    sed -e 's/[[:space:]]*#.*$//' "$CONF_TMPL" \
      | grep -vE '^[[:space:]]*$|^#' \
      | sed -e "s#/ABSOLUTE/PATH/TO/local/build/capture/game#$ROOT/$GAME_DIR#" \
            -e "s#^BUMPY.EXE#$1#"
}

OUT_ORIG="$TMP/fb_orig.bin"
OUT_PLAY="$TMP/fb_play.bin"
\rm -f "$OUT_ORIG" "$OUT_PLAY"

# 4a. ORIGINAL BUMPY.EXE
echo "== capturing ORIGINAL BUMPY.EXE: $FRAMES idle in-level per-tick frames =="
CONF_ORIG="$TMP/playable_orig.conf"; \rm -f "$CONF_ORIG"
mk_conf "BUMPY.EXE" > "$CONF_ORIG"
HOME="$TMP" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  BUMPYCAP_SCRIPT="$ROOT/$ORIG_SCRIPT" \
  BUMPYCAP_DGROUP="$ORIG_DGROUP" \
  BUMPYCAP_OFF_CURLEVEL="$ORIG_OFF_CURLEVEL" BUMPYCAP_OFF_GAMEMODE="$ORIG_OFF_GAMEMODE" \
  BUMPYCAP_FB_OUT="$OUT_ORIG" BUMPYCAP_FB_FRAMES="$FRAMES" \
  BUMPYCAP_FB_TRIG_CS="$ORIG_TRIG_CS" BUMPYCAP_FB_TRIG_IP="$ORIG_TRIG_IP" \
  timeout -k 10 200 "$DBX_BIN" -conf "$CONF_ORIG" -nomenu -nogui >"$TMP/fb_orig.log" 2>&1 || true
[ -s "$OUT_ORIG" ] || { tail -20 "$TMP/fb_orig.log" >&2; fail "original capture produced no frames ($OUT_ORIG)"; }
echo "   original -> $(stat -c%s "$OUT_ORIG") bytes ($(( $(stat -c%s "$OUT_ORIG") / (4*8000) )) frames)"

# 4b. PLAYABLE BUMPYP.EXE
echo "== capturing PLAYABLE BUMPYP.EXE: $FRAMES idle in-level per-tick frames =="
CONF_PLAY="$TMP/playable_play.conf"; \rm -f "$CONF_PLAY"
mk_conf "BUMPYP.EXE" > "$CONF_PLAY"
HOME="$TMP" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  BUMPYCAP_SCRIPT="$ROOT/$PLAY_SCRIPT" \
  BUMPYCAP_DGROUP="$PLAY_DGROUP" \
  BUMPYCAP_KEYTBL_DIRECT=1 BUMPYCAP_OFF_KEYTBL="$PLAY_KEYTBL" \
  BUMPYCAP_OFF_CURLEVEL="$PLAY_OFF_CURLEVEL" BUMPYCAP_OFF_GAMEMODE="$PLAY_OFF_GAMEMODE" \
  BUMPYCAP_FB_OUT="$OUT_PLAY" BUMPYCAP_FB_FRAMES="$FRAMES" \
  BUMPYCAP_FB_TRIG_CS="$PLAY_TRIG_CS" BUMPYCAP_FB_TRIG_IP="$PLAY_TRIG_IP" \
  timeout -k 10 240 "$DBX_BIN" -conf "$CONF_PLAY" -nomenu -nogui >"$TMP/fb_play.log" 2>&1 || true
[ -s "$OUT_PLAY" ] || { tail -20 "$TMP/fb_play.log" >&2; fail "playable capture produced no frames ($OUT_PLAY)"; }
echo "   playable -> $(stat -c%s "$OUT_PLAY") bytes ($(( $(stat -c%s "$OUT_PLAY") / (4*8000) )) frames)"

# ── 5. Build + run the plane-for-plane comparator ─────────────────────────────
echo "== plane-for-plane frame-compare (tools/fb_compare.c) =="
CMP="$TMP/fb_compare"
cc -O2 -Wall -o "$CMP" tools/fb_compare.c || fail "fb_compare build failed"
if "$CMP" "$OUT_ORIG" "$OUT_PLAY" --phase "$PHASE" --min-frames "$MIN_FRAMES"; then
    echo
    echo "validate_playable: PASS"
    echo " -> SCOPE: $MIN_FRAMES+ idle in-level per-tick frames, BUMPYP.EXE vs original BUMPY.EXE,"
    echo "          plane-for-plane (planes 0-3 of the displayed VGA page), phase +/-$PHASE."
    exit 0
else
    echo
    echo "validate_playable: FAIL"
    echo " -> first (frame,plane,offset) mismatch decoded above isolates the host-glue divergence."
    echo " -> logs: $TMP/fb_orig.log  $TMP/fb_play.log"
    exit 1
fi
