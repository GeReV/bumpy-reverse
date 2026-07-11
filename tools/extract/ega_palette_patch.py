#!/usr/bin/env python3
"""Extract the EGA palette-patch source tables (the 16-byte Attribute-Controller
palettes) that the title/menu/copyprot screens copy over img+0x23 when palette_mode==1.
Source: the unpacked, static BUMPY image. Grounded in the cmdvec/patch RE (2026-07-11)."""
from __future__ import annotations
import json
import struct
from pathlib import Path

UNPACKED = Path("local/build/unpack/BUMPY_unpacked.exe")
HDR = 265 * 16  # e_cparhdr(265)*16; image base seg 0x1000 -> file off 0 after header

# DGROUP (seg 0x203b) offsets of the 16-byte AC palette-patch tables.
TABLES: dict[str, int] = {
    "dgroup_pal_patch_63a": 0x63A,  # show_title_background
    "dgroup_pal_patch_72e": 0x72E,  # show_title_and_init
    "dgroup_pal_patch_64a": 0x64A,  # run_main_menu
    "dgroup_pal_patch_71e": 0x71E,  # show_menu_select_screen / highscore
    "copyprot_palette_src": 0x65A,  # decode_copyprot_level
}

def foff(seg: int, off: int) -> int:
    return HDR + (seg - 0x1000) * 16 + off

def extract() -> dict[str, list[int]]:
    data = UNPACKED.read_bytes()
    out: dict[str, list[int]] = {}
    for name, off in TABLES.items():
        base = foff(0x203B, off)
        out[name] = list(data[base:base + 16])
    return out

def main() -> None:
    tables = extract()
    # Self-check: run_main_menu's AC is the fidelity-doc reference value.
    assert tables["dgroup_pal_patch_64a"] == [
        0, 0, 0, 0, 0, 0, 0, 2, 0xA, 4, 6, 6, 0xC, 0xE, 0xF, 0xF
    ], tables["dgroup_pal_patch_64a"]
    for v in tables.values():
        assert all(b <= 0x0F for b in v), v  # EGA colour indices 0..15
    Path("results").mkdir(exist_ok=True)
    Path("results/ega_palette_patch.json").write_text(json.dumps(tables, indent=2))
    for name, v in tables.items():
        body = ", ".join(f"0x{b:02x}u" for b in v)
        print(f"static const u8 {name}_src[16] = {{ {body} }};")

if __name__ == "__main__":
    main()
