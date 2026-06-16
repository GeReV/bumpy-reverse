#!/usr/bin/env python3
"""Render Bumpy puzzle levels PURE-PYTHON, straight out of the game files.

A "world" = D<n>.{PAV,DEC,BUM}; a world holds up to 15 puzzle levels.
  - D<n>.PAV  -> 320x192 background raster (op4/op12 vec_run decompress; planar @ off 0)
  - D<n>.BUM  -> per-level map-header table (2 + 15*0xc2). Each 0xc2 header IS a level:
        layer A @ +0x00 (6x8) : anim-channel-A entities  (TODO: anim render)
        layer B @ +0x30 (6x8) : anim-channel-B entities  (TODO)
        layer C @ +0x60 (6x8) : static sprites, BUMSPJEU frame = code + 0x179
        +0x90 spawn cell, +0x91 exit cell, +0x92 items, +0x93..96 player-2 data
  - cell (row*8+col) -> pixel (8 + col*40, 8 + row*32)   [table @ DGROUP 0x274]

Confirmed: the .BUM pure-Python decode is byte-exact vs the emulator; the gameplay
palette is shared by all worlds (only MONDE world-maps are per-world).
Supersedes the old emulator+vec_cpu hybrid. See docs/formats/LEVELS.md.
"""
from __future__ import annotations
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools/render"))
sys.path.insert(0, os.path.join(ROOT, "tools/extract"))
import json  # noqa: E402
from op12_port import Op12, DG          # noqa: E402
from vec_render import render_planar, write_png  # noqa: E402

GAME = os.path.join(ROOT, "local/build/capture/game")
W, H = 320, 200          # display height is 200 (the .PAV background is 192 tall)
BG_H = 192               # .PAV atlas height
STREAM = 0x67bf0
LAYER_C_FRAME_BIAS = 0x179
TILE = 16                # background tile size (atlas is a 20-col grid of 16x16 tiles)
PLAYER_FRAME = 0         # Bumpy idle (orange ball w/ face; frames 0-11 = Bumpy anim)
# P2 (the AI opponent / enemy) base-frame table @ DGROUP 0x2546, indexed by level[0x96].
# spawn_and_draw_level_entities: p2_frame_base = [level[0x96]*2 + 0x2546]; draw_p2_sprite
# blits frame p2_frame_base (+p2_move_anim, 0 at rest) at p2_cell when level[0x93] != 0.
P2_FRAME_TABLE = [4427, 346, 350, 354, 358, 362, 366, 370, 374,
                  475, 479, 483, 487, 491, 495, 499, 503, 507]

# Static anim tables from the game's DGROUP (per-cell blit positions + entity->frame maps
# for layers A/B). Committed input data; regenerate from a memory snapshot with
# tools/extract/dump_anim_tables.py. See docs/formats/LEVELS.md.
ANIM = json.load(open(os.path.join(ROOT, "tools/extract/anim_tables.json")))

# Shared gameplay palette (6-bit RGB, logical index order); identical for all worlds.
LPAL6 = [(0, 0, 0), (0, 0, 16), (0, 0, 24), (56, 40, 0), (0, 32, 16), (24, 0, 16),
         (32, 0, 0), (40, 16, 0), (48, 24, 0), (0, 16, 8), (0, 24, 16), (0, 16, 56),
         (16, 24, 56), (32, 8, 24), (32, 32, 24), (0, 0, 8)]


def _e6(v: int) -> int:
    return ((v & 0x3F) << 2) | ((v & 0x3F) >> 4)


PALETTE = [(_e6(r), _e6(g), _e6(b)) for (r, g, b) in LPAL6]   # world-1 default / fallback


def world_palette(world: int) -> list:
    """Per-world gameplay palette (6-bit RGB) captured from the emulator; each world has
    its own colour theme. Falls back to the world-1 default if not captured."""
    p = os.path.join(ROOT, "local/build/render/bum", "world%d.pal.json" % world)
    if os.path.exists(p):
        return [(_e6(r), _e6(g), _e6(b)) for (r, g, b) in json.load(open(p))]
    return PALETTE


