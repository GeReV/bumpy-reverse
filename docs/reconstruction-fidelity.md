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
| `bgi_overlay.c` | **Behavior-faithful** | `restore_bg_view` (`1000:80bc` → `1ab9:0d77`) and `render_player_view` (`1000:93b8` → `1ab9:1028` → `1ab9:0db0`) reconstruct the two BGI-overlay dispatch wrappers. `render_player_view` sub-handler 0 (full 4-plane GC Read-Map-Select + rep-movsw copy) is fully reconstructed; sub-handlers 3–6 (masked copy variants) are STUBBED and UNVALIDATED. **Key finding:** both functions are structural NOPs in the layer-A/B draw context (code-embedded view descriptors at `0x114b:0x74a0` / `0x751e` have `word[0]` / `word[0x0e]` > 1 → NOP guard fires; see `present_model.md` §5). The NOP path is exercised by the harness via a `nop_view` descriptor mirroring the code-embedded values. |
| `sprite_chain.c` | Transcription | `sprite_blit_object_list` (0e48) + `sprite_blit_clip` (0f50) + `sprite_blit_setup` (103d) reconstructed 1:1 from the clean decomp. `sprite_blit_build_desc` is a thin public wrapper. One documented residual deviation: `sprite_blit_setup` fills the descriptor rather than tail-calling `sprite_blit_planar_vga` (the composite invokes the blitter separately). Validated descriptor-exact (17/17). |
| `entity.c` | Mixed | Layer-C/A/B placement + `draw_p1/p2_sprite` are transcriptions of the `spawn_and_draw_level_entities` (`1000:2a78`) loops and the draw fns. `entity_blit_object` is a shared helper the engine doesn't have (the engine inlines/repeats the prepare→blit per call). `entity_draw_layer_a/_b` now mirror the engine's full 3-step draw sequence: **erase (`restore_bg_view`) → blit (`blit_sprite`) → save-under (`render_player_view`)** — both erase and save-under are STRUCTURALLY PRESENT, called via `nop_view`, and are effective NOPs (code-embedded views in the engine; see `bgi_overlay.c` entry). Composite unchanged: 54152/64000 (world-8 live page). See [rendering-pipeline.md](rendering-pipeline.md). |
| `main.c` | **Reimplementation deviation (CRT/startup)** | The original's `main` and ~60 CRT-garble startup functions (TC++ CRT0, `__cstart`, etc.) are NOT reproduced — they are not decompilable and are not meaningful. This reconstruction uses the Open Watcom CRT with a standard `main`. As of Phase-1 Task 7 `main` is WIRED TO THE REAL SESSION: it calls `init_game_session_state()` then `run_game_session()` (both reconstructed in `game.c`); the Task-3 stubs are gone. |
| `game.c` | Transcription (spine) | The four session/loop functions ported 1:1 from the decomp (cross-checked live via Ghidra MCP): `run_game_session` (1000:0258), `init_game_session_state` (1000:0282), `reset_game_state` (1000:0bf9), `game_loop` (1000:0c18). Control flow, comparisons and call order reproduced verbatim. Owns the cross-module session/round/tick control globals (`round_continue_flag` 0x9d30, `session_continue_flag` 0x856d, `frame_abort_flag` 0x928d), the `tilemap` far pointer (0xa0d8 — no single module home), and the menu/score/level-index scratch. **Deviations** (all noted in-code): (a) the per-function Turbo-C `stack_check_limit`/`FUN_ab83` stack-check prologue is CRT scaffolding, omitted (OW CRT does its own); (b) `init_game_session_state`'s VGA mode set is surfaced via `video_set_mode_0d()` so the boot harness has an observable mode 0x0D (engine sets it inside the FUN_9821 CRTC block); (c) the ~46 trailing UNNAMED `DAT_203b_xxxx` resets in `init_game_session_state` are collapsed into the documented `reset_opaque_session_globals()` stub rather than invented as named externs. |
| `game_stubs.c` | **Linkability scaffolding (DEFERRED Phase 2)** | NOT a reconstruction. Faithful-SIGNATURE no-op/benign-default stubs for the ~70 engine functions the session/loop spine transitively calls but that are not yet reconstructed (hardware/resource init, the menu/transition/screen functions, P2 step, grid-history, the anim-channel STEP/DRAW/ERASE chain, player-view erase/render, present/tick-wait, input/collision helpers, the 2 Task-6c landing leaves `land_on_tile_below` 2810 / `check_tile_below_ladder_or_land` 29a6, the out-of-scope player handler-table modes, sound/anim leaves, the runtime-populated `mode_script_tbl`, and `dos_abort`). Each cites its engine address. These exist only so `BUMPY.EXE` LINKS; they make the loop link, not RUN correctly. Where a stub's return value steers `game_loop` (`run_main_menu`→0, `all_entries_flag_set`→0) the loop effect is noted in-code. |
| `level.c` | Transcription (compile/link) | `start_level` (1000:2d14) level-1 path wired to the validated Phase-0 render API; see prior audit. Owns `current_level` (0x8f40), `copyprotect_flag`, `p1_start_x/y`, `current_entity_index`. **No ownership edits were needed in Task 7** — its globals were already cleanly owned. |
| `input.c` | Transcription | Pure keyboard-input layer ported 1:1 (install_keyboard_isr 798a, get_key_state 7ab4, flush_keyboard_buffer 7b01, input_state_clear 65d2, poll_input 1dde, read_input_action 75a2). Owns `input_state` (0x8244), `g_key_state_table` (0x4d42), `g_joystick_handler_table` (0x4cf2), `g_keyboard_isr_installed` (0x4dc4). Its `extern int dos_abort(void)` (faithful abort-path control flow) is resolved by a `game_stubs.c` stub for the linked build. **No ownership edits were needed in Task 7.** |
| `player.c` | Transcription | P1 move-execution spine + 19 game-mode handlers + tile-collision leaves, ported 1:1 (Tasks 6a/6b/6c); dispatch/contact/action tables dumped byte-exact from the unpacked image. Owns all P1 move-state scalars (`p1_pixel_x/y`, `game_mode`, `move_locked`, `p1_cell`, `rng_frame`, …) and the dumped tables. Its forward-declared out-of-scope handlers + the 2 deferred landing leaves + `mode_script_tbl` are resolved by `game_stubs.c` for the linked build (NOT reached on the level-1 boot path). **No ownership edits were needed in Task 7.** |

