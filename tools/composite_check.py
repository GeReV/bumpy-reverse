#!/usr/bin/env python3
"""Render the full background (bg_render_grid logic) from frame_oracle.bin's atlas+map
and diff it against the captured engine frame.  Confirms the bg matches the real
composite and isolates the entity pixels (where bg != frame = drawn sprites).
Writes a diff PNG: background dimmed, entity pixels in magenta."""
import struct
import sys
import importlib.util
from typing import Dict, List

spec = importlib.util.spec_from_file_location("bgref", "tools/bg_blit_ref.py")
bgref = importlib.util.module_from_spec(spec); spec.loader.exec_module(bgref)
PLANE = 0x10000


def load(path: str) -> tuple[bytes, bytes, bytes, bytes]:
    """Read an FRM2, FRM3, or FRM4 file and return the bg-relevant fields.

    Accepts FRM4 (new) as well as FRM3 and legacy FRM2 tags so the bg-diff
    path continues to work after the oracle is regenerated.

    Returns a 4-tuple (planes, dac, atlas, bmap) of bytes.
    """
    d = load_frame3(path)
    return d["planes"], d["dac"], d["atlas"], d["map"]


def load_frame3(path: str) -> Dict:
    """Parse an FRM3 oracle file into a dict with all blocks.

    Keys returned:
        tag (bytes)        — b"FRM4", b"FRM3" or b"FRM2" (legacy; new-block fields are empty stubs)
        planes (bytes)     — 4 VGA planes, 0x10000 B each (total 0x40000 B)
        dac (bytes)        — 256*3 palette (0..63 per channel)
        atlas (bytes)      — PAV atlas raster (0x8000 B)
        map (bytes)        — level tile map (0x1000 B)
        level (int)        — 1-based level index
        bum (bytes)        — 0xc2 B BUM per-level header for this level
        p1_obj (bytes)     — 0x18 B p1 sprite obj struct (DGROUP:0x792e)
        p2_obj (bytes)     — 0x18 B p2 sprite obj struct (DGROUP:0x795a)
        p1_glob (bytes)    — 6 B: pixel_x(u16) pixel_y(u16) move_anim(u16)
        p2_glob (bytes)    — 6 B: pixel_x(u16) pixel_y(u16) move_anim(u16)
        chan_a (list)       — 3 x bytes(0xc): layer-A channel records
        chan_b (list)       — 4 x bytes(0xc): layer-B channel records
        chan_tbl_raw (bytes)— 8 B: 4 u16 far-ptr table words
                               (A_off, A_seg, B_off, B_seg)
        dg (bytes)         — 0x10000 B full DGROUP snapshot
    """
    with open(path, "rb") as fh:
        b = fh.read()
    tag = b[:4]
    assert tag in (b"FRM2", b"FRM3", b"FRM4"), "unexpected tag: %r" % tag

    o = 4
    plen = struct.unpack_from("<I", b, o)[0]; o += 4
    planes = b[o:o + plen]; o += plen
    dac = b[o:o + 256 * 3]; o += 256 * 3
    alen = struct.unpack_from("<I", b, o)[0]; o += 4
    atlas = b[o:o + alen]; o += alen
    mlen = struct.unpack_from("<I", b, o)[0]; o += 4
    bmap = b[o:o + mlen]; o += mlen

    if tag == b"FRM2":
        # Legacy file: return stub values for new fields so callers can test gracefully.
        return dict(tag=tag, planes=planes, dac=dac, atlas=atlas, map=bmap,
                    level=0, bum=b"", p1_obj=b"", p2_obj=b"",
                    p1_glob=b"", p2_glob=b"",
                    chan_a=[], chan_b=[],
                    chan_tbl_raw=b"", dg=b"")

    # FRM3 / FRM4 new blocks (identical positions for both tags)
    assert tag in (b"FRM3", b"FRM4")
    level = struct.unpack_from("<H", b, o)[0]; o += 2
    bum = b[o:o + 0xc2]; o += 0xc2
    p1_obj = b[o:o + 0x18]; o += 0x18
    p2_obj = b[o:o + 0x18]; o += 0x18
    p1_glob = b[o:o + 6]; o += 6
    p2_glob = b[o:o + 6]; o += 6
    # Channel records: layer-A (3 × 0xc), layer-B (4 × 0xc)
    chan_a: List[bytes] = []
    for _ in range(3):
        chan_a.append(b[o:o + 0xc]); o += 0xc
    chan_b: List[bytes] = []
    for _ in range(4):
        chan_b.append(b[o:o + 0xc]); o += 0xc
    chan_tbl_raw = b[o:o + 8]; o += 8
    dg = b[o:o + 0x10000]; o += 0x10000

    return dict(tag=tag, planes=planes, dac=dac, atlas=atlas, map=bmap,
                level=level, bum=bum, p1_obj=p1_obj, p2_obj=p2_obj,
                p1_glob=p1_glob, p2_glob=p2_glob,
                chan_a=chan_a, chan_b=chan_b,
                chan_tbl_raw=chan_tbl_raw, dg=dg)