def decompress(path: str, declen: int) -> bytes:
    """Run the pure-Python vec_run (op4/op12) decompressor on a level file."""
    raw = open(path, "rb").read()
    mem = bytearray(0xA0000)
    mem[STREAM:STREAM + len(raw)] = raw

    def sv(o: int, v: int) -> None:
        mem[DG + o] = v & 0xFF
        mem[DG + o + 1] = (v >> 8) & 0xFF

    def setl(off_o: int, seg_o: int, lin: int) -> None:
        sv(off_o, lin & 0xF)
        sv(seg_o, (lin >> 4) & 0xFFFF)

    setl(0x4e0e, 0x4e10, STREAM)
    setl(0x4e0a, 0x4e0c, STREAM + declen)
    sv(0x4e28, len(raw) & 0xFFFF)
    sv(0x4e2a, (len(raw) >> 16) & 0xFFFF)
    Op12(mem).vec_run(dispatch_current=False)
    return bytes(mem[STREAM:STREAM + declen])


# Game buffer linear addresses (from the runtime vec_run trace), used by the chain decode.
PAV_BUF, DEC_BUF, BUM_BUF = 0x472d0, 0x64750, 0x6f960


def _decode_into(mem: bytearray, name: str, stream: int, declen: int):
    """Decode one op4/op12 file in-place into `mem` at `stream`, sharing the op12 sliding
    window (DG:0x4e97). Returns (decoded `declen` bytes, meaningful_len) where
    meaningful_len is the real payload size = the last processed record's vec_src, or the
    file size when the stream is stored uncompressed (vec_run processes no records, e.g.
    D6/D9.BUM whose first 12 bytes are a terminator)."""
    raw = open(os.path.join(GAME, name), "rb").read()
    mem[stream:stream + len(raw)] = raw

    def sv(o: int, v: int) -> None:
        mem[DG + o] = v & 0xFF
        mem[DG + o + 1] = (v >> 8) & 0xFF

    def setl(off_o: int, seg_o: int, lin: int) -> None:
        sv(off_o, lin & 0xF)
        sv(seg_o, (lin >> 4) & 0xFFFF)

    setl(0x4e0e, 0x4e10, stream)
    setl(0x4e0a, 0x4e0c, stream + declen)
    sv(0x4e28, len(raw) & 0xFFFF)
    sv(0x4e2a, (len(raw) >> 16) & 0xFFFF)
    op = Op12(mem)
    last_vsrc = [0]
    _rr = op.vec_read_record

    def rr():
        r = _rr()
        if r is not None and r > 0:            # a record that gets decompressed
            last_vsrc[0] = op.gv(0x4e24) | (op.gv(0x4e26) << 16)
        return r

    op.vec_read_record = rr
    op.vec_run(dispatch_current=False)
    meaningful = last_vsrc[0] if last_vsrc[0] else len(raw)
    return bytes(mem[stream:stream + declen]), meaningful


