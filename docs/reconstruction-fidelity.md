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
| `game_stubs.c` | **Linkability scaffolding (DEFERRED Phase 2)** | NOT a reconstruction. Faithful-SIGNATURE no-op/benign-default stubs for the ~70 engine functions the session/loop spine transitively calls but that are not yet reconstructed (hardware/resource init, the menu/transition/screen functions, P2 step, grid-history, the anim-channel STEP/DRAW/ERASE chain, player-view erase/render, present/tick-wait, input/collision helpers, the out-of-scope player handler-table modes, the FX/anim-channel allocator (`apply_cell_animation` 69aa) + sound/anim leaves, the runtime-populated `mode_script_tbl`, and `dos_abort`). Each cites its engine address. These exist only so `BUMPY.EXE` LINKS; they make the loop link, not RUN correctly. **Phase 2 un-stubbed the two landing leaves (`land_on_tile_below` 2810 / `check_tile_below_ladder_or_land` 29a6) and the jump/fall/bounce move-step substates into `player.c`** (see the Phase-2 audit below); `game_stubs.c` now stubs only their FX/sound/exit/item callees + the still-deferred spine. Where a stub's return value steers `game_loop` (`run_main_menu`→0, `all_entries_flag_set`→0) the loop effect is noted in-code. |
| `level.c` | Transcription (compile/link) | `start_level` (1000:2d14) level-1 path wired to the validated Phase-0 render API; see prior audit. Owns `current_level` (0x8f40), `copyprotect_flag`, `p1_start_x/y`, `current_entity_index`. **No ownership edits were needed in Task 7** — its globals were already cleanly owned. |
| `input.c` | Transcription | Pure keyboard-input layer ported 1:1 (install_keyboard_isr 798a, get_key_state 7ab4, flush_keyboard_buffer 7b01, input_state_clear 65d2, poll_input 1dde, read_input_action 75a2). Owns `input_state` (0x8244), `g_key_state_table` (0x4d42), `g_joystick_handler_table` (0x4cf2), `g_keyboard_isr_installed` (0x4dc4). Its `extern int dos_abort(void)` (faithful abort-path control flow) is resolved by a `game_stubs.c` stub for the linked build. **No ownership edits were needed in Task 7.** |
| `player.c` | Transcription | P1 move-execution spine + 19 game-mode handlers + tile-collision leaves, ported 1:1 (Tasks 6a/6b/6c); dispatch/contact/action tables dumped byte-exact from the unpacked image. Owns all P1 move-state scalars (`p1_pixel_x/y`, `game_mode`, `move_locked`, `p1_cell`, `rng_frame`, …) and the dumped tables. Its forward-declared out-of-scope handlers + `mode_script_tbl` are resolved by `game_stubs.c` for the linked build (NOT reached on the level-1 boot path). **No ownership edits were needed in Task 7.** (Phase 2 reconstructs the 2 landing leaves + the jump/fall/bounce move-step substates here — see the Phase-2 audit below.) |

## Phase-2 module audit (physics state machine additions to `player.c`)

