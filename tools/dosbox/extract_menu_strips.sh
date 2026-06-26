#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════════
# extract_menu_strips.sh — derive the playable build's MENUDIFF.BIN sidecar from
# the user's OWN original BUMPY.EXE.
#
# run_main_menu's option-2 cycles a difficulty label (EASY / MEDIUM / HARD) drawn
# as a 6×2-tile (96×16, 16-colour planar, 768 B) strip beside the "LEVEL :" text.
# In the engine these three strips are RUNTIME-DECODED sprite frames living in
# DGROUP (run_main_menu reads them via the static far-ptr table at DGROUP
# +0x75e off / +0x760 seg, indexed by menu_option2_setting).  The reconstruction's
# sprite path doesn't reproduce that particular DGROUP decode, so the playable host
# (src/host/host_resource.c host_load_menu_strips) LOADS the three already-decoded
# strips from this sidecar instead.
#
# This script boots the user's original under the instrumented dosbox-x, drives it
# to run_main_menu (where the strips are populated), dumps the DGROUP region via the
# BUMPYCAP memory-dump hook, reads the +0x75e/+0x760 pointer table to locate the
# three strips, and writes them in SETTING ORDER (0=EASY, 1=MEDIUM, 2=HARD) to
# local/build/capture/game/MENUDIFF.BIN.
#
# The sidecar is DERIVED GAME DATA — it lives under the git-ignored local/ tree and
# is NEVER committed.  Requires: the instrumented dosbox-x (tools/dosbox/build-dosbox-x.sh)
# and the user-supplied original at local/build/capture/game/BUMPY.EXE.
# ════════════════════════════════════════════════════════════════════════════
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
TMP="${TMPDIR:-/tmp}"

DBX_BIN="local/toolchain/dosbox-x-src/src/dosbox-x"
GAME_DIR="local/build/capture/game"
CONF_TMPL="tools/dosbox/bumpy-capture.conf"
ORIG_EXE="$GAME_DIR/BUMPY.EXE"
OUT="$GAME_DIR/MENUDIFF.BIN"

# Original calibration (this 1992 EN release): runtime DGROUP 0x185f; run_main_menu's
# option-2 pointer table at DGROUP +0x75e (off) / +0x760 (seg), 3 entries stride 4.
ORIG_DGROUP="0x185f"
PTR_TABLE_OFF=0x75e            # dump base: the +0x75e pointer table
DUMP_LEN=0x8800               # covers the table + the strips up through ~0x8e88
SHOT_FRAME=800                # strips are populated once run_main_menu runs (after the title FIRE)

[ -x "$DBX_BIN" ] || { echo "instrumented dosbox-x missing -> bash tools/dosbox/build-dosbox-x.sh" >&2; exit 1; }
[ -f "$ORIG_EXE" ] || { echo "user-supplied original not found at $ORIG_EXE" >&2; exit 1; }

# Boot input: F2 (EGA — the F3/VGA path hits the code-entry text screen) + F5 (no sound),
# then ONE FIRE tap to pass the title intro into run_main_menu, then park.
SCRIPT="$TMP/extract_menu_boot.txt"
printf '120 3c 1\n300 3c 0\n340 3f 1\n388 3f 0\n600 1c 1\n640 1c 0\n' >| "$SCRIPT"

CONF="$TMP/extract_menu.conf"; \rm -f "$CONF"
sed -e 's/[[:space:]]*#.*$//' "$CONF_TMPL" | grep -vE '^[[:space:]]*$|^#' \
  | sed -e "s#/ABSOLUTE/PATH/TO/local/build/capture/game#$ROOT/$GAME_DIR#" > "$CONF"

DUMP="$TMP/menu_strip_region.bin"; \rm -f "$DUMP"
echo "== booting original to run_main_menu, dumping DGROUP +$PTR_TABLE_OFF (len $DUMP_LEN) at frame $SHOT_FRAME =="
HOME="$TMP" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  BUMPYCAP_SCAN_INJECT=1 BUMPYCAP_SCRIPT="$SCRIPT" \
  BUMPYCAP_DGROUP="$ORIG_DGROUP" BUMPYCAP_OFF_CURLEVEL=0x79b2 BUMPYCAP_OFF_GAMEMODE=0x792c \
  BUMPYCAP_SHOT_FRAME="$SHOT_FRAME" \
  BUMPYCAP_MEMDUMP_OUT="$DUMP" BUMPYCAP_MEMDUMP_OFF="$PTR_TABLE_OFF" BUMPYCAP_MEMDUMP_LEN="$DUMP_LEN" \
  timeout -k 5 60 "$DBX_BIN" -conf "$CONF" -nomenu -nogui >"$TMP/extract_menu.log" 2>&1 || true

[ -s "$DUMP" ] || { echo "no DGROUP dump produced" >&2; tail -5 "$TMP/extract_menu.log" >&2; exit 1; }

UV_CACHE_DIR="$TMP/uvcache" XDG_CACHE_HOME="$TMP/xdg" HOME="$TMP" \
uv run python - "$DUMP" "$OUT" "$PTR_TABLE_OFF" <<'PY'
import sys, struct
dump = open(sys.argv[1], "rb").read()
out  = sys.argv[2]
base = int(sys.argv[3], 16)   # DGROUP offset the dump starts at (the +0x75e table)
STRIP = 768
# pointer table: 3 entries {u16 off, u16 seg} stride 4, at dump rel 0 (== DGROUP base).
offs = [struct.unpack_from("<H", dump, i * 4)[0] for i in range(3)]   # setting 0,1,2
strips = []
for setting, o in enumerate(offs):
    rel = o - base
    if rel < 0 or rel + STRIP > len(dump):
        sys.exit(f"strip {setting} off {o:#06x} (rel {rel:#x}) out of dumped region")
    blob = dump[rel:rel + STRIP]
    nz = sum(1 for b in blob if b)
    if nz == 0:
        sys.exit(f"strip {setting} off {o:#06x} is all-zero — menu not reached / wrong frame")
    print(f"  setting {setting}: DGROUP off {o:#06x} nonzero {nz}/{STRIP}")
    strips.append(blob)
open(out, "wb").write(b"".join(strips))   # setting order: EASY, MEDIUM, HARD
print(f"wrote {out} ({3*STRIP} bytes)")
PY
rc=$?
[ $rc -eq 0 ] && echo "== extract_menu_strips: OK ==" || { echo "== extract_menu_strips: FAILED ==" >&2; exit 1; }
