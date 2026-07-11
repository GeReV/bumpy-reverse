#!/usr/bin/env python3
"""Derive runtime capture offsets from an Open Watcom linker map (BUMPYP.map).

The BUMPYP capture harness (tools/dosbox/*.sh) needs several BUMPYCAP_* values that
are *layout-dependent*: the runtime DGROUP segment and the DGROUP-relative offsets of
globals such as ``_current_level`` and ``_game_mode``.  Historically these were
hand-copied constants (e.g. ``BUMPYCAP_DGROUP=0x49d9``) that silently went stale every
time the link layout shifted -- which is exactly what the DGROUP sweep deliberately
does.  This tool reads them straight out of the freshly-generated ``.map`` so nothing
is hardcoded.

Map anatomy (Open Watcom "Version 2.0" text map, ``-fm=`` output)::

    +---- Groups ----+
    DGROUP            43c9:0000   0000ab10        <- link-time DGROUP paragraph

    +---- Segments ----+
    Segment       Class      Group    Address     Size
    main_TEXT     CODE       AUTO     0000:0000   0000005b
    ...
    _BSS          BSS        DGROUP   4754:000a   00003254

    +---- Memory Map ----+
    Address        Symbol
    =======        ======
    Module: play/game.obj(play/game.obj)
    43c9:0c26      _current_level                 <- data: SEG == DGROUP link seg
    0000:0c34      present_frame_                 <- code: SEG == 0000 (code group)

Address arithmetic.  DOS loads the EXE image at a fixed paragraph for a given DOSBox
memory config (the "load base"; 0x824 for tools/dosbox/bumpy-capture.conf).  So::

    runtime_seg = link_seg + load_base
    runtime DGROUP = 0x43c9 + 0x824 = 0x4bed

A *data* symbol's link segment equals the DGROUP link paragraph, so its DGROUP-relative
offset is just its ``:off`` (what BUMPYCAP_OFF_* / _POKE_OFF want).  A *code* symbol
lives in the code group (link seg 0x0000 while the code fits one 64 KiB segment), so its
runtime CS:IP is ``(load_base + link_seg):off`` (what WWATCH / FB_TRIG_* want).

Pure stdlib; no third-party deps.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

# DOS load paragraph for tools/dosbox/bumpy-capture.conf.  Constant for a given DOSBox
# memory layout (it is where DOS places PSP+0x10), independent of the EXE contents;
# override with --load-base if the capture conf's memory config changes.
DEFAULT_LOAD_BASE = 0x824

# Well-known symbol -> BUMPYCAP variable(s) the w2 capture harness consumes.  Each entry
# is (map_symbol_name, kind) where kind selects which address form to emit:
#   "dgroup_off" -> the DGROUP-relative offset (BUMPYCAP_OFF_* / _POKE_OFF)
#   "code_csip"  -> runtime CS + IP (BUMPYCAP_*_TRIG_CS / _IP, WWATCH)
WELL_KNOWN: dict[str, tuple[str, str]] = {
    "_current_level": ("_current_level", "dgroup_off"),
    "_game_mode": ("_game_mode", "dgroup_off"),
}

_HEX = r"[0-9a-fA-F]"
# A Groups-section DGROUP line: "DGROUP   43c9:0000   0000ab10".
_GROUP_RE = re.compile(
    rf"^DGROUP\s+({_HEX}{{1,4}}):({_HEX}{{1,4}})\s+({_HEX}+)\s*$"
)
# A symbol line in the Memory Map: "43c9:0c26+    _current_level".  The optional
# trailing flag is '+' (referenced only locally) or '*' (unreferenced).
_SYM_RE = re.compile(
    rf"^({_HEX}{{1,4}}):({_HEX}{{1,4}})([+*]?)\s+(\S+)\s*$"
)


@dataclass(frozen=True)
class Symbol:
    """One entry from the map's Address/Symbol section."""

    name: str
    link_seg: int
    off: int
    flag: str  # '', '+', or '*'