| Module / function set | Fidelity | Notes |
|---|---|---|
| `land_on_tile_below` (2810) | Transcription | Landing handler: prev-mode guard, per-device sound lookup, cell-8 tile probe → `land_mode_fx_tbl`, FX dispatch. Tables `land_mode_fx_tbl` (0x60 B), `land_sound_tbl_opl` / `land_sound_tbl_std` (0x30 B each) dumped byte-exact from the unpacked image. Callee `apply_cell_animation` (69aa, anim-channel allocator) stays stubbed → Phase 5. Sound callees stubbed → Phase 6. |
| `check_tile_below_ladder_or_land` (29a6) | Transcription | Tile-below probe: signed `== 0x0e` ladder test (mirrors engine's signed char compare), three exit paths — climb (enter mode 0x0a + `apply_cell_animation` stubbed), down-held delegate (`move_down_step`), else pending-action delegate (`p1_exec_pending_action`). `apply_cell_animation` stubbed → Phase 5; delegate substates ported in Task 4. |
| Move-step substates: `cursor_move_up` / `_down` / `_right` (64e2/64ff/6535) | Transcription | Cursor row/column counters; thin wrappers around `enter_game_mode` + `dispatch_move_step`. Ported 1:1. |
| `p1_try_jump_action` (6587) | Transcription | Jump-trigger: checks `move_locked` + input bit; sets `jump_ticks = 0x34`; enters jump mode + dispatches. Dumped jump-tick constant matches engine. |
| `p1_try_trigger_pending_action` (654e) | Transcription | Pending-action trigger: indexes `pending_anim_lut_3cda` by `p1_pending_action`; calls `p1_set_cell_animation`. Table (0x30 B) dumped byte-exact. |
| `input_state_mask_10` / `_1d` / `_0f` (65e5/65fb/6611) | Transcription | Input-state mask combiners: AND `input_state` with the named literal; thin, ported 1:1. |
| `p1_move_step_with_sound` (6648) | Transcription | Move-step with sound: anim select via `pending_anim_lut_3d0a`, per-device sound via `move_sound_lut_opl_25ae` / `_std_25de` (tables dumped byte-exact, 0x30 B each); runs `p1_dispatch_pending_action` + `play_sound` (stubbed → Phase 6). |
| `move_step_last_variant` (66d8) | Transcription | Contact-sound dispatch: indexes `contact_sound_lut_35de` by `p1_contact_code` (table 0x30 B, dumped byte-exact); calls `apply_contact_action` (stubbed → Phase 5/6). |
| `move_step_landed` (6717) | Transcription | Post-step land: reads tile under cell, checks `tile_followup_action_lut`; if tile is `'['` clears `level_complete_flag`; indexes `g_anim_channel_idx`. Table `tile_followup_action_lut` (0x30 B) dumped byte-exact including the 6 tail bytes that overlap the dispatch table. |
| `move_step_noop` / `move_step_noop_sentinel` (673a/7111) | Transcription | No-op substates; return immediately. Ported 1:1. |
| `run_physics_settle_wrap` / `run_physics_settle` / `FUN_1000_22b0` (22c1/22fc/22b0) | Transcription | Physics settle: unfreeze, busy-loop 1000 × `p1_read_tile_under`, decrement `settle_countdown`, raise `frame_abort_flag` on expiry. Ported 1:1 including the 1000-iteration literal. |
| `p1_exec_pending_action` (465e) | Transcription | Pending-action executor: indexes `pending_action_lut_36be` (0x30 B, dumped byte-exact) by `p1_pending_action`; calls `exec_move_action` with the result. |
| `move_down_step` (253f) | Transcription | Downward step: increments `move_step_count`; if counter reaches threshold delegates to `check_tile_below_ladder_or_land`, else dispatches. Ported 1:1. |
| Supporting wrappers: `p1_set_cell_animation*`, `p1_trigger_cell_animation`, `p1_dispatch_pending_action`, `p1_step_landed`, `p1_read_tile_under`, `run_physics_settle` | Transcription | Thin anim/tile wrappers, each ported 1:1 from the live decomp. All call the stubbed `apply_cell_animation` (→ Phase 5) or the stubbed `play_sound` (→ Phase 6). |

**Phase-2 deviations (all in-code RECONSTRUCTION FIDELITY notes present):**

- `apply_cell_animation` (69aa, anim-channel allocator) — extern stub throughout; the full channel-A allocator subsystem is out of scope → Phase 5.
- `play_sound` / `play_state_sound` (6e11/647e) — extern stubs → Phase 6.
- `check_exit_tile_vert` (6372) / `move_step_read_item` (6627) — exit/item boundary callees stay stubbed → Phase 3.
- `p1_movement_dispatch` (1e02) call-through — this Phase-1 dispatcher tail-calls `dispatch_move_step` with the raw engine near-offset from `game_mode_handlers`; the host cannot call through that table (engine offsets, not host fn-ptrs). The harness skips these 624 records (`UNPORTED`); the physics validation covers every ported substate directly. Modeling the call-through dispatcher as a host-safe table is deferred (not a physics gap).

**Phase-2 validation method:** per-function physics differential (seed entry snapshot + captured script/tilemap → call the reconstructed C fn → assert 10 output fields vs the engine's exit snapshot) **plus** trajectory-stitch over 5 scenarios (walk / jump-up→bounce / jump+lateral arc / ledge-fall / land). Validation granularity is the engine's **move-step**, not the int8 tick (the int8 end-to-end gate remains deferred — see Phase-1 status note below). The 624 UNPORTED records are solely the `p1_movement_dispatch` call-through dispatcher entries; every ported substate passes FAIL=0.

## Phase-2 status (Player-1 physics state machine)

As of Phase-2 Task 4, the Player-1 physics state machine is **reconstructed and validated**:

- **Reconstructed 1:1**: landing leaves (`land_on_tile_below` 2810, `check_tile_below_ladder_or_land` 29a6) plus the full set of move-step substates and handler delegates (`cursor_move_*`, `p1_try_jump_action`, `p1_try_trigger_pending_action`, `input_state_mask_*`, `p1_move_step_with_sound`, `move_step_last_variant`, `move_step_landed`, `move_step_noop*`, `run_physics_settle_wrap`, `p1_exec_pending_action`, `move_down_step`) — all ported 1:1 from the live Ghidra decomp (verified via MCP + disasm).
- **Runtime move-scripts recovered**: the bounce/gravity arc scripts (`[anim, dx, dy]` 6-byte entries for modes 0x0/0x2/0x3/0x4/0x6/0x2d) recovered by T1 physics oracle capture; byte-exact land/contact tables (six LUTs, 0x30–0x60 B each) dumped from the unpacked image.
- **Gate re-confirmed (2026-06-19)**: `validate_physics` PASS=16584 FAIL=0 UNPORTED=624; trajectory-stitch fully matches all 5 scenarios (matched == total − skipped_dispatch, no early stop). No-regression: `validate_blit` 17/17 chain + 24/24 blits; `validate_composite` 54152 @ 53858 baseline; `validate_player` PASS; `BUMPY.EXE` links clean.

**Deferred from Phase 2:**

1. **Exit / item boundary callees** (`check_exit_tile_vert` 6372, `move_step_read_item` 6627) — Phase 3.
2. **Player 2** — Phase 4.
3. **Anim-channel / FX allocator** (`apply_cell_animation` 69aa) — Phase 5.
4. **Sound** (`play_sound`, `play_state_sound`) — Phase 6.
5. **`p1_movement_dispatch` call-through host model** — the dispatcher's handler table holds raw engine near-offsets; a host-safe call-through table model is needed before end-to-end host replay can drive the full game-mode handler chain. Not a physics gap; deferred.
6. **int8-synced end-to-end gate** — the Unicorn capture granularity does not match the engine's physics-frame rate. A frame-accurate capture (DOSBox path) is needed before the full game-loop can be replay-validated tick-for-tick. Deferred.

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
