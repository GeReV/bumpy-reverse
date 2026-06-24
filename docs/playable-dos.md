# Playing the reconstruction on DOS — `BUMPYP.EXE`

This documents the **playable build** of the reconstructed engine: `BUMPYP.EXE`, a
pure-DOS executable that links a thin host-platform layer (`src/host/*.c`) on top of the
faithful `src/` decomposition so the engine actually *runs and plays* under DOSBox.

This is the *Devilution-X*-flavoured runnable side of the project. The default
`BUMPY.EXE` build remains a **strictly faithful, link-only** decompilation (it documents
the original byte-for-byte and is byte-identical regardless of this work). Everything the
playable build adds is gated behind `#ifdef BUMPY_PLAYABLE`; every place it diverges from
the original's mechanism carries an in-code `RECONSTRUCTION FIDELITY` note and an entry in
[`docs/reconstruction-fidelity.md`](reconstruction-fidelity.md) (see *“Playable host
platform (Plan A)”*).

> **Status (current).** `BUMPYP.EXE` boots pure-DOS → BIOS video mode 0x0D → level 1 →
> the main menu → and into the **per-tick `game_tick()` gameplay loop** (`game_mode`
> reaches 4 and holds), verified under scripted boot input. Interactive keyboard play is
> the thing for you to try. The automated pixel frame-compare gate
> (`tools/validate_playable.sh`) is scaffolded and validated on the original-game side;
> its playable-side capture trigger still needs calibration (see *Known limitations*).

---

## What the playable build adds (the host layer)

The faithful `src/` tree leaves the hardware/timing/IO leaves as carve-out stubs
(`game_stubs.c`) — it links but never runs. The playable build swaps those for real
implementations under `src/host/`:

| File | Provides |
|------|----------|
| `host_video.c` | VGA mode 0x0D init, DAC palette upload, the A000/A200 double-buffer, `present_frame` (RAM-image → VGA planes + CRTC page-flip), `clear_viewport` |
| `host_render.c` | the render-leaf binding — routes the validated planar blitters (`sprite_blit_planar_vga`, `bg_render_grid`) into a flat 4-plane RAM framebuffer |
| `host_timer.c` | the INT8/PIT frame-pacing ISR (`rotate_timing_flags_and_wait`, `run_n_frames`) |
| `host_input.c` | the real INT9 keyboard ISR (fills `g_key_state_table`), `wait_keypress`, the action poll |
| `host_view.c` | view/setup leaves, the flat-RAM save-under, and the anim-channel slot-table wiring |
| `host_boot.c` | boot-init leaves (joystick/sound tables, level (re)load) and the seeded input-handler script |

The central documented divergence: the original writes **directly** to the VGA A000
(page 0) / A200 (page 1) planes via the Sequencer/Graphics-Controller during the blit.
The reconstructed blitters were validated against a **flat, contiguous** 4-plane memory
image, which is incompatible with VGA's interleaved planes — so the playable build
composes each frame into a flat RAM framebuffer and `present_frame` copies it to the
off-screen VGA page (per-plane via the Sequencer Map Mask) then page-flips via the CRTC
Start Address. Faithful in *result* (the displayed pixels), divergent in *mechanism*.
Full rationale: [`docs/reconstruction-fidelity.md`](reconstruction-fidelity.md).

---

## Building `BUMPYP.EXE`

Requires the Open Watcom 16-bit DOS toolchain (under `local/toolchain/open-watcom/`).

```sh
cd src
wmake play          # compiles every TU -dBUMPY_PLAYABLE into play/, links -k0x4000
                    # -> src/BUMPYP.EXE  (also copied to local/build/src/BUMPYP.EXE)
```

`wmake all` (the default target) builds the faithful, non-running `BUMPY.EXE`; the two
targets do not share objects, so the default build stays byte-identical
(`md5 cac9ff236a832284fec6fafff2d8602b`).

> zsh note: the interactive shell shadows `make`/`wmake` with an autoload stub — use
> `command wmake play` (or run from bash) if `wmake` no-ops.

---

## Running / playing it under DOSBox

`BUMPYP.EXE` needs the original game's data files (`*.PAV/.DEC/.BUM/.VEC/.BNK`,
`CODES.EXE`, …) in its working directory. They are copyright and **user-supplied** — put
them, alongside `BUMPYP.EXE`, in a directory you mount in DOSBox.

A minimal, deterministic DOSBox config (DOSBox-X recommended):

