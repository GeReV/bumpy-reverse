# Bumpy's Arcade Fantasy — reverse engineering

This is a Claude-led reverse-engineering effort of **[Bumpy's Arcade Fantasy](https://www.mobygames.com/game/3832/bumpys-arcade-fantasy/)** (1992, Loriciel) — the DOS English
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

## Notes and Thoughts

This was initially supposed to be a weekend challenge for Claude Fable 5, before it was banned. I decided to nevertheless
continue this project using Opus 4.8.

Bumpy was chosen after trying to find a classic game candidate that was both influential enough to be widely 
known and have some emotional resonance with me, and have no prior reversing or reimplementation work done.

The effort itself took about 4 work days of Claude driving its own efforts with myself generally steering it.

Claude has overall performed remarkably. While it required a bit of pushing from my side to keep it digging at the
problems that it ran into, and occasionally throwing an idea to make it try a different approach or method.  
Despite this, it eventually managed to crack every single problem it ran into, performing work that would have 
taken me (a very inexperienced reverse-engineer) weeks or months, if I succeeded at all.

I _would_ say I am impressed.
