"""Recover the game's 16-colour palette by correlating a decoded .VEC index
image against a ground-truth screenshot, then re-render to validate the decode."""
from __future__ import annotations
import collections
import sys
from typing import List, Tuple
from vec_render import decode_vec, write_png


def decode_indices(buf: bytes, w: int, h: int, hdr: int) -> List[int]:
    """Decode a sequential-plane EGA buffer to a flat list of 0..15 indices."""
    wb: int = w // 8
    plane: int = wb * h
    idx: List[int] = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            bit: int = 7 - (x % 8)
            v: int = 0
            for p in range(4):
                off: int = hdr + p * plane + y * wb + x // 8
                if 0 <= off < len(buf):
                    v |= ((buf[off] >> bit) & 1) << p
            idx[y * w + x] = v
    return idx


def read_ppm(path: str) -> Tuple[int, int, bytes]:
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6", "expected binary PPM"
    # parse header: P6 <w> <h> <maxval>\n<rgb...>
    parts = data.split(None, 4)
    w, h, _maxval = int(parts[1]), int(parts[2]), int(parts[3])
    return w, h, parts[4]


def main() -> None:
    vec: str = sys.argv[1] if len(sys.argv) > 1 else "local/originals/old-games/bumpy/TITRE.VEC"
    ref_ppm: str = sys.argv[2] if len(sys.argv) > 2 else "local/build/capture/menu.ppm"
    W, H, HDR = 320, 200, 99
    buf = decode_vec(vec)
    idx = decode_indices(buf, W, H, HDR)
    rw, rh, rgb = read_ppm(ref_ppm)
    print("decoded %s, ref %dx%d" % (vec, rw, rh))
    # majority RGB per index
    votes: List[collections.Counter] = [collections.Counter() for _ in range(16)]
    for y in range(min(H, rh)):
        for x in range(min(W, rw)):
            o = (y * rw + x) * 3
            votes[idx[y * W + x]][(rgb[o], rgb[o + 1], rgb[o + 2])] += 1
    palette: List[Tuple[int, int, int]] = []
    for i in range(16):
        col = votes[i].most_common(1)[0][0] if votes[i] else (0, 0, 0)
        palette.append(col)
        print("  idx %2d -> %s  (%d px)" % (i, col, sum(votes[i].values())))
    # re-render with recovered palette
    out = bytearray(W * H * 3)
    for p in range(W * H):
        r, g, b = palette[idx[p]]
        out[p * 3] = r; out[p * 3 + 1] = g; out[p * 3 + 2] = b
    write_png("local/build/render/TITRE.recovered.png", W, H, out)
    print("wrote build/render/TITRE.recovered.png")


if __name__ == "__main__":
    main()