def load_bum(world: int):
    """Decode D<n>.BUM purely in Python (no emulator) by replaying the game's
    D<n>.PAV -> D<n>.DEC -> D<n>.BUM decode sequence into one shared memory image, so the
    op12 sliding window (DG:0x4e97) holds the same leftover state the game has when it
    decodes the .BUM. The .BUM's in-place LZ finalize copies that window into the level
    table's wrap region, so worlds whose layout table overruns the window boundary need
    the chain to reproduce byte-for-byte. Byte-exact vs the emulator for worlds 1-9
    (and recovers world 3's last levels, which the emulator capture had corrupted).

    Returns (bum_bytes, nlevels). `nlevels` is the *real* puzzle count = (payload-2)//0xc2,
    so callers don't render the stale tail past the decoded data (the .DEC window leftover
    that the in-place finalize leaves in the buffer past the last real level — e.g. worlds
    4 & 5 only have 12 puzzles, not the 15 the table has room for). See docs/formats/LEVELS.md."""
    mem = bytearray(0xA0000)
    _decode_into(mem, "D%d.PAV" % world, PAV_BUF, 0x7806)
    _decode_into(mem, "D%d.DEC" % world, DEC_BUF, 0x2f96)
    buf, payload = _decode_into(mem, "D%d.BUM" % world, BUM_BUF, 0xb60)
    nlevels = max(0, min(15, (payload - 2) // 0xc2))
    return buf, nlevels


class Sprites:
    """BUMSPJEU.BIN frame decoder (4-plane, 16px-block interleaved; 0 = transparent)."""

    def __init__(self) -> None:
        self.b = open(os.path.join(GAME, "BUMSPJEU.BIN"), "rb").read()
        self.data = 0x800
        self._cache: dict = {}

    def _be32(self, o: int) -> int:
        return struct.unpack(">I", self.b[o:o + 4])[0]

    def _be16(self, o: int) -> int:
        return struct.unpack(">H", self.b[o:o + 2])[0]

    def frame(self, idx: int):
        """Return (rows_of_indices, width_px, height) or None for an unusable frame."""
        if idx in self._cache:
            return self._cache[idx]
        out = None
        v = self._be32(idx * 4)
        if v:
            fp = self.data + v
            if self.data <= fp < len(self.b):
                w = self._be16(fp - 4)
                h = self._be16(fp - 2)
                if w and h and w % 4 == 0 and fp + w * 2 * h <= len(self.b):
                    blocks = w // 4
                    img = []
                    for r in range(h):
                        words = [self._be16(fp + r * w * 2 + 2 * i) for i in range(w)]
                        row = []
                        for blk in range(blocks):
                            planes = words[blk * 4:blk * 4 + 4]
                            for col in range(16):
                                row.append(sum(((planes[p] >> (15 - col)) & 1) << p
                                               for p in range(4)))
                        img.append(row)
                    out = (img, blocks * 16, h)
        self._cache[idx] = out
        return out


def blit(rgb: bytearray, fr, px: int, py: int, pal=PALETTE) -> None:
    """Composite a decoded sprite (index grid, 0=transparent) onto the RGB buffer."""
    grid, wpx, h = fr
    for r in range(h):
        y = py + r
        if not (0 <= y < H):
            continue
        base = y * W * 3
        srow = grid[r]
        for c in range(wpx):
            x = px + c
            if 0 <= x < W:
                v = srow[c]
                if v:
                    o = base + x * 3
                    rgb[o], rgb[o + 1], rgb[o + 2] = pal[v]


def planar_indices(buf: bytes, w: int, h: int, hdr: int = 0) -> bytearray:
    """Decode an EGA-planar (`seq` layout) buffer to per-pixel palette indices (0..15).
    Parallel to vec_render.render_planar but keeps the raw index so the background
    compositor can treat index 0 as transparent (mask)."""
    wb = w // 8
    plane = wb * h
    idx = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            bit = 7 - (x % 8)
            v = 0
            for p in range(4):
                off = hdr + p * plane + y * wb + x // 8
                if 0 <= off < len(buf):
                    v |= ((buf[off] >> bit) & 1) << p
            idx[y * w + x] = v
    return idx


def draw_background(rgb: bytearray, atlas: bytes, atlas_idx: bytes, dec: bytes, lvl: int) -> None:
    """Build the level background by placing .PAV atlas tiles per the .DEC tile grid.

    redraw_level_background_tiles: 20 cols x 13 rows (cell_y 0..0x18 step 2). Each cell's
    tile code = dec[2 + lvl*0x32c + cell_x*0x27 + (cell_y>>1)*3 + 0x20]; tile_id = code-1;
    atlas tile = (id % 20, id // 20), 16x16 px, drawn at (cell_x*16, cell_y*8). A code
    >=0xf1 is a "run" cell: restore_bg_tile_run draws col_count = (byte)(-code - 5) tiles,
    reading tile_ids from dec[slot+1 .. slot+col_count-1] (the cell's own group bytes) and
    blitting EACH at the SAME (cell_x, cell_y) (descriptor[0x14]=cell_x for every iter). The
    blit is MASKED (index 0 = transparent), so within a run cell the first sub-tile is an
    opaque base (e.g. a carousel pole + its backdrop) and the rest overlay through it — that
    is what lets the merry-go-round poles show *through* the animal tiles in world 2. (Real
    data only ever uses 0xf8 = 2 sub-tiles per run cell.) NOTE: the atlas must be the .PAV
    raster rendered at offset 6 — the .PAV has a 6-byte header (restore_bg_tile_run uses
    `level_pav_buf+6`); offset 0 misaligns every tile.
    """
    grid = 2 + lvl * 0x32c

    def put(tile_id: int, dx: int, dy: int, mask: bool) -> None:
        sx = (tile_id % 20) * TILE
        sy = (tile_id // 20) * TILE
        for yy in range(TILE):
            ay = sy + yy
            oy = dy + yy
            if ay >= BG_H or not (0 <= oy < H):
                continue
            arow = ay * W + sx
            orow = (oy * W + dx) * 3
            for xx in range(TILE):
                if 0 <= dx + xx < W and sx + xx < W:
                    if mask and atlas_idx[arow + xx] == 0:
                        continue
                    o = orow + xx * 3
                    a = (arow + xx) * 3
                    rgb[o:o + 3] = atlas[a:a + 3]

    for cy in range(0, 0x1a, 2):
        for cx in range(20):
            slot = grid + cx * 0x27 + (cy >> 1) * 3 + 0x20
            code = dec[slot]
            if code == 0:
                continue
            if code >= 0xf1:                       # run: opaque base + masked overlay(s)
                col_count = (-code - 5) & 0xFF
                for i in range(1, col_count):
                    tid = dec[slot + i] - 1
                    if tid >= 0:
                        put(tid, cx * TILE, cy * 8, mask=(i > 1))
            else:
                put(code - 1, cx * TILE, cy * 8, mask=False)


def render_level(atlas: bytes, atlas_idx: bytes, dec: bytes, bum: bytes, lvl: int, spr: Sprites,
                 world: int = 1, pal=PALETTE) -> bytearray:
    """Composite one puzzle level: .DEC-tile background + layers A/B/C + Bumpy at spawn."""
    # The engine clears the view to palette index 0 before drawing tiles, so background
    # grid cells with code 0 (and any masked-through gaps) show that base colour rather
    # than black. Pre-fill with pal[0] to match (e.g. world 3's dark-purple backdrop).
    rgb = bytearray(bytes(pal[0]) * (W * H))
    draw_background(rgb, atlas, atlas_idx, dec, lvl)
    base = 2 + lvl * 0xc2
    layer_a = bum[base + 0x00:base + 0x30]
    layer_b = bum[base + 0x30:base + 0x60]
    layer_c = bum[base + 0x60:base + 0x90]

    def draw_anim(layer, posname, codemap):
        pos = ANIM[posname]
        for cell in range(48):
            code = layer[cell]
            if code == 0:
                continue
            if posname == "posB" and cell % 8 == 7:        # layer B skips col 7
                continue
            ent = codemap.get(str(code))
            if ent is None:
                continue
            fr = spr.frame(ent["frame"])
            if fr is None:
                continue
            x, y = pos[cell]
            blit(rgb, fr, x, y + ent["yoff"], pal)

    draw_anim(layer_a, "posA", ANIM["A"])
    draw_anim(layer_b, "posB", ANIM["B"])
    # layer C: static sprites, frame = code + 0x179, position table posC (no y-offset)
    posc = ANIM["posC"]
    for cell in range(48):
        code = layer_c[cell]
        if code == 0:
            continue
        fr = spr.frame(code + LAYER_C_FRAME_BIAS)
        if fr is None:
            continue
        x, y = posc[cell]
        blit(rgb, fr, x, y, pal)
    # Bumpy (player) at its spawn cell (header +0x90, 1-based; cell -> posC pixel)
    spawn = bum[base + 0x90]
    pl = spr.frame(PLAYER_FRAME)
    if pl is not None and 1 <= spawn <= 48:
        x, y = ANIM["posC"][spawn - 1]
        blit(rgb, pl, x, y, pal)
    # P2 / enemy opponent at its starting cell, when placed (header +0x93 != 0). Its sprite
    # = P2_FRAME_TABLE[level[0x96]] drawn at posC[p2_cell]+(7,7) (p2_set_pixel_from_cell).
    p2c = bum[base + 0x93]
    if p2c != 0:
        p2_cell = p2c - 1
        fb = bum[base + 0x96]
        frame = P2_FRAME_TABLE[fb] if fb < len(P2_FRAME_TABLE) else 0
        en = spr.frame(frame)
        if en is not None and 0 <= p2_cell < 48:
            x, y = ANIM["posC"][p2_cell]
            blit(rgb, en, x + 7, y + 7, pal)
    return rgb


def montage(tiles: list, cols: int, scale: int) -> tuple:
    """Pack a list of (W,H,rgb) tiles into a grid montage. scale=1 keeps thumbnails at
    full 1:1 resolution (whole-row copy); scale>1 nearest-downsamples. Returns (w,h,rgb)."""
    tw, th, pad = W // scale, H // scale, 4
    rows = (len(tiles) + cols - 1) // cols
    mw, mh = cols * (tw + pad) + pad, rows * (th + pad) + pad
    out = bytearray(mw * mh * 3)
    for i, rgb in enumerate(tiles):
        cx = (i % cols) * (tw + pad) + pad
        cy = (i // cols) * (th + pad) + pad
        for y in range(th):
            d = ((cy + y) * mw + cx) * 3
            if scale == 1:                                 # 1:1 — copy the whole row
                s = (y * W) * 3
                out[d:d + tw * 3] = rgb[s:s + tw * 3]
            else:
                for x in range(tw):
                    s = ((y * scale) * W + (x * scale)) * 3
                    out[d + x * 3:d + x * 3 + 3] = rgb[s:s + 3]
    return mw, mh, bytes(out)


def main() -> None:
    # CLI: render_levels.py [world ...] [--montage]
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    do_montage = "--montage" in sys.argv[1:] or not args
    worlds = [int(a) for a in args] or list(range(1, 10))
    spr = Sprites()
    out = os.path.join(ROOT, "local/results/levels_png")
    os.makedirs(out, exist_ok=True)
    total = 0
    for n in worlds:
        pal = world_palette(n)                                       # per-world gameplay palette
        pav = decompress(os.path.join(GAME, "D%d.PAV" % n), 0x7806)
        atlas = render_planar(pav, W, BG_H, pal, "seq", 6)           # .PAV brush atlas (6-byte header + 320x192)
        atlas_idx = planar_indices(pav, W, BG_H, 6)                  # parallel index map (for color-0 masking)
        dec = decompress(os.path.join(GAME, "D%d.DEC" % n), 0x2f96)  # background tile grid
        bum, nlevels = load_bum(n)                                   # pure-Python decode + real puzzle count
        tiles = []
        for lvl in range(nlevels):
            b = 2 + lvl * 0xc2
            if not any(bum[b:b + 0x90]):
                continue
            rgb = bytes(render_level(atlas, atlas_idx, dec, bum, lvl, spr, world=n, pal=pal))
            write_png(os.path.join(out, "world%d_lvl%02d.png" % (n, lvl + 1)), W, H, rgb)
            tiles.append(rgb)
            total += 1
        if do_montage and tiles:
            mw, mh, mrgb = montage(tiles, cols=5, scale=1)
            write_png(os.path.join(out, "world%d_montage.png" % n), mw, mh, mrgb)
        print("world %d: rendered %d levels -> world%d_lvl*.png%s" % (
            n, len(tiles), n, " (+montage)" if do_montage and tiles else ""), flush=True)
    print("TOTAL: %d puzzle PNGs in results/levels_png/ (pure-Python, real .DEC background)"
          % total)


if __name__ == "__main__":
    main()
