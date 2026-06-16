# Bumpy's Arcade Fantasy — reverse engineering

Reverse-engineering of **Bumpy's Arcade Fantasy** (1992, Loriciel) — the DOS English
release — into documentation and **pure-Python asset extractors/renderers**, plus a
Ghidra-based decompilation of the engine.

The original game files are copyright and **not distributed here**; you supply your own.

## Layout

| Path | What |
|------|------|
| `tools/extract/` | pure-Python decoders/renderers for the game's data (`.PAV/.DEC/.BUM`, `.VEC`, `.BIN`, `.CAR`, `.BNK`) → level PNGs, sprite sheets, world maps, JSON level tables |
| `tools/render/`  | the Unicorn-based DOS emulator (`dosemu.py`) + the `vec_cpu` oracle used to validate the pure-Python ports |
| `tools/ghidra_scripts/`, `tools/bin/ghidra-headless` | Ghidra analysis scripts + headless wrapper |
| `tools/*.py`     | TinyProg unpacker (`tinyprog_unpack.py`) and small RE helpers |
| `docs/`          | the write-up: inventory, unpacking, copy-protection, engine, and `docs/formats/*.md` (file-format specs) |
| `local/`         | **git-ignored** working tree: your game files, the toolchain (Ghidra/JDK/DOSBox), build intermediates, and generated `results/` |

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
   uv run python tools/extract/render_levels.py     # all puzzles -> local/results/levels_png/
   uv run python tools/extract/render_vec_images.py # title/score/world maps
   ```

See `docs/` for the format specs and how each piece was reversed.
