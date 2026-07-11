#!/usr/bin/env python3
"""Fingerprint the world-2 platform region of a BUMPYSHOT dump.

The world-2 platform bug is DGROUP-layout sensitive: at some link layouts the
interactive platforms render (present), at others their per-tick anim blit is skipped
so they read as erased.  To bisect *where* that flips as the DGROUP base sweeps, we
need a scalar per render that says "platform present" vs "platform erased".

We compare the raw 4-bit planar pixel indices (the direct output of the blit path),
not the palette-mapped RGB -- the bug changes which index is written at the platform
pixels (platform-face index -> background-fill index), so an index diff is the most
direct, palette-independent signal.

Two workflows:

  calibrate GOOD BUGGY -> calib.json
      Record the pixel positions where a known-good and known-buggy render disagree.
      Those positions ARE the platform pixels (plus any incidental jitter).  For each,
      store the good index and the buggy index.

  score IN --calib calib.json
      For each calibrated position, classify IN's index as matching good, buggy, or
      neither.  good_frac ~1.0 => platform present; ~0.0 => erased.  Needs no absolute
      oracle beyond the two calibration endpoints (which the sweep itself produces:
      pad=0 is the known-buggy 0x43c9 build, a large pad reproduces the known-good
      ~0x4f20 build).

Plus a quick oracle-free diff:  diff A B  -> raw index-diff pixel count.

Pure stdlib; decode mirrors tools/dosbox/shot_to_png.py exactly.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

W, H = 320, 200
PLANE = 8000  # 320*200/8 bytes per plane


def decode_indices(path: Path) -> list[int]:
    """Decode a BUMPYSHOT .bin to a flat list of W*H 4-bit pixel indices (0..15)."""
    data = path.read_bytes()
    need = 4 * PLANE
    if len(data) < need:
        raise ValueError(f"{path}: short dump ({len(data)} < {need} bytes)")
    planes = [data[p * PLANE:(p + 1) * PLANE] for p in range(4)]
    out = [0] * (W * H)
    for i in range(W * H):
        byte_i = i >> 3
        bit = 7 - (i & 7)
        idx = 0
        for p in range(4):
            idx |= ((planes[p][byte_i] >> bit) & 1) << p
        out[i] = idx
    return out


def region_of(i: int) -> tuple[int, int]:
    """(x, y) of flat index i."""
    return i % W, i // W


def diff_positions(a: list[int], b: list[int]) -> list[int]:
    return [i for i in range(W * H) if a[i] != b[i]]


def band_histogram(positions: list[int]) -> dict[int, int]:
    """Count differing pixels per 8-row band (rough vertical profile of the change)."""
    hist: dict[int, int] = {}
    for i in positions:
        _, y = region_of(i)
        band = (y // 8) * 8
        hist[band] = hist.get(band, 0) + 1
    return hist


def cmd_diff(args: argparse.Namespace) -> int:
    a = decode_indices(args.a)
    b = decode_indices(args.b)
    pos = diff_positions(a, b)
    print(f"diff pixels: {len(pos)} / {W*H}")
    hist = band_histogram(pos)
    if pos:
        top = sorted(hist.items(), key=lambda kv: -kv[1])[:8]
        print("top-changed bands (y0: count): "
              + ", ".join(f"y{y}:{n}" for y, n in top))
    return 0


def cmd_calibrate(args: argparse.Namespace) -> int:
    good = decode_indices(args.good)
    buggy = decode_indices(args.buggy)
    pos = diff_positions(good, buggy)
    calib = {
        "w": W,
        "h": H,
        "positions": pos,
        "good": [good[i] for i in pos],
        "buggy": [buggy[i] for i in pos],
    }
    args.out.write_text(json.dumps(calib))
    print(f"calibrated {len(pos)} discriminating pixels -> {args.out}", file=sys.stderr)
    hist = band_histogram(pos)
    top = sorted(hist.items(), key=lambda kv: -kv[1])[:8]
    print("platform bands (y0: count): "
          + ", ".join(f"y{y}:{n}" for y, n in top), file=sys.stderr)
    return 0


def cmd_score(args: argparse.Namespace) -> int:
    calib = json.loads(args.calib.read_text())
    positions: list[int] = calib["positions"]
    good: list[int] = calib["good"]
    buggy: list[int] = calib["buggy"]
    idx = decode_indices(args.inp)
    n = len(positions)
    n_good = n_buggy = n_other = 0
    for k, i in enumerate(positions):
        v = idx[i]
        if v == good[k]:
            n_good += 1
        elif v == buggy[k]:
            n_buggy += 1
        else:
            n_other += 1
    good_frac = (n_good / n) if n else 0.0
    if args.json:
        print(json.dumps({
            "n": n,
            "n_good": n_good,
            "n_buggy": n_buggy,
            "n_other": n_other,
            "good_frac": round(good_frac, 4),
        }))
    else:
        print(f"good_frac={good_frac:.4f}  "
              f"good={n_good} buggy={n_buggy} other={n_other}  n={n}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_diff = sub.add_parser("diff", help="raw index-diff pixel count between two bins")
    p_diff.add_argument("a", type=Path)
    p_diff.add_argument("b", type=Path)
    p_diff.set_defaults(func=cmd_diff)

    p_cal = sub.add_parser("calibrate", help="derive platform pixels from good+buggy")
    p_cal.add_argument("good", type=Path)
    p_cal.add_argument("buggy", type=Path)
    p_cal.add_argument("out", type=Path)
    p_cal.set_defaults(func=cmd_calibrate)

    p_sc = sub.add_parser("score", help="score a bin against a calibration")
    p_sc.add_argument("inp", type=Path, metavar="IN")
    p_sc.add_argument("--calib", type=Path, required=True)
    p_sc.add_argument("--json", action="store_true")
    p_sc.set_defaults(func=cmd_score)

    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except (OSError, ValueError) as e:
        print(f"platform_fingerprint: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
