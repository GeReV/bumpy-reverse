#!/usr/bin/env python3
"""Phase 1 residue diagnostic for Plan 6b Task 7.

Builds the bg planes from oracle data, computes the bg-vs-frame diff,
and characterizes where the residual mismatch lives (which grid rows/cells,
which entity types from the BUM header).

Run from repo root:
  uv run python tools/residue_diag.py [frame_oracle.bin]
"""
import struct
import sys
import os
import importlib.util
from typing import Dict, List, Tuple

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from composite_check import load_frame3, idx_at
from frame_render import write_png

PLANE = 0x10000
W, H = 320, 200


def build_bg_planes(atlas: bytes, bmap: bytes) -> bytes:
    """Python equivalent of bg_render_grid via bg_blit_ref."""
    spec = importlib.util.spec_from_file_location(
        "bgref", os.path.join(os.path.dirname(__file__), "bg_blit_ref.py"))
    bgref = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bgref)
    raster = atlas[6:]
    bg = [bytearray(PLANE) for _ in range(4)]
    for cy in range(0, 25, 2):
        for cx in range(20):
            rc = bmap[cx * 0x27 + (cy >> 1) * 3 + 0x20]
            bgref.blit_cell(bg, raster, bmap, dict(cx=cx, cy=cy, run_code=rc))
    return b"".join(bytes(p) for p in bg)


