# int8-synced end-to-end capture via an instrumented DOSBox

**Status:** DONE — the capture harness is built and the int8-synced end-to-end gate
(`tools/validate_int8.sh`) PASSES: the reconstructed `game_tick()` replays the real
captured 150-frame level-1 scenario tick-for-tick, every gameplay scalar field matching,
`--perturb` caught, oracle anchor agreeing. This documents the realised design and how to
reproduce it from a clean checkout. (See "Design (capture + replay) — as realised" below.)

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
  it. This is now the standing injection path — uniform across all screens (works whether or
  not the ISR is installed) and deterministic.
- **Injection is script-driven (data, not code).** The hook reads an input script from
  `$BUMPYCAP_SCRIPT` (lines `<frame-dec> <scancode-hex> <value-dec>`), so the startup input
  sequence is tuned without rebuilding the emulator. The emulator was rebuilt **once** for
  this mechanism; every subsequent attempt is a one-line edit of the script file.
- **Input mapping decoded from the live game.** Dumping the runtime key/joystick handler
  script (`g_joystick_handler_table[0]` at DGROUP `0x4cf2`) and decoding it through
  `read_input_action` gives the real scancode→`input_state` map:
  `0x01`=UP (↑ `0x48`), `0x02`=DOWN (↓ `0x50`), `0x04`=LEFT (← `0x4b`), `0x08`=RIGHT
  (→ `0x4d`), **`0x10`=FIRE (Enter `0x1c`, Space `0x39`, or `0x74`)**.
