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

## Phase-3 module audit (item/scoring/exit game-state — `items.c`)

| Module / function set | Fidelity | Notes |
|---|---|---|
| `read_tile_layer2` (6bf4) | Transcription | Layer-C item-byte read leaf: `p1_item_code = tilemap[cell + 0x60]`. Sibling of `read_tile_layer_contact` (+0x30) / `read_tile_at_cell` (+0x00) in `player.c`; owns `p1_item_code`, reconstructed here. Ported 1:1. |
| `p1_collect_item_score` (6c95) | Transcription | Score award: base +250 (`ADD score_lo,0xfa; ADC score_hi,0`), then per item-code: `'#'` increments `sharp_item_counter` only; `'/'` adds +9750 (`+0x2616`); `'0'` adds +49750 (`+0xc256`). **Score arithmetic**: the Ghidra decomp hoists the carry into comparator expressions; the raw disasm (6ceb–6d1f) is a plain `ADD/ADC` sequence — this port mirrors the machine code (faithful form); result is identical. **Cell-view erase staging** (6ca1–6ce7): the engine queues a 2-tile view-erase (`pending_erase_count`, pixel tables, erase_kind) into the per-tick view-present chain — render-subsystem state (→ Phase 5); documented in-code, not reproduced against invented render globals. Numerically verified via items-trace differential. |
| `p1_collect_item` (6c14) | Transcription | Item pickup: calls `p1_collect_item_score`, clears `tilemap[p1_cell+0x60]`; decrements `items_remaining` (unless item is `\x01` or `'#'`); on last item sets `level_complete_flag = 1`, arms `level_complete_anim_counter = 0xf2`, relocates `anim_target_cell`, fires exit animation. Deviations (all noted in-code): `apply_cell_animation` (69aa → Phase 5), `play_sound` (→ Phase 6), `collect_mode_2810 = 0xf` DGROUP tag reproduced but not part of validated semantic state. |
| `move_step_read_item` (6627) | Transcription | Move-step dispatch leaf: calls `read_tile_layer2(p1_cell)`; if `p1_item_code != 0`, calls `p1_collect_item`. Ported 1:1 from the decomp. |
| `check_exit_tile_vert` (6372) | Transcription | Exit-tile detection: if `move_step_count != 7` AND `tilemap[p1_cell+0x30] == 0x0c`, commits the exit transition: `p1_move_step_idx = 0`, `physics_frozen = 1`, `enter_game_mode(0x2e)`, plays exit sound (device-dependent). Verified against disasm 1000:6372–63bd (offset guards, tilemap probe, sound-id select). `play_sound` stays stubbed → Phase 6. |
| `teleport_to_next_exit_tile` (25ad) | Transcription | Teleport: forward-scans `tilemap[scan_cell]` (wrapping at 0x30) for the next `0x0f` tile; on hit: sets `anim_target_cell = p1_cell = scan_cell`, calls `p1_set_pixel_from_cell`, nudges `p1_pixel_y += 0x0d`, fires teleport FX + sound, enters game mode 0x0f, dispatches. Verified against disasm 1000:25ad–2633. `apply_cell_animation` + `play_sound` stubbed → Phase 5/6; `p1_set_pixel_from_cell` (4906) is a faithful-signature stub in `game_stubs.c` for `BUMPY.EXE` (→ Phase-2 player subsystem completion); the items harness reproduces its coord-table effect for the per-fn differential. |
| **Protection hook (`level.c`)** | **Deviation** | `start_level` (1000:2d14) opens with: `if (1 < current_level && copyprotect_flag == 0) copyprotect_challenge(); if (copyprotect_flag == -1) current_level = 1;` The entire hook (both guards + challenge body) is compiled behind `#ifdef BUMPY_COPY_PROTECTION`, **not defined** in the default build → compiles out. With it OFF, level-advance to levels 2+ flows with no challenge, exactly matching the **cracked-build** runtime (the shipped DOS English release unconditionally sets `copyprotect_flag = 1` in `copyprotect_challenge` — before any input — so the compare never fires and `-1` is never written). The `#define` switches the hook back on; the placeholder `copyprotect_challenge()` carries only the cracked-build invariant until the **faithful un-cracked body** (sprite quiz + answer compare) ports in **Phase 7b**. |
| **Level-advance wiring (`game.c`)** | Transcription (structure) | The exit→advance path is the Phase-1 `run_game_session`/`game_loop` tail (1000:0258/0c18), ported 1:1 from the decomp: `all_entries_flag_set()` → `current_level + 1` → `if (current_level != 0x0a)` break (title return), else loop. `start_level(current_level, current_level)` is the generalised call (the engine's `start_level` reads `current_level` directly; passing it for both args reproduces the read exactly). **Validation method**: the advance state-transition was validated by a **hand-rolled re-derivation of the advance decision inside the test harness** (`items_ctest.c`'s `adv_step`/`adv_start_level`), which exercises the advance logic (level-complete flag → `current_level+1`, protection-off path, boundary at 0x0a) without going through the reconstructed `run_game_session` / `game_loop` engine path. The reconstructed engine path (`run_game_session` loop with `start_level`, screen-transition stubs, etc.) was NOT exercised end-to-end — its per-tick callees remain stubs, making a full reconstructed-path advance run impractical. That full engine-path validation is DEFERRED (see Phase-3 deferred list). The wiring structure (control-flow, comparisons, call order) is structurally 1:1 from the decomp and is documented in-code. |