def main() -> None:
    oracle_path = (sys.argv[1]
                   if len(sys.argv) > 1
                   else "local/build/render/frame_oracle.bin")
    d = load_frame3(oracle_path)
    cap_planes: bytes = d["planes"]
    atlas: bytes = d["atlas"]
    bmap: bytes = d["map"]
    bum: bytes = d["bum"]
    dg: bytes = d["dg"]
    dac: bytes = d["dac"]
    level: int = d["level"]

    print(f"=== Residue Diagnostic — Level {level} ===")
    print()

    # Build bg reference
    bg_planes = build_bg_planes(atlas, bmap)

    # Collect all diff pixels (bg != cap_frame)
    diff_pixels: List[Tuple[int, int]] = []
    for y in range(H):
        for x in range(W):
            if idx_at(bg_planes, x, y) != idx_at(cap_planes, x, y):
                diff_pixels.append((x, y))

    print(f"bg vs frame: {len(diff_pixels)} entity pixels")

    # BUM metadata
    p1_spawn_cell: int = bum[0x90] - 1  # zero-based
    exit_cell: int = bum[0x91] - 1      # zero-based
    items_remaining: int = bum[0x92]
    p2_spawn_raw: int = bum[0x93]
    p2_spawn_cell: int = p2_spawn_raw - 1 if p2_spawn_raw != 0 else -1

    print()
    print("=== BUM metadata (out-of-scope entity slots) ===")
    print(f"  bum[0x90] = 0x{bum[0x90]:02x} ({bum[0x90]:3d}) → P1 spawn cell 0-based={p1_spawn_cell}")
    print(f"  bum[0x91] = 0x{bum[0x91]:02x} ({bum[0x91]:3d}) → level-exit cell 0-based={exit_cell}")
    print(f"  bum[0x92] = 0x{bum[0x92]:02x} ({bum[0x92]:3d}) → items_remaining")
    print(f"  bum[0x93] = 0x{bum[0x93]:02x} ({bum[0x93]:3d}) → P2 spawn (0=absent)")
    print(f"  bum[0x94] = 0x{bum[0x94]:02x} ({bum[0x94]:3d})")

    # Level-exit position (uses posC table: x=8+col*40, y=8+row*32)
    exit_row: int = exit_cell // 8
    exit_col: int = exit_cell % 8
    exit_x: int = 8 + exit_col * 40
    exit_y: int = 8 + exit_row * 32
    print()
    print(f"Level-exit: cell {exit_cell} (row={exit_row}, col={exit_col})")
    print(f"  posC position: ({exit_x}, {exit_y})")
    # Count diff pixels in a 40x40 window around exit
    exit_win = sum(1 for (x, y) in diff_pixels
                   if exit_x - 2 <= x <= exit_x + 34
                   and exit_y - 2 <= y <= exit_y + 34)
    print(f"  Diff pixels in 36x36 window around exit: {exit_win}")

    # P1 spawn position (also posC based)
    p1_row: int = p1_spawn_cell // 8
    p1_col: int = p1_spawn_cell % 8
    p1_x: int = 8 + p1_col * 40
    p1_y: int = 8 + p1_row * 32
    print()
    print(f"P1 spawn: cell {p1_spawn_cell} (row={p1_row}, col={p1_col})")
    print(f"  posC position: ({p1_x}, {p1_y})")

    # Row distribution
    print()
    print("=== Diff pixels by screen row (non-zero rows only) ===")
    row_counts: List[int] = [0] * H
    for (x, y) in diff_pixels:
        row_counts[y] += 1
    for row in range(H):
        if row_counts[row] > 0:
            print(f"  y={row:3d}: {row_counts[row]:4d} px")

    # Column distribution (byte columns)
    print()
    print("=== Diff pixels by 8-pixel column ===")
    col_counts: List[int] = [0] * 40
    for (x, y) in diff_pixels:
        col_counts[x // 8] += 1
    for col in range(40):
        if col_counts[col] > 0:
            print(f"  bcol {col:2d} (x={col*8:3d}-{col*8+7:3d}): {col_counts[col]:5d} px")

    # Composite analysis.
    # NOTE: these are the HISTORICAL level-1 / page-0 development snapshot used during
    # Plan-6b bring-up.  The settled figure is 54152/64000 (world-8 LIVE page, see
    # validate_composite.sh + docs/reconstruction-fidelity.md); this diagnostic predates
    # the live-page switch (6c-T3) and is kept only for the level-1 per-cell breakdown.
    bg_match = W * H - len(diff_pixels)
    total = 54239  # historical level-1/page-0 snapshot (superseded by 54152 world-8 live page)
    residue = W * H - total
    print()
    print("=== Composite match summary (historical level-1/page-0 dev snapshot) ===")
    print(f"  bg:        50587/64000 (79.0%)")
    print(f"  bg+C:      51397/64000 (+810,  80.3%)")
    print(f"  bg+C+P1:   51561/64000 (+164,  80.6%)")
    print(f"  bg+C+P1+A: 54239/64000 (+2678, 84.7%)")
    print(f"  Residue:   {residue} px (15.3%)  [settled figure: 54152 vs world-8 live page]")
    print()
    print(f"  Entity pixels total (bg diff):   {len(diff_pixels)}")
    print(f"  Entity pixels captured by ports: {len(diff_pixels) - residue}")
    print(f"  Entity pixels still residual:    {residue}")

    # Generate diff PNG showing composite vs cap
    # Color scheme:
    #   - matched bg pixels: dimmed
    #   - entity diff pixels: colored by type:
    #     - near exit cell: red
    #     - near P1 spawn: blue
    #     - all others: magenta
    pal = [(min(255, dac[i*3]*255//63),
            min(255, dac[i*3+1]*255//63),
            min(255, dac[i*3+2]*255//63)) for i in range(256)]

    diff_set = set(diff_pixels)

    rgb = bytearray(W * H * 3)
    for y in range(H):
        for x in range(W):
            cap_idx = idx_at(cap_planes, x, y)
            bg_idx = idx_at(bg_planes, x, y)
            if bg_idx == cap_idx:
                r, g, b = pal[cap_idx]; r //= 3; g //= 3; b //= 3
            else:
                # classify
                near_exit = (exit_x - 2 <= x <= exit_x + 34
                             and exit_y - 2 <= y <= exit_y + 34)
                near_p1 = (p1_x - 4 <= x <= p1_x + 36
                           and p1_y - 4 <= y <= p1_y + 36)
                if near_exit:
                    r, g, b = 255, 64, 0   # orange-red = level-exit
                elif near_p1:
                    r, g, b = 64, 128, 255  # blue = P1 area
                else:
                    r, g, b = 255, 0, 255   # magenta = other entity
            px = (y * W + x) * 3
            rgb[px] = r; rgb[px+1] = g; rgb[px+2] = b

    out_path = "local/build/render/residue_diag.png"
    write_png(out_path, W, H, rgb)
    print()
    print(f"Diff PNG written: {out_path}")
    print("  orange-red = level-exit window")
    print("  blue       = P1 spawn window")
    print("  magenta    = other entity diff")

    # Item count — items_remaining is the collectible counter (game-state).
    print()
    print(f"=== items_remaining = {items_remaining} ===")
    print("CORRECTION (verified by xref): level-exit (bum+0x91) and items (bum+0x92) are")
    print("GAME-STATE, read only by p1_collect_item (collision/scoring) + spawn_and_draw init;")
    print("NO draw routine reads them.  Their visuals are ordinary level-grid cells (already")
    print("reconstructed) + the HUD — there is no separate draw_exit/item_draw to port.")
    print()
    print("=== VERDICT (superseded; kept for the level-1 per-cell breakdown above) ===")
    print("The original 'residue = out-of-scope exit/item sprites' verdict was a MISATTRIBUTION.")
    print("Verified residue cause: validating the single-page composite against the lagging")
    print("visible page instead of the live draw page — fixed in 6c-T3 (validate vs live page).")
    print("See docs/rendering-pipeline.md and docs/reconstruction-fidelity.md.")


if __name__ == "__main__":
    main()