@dataclass
class MapInfo:
    """Everything derived from a single linker map."""

    dgroup_link_seg: int
    load_base: int
    symbols: dict[str, Symbol]

    @property
    def dgroup_runtime_seg(self) -> int:
        return self.dgroup_link_seg + self.load_base

    def runtime_seg(self, sym: Symbol) -> int:
        """Runtime segment for a symbol (data -> runtime DGROUP; code -> load_base+seg)."""
        return sym.link_seg + self.load_base

    def is_dgroup(self, sym: Symbol) -> bool:
        return sym.link_seg == self.dgroup_link_seg

    def dgroup_off(self, name: str) -> int:
        """DGROUP-relative offset of a near-data global (raises if not in DGROUP)."""
        sym = self.require(name)
        if not self.is_dgroup(sym):
            raise ValueError(
                f"{name!r} is not a DGROUP symbol (link seg "
                f"{sym.link_seg:#06x} != DGROUP {self.dgroup_link_seg:#06x})"
            )
        return sym.off

    def require(self, name: str) -> Symbol:
        try:
            return self.symbols[name]
        except KeyError:
            raise KeyError(f"symbol {name!r} not found in map") from None


def parse_map(text: str, load_base: int = DEFAULT_LOAD_BASE) -> MapInfo:
    """Parse an Open Watcom text map into a MapInfo."""
    dgroup_link_seg: int | None = None
    symbols: dict[str, Symbol] = {}
    in_symbol_section = False

    for raw in text.splitlines():
        line = raw.rstrip("\n")

        if dgroup_link_seg is None:
            m = _GROUP_RE.match(line)
            if m is not None:
                dgroup_link_seg = int(m.group(1), 16)
                continue

        # The Address/Symbol table opens with a "Address ... Symbol" header; only
        # parse symbol lines after it so we never mistake a Segments-section row for a
        # symbol (segment rows carry extra columns and never start with "hex:hex ").
        if not in_symbol_section:
            if re.match(r"^Address\s+Symbol\s*$", line):
                in_symbol_section = True
            continue

        if line.startswith("Module:"):
            continue
        m = _SYM_RE.match(line)
        if m is None:
            continue
        seg = int(m.group(1), 16)
        off = int(m.group(2), 16)
        flag = m.group(3)
        name = m.group(4)
        # Keep the first definition; a duplicate name in a Watcom map is a comdat/alias
        # and the first (real) address is the one we want.
        if name not in symbols:
            symbols[name] = Symbol(name=name, link_seg=seg, off=off, flag=flag)

    if dgroup_link_seg is None:
        raise ValueError("no DGROUP group line found in map (not an Open Watcom map?)")

    return MapInfo(
        dgroup_link_seg=dgroup_link_seg, load_base=load_base, symbols=symbols
    )


def _hex16(v: int) -> str:
    return f"0x{v:04x}"


def emit_env(info: MapInfo, extra: dict[str, str]) -> list[str]:
    """Shell-assignable KEY=VALUE lines for the w2 capture harness.

    ``extra`` maps additional map-symbol names to BUMPYCAP variable names, e.g.
    ``{"present_frame_": "BUMPYCAP_FB_TRIG"}`` -> emits _CS/_IP for a code symbol.
    """
    lines: list[str] = [
        f"# derived from linker map; load_base={_hex16(info.load_base)} "
        f"link_dgroup={_hex16(info.dgroup_link_seg)}",
        f"BUMPYCAP_DGROUP={_hex16(info.dgroup_runtime_seg)}",
    ]
    cur = info.dgroup_off("_current_level")
    gm = info.dgroup_off("_game_mode")
    lines.append(f"BUMPYCAP_OFF_CURLEVEL={_hex16(cur)}")
    lines.append(f"BUMPYCAP_OFF_GAMEMODE={_hex16(gm)}")
    # current_level doubles as the world-forcing poke target.
    lines.append(f"BUMPYCAP_POKE_OFF={_hex16(cur)}")

    for sym_name, var in extra.items():
        sym = info.require(sym_name)
        if info.is_dgroup(sym):
            lines.append(f"{var}={_hex16(sym.off)}")
        else:
            lines.append(f"{var}_CS={_hex16(info.runtime_seg(sym))}")
            lines.append(f"{var}_IP={_hex16(sym.off)}")
    return lines


