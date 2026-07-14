# Bumpy's Arcade Fantasy — reverse engineering

This is a Claude-led reverse-engineering effort of **[Bumpy's Arcade Fantasy](https://www.mobygames.com/game/3832/bumpys-arcade-fantasy/)** (1992, Loriciel) — the DOS English
release. The primary deliverable is a **faithful decompilation** under `src/`: a
structure-faithful C mirror of the original binary (one function per original
function, same control flow, same data layout — the *Devilution* model, not a clean
rewrite), grounded throughout in a Ghidra decompilation of the engine. Alongside it
are pure-Python decoders/renderers for the game's asset formats.

The original game files are copyright and **not distributed here**; you supply your own.

## Layout

| Path | What |
|------|------|
| `src/` | the reconstructed C source — the decompilation itself. Builds two targets: the faithful `BUMPY.EXE` (link-only, byte-compared, never run) and the playable `BUMPYP.EXE` (adds a thin host platform layer so the engine actually runs — see [`docs/playable-dos.md`](docs/playable-dos.md)) |
| `tools/extract/` | pure-Python decoders/renderers for the game's data (`.PAV/.DEC/.BUM`, `.VEC`, `.BIN`, `.CAR`, `.BNK`) → level PNGs, sprite sheets, world maps, JSON level tables. Pure stdlib, no third-party deps. |
| `tools/tinyprog_unpack.py` | recovers the unpacked load module from the TinyProg-packed `BUMPY.EXE` (see [`docs/tinyprog.md`](docs/tinyprog.md)) |
| `tools/disasm16.py` | a small capstone-based 16-bit disassembler, for inspecting the binary or a built `.EXE` directly |
| `docs/`          | reference docs: engine internals, rendering pipeline, the reconstruction-fidelity audit, executable packing, copy protection, data files & resource pipeline, and the file-format specs under `docs/formats/` ([index](docs/README.md)) |
| `local/`         | **git-ignored** working tree: your game files, the toolchain (Ghidra/JDK/DOSBox/Open Watcom), and build intermediates |
| `results/`       | **git-ignored** generated outputs (level PNGs, sprite sheets, world maps, level JSON) — regenerable from the game files via the tools |

## Reproducing

### Asset extraction (Python, no game binary needed beyond the data files)

1. **Dependencies** (managed with [uv](https://docs.astral.sh/uv/)):
   ```sh
   uv sync
   ```
   The extractors/renderers are pure stdlib and need no third-party packages.
2. **Supply the game files** into `local/build/capture/game/` (the `D1..D9.{PAV,DEC,BUM}`,
   `MONDE*.VEC`, `BUMSPJEU.BIN`, etc. from your copy of the game).
3. **Run the tools**, e.g.:
   ```sh
   uv run python tools/extract/render_levels.py     # all puzzles -> results/levels_png/
   uv run python tools/extract/render_vec_images.py # title/score/world maps
   ```

### Building the reconstruction (Open Watcom, 16-bit DOS)

Requires the Open Watcom 16-bit DOS toolchain vendored under
`local/toolchain/open-watcom/` (user-supplied, git-ignored):

```sh
src/build.sh          # builds the playable BUMPYP.EXE
src/build.sh BUMPY     # builds the faithful, non-running BUMPY.EXE
```

See [`docs/playable-dos.md`](docs/playable-dos.md) for running `BUMPYP.EXE` under
DOSBox, and [`docs/engine.md`](docs/engine.md) for the engine internals the
reconstruction documents.

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
