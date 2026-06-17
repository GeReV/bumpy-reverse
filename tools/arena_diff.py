#!/usr/bin/env python3
"""Byte-for-byte diff of two decode-arena files.

Usage:
    python3 tools/arena_diff.py a.ARENA b.ARENA

Exit codes:
    0  — files are byte-identical (prints ARENA MATCH)
    1  — files differ (prints ARENA DIFF with first-diff offset and counts)
    2  — usage / I/O error
"""
from __future__ import annotations

import sys
from typing import Tuple


def arena_diff(path_a: str, path_b: str) -> Tuple[bool, int, int, int, int, int]:
    """Compare two arena files byte-for-byte.

    Returns (match, first_diff_offset, byte_a, byte_b, n_diff, n_total).
    If match is True, first_diff_offset / byte_a / byte_b are meaningless (-1).
    Raises IOError if either file cannot be read.
    """
    with open(path_a, "rb") as f:
        data_a: bytes = f.read()
    with open(path_b, "rb") as f:
        data_b: bytes = f.read()

    n_a: int = len(data_a)
    n_b: int = len(data_b)
    n_total: int = max(n_a, n_b)

    first_diff: int = -1
    first_a: int = -1
    first_b: int = -1
    n_diff: int = 0

    for i in range(n_total):
        ba: int = data_a[i] if i < n_a else -1
        bb: int = data_b[i] if i < n_b else -1
        if ba != bb:
            n_diff += 1
            if first_diff < 0:
                first_diff = i
                first_a = ba
                first_b = bb

    match: bool = (n_diff == 0)
    return match, first_diff, first_a, first_b, n_diff, n_total


def main() -> None:
    if len(sys.argv) != 3:
        print("Usage: arena_diff.py <a.ARENA> <b.ARENA>", file=sys.stderr)
        sys.exit(2)

    path_a: str = sys.argv[1]
    path_b: str = sys.argv[2]

    try:
        match, first_off, byte_a, byte_b, n_diff, n_total = arena_diff(path_a, path_b)
    except OSError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        sys.exit(2)

    if match:
        print("ARENA MATCH (%d bytes)" % n_total)
        sys.exit(0)
    else:
        a_str: str = ("0x%02x" % byte_a) if byte_a >= 0 else "<eof>"
        b_str: str = ("0x%02x" % byte_b) if byte_b >= 0 else "<eof>"
        print("ARENA DIFF first@0x%x (%s=%s vs %s=%s), %d/%d bytes differ"
              % (first_off, path_a, a_str, path_b, b_str, n_diff, n_total))
        sys.exit(1)


if __name__ == "__main__":
    main()