```ini
[cpu]
core    = normal
cputype = 386
cycles  = fixed 6000
[dosbox]
machine = vgaonly
[autoexec]
mount c /path/to/your/game-dir
c:
BUMPYP.EXE
exit
```

> **Important:** do **not** put inline `# ...` comments on the value lines — DOSBox-X
> does not strip them, so `machine = vgaonly  # foo` is parsed verbatim and corrupts the
> machine type (which stalls the playable boot at a BIOS wait). Keep values clean.

**What you should see:** unlike the original, the playable build hardcodes
`palette_mode = 2` and **skips the interactive F2/F5 palette-select screen** (a documented
deviation, `src/main.c`). It boots straight toward the menu. The seeded input-handler maps:

| Action | Key (scancode) |
|--------|----------------|
| Up / Down / Left / Right | arrow keys (`0x48` / `0x50` / `0x4b` / `0x4d`) |
| Fire / confirm | **Enter** (`0x1c`) |

So: at the menu, **Enter** selects *Start*; at the level intro, **Enter** enters the
round; then the arrow keys drive Bumpy.

The host INT9 ISR + input path are reconstructed and unit-validated
(`tools/validate_input.sh`, 100/100), and the engine reaches active gameplay under
scripted input. Live interactive keyboard play under a windowed DOSBox is exactly what
this build is for you to exercise.

---

## The headless capture + frame-compare gate (for validation)

`tools/validate_playable.sh` is the Plan-A integration gate: it runs **both** `BUMPYP.EXE`
and the **real original** `BUMPY.EXE` under one instrumented DOSBox-X build, captures the
rendered VGA planes per per-tick frame, and compares them plane-for-plane (the idea: same
level → identical pixels, proving the host render/present/flip glue is faithful).

```sh
# one-time: build the instrumented emulator (pinned source + tracked patches, no fork)
bash tools/dosbox/build-dosbox-x.sh
# the gate (needs the real original game under local/build/capture/game/)
tools/validate_playable.sh
```

The instrumented emulator is built from pinned upstream DOSBox-X plus the tracked patches
under `tools/dosbox/patches/` (`01` bring-up/input hook, `02` int8 SNAP, `03` per-frame
A000 plane dump) — **no DOSBox fork is vendored**; the build script re-applies the patches
onto the pinned tag. See `tools/dosbox/patches/README.md`.

### Calibration notes
- **Runtime DGROUP shifts with code size.** This build’s runtime DGROUP segment is
  `0x3fe4`; it moves whenever `BUMPYP.EXE`’s code size changes. The **DGROUP-internal
  offsets are stable** (`keytbl 0x9dbe`, `current_level 0x05c2`, `game_mode 0x9eca`,
  `input_state 0x9e83`). After a rebuild, re-derive `BUMPYCAP_DGROUP` from the live `DS`
  in the run log if the gate’s map-derived value warns of a mismatch.
- **dosbox hook rebuilds:** after editing a patch, force-remove the stale objects/archive
  (`hardware/vga_draw.o`, `hardware/libhardware.a`, `src/dosbox-x`) before `make`, and
  verify with `strings src/dosbox-x | grep BUMPYCAP` — a plain `make` can silently keep a
  stale hook.

### Known limitations (gate not yet green)
- The gate’s **playable-side per-tick capture trigger** (the CS:IP at which it samples the
  framebuffer each tick) is not yet correctly calibrated: the playable engine’s gameplay
  code runs in a relocated code segment (observed `mode=4` code at runtime segment `ba51`,
  not the boot/menu segment `0824`), and the per-tick loop-top IP is not the linker-map
  offset. Locating it needs an instruction-level CS:IP probe of the running playable
  `game_loop`. Until then the playable capture produces 0 frames and the gate reports FAIL
  at the “playable capture produced no frames” step. The **original-side capture works**
  (32 frames, decoding to a real level render), and two real harness bugs were fixed along
  the way (the inline-comment conf corruption above; dosbox ignoring `timeout`’s SIGTERM →
  `timeout -k`).
- **Reconstruction correctness does not depend on this gate.** The per-tick game state is
  already validated tick-for-tick by the int8 end-to-end gate (`tools/validate_int8.sh`),
  and the planar blitters/compose are byte-exact (`tools/validate_composite.sh`,
  `validate_host_compose.sh`). The pixel frame-compare gate is *additional* assurance over
  the host present/flip glue, not the project’s primary validation.
