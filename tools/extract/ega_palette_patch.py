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

# DGROUP (seg 0x203b) offsets of the 16-byte AC palette-patch tables.  These are the
# per-SCREEN AC-index palettes the title/menu/copyprot builders copy over img+0x23 when
# palette_mode==1 (show_title_background/…/decode_copyprot_level; see design §3.3).
TABLES: dict[str, int] = {
    "dgroup_pal_patch_63a": 0x63A,  # show_title_background
    "dgroup_pal_patch_72e": 0x72E,  # show_title_and_init
    "dgroup_pal_patch_64a": 0x64A,  # run_main_menu
    "dgroup_pal_patch_71e": 0x71E,  # show_menu_select_screen / highscore
    "copyprot_palette_src": 0x65A,  # decode_copyprot_level  (== per-level table for level 1)
}

# The nine per-LEVEL/world AC-index palettes level_intro_screen (1000:3852, the overworld
# map) copies over the decoded MONDE<n>.VEC image at img+0x23 when palette_mode==1.  The
# engine reaches them through level_palette_ptr_table (DGROUP 0x6e6, a far-ptr array indexed
# by current_level 1..9); each entry points at one of these 16-byte tables.  Contiguous at
# DGROUP 0x65a, stride 0x10 (level 1 shares copyprot_palette_src 0x65a).
LEVEL_TABLE_BASE = 0x65A
LEVEL_TABLE_COUNT = 9  # current_level 1..9 (level_palette_ptr_table[1..9])

# The single FIXED in-game AC-index palette load_palette (1000:08d1) copies over the staging
# buffer at +0x23 when palette_mode==1 — used for ALL in-level gameplay frames (every world),
# unlike the per-world overworld tables above.
INGAME_TABLE_OFF = 0x70E

def foff(seg: int, off: int) -> int:
    return HDR + (seg - 0x1000) * 16 + off

def tbl16(data: bytes, off: int) -> list[int]:
    base = foff(0x203B, off)
    return list(data[base:base + 16])

def extract() -> dict[str, object]:
    data = UNPACKED.read_bytes()
    out: dict[str, object] = {}
    for name, off in TABLES.items():
        out[name] = tbl16(data, off)
    out["level_palette_tables"] = [
        tbl16(data, LEVEL_TABLE_BASE + i * 0x10) for i in range(LEVEL_TABLE_COUNT)
    ]
    out["ingame_palette_70e"] = tbl16(data, INGAME_TABLE_OFF)
    return out

def main() -> None:
    tables = extract()
    # Self-check: run_main_menu's AC is the fidelity-doc reference value.
    assert tables["dgroup_pal_patch_64a"] == [
        0, 0, 0, 0, 0, 0, 0, 2, 0xA, 4, 6, 6, 0xC, 0xE, 0xF, 0xF
    ], tables["dgroup_pal_patch_64a"]
    # Self-check: the copyprot table IS the level-1 per-level table (same DGROUP 0x65a).
    assert tables["copyprot_palette_src"] == tables["level_palette_tables"][0], tables
    # Self-check: the fixed in-game table is the RE'd value.
    assert tables["ingame_palette_70e"] == [
        0, 1, 9, 0xE, 0xA, 5, 4, 6, 0xC, 2, 0xA, 9, 0xB, 5, 7, 0
    ], tables["ingame_palette_70e"]
    # Every AC entry must be a valid EGA colour index (0..15).
    flat: list[list[int]] = [
        v for k, v in tables.items() if k not in ("level_palette_tables",)
        for v in ([v] if isinstance(v[0], int) else v)  # type: ignore[index]
    ]
    for v in flat + list(tables["level_palette_tables"]):  # type: ignore[operator]
        assert all(b <= 0x0F for b in v), v
    Path("results").mkdir(exist_ok=True)
    Path("results/ega_palette_patch.json").write_text(json.dumps(tables, indent=2))
    for name in TABLES:
        body = ", ".join(f"0x{b:02x}u" for b in tables[name])  # type: ignore[arg-type]
        print(f"static const u8 {name}_src[16] = {{ {body} }};")
    print("static const u8 level_ega_ac_tables[9][16] = {")
    for i, v in enumerate(tables["level_palette_tables"], start=1):  # type: ignore[arg-type]
        body = ", ".join(f"0x{b:02x}u" for b in v)
        print(f"    {{ {body} }},  /* level {i} @0x{LEVEL_TABLE_BASE + (i - 1) * 0x10:03x} */")
    print("};")
    body = ", ".join(f"0x{b:02x}u" for b in tables["ingame_palette_70e"])  # type: ignore[arg-type]
    print(f"static const u8 ingame_ega_ac_70e[16] = {{ {body} }};")

if __name__ == "__main__":
    main()
