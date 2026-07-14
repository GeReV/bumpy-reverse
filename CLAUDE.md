# Bumpy's Arcade Fantasy — reverse-engineering & source reconstruction

Reverse-engineering of **Bumpy's Arcade Fantasy** (1992, Loriciel) — the DOS English
release. This repo holds documentation, pure-Python asset extractors/renderers, a
Ghidra-based decompilation of the engine, and (in progress) a **reconstructed C
source tree** under `src/`.

## Leading tenet — decompilation, not reimplementation

The primary deliverable is a **faithful decompilation that documents the original
game's source/assembly**: what the binary *actually is* — every function in its real
structure, its data layouts and algorithms, traceable to the Ghidra decomp + asm.
This is the *Devilution* model (document the original), **not** *Devilution-X* (a
clean, runnable rewrite). A from-scratch reimplementation is a legitimate but
**separate** project. Everything here — the Ghidra project, `docs/`, and the `src/`
tree — serves documenting the original faithfully; prefer fidelity to the binary over
convenient abstractions, and **document every deviation** where 1:1 isn't achievable.

### Adhere to the binary — never invent

**Before reconstructing, fixing, or extending ANY behavior, first establish what the
binary actually does** — decompile/disassemble the relevant function(s) (Ghidra MCP +
the raw asm) and reproduce *that*. Do **not** invent a mechanism, data layout, buffer
scheme, or "equivalent" approximation out of convenience or guesswork. Concretely:

- **Ground every change in the asm/decomp.** If you can't point to the original code
  (or data) a change reproduces, you are inventing — stop and go read the binary.
- **Match the original's structure and data, not just the observable output.** Reuse
  the original's buffers, offsets, tables, and control flow; don't substitute your own.
