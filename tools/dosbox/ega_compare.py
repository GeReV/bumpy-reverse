#!/usr/bin/env python3
"""Pixel-resolved RGB compare of two (or two bursts of) BUMPYSHOT dumps.

Used by tools/validate_ega.sh to compare a REFERENCE capture (the real original
BUMPY.EXE, EGA mode) against a CANDIDATE capture (the playable BUMPYP.EXE, EGA mode)
for the *same logical screen* (e.g. the title screen).  Both sides are decoded via
shot_to_png.decode_rgb() -- the DAC[AC[pixel]] resolution documented there -- so the
compare is on final on-screen RGB, not raw plane bits or a palette-index guess.

Each side may be a single shot file or a small burst (tools/dosbox/patches/06's
BUMPYCAP_SHOT_STRIDE/_COUNT, written as <out>.000, <out>.001, ...).  A burst absorbs a
few frames of timing jitter between two independently-booted runs (e.g. a blinking
cursor or a one-frame present/flip skew) -- the "phase tolerance" mentioned in the
Task-7 brief, analogous to tools/fb_compare.c's --phase but done by brute-force
all-pairs search since a screen burst is a handful of frames, not hundreds.

A shot captured before the target screen has actually rendered (still blank / mid mode-
switch) trivially "matches" any other blank shot regardless of colour mode -- this bit us
during Task-7 bring-up (an early-frame blank in one burst matched an early-frame blank in
the other, reporting a false 0%-mismatch "PASS"). Shots that are >99% a single RGB value
are treated as blank and excluded before the pair search; if a whole side is blank-only,
that is a hard error (the capture never reached the target screen), not a pass.

Usage:
    ega_compare.py <ref.bin | ref.bin.NNN glob-prefix> <cand.bin | cand.bin.NNN glob-prefix> \
        [--max-mismatch-frac F | --min-mismatch-frac F]

Exit 0 iff the BEST (ref_i, cand_j) pair's mismatched-pixel fraction <= --max-mismatch-frac
(default 0.0 -- byte-exact RGB at the best alignment). Prints the best pair, the
mismatch fraction, and (on failure) the first diverging pixel's (x,y) + both RGB
triples, so a real divergence is diagnosable rather than a bare "FAIL".

--min-mismatch-frac inverts the assertion (used by validate_ega.sh's self-contained
EGA-vs-VGA differential check, which needs to prove two renders of the SAME playable
build are *sufficiently different* -- a regression guard against palette_mode gating
silently no-op'ing). Exit 0 iff the WORST (least-different) pair's mismatch fraction is
still >= --min-mismatch-frac. --max-mismatch-frac and --min-mismatch-frac are mutually
exclusive.
"""
from __future__ import annotations

import argparse
import glob
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import shot_to_png  # noqa: E402  (local import after sys.path fixup)


def _candidates(spec: str) -> list[Path]:
    """A single shot file, or the sorted burst <spec>.000, <spec>.001, ... if present."""
    burst = sorted(Path(p) for p in glob.glob(spec + ".[0-9][0-9][0-9]"))
    if burst:
        return burst
    p = Path(spec)
    if not p.is_file():
        raise SystemExit(f"ega_compare: no shot(s) found for {spec!r}")
    return [p]


def _decode(path: Path) -> list[bytearray]:
    return shot_to_png.decode_rgb(path.read_bytes())


_BLANK_FRAC = 0.99  # a shot >99% one RGB value is treated as "not yet rendered"


def _is_blank(rows: list[bytearray]) -> bool:
    counts: dict[tuple[int, int, int], int] = {}
    total = 0
    for row in rows:
        for x in range(0, len(row), 3):
            px = (row[x], row[x + 1], row[x + 2])
            counts[px] = counts.get(px, 0) + 1
            total += 1
    return total > 0 and max(counts.values()) / total >= _BLANK_FRAC


def _decode_non_blank(paths: list[Path]) -> list[tuple[Path, list[bytearray]]]:
    out = []
    for p in paths:
        rows = _decode(p)
        if _is_blank(rows):
            print(f"ega_compare: {p.name} looks blank (>=99% one colour) -- excluded", file=sys.stderr)
            continue
        out.append((p, rows))
    if not out:
        raise SystemExit(
            f"ega_compare: all {len(paths)} shot(s) for this side are blank -- "
            "the capture never reached the target screen (bad frame calibration, "
            "not a real compare result)"
        )
    return out


def _compare_rows(a: list[bytearray], b: list[bytearray]) -> tuple[int, int, tuple[int, int, tuple, tuple] | None]:
    """Returns (mismatched_pixels, total_pixels, first_mismatch|None)."""
    w = shot_to_png.W
    total = shot_to_png.W * shot_to_png.H
    mismatched = 0
    first = None
    for y, (ra, rb) in enumerate(zip(a, b)):
        for x in range(w):
            pa = tuple(ra[x * 3:x * 3 + 3])
            pb = tuple(rb[x * 3:x * 3 + 3])
            if pa != pb:
                mismatched += 1
                if first is None:
                    first = (x, y, pa, pb)
    return mismatched, total, first


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref", help="reference shot file, or burst prefix (ref.bin -> ref.bin.000...)")
    ap.add_argument("cand", help="candidate shot file, or burst prefix")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--max-mismatch-frac", type=float,
                       help="assert SIMILAR: max mismatched-pixel fraction at the best "
                            "(least-different) alignment (default mode; default 0.0 = exact)")
    mode.add_argument("--min-mismatch-frac", type=float,
                       help="assert DIFFERENT: min mismatched-pixel fraction required even at "
                            "the worst (least-different) alignment")
    args = ap.parse_args(argv)

    refs = _candidates(args.ref)
    cands = _candidates(args.cand)
    ref_rows = _decode_non_blank(refs)
    cand_rows = _decode_non_blank(cands)

    pairs = []  # (mismatched, total, ref_path, cand_path, first_mismatch)
    for rp, ra in ref_rows:
        for cp, cb in cand_rows:
            mismatched, total, first = _compare_rows(ra, cb)
            pairs.append((mismatched, total, rp, cp, first))

    if args.min_mismatch_frac is not None:
        # "assert different": the LEAST-different pair is the adversarial case.
        mismatched, total, rp, cp, first = min(pairs, key=lambda p: p[0])
        frac = mismatched / total
        print(f"ega_compare: least-different pair ref={rp.name} cand={cp.name} "
              f"({len(ref_rows)}x{len(cand_rows)} pairs tried)")
        print(f"ega_compare: mismatched={mismatched}/{total} ({frac:.4%})")
        if frac >= args.min_mismatch_frac:
            print("ega_compare: PASS (sufficiently different)")
            return 0
        print("ega_compare: FAIL (too similar -- possible palette_mode gating no-op)")
        return 1

    # default: "assert similar" (real screen-content compare).
    max_frac = args.max_mismatch_frac if args.max_mismatch_frac is not None else 0.0
    mismatched, total, rp, cp, first = min(pairs, key=lambda p: p[0])
    frac = mismatched / total
    print(f"ega_compare: best alignment ref={rp.name} cand={cp.name} "
          f"({len(ref_rows)}x{len(cand_rows)} pairs tried)")
    print(f"ega_compare: mismatched={mismatched}/{total} ({frac:.4%})")

    if frac <= max_frac:
        print("ega_compare: PASS")
        return 0

    if first is not None:
        x, y, pa, pb = first
        print(f"ega_compare: first mismatch at (x={x}, y={y}): ref={pa} cand={pb}")
    print("ega_compare: FAIL")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
