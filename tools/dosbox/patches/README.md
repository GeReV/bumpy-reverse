# dosbox-x instrumentation patches

Tracked recipe (the patch), applied by `tools/dosbox/build-dosbox-x.sh` onto the pinned
source. No DOSBox fork is vendored — only these patches.

- **`01-bumpycap-hook.patch`** — the BUMPYCAP capture hook in `VGA_VerticalTimer`
  (once per video frame). It does three things, all via `phys_readb`/`phys_writeb`
  (`real_readb`/`mem_readb` are forbidden in that VGA-render TU):
  1. **Observability** — `LOG_MSG`s the BIOS video mode (BDA `0040:0049`; `0x02`=text,
     `0x0d`=Bumpy's 320×200×16 gameplay graphics) plus the foreground `CS:IP`, throttled
     to every 35th frame and on mode change. Confirms how far BUMPY.EXE got and where it
     runs (load-segment calibration).
  2. **Handler dump** — dumps the live key/joystick handler script
     (`g_joystick_handler_table[0]` at DGROUP `0x4cf2`) whenever its content changes, so
     the scancode→`input_state` mapping (`read_input_action`) can be decoded from the
     running game rather than guessed.
  3. **Script-driven input injection** — reads an input script from the file named by
     `$BUMPYCAP_SCRIPT` (lines `<frame-dec> <scancode-hex> <value-dec>`; `#` comments and
     blanks skipped) and applies each entry by writing the game's polled key-state table
     at DGROUP `[0x4d42]+scancode` (1=down, 0=up). Direct table writes are uniform across
     startup screens — they work whether or not the game's INT9 ISR is installed, and the
     deterministic config means a given script reproduces the same run every time.

  The runtime DGROUP segment is hard-coded `0x185f` (calibration in
  `docs/dosbox-int8-capture.md`). This patch is the bring-up + input-drive instrumentation;
  the frame-boundary SNAP capture (binary trace at the logical present boundary) is the
  follow-on `02` patch that builds on the same calibration.

- **`02-int8-snap-capture.patch`** — the int8-synced frame-boundary SNAP emitter, applied
  ON TOP of `01`. Adds a CS:IP-triggered capture to the heavy-debug per-instruction hook
  (`DEBUG_HeavyIsBreakpoint` in `src/debug/debug.cpp`, called once per instruction under
  the `--enable-debug=heavy` build). The trigger is the innermost per-tick loop TOP of the
  original `game_loop` (`FUN_1000_0c18`) — the `rng_frame = rand();` site at ghidra
  `1000:0cda`, runtime **`cs=0x0824, ip=0x0cda`** (runtime = ghidra − 0x7DC for the seg;
  offsets identical to the unpacked image). Captured at loop-top (before `rand()` runs) so
  `rng_frame` (DGROUP `0x79b3`) / `input_state` (`0x8244`) still hold the just-completed
  tick's TRAILING values.

  On each armed hit it emits a binary trace whose layout MIRRORS `tools/int8_trace.h`
  byte-for-byte (header magic `"BINT"`, `version = INT8_VERSION = 3`, `dgroup_seg 0x185f`,
  `frame_count = N`, `init_size 19369`, `frame_stride 61`), then the INIT record (live
  tilemap `0x300` via the `0xa0d8` far ptr + the 7×12 anim-channel records via the
  `0x4c70`/`0x4cbc` slot tables + `entity_state[0x200]` carrying the P1/P2 sprite-object
  descriptor pointees + a `move_data[0x4600]` low-DGROUP static window (the move-script +
  cell-animation tables and their tile-def/frame/stream blobs) + the 85-byte scalar union),
  then `FRAME[0]` (INIT-scalar mirror) and one `FRAME[k]` per tick (trailing
  rng/input + FNV-1a `tilemap_hash` + the assert-set state). The file holds `N+1` frame
  records; `frame_count = N` matches `tools/int8_ctest.c` `read_trace`/`run_replay` (which
  loops `k=1..frame_count` over `FRAME[k]`). Every DGROUP field offset is grounded in the
  per-function oracle gates (`tools/*_oracle.py`). All guest reads via `phys_readb` at
  `DGROUP(0x185f):offset`.

  Arms only inside a real level (`current_level (0x79b2) >= 1` and `game_mode (0x792c) != 0`)
  so title/menu/level-intro loop-top hits are skipped; after N tick frames it flushes,
  closes, and `DoKillSwitch()`es for a clean headless exit. Entirely gated behind
  `BUMPYCAP_INT8_OUT` (output path) + `BUMPYCAP_INT8_FRAMES` (N) — a run without those env
  vars is unaffected. `INT8_VERSION` in `int8_trace.h` is the drift guard: on any layout
  change bump it there and update this emitter (stale traces then hard-fail at load).

- **`03-framebuffer-capture.patch`** — the per-frame VGA `A000` 4-plane framebuffer dump,
  applied ON TOP of `01`+`02` (it adds a second hook to the same `DEBUG_HeavyIsBreakpoint`
  in `src/debug/debug.cpp`; `01`/`02` are unchanged). It is the rendered-pixel companion to
  `02`'s int8 state SNAP: it dumps the displayed VGA page so the playable `BUMPYP.EXE`'s
  rendered frames can be diffed pixel-for-pixel against the original `BUMPY.EXE`'s (the
  Task-11 frame-compare gate).

  The trigger is the SAME per-tick loop TOP as `02` (so an FB frame lines up 1:1 with the
  int8 tick at that loop iteration — the just-presented frame), but the trigger CS:IP is
  **env-overridable** (`BUMPYCAP_FB_TRIG_CS` / `BUMPYCAP_FB_TRIG_IP`, hex), default = the
  original's `cs=0x0824 ip=0x0cda`, so Task 11 can retarget the playable relink's different
  code offsets. Arming reuses the in-level gate (`current_level >= 1 && game_mode != 0`),
  honoring the `01`-overridable offsets `BUMPYCAP_OFF_CURLEVEL` / `BUMPYCAP_OFF_GAMEMODE`.

  **VGA planar read:** dosbox-x does NOT store VGA RAM in the system memory image, so
  `phys_readb(0xA0000)` is wrong (it reads unmapped system RAM = `0xFF`). Instead the planes
  are read the way the renderer does — from `vga.mem.linear[]`, which holds the 4 planes
  INTERLEAVED as one byte each per byte-offset (the EGA latch dword; see
  `VGA_Generic_Read_Handler` in `src/hardware/vga_memory.cpp`): plane P at byte offset `off`
  = `vga.mem.linear[(off*4 + P) & memmask]`. The displayed page is selected by the CRTC Start
  Address (`vga.crtc.start_address_high`/`_low`, word units): page0 → byte 0, page1 → byte
  `0x2000` (the original double-buffers `A000`(page0)/`A200`(page1) at the same linear
  addresses, masked to the `A000` 16KB aperture). `VGA_PLANE_BYTES = 0x1F40` (8000 =
  320×200/8, the visible area) bytes per plane are read.

  **Record:** per frame = 4 planes × `0x1F40` bytes, plane order 0,1,2,3 — a flat raw stream,
  no header. Total file = `frames * 4 * 0x1F40`. Lifecycle mirrors `02`: env-gated init,
  in-level arm, one record per armed hit, then flush/close + `DoKillSwitch()` after N frames.
  Entirely gated behind `BUMPYCAP_FB_OUT` (output path) + `BUMPYCAP_FB_FRAMES` (N, default 150)
  — a run without those env vars is unaffected.
