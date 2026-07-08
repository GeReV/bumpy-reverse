#!/usr/bin/env bash
# Build the Bumpy reconstruction with the vendored Open Watcom toolchain — no manual
# environment setup required.  The Makefile is self-bootstrapping (it points
# wcc/wcl/wlink at ../local/toolchain/open-watcom itself), so this wrapper only has to
# locate and run the vendored wmake from the src/ directory.
#
# Usage (from anywhere):
#     src/build.sh            # builds the playable BUMPYP.EXE (default)
#     src/build.sh play       # same
#     src/build.sh BUMPY      # the faithful default BUMPY.EXE
#     src/build.sh clean
#     src/build.sh <any wmake target/args...>
#
# Immune to the interactive zsh 'wmake' autoload stub + cp/rm -i aliases (this is bash).
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WMAKE="$SRC_DIR/../local/toolchain/open-watcom/binl64/wmake"

if [ ! -x "$WMAKE" ]; then
    echo "build.sh: vendored wmake not found at $WMAKE" >&2
    echo "  (expected the Open Watcom toolchain under local/toolchain/open-watcom)" >&2
    exit 1
fi

cd "$SRC_DIR"                       # WROOT in the Makefile is relative to src/
if [ "$#" -eq 0 ]; then
    set -- play                    # default target
fi
exec "$WMAKE" "$@"