**Phase-3 validation method:** per-function semantic-state differential at the item/exit function-call boundary (discrete events; no trajectory desync). Engine ground truth captured by `tools/items_oracle.py` (Unicorn instrumentation); 5 scenarios (collect normal/special/last, exit detect, reach exit + complete); 11 records total; every ported record PASS, FAIL=0, UNPORTED=0.

**Phase-3 deviations (all in-code RECONSTRUCTION FIDELITY notes present):**

- `apply_cell_animation` (69aa, anim-channel allocator) — extern stub throughout; FX allocator subsystem → Phase 5.
- `play_sound` / `play_state_sound` (6e11/647e) — extern stubs → Phase 6.
- `p1_set_pixel_from_cell` (4906) — faithful-signature stub in `game_stubs.c` for the `BUMPY.EXE` link; its coord-table effect is reproduced in the items-harness differential → Phase-2 player subsystem completion.
- Cell-view erase staging (6ca1–6ce7) — render-subsystem state (per-tick view-present chain, → Phase 5); documented in-code rather than reproduced against invented render globals.
- Copy-protection challenge body — `#ifdef BUMPY_COPY_PROTECTION` gated (OFF by default); faithful un-cracked sprite-quiz body → Phase 7b.
- Full reconstructed-engine-path level-advance validation — deferred (live `run_game_session` loop has stubbed callees; impractical until stubs are un-stubbed or a live loop replay harness is built).

## Phase-3 status (item/scoring/exit game-state + level-advance)

As of Phase-3 Task 4 (validate gate re-confirmed Phase-3 Task 5), the item/scoring/exit game-state is **reconstructed and validated**:

- **Reconstructed 1:1**: `read_tile_layer2` (6bf4), `p1_collect_item_score` (6c95), `p1_collect_item` (6c14), `move_step_read_item` (6627), `check_exit_tile_vert` (6372), `teleport_to_next_exit_tile` (25ad) — all ported 1:1 from the live Ghidra decomp (verified via MCP + raw disasm).
- **Gameplay loop closed (structurally)**: collect → `level_complete_flag` → `all_entries_flag_set` → `current_level+1` → `start_level(N)` wiring is structurally 1:1 from the decomp. The copy-protection hook is faithful-structure (cracked-build semantics reproduced by default; `#define` switches the hook on for the un-cracked body when it lands in Phase 7b).
- **Gate re-confirmed (2026-06-20)**: `validate_items` PASS=11 FAIL=0 UNPORTED=0 (all five item/exit functions ported); level-advance state-transition re-derivation PASS (1→2 `start_level(2)`, protection-OFF path, boundary 0x0a). No-regression: `validate_blit` 17/17 anim + 17/17 chain + 24/24 blits; `validate_composite` 54152 @ 53858 baseline; `validate_player` PASS; `validate_physics` PASS=16584 FAIL=0 UNPORTED=624; `BUMPY.EXE` links clean.

**Deferred from Phase 3:**

1. **Faithful un-cracked copy-protection challenge body** (`copyprotect_challenge` 1000:4015, sprite quiz + answer compare) — Phase 7b. `#define BUMPY_COPY_PROTECTION` switches it on.
2. **Score HUD display** (rendering the 32-bit score on-screen) — Phase 7.
3. **FX allocator** (`apply_cell_animation` 69aa, anim-channel allocator) — Phase 5.
4. **Sound** (`play_sound`, `play_state_sound`) — Phase 6.
5. **Player 2 collision** — Phase 4.
6. **`p1_set_pixel_from_cell` (4906)** — faithful-signature stub; completion deferred to Phase-2 player subsystem completion.
7. **Full reconstructed-engine-path level-advance validation** — the live `run_game_session` loop has stubbed callees (screen-transition fns, P2 step, etc.); a complete advance run through the reconstructed engine path requires those stubs to be un-stubbed (or a live loop replay harness). Deferred.
8. **int8-synced end-to-end gate** — Unicorn capture granularity does not match the engine's physics-frame rate; a frame-accurate capture (DOSBox path) is needed. Deferred (unchanged from Phase 2).

