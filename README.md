# Bumpy's Arcade Fantasy — reverse engineering

Reverse-engineering of **Bumpy's Arcade Fantasy** (1992, Loriciel) — the DOS English
release — into documentation and **pure-Python asset extractors/renderers**, plus a
Ghidra-based decompilation of the engine.

The original game files are copyright and **not distributed here**; you supply your own.

## Layout

| Path | What |
|------|------|
| `tools/extract/` | pure-Python decoders/renderers for the game's data (`.PAV/.DEC/.BUM`, `.VEC`, `.BIN`, `.CAR`, `.BNK`) → level PNGs, sprite sheets, world maps, JSON level tables. Self-contained: the only third-party deps are the emulators below. |
| `tools/emu/`     | the from-scratch 16-bit x86 interpreter (`vec_cpu.py`) + pure-Python DOS host (`pydos.py`) and the Unicorn-based DOS emulators (`dosemu.py`, `vec_emu.py`, `vec_interp.py`) used while reversing the renderer |
| `tools/ghidra_scripts/`, `tools/bin/ghidra-headless` | Ghidra analysis scripts + headless wrapper |
| `tools/*.py`     | TinyProg unpacker (`tinyprog_unpack.py`) and the relocation/diff helpers (`inspect_relocs.py`, `compare_unpacked.py`) behind the unpacking write-up |
| `docs/`          | reference docs: executable packing (`tinyprog.md`), copy protection, data files & resource pipeline, engine (draft), and the file-format specs under `docs/formats/` ([index](docs/README.md)) |
| `local/`         | **git-ignored** working tree: your game files, the toolchain (Ghidra/JDK/DOSBox), and build intermediates |
| `results/`       | **git-ignored** generated outputs (level PNGs, sprite sheets, world maps, level JSON) — regenerable from the game files via the tools |

## Reproducing

1. **Dependencies** (managed with [uv](https://docs.astral.sh/uv/)):
   ```sh
   uv sync          # creates .venv with unicorn + capstone
   ```
   The asset extractors/renderers are pure stdlib; only the emulator/disassembly tools
   need the packages above.
2. **Supply the game files** into `local/build/capture/game/` (the `D1..D9.{PAV,DEC,BUM}`,
   `MONDE*.VEC`, `BUMSPJEU.BIN`, etc. from your copy of the game).
3. **Run the tools**, e.g.:
   ```sh
   uv run python tools/extract/render_levels.py     # all puzzles -> results/levels_png/
   uv run python tools/extract/render_vec_images.py # title/score/world maps
   ```

See [`docs/`](docs/README.md) for the file-format references and the game's
data/resource and copy-protection systems.
