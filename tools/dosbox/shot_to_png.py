#!/usr/bin/env python3
"""Decode a BUMPYSHOT dump (4 planes x 8000 bytes + 256x3 DAC palette) to a PNG.

Mode 0x0D is 320x200x16 planar: each pixel's 4-bit colour index = bit from each of
the 4 planes at the pixel's bit position (plane0=LSB).  DAC values are 6-bit (0-63);
scale to 8-bit for the PNG.  Usage: shot_to_png.py <shot.bin> <out.png>

ATTRIBUTE-CONTROLLER STAGE (do not remove — see below):
The 4-bit pixel value is NOT a direct DAC index.  On a VGA it passes through the
Attribute Controller palette registers first: pixel v -> AC[v] -> DAC[AC[v]].  The
Bumpy engine loads the DAC palette only at slots 0..7 and 0x10..0x17
(vga_dac_upload_from_buffer), leaving 8..15 at the BIOS mode-0x0D default EGA ramp,
and programs the AC to steer pixels 8..15 to DAC 0x10..0x17.  Decoding pixel v as
DAC[v] directly therefore mis-colours every pixel with value 8..15 (reads the leftover
EGA-default ramp instead of the image's real colours) — e.g. the TITRE title logo
comes out rainbow-banded instead of gold.

Newer shot dumps append the 16 actual AC palette registers after the DAC (see the
BUMPYCAP hook, patches/05-bumpycap-shot-attr-palette.patch); when present we apply the
REAL AC so every shot — playable (VGA) or original (EGA, which programs an image-
specific AC) — decodes exactly as the hardware scans it out.  Dumps that predate the
AC section fall back to the engine's standard overlay map AC[v] = (v<8) ? v : 0x10+(v-8)
(correct for the playable, which always programs that mapping).
"""
import sys
import struct
import zlib

W, H = 320, 200
PLANE = 8000  # 320*200/8
DAC_BYTES = 256 * 3
AC_BYTES = 16


def gfx_ac(v: int) -> int:
    """Engine overlay Attribute-Controller mapping: 4-bit pixel value -> DAC index."""
    return v if v < 8 else 0x10 + (v - 8)


def decode_rgb(data: bytes) -> list[bytearray]:
    """Decode a BUMPYSHOT dump to a list of H rows, each W*3 bytes of packed RGB.

    Shared by this module's PNG writer and by other tools (e.g. tools/validate_ega.sh's
    comparator) that need the same DAC[AC[pixel]] resolution without going through a
    PNG round-trip. See the module docstring for why the AC stage is required.
    """
    planes = [data[p * PLANE:(p + 1) * PLANE] for p in range(4)]
    pal_off = 4 * PLANE
    pal = [
        (data[pal_off + i * 3] * 255 // 63,
         data[pal_off + i * 3 + 1] * 255 // 63,
         data[pal_off + i * 3 + 2] * 255 // 63)
        for i in range(256)
    ]
    # AC palette registers: use the dumped ones if the shot includes them, else the
    # engine's standard overlay mapping (see the module docstring).
    ac_off = pal_off + DAC_BYTES
    if len(data) >= ac_off + AC_BYTES:
        ac = [data[ac_off + v] for v in range(16)]
    else:
        ac = [gfx_ac(v) for v in range(16)]
    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            byte_i = (y * W + x) >> 3
            bit = 7 - (x & 7)
            idx = 0
            for p in range(4):
                idx |= ((planes[p][byte_i] >> bit) & 1) << p
            row += bytes(pal[ac[idx]])
        rows.append(row)
    return rows


def main() -> None:
    src, out = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    planes = [data[p * PLANE:(p + 1) * PLANE] for p in range(4)]
    rows = decode_rgb(data)
    # minimal PNG (RGB, no filter)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(tag: bytes, body: bytes) -> bytes:
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(out, "wb").write(png)
    # quick stats so a noise-vs-image guess is possible without opening it
    hist: dict[int, int] = {}
    for p in range(4):
        pass
    nz = sum(1 for p in planes for b in p if b)
    print(f"wrote {out}  ({W}x{H})  nonzero plane bytes={nz}/{4*PLANE}")


if __name__ == "__main__":
    main()