- **When 1:1 is genuinely impossible** (e.g. the playable host's reduced memory, the
  self-modifying graphics overlay — `gfx_*`, Loriciel-custom, not Borland BGI — that
  doesn't decompile), **surface the constraint and ask
  / document it** — do not silently swap in an invented workaround. Every unavoidable
  deviation gets a `RECONSTRUCTION FIDELITY` note + a `docs/reconstruction-fidelity.md`
  entry, and stays clearly labelled as a deviation.
- **Don't guess** the ~60 unnamed functions or any uncertain data; verify against the
  binary instead.

The original game files are copyright and **must never be committed**. They live
under the git-ignored `local/` tree; users supply their own.

## The target binary

- **`BUMPY.EXE`** — 16-bit DOS **real-mode**, segmented (medium/large model),
  built with the **Turbo C++ 1990 (Borland)** C runtime. Graphics go through the
  game's **own VGA planar overlay** at segment `1ab9` — **NOT stock Borland BGI**
  (verified 2026-07-11: no `EGAVGA.BGI` linked or loaded, no driver banner in the
  image; only a 42-byte incidental match). The overlay's `gfx_*` symbols
  (`gfx_set_mode_00/01/10/11`, `gfx_overlay*`, `gfx_view_desc`, etc.) were **renamed
  from `bgi_*` (2026-07-11)** — an early naming pass wrongly assumed Borland BGI; it is
  a Loriciel-custom engine. Originally **TinyProg-packed** (CRC-keyed anti-tamper +
  LZSS); unpacking is done & verified.
- Work against the **unpacked** image: `local/build/unpack/BUMPY_unpacked.exe`
  (also at `local/originals/unpacked/BUMPY_unpacked.exe`).
- ~399 functions total. Far-pointer / 32-bit global pairs (`_off`/`_seg`,
  `_lo`/`_hi`) are **deliberately kept split** as two 2-byte items — merging makes
  the decompiled C worse for this segmented code. See `docs/engine.md`.

## Repo layout

| Path | What | Tracked? |
|------|------|----------|
| `src/` | Reconstructed C source (the current big effort) | yes |
| `tools/extract/` | Pure-stdlib decoders/renderers (`.PAV/.DEC/.BUM`, `.VEC`, `.BIN`, `.CAR`, `.BNK`) | yes |
| `tools/tinyprog_unpack.py`, `tools/disasm16.py` | TinyProg unpacker + a small disassembly utility | yes |
| `imhex/` | ImHex pattern-language format specs (`.hexpat`), verified via `plcli` | yes |
| `docs/` | Format specs (`docs/formats/`), packing, copy-protection, data files, engine | yes |
| `local/` | Game files, toolchain (Ghidra/JDK/DOSBox/Open Watcom), build intermediates, planning, and the archived dev-only tooling (`local/archive/tools/`: the Ghidra analysis scripts, the from-scratch emulators, the `validate_*.sh`/oracle/ctest fidelity-validation suite, and the DOSBox capture pipeline that produced the results in `docs/reconstruction-fidelity.md`) | **gitignored** |
| `results/` | Generated outputs (PNGs, sprite sheets, JSON) — regenerable | **gitignored** |

## Ghidra (decompilation source of truth)

- Live project **`BumpyDecomp`** in Ghidra 12.1.2 (`local/toolchain/ghidra_12.1.2_PUBLIC`).
  All functions are named, typed, and commented (the last raw `FUN_*` symbols were
  resolved 2026-07-14); a handful of low-confidence identities carry a `maybe_` prefix
  rather than an invented meaning — see `docs/ghidra-symbol-map.md`'s "Uncertain
  identities" section. **Don't guess** them.
- **Ghidra MCP** is wired up (`.mcp.json` → bridge on `http://127.0.0.1:18080/`).
  Requires the Ghidra GUI running with the GhidraMCP plugin and `BumpyDecomp` open.
  Use the `mcp__ghidra__*` tools to decompile / list / xref / rename / retype.
  If `decompile_function` by friendly name fails, fall back to the raw `FUN_seg_off`
  label or `decompile_function_by_address`.
- When you decompile a function for reconstruction, also keep Ghidra tidy: rename
  vars, set types/prototypes — not just the function name.

## Source reconstruction (`src/`)

`src/` is part of the decompilation: a **strictly structure-faithful mirror** of the
original in C — one C function per original function, the same control flow, the same
data layouts — so the C reads as documentation of what the binary does. It is **not**
a reimplementation.

- **Primary goal: structural faithfulness to the original.** Ground every function in
  the Ghidra decomp + asm; mirror the engine's real structure; never invent
  abstractions the binary doesn't have, and don't merge or "clean up" original
  functions. Compilable/runnable is desirable but **secondary** to faithfulness.
- **Document every deviation.** Where 1:1 isn't achievable, add an in-code
  `RECONSTRUCTION FIDELITY` note and record it in the fidelity audit
  (`docs/reconstruction-fidelity.md`). Known reimplementation-leaning deviations to
  flag and, where feasible, bring back toward 1:1: the two planar blitters
  (`sprite_blit`, `bg_render`) are *behavior*-faithful semantic reconstructions of
  self-modifying graphics-overlay code (`gfx_*`, Loriciel-custom, not Borland BGI) that
  does not decompile; `sprite_chain` merges
  `object_list`+`clip`+`setup`; the composite/oracle host models a 4-plane *memory
  image* rather than the engine's VGA-hardware port writes + a000/a200 double-buffer.
  These are validation/reimplementation tools, kept clearly labeled as such.

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
- `docs/engine.md` — engine internals (game loop, two-player pipeline, physics
  state machine, anim channels).
- `docs/rendering-pipeline.md` — how the engine draws: the (Loriciel-custom, `gfx_*`)
  graphics-overlay dispatch, the a000/a200 VGA double-buffer, background/sprite/entity
  blit paths.
- `docs/reconstruction-fidelity.md` — per-module audit of where `src/` deviates from
  the original's structure (the deviations to keep labeled / bring toward 1:1).
- `docs/formats/` — file-format specs.
- `docs/tinyprog.md`, `docs/copy-protection.md`, `docs/data-files.md` — systems.
