# 01 — Inventory & Fingerprinting

**Project location:** `~/fable5-retro-greenfield` (native ext4). Relocated here from the
`/mnt/c/...` Windows mount for speed and to avoid DrvFs read-only issues.

Three copies of "Bumpy" were provided across the sources. They fall into **two different
games**: the 1989 Loriciel original, and the 1992 "Bumpy's Arcade Fantasy" re-release.

## Target

**`Bumpys-Arcade-Fantasy_DOS_EN.zip` — the English "Bumpy's Arcade Fantasy" (1992)** is the
intended reverse-engineering target. (The first MyAbandonware download, `Bumpy_myabandonware.zip`,
was the wrong game — the 1989 French original — and is kept only as reference.)

## The three copies

### A — `Bumpy_myabandonware.zip` → `originals/myabandonware/Bumpy/`  *(WRONG GAME — reference only)*
The **1989 Loriciel original** (French).

| Property | Value |
|---|---|
| `BUMPY.EXE` SHA256 | `f5ab66ba177679cdcd1231dde165d50e938a07944dd817d17831ca262e957036` |
| Packer | **Microsoft EXEPACK** → unpacked to `originals/unpacked/BUMPY1989.EXE` (`2e7322d2…`) |
| Compiler | **Borland Turbo C 2.0** (`Turbo-C - Copyright (c) 1988 Borland Intl.`) |
| Graphics | CGA via Borland **BGI** (`CGA.BGI`) |
| Data | `.BUM`, `.CPL`, `.SPL`; French strings; floppy `A:` paths; built-in level editor |

### B — `Bumpys-Arcade-Fantasy_DOS_EN.zip` → `originals/myabandonware-baf/bumpy-s-arcade-fantasy/bumpy/`  *(TARGET)*
The **1992 English "Bumpy's Arcade Fantasy"** — most complete copy.

| Property | Value |
|---|---|
| `BUMPY.EXE` SHA256 | `172e59a02fc9fbbeb20207d3b11419b2c1ebe8fa4ec5237a550dadfe51f79717` |
| Packer | **TinyProg** (`TINYPROG says, "Bad program file!"`) — not yet unpacked |
| Graphics | VGA; world files `MONDE1-9.VEC`, `TITRE/SCORE/DESSFIN.VEC` |
| Audio | **AdLib/OPL** — `BUMPY.BNK` + `BUMPY.MID` |
| Level data | `D1-D9.{BUM,DEC,PAV}`, `BUMSPJEU.BIN` (sprites), `DDFNT2.CAR` (font) |
| Extras | `INSTALL.BAT`, `RUNME.BAT`, `LIMITS.COM`, `CHKLIST.CPS`, group-release marker files (`WORLD`, `TECH'S`, `BIO-`, `FLT.NFO/BAK`) |

### C — `bumpy-old-games.zip` → `originals/old-games/bumpy/`  *(secondary copy of the 1992 game)*
Another 1992 BAF dump, but a **different build** than B.

| Property | Value |
|---|---|
| `BUMPY.EXE` SHA256 | `69e3a0fd0e219d5e44dc8d68d07c22e0813c8f1ddc8f705eea1f904cb463db57` (≠ copy B) |
| Same overall layout as B | TinyProg-packed; VGA; AdLib; `D1-D9` + `MONDE*` |
| Distributor extras | `Go.bat` (old-games.org banner), `CODES.EXE` (code-wheel?) |

Most data assets (`MONDE*.VEC`, `D*.{BUM,DEC,PAV}`, `BUMSPJEU.BIN`, fonts) are **byte-identical
between B and C** — only the EXE build and packaging differ. Useful as a cross-check.

## Implication for the decomp

The target (B) is **TinyProg-packed with an as-yet-unknown compiler** — the first real RE step is
to unpack it (find/port a TinyProg unpacker, or dump from DOSBox-X memory after the stub runs),
then fingerprint the runtime as was done for the 1989 build. The 1989 Turbo C binary (A) remains a
handy Rosetta stone for Loriciel's coding idioms even though it's a different game.

## Tooling (`tools/`, self-contained, no system installs)

| Tool | Location | Purpose |
|---|---|---|
| Ghidra 12.1.2 | `tools/ghidra_12.1.2_PUBLIC/` — wrapper `tools/bin/ghidra-headless` | Static disassembly/decompilation |
| Temurin JDK 21 | `tools/jdk-21.0.11+10/` | Ghidra runtime (system had JRE only) |
| exepack | `tools/bin/exepack` | EXEPACK unpacker (built from bamsoftware source) |
| DOSBox-X 2026.06.02 | `tools/dosbox-x/` (Windows build, runs via WSL interop) | Dynamic analysis + running the game |

`tools/bin/ghidra-headless` is location-independent (derives its root from its own path) and keeps
Ghidra's config/cache/temp under `tools/ghidra-home/` so nothing touches `~/.config` or `/tmp`.
