# Reconstruction fidelity audit (`src/`)

`src/` is a **structure-faithful mirror** of the original (see CLAUDE.md "Leading
tenet"). This audit lists, per module, where the reconstruction **deviates** from the
original's structure — the reimplementation-leaning parts to keep labeled and, where
feasible, bring back toward 1:1. Each module's `.h` carries the detailed in-code
`RECONSTRUCTION FIDELITY` note; this is the index.

## Classification

- **Transcription** — 1:1 with the decompiled function (same control flow, same data).
  This is the target for all of `src/`.
- **Behavior-faithful reconstruction** — the original does **not** decompile (e.g.
  self-modifying overlay blitters); reconstructed from raw disasm + validated
  byte/plane-exact against the engine, but the original's *structure* is not preserved.
- **Reimplementation deviation** — an abstraction or merge introduced for the C port
  that the binary does not have. To be minimized / clearly labeled.

## Module audit

| Module | Fidelity | Notes |
|---|---|---|
| `vec.c` / `op12.c` | Transcription | `vec_run` op0/op4/op12 decoders, transliterated from the decomp. |
| `video.c` | Transcription | planar VGA mode-set + map-mask blit (BGI `int 10h` mode set ported). |
| `sprite.c` | Transcription | `sprite_bank_load_transform` = `sprite_bank_relocate_frames` + `sprite_frame_transform`. |
| `sprite_anim.c` | Transcription | `sprite_prepare_frame` = the per-frame select in `prepare_sprite_frames`. The dead `ctrl&0x40` expansion path is reconstructed and kept, marked UNVALIDATED. |
| `sprite_blit.c` | **Behavior-faithful** | `sprite_blit_planar_vga` reconstructs `1cec:10e1`, which does not decompile (self-modifying, unrolled, jump-table). Models the GC/Seq map-mask + bit-mask RMW as a portable 4-plane memory op, **not** VGA-hardware port writes. Validated byte-exact. `sel!=0` + left-edge clip-carry preload ported but UNVALIDATED. |
| `bg_render.c` | **Behavior-faithful** | `bg_tile_run` / `bg_render_grid` reconstruct `restore_bg_tile_run`'s loop + the mode-01 BGI-overlay tile blitter (`1ab9:0aa0`), which does not decompile. Same memory-image-vs-hardware caveat as `sprite_blit`. Validated byte-exact (119/119 cells). |
| `sprite_chain.c` | **Reimplementation deviation** | `sprite_blit_build_desc` **merges three** original functions (`object_list` + `clip` + `setup`) into one descriptor builder. The three decompile cleanly and *should* be reconstructed 1:1; the merge is a convenience to revisit. Validated descriptor-exact. |
| `entity.c` | Mixed | Layer-C/A/B placement + `draw_p1/p2_sprite` are transcriptions of the `spawn_and_draw_level_entities` (`1000:2a78`) loops and the draw fns. **Deviations:** `entity_blit_object` is a shared helper the engine doesn't have (the engine inlines/repeats the prepare→blit per call); the engine's erase (`restore_bg_view`) and `render_player_view` save-under/read-back steps are **omitted** (the composite builds bg first and models a single page). See [rendering-pipeline.md](rendering-pipeline.md). |

## Host/validation tooling (not part of the decompilation)

These are reimplementation/validation artifacts — the *Devilution-X*-flavored side — kept
clearly separate from the documentary `src/` mirror:

- `tools/composite_ctest.c` — models a **4-plane memory image** and the a000/a200
  double-buffer to validate the entity composite plane-exact. It is **not** how the
  engine renders (no BGI overlay, no VGA ports, no real page-flip); it's a differential
  oracle harness.
- `tools/sprite_oracle.py` — boots the real binary under Unicorn to capture engine ground
  truth (FRM3/FRM4); pure instrumentation.
- The various `*_ctest.c` host replays + `validate_*.sh` — differential validation only.

## Known open items (toward stricter fidelity)

1. **`sprite_chain`**: split the merged `object_list`/`clip`/`setup` back into three 1:1
   functions.
2. **Blitters**: the two behavior-faithful blitters are the best achievable for
   non-decompiling self-modifying overlay code; keep the disasm-grounded reconstruction +
   document that the structure is intentionally not preserved.
3. **`entity.c`**: the omitted erase / `render_player_view` save-under path is documented
   in [rendering-pipeline.md](rendering-pipeline.md); decide whether the `src/` mirror
   should include those engine steps (for fidelity) even though the memory-image composite
   doesn't need them.