def emit_json(info: MapInfo, names: list[str]) -> str:
    def sym_obj(sym: Symbol) -> dict[str, object]:
        d: dict[str, object] = {
            "link_seg": _hex16(sym.link_seg),
            "off": _hex16(sym.off),
            "flag": sym.flag,
            "runtime_seg": _hex16(info.runtime_seg(sym)),
            "is_dgroup": info.is_dgroup(sym),
        }
        if info.is_dgroup(sym):
            d["dgroup_off"] = _hex16(sym.off)
        return d

    wanted = names or list(WELL_KNOWN.keys())
    out: dict[str, object] = {
        "load_base": _hex16(info.load_base),
        "dgroup_link_seg": _hex16(info.dgroup_link_seg),
        "dgroup_runtime_seg": _hex16(info.dgroup_runtime_seg),
        "symbols": {
            n: sym_obj(info.symbols[n]) for n in wanted if n in info.symbols
        },
        "missing": [n for n in wanted if n not in info.symbols],
    }
    return json.dumps(out, indent=2)


def parse_symbol_specs(specs: list[str]) -> dict[str, str]:
    """Parse ``--symbol NAME=VAR`` / ``--symbol NAME`` into {map_name: bumpycap_var}."""
    out: dict[str, str] = {}
    for spec in specs:
        if "=" in spec:
            name, var = spec.split("=", 1)
        else:
            name = spec
            var = "BUMPYCAP_OFF_" + name.strip("_").upper()
        out[name] = var
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Derive BUMPYCAP_* capture offsets from an Open Watcom map."
    )
    ap.add_argument("map", type=Path, help="path to BUMPYP.map")
    ap.add_argument(
        "--load-base",
        type=lambda s: int(s, 0),
        default=DEFAULT_LOAD_BASE,
        help=f"DOS load paragraph (default {_hex16(DEFAULT_LOAD_BASE)})",
    )
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument(
        "--env", action="store_true", help="emit shell KEY=VALUE lines (default)"
    )
    mode.add_argument("--json", action="store_true", help="emit JSON")
    mode.add_argument(
        "--query", metavar="NAME", help="print one symbol's runtime seg:off"
    )
    ap.add_argument(
        "--symbol",
        action="append",
        default=[],
        metavar="NAME[=VAR]",
        help="extra symbol to include in --env/--json (repeatable)",
    )
    args = ap.parse_args(argv)

    try:
        info = parse_map(args.map.read_text(), load_base=args.load_base)
    except (OSError, ValueError) as e:
        print(f"map_offsets: {e}", file=sys.stderr)
        return 2

    extra = parse_symbol_specs(args.symbol)

    if args.query is not None:
        sym = info.symbols.get(args.query)
        if sym is None:
            print(f"map_offsets: symbol {args.query!r} not found", file=sys.stderr)
            return 1
        rt = info.runtime_seg(sym)
        if info.is_dgroup(sym):
            print(f"{_hex16(rt)}:{_hex16(sym.off)} dgroup_off={_hex16(sym.off)}")
        else:
            print(f"{_hex16(rt)}:{_hex16(sym.off)}")
        return 0

    if args.json:
        print(emit_json(info, list(extra.keys())))
        return 0

    # default: env
    try:
        for line in emit_env(info, extra):
            print(line)
    except (KeyError, ValueError) as e:
        print(f"map_offsets: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
