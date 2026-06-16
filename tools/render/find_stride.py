"""Find a bitmap's row stride in a decoded .VEC buffer via autocorrelation: a
raster image is periodic at its row-stride (and plane-size), so correlation
between byte b[i] and b[i+lag] peaks at those lags."""
from __future__ import annotations
import sys
from typing import List, Tuple
from vec_render import decode_vec


def autocorr(buf: bytes, lag: int) -> float:
    n = len(buf) - lag
    if n <= 0:
        return 0.0
    same = sum(1 for i in range(0, n, 3) if buf[i] == buf[i + lag])  # sample every 3rd
    return same / (n / 3.0)


def main() -> None:
    path: str = sys.argv[1]
    buf = decode_vec(path)
    print("%s decoded=%d" % (path.split("/")[-1], len(buf)))
    scores: List[Tuple[float, int]] = []
    for lag in range(8, 401):
        scores.append((autocorr(buf, lag), lag))
    scores.sort(reverse=True)
    print("top row-stride candidates (lag bytes : match-fraction):")
    for s, lag in scores[:12]:
        # width if this lag is a 1bpp row (lag*8) or a 4-plane row (lag/4*8)
        w1 = lag * 8
        w4 = lag // 4 * 8 if lag % 4 == 0 else None
        print("  lag=%-3d  match=%.3f   -> 1bpp width=%d  4plane width=%s" % (lag, s, w1, w4))


if __name__ == "__main__":
    main()