def load_frame4(path: str) -> Dict:
    """Parse an FRM4 oracle file into a dict with all blocks including FRM4 additions.

    Extends load_frame3 with the present-path data captured by Plan 6c Task 2.
    Returns a superset of the load_frame3 dict.  Falls back gracefully if the file
    is an older FRM2/FRM3 (FRM4-specific keys will be empty stubs).

    Additional keys beyond load_frame3:
        fullscreen_buf_seg (int)  — seg from DGROUP:0x7928
        fullscreen_buf_off (int)  — off from DGROUP:0x7926
        fullscreen_buf (bytes)    — 32000 B, 4 planes * 8000 B each
                                    (save-under captured at level start:
                                     setup_fullscreen_view copies a000→fullscreen_buf)
        present_calls (list)      — list of dicts, one per active gfx_set_mode_10 call:
            desc_seg (int)         — view descriptor seg
            desc_off (int)         — view descriptor off
            w0 (int)               — view->word[0]: 0=src a200, 1=src a000
            dest_seg (int)         — dest far ptr seg (view+0x12)
            dest_off (int)         — dest far ptr off (view+0x10)
            sh_idx (int)           — sub-handler index (view+0x1c)
            n_all (int)            — total gfx_set_mode_10 calls so far at this point
            call_ord (int)         — ordinal of this record in present_calls list
        csd_obs (list)            — list of dicts, one per blit_sprite_vga call:
            csd_seg (int)          — cur_sprite_data seg
            csd_off (int)          — cur_sprite_data off
            call_n (int)           — per-sprite call index
    """
    d = load_frame3(path)
    # Stub values for non-FRM4 files
    d["fullscreen_buf_seg"] = 0
    d["fullscreen_buf_off"] = 0
    d["fullscreen_buf"] = b""
    d["present_calls"] = []
    d["csd_obs"] = []

    if d["tag"] != b"FRM4":
        return d

    # Re-open to read from where load_frame3 left off.  Rather than re-parsing the
    # whole file, rebuild the offset: fixed prefix + variable atlas/map.
    with open(path, "rb") as fh:
        b = fh.read()

    # Locate the FRM4 block start: skip tag(4)+planes_len(4)+planes+dac+atlas_hdr+atlas+map_hdr+map
    # then the FRM3 new-block fields.
    # Reconstruct `o` (offset after dg block) — same parse as load_frame3.
    o = 4
    plen = struct.unpack_from("<I", b, o)[0]; o += 4
    o += plen             # planes
    o += 256 * 3          # dac
    alen = struct.unpack_from("<I", b, o)[0]; o += 4
    o += alen             # atlas
    mlen = struct.unpack_from("<I", b, o)[0]; o += 4
    o += mlen             # map
    # FRM3 new blocks
    o += 2                # level u16
    o += 0xc2             # bum
    o += 0x18 + 0x18      # p1_obj + p2_obj
    o += 6 + 6            # p1_glob + p2_glob
    o += 3 * 0xc          # chan_a
    o += 4 * 0xc          # chan_b
    o += 8                # chan_tbl_raw
    o += 0x10000          # dg

    if o + 4 + 4 > len(b):
        return d          # truncated / no FRM4 block

    # FRM4 block
    fb_seg, fb_off = struct.unpack_from("<HH", b, o); o += 4
    fb_len = struct.unpack_from("<I", b, o)[0]; o += 4
    fb_data = b[o:o + fb_len]; o += fb_len
    d["fullscreen_buf_seg"] = fb_seg
    d["fullscreen_buf_off"] = fb_off
    d["fullscreen_buf"] = fb_data

    if o + 2 > len(b):
        return d
    n_present = struct.unpack_from("<H", b, o)[0]; o += 2
    present_calls = []
    for _ in range(n_present):
        if o + 18 > len(b):
            break
        desc_seg, desc_off, w0, dest_seg, dest_off, sh_idx, n_all, call_ord = \
            struct.unpack_from("<HHHHHHHI", b, o)
        o += 18
        present_calls.append(dict(
            desc_seg=desc_seg, desc_off=desc_off,
            w0=w0, dest_seg=dest_seg, dest_off=dest_off,
            sh_idx=sh_idx, n_all=n_all, call_ord=call_ord,
        ))
    d["present_calls"] = present_calls

    if o + 2 > len(b):
        return d
    n_csd = struct.unpack_from("<H", b, o)[0]; o += 2
    csd_obs = []
    for _ in range(n_csd):
        if o + 6 > len(b):
            break
        csd_seg, csd_off, call_n = struct.unpack_from("<HHH", b, o); o += 6
        csd_obs.append(dict(csd_seg=csd_seg, csd_off=csd_off, call_n=call_n))
    d["csd_obs"] = csd_obs

    return d


