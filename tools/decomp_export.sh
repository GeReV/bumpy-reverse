#!/usr/bin/env bash
# Export the BumpyDecomp decompiled C to local/decomp/ without touching the live
# Ghidra GUI: copy the project, strip its lock, run ExportDecomp headless.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/local/ghidra-proj"
TMP="$(mktemp -d)"
cp -rf "$SRC" "$TMP/proj"
rm -f "$TMP/proj"/*.lock "$TMP/proj"/*.lock~
OUT="$ROOT/local/decomp"
rm -rf "$OUT"; mkdir -p "$OUT"
timeout 1800 "$ROOT/tools/bin/ghidra-headless" "$TMP/proj" BumpyDecomp \
  -process -noanalysis \
  -scriptPath "$ROOT/tools/ghidra_scripts" \
  -postScript ExportDecomp.java "$OUT"
rm -rf "$TMP"
echo "exported $(ls "$OUT" | wc -l) files to $OUT"
