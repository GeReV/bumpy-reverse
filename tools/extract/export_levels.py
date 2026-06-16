#!/usr/bin/env python3
"""Export all Bumpy puzzle levels as clean JSON from each world's .BUM level table.

Each world's .BUM holds a table of up to 15 level headers (0xc2 bytes each, from +2):
  layer A/B/C : three 6x8 entity grids (offsets +0x00/+0x30/+0x60)
  +0x90 spawn cell, +0x91 exit cell, +0x92 items, +0x93..96 player-2 data
NOTE: the .BUM tables come via load_bum() (emulator-captured, byte-correct) because the
pure-Python .BUM decoder has a known op12 bug; see docs/formats/LEVELS.md.
Writes results/levels/world<n>_lvl<NN>.json + an index.
"""
from __future__ import annotations
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools/render"))
sys.path.insert(0, os.path.join(ROOT, "tools/extract"))
from render_levels import load_bum, P2_FRAME_TABLE  # noqa: E402

OUT = os.path.join(ROOT, "local/results/levels")


def grid(b: bytes) -> list:
    """48 bytes -> 6 rows of 8 cols."""
    return [[b[r * 8 + c] for c in range(8)] for r in range(6)]


def export_level(bum: bytes, lvl: int) -> dict:
    base = 2 + lvl * 0xc2
    h = bum[base:base + 0xc2]
    spawn = h[0x90]
    exit_ = h[0x91]
    # The AI opponent ("enemy") is present only when header +0x93 != 0; its starting cell is
    # that byte - 1 and its sprite frame is P2_FRAME_TABLE[+0x96] (see render_levels).
    p2c = h[0x93]
    enemy = None
    if p2c:
        fb = h[0x96]
        enemy = {
            "cell": p2c - 1,
            "frame": P2_FRAME_TABLE[fb] if fb < len(P2_FRAME_TABLE) else None,
            "frame_base_index": fb,
            "move_state": h[0x94],
            "ai_threshold": h[0x95],
        }
    return {
        "layer_a": grid(h[0x00:0x30]),
        "layer_b": grid(h[0x30:0x60]),
        "layer_c": grid(h[0x60:0x90]),
        "bumpy_spawn_cell": spawn - 1 if spawn else 0,
        "exit_cell": exit_ - 1 if exit_ else 0,
        "items": h[0x92],
        "enemy": enemy,
    }


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    index = {}
    total = 0
    for n in range(1, 10):
        bum, nlevels = load_bum(n)
        levels = []
        for lvl in range(nlevels):
            b = 2 + lvl * 0xc2
            if not any(bum[b:b + 0x90]):
                continue
            data = export_level(bum, lvl)
            data["world"] = n
            data["level"] = lvl + 1
            path = os.path.join(OUT, "world%d_lvl%02d.json" % (n, lvl + 1))
            with open(path, "w") as f:
                json.dump(data, f, indent=1)
            levels.append(lvl + 1)
            total += 1
        index["world%d" % n] = levels
        print("world %d: %d levels" % (n, len(levels)))
    with open(os.path.join(OUT, "index.json"), "w") as f:
        json.dump({"total_levels": total, "worlds": index}, f, indent=1)
    print("total: %d puzzle levels -> %s/world<n>_lvl<NN>.json" % (total, os.path.relpath(OUT, ROOT)))


if __name__ == "__main__":
    main()
