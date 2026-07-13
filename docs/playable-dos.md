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
> the thing for you to try.

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

### Audio devices

The engine lets you pick the sound device at boot — **None** (F5), **PC-speaker** (F6),
**AdLib/OPL2** (F7), or **MT-32** (F8). Two of those target a *specific* Roland/Yamaha
part, so faithful playback needs the emulator pointed at the right one (see
[engine.md](engine.md#target-playback-hardware) for why). Add these sections to
the config above:

```ini
[sblaster]
oplmode = opl2
oplemu  = nuked
[midi]
mpu401      = intelligent
mididevice  = mt32
mt32.model  = cm32l
mt32.romdir = /path/to/dir/holding/CM32L_CONTROL.ROM+CM32L_PCM.ROM
```

- **AdLib (F7):** the game drives a single OPL2. DOSBox-X's default OPL core (`DBOPL`) is
  a fast approximation that can add a volume transient at each note's onset; the
  cycle-accurate `oplemu = nuked` core (with `oplmode = opl2`, the game's real hardware)
  removes it. The reconstruction's OPL register writes are byte-faithful either way — the
  difference is purely the emulated chip.
- **MT-32 (F8):** the game is a **CM-32L** title — it uses rhythm-part sound-effect keys
  (e.g. the overworld→level "enter" sound is note 83) that a first-generation MT-32 ROM
  leaves silent. Use CM-32L ROMs (`CM32L_CONTROL.ROM` / `CM32L_PCM.ROM`) and
  `mt32.model = cm32l`; the CM-32L is backward-compatible with the melodic music. The
  ROMs are copyright and **user-supplied**. DOSBox-X's built-in `munt` needs no external
  synth.

Everything else about the audio is device-agnostic and already correct in the build.

**What you should see:** like the original, the playable build boots into the
interactive graphics-select screen (`gfx_driver_init`, `src/main.c`) — press **F2** for
EGA (`palette_mode = 1`) or **F3** for VGA (`palette_mode = 2`, the default and most-
validated path) — before it proceeds toward the menu. The seeded input-handler maps:

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

## Validation

The playable build's fidelity was proven during development by an internal harness (a
per-tick pixel frame-compare gate running both `BUMPYP.EXE` and the real original under an
instrumented DOSBox-X, plus a frame-accurate int8-synced end-to-end replay of the
reconstructed game loop against a captured golden trace). That harness — and the dosbox-x
build/patch/capture tooling it depended on — is development scaffolding and isn't part of
this repo; see [`docs/reconstruction-fidelity.md`](reconstruction-fidelity.md) for the
methodology and results it established (the "int8-synced end-to-end gate" section and the
per-module fidelity audit). Reconstruction correctness does not depend on any tooling in
the public tree — it's a property of `src/` matching the decompiled original.