def idx_at(planes: bytes, x: int, y: int, page_off: int = 0) -> int:
    """Return the 4-bit palette index at pixel (x, y) in a 4-plane buffer.

    page_off — byte offset within each plane's 0x10000-byte region to use as
    the start of the 320x200 scanlines.  0 = page0 (a000), 0x2000 = page1 (a200).
    """
    off = y * 40 + x // 8 + page_off
    m = 0x80 >> (x & 7)
    return (((planes[off] & m) and 1) | (((planes[PLANE + off] & m) and 1) << 1)
            | (((planes[2 * PLANE + off] & m) and 1) << 2)
            | (((planes[3 * PLANE + off] & m) and 1) << 3))


def _live_plane_off(dg: bytes) -> int:
    """Derive the live VGA page plane-byte offset from the captured DGROUP.

    Reads cur_sprite_data_seg at DGROUP:0x56e4.
      0xa200 -> page1, offset 0x2000
      0xa000 (or other) -> page0, offset 0x0000
    """
    seg = struct.unpack_from("<H", dg, 0x56e4)[0]
    if seg == 0xa200:
        return 0x2000
    return 0


def main() -> None:
    src = sys.argv[1] if len(sys.argv) > 1 else "local/build/render/frame_oracle.bin"
    d = load_frame3(src)
    planes = d["planes"]
    dac = d["dac"]
    atlas = d["atlas"]
    bmap = d["map"]
    dg = d["dg"]
    raster = atlas[6:]

    # Determine live page offset from DGROUP:0x56e4 (Plan 6c Task 3)
    live_off = _live_plane_off(dg)

    # render the full bg grid into a fresh plane buffer
    bg = [bytearray(PLANE) for _ in range(4)]
    for cy in range(0, 25, 2):
        for cx in range(20):
            rc = bmap[cx * 0x27 + (cy >> 1) * 3 + 0x20]
            bgref.blit_cell(bg, raster, bmap, dict(cx=cx, cy=cy, run_code=rc))
    bgflat = b"".join(bytes(p) for p in bg)

    # compare bg vs frame at the live page, per pixel (320x200 playfield)
    W, H = 320, 200
    match = diff = 0
    for y in range(H):
        for x in range(W):
            if idx_at(bgflat, x, y) == idx_at(planes, x, y, page_off=live_off):
                match += 1
            else:
                diff += 1
    tot = W * H
    print(f"bg vs frame: {match}/{tot} pixels match ({100*match/tot:.1f}%), "
          f"{diff} differ (entities/overlays)")

    # diff PNG: bg dimmed, entity pixels magenta (uses live page)
    pal = [(min(255, dac[i*3]*255//63), min(255, dac[i*3+1]*255//63),
            min(255, dac[i*3+2]*255//63)) for i in range(256)]
    rgb = bytearray(W * H * 3)
    for y in range(H):
        for x in range(W):
            fi = idx_at(planes, x, y, page_off=live_off)
            if idx_at(bgflat, x, y) == fi:
                r, g, b = pal[fi]; r //= 3; g //= 3; b //= 3
            else:
                r, g, b = 255, 0, 255
            px = (y * W + x) * 3
            rgb[px] = r; rgb[px + 1] = g; rgb[px + 2] = b
    out = "local/build/render/composite_diff.png"
    frspec = importlib.util.spec_from_file_location("fr", "tools/frame_render.py")
    fr = importlib.util.module_from_spec(frspec); frspec.loader.exec_module(fr)
    fr.write_png(out, W, H, rgb)
    print(f"(entities highlighted magenta) -> {out}")


if __name__ == "__main__":
    main()