## Phase-1 slice status (vertical slice — session → loop → modules)

As of Phase-1 Task 7 the reconstructed `src/` tree forms a complete, **linkable**
session→loop→module graph:

- **Compiles clean**: every reconstructed TU builds under `wcc -ml -bt=dos -zq -wx`
  (zero warnings), including the new `game.c` / `game_stubs.c`.
- **Links**: `BUMPY.EXE` LINKS as a whole — `main → init_game_session_state +
  run_game_session → game_loop` plus the reconstructed `level` / `input` / `player`
  modules and the validated Phase-0 render core. Cross-module DGROUP global
  ownership is fully resolved (one owner per global; no duplicate symbols). The
  not-yet-reconstructed per-tick callees are faithful-signature stubs in
  `game_stubs.c`.
- **Boots to mode 0x0D**: under `tools/run_bumpy.py` (Unicorn) the binary sets
  VGA mode 0x0D (the one externally observable boot effect) before entering the
  game loop.
- **Validated per-function** (unchanged in Task 7): blit 17/17 + chain 17/17 +
  blitter 24/24 (`validate_blit`), composite 54152/64000 @ 53858 baseline
  (`validate_composite`), the P1 move spine + dispatch + tile-collision
  (`validate_player`), per-tick input 100/100 (`validate_input`).

**DEFERRED (known Phase-1 limitations):**

1. **End-to-end per-tick validation** — the Unicorn golden trace is a desynced
   capture (its tick granularity ≠ the engine's physics-frame rate; Bumpy walks off
   level 1). A frame-accurate capture (the roadmap's DOSBox path) is needed before
   the full game loop can be replay-validated. So Task 7 ships the faithful
   session/loop *decompilation* + module integration, NOT an end-to-end gate.
2. **The per-tick spine bodies** — the ~70 `game_stubs.c` entries (P2 step,
   grid-history, the anim-channel STEP/DRAW/ERASE chain, player-view present,
   present/tick-wait, the menu/transition/screen functions, the 2 landing leaves,
   the out-of-scope player handler modes) are linkability stubs, not reconstructions.
   They are Phase-2 work.
3. **Runtime level load** — the boot harness has no INT 21h file I/O, so once the
   loop reaches `start_level` / the per-tick spine it cannot load level data; the
   boot run spins rather than exiting cleanly. Expected and deferred.

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

1. **Blitters**: the two behavior-faithful blitters are the best achievable for
   non-decompiling self-modifying overlay code; keep the disasm-grounded reconstruction +
   document that the structure is intentionally not preserved.
2. **`bgi_overlay.c` sub-handlers 3–6**: the masked copy variants (`1ab9:0ecc`–`0e3c`) are
   STUBBED.  They are not triggered by any oracle-captured path (fullscreen_buf and
   layer-A/B NOP; sub-handler 0 is the only active path seen in the oracle).  Reconstruct
   when a call site that exercises them is identified.
3. **`restore_bg_view` inner blit**: the outer dispatch wrapper is reconstructed; the inner
   `1ab9:0aa0` bg-tile blitter is NOT separately exposed from `bg_render.c` — it is covered
   there as `bg_tile_run`.  Structural consolidation may be desirable for completeness.
