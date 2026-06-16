#!/usr/bin/env bash
# Run each ImHex pattern over its synthetic fixture via plcli and assert with jq.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
PLCLI="${PLCLI:-$root/local/tools/pl/build/cli/plcli}"
INC="${IMHEX_INCLUDES:-$root/local/tools/squashfs-root/usr/share/imhex/includes}"

[ -x "$PLCLI" ] || { echo "plcli not found at $PLCLI (build it first)"; exit 1; }
python3 "$here/make_fixtures.py"

run() {  # run <pattern> <fixture>
  "$PLCLI" format -p "$root/imhex/$1" -i "$here/fixtures/$2" -I "$INC"
}

echo "== car =="
run car.hexpat car.bin | jq -e \
  '.header.first_char == 32 and .entries[1].glyph.width == 5 and .entries[1].glyph.height == 3'

echo "== bin =="
run bin.hexpat bin.bin | jq -e \
  '(.bin.entries | length) == 1 and .bin.entries[0].frame.hdr.width_words == 4 and (.bin.entries[0].frame.pixels | length) == 8'

echo "== container =="
run container.hexpat container.bin | jq -e \
  '.header.magic == 0 and .header.decoded_size == 32099 and .record0.op.opcode == 4 and .record0.op.flag == 1'

echo "ALL PATTERNS PARSED OK"
