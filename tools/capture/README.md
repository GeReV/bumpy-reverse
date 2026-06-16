# `tools/capture` — DOSBox screenshot oracle

Boots the real **Bumpy's Arcade Fantasy** under DOSBox and screenshots what it
draws, to use as **ground truth** for the pure-Python `.VEC` decoder (the in-game
vector renderer is a large, state-dependent DOS routine; rather than fully
emulate it, we let the real game render and compare).

`capture_title.py` produced a clean capture of the title screen (`TITRE.VEC`) on
the first working run.

## What it does

1. Copies the game files from `originals/old-games/bumpy/` into a writable work
   dir (`build/capture/game/`, force-writable) so `originals/` stays pristine and
   DOSBox can write `QUELDISK` etc.
2. Generates a `dosbox-staging` config (`build/capture/bumpy.conf`): mounts the
   work dir as `C:`, `[autoexec]`-runs `BUMPY.EXE`, and — crucially for a faithful
   reference — sets `[render] glshader = sharp` (pixel-perfect, **no** `crt-auto`
   CRT filter) and `aspect = false` (square 1:1 pixels, raw 320×200 grid).
3. Launches the bundled **dosbox-staging** binary on the current X display, with
   `HOME`/`XDG_CONFIG_HOME` redirected into `build/capture/home/` (the sandbox
   blocks `~/.config`).
4. Finds the DOSBox window via **python-xlib** — matching `WM_CLASS`/the
   `"cycles/ms"` title suffix (its `WM_NAME` changes to the running program, e.g.
   `"BUMPY.EXE - 3000 cycles/ms"`; do **not** match bare `"bumpy"`, which also
   hits a `BumpyDecomp` Ghidra window). It then **raises + clicks** the window to
   take keyboard focus (a focus *move* isn't enough under WSLg/Weston — XTEST keys
   go to the focused window) and injects the startup-menu keys on a timer
   (default `F3`=VGA video mode, `F7`=AdLib sound).
5. Grabs the window with ImageMagick **`import`**, **trims the black letterbox**
   (DOSBox centres the image in the window with black bars), then nearest-filter
   downscales the content to the native DOS **320×200, 16-colour**
   (`build/capture/title.png`; full-window grab kept as `title.png.raw.png`).
6. Terminates DOSBox.

## Dependencies (all already present here)

| Need | Where |
|------|-------|
| DOSBox | bundled `tools/dosbox-staging-linux-x86_64-0.82.2-5e2ba/dosbox` (no install) |
| X automation | `python-xlib` — `tools/venv-emu/bin/pip install python-xlib` |
| Screenshot | ImageMagick `import` + `convert` (system) |
| Display | an X server. Here: **WSLg `:0`** (a window briefly appears on the desktop) |

## Run

Driven by a `--timeline` of space-separated steps: **`wN`** = wait N seconds,
**`kKEY`** = press KEY (X keysym name, e.g. `F3`, `Return`, `space`), **`sNAME`**
= screenshot to `build/capture/NAME.png`.

```bash
# MUST run unsandboxed: the X11 socket is blocked by the command sandbox.
# Default timeline boots to the title:  "w7 kF3 kF7 w6 stitle"
tools/venv-emu/bin/python tools/capture/capture_title.py
```

`kF3` picks VGA video mode, `kF7` picks AdLib sound. Raise the `wN` waits if a
screen isn't up yet when the shot is taken.

### Navigation recipes (the game flow)

`title --(space)--> main menu --(Return, space)--> gameplay`:

```bash
# title, menu, and an in-game level (MONDE world + D*.PAV playfield):
tools/venv-emu/bin/python tools/capture/capture_title.py --timeline \
  "w7 kF3 kF7 w6 stitle kspace w4 smenu kReturn w1 kspace w7 sgameplay"
```

Captured reference set (under `build/capture/`, gitignored): `title.png`
(`TITRE.VEC`), `menu.png`, `gameplay.png`.

## Important caveats

- **Sandbox:** connecting to the X display fails under the command sandbox
  (`Operation not permitted` on the AF_UNIX socket). Run it outside the sandbox
  (the harness will prompt; or use `/sandbox`).
- **WSLg window:** with no `Xvfb` installed we use the live `:0`, so a DOSBox
  window pops up and briefly takes focus while keys are injected. For a fully
  headless run, install `Xvfb` and point `--display` at it (`Xvfb :99 ...`).
- **Timing:** the menu keystrokes are time-based. If the capture shows the menu
  instead of the title, raise `--boot-wait`/`--title-wait`.
- **Other screens:** to capture a level/editor, extend the injected key sequence
  (e.g. start a game, then capture) — the keystroke timeline is the only
  game-specific part.

## Faithfulness / pixel-exactness

The config already disables the `crt-auto` CRT shader (`glshader = sharp`) and
aspect correction (`aspect = false`), so the output is **square, unfiltered,
16-colour 320×200** — faithful to the framebuffer (verified: 16 colours, hard
pixel edges). It is still a nearest-neighbour downscale of an integer-scaled
window, so it should be 1:1, but if you need a **guaranteed bit-exact** reference,
trigger dosbox-staging's own raw screenshot (PNG at the emulated resolution) via
the same injector, or use the **dosbox-x** debug build to break after the title
renders and `MEMDUMP 0xA0000` the raw VRAM — the exact bytes the decoder must
reproduce.

## Output

- `build/capture/title.png` — 320×200 reference (gitignored under `build/`)
- `build/capture/title.png.raw.png` — full-window grab
- `build/capture/{bumpy.conf,game/,home/}` — generated work dir
