"""Locate the 16-colour palette inside a decoded .VEC and factor the remaining
(image) bytes into plausible dimensions, to reverse the world/level layout."""
from __future__ import annotations
import sys
from typing import List, Optional, Tuple
from vec_render import decode_vec


def is_palette(buf: bytes, off: int) -> bool:
    """A real VGA 6-bit palette: 48 bytes, all <= 0x3f, idx0 == black, varied."""
    if off < 0 or off + 48 > len(buf):
        return False
    if any(b > 0x3F for b in buf[off:off + 48]):
        return False
    if buf[off] or buf[off + 1] or buf[off + 2]:
        return False
    triples = {tuple(buf[off + i * 3:off + i * 3 + 3]) for i in range(16)}
    bright = max(buf[off:off + 48])
    return len(triples) >= 10 and bright >= 0x28


def factor(planar: int) -> List[str]:
    out: List[str] = []
    for planes, bpp in ((4, "4-plane"), (1, "1-plane/8bpp-ish")):
        for w in (320, 256, 160, 288, 304, 192):
            stride = (w // 8) * planes
            if stride and planar % stride == 0:
                out.append("%dx%d %s" % (w, planar // stride, bpp))
    return out


def main() -> None:
    for path in sys.argv[1:]:
        buf = decode_vec(path)
        cands = [o for o in range(0, len(buf) - 48) if is_palette(buf, o)]
        print("%-26s decoded=%d  palette@=%s" % (path.split("/")[-1], len(buf), cands[:5]))
        for off in cands[:2]:
            before, after = off, len(buf) - (off + 48)
            print("    pal@%d: bytes-before=%d, bytes-after=%d" % (off, before, after))
            print("      factor(after): %s" % (factor(after) or "none"))
            print("      factor(before): %s" % (factor(before) or "none"))


if __name__ == "__main__":
    main()