## Phase-4 module audit (two-player AI subsystem — `player2.c`)

| Module / function set | Fidelity | Notes |
|---|---|---|
| **Move-state machine** (T3): `p2_set_move_state` (4bc6), `p2_step_scripted_move` (4c14), `p2_update_grid_cell` (4b4e), `p2_tile_move_check` (4c99), `p2_set_pixel_from_cell` (48a9), `p2_advance_grid_history` (13b2) | Transcription | Ported 1:1 from the live Ghidra decomp. P2 move-state machine mirrors P1's (player.c) in structure but operates on P2-specific globals (`p2_pixel_x` 0x79ba, `p2_pixel_y` 0x79bc, `p2_move_anim` 0x8560, `p2_cell` 0x8571). |
| **AI decision layer** (T4): `p2_ai_dispatch_move` (4f4e), `p2_ai_select_move_a` (4f04), `p2_ai_select_move_b` (4f89), `p2_ai_select_move_random` (4fd3), `p2_choose_move_state1`, `p2_choose_move_state2`, `p2_pick_move_priority_a/b/c`, `p2_run_move_state_handler` (5003), `p2_cell_move_up/down/left/right` | Transcription | Ported 1:1 from the decomp. **AI determinism validated genuinely**: the rng-driven decisions in `p2_ai_select_move_random` call `rand()` (the reconstructed prng in `src/prng.c`) AND read `rng_frame`; the validate_p2 harness seeds the reconstructed prng state so `rand()` returns the same sequence as the engine — the determinism is real, not faked. `select_move_b` threshold logic and `select_move_random` modulo are reproduced 1:1 from the decomp. **Coverage note**: 3 of 4 cell-move handlers are direction-seeded; the 0x85c handler table is runtime data (populated at boot, not decompilable as a static literal), so only `state-2 → cell_move_down` is end-to-end capture-validated; `cell_move_up/left/right` are transcribed from the decomp and pass the per-fn differential but are not independently capture-validated. |
| **Render / view + pvp** (T5): `draw_p2_sprite` (1cea), `render_p2_view` (1c41), `erase_p2_view` (19a1), `update_p2_bbox` (50c0), `check_pvp_collision` (50fb) | Mixed (transcription + faithful-signature stubs) | Ported 1:1 from the decomp. P2 draw validated at the **descriptor level** (the `draw_p2_sprite` descriptor fields: `p2_pixel_x`, `p2_pixel_y`, `p2_move_anim` match the capture; the underlying blitter is already plane-exact 24/24 from Phase 0). The blit/view leaf calls are **faithful-signature stubs**: `blit_sprite` is inlined in `entity.c` and not separately callable; `render_player_view` / `restore_bg_view` carry the 3-arg work-buffer signature matching the engine prototype but their sub-handlers 3–6 are stubbed (the Phase-0 `bgi_overlay.c` core is untouched). The frame-word in the draw descriptor is partially self-referential: `p2_frame_base` (0xa0de) was back-derived from the descriptor capture rather than read directly from the SNAP, so the x/y fields are full gates but the frame-word gate has limited independence. `check_pvp_collision` is validated: overlap/disjoint flag values match the capture (3/3 PvP records). |

**Phase-4 deviations (all in-code RECONSTRUCTION FIDELITY notes present):**

- `blit_sprite` leaf — inlined in `entity.c`, not separately callable; the P2 draw calls through the entity path (faithful-signature stub in `game_stubs.c` un-stubbed to `entity.c`).
- `render_player_view` / `restore_bg_view` (3-arg work-buffer) — faithful-signature, but sub-handlers 3–6 are STUBBED and UNVALIDATED (Phase-0 `bgi_overlay.c` state; see that entry).
- `apply_cell_animation` (69aa, anim-channel allocator) — extern stub throughout → Phase 5.
- `play_sound` / `play_state_sound` (6e11/647e) — extern stubs → Phase 6.
- 0x85c cell-move handler table — runtime data; only `state-2 → cell_move_down` is end-to-end capture-validated; remaining handlers (`cell_move_up/left/right`) are transcription-only.
- `p2_frame_base` (0xa0de) back-derived from descriptor capture for the frame-word gate; x/y fields are independently gated.

