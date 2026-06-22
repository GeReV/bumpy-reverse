# int8-synced end-to-end capture via an instrumented DOSBox

**Status:** in progress (capture harness bring-up). This documents what is being set
up, why, and how to reproduce it from a clean checkout.

**Bring-up findings (spike, 2026-06-22/23):** the reproducible build produces a working
integrated-DOS dosbox-x; BUMPY.EXE boots and **runs** under it.

- **Calibration solved.** Disassembling the loop the game parks in showed it is our
  `get_key_state` reading `g_key_state_table` at `DGROUP:0x4d42` — so the runtime **DGROUP
  segment is `0x185f`**, runtime code seg `0824` (a second seg `12dd` = the `1ab9` overlay),
  and **runtime offsets are identical to the unpacked image** (`runtime_seg = ghidra_seg −
  0x7DC`). We can now read any SNAP field at `0x185f:<offset>`.
- **Reconstruction cross-validated against the live binary**: `get_key_state` (7ab4),
  `g_key_state_table` (`0x4d42`), and `palette_mode` (`0x541d`) all match `src/` exactly.
- **The startup is a sequence of input-gated screens.** The first is `gfx_driver_init`
  (`1ab9:0316`): a loop polling **F2 (scancode 0x3c → `palette_mode`=1)** / **F3 (0x3d →
  `palette_mode`=2)** — a palette/monitor select. It loops in text mode until one is pressed.
- **Injection mechanism = direct key-state-table write.** The game's INT9 ISR is **not
  installed** during these early prompts (INT9 vector = BIOS `f000:e987`), so
  `KEYBOARD_AddKey` (scancode→IRQ1→INT9) does **not** populate the game's table. Writing the
  key directly into the table the game polls (`DGROUP:[0x4d42]+scancode = 1`) **does** advance
  it — verified: an F2 table-write drove the game past `gfx_driver_init` into new code.

So: build ✅, runs ✅, per-frame state readable ✅, DGROUP calibrated ✅, reconstruction
cross-validated ✅, **input injection proven** ✅. Remaining: script the full startup key
sequence to reach the gameplay graphics loop (`mode 0x0D`), then move the hook to the game's
frame boundary and emit the SNAP trace, then the host replay harness.

## Goal

Produce a **frame-accurate golden reference** of the real `BUMPY.EXE` running, so the
reconstructed `src/` game loop can be validated **end-to-end, tick-for-tick** — not just
per-function. This is the project's last standing validation gap (see
`docs/reconstruction-fidelity.md`: the *int8-synced end-to-end gate*). Per-function gates
prove each function is faithful in isolation; this gate proves the functions **compose**
into a faithful running game (correct call order, inter-function state flow, loop timing).

"int8" = the PC timer interrupt (IRQ0 / PIT tick) that drives the game's frame cadence.
`game_loop` waits for it each iteration (`rotate_timing_flags_and_wait`), and the sound
sequencer runs inside its ISR (`pit_timer_isr_multiplexer` 7c02). "int8-synced" means we
sample engine state at the **real per-frame boundary**, not at an arbitrary instruction
count — which is exactly why the Phase-1 Unicorn trace was unusable (its sampling
granularity did not line up with where the engine advances physics).

## Why DOSBox (and why a *patched* build)

We need a reference that runs the game **faithfully**: real BGI overlay rendering, the real
self-unpacking TinyProg image, and a real int8 cadence. A full-system DOS emulator gives
that; the Unicorn harness (great for per-function capture) does not model the BGI/CRTC/PIT
environment.

To get state *out* at the frame boundary we instrument the emulator with a small hook
(read the game's DGROUP state struct from guest RAM once per frame, inject the scripted
input, append a binary record). Building from source + a patch is the cleanest, fastest
capture mechanism. (Alternatives considered: an in-guest DOS TSR logger — no emulator
changes but fiddly; the dosbox-x debugger — no Linux prebuilt exists. We chose the patched
build.)

### Emulator choice: dosbox-x

We build **dosbox-x** from source for two reasons that directly serve the goal:

- **Compatibility / faithfulness.** dosbox-x is the compatibility-maximizing DOS emulator
  (it aims to run even quirky old software accurately, with fine-grained hardware control).
  The whole point here is a *faithful* golden reference, so that matters more than build
  convenience.
- **The built-in debugger** (`--enable-debug=heavy`). Even though the production capture is
  driven by our patch, *bring-up* is debugger-heavy: locating the exact frame-boundary
  address, calibrating the load segment after the TinyProg self-unpack, watching DGROUP
  live, and confirming the hook fires in the right place. dosbox-x gives us the debugger
  **and** patchability in one emulator.