- **Startup gate sequence solved (boot → gameplay graphics).** Two text-mode keypress gates
  in the graphics-overlay init guard the gameplay graphics mode, then the in-graphics screens take
  over:
  1. **gfx_driver_init palette select** — **F2** (`0x3c`) (or F3 `0x3d`); text mode `0x02`.
  2. **second overlay gate** — **F5** (`0x3f`); on pressing it the BIOS video mode flips to
     **`0x0D`** (Bumpy's 320×200×16 gameplay graphics). Found by a scancode sweep
     (`0x01..0x44`) watching for the mode flip; confirmed minimal (`F2` then `F5` reaches
     `0x0D` directly).
  3. in `0x0D`, injecting **FIRE (Enter `0x1c`)** drives the game on through the in-graphics
     screens (title/menu) into further new code — i.e. the real input path now carries the
     game forward frame-deterministically.

- **The in-graphics screen flow is the standard `game_loop` path** (mapped with the Ghidra
  GUI). After `0x0D` the game runs `game_loop`: `init_title_graphics` → `run_main_menu`
  (poll until FIRE selects an option) → `start_level` → `level_intro_screen` (per-level
  border image + Bumpy intro animation; its wait loop exits on a direction or **FIRE
  (`0x10`) → `intro_start_level`**) → the per-tick gameplay loop. `wait_keypress` (`1000:328f`)
  and the menu/intro waits all read `input_state` via `poll_input`, so the decoded FIRE
  scancode (Enter `0x1c`) drives them. The non-`game_loop` segments seen in the trace are the
  regular static code segments (`CODE_6` `1cec`/runtime `1510` is the **graphics-overlay EGAVGA driver**,
  un-analyzed — all rendering routes through it) plus the int8/PIT sound ISR
  (`pit_timer_isr_multiplexer` `1000:7c02`); the once-per-video-frame PC sampler sits in those
  + the input pollers, so it rarely catches the brief `game_loop`/physics frames directly.
- **Engine state advances under scripted input (SNAP core proven).** A gameplay-state
  read-out in the hook (the per-function gates' globals, read live from DGROUP:
  `game_mode 0x792c`, `current_level 0x79b2`, `p1_pixel_x/y 0x9290/0x9292`, `score
  0xa0d4/0xa0d6`) shows them **zeroed pre-gameplay** then, once the FIRE train drives past
  the menu/intro, **advancing every frame**: `p1_pixel_x` locks to the grid start column
  (`15`) while `p1_pixel_y` animates and `game_mode` cycles through movement states. So the
  reconstructed-vs-real comparison surface is readable and live at the frame boundary.

So: build ✅, runs ✅, per-frame state readable ✅, DGROUP calibrated ✅, reconstruction
cross-validated ✅, script-driven injection ✅, input map decoded ✅, boot→`0x0D` gateway
(F2,F5) reproducible ✅, `game_loop` screen flow mapped ✅, **gameplay-state globals advance
under scripted input** ✅. Remaining (the SNAP phase): pin the exact per-tick-loop entry
frame and cross-check the live globals against the per-function oracle
(`tools/physics_oracle.py`) to validate the calibration, then move the hook to the game's
logical frame boundary (after the last state mutation, before `present_frame`) and emit the
binary SNAP trace, then build the host replay harness that drives the reconstructed
`game_loop` tick-for-tick against it.

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

We need a reference that runs the game **faithfully**: real graphics-overlay rendering, the real
self-unpacking TinyProg image, and a real int8 cadence. A full-system DOS emulator gives
that; the Unicorn harness (great for per-function capture) does not model the graphics-overlay/CRTC/PIT
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

3. **Capture** — the instrumented emulator emits the trace directly when run with the
   capture env vars; no separate driver is needed (the `02-int8-snap-capture.patch`
   frame-boundary hook does the work). Use the frozen deterministic conf
   (`tools/dosbox/bumpy-capture.conf`, with the mount path filled in for your tree)
   and the level-1 scripted-gameplay input (`tools/dosbox/scripts/level1-scripted.txt`):
   ```
   HOME="$TMPDIR" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
     BUMPYCAP_SCRIPT="$PWD/tools/dosbox/scripts/level1-scripted.txt" \
     BUMPYCAP_INT8_OUT="$PWD/local/build/render/int8_trace.bin" BUMPYCAP_INT8_FRAMES=150 \
     timeout 200 local/toolchain/dosbox-x-src/src/dosbox-x \
       -conf <run-conf-with-abs-mount> -nomenu -nogui
   ```
   `BUMPYCAP_SCRIPT` **must be an absolute path** (the emulator's cwd differs from the
   shell's). The whole capture→replay→perturb→oracle pipeline is wrapped by the
   one-command gate **`tools/validate_int8.sh`** (it builds the emulator + captures the
   trace on demand if either is missing).

## Environment notes (why the deps step is manual)

The agent sandbox allows network only to a fixed host set (github/pypi/crates/…), **not** the
Ubuntu archive — so `apt install` of the SDL2 dev headers must be run by a human on the host.
Everything after the deps step (fetch pinned source from GitHub, patch, build, run) is
reproducible inside the normal toolchain.

## Design (capture + replay) — as realised

The capture + replay is implemented and the end-to-end gate **passes**: the
reconstructed `game_tick()` replays the real captured 150-frame level-1 scenario
tick-for-tick with every gameplay scalar field matching, the `--perturb` differential
caught, and the oracle calibration anchor agreeing. One-command gate:
`tools/validate_int8.sh`.

- **Boundary (CS:IP, not a call site):** the SNAP fires at the innermost per-tick loop
  TOP of `game_loop` — runtime `cs=0x0824, ip=0x0cda` (Ghidra `1000:0cda`, the
  `rng_frame = rand()` at the loop head). Triggered from the heavy-debug per-instruction
  hook (`DEBUG_HeavyIsBreakpoint`). At the loop top BEFORE `rand()` runs, `rng_frame` /
  `input_state` still hold the just-completed tick's TRAILING values, so `FRAME[k>=1]`
  carries the rng/input that DROVE the tick producing `FRAME[k].state`. (The spec named
  the `rotate_timing_flags_and_wait` site; the loop top gives a clean symmetric
  seed/compare — see Task 7 notes.) Arms only inside a real level
  (`current_level >= 1 && game_mode != 0`).
- **Calibration:** runtime DGROUP seg `0x185f` (= Ghidra `0x203b − 0x7DC`); offsets are
  identical to the unpacked image. Confirmed live (`get_key_state` @ `0x4d42`) and
  cross-checked by `tools/int8_oracle_check.py` (the `update_p1_bbox` pixel→AABB anchor).
- **Determinism:** `core=normal`, `cycles=fixed 6000`, frozen conf
  (`tools/dosbox/bumpy-capture.conf`), scripted input
  (`tools/dosbox/scripts/level1-scripted.txt`).
- **Input injection:** the `01` hook writes the next script entry into the engine's raw
  key-state table (`g_key_state_table` @ `0x4d42`); the real `poll_input` /
  `read_input_action` pipeline then processes it — frame-aligned, deterministic, real
  input path.
- **Trace layout (`tools/int8_trace.h`, the authoritative side; the `02` patch mirrors
  it):** `header(14) + INIT + (N+1)×FRAME(61)`. `INIT` (v3, 19369 B) =
  `tilemap[0x300]` + 7×12-byte anim records + `entity_state[0x200]` (now the P1/P2
  sprite-object descriptor pointee blocks — the `+0x14/+0x16` origin words the grid
  recompute reads) + a `move_data[0x4600]` low-DGROUP static-data window (the
  move-script `mode_script_tbl`/headers/`[anim,dx,dy]` arrays AND the cell-animation
  tiledef/frame/grid/pos tables + their tile-def/frame/stream blobs) + an 85-byte scalar
  union. `FRAME` = trailing `rng`/`input` + a tilemap FNV hash + the 55-byte
  gameplay-state assert-set. `INT8_VERSION` guards stale traces (hard-fail on mismatch);
  it is **3** (the read-set was extended twice: v2 added the sprite descriptors +
  move-script window + `p1_move_script` ptr and fixed the `move_step_count` offset bug,
  0x855e→0x824c; v3 widened the window to the whole low-DGROUP move+anim static region).
- **Frame SNAP scalars:** the union of the per-function gates' compared fields (p1/p2
  pixel x/y, grid cells + history, bbox, `game_mode`/move-step machine, score,
  `items_remaining`, exit/level/anim state, `move_override`, the round/session flags,
  …). Pure render/hardware state (VGA pages, DAC, the sound ISR's private tone frame) is
  **not** in the SNAP — the documented render/sound carve-out partition.
- **Replay harness (host side, `tools/int8_ctest.c`):** seed the live reconstructed
  engine globals ONCE from `INIT` (`seed_from_init` — scalars, the tilemap window, the
  7 anim records, the sprite descriptors, and the move+anim DGROUP window placed into
  host far-memory at the captured runtime DGROUP linear base so every far-pointer hop
  resolves 1:1), then per FRAME feed the trailing rng/input, call the REAL `game_tick()`,
  and compare the evolved scalars against `FRAME[k].state`. First divergence prints
  `(frame, field, got/want)`. `--perturb` corrupts one seeded field so a tick must
  diverge.
- **One precise exclusion (NOT a tolerance):** the per-frame `tilemap_hash` is excluded
  from the compare. The replay reproduces every gameplay-collision tilemap write 1:1
  (item-collection, contact-action, cell-animation); the only residual full-tilemap
  divergence is the **animated-tile FX-graphics layer** (cell+0xa0, e.g. cell `0xc8` =
  anim-slot cell `0x28` + `0xa0` cycling its displayed tile-graphic +6/tick), written
  INSIDE the carved-out graphics-overlay render core (`render_player_view` → `gfx_set_mode_10` → the
  un-analyzed EGAVGA overlay handler `1ab9:0db0`). No gameplay-collision callee reads
  that layer; it is render-only, so it is legitimately excluded from the state-spine
  SNAP. The collision-layer tilemap is validated per-cell by the items/anim/spawn
  per-function gates. (See `tools/int8_ctest.c` `run_replay` + `docs/reconstruction-fidelity.md`.)
- **Trust anchor:** `tools/int8_oracle_check.py` boots the real `BUMPY.EXE` under Unicorn
  to level 1 and cross-checks the DOSBox capture's INIT + first frames against the
  `update_p1_bbox` per-function oracle (the closed-form pixel→AABB map) — tying the new
  reference to established per-function ground truth.