**Phase-4 validation method:** per-function semantic-state differential (seed entry snapshot + captured script/tilemap → call the reconstructed C fn → assert output fields vs the engine's exit snapshot) across 14 scenarios (P2 trajectory, move-state, AI rng-decision, AI selection branches, move-step, handler dispatch, PvP overlap/disjoint, P2 draw descriptor). 74 records; PASS=74, FAIL=0, UNPORTED=0, DESC_CHECKED=1.

## Phase-4 status (two-player AI subsystem)

As of Phase-4 Task 5 (validate gate re-confirmed Phase-4 Task 6), the complete P2 subsystem is **reconstructed and validated**:

- **Reconstructed 1:1**: the P2 move-state machine (`p2_set_move_state`, `p2_step_scripted_move`, `p2_update_grid_cell`, `p2_tile_move_check`, `p2_set_pixel_from_cell`, `p2_advance_grid_history`), the AI decision layer (`p2_ai_dispatch_move`, `p2_ai_select_move_a/b/random`, `p2_choose_move_state1/2`, `p2_pick_move_priority_a/b/c`, `p2_run_move_state_handler`, `p2_cell_move_up/down/left/right`), and the render/view + PvP functions (`draw_p2_sprite`, `render_p2_view`, `erase_p2_view`, `update_p2_bbox`, `check_pvp_collision`) — all ported 1:1 from the live Ghidra decomp (verified via MCP + disasm).
- **AI determinism genuine**: `p2_ai_select_move_random` uses the reconstructed `prng.c` rand(); seeding the reconstructed prng state reproduces the engine's rng-driven move decisions exactly.
- **Gate re-confirmed (2026-06-20)**: `validate_p2` PASS=74 FAIL=0 UNPORTED=0 DESC_CHECKED=1; `validate_blit` 17/17 chain + 24/24 blits; `validate_composite` 54152 @ 53858 baseline; `validate_player` PASS; `validate_physics` PASS=16584 FAIL=0 UNPORTED=624; `validate_items` PASS=11 FAIL=0 UNPORTED=0; `BUMPY.EXE` links clean (211K, Open Watcom 16-bit DOS).

**Deferred from Phase 4:**

1. **Anim-channel / FX allocator** (`apply_cell_animation` 69aa) — Phase 5.
2. **Sound** (`play_sound`, `play_state_sound`) — Phase 6.
3. **`bgi_overlay.c` sub-handlers 3–6** (masked copy variants) — deferred from Phase 0; unchanged.
4. **0x85c cell-move handler table end-to-end validation** — only `state-2 → cell_move_down` is capture-validated; the remaining handlers are transcribed from the decomp (per-fn differential passes) but lack independent capture validation.
5. **int8-synced end-to-end gate** — the Unicorn capture granularity does not match the engine's physics-frame rate; a frame-accurate capture (DOSBox path) is needed before the full game loop can be replay-validated tick-for-tick. Deferred (unchanged from Phase 2/3).

## Phase-5 module audit (anim-channel FX subsystem — `src/anim.c`)

The complete anim-channel FX subsystem — the channel-A slot allocator, the A/B
stream steppers, and the A/B overlay draw/erase — ported 1:1 from the live Ghidra
decomp. Validation splits along the same line as the rest of `src/`: the allocator +
steppers are gated at the **semantic-state** level (12-byte channel records + DGROUP
scalars); the draw/erase functions are gated at the **view-descriptor** level over the
already-plane-exact Phase-0 blitter (the BGI-overlay leaves stay faithful-signature
stubs). The shared channel record is 12 bytes: `[0]`active `[1]`cell `[2..5]`stream-ptr
(far) `[6]`frame-byte `[8..11]`data-ptr (far).

| Module / function set | Fidelity | Notes |
|---|---|---|
| **Allocator** (T3): `apply_cell_animation` (69aa) | Transcription | Channel-A slot allocator. Two-scan free-slot search + the `0xFF`-restart control flow reproduced verbatim; per-action far-ptr tile-def table at DGROUP `0x2ede`/`0x2ee0` (`action*4`); stamps `tilemap[anim_target_cell]`. The A-table has a real **4th `0xFF`-terminator** entry at DGROUP `0x4c64` (3 active records + the terminator; engine-verified vs `BUMPY_unpacked.exe`). Validated at the semantic-state level (3 alloc scenarios + the A-lifecycle). |
| **Steppers** (T3): `step_anim_channels_a` (14e4), `step_anim_channels_b` (15a1) | Transcription | A advances **3** channels, B advances **4**; each non-zero frame byte indexes the per-channel far frame table (A `0x3d6a`/`0x3d6c`, B `0x40a6`/`0x40a8`) for the far data ptr stored into the record's `[+8..+11]`. B is the 4-channel analogue of A (the decomp aliases the loops). Stream-ptr advance + data-ptr store are the captured-delta gate. Working scalars (`g_anim_stream_ptr` 0xa0be etc.) rebuilt at the use site (no hidden engine state). |
| **Draw** (T4): `draw_anim_channels_a` (165e), `draw_anim_channels_b` (17c7) | Transcription | Per active (non-0, non-`0xFF`) slot, A builds two view descriptors + the `p1_sprite` blit descriptor (0x8884 far ptr → 0x792e pointee); B is the layer-B shadow/mask analogue (frame `+0xf1` bias, gridB/posB tables). Validated at the **view-descriptor + p1_sprite-pointee (0x792e) level** over the already-plane-exact blitter — a perturbation-proven real gate (work-buffer `+4` → 28 FAILs). The if/else bodies are ported verbatim. |
| **Erase** (T4): `erase_anim_channels_a` (1a67), `erase_anim_channels_b` (1b2b) | Transcription | Restore each active cell's background via the CLEAR-view + work-buffer-ptr path (B is the layer-B analogue). Same descriptor-level gate as draw. |

**Phase-5 deviations (all in-code RECONSTRUCTION FIDELITY notes present):**

- **Channel-B populator deferred.** `spawn_and_draw_level_entities` (2a78) — the
  level-entity spawn path that *fills* the channel-B slots — is NOT ported (a separate
  entity-spawn concern). Channel B has no allocator of its own, so its step/draw/erase
  are validated against **synthetic, harness-seeded** entry-records: the B function
  *bodies* run 1:1 on the seed and the captured deltas (stream-ptr advance) are
  genuine. This is a coverage caveat, not an integrity gap — analogous to the Phase-4
  direction-seeded cell-move minor (the channel-A path *is* allocator-driven and
  semantic-state validated end-to-end).
- **BGI-overlay leaves stay faithful-signature stubs.** `restore_bg_view`,
  `blit_sprite`, `render_player_view`, `FUN_1000_80ac` — the Phase-0 render core is
  untouched; draw/erase are validated at the descriptor level over that already-
  plane-exact core (see the `bgi_overlay.c` / Phase-0 entries).
- **`draw_anim_channels_b` is gated at the EXIT/final state only.** Its `view1`
  (0x8cc) is written multiple times with leaf calls between (a shadow/mask pre-pass);
  the per-function gate captures the descriptor state at the function boundary, so the
  intermediate pre-pass states are not independently checked. The `if`/`else` is ported
  verbatim — a documented coverage limit, not a deviation in the port.
- **DS-segment expression.** The view descriptors store the engine's *runtime* DS
  (`0x114b`); the harness drives this via a runtime-seg override
  (`ANIM_DGROUP_RUNTIME_SEG`), while the source default is the decomp's static-image
  link-time literal (`0x203b`). The gate compares genuine engine-captured EXIT bytes —
  proven real by removing the override (the gate then fails). The far-ptr SEG *data*
  halves stay the static `0x203b`; only the DS *register* store is the runtime value.
- **FX sound callees → Phase 6** (`play_sound` / `play_state_sound` 6e11/647e).

**Phase-5 validation method:** per-function semantic-state differential (allocator +
steppers: seed entry snapshot → call the reconstructed C fn → assert the 12-byte
channel records + DGROUP scalars vs the engine's exit snapshot) **plus** a
view-descriptor + `p1_sprite`-pointee differential for draw/erase (assert the descriptor
bytes the engine actually wrote, over the plane-exact blitter). Engine ground truth
captured by `tools/anim_oracle.py` (Unicorn instrumentation); 6 scenarios, 46 records.
**Gate: `validate_anim` PASS=45 FAIL=0 UNPORTED=1 DESC_CHECKED=28** (the 1 UNPORTED is
a single skeleton-coverage record, not a port gap; the descriptor gate is perturbation-
proven). The B path's step/draw/erase run on synthetic-seeded records (B-populator
deferred); the channel-A path is allocator-driven end-to-end.

## Phase-5 status (anim-channel FX subsystem)

As of Phase-5 Task 4 (validate gate re-confirmed Phase-5 Task 5), the complete
anim-channel FX subsystem is **reconstructed and validated**:

- **Reconstructed 1:1**: the channel-A slot allocator (`apply_cell_animation` 69aa),
  both stream steppers (`step_anim_channels_a` 14e4 / `_b` 15a1), and both A/B overlay
  draw + erase paths (`draw_anim_channels_a` 165e / `_b` 17c7,
  `erase_anim_channels_a` 1a67 / `_b` 1b2b) — all ported 1:1 from the live Ghidra
  decomp (verified via MCP + raw disasm). The 12-byte channel record, the A 4th
  `0xFF`-terminator (DGROUP 0x4c64), and the per-channel far tables (0x2ede / 0x3d6a /
  0x40a6) are engine-verified.
- **Validation split**: allocator + steppers at the **semantic-state** level (channel
  records + scalars); draw + erase at the **view-descriptor + p1_sprite-pointee**
  level over the already-plane-exact Phase-0 blitter (perturbation-proven real gate).
  Channel-B step/draw/erase run on **synthetic harness-seeded** entry-records (B has no
  allocator) — the bodies run 1:1 and the deltas are genuine; the channel-A path is
  allocator-driven and validated end-to-end.
- **Gate re-confirmed (2026-06-20)**: `validate_anim` PASS=45 FAIL=0 UNPORTED=1
  DESC_CHECKED=28. No-regression: `validate_blit` 17/17 anim + 17/17 chain + 24/24
  blits; `validate_composite` 54152 @ 53858 baseline; `validate_player` PASS;
  `validate_physics` PASS=16584 FAIL=0 UNPORTED=624; `validate_items` PASS=11 FAIL=0
  UNPORTED=0; `validate_p2` PASS=74 FAIL=0 UNPORTED=0 DESC_CHECKED=1; `BUMPY.EXE` links
  clean (212K, Open Watcom 16-bit DOS, `anim.obj` now in the link set).

**Deferred from Phase 5:**

1. **Channel-B populator / level-entity spawn** (`spawn_and_draw_level_entities` 2a78)
   — the path that fills the channel-B slots; B's step/draw/erase are validated on
   synthetic seeds until it lands. A separate entity-spawn concern.
2. **Sound** (`play_sound`, `play_state_sound` 6e11/647e) — extern stubs → Phase 6.
3. **BGI-overlay leaves / `bgi_overlay.c` sub-handlers 3–6** — Phase-0 render core
   untouched; draw/erase validated at the descriptor level over it. Unchanged.
4. **int8-synced end-to-end gate** — the Unicorn capture granularity does not match the
   engine's physics-frame rate; a frame-accurate capture (DOSBox path) is needed before
   the full game loop can be replay-validated tick-for-tick. Deferred (unchanged from
   Phase 2/3/4).

## Phase-6 module audit (sound subsystem — `src/sound.c`)

The complete sound subsystem — L1 effect dispatch, L2 device state machine, L3 tone
submit + timer-table management, L4 hardware drivers, and the L5 PIT ISR tone-sequencer
— ported 1:1 from the live Ghidra decomp + raw disassembly. Validation splits by layer:
L1–L3 are gated at the **semantic-state / data** level (the device/driver scalars, the
10-word tone param frame `DAT_1000_9788..979a`, the installed far-cb ptr, and the two L3
timer tables); L4 is gated by a **port-write-sequence** differential (the engine's real
`OUT(port,val,size)` trace captured under Unicorn, replayed and diffed byte-for-byte);
L5 is reconstructed 1:1 as **documentation** (NOT runtime-gated — see below).

| Module / function set | Fidelity | Notes |
|---|---|---|
| **L1 dispatch + L3 tone-submit** (T3): `play_sound` (6e11), `play_sound_effect` (6e30, 21-case effect→tone switch), `schedule_timer_callback_a/b/c` (9488/9502/956d) | Transcription | The effect→frame pipeline. `play_sound_effect`'s 21-case switch ported VERBATIM (cases not collapsed; the `LAB_70d6` shared tail preserved). The schedulers fill the 10-word param frame (`DAT_1000_9788..979a`) + install the far cb ptr (0x9631/0x96c4/0x95b5 : seg 0x1000), then tail into `set_timer_slot_raw`. The static-image install seg literal 0x1000 is the load-base; the host harness relocates it (0x1000→0x110-class runtime CODE seg) for the captured exit, the source keeps the faithful image literal. Validated at the **semantic-state** level (entry SNAP→call→exit SNAP). |
| **L1 event wrappers** (T4): `play_action_sound` (63be), `play_contact_sound` (640c), `play_exit_sound` (6305), `play_pickup_sound` (645d), `play_event_sound_64c1` (64c1), `play_state_sound_79b9` (647e) | Transcription | Each reads a per-device byte LUT (OPL table when `sound_device_state==4`, else std) indexed by an event/state global → sound id → `play_sound`. The six 0x30-byte LUTs (DGROUP 0x260e/0x263e/0x26ce/0x26fe/0x276e/0x278e) dumped byte-exact from the unpacked image. Semantic-state validated. |
| **L2 device state** (T4): `sound_select_device` (6de3), `snddrv_init` (88e5), `select_sound_device_from_mask` (891e), `snddrv_dispatch_a-d` (85b5/85db/8600/8626), `snd_busy_delay` (872e) | Transcription | The device init/select state machine (`sound_init_state` 0→1→2; `snddrv_mode` 0x85b3; `sound_active_device_mask` 0x5586). The dead `if(!substep_ok)` failure arms in `snddrv_init` preserved 1:1. Semantic-state validated. |
| **L3 timer-table mgmt** (T4): `set_timer_slot(_raw)` (7de8/7df9), `arm_timer_callback` (7f2b), `disable_timer_callback` (7f65), `get_timer_slot_field` (7e3d), `timer_restore` (7fde) | Transcription | Two tables: the 0x5516 cb table (`arm`/`disable`, 8-byte slot `{current@0, reload@2, cb_off@4, cb_seg@6}`) and the 0x549c slot table (`set`/`get`, `{value@0, 0@2, cb_seg@4, cb_off@6}` at `(channel+2)*8`). Slot-store register semantics recovered from the asm. Semantic-state + TABLE-diff validated. |
| **L4 hardware backends** (T5): `pc_speaker_silence` (9115), `speaker_gate_reset/strobe` (9440/9451), `record_status_and_strobe_speaker` (946e), `opl_write_reg` (9007), `opl_play_note` (905d), `FUN_89e2` (89e2, MPU byte-out), `FUN_8a07` (8a07, MPU sample), `FUN_8ad0` (8ad0, MPU settle), `FUN_8e2f` (8e2f, OPL all-off) | Transcription | The engine's real port-I/O drivers (PC-speaker/PIT 0x61, MPU-401 0x330/0x331, OPL2 0x388/0x389). Validated by the **port-write-sequence** differential: capture the engine's `OUT(port,val,size)` under Unicorn, replay the recorded IN sequence into the host `in()` shim, run the reconstructed driver, diff its OUT capture vs the engine's — a perturbation-proven real gate (corrupting any emitted OUT FAILs). |
| **L5 ISR tone-sequencer** (T6): `pit_timer_isr_multiplexer` (7c02), `tone_seq_callback_9631` (9631), `tone_seq_callback_96c4` (96c4), `tone_seq_callback_95b5` (95b5) | **Behavior-faithful (NOT runtime-gated)** | The PIT (IRQ0/int-8) multiplexer walks the 0x5516 cb table each tick (`current += reload`; on the 500-tick period far-call the slot's installed callback) and the three tone-sequencer callbacks advance the L3 param frame + reprogram PIT ch2 (0x42/0x43) / strobe the speaker gate (0x61) to sweep the tone. Reached ONLY via the installed far pointer — NO Ghidra function boundary, NOT hooked by the oracle, so the trace has ZERO records and there is no host differential. Transcribed verbatim from the raw disasm as documentation; the **self-modifying-BGI-blitter precedent** (`sprite_blit`/`bg_render`): faithful to what the binary does, validated by inspection vs the asm, not by a runtime gate. |

**Phase-6 deviations (all accurate; stated plainly; in-code RECONSTRUCTION FIDELITY notes present):**

- **(a) L5 async per-PIT-tick sweep not host-replayable.** The L5 sequencer is driven by
  hardware interrupts mutating the param frame out from under the foreground game loop;
  this async sweep is not reproducible as a deterministic differential. Ported 1:1 for
  documentation/faithfulness; **not runtime-gated** (the blitter precedent).
- **(b) OPL note-program exclusion.** `opl_play_note` (905d) + `FUN_8e2f` (8e2f, which
  drives it) read the per-note F-number / per-channel block tables (DGROUP 0x5593 / 0x559c
  / 0x55b4 / 0x5614) that an OPL-init routine populates at **runtime** (the static image
  leaves them zero / BSS). The Phase-6 T1 capture does NOT serialize those tables, so the
  exact note OUTs are not host-reproducible from the trace. Both are ported **1:1** for the
  link + faithfulness, and registered **UNPORTED** in the port-write gate (a documented
  exclusion — not a port gap).
- **(c) Recovered value byte vs genuinely-gated port/order/size/count.** For `FUN_89e2`
  (89e2) / `FUN_8a07` (8a07) / `opl_write_reg` (9007), the *value byte* written arrives in a
  CPU register/arg the SND_SNAP does not serialize; the host harness recovers it from the
  captured OUT event and stages it (so that byte is self-consistent for that record). The
  **port, order, size, and count** of the writes are genuinely gated — a wrong/extra/missing
  write at the wrong port diverges and FAILs.
- **(d) `play_state_sound` gameplay records UNPORTED.** `play_state_sound_79b9` (647e) tail-
  calls `p1_try_trigger_pending_action` (654e, player.c) whose captured exit on the gameplay
  path is dominated by a cross-module player/anim tail the sound harness cannot link. Those
  records are reported **UNPORTED** on the gameplay path; the function is frame-validated via
  4 SEEDED inert-tail records (scenario 6).
- **(e) Status ports read fixed synthesized IN values.** There is no PC-speaker / MPU /
  status hardware under Unicorn, so each `IN AL,0x61` / `IN AL,0x331` returns a fixed,
  run-deterministic byte (0x61→0xFF, 0x331→0x00 DSR-clear). What is validated is not the
  absolute byte but the engine's bit manipulation over the **replayed** IN sequence (the
  L4 gate replays the exact IN bytes the engine saw).
- **(f) Already-reconstructed callers stay in `player.c`.** `do_move_with_sound` /
  `p1_move_step_with_sound` (the move-step paths that call `play_sound`) were reconstructed
  in `player.c` in Phase 2; they are not re-ported here.
- **(g) The flags-carry convention modeled as a scalar.** `schedule_timer_callback_a/b/c`
  in the original read the entry CPU FLAGS (PUSHF) and pass the packed flags word to the
  no-op `record_min_status_code`, then fill the frame only `if (!in_CF)`. The host has no
  incoming CPU carry; it is modeled with a file-scope `snd_sched_carry_in` (default 0 =
  carry clear, reproducing the captured frame-fills) rather than a literal parameter (a true
  param would perturb the 21-case switch's verbatim call sites). The `record_min_status_code`
  arg is given the available value (`param_1`) in lieu of reconstructing the host-absent
  packed-FLAGS register — observationally identical (the callee is a no-op stub).

**Phase-6 validation method:** per-function differential with two comparators (the L1–L3
**semantic-state** gate: seed entry SND_SNAP → call the reconstructed C fn → assert the
device/driver scalars + 10-word tone param frame + installed far-cb ptr (+ the two L3
timer tables for the timer-mgmt fns) vs the engine exit SNAP; and the L4
**port-write-sequence** gate: prime the host `in()` shim with the record's recorded IN
sequence, clear the `out()` capture, run the driver, assert the OUT capture == the engine's
captured OUT events). Engine ground truth captured by `tools/sound_oracle.py` (Unicorn
instrumentation, port-I/O scoped to the L4 driver windows + the sound-hardware port set);
8 scenarios, 4439 records, 23581 port-I/O events. **Gate: `validate_sound` FAIL=0
PORT_CHECKED=3752 UNPORTED=25** (the 25 UNPORTED = the OPL note-program exclusion
`opl_play_note`/`FUN_8e2f` + the `play_state_sound` gameplay records; the L5 ISR adds no
trace records). The L5 sequencer is reconstructed as documentation (no gate).

## Phase-6 status (sound subsystem)

As of Phase-6 Task 6, the complete sound subsystem is **reconstructed**:

- **Reconstructed 1:1** (live Ghidra decomp + raw disasm, verified via MCP): L1 dispatch
  (`play_sound` 6e11, `play_sound_effect` 6e30 + the 6 event wrappers), L2 device state
  (`sound_select_device` 6de3, `snddrv_init` 88e5, `select_sound_device_from_mask` 891e,
  `snddrv_dispatch_a-d`, `snd_busy_delay`), L3 tone-submit + timer-table mgmt
  (`schedule_timer_callback_a/b/c` 9488/9502/956d, `arm`/`set`/`get`/`disable`/`restore`),
  L4 hardware drivers (`pc_speaker_silence` 9115, `speaker_gate_*`, `opl_write_reg` 9007,
  `opl_play_note` 905d, `FUN_89e2`/`8a07`/`8ad0`/`8e2f`), and the **L5 PIT ISR
  tone-sequencer** (`pit_timer_isr_multiplexer` 7c02, `tone_seq_callback_9631`/`96c4`/`95b5`).
- **Validation split**: L1–L3 at the **semantic-state / data** level; L4 by the
  **port-write-sequence** differential (perturbation-proven real gate); **L5 documented**
  (reconstructed 1:1 from the disasm, NOT runtime-gated — the blitter precedent; reached
  only via the installed far pointer, no trace records).
- **Gate re-confirmed (2026-06-20)**: `validate_sound` FAIL=0 PORT_CHECKED=3752 UNPORTED=25
  (= the OPL note-program exclusion + the `play_state_sound` gameplay records). No-regression:
  `validate_blit` 17/17 anim + 17/17 chain + 24/24 blits; `validate_composite` 54152 @ 53858
  baseline; `validate_player` PASS; `validate_physics` PASS=16584 FAIL=0 UNPORTED=624;
  `validate_items` PASS=11 FAIL=0 UNPORTED=0; `validate_p2` PASS=74 FAIL=0 UNPORTED=0
  DESC_CHECKED=1; `validate_anim` PASS=45 FAIL=0 UNPORTED=1 DESC_CHECKED=28; `BUMPY.EXE`
  links clean (216K, Open Watcom 16-bit DOS, `sound.obj` in the link set, no duplicate
  symbols).

**Deferred from Phase 6:** none specific to sound — the complete subsystem
(dispatch + device + tone-engine + hardware drivers + ISR sequencer) is reconstructed and
validated (L1–L4) / documented (L5). The **int8-synced end-to-end gate** remains the single
project-wide deferral (the Unicorn capture granularity does not match the engine's
physics-frame rate; a frame-accurate DOSBox-path capture is needed before the full game
loop can be replay-validated tick-for-tick — unchanged from Phases 2/3/4/5).

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
