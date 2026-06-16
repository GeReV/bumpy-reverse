# Bumpy's Arcade Fantasy — reverse-engineering & source reconstruction

Reverse-engineering of **Bumpy's Arcade Fantasy** (1992, Loriciel) — the DOS English
release. This repo holds documentation, pure-Python asset extractors/renderers, a
Ghidra-based decompilation of the engine, and (in progress) a **reconstructed C
source tree** under `src/`.

The original game files are copyright and **must never be committed**. They live
under the git-ignored `local/` tree; users supply their own.

## The target binary

- **`BUMPY.EXE`** — 16-bit DOS **real-mode**, segmented (medium/large model),
  built with **Turbo C++ 1990 (Borland)**, graphics via **BGI**. Originally
  **TinyProg-packed** (CRC-keyed anti-tamper + LZSS); unpacking is done & verified.
- Work against the **unpacked** image: `local/build/unpack/BUMPY_unpacked.exe`
  (also at `local/originals/unpacked/BUMPY_unpacked.exe`).
- ~399 functions total. Far-pointer / 32-bit global pairs (`_off`/`_seg`,
  `_lo`/`_hi`) are **deliberately kept split** as two 2-byte items — merging makes
  the decompiled C worse for this segmented code. See `docs/06-engine.md`.

## Repo layout

| Path | What | Tracked? |
|------|------|----------|
| `src/` | Reconstructed C source (the current big effort) | yes |
| `tools/extract/` | Pure-stdlib decoders/renderers (`.PAV/.DEC/.BUM`, `.VEC`, `.BIN`, `.CAR`, `.BNK`) | yes |
| `tools/emu/` | From-scratch 16-bit x86 interpreter + Unicorn-based DOS emulators | yes |
| `tools/ghidra_scripts/`, `tools/bin/ghidra-headless` | Ghidra analysis scripts + headless wrapper | yes |
| `tools/*.py` | TinyProg unpacker + relocation/diff helpers | yes |
| `docs/` | Format specs (`docs/formats/`), packing, copy-protection, data files, engine | yes |
| `local/` | Game files, toolchain (Ghidra/JDK/DOSBox), build intermediates, planning | **gitignored** |
| `results/` | Generated outputs (PNGs, sprite sheets, JSON) — regenerable | **gitignored** |

## Ghidra (decompilation source of truth)

- Live project **`BumpyDecomp`** in Ghidra 12.1.2 (`local/toolchain/ghidra_12.1.2_PUBLIC`).
  ~340/399 functions are named, typed, and commented. The remaining ~60 are
  C-runtime startup garble, tiny thunks, and low-confidence stubs — left unnamed
  on purpose; **don't guess** them.
- **Ghidra MCP** is wired up (`.mcp.json` → bridge on `http://127.0.0.1:8080/`).
  Requires the Ghidra GUI running with the GhidraMCP plugin and `BumpyDecomp` open.
  Use the `mcp__ghidra__*` tools to decompile / list / xref / rename / retype.
  If `decompile_function` by friendly name fails, fall back to the raw `FUN_seg_off`
  label or `decompile_function_by_address`.
- When you decompile a function for reconstruction, also keep Ghidra tidy: rename
  vars, set types/prototypes — not just the function name.

## Source reconstruction (`src/`)

- **Goal: compilable & runnable**, organized into a real source tree — builds with a
  16-bit toolchain and behaves like the game. **Not** aiming for byte-identical
  output. (Design doc under `docs/superpowers/specs/` once finalized.)
- Ground every function in the Ghidra decomp + the format/engine docs. Mirror the
  engine's real structure; don't invent abstractions the binary doesn't have.

## Conventions

- **Python tools**: pure stdlib where possible; only emulator/disasm tools need deps
  (`unicorn`, `capstone`). Run via `uv run python tools/...`. Add type annotations.
- **Shell**: use `jq` (not Python) for JSON. Be careful with the sandbox shell —
  zsh `cp -i` alias, `noclobber`; prefer `\cp -f` + absolute paths.
- **Style**: semicolons always; multiline `if` statements (ternaries only for trivial
  single expressions).
- **Docs are human-facing references**, not changelogs. Mermaid only for
  packet/struct/data-layout diagrams, not process flowcharts.
- Never commit anything from `local/` or `results/`, and never commit game data.

## Where to read first

- `README.md` — project overview & how to reproduce.
- `docs/README.md` — documentation index.
- `docs/06-engine.md` — engine internals (game loop, two-player pipeline, physics
  state machine, anim channels). To be rewritten from the reconstructed source.
- `docs/formats/` — file-format specs.
- `docs/tinyprog.md`, `docs/copy-protection.md`, `docs/data-files.md` — systems.
