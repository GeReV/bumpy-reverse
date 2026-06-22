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
  the frame-boundary SNAP capture (binary trace at the logical present boundary) will be a
  follow-on patch that builds on the same hook.