(dosbox-staging was considered — lighter meson build, and we have a matching `0.82.2`
binary — but its debugger is a compile-time extra and its compatibility envelope is
narrower. Since deps are being installed anyway, the build-convenience edge doesn't
outweigh dosbox-x's compatibility + debugger for this task.)

dosbox-x ships **no Linux prebuilt** (GitHub releases are Windows/ARM only), so a from-source
build is required regardless. We DISABLE the optional heavy features (ffmpeg video-capture,
FluidSynth, freetype/TTF, slirp/sdlnet networking, GL output) to keep the dependency
footprint small.

Pinned version: **`dosbox-x-v2026.06.02`**.

> **Pitfall — pin the regular tag, not `-osfree`.** Upstream ships paired tags per release:
> `dosbox-x-vYYYY.MM.DD` **and** `dosbox-x-vYYYY.MM.DD-osfree`. GitHub's "latest release" is
> the `-osfree` one, which ships `vs/config_package.h` with `#define OSFREE` — `configure`
> turns that into an **OS-Free build with no built-in DOS**, so `mount c …; BUMPY.EXE` just
> drops to a stub COMMAND.COM and exits (the game never runs). The build script pins the
> **regular** tag, which has the integrated DOS kernel.

## Reproducibility model — what is tracked vs. built

All of `/local/` is git-ignored (toolchain, game data, build outputs). So the rule is:
**track the recipe, never the fork.**

| Tracked (in the repo) | Untracked (`local/`, you build/supply) |
|---|---|
| `tools/dosbox/build-dosbox-x.sh` — fetch pinned source + apply patch + build | the dosbox-x **source checkout** + the built binary |
| `tools/dosbox/patches/*.patch` — our instrumentation patch(es) | the captured **golden trace** |
| `tools/dosbox/*.conf` — the frozen, deterministic DOSBox config | the game files (`BUMPY.EXE`, user-supplied) |
| `tools/dosbox/*.py` — the capture driver + input-script format | |
| this doc | |

There is **no DOSBox fork vendored** in the repo — only a patch file + a build script that
fetches the pinned upstream source. On a clean checkout you run the build script and it
reproduces the instrumented emulator.

## How to reproduce from a clean checkout

1. **Install build deps** (Ubuntu 24.04; one-time, needs sudo). Minimal set — the optional
   heavy features are disabled by the build script:
   ```
   sudo apt update && sudo apt install -y build-essential automake autoconf libtool \
     pkg-config nasm libncurses-dev libsdl2-dev libpng-dev zlib1g-dev \
     libxkbfile-dev libxrandr-dev
   ```
   (The runtime SDL2 lib is usually already present; the `-dev` headers + `nasm` + the
   autotools are what a build needs and what the sandbox cannot `apt`-install itself.
   `libncurses-dev` is required for the `--enable-debug=heavy` debugger TUI.)

2. **Build the instrumented emulator** (fetches the pinned `dosbox-x-v2026.06.02`
   source into the git-ignored `local/toolchain/`, applies `tools/dosbox/patches/`, builds
   `src/dosbox-x` with the debugger):
   ```
   tools/dosbox/build-dosbox-x.sh
   ```

3. **Capture** (TODO — driver + config land with the patch):
   ```
   tools/dosbox/capture.py --script <input-script> --frames N --out local/build/render/int8_trace.bin
   ```

## Environment notes (why the deps step is manual)

The agent sandbox allows network only to a fixed host set (github/pypi/crates/…), **not** the
Ubuntu archive — so `apt install` of the SDL2 dev headers must be run by a human on the host.
Everything after the deps step (fetch pinned source from GitHub, patch, build, run) is
reproducible inside the normal toolchain.

## Design (capture + replay) — for reference

- **Boundary:** capture once per frame, right after the last state mutation and before
  `present_frame` (so each SNAP is a fully-advanced tick; render/timing — the stubbed parts —
  are excluded by construction).
- **Calibration:** read the load segment after the TinyProg self-unpack (PSP/MCB), map
  DGROUP (static-image seg 0x203b) + CODE (seg 0x1000) to live linear addresses.
- **Determinism:** `core=normal`, `cycles=fixed`, frozen config, scripted input.
- **Input injection:** write the next script entry into the engine's raw key-state table
  (`g_key_state_table` @ 0x4d42) from the frame hook, then let the real `poll_input` /
  `read_input_action` pipeline process it — frame-aligned and deterministic, while still
  exercising the real input path (the keyboard layer itself is already per-function gated).
- **Frame SNAP:** the union of the gameplay globals the per-function gates already track
  (p1/p2 pixel x/y, cells, `game_mode`, the move-step counters, score, `items_remaining`,
  anim-channel records, `current_level`, rng/prng, `input_state`, the round/session flags).
  Pure render/hardware state (VGA pages, DAC, the sound ISR's private tone frame) is **not**
  in the SNAP — sidestepping the async-int8 problem the same way the L5 ISR is a documented
  carve-out elsewhere.
- **Replay harness (host side):** seed the initial frame, run the reconstructed `game_loop`
  per-tick body once per captured frame feeding the same input script, assert reconstructed
  state == captured state each frame. First divergence pinpoints a composition bug.
- **Trust:** cross-check the DOSBox capture against the per-function oracles at a few
  boundaries (e.g. the gameplay state at the first physics-fn call should match what
  `physics_oracle.py` already captured) so the new reference is tied to established ground
  truth.
