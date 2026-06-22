# dosbox-x instrumentation patches

Tracked recipe (the patch), applied by `tools/dosbox/build-dosbox-x.sh` onto the pinned
source. No DOSBox fork is vendored — only these patches.

- **`01-observability-probe.patch`** — bring-up probe. Adds a per-video-frame hook in
  `VGA_VerticalTimer` that `LOG_MSG`s the BIOS video mode (BDA `0040:0049`, via `phys_readb`
  — `real_readb`/`mem_readb` are forbidden in that VGA-render TU) plus the foreground
  `CS:IP`. Used to confirm BUMPY.EXE runs and to read its runtime segments. **Verbose**
  (throttled to every 35th frame) and an observability aid, not the production capture —
  the frame-boundary SNAP capture + input injection will supersede/gate it.
