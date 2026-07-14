# Building the reconstruction

How to build the reconstructed engine under [`src/`](../src) into 16-bit DOS
executables with the vendored Open Watcom toolchain.

The build produces **two** targets from the same C tree — they share no object files,
so building one never disturbs the other:

| Target | EXE | What it is | Runs? |
|--------|-----|------------|-------|
| `all` | `BUMPY.EXE` | The **faithful** decompilation — a strictly structure-faithful mirror of the original, compiled and linked to prove the reconstruction is well-formed. It documents the binary; it is **link-only** and byte-compared, not executed. | no (by design) |
| `play` | `BUMPYP.EXE` | The **playable** build — the same `src/` tree plus a thin host-platform layer (`src/host/*.c`, everything gated behind `#ifdef BUMPY_PLAYABLE`) so the engine actually boots and plays under DOSBox. | yes |

This is the *Devilution* vs *Devilution-X* split: `BUMPY.EXE` documents the original,
`BUMPYP.EXE` is the runnable flavour. See
[`docs/playable-dos.md`](playable-dos.md) for **running** `BUMPYP.EXE` (DOSBox
config, supplying the copyrighted data files, and picking an audio device), and the
[README asset-extraction section](../README.md#reproducing) for the pure-Python
tools that don't need a compiler at all.

---

## Prerequisites

You need the **Open Watcom 16-bit DOS toolchain** placed under
`local/toolchain/open-watcom/`. It is user-supplied and git-ignored (like everything
in `local/`); this repo does not ship it.

- **Get it** from the [Open Watcom v2 fork](https://github.com/open-watcom/open-watcom-v2)
  (the build here was made with *Open Watcom Make 2.0 beta*). Grab a release build or
  build it yourself, then unpack it so that `local/toolchain/open-watcom/` contains the
  usual Open Watcom layout (`binl64/`, `binl/`, `h/`, `eddat/`, `wipfc/`, …).
- **Host:** a 64-bit Linux x86-64 machine — the build drives the toolchain's native
  `binl64/` binaries (`wmake`, `wcc`, `wcl`, `wlink`).
- **No environment setup.** The [`src/Makefile`](../src/Makefile) is
  *self-bootstrapping*: it points `wcc`/`wcl`/`wlink` at the vendored toolchain itself
  by setting `%WATCOM`/`%INCLUDE`/`%PATH` from a repo-relative `WROOT`. You do **not**
  need to source an `ow-env.sh` or put anything on your `PATH` first, and because
  `WROOT` is repo-relative the tree stays relocatable.

---

## Quick start

`src/build.sh` is the recommended entry point. It works from any directory (it locates
itself and `cd`s into `src/` for you) and runs the vendored `wmake` from `bash`, so it
sidesteps the interactive-zsh gotchas described under [Troubleshooting](#troubleshooting).

```sh
./src/build.sh          # default: builds the playable BUMPYP.EXE
./src/build.sh all      # builds the faithful BUMPY.EXE (+ the BVEC/BSPRITE asset tools)
./src/build.sh clean    # removes objects and the built EXEs
```

Both EXEs land in `local/build/src/` by default:

```
local/build/src/BUMPYP.EXE     the playable build
local/build/src/BUMPY.EXE      the faithful, link-only build
```

That directory is where the repo's harness scripts expect the built EXEs, which is why
it's the default — see [Output location](#output-location--the-outdir-override) to
redirect it.

---

## Build targets

| Command | Builds | Notes |
|---------|--------|-------|
| `./src/build.sh` &nbsp;/&nbsp; `./src/build.sh play` | `BUMPYP.EXE` | The playable host build. This is `build.sh`'s default when you pass no target. |
| `./src/build.sh all` | `BUMPY.EXE`, `BVEC.EXE`, `BSPRITE.EXE` | The faithful engine plus the two standalone asset-decoder sub-tools (`BVEC` = `.VEC` decode, `BSPRITE` = sprite-bank decode). `all` is the Makefile's own default target. |
| `./src/build.sh clean` | — | `rm`s `*.obj`, the `play/` objects, and the linked EXEs. |

> **`build.sh BUMPY` does not work.** In the Makefile, `BUMPY` is the *variable* holding
> the output path (`$(OUTDIR)/BUMPY.EXE`), not a target name. To build the faithful
> engine, use **`all`** (which links `BUMPY.EXE` along with the asset tools). `all` is
> also what a bare `wmake` builds, because it is the first target in the makefile —
> note that this differs from `build.sh`'s no-argument default, which is `play`.

### Why the two targets never collide

The faithful objects are compiled **without** `-dBUMPY_PLAYABLE` and live next to the
sources in `src/` (`*.obj`). The playable objects are compiled **with**
`-dBUMPY_PLAYABLE -dHOST_FB_16K` into the `src/play/` subdirectory. Because the two sets
occupy different paths, `all` and `play` can be run from a clean tree in either order
without clobbering each other, and building the playable side leaves the faithful
build's byte image untouched.

---

## Invoking the build without `build.sh`

`build.sh` is a thin wrapper around the vendored `wmake`. If you'd rather call `wmake`
directly, run it **from `src/`** (the Makefile's `WROOT` is relative to `src/`):

```sh
cd src
../local/toolchain/open-watcom/binl64/wmake play    # playable BUMPYP.EXE
../local/toolchain/open-watcom/binl64/wmake all      # faithful BUMPY.EXE + asset tools
```

If you've already put the toolchain's `binl64/` on your `PATH`, plain `wmake play` /
`wmake all` works too — the Makefile's `%WATCOM`/`%INCLUDE`/`%PATH` assignments are
harmless when the environment is already set.

### What the flags mean

The Makefile compiles with `wcc` and links with `wcl`:

| Flag | Meaning |
|------|---------|
| `-ml` | **Large** memory model (the original is medium/large, segmented) |
| `-bt=dos` | Target = 16-bit DOS |
| `-zq` | Quiet operation |
| `-wx` | Maximum warning level |
| `-dBUMPY_PLAYABLE` | *(playable only)* enables the `src/host/*` platform layer |
| `-dHOST_FB_16K` | *(playable only)* 16 KB-**per-plane** host framebuffer — the shipping playable layout, matching real VGA mode 0x0D's 4×16 KB planes (the default build uses the larger offline 64 KB/plane layout) |
| `-k0x4000` | *(playable link only)* 16 KB stack — the default 2 KB DOS stack is too small for the playable build's call depth plus the 500 Hz host `INT8` ISR frames |

---

## Output location & the `OUTDIR` override

By default the linked EXEs (and the playable `.map`) go to `local/build/src/`
(`OUTDIR = ../local/build/src`, relative to `src/`). That path is the repo standard —
the tooling under `tools/` reads the built EXEs from there — so leave it alone for a
normal build of the primary checkout.

To build in a **git worktree** without writing back into the main checkout, override
`OUTDIR`. (A worktree reaches the git-ignored toolchain and data through a `local`
symlink into the main checkout, so the *default* `OUTDIR` would send this worktree's
build output back into the main tree.) Precedence, high → low:

```sh
# from the worktree's src/:
../local/toolchain/open-watcom/binl64/wmake OUTDIR=out play      # command-line arg
BUMPY_OUTDIR=out ../local/toolchain/open-watcom/binl64/wmake play # environment variable
# → builds into <worktree>/src/out/ instead of the shared local/build/src
```

---

## Verifying the build

A successful build leaves the EXE(s) in your output directory:

```sh
ls -l local/build/src/BUMPY.EXE local/build/src/BUMPYP.EXE
md5sum local/build/src/BUMPY.EXE                # record it to compare later builds
```

Use the checksum to confirm a rebuild is reproducible on your machine, or to check that
a change you *didn't* intend to affect the faithful build indeed left it byte-stable.

> **The faithful `BUMPY.EXE` is not byte-identical to the original game binary,** and
> its checksum is **not** a fixed constant. It tracks the *reconstruction*: as more of
> the engine is reconstructed from stubs into real code, the faithful build's bytes move
> with it. So don't treat any single md5 as canonical — compute your own and compare it
> across your own builds. (Fidelity to the original is verified by matching the
> decompiled structure, not by a whole-image checksum — see
> [`docs/reconstruction-fidelity.md`](reconstruction-fidelity.md).)

---

## Troubleshooting

- **`wmake` (or `make`) silently does nothing / "function definition file not found".**
  The interactive zsh in this environment shadows `make`/`wmake` with an autoload stub
  that fails non-interactively, so the real binary never runs. Use `./src/build.sh`
  (it runs under `bash`, which has no such function) or, if calling the tool directly,
  `command wmake …`.

- **`build.sh: vendored wmake not found at …`.** The Open Watcom toolchain isn't where
  the build expects it. Make sure `local/toolchain/open-watcom/binl64/wmake` exists and
  is executable (see [Prerequisites](#prerequisites)).

- **`Error(F38): (BUMPY) does not exist and cannot be made`.** You ran `build.sh BUMPY`
  or `wmake BUMPY`. `BUMPY` is a path variable, not a target — build the faithful engine
  with **`all`** instead.

- **`zsh: file exists` when redirecting build logs.** The interactive shell sets
  `noclobber`; use `>|` to force-overwrite an existing log file (or `>>` to append).

- **Playable boot overflows the stack / falls off the end of DGROUP.** The playable
  link needs its larger stack; the Makefile already passes `-k0x4000`. If you link the
  playable objects by hand, include that flag.

---

## Next steps

- **Run it:** [`docs/playable-dos.md`](playable-dos.md) — booting `BUMPYP.EXE` under
  DOSBox, supplying the (user-supplied, copyrighted) data files, and choosing an audio
  device.
- **Extract assets without a compiler:** the [README's *Reproducing* section](../README.md#reproducing)
  covers the pure-Python `.PAV/.DEC/.BUM/.VEC/.BIN` decoders (run via `uv`).
- **Understand what you built:** [`docs/engine.md`](engine.md) (engine internals) and
  [`docs/rendering-pipeline.md`](rendering-pipeline.md) (how it draws).
