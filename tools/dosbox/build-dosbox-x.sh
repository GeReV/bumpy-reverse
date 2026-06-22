#!/usr/bin/env bash
# Build an INSTRUMENTED dosbox-x (with the heavy debugger) from PINNED upstream
# source for the int8-synced end-to-end capture (see docs/dosbox-int8-capture.md).
#
# Reproducibility model: this script + tools/dosbox/patches/*.patch are the ONLY
# tracked artifacts — the upstream source and the built binary live under the
# git-ignored local/toolchain/.  No DOSBox fork is vendored in the repo.  On a
# clean checkout, install the deps (printed below if missing) and run this script.
#
# We pick dosbox-x for its compatibility (a faithful golden reference) and its
# built-in debugger (--enable-debug=heavy), which is invaluable for bring-up:
# locating the frame-boundary address, calibrating the load segment after the
# TinyProg self-unpack, and watching DGROUP live.  We DISABLE the optional heavy
# features (ffmpeg video-capture, FluidSynth, freetype/TTF, slirp/sdlnet network,
# GL output) so the dependency footprint stays small and the build is reproducible.
set -euo pipefail

DBX_TAG="dosbox-x-v2026.06.02-osfree"
DBX_REPO="https://github.com/joncampbell123/dosbox-x"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/local/toolchain/dosbox-x-src"
PATCHES_DIR="$ROOT/tools/dosbox/patches"
cd "$ROOT"

DEPS_HINT='sudo apt update && sudo apt install -y build-essential automake autoconf libtool \
  pkg-config nasm libncurses-dev libsdl2-dev libpng-dev zlib1g-dev libxkbfile-dev libxrandr-dev'

# ── 1. Dependency check (fail early with the exact apt line) ──────────────────
missing=()
# NB: check `libtoolize` not `libtool` — on modern Ubuntu the libtool package
# provides libtoolize (+ the m4 macros aclocal needs) but no `libtool` binary on PATH.
for t in gcc g++ make aclocal automake autoconf libtoolize pkg-config nasm git; do
    command -v "$t" >/dev/null || missing+=("$t")
done
command -v sdl2-config >/dev/null || missing+=("libsdl2-dev (provides sdl2-config)")
{ [ -f /usr/include/ncurses.h ] || pkg-config --exists ncursesw 2>/dev/null; } \
    || missing+=("libncurses-dev")
if [ "${#missing[@]}" -ne 0 ]; then
    echo "ERROR: missing build dependencies: ${missing[*]}" >&2
    echo "Install them (one-time, needs sudo):" >&2
    echo "  $DEPS_HINT" >&2
    exit 1
fi

# ── 2. Fetch the pinned source (shallow) into git-ignored local/ ──────────────
if [ ! -d "$SRC/.git" ]; then
    echo "== cloning $DBX_REPO @ $DBX_TAG =="
    git clone --depth 1 --branch "$DBX_TAG" "$DBX_REPO" "$SRC"
else
    echo "== source present; pinning to $DBX_TAG =="
    git -C "$SRC" fetch --depth 1 origin "refs/tags/$DBX_TAG:refs/tags/$DBX_TAG" 2>/dev/null || true
    git -C "$SRC" checkout -f "$DBX_TAG"
fi

# ── 3. Re-apply our instrumentation patches cleanly (idempotent) ──────────────
git -C "$SRC" checkout -- .            # revert any previously-applied patch
shopt -s nullglob
patches=("$PATCHES_DIR"/*.patch)
if [ "${#patches[@]}" -eq 0 ]; then
    echo "== no patches in $PATCHES_DIR yet — building STOCK (toolchain check) =="
else
    for p in "${patches[@]}"; do
        echo "== applying patch: $(basename "$p") =="
        git -C "$SRC" apply --whitespace=nowarn "$p"
    done
fi

# ── 4. Configure (debugger + SDL2; heavy optional features disabled) + build ──
cd "$SRC"
echo "== autogen =="
./autogen.sh
echo "== configure (--enable-debug=heavy --enable-sdl2, optional features off) =="
./configure --enable-debug=heavy --enable-sdl2 \
    --disable-avcodec --disable-freetype --disable-libfluidsynth \
    --disable-libslirp --disable-sdlnet --disable-opengl
echo "== make =="
make -j"$(nproc)"

BIN="$SRC/src/dosbox-x"
echo
echo "== built: $BIN =="
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$BIN" -version 2>&1 | head -3 || true
echo "OK"
