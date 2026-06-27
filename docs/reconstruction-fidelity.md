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
| `game.c` | Transcription (spine) | The four session/loop functions ported 1:1 from the decomp (cross-checked live via Ghidra MCP): `run_game_session` (1000:0258), `init_game_session_state` (1000:0282), `reset_game_state` (1000:0bf9), `game_loop` (1000:0c18). Control flow, comparisons and call order reproduced verbatim. Owns the cross-module session/round/tick control globals (`round_continue_flag` 0x9d30, `session_continue_flag` 0x856d, `frame_abort_flag` 0x928d), the `tilemap` far pointer (0xa0d8 — no single module home), and the menu/score/level-index scratch. **Deviations** (all noted in-code): (a) the per-function Turbo-C `stack_check_limit`/`FUN_ab83` stack-check prologue is CRT scaffolding, omitted (OW CRT does its own); (b) `init_game_session_state`'s VGA mode set is surfaced via `video_set_mode_0d()` so the boot harness has an observable mode 0x0D (engine sets it inside the FUN_9821 CRTC block); (c) the ~46 trailing UNNAMED `DAT_203b_xxxx` resets in `init_game_session_state` are collapsed into the documented `reset_opaque_session_globals()` stub rather than invented as named externs. As of Phase-9 T3/T4 `game.c` also owns the reconstructed `init_view_anim_descriptors` / `game_post_present` / `game_post_input` per-tick spine fns, and `game_loop` drives the real P1 path (the carve-out boundary is gated by `validate_integration.sh`); see the Phase-9 sections. |
| `game_stubs.c` | **Genuine carve-outs (Phase-9 T4)** | NOT a reconstruction. Faithful-SIGNATURE no-op/benign-default stubs for the engine functions the reconstructed call graph references but that are deliberately NOT reconstructed — every remaining symbol is a documented CARVE-OUT, in the same spirit as the self-modifying BGI-overlay blitters and the L5 timer ISR. As of Phase-9 T4 the file holds **58 carve-out function symbols + `mode_script_tbl`**, all in one of these classes: HARDWARE-INIT (the `init_game_session_state` CRTC/audio/resource/IRQ block), CRTC PAGE-FLIP (`present_frame`), int8-TIMING (`run_n_frames`/`rotate_timing_flags_and_wait`/`wait_keypress`), ENGINE STANDALONE LOADER (`load_current_level_data` — slice loads via `level.c start_level`), NEVER-DECOMPILED (`reset_round_counters` = `init_round_state` 0x31de; Ghidra-labelled but decompilation fails), RENDER-CORE LEAVES (`init_sprite_structs`, `init_fullscreen_view_desc`, `setup_fullscreen_view`, `apply_level_palette`, `show_text_screen`, `show_pause_screen`, `fun_75a2_poll_action`), OUT-OF-SCOPE SOUND L4/L6 drivers + player handler-table targets + the P2 indirect-call backend, and the `dos_abort` CRT thunk. Through Phases 2–8 most of the original deferral list was un-stubbed into real module bodies (player/player2/anim/screens/sound/spawn/level); what remains is carve-out-only. **NAMING:** symbols still spelled `FUN_1000_<off>` (e.g. `FUN_1000_6183` = `sweep_active_entities`) have canonical Ghidra labels but are kept `FUN_`-spelled to mark "unported carve-out, link-only" and to avoid churning the validated call sites that reference them; the canonical name is cited in-code. The carve-out boundary (which `game_loop` callees are genuine hardware/CRT/int8-timing leaves vs reconstructed game logic) is ENFORCED by `tools/validate_integration.sh`: it links `BUMPY.EXE` (no dup symbols), asserts every `game_loop` per-tick callee resolves to its real module `.obj` (not `game_stubs.obj`), and asserts `game_stubs.obj`'s symbol set ⊆ the explicit carve-out allowlist; a built-in `--self-test` proves the gate fails when a spine callee regresses to a stub. |
| `level.c` | Transcription (compile/link) | `start_level` (1000:2d14) level-1 path wired to the validated Phase-0 render API; see prior audit. Owns `current_level` (0x79b2), `copyprotect_flag`, `p1_start_x/y`, `current_entity_index`. **No ownership edits were needed in Task 7** — its globals were already cleanly owned. |
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
- `p1_movement_dispatch` (1e02) call-through — this Phase-1 dispatcher tail-calls `dispatch_move_step` with the raw engine near-offset from `game_mode_handlers`; the host could not call through that table (engine offsets, not host fn-ptrs), so the harness skipped these 624 records (`UNPORTED`). **RESOLVED in Phase-9 T2:** `move_step_handler_for_offset` makes `dispatch_move_step` host-safe, so `p1_movement_dispatch` is now called for real and all 624 records pass (see the Phase-9 T2 section).

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
5. **`p1_movement_dispatch` call-through host model** — **RESOLVED in Phase 9 (T2).** The dispatcher's handler table (`move_step_dispatch_tbl`, DGROUP 0x43c0) holds raw engine near-offsets; a host-safe call-through model was needed before end-to-end host replay could drive the full game-mode handler chain. Phase-9 T2 added `move_step_handler_for_offset` (a byte-table-unchanged offset→host-fn resolver), making `dispatch_move_step` / `p1_movement_dispatch` host-callable; the 624 `UNPORTED` records converted to PASS (`validate_physics` UNPORTED 624→0). See the Phase-9 T2 section.
6. **int8-synced end-to-end gate** — the Unicorn capture granularity does not match the engine's physics-frame rate. A frame-accurate capture (DOSBox path) is needed before the full game-loop can be replay-validated tick-for-tick. Deferred (still open after Phase 9).

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

- **Channel-B populator — CLOSED in Phase 8.** Channel B has no allocator of its own;
  its real populator is `spawn_and_draw_level_entities` (2a78), deferred at Phase 5. It
  was reconstructed 1:1 + validated in Phase 8 (see the Phase-8 section). The Phase-8 T3
  re-validation runs the channel-B step/draw/erase against records **populated by the
  real `spawn_and_draw_level_entities`** on a B-firing level (level 2): the oracle
  (re)loads the level, runs the real orchestrator to stamp the B slot record's cell +
  frame-data ptr from the level's layer-B grid, re-applies the engine's own
  `active=1`/`frame=1` (spawn step-1 values), and then steps/draws/erases. The earlier
  fully-synthetic seed is gone. **Documented residual:** the one B-record field the spawn
  path never writes — the per-tick step-B *stream* ptr (rec bytes [2..5]; confirmed 0 in
  both the 2a78 decomp and the spawn_oracle level-2 capture `001e…0002001700`) — remains
  harness-supplied; the cell + frame-data ptr are engine-real.
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
proven). As of Phase 8 the B path's step/draw/erase run on records populated by the real
`spawn_and_draw_level_entities` (level 2) — no longer synthetic seeds (caveat CLOSED; see
Phase-8 section); the channel-A path is allocator-driven end-to-end.

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
  Channel-B step/draw/erase: validated against records populated by the real
  `spawn_and_draw_level_entities` as of Phase 8 (CLOSED — see Phase-8 section; was
  synthetic-seeded while the populator was deferred); the channel-A path is
  allocator-driven and validated end-to-end.
- **Gate re-confirmed (2026-06-20)**: `validate_anim` PASS=45 FAIL=0 UNPORTED=1
  DESC_CHECKED=28. No-regression: `validate_blit` 17/17 anim + 17/17 chain + 24/24
  blits; `validate_composite` 54152 @ 53858 baseline; `validate_player` PASS;
  `validate_physics` PASS=16584 FAIL=0 UNPORTED=624; `validate_items` PASS=11 FAIL=0
  UNPORTED=0; `validate_p2` PASS=74 FAIL=0 UNPORTED=0 DESC_CHECKED=1; `BUMPY.EXE` links
  clean (212K, Open Watcom 16-bit DOS, `anim.obj` now in the link set).

**Deferred from Phase 5:**

1. **Channel-B populator / level-entity spawn** (`spawn_and_draw_level_entities` 2a78)
   — **DONE in Phase 8** (`src/spawn.c`; see Phase-8 section). B's step/draw/erase are
   now validated against records the real populator stamps, not synthetic seeds.
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
validated (L1–L4) / documented (L5). The **int8-synced end-to-end gate** remains the standing
project-wide deferral (the Unicorn capture granularity does not match the engine's
physics-frame rate; a frame-accurate DOSBox-path capture is needed before the full game
loop can be replay-validated tick-for-tick — unchanged from Phases 2/3/4/5). (At the time of
Phase 6 this was the *single* project-wide deferral, alongside the then-open
`p1_movement_dispatch` call-through; that call-through was **RESOLVED in Phase 9** — see the
Phase-9 sections — leaving the int8 end-to-end gate and the separate CODES.EXE registration
RE as the remaining deferrals.)

## Phase-7 module audit (front-end + in-game HUD — `src/screens.c`)

The complete front-end subsystem — text/number formatters + in-game HUD, the
title/menu/transition flow, the VGA-DAC palette upload, and the highscore + level-intro
screens — ported 1:1 from the live Ghidra decomp + raw disassembly. Validation splits by
comparator: the **semantic-state differential** gates the menu/title/highscore/intro
state machines + the number format (seed entry SCRSNAP → call → assert the screen
globals + AX return == exit SNAP); the **descriptor-level differential** gates the
text/number/HUD builders + the screen draws (assert the bytes written into the host
view-struct / `p1_sprite` descriptors == the captured exit descriptors); the
**port-write-sequence differential** drives the reconstructed VGA-DAC writer over a
seeded palette and asserts its `(port,value)` sequence. Gate: **`validate_screen_fns`
PASS=884 FAIL=0 UNPORTED=0** (DESC_CHECKED=41, PORT_CHECKED=837 over 500 port-I/O events;
note the `PORT_CHECKED` metric also counts the 0-emission consistency checks — see
deviation (a)).

| Module / function set | Fidelity | Notes |
|---|---|---|
| **Text/number + HUD** (T3): `draw_text_at` (07f0), `draw_number` (0816), `draw_number_sprites` (603d), `draw_hud_composite` (51d8) | Transcription | `draw_number` formats a native u32 in decimal by repeated ÷10/×10 modeling the engine's `crt_uldiv_32`/`crt_lmul_32` runtime calls, emits `"OVER FLOW"` when `width>=8`, then tail-calls `draw_text_at`. `draw_hud_composite` builds the 7-fill HUD via per-fill descriptors. **Descriptor-level gated** (`draw_hud_composite`'s 7 fills are checked per-fill against the oracle's hooked-leaf capture — perturbation-proven) + **number-format gated** (the formatted-buffer bytes for `draw_number`). The formatter scratch lives in the module-static `formatted_number_buf` (storage-class deviation — see (d)). |
| **Title + menu + transition** (T4): `init_title_graphics` (2ef8), `show_title_background` (2fac), `show_title_and_init` (3ed4), `run_main_menu` (35a5), `show_menu_select_screen` (0f7a), `play_iris_wipe_transition` (3467) | Transcription | `run_main_menu` is the **4-option cursor state machine** (cursor up/down guards, option-2 cycles `menu_option2_setting` 0→1→2→0 instead of returning) — **semantic-state gated** via captured-input replay (perturbation-proven). `show_title_background`/`init_title_graphics`/`show_title_and_init` build the title + run the iris transition; `play_iris_wipe_transition`'s rectangle-wipe DESCRIPTOR stepping is reconstructed 1:1 and descriptor-gated (its render/DAC leaves are stubbed — see (e)). `show_menu_select_screen` is **position-only descriptor gated** — see (b). |
| **Vsync wait + DAC writer** (T4): `wait_vretrace_thunk` (9864) / `wait_vretrace_dispatch` (2036:0000) + the reconstructed `vga_dac_upload_from_buffer` | **Behavior-faithful (vsync wait dispatch)** + **Behavior-faithful (static DAC writer)** | ⚠️ *Misnomer corrected 2026-06-27 (Task 2):* `upload_vga_dac_palette` / `dispatch_by_palette_mode_2036` were WRONG NAMES — this is a **vsync WAIT**, not a DAC upload. `wait_vretrace_thunk` (`1000:9864`) CALLFs `wait_vretrace_dispatch` (`2036:0000`), which indirect-calls the overlay table `[palette_mode*2 + 0x6976]`; for VGA (`pm==2`) the handler (`2036:0015`) polls Input Status #1 (`0x3da` bit 3) to wait for vertical retrace. The iris wipe calls it 4×/step as the wipe PACING. The genuine DAC upload is `7bca` (`host_bgi_upload_palette_to_dac`). The reconstructed `vga_dac_upload_from_buffer` is the mode-2 VGA-DAC writer, reconstructed from the raw disassembly of the static DAC writer (function entry image off `0xb204`; the DAC `out` block begins at `0xb214`): it reads the 16-colour 6-bit palette from the decoded-image buffer at `+0x33` and emits the canonical VGA-DAC sequence (`out 0x3c8,0`; 8×RGB→0x3c9; `out 0x3c8,0x10`; 8×RGB). **This static writer IS the DAC port-write gate** (driven standalone over a seeded palette, perturbation-proven). The DAC carve-out is deviation (a). |
| **Highscore + level-intro** (T5): `show_highscore_screen` (5681), `render_highscore_table` (57e1), `highscore_enter_name` (59d3), `enter_highscore_name` (5c87), `draw_name_entry_cursor` (5fdb), `level_intro_screen` (3852), `show_level_intro_screen` (0d9d) | Transcription | The highscore display/name-entry flow + the level-intro screen. `enter_highscore_name` is the per-letter name-entry state machine (the `4=prev`/`8=next` cursor paths ported from RAW ASM — the Ghidra decompiler under-rendered them; see (f)); `draw_name_entry_cursor` is the shared cursor-draw helper. **Semantic-state gated** (the name row + the table-match AX return) + **descriptor gated** (the table/intro draws). `show_level_intro_screen` is **position-only descriptor gated** — see (b). The name buffer lives in the module-static `enter_name_buf` (storage-class deviation — see (d)). |

**Phase-7 deviations (all accurate; stated plainly; in-code RECONSTRUCTION FIDELITY notes present):**

- **(a) DAC carve-out — the runtime DAC path is NOT engine-port-trace-validated.** The
  engine's RUNTIME DAC writes come from runtime-loaded **BGI-overlay code** that does NOT
  execute under Unicorn (the `[palette_mode*2 + 0x6976]` handler table is runtime-populated
  by BGI driver init; under the natural boot `palette_mode==2` the standalone-vsync records
  carry **0 captured DAC** — an engine fact surfaced by the T1 oracle). So
  `wait_vretrace_thunk`'s records carry **0 OUT events** and are gated as a **no-emission
  consistency check** (NOT a DAC validation; ⚠️ the old name was `upload_vga_dac_palette`
  — misnomer corrected 2026-06-27 by Task 2; the thunk is a vsync WAIT, not a DAC upload); the `PORT_CHECKED` metric counts these
  0-emission checks too. The **REAL** DAC gate is the reconstructed `vga_dac_upload_from_buffer`
  (raw disasm of the static writer, function entry image off `0xb204` / DAC `out` block at `0xb214`, palette `@+0x33`, verified
  byte-for-byte) run STANDALONE over a SEEDED palette and asserted vs the canonical VGA-DAC
  hardware protocol sequence (an external standard, perturbation-proven). Plainly: the engine's
  runtime DAC path is **not** port-trace-validated against the engine; the **static writer IS**
  gated 1:1 + protocol-correct. (The 50-write DAC sequence the T1 oracle captured comes from the
  iris-wipe's per-step DAC upload — BGI overlay `1ab9:0677` dispatched via thunk `fun_7bca`
  (`bgi_upload_palette_to_dac_dispatch`) — over its faded palette state, NOT reconstructable 1:1
  from the static image (`1ab9:0677` is runtime-loaded BGI overlay code). ⚠️ *Misnomer corrected
  2026-06-27*: the old text attributed these writes to "`FUN_7b4a`" — that is `bgi_init_viewport`
  (clip/viewport, null VGA blit slot) which emits NO DAC writes. The iris-wipe's descriptor RECT
  SWEEP is the faithfully-reconstructed, validated part.) This is the self-modifying-blitter / L5-ISR class
  carve-out (faithful to what the binary does, validated by inspection vs the asm + protocol,
  not by an engine-runtime port differential).
- **(b) Partial glyph gates (position-only).** `show_menu_select_screen` (0f7a) and
  `show_level_intro_screen` (0d9d) are **position-only (P)** descriptor-gated: each row-2 glyph's
  **FRAME word** derives from a runtime DGROUP table (the `0x1354` per-level-name table /
  SS-local text fmemcpy'd from DGROUP) that the trace does NOT capture. The **position words ARE
  gated**; the glyph-frame word is not.
- **(c) `enter_highscore_name` name-buffer EDIT validated only indirectly.** Its return value
  (`= table-match`) IS gated, but on the empty-boot capture the table is empty → the return is 0,
  so the per-letter EDIT of the 6-char select buffer is **not byte-checked directly** (an
  empty-boot-capture limit; the edit is validated via the return-path, not the buffer bytes).
- **(d) Storage-class deviations.** `formatted_number_buf` (number formatter scratch) and
  `enter_name_buf` (name-entry buffer) are **module-static** here vs the engine's **SS-local**
  (stack-frame) arrays. A storage-class-only deviation; it is what ENABLES the semantic gates
  (the host observes the buffer as a named global).
- **(e) Resource-load seeded; render-core leaves stay faithful-signature stubs.**
  `open_resource`/`read_chunked` (the INT 21h file-I/O path) stay **stubbed** — no INT 21h in the
  harness; the decoded image buffer is **seeded** from the oracle's REAL engine load+decode. The
  render-core leaves (`FUN_80ac`/`restore_bg_view`/`blit_sprite`/`present_frame`) stay
  **faithful-signature stubs** (Phase-0 untouched; the descriptors are validated OVER them, i.e.
  the builders write into the host view-struct the harness inspects). `wait_keypress` is seeded.
- **(f) `vec_decode_planar` + name-entry RAW-ASM paths.** `vec_decode_planar` is already
  reconstructed (in `level.c` / the `bvec` path), not re-ported here. The name-entry `4=prev` /
  `8=next` cursor paths in `enter_highscore_name` were ported from RAW ASM because the Ghidra
  decompiler under-rendered them.

**Phase-7 validation method:** per-function differential with three comparators (semantic-state
for the menu/title/highscore/intro state machines + the number format; descriptor-level for the
text/number/HUD builders + the screen draws; port-write-sequence for the standalone VGA-DAC
writer). Engine ground truth captured by `tools/screens_oracle.py` (Unicorn instrumentation with
hooked render/leaf capture); 10 scenarios, 884 records. **Gate: `validate_screen_fns` PASS=884
FAIL=0 UNPORTED=0** (DESC_CHECKED=41, PORT_CHECKED=837, 500 port-I/O events). The DAC carve-out
(deviation (a)) is documented, not engine-runtime port-gated.

## Phase-7 status (front-end + in-game HUD)

As of Phase-7 Task 6, the complete front-end subsystem is **reconstructed**:

- **Reconstructed 1:1** (live Ghidra decomp + raw disasm, verified via MCP): the text/number
  formatters + in-game HUD (`draw_text_at` 07f0, `draw_number` 0816, `draw_number_sprites` 603d,
  `draw_hud_composite` 51d8), the title/menu/transition flow (`init_title_graphics` 2ef8,
  `show_title_background` 2fac, `show_title_and_init` 3ed4, `run_main_menu` 35a5,
  `show_menu_select_screen` 0f7a, `play_iris_wipe_transition` 3467), the vsync wait
  (`wait_vretrace_thunk` 9864 / `wait_vretrace_dispatch` 2036:0000 — ⚠️ misnomer corrected
  2026-06-27; was `upload_vga_dac_palette`/`dispatch_by_palette_mode`) + the reconstructed
  VGA-DAC writer (`vga_dac_upload_from_buffer`), and the
  highscore + level-intro screens (`show_highscore_screen` 5681, `render_highscore_table` 57e1,
  `highscore_enter_name` 59d3, `enter_highscore_name` 5c87, `draw_name_entry_cursor` 5fdb,
  `level_intro_screen` 3852, `show_level_intro_screen` 0d9d).
- **Validation split**: **semantic** (the title/menu/highscore/intro STATE MACHINES + the number
  format, perturbation-proven) + **descriptor-level** (the screen/HUD builds — full descriptors,
  or position-only where the glyph-frame word is a runtime DGROUP table — deviation (b)) + **DAC**
  (the static writer is protocol-gated 1:1; the runtime DAC path is a documented carve-out —
  deviation (a), NOT engine-runtime port-trace-validated).
- **Gate re-confirmed (2026-06-21)**: `validate_screen_fns` PASS=884 FAIL=0 UNPORTED=0
  (DESC_CHECKED=41, PORT_CHECKED=837). No-regression across the full gate set: the SEPARATE,
  pre-existing VEC pixel gate `validate_screens` still green (all 5 screens pixel-exact, untouched);
  `validate_blit` 17/17 anim + 17/17 chain + 24/24 blits; `validate_composite` 54152 @ 53858
  baseline; `validate_player` PASS; `validate_physics` PASS=16584 FAIL=0 UNPORTED=624;
  `validate_items` PASS=11 FAIL=0 UNPORTED=0; `validate_p2` PASS=74 FAIL=0 UNPORTED=0
  DESC_CHECKED=1; `validate_anim` PASS=45 FAIL=0 UNPORTED=1 DESC_CHECKED=28; `validate_sound`
  PASS=4414 FAIL=0 UNPORTED=25; `BUMPY.EXE` links clean (Open Watcom 16-bit DOS, `screens.obj`
  in the link set, no duplicate symbols).

**Deferred from Phase 7:** none specific to the front-end — the complete subsystem
(title/menu/transition + highscore + level-intro + in-game HUD/number) is reconstructed and
validated (semantic + descriptor + the static DAC writer). The **int8-synced end-to-end gate**
and the **copy-protection path** (kept behind `#define` OFF, Phase 7b) remain the project-wide
deferrals (unchanged from Phases 2/3/4/5/6).

## Phase-7b module audit (copy-protection challenge — `src/level.c`)

The platform-number copy-protection challenge `copyprotect_challenge` (1000:4015) — the
**only** protection code in `BUMPY.EXE` — reconstructed in `src/level.c` behind
`#define BUMPY_COPY_PROTECTION` (default **OFF** → default build byte-unchanged /
cracked-equivalent). The reconstruction has two layers: the **present routine** (the cracked
build's body, transcribed 1:1 from the live decomp at `4015`) and the **documented un-crack**
(the answer compare the crack removed, recovered from `docs/copy-protection.md`, plus the
collapsed round-state flow inferred). See `docs/copy-protection.md` for the full protection
write-up (including the orphaned password/registration path — `CODES.EXE`/`VS.VSN`/`VGUARD.DAT`,
a deferred separate-binary effort).

| Module / function set | Fidelity | Notes |
|---|---|---|
| **Present routine** (T2): `copyprotect_challenge` (1000:4015) | Transcription | The cracked-build body, ported 1:1 from the live decomp: the two table copies (`sprite_id_tbl` 16 words from DGROUP `0x11b6`, `answer_tbl` 16 bytes from `0x11d6`), the challenge setup + resource-0x90 load, the random sprite-index pick in **2..15** (seeded from the captured LIVE prng state `(0x5192,0,0)` → index 12), the platform-display descriptor (`p1_sprite` x=0x90, y=100, frame=`sprite_id_tbl[12]`), and the +/- input dial. **Present-parts gated** — per-function differential vs the cracked `4015` (table copies, reproduced index, the entered-number trajectory under the 4×-poll-per-action sampling, the display descriptor). |
| **Documented un-crack** (T2): the answer compare + round-state | Reconstruction (documented) | The original input-loop tail `if (entered != answer_tbl[index]) copyprotect_flag = -1;` (PASS leaves the flag at its initial value; mismatch writes `-1`, which the `start_level` hook reads to reset `current_level` to 1). **Recovered from `docs/copy-protection.md`** — this compare and the `-1` fail path do **NOT exist in the cracked binary** (the shipped build sets `copyprotect_flag = 1` unconditionally at `1000:412e`, before input). The collapsed `round_state` flow is **inferred**. **Un-crack-logic gated** — the documented compare (pass-on-match / `-1`-on-mismatch) is perturbation-proven via the descriptor-frame gate, plus the +/- ceiling clamp (saturates at 0x63) that the T1 capture did not exercise. |

**Phase-7b deviations (stated plainly; in-code RECONSTRUCTION FIDELITY note present; NO over-claim):**

- **The un-crack is a documented reconstruction, not a recovered original.** The answer compare
  (`entered != answer_tbl[index] → copyprotect_flag = -1`) and the collapsed `round_state` flow
  are reconstructed from `docs/copy-protection.md`, not transcribed from a protection-active
  binary (no such binary is available; both 1992 copies are the same cracked build). The
  **present-parts** of the routine are 1:1 from the cracked `4015`; the **un-crack** is the
  documented-logic layer, gated by logic check (not by an engine differential against an
  un-cracked original).
- **`+` plus guard saturates at 0x63.** The dial's increment is reconstructed with the engine's
  `CMP 0x63 / JNC` ceiling: the entered number clamps at 0x63 rather than wrapping. (T1 did not
  exercise the ceiling; the clamp is validated by the un-crack-logic comparator.)
- **Input-dial `draw_text_at` inlined as two calls.** The reconstruction emits the dial's
  prompt + number via two `draw_text_at` calls where the decomp shares a single call site;
  effect-identical (same descriptors written), a call-shape-only deviation.
- **The password/registration path is a deferred separate-binary effort.** `CODES.EXE`
  (TinyProg-packed), `VS.VSN` (`YZFA`-encrypted), and `VGUARD.DAT` are NOT reachable from
  BUMPY's code (the `0x119e` password table + the four password strings at `203b:12e7`–`1318`
  have no code xref; BUMPY opens no `.DAT`/`.VSN`/`CODES` file). RE of those files is deferred
  to its own effort — see `docs/copy-protection.md`.

**Phase-7b validation method:** per-function differential split — **present-parts** vs the
cracked `4015` (the table copies, the live-prng index reproduction with seed state `0x5192`, the
entered-number trajectory, the display descriptor) + **un-crack logic** (the documented
pass-on-match / `-1`-on-mismatch compare and the `+` ceiling clamp, perturbation-proven via the
descriptor-frame gate). The challenge compiles in BOTH modes (default OFF → compiled out;
`-dBUMPY_COPY_PROTECTION` → body compiled). **Gate: `validate_copyprot` PASS=36 FAIL=0** (level.c
builds clean in both modes; present-parts + un-crack comparators both FAIL=0).

## Phase-7b status (copy-protection challenge)

As of Phase-7b Task 3, the copy-protection challenge is **reconstructed and documented**:

- **Reconstructed**: `copyprotect_challenge` (1000:4015) — the present routine 1:1 from the
  cracked decomp + the documented un-crack (answer compare + collapsed round-state), behind
  `#define BUMPY_COPY_PROTECTION` (default OFF → default build unchanged / cracked-equivalent).
- **Traced (Part-B, no reconstruction)**: the platform-number challenge is the **only** protection
  code in `BUMPY.EXE`; the password/registration path (`0x119e` table + the four `203b:12e7`–`1318`
  strings) is **orphaned** (no code xref, no file open) and lives in the separate
  `CODES.EXE`/`VS.VSN`/`VGUARD.DAT` files — RE of those is **DEFERRED** to its own effort.
  `"INSERT THE OTHER DISK…"` (`203b:0606`) is the multi-disk disk-swap prompt, unrelated.
- **Gate re-confirmed (2026-06-21)**: `validate_copyprot` PASS=36 FAIL=0 (present-parts + un-crack,
  both modes compile clean). No-regression across the full gate set: `validate_blit` 17/17 anim +
  17/17 chain + 24/24 blits; `validate_composite` 54152 @ 53858 baseline; `validate_player` PASS;
  `validate_physics` PASS=16584 FAIL=0 UNPORTED=624; `validate_items` PASS=11 FAIL=0 UNPORTED=0;
  `validate_p2` PASS=74 FAIL=0 DESC_CHECKED=1; `validate_anim` PASS=45 FAIL=0 UNPORTED=1
  DESC_CHECKED=28; `validate_sound` PASS=4414 FAIL=0 UNPORTED=25; `validate_screen_fns` PASS=884
  FAIL=0 UNPORTED=0; `BUMPY.EXE` links clean (Open Watcom 16-bit DOS).

**Deferred from Phase 7b:** RE of the separate registration binaries (`CODES.EXE` unpack +
`VS.VSN`/`VGUARD.DAT` formats) — its own effort; and the project-wide **int8-synced end-to-end
gate** (unchanged from Phases 2–7).

## Phase-8 module audit (level-load entity spawn — `src/spawn.c`)

The level-load entity-placement orchestrator — the channel-A/B record **populator** + the
layer-C static-sprite blitter + the P1/P2 BUM-header spawn-field reader — ported 1:1 from
the live Ghidra decomp + raw disassembly of `1000:2a78`. It runs once per level load,
placing the level's static entities and seeding the per-frame animation channels. This
**closes the Phase-5 channel-B coverage caveat**: channel B has no allocator of its own, so
its per-tick step/draw/erase were Phase-5-validated on synthetic harness-seeded records
until this populator landed.

| Module / function | Fidelity | Notes |
|---|---|---|
| `spawn_and_draw_level_entities` (2a78) | Transcription | 1:1 mirror of the orchestrator: (1) zero the active byte of the 3 A + 4 B records then activate slot-0 of each channel (`active=1`, `frame=1`, + the two `0x8e8b`/`0x8e8c` mirrors + the `0x8578`/`0x8579` cmd-byte scalars); (2) BUM-header spawn reads (p1_cell / level_exit_cell / items_remaining via `tilemap` 0xa0d8; p2_cell / p2_ai_threshold / p2_move_state / p2_frame_base via `level_src_ptr` 0x75d0); (3) `setup_fullscreen_view`; (4) the 6×8 grid scan (cell = row*8+col) placing layer A / layer B (skip col 7) / layer C per cell; (5) slot-0 deactivate. Control flow, the per-layer `if cv != 0` guards, and the slot-record byte offsets (`+1` cell / `+6` frame / `+8`/`+10` data-ptr) match the asm. |

**Phase-8 deviations (all in-code RECONSTRUCTION FIDELITY notes present in `src/spawn.c`):**

- **Render-core leaves stay module-owned / faithful-signature.** `setup_fullscreen_view`
  (483c), `draw`/`erase_anim_channels_a`/`_b` (anim.c, Phase-5-validated), and `blit_sprite`
  (942a, the BGI-overlay self-modifying leaf, routed via `anim_blit_sprite_leaf`) are NOT
  re-implemented here; spawn.c calls them by name over the already-validated render core.
- **Nested blit inside layer A/B.** `draw_anim_channels_a`/`_b` each NEST a `blit_sprite` per
  active entity (Phase-5 behavior); the host replay harness (`tools/spawn_ctest.c`) separates
  those nested blits from spawn's own layer-C blits via the trace's per-fill layer tag.
- **Slot-table far-ptr split modelled as a typed `__far *[]`.** The decomp CONCATs the
  `0x4c70 off`/`0x4c72 seg` halves to reach slot-0's record; spawn.c reaches it as
  `anim_channels_X_tbl[0]` (off+seg in one far ptr) — the same model anim.c owns.
- **Level data seeded.** The host replay seeds each level's tilemap (layers A/B/C), BUM header,
  and the spawn type/frame tables from the oracle's REAL engine load+`vec_decode` (no INT 21h
  in the harness); the orchestrator's placement logic runs genuinely over that seeded input.

**Phase-8 validation method:** per-function semantic-state + descriptor differential, run over
a **multi-level** capture so channel A+B+C are exercised together (level 1's decoded layer-B is
genuinely empty — only levels 2/3/4/5/6/8/9 fire layer B). The oracle (`tools/spawn_oracle.py`,
Unicorn instrumentation) (re)loads each level, invokes the real orchestrator, and captures its
entry/exit channel-record snapshots + the per-cell leaf descriptors. The host replay
(`tools/spawn_ctest.c`) seeds each level's ENTRY state, calls the reconstructed C fn, and asserts
the populated A/B records + the layer-C blit descriptors == the engine's exit capture; a seeded
layer-A cell perturbation must fail (gate has teeth). **Gate: `validate_spawn` PASS — 9 runs, 7
with layer-B, FAIL=0.**

**Phase-5 channel-B caveat — CLOSED (Phase-8 T3).** With the real populator in place, the
`validate_anim` channel-B step/draw/erase no longer run on a fully synthetic seed:
`tools/anim_oracle.py` scenario 6 (`b_lifecycle_real_spawn`) (re)loads a B-firing level (level
2), runs the **real `spawn_and_draw_level_entities`** to stamp the channel-B slot-0 record's
**cell + frame-data ptr** from the level's real layer-B grid, re-applies the engine's own
`active=1`/`frame=1` (spawn step-1 values), and steps/draws/erases over that engine-populated
record. **Documented residual:** the one B-record field the spawn path never writes — the
per-tick step-B *stream* ptr (rec bytes [2..5]; confirmed 0 in both the 2a78 decomp and the
spawn_oracle level-2 record `001e…0002001700`) — stays harness-supplied; cell + frame-data ptr
are engine-real. `validate_anim` re-confirmed **PASS=45 FAIL=0 UNPORTED=1 DESC_CHECKED=28**
(record count unchanged; scenario 6's 18 B records now operate on engine-populated state).

## Phase-8 status (level-load entity spawn)

As of Phase-8 Task 3 the level-load entity-spawn path is **reconstructed and validated**, and
the Phase-5 channel-B caveat is **closed**:

- **Reconstructed 1:1**: `spawn_and_draw_level_entities` (2a78) in `src/spawn.c` — the
  channel-A/B record populator + layer-C static blitter + BUM-header spawn reader, ported 1:1
  from the live decomp + raw disasm (verified via MCP).
- **Validated**: semantic record-population + descriptor differential across a multi-level trace
  (levels 1–9; layer B exercised on 2/3/4/5/6/8/9) over the already-validated render core; the
  level data is seeded from the engine's real load+decode.
- **Channel-B caveat closed**: `validate_anim`'s B step/draw/erase now validate against records
  populated by the real spawn (see the caveat note above).
- **Gate re-confirmed (2026-06-21)** — all 11 green: `validate_spawn` PASS (9 runs, 7 with
  layer-B, FAIL=0); `validate_anim` PASS=45 FAIL=0 UNPORTED=1 DESC_CHECKED=28; `validate_blit`
  17/17 anim + 17/17 chain + 24/24 blits; `validate_composite` 54152 @ 53858 baseline;
  `validate_player` PASS; `validate_physics` PASS=16584 FAIL=0 UNPORTED=624; `validate_items`
  PASS=11 FAIL=0 UNPORTED=0; `validate_p2` PASS=74 FAIL=0 DESC_CHECKED=1; `validate_sound`
  PASS=4414 FAIL=0 UNPORTED=25; `validate_screen_fns` PASS=884 FAIL=0 UNPORTED=0;
  `validate_copyprot` PASS=36 FAIL=0; `BUMPY.EXE` links clean (Open Watcom 16-bit DOS).

**Deferred from Phase 8:** the project-wide **int8-synced end-to-end gate** (unchanged from
Phases 2–7); the step-B *stream* ptr engine path (the only field the spawn populator never
writes — see the channel-B caveat residual above).

## Phase-9 module audit (P1 contact-action handler family — `src/player.c`)

The Player-1 contact-action handler family — the `move_step_dispatch_tbl` contact-action
micro-handlers + the `apply_contact_action` leaf — ported 1:1 from the live Ghidra decomp
+ raw disassembly of `1000:6a89` and `1000:6832..693a`.  This un-stubs `apply_contact_action`
(a `game_stubs.c` no-op through Phases 2–8) so **every `move_step_dispatch_tbl` target offset
now resolves to a real function** — the prerequisite for the Phase-9 T2 offset→fn resolver.

| Module / function set | Fidelity | Notes |
|---|---|---|
| `apply_contact_action` (6a89) | Transcription | The channel-B slot allocator + contact-sound + tilemap stamp.  Structural twin of `apply_cell_animation` (anim.c 69aa) but on channel B (`anim_channels_b_tbl`, 4 slots + 0xFF terminator), keyed by `p1_cell_prev` (not `anim_target_cell`).  Stores the action code as `last_contact_action` @ DGROUP 0x8566 (the SAME byte the channel-B stepper reuses as `anim_b_loop_idx`; the engine aliases it — written through the anim.c-owned symbol, no new global).  Two-scan free-slot search + 0xFF restart reproduced verbatim; on claim: `slot->cell=p1_cell_prev`, `slot->stream=tile_def[2..5]`, `slot->active=1`, `tilemap[p1_cell_prev+0x30]=tile_def[0]`.  Verified vs disasm 6a89–6bb4. |
| `p1_dispatch_contact_action` (686a) | Transcription | `apply_contact_action(action_table[p1_contact_code])` + `input_state=0`.  Takes the action table as a **far-pointer arg** (engine passes `DS:0x363e`/`0x365e`); the _main/_prev wrappers pass the corresponding dumped LUT. |
| `p1_apply_contact_action_main/_prev` (6832/684b) | Transcription | 1:1 thunks to `p1_dispatch_contact_action` with table 0x363e / 0x365e; _prev first latches `p1_cell_prev = p1_cell`. |
| `p1_apply_contact_action_at_start/_before_end` (6890/68bb) | Transcription | Guarded by `move_step_count != 0` / `!= 7`; apply `contact_action_lut_35fe/361e[p1_contact_code]` (near DGROUP read) + clear `input_state`. |
| `p1_apply_contact_action_tbl_367e/369e` (68fe/693a) | Transcription | Unconditional apply via LUT 0x367e / 0x369e + clear `input_state`. |
| `p1_apply_contact_action_at_start_b/_before_end_b` (68e6/6922) | Transcription | Guarded delegates to `_tbl_367e` / `_tbl_369e`. |

**Phase-9 T1 deviations (all in-code RECONSTRUCTION FIDELITY notes present):**

- **`last_contact_action` aliases `anim_b_loop_idx` (DGROUP 0x8566).** The engine reuses one
  DGROUP byte for the contact action latch (apply_contact_action) and the channel-B step loop
  index (step_anim_channels_b; the decomp itself labels it `last_contact_action` there).  The
  store is written through the anim.c-owned `anim_b_loop_idx` symbol — one owner per global, no
  duplicate.
- **Dispatch LUTs / sound LUTs / tiledef table dumped byte-exact** (six 0x30-byte action LUTs
  0x35fe..0x369e; two 0x30-byte contact-sound LUTs 0x272e/0x274e; the action*4 tile-def far-ptr
  table 0x3256) from `BUMPY_unpacked.exe` (DGROUP file base 0x11440), modeled as raw byte blobs
  (same idiom as `tile_followup_action_lut` / `anim_a_tiledef_tbl`); far ptrs rebuilt with
  `MK_FP` at the use site.  The tiledef seg halves are the static link-time DGROUP seg 0x103b
  (host-seeded/relocated for the gate, as with anim.c's far-ptr tables).
- **Channel-B table grew a 0xFF terminator.** `anim_channels_b_tbl` is now `[ANIM_B_SLOTS+1]`
  (4 slots + a 0xFF-terminator record, engine-verified at DGROUP 0x4cb0) — the channel-B
  analogue of the A table's terminator.  `apply_contact_action`'s unbounded scan and the
  draw/erase-B `while active != 0xFF` loops require it; the 5th entry was dormant while channel
  B had no allocator (Phase 5).  A faithful correction (the engine's B table genuinely has it),
  not a deviation.
- **`play_sound` (6e11)** is the Phase-6 leaf (already linked from sound.c).

**Phase-9 T1 validation method:** the contact handlers are not in the physics 16-byte SNAP, so
they are gated by a **self-contained differential** added to the physics harness
(`tools/physics_ctest.c` → `run_contact_family`, gated by `tools/validate_physics.sh`).  Ground
truth is the engine's own byte-exact tables read **directly from `BUMPY_unpacked.exe`** (NOT from
the reconstruction's arrays) + the engine-verified channel-B slot-allocator semantics; the
reconstruction's LUTs/tiledef are asserted byte-equal to the image, then `apply_contact_action`
+ the four dispatch handlers are driven over a sweep (contact codes 0..0x17, both devices, both
move-step guard branches) and their effects (last_contact_action, device-selected sound, claimed
slot cell/active/stream ptr, tilemap stamp, resolved action, `input_state=0`) asserted against an
independent recomputation from the engine image.  **Perturbation-proven** (`CONTACT_PERTURB=1`
corrupts the engine-image baseline → the gate FAILs).  **Gate: `validate_physics` PASS=16733
FAIL=0 UNPORTED=624** (was 16584; +149 contact-family records; the 624 UNPORTED is unchanged —
the p1_movement_dispatch call-through that Phase-9 T2 converts).

## Phase-9 T1 status (P1 contact-action handler family)

As of Phase-9 Task 1 the Player-1 contact-action handler family is **reconstructed and
validated**:

- **Reconstructed 1:1**: `apply_contact_action` (6a89), `p1_dispatch_contact_action` (686a),
  `p1_apply_contact_action_main/_prev/_at_start/_before_end/_at_start_b/_before_end_b` (6832/
  684b/6890/68bb/68e6/6922), `p1_apply_contact_action_tbl_367e/_369e` (68fe/693a) — all in
  `src/player.c`, ported 1:1 from the live Ghidra decomp + raw disasm (verified via MCP).  The
  `apply_contact_action` `game_stubs.c` no-op is removed.
- **Gate re-confirmed (2026-06-21)**: `validate_physics` PASS=16733 FAIL=0 UNPORTED=624
  (contact-family PASS=149, perturbation-proven).  No-regression: `validate_player` PASS;
  `validate_anim` PASS=45 FAIL=0 UNPORTED=1 DESC_CHECKED=28 (B table grew the terminator;
  harness wired it); `validate_spawn` PASS (9 runs, 7 with layer-B, FAIL=0); `validate_items`
  PASS=11 FAIL=0; `validate_p2` PASS=74 FAIL=0; `validate_input` 100/100; `validate_blit` 17/17
  + 24/24; `validate_bg` 119/119; `validate_composite` 54152 @ 53858; `validate_sound` PASS=4414
  FAIL=0 UNPORTED=25; `validate_screen_fns` PASS=884 FAIL=0; `validate_copyprot` PASS=36 FAIL=0;
  `BUMPY.EXE` links clean (Open Watcom 16-bit DOS, apply_contact_action now in player.obj, no
  duplicate symbols).  (`validate_sprites` / `validate_screens` need regenerable inputs
  `SPROUT.BIN` / `BUMPRESE.PLN` absent from this checkout — pre-existing, unrelated to this task.)

**Deferred from Phase 9 T1:** the offset→fn resolver that converts the 624 UNPORTED
`p1_movement_dispatch` call-through records (Phase-9 T2 — **now DONE, below**); the
project-wide **int8-synced end-to-end gate** (unchanged from Phases 2–8).

## Phase-9 T2 module audit (dispatch-knot resolver + `handle_gameplay_input` — `src/player.c`)

The `move_step_dispatch_tbl` (DGROUP 0x43c0) is a **byte-exact** dump of the engine's 2D
table of little-endian 16-bit NEAR offsets (0x40 modes × 0x11 word entries). The engine
`CALL`s through those offsets directly (in real mode the offset *is* the code address).
The host cannot relocate a bare near offset to a reconstructed function, so `dispatch_move_step`
(238e) previously could not be executed on the host — the root cause of the 624 physics
`UNPORTED` records (the `p1_movement_dispatch` call-through records, whose handlers tail-call
`dispatch_move_step`).

| Item | Class | Note |
|------|-------|------|
| `move_step_dispatch_tbl` bytes (0x43c0) | **Transcription (unchanged)** | Still the byte-identical engine dump; `git diff` shows NO change to any `MV(...)` entry. The `dispatch_move_step` index arithmetic (`game_mode*0x22 + p1_move_step_idx*2`) is unchanged. |
| `move_step_handler_for_offset` (NEW) | **Reimplementation-leaning (host execution)** | A `switch(off)` returning the reconstructed host fn for each of the 39 distinct near offsets in the table. `0x7111`→`move_step_noop_sentinel` (the common filler slot; no fn exists at 1000:7111), `0x0000`→ documented no-op terminator (trailing row padding, never dispatched). Every other offset → its 1:1-reconstructed host fn at 1000:`<off>` (each verified via Ghidra MCP `get_function_by_address`). This resolver is the **single isolated host-execution deviation** for move-step dispatch. |
| `dispatch_move_step` (238e) | Transcription | Rewired to read the raw 16-bit offset from the (unchanged) table and route it: `move_step_handler_for_offset(off)()`. Index arithmetic 1:1. |
| 9 move-step micro-handlers: `move_step_first_variant` (6699), `_first_variant_b` (6748), `_last_variant_b` (6789), `_first_gate_c` (67ca), `_body_c` (67e2), `_last_gate_c` (67fb), `_last_body_c` (6813), `check_exit_tile_horiz` (6326), `cursor_move_left` (651c) | Transcription | Targets of the table that were not reconstructed in earlier phases; ported 1:1 here from the live decomp + raw disasm so the resolver maps **every** offset to a real host fn. Their two new DGROUP LUTs (`contact_action_lut_35be`, `pending_anim_lut_3c7a`) are byte-exact dumps from `BUMPY_unpacked.exe`. |
| `handle_gameplay_input` (1d26) | Transcription | The per-tick player-spine input handler. Reconstructed 1:1 (decompiled fresh via MCP): F1–F7 debug keys via `get_key_state(0x3b..0x3f/0x01/0x44)` setting `timing_flag_accumulator` (0/0x88/0xaa/0xee/0xff) / `run_physics_settle` / skip-level (`frame_abort_flag`=1 + clear round/session continue); then if `!pvp_collision_flag`: `p1_read_tile_under`; `poll_input`; `p1_move_steps_left==0 ? p1_movement_dispatch() : dispatch_move_step()`; else `begin_physics_freeze()`. Un-stubbed from `game_stubs.c`. |
| `begin_physics_freeze` (228d) | Transcription | A leaf `handle_gameplay_input` calls (was neither reconstructed nor stubbed). Ported 1:1: `physics_frozen`=1, `p1_move_step_idx`=0, `pvp_collision_flag`=0, `enter_game_mode(0x2e)`. |

## Phase-9 T2 status (dispatch-knot resolver)

- **624 UNPORTED → 0.** `validate_physics` now PASS=**17357** FAIL=0 **UNPORTED=0** (was
  PASS=16733 FAIL=0 UNPORTED=624). With the resolver, `dispatch_move_step` and
  `p1_movement_dispatch` are host-callable: the harness (`tools/physics_ctest.c`) seeds each
  former-UNPORTED record's entry, calls the real resolver-backed dispatch, and asserts the
  captured exit SNAP — all 624 convert to PASS. The trajectory-stitch also matches fully.
- **Perturbation-proven.** `PHYS_PERTURB=1` mis-routes one resolver mapping (offset
  `0x7111`→`0x65e5`) so dispatch routes records to the wrong host fn — the gate then FAILs on
  the `input` SNAP field (proving the resolver routing is genuinely exercised). `CONTACT_PERTURB`
  (Phase-9 T1) still has teeth.
- **No regression / links clean.** `validate_player` PASS (its E6 + the F-section step-slot
  installs were updated: dispatch is host-safe now, no pointer injection); `validate_input`,
  `validate_items`, `validate_p2`, `validate_anim`, `validate_sound`, `validate_spawn`,
  `validate_blit`, `validate_bg`, `validate_screen_fns`, `validate_copyprot`, `validate_composite`
  all PASS. `BUMPY.EXE` links clean (Open Watcom 16-bit DOS; `handle_gameplay_input` +
  `begin_physics_freeze` now in `player.obj`, no duplicate symbols). (`validate_sprites` /
  `validate_screens` need regenerable assets `SPROUT.BIN` / `*.PLN` absent from this checkout —
  pre-existing, unrelated; `validate_composite` needs sandbox-disabled uv.)

## Phase-9 T3 module audit (P1 per-tick spine — `player.c` / `player2.c` / `level.c` / `game.c`)

The symmetric Player-1 counterparts of the already-validated P2 per-tick functions,
reconstructed 1:1 so `game_loop` drives the real P1 path (the P1 callees were the last
per-tick stubs in `game_stubs.c`).  Each is the structural mirror of its P2 sibling in
`player2.c`, cross-checked function-for-function; the one consistent difference is that P1
has **no `p1_cell != 0xff` presence guard** (P1 is always present — that guard is
P2-specific, since P2 may be absent on a 1-player level).  Validation splits the same way as
the rest of `src/`: the scalar fns at the **semantic-state** level, the render fns at the
**view-descriptor** level over the already-plane-exact Phase-0 blitter.

| Module / function set | Fidelity | Notes |
|---|---|---|
| `p1_update_grid_cell` (1473) / `p1_advance_grid_history` (138c) | Transcription | Grid-cell recompute (p1 pixel − sprite origin, SAR-4−1 / SAR-3, clamp 0..0x12 / 0..0x16) + history slide.  Mirror of `p2_update_grid_cell` 4b4e / `p2_advance_grid_history` 13b2, minus the presence guard.  Semantic-state validated (the grid scalars). |
| `render_p1_view` (1bd7) / `erase_p1_view` (19e4) | Transcription | View scroll compute + the 4 descriptor field-writes (render +6/+8/+1e/+20; erase +14/+16/+1e/+20) then the present/restore leaf.  Mirror of `render_p2_view` 1c41 / `erase_p2_view` 19a1.  Descriptor-level validated over the plane-exact blitter; the present leaf is a faithful-signature stub (`p1_render_view_leaf` / `p1_restore_view_leaf`), same convention as the P2 wrappers + `anim.c`. |
| `draw_p1_sprite` (1cb2) | Transcription | Build the `p1_sprite` obj descriptor (+0 px, +2 py, +4 frame=p1_move_anim) then `blit_sprite(0x792e,0x203b)`; skipped on the hidden sentinel (p1_move_anim==100).  Mirror of `draw_p2_sprite` 1cea.  **Reconciliation:** `draw_p1_sprite` is the ZERO-ARG game-loop ENTRY (reads live globals, writes the engine's p1_sprite obj); `entity.c`'s `entity_draw_p1` is the EXPLICIT-ARG render helper used by the validated spawn/composite path.  Two distinct symbols (exactly as P2: `draw_p2_sprite` vs `entity_draw_p2`); no duplicate.  Descriptor-level validated. |
| `update_p1_bbox` (5085) | Transcription | P1 AABB from pixel pos (x∈[px−5,px+6], y∈[py−5,py+5]) unless physics frozen.  Mirror of `update_p2_bbox` 50c0.  **Lives in `player2.c`** because it writes the pvp P1 bbox words (0x84c/0x84e/0x850/0x852) that module owns — the same words `check_pvp_collision` tests.  Semantic-state validated. |
| `restore_bg_pending` (1a20) | Transcription | Deferred bg-restore: if `pending_erase_count != 0`, decrement + write the saved cell into the pending-erase view (+6/+8/+14/+16) + restore leaf.  No P1/P2 analog (shared deferred-erase helper, staged by `p1_collect_item_score`).  Owns the pending-erase DGROUP globals in `player.c`.  Scalar + descriptor validated. |
| `all_entries_flag_set` (3e8a) | Transcription | Level-complete predicate the `game_loop` round-exit polls: walk the per-level move-descriptor table (9-byte stride from index 1 until a 0xff sentinel) ANDing each record's [0] flag → 1 iff all set.  Was a `game_stubs.c` no-op returning 0 (round never completed); now the real predicate.  **Lives in `level.c`** (move_descriptor_table is level state).  Semantic-state validated (the AL return). |
| `init_view_anim_descriptors` (535e) | Transcription | `run_game_session` one-time setup of all 15 view descriptor structs (P1/P2 render+erase, channel-A/B anim clear/draw/erase for both player sides, the status-row render descriptor, the pending-erase view).  Ported 1:1 from the disasm field-by-field.  **Lives in `game.c`** (session-setup; references view far ptrs owned across modules).  Descriptor-level validated (the words it writes per view). |
| `game_post_present` (629c) | Transcription | Per-tick post-present helper — **REAL game logic, reconstructed** (decompiled fresh; the Ghidra auto-decompiler failed on the far-ptr walk but the disasm is clean).  Drives a deferred-contact event queue: a far ptr walks a near event buffer; a per-event countdown gates a delayed `apply_contact_action(0x18)` + device-dependent contact sound.  No port I/O / CRTC.  **Lives in `game.c`.** |
| `game_post_input` (233a) | Transcription | Per-tick post-input helper — REAL game logic: the level-complete EXIT-animation tick (counts `level_complete_anim_counter` up to 9, then relocates `anim_target_cell = level_exit_cell` + fires `apply_cell_animation('Z')`).  **Lives in `game.c`.** |

**Phase-9 T3 deviations (all in-code RECONSTRUCTION FIDELITY notes present):**

- **Present/blit leaf stubs.** `render_p1_view` / `erase_p1_view` / `restore_bg_pending` /
  `draw_p1_sprite` issue the engine's far-ptr present/blit leaf (`render_player_view` 93b8 /
  `restore_bg_view` 80bc / `blit_sprite` 942a) as a faithful-signature stub
  (`p1_render_view_leaf` / `p1_restore_view_leaf` / `p1_blit_sprite_leaf`) — the descriptor
  field-writes are the validated output, over the already-plane-exact Phase-0 blitter (same
  convention as `player2.c`'s P2 wrappers and `anim.c`'s `anim_*_leaf`).
- **DS-register seg store.** `init_view_anim_descriptors` writes the view far-ptr SEG halves
  as the runtime DS register; the source default is the static-image DGROUP literal `0x203b`
  via `GAME_DGROUP_RUNTIME_SEG`, the descriptor gate overrides it to the captured runtime
  `0x114b` (same mechanism as `anim.c`'s `ANIM_DGROUP_RUNTIME_SEG`).  `render_descriptor_ptr`'s
  +0x22..+0x25 writes fall outside the oracle's 0x22-byte capture window and are not gated
  (documented in the harness).
- **Four unnamed P2-side anim views.** The view descriptors at DGROUP 0x8c8/0x8cc/0x8d8/0x8dc
  are unnamed in the engine (no Ghidra symbol; written only by init); owned + named in `game.c`.
- **`game_post_present` / `game_post_input` not independently gated.** Both reconstruct REAL
  game logic and host-compile (the validate script extracts their verbatim bodies for the
  harness), but their effects flow only through stubbed leaves (`apply_contact_action`,
  `play_sound`, `apply_cell_animation`) — so they are link- + compile-validated, not gated by a
  per-fn differential.  Stated plainly (the blitter/sound-L5 precedent for documented-but-not-
  runtime-gated reconstructions).
- **`deferred_contact_buf` reset target.** `game_post_present` resets its queue cursor to the
  engine's literal DS:0x886 near offset; reconstructed as the module-owned `deferred_contact_buf`
  head (functionally identical — the cursor walks the buffer; the literal offset is not
  load-bearing for the walk).

**Phase-9 T3 validation method:** per-function semantic-state + view-descriptor differential
(`tools/p1_spine_oracle.py` boots the real `BUMPY.EXE`, loads level 1, hooks the 9 captured P1
fns entry+exit, captures the P1 grid/bbox scalars + the all_entries predicate return + the
view/obj descriptor bytes; `tools/p1_spine_ctest.c` seeds each record's entry, calls the real
reconstructed C fn, asserts the exit scalars / the descriptor fields the fn writes).  The
descriptor gate is over the already-plane-exact Phase-0 blitter (no a000/a200 re-litigation).
**Gate: `validate_p1_spine` PASS=30 FAIL=0 DESC_CHECKED=17**, perturbation-proven (the
`--perturb` run corrupts a seeded field per record and the gate catches 25 — a genuine
differential).

## Phase-9 T3 status (P1 per-tick spine)

- **Reconstructed 1:1**: `p1_update_grid_cell` (1473), `p1_advance_grid_history` (138c),
  `render_p1_view` (1bd7), `erase_p1_view` (19e4), `update_p1_bbox` (5085), `draw_p1_sprite`
  (1cb2), `restore_bg_pending` (1a20) [all `player.c`/`player2.c`], `all_entries_flag_set`
  (3e8a) [`level.c`], `init_view_anim_descriptors` (535e), `game_post_present` (629c),
  `game_post_input` (233a) [all `game.c`] — verified via Ghidra MCP + raw disasm; un-stubbed
  from `game_stubs.c` (no duplicate symbols).  `game_loop` now drives the real P1 path.
- **Gate**: `validate_p1_spine` PASS=30 FAIL=0 DESC_CHECKED=17 (perturbation-proven).
- **No-regression (2026-06-21)**: `validate_physics` PASS=17357 FAIL=0 UNPORTED=0;
  `validate_player` PASS; `validate_p2` PASS=74 FAIL=0 UNPORTED=0 DESC_CHECKED=1;
  `validate_anim` PASS=45 FAIL=0 UNPORTED=1 DESC_CHECKED=28; `validate_spawn` FAIL=0.
  `BUMPY.EXE` links clean (227.8K, Open Watcom 16-bit DOS; the P1 spine bodies now in
  `player.obj`/`player2.obj`/`level.obj`/`game.obj`, no duplicate symbols).

## Phase-9 T4 module audit (spine wiring + carve-out boundary — `game.c` / `game_stubs.c`)

Phase-9 T4 wires the `game_loop` per-tick spine to the real reconstructed P1 path (the T1–T3
bodies), reconciles the remaining `FUN_*`-spelled link symbols to confirmed Ghidra names, and
formalises the **carve-out boundary**: which `game_loop` callees are genuine
hardware/CRT/int8-timing leaves vs reconstructed game logic. The T4 work is **behaviour-neutral**
— the `game.c` / `game.h` edits are comment/documentation-only (the call lines are byte-identical
both sides); what changes is the framing (`game_stubs.c` reworded DEFERRED → carve-out) and the
new structural gate that enforces it.

| Item | Class | Note |
|------|-------|------|
| `game_loop` spine wiring | Transcription (unchanged code) | The four session/loop fns (`run_game_session` 0258, `init_game_session_state` 0282, `reset_game_state` 0bf9, `game_loop` 0c18) are the Phase-1 1:1 ports; with the T1–T3 bodies un-stubbed, the per-tick callees they invoke now resolve to **real module `.obj` code** (player/player2/level/game/anim/spawn) rather than `game_stubs.c` no-ops. No call line changed in T4 — only the resolution target (stub → real fn) as the bodies landed across Phases 2–9. |
| `game_stubs.c` (58 carve-out fn symbols + `mode_script_tbl`) | **Genuine carve-outs (not reconstructions)** | Reworded DEFERRED → carve-out. Every remaining symbol is a faithful-signature no-op/benign-default stub in one of the documented carve-out classes (HARDWARE-INIT / CRTC PAGE-FLIP `present_frame` / int8-TIMING `run_n_frames`·`rotate_timing_flags_and_wait`·`wait_keypress` / ENGINE STANDALONE LOADER `load_current_level_data` / NEVER-DECOMPILED `init_round_state` 0x31de / RENDER-CORE LEAVES / OUT-OF-SCOPE SOUND-L4·L6 + player handler-table targets + the P2 indirect-call backend / the `dos_abort` CRT thunk) — the same spirit as the self-modifying BGI-overlay blitters and the L5 timer ISR. Through Phases 2–8 most of the original deferral list was un-stubbed into real bodies; what remains is carve-out-only. |
| `FUN_*` link-symbol reconciliation | Naming (no code change) | The `FUN_*`-spelled symbols with **confirmed** Ghidra labels were cited inline to their canonical names (`9821`=`set_crtc_window`, `6183`=`sweep_active_entities`, `4802`=`move_step_teleport_exit`, `1e3d`=`game_mode_handler_idx30`, `7563`=`init_sound_tables`, `31de`=`init_round_state`). The link symbols themselves are kept `FUN_*`-spelled to mark "unported carve-out, link-only" and avoid churning the validated call sites; the canonical name is cited in-code (no guessed names — the live sound/player stub symbols whose labels are not confirmed stay `FUN_*` with no invented name). |

**Phase-9 T4 deviations / honest carve-out boundary (in-code notes present):**

- **The carve-out boundary is what is NOT reconstructed**, by design: the
  `init_game_session_state` CRTC/audio/resource/IRQ hardware-init block, the CRTC page-flip
  `present_frame`, the int8 timing/wait leaves, the engine standalone loader, the
  never-decompiled `init_round_state` (Ghidra-labelled but decompilation fails), the
  render-core BGI-overlay leaves, and the out-of-scope sound-L4/L6 + handler-table tails. These
  are stated plainly as carve-outs (the blitter / L5-ISR / DAC precedents), not claimed as ports.
- **The carve-out partition is gated, not asserted.** `tools/validate_integration.sh` proves
  the boundary mechanically rather than by trust (see the gate description below).

## Phase-9 T4 status (spine wiring + integration gate)

- **`game_loop` drives the real P1 path.** With the T1–T3 bodies un-stubbed, every per-tick
  spine callee `game_loop` invokes resolves to its real reconstructed module `.obj` (39 spine
  callees), leaving only the 58 documented carve-outs in `game_stubs.obj`.
- **New gate — `tools/validate_integration.sh` (structural)**: [1/3] real `wmake` link of
  `BUMPY.EXE`, failing on any duplicate / multiply-defined symbol; [2/3] parses the `wlink`
  `option map` per-`.obj` public-symbol listing (`module_of()`), resolves all 39 `game_loop`
  spine callees, and asserts **none** resolve to `game_stubs.obj` (each is a real reconstructed
  body); [3/3] asserts `game_stubs.obj`'s 58 fn symbols ⊆ the explicit `CARVEOUT_ALLOWLIST`
  (no stale / missing carve-out). The 39-spine and 58-stub sets have **zero overlap** (clean
  partition). A built-in `--self-test` perturbs the map and confirms a victim spine callee
  routing to `game_stubs.obj` is caught (the gate has teeth).
- **Gate re-confirmed (2026-06-21)**: `validate_integration` PASS — `BUMPY.EXE` links clean
  (233 230 bytes, no duplicate symbols); 39 spine callees all in real module `.obj`; 58 stub
  symbols all allowlisted carve-outs. No-regression across the full gate set (see the Phase-9
  consolidated status below).
- **`game_tick` (game.c) — extraction-only factoring of `game_loop`'s innermost per-tick loop
  body for the int8 end-to-end harness; verbatim statements, no reorder.**

## Phase-9 status (final integration — spine wiring + dispatch resolution)

As of Phase-9 Task 4 (validate gates re-confirmed Phase-9 Task 5), the final-integration work
is **complete and validated**: the P1 game-mode dispatch knot is resolved, the P1 contact-action
family and per-tick spine are reconstructed, and `game_loop` drives the real P1 path with the
carve-out boundary mechanically gated.

- **Reconstructed 1:1 (Phase 9)**: the P1 contact-action handler family + `apply_contact_action`
  allocator (T1); the `handle_gameplay_input` (1d26) + `begin_physics_freeze` (228d) input
  handler, the 9 remaining `move_step_dispatch_tbl` micro-handlers, and the `dispatch_move_step`
  rewire (T2); the 11 P1 per-tick spine fns — grid/view/bbox/draw + `all_entries_flag_set`,
  `restore_bg_pending`, `init_view_anim_descriptors`, `game_post_present`, `game_post_input`
  (T3) — all ported 1:1 from the live Ghidra decomp + raw disasm.
- **The dispatch knot — RESOLVED.** `move_step_handler_for_offset` (T2) is the **single isolated
  host-execution deviation**: the `move_step_dispatch_tbl` (0x43c0) byte blob is **unchanged**
  (byte-identical engine dump, no `MV()` diff), and the index arithmetic is 1:1; the resolver
  only maps each of the 39 distinct near offsets to its 1:1-reconstructed host fn so a bare
  real-mode near offset can be executed on the host. Precedent: `game_mode_handlers` (0x7ca) was
  already a host-fn-ptr table; this is the analogous (and last) call-through. The 624 physics
  `UNPORTED` records (`p1_movement_dispatch` call-through) converted to **0**.
- **`game_loop` drives the real P1 path** (T4): 39 spine callees → real module `.obj`, 58
  carve-out-only stubs ⊆ allowlist, enforced by `validate_integration.sh` (`--self-test` proves
  it fails on a spine→stub regression). `game_stubs.c` reworded DEFERRED → carve-out; `FUN_*`
  link symbols reconciled to confirmed Ghidra names (no guessed names).
- **Honest carve-outs / coverage limits (stated plainly):** `game_post_present` /
  `game_post_input` are reconstructed 1:1 but **not per-fn gated** (their effects flow only
  through stubbed `apply_contact_action` / `play_sound` / `apply_cell_animation` leaves; they are
  link- + compile-validated — the blitter / L5-ISR / DAC precedent); `init_view_anim_descriptors`'
  slot-0 `render_descriptor_ptr` +0x22 write falls outside the oracle's 0x22-byte capture window
  (other slots gated). The T3 review found + fixed a `game_post_present` sound-id inversion
  (0x0e/0x11) that was masked by the carve-out — exactly the hazard such carve-outs carry.
- **New gates added in Phase 9**: `validate_p1_spine` (T3, P1 per-tick spine differential) and
  `validate_integration` (T4, structural spine-vs-carve-out partition).
- **Full gate set re-confirmed (2026-06-21)** — all green:
  - `validate_physics` **PASS=17357 FAIL=0 UNPORTED=0** (was 16584/624; +149 contact-family, 624
    dispatch-knot records converted) — the headline Phase-9 result.
  - `validate_p1_spine` **PASS=30 FAIL=0 DESC_CHECKED=17** (perturbation-proven; the `--perturb`
    self-test flips PASS=5/FAIL=25, proving the differential has teeth).
  - `validate_integration` **PASS** (`BUMPY.EXE` 233 230 B, no dup symbols; 39 spine callees in
    real `.obj`; 58 stubs ⊆ allowlist).
  - `validate_player` PASS; `validate_input` 100/100; `validate_items` PASS=11 FAIL=0 UNPORTED=0;
    `validate_p2` PASS=74 FAIL=0 UNPORTED=0 DESC_CHECKED=1; `validate_anim` PASS=45 FAIL=0
    UNPORTED=1 DESC_CHECKED=28; `validate_spawn` PASS (9 runs, 7 with layer-B, FAIL=0);
    `validate_sound` FAIL=0 PORT_CHECKED=3752 UNPORTED=25; `validate_screen_fns` PASS=884 FAIL=0
    UNPORTED=0 (DESC_CHECKED=41, PORT_CHECKED=837); `validate_copyprot` PASS=36 FAIL=0.
  - `validate_blit` 17/17 anim + 17/17 chain + 24/24 blits; `validate_bg` 119/119;
    `validate_composite` 54152 @ 53858 baseline; `validate_screens` 5/5 VEC pixel-exact;
    `validate_sprites` PASS (sprite-bank transform 87068 B byte-exact).
  - `BUMPY.EXE` links clean (233 230 B, Open Watcom 16-bit DOS, no duplicate symbols).

**Deferred from Phase 9 (the remaining open items):**

1. **int8-synced end-to-end (tick-for-tick) gate** — **RESOLVED** (int8-snap, Task 7; see the
   dedicated section below). A frame-accurate DOSBox capture now drives the reconstructed
   `game_tick()` tick-for-tick; the gate `tools/validate_int8.sh` PASSES (150-frame replay,
   every gameplay scalar field matched, `--perturb` caught, oracle anchor agreeing) with one
   precise documented carve-out (the animated-tile FX-graphics layer, render-core-owned).
2. **CODES.EXE registration RE** — the separate TinyProg-packed registration binary
   (`CODES.EXE` + `VS.VSN` + `VGUARD.DAT`), orphaned from `BUMPY.EXE` (Phase 7b finding). Its
   own effort; deferred. **(The one remaining project-wide deferral.)**

## int8-synced end-to-end gate — RESOLVED (int8-snap, Task 7)

The project's last standing composition deferral is closed. An **instrumented DOSBox-x**
(`tools/dosbox/build-dosbox-x.sh` + `tools/dosbox/patches/01`+`02`) captures the REAL
`BUMPY.EXE` per-tick gameplay loop at the int8 frame boundary (runtime `cs:ip = 0824:0cda`,
the `game_loop` inner-loop top) into a binary trace (`tools/int8_trace.h` layout), and the
host harness `tools/int8_ctest.c` replays it: seed the reconstructed engine globals ONCE
from the captured `INIT`, then per frame feed the trailing `rng`/`input` and call the REAL
`game_tick()`, asserting the evolved gameplay scalars == the captured `FRAME[k].state`. The
one-command gate is `tools/validate_int8.sh` (build emulator + capture on demand → OW
compile-check → build harness → oracle anchor → replay → perturb).

**Result — PASS.** The reconstructed `game_tick()` matches the real captured 150-frame
level-1 scenario tick-for-tick on **every** gameplay scalar field (the union of the
per-function gates' compared fields: pixel/grid/bbox, the `game_mode`/move-step machine,
score, items/exit/level/anim state, `move_override`, the round/session flags, …). The
`--perturb` differential is caught (corrupting one seeded field diverges tick 0), and the
calibration trust anchor (`tools/int8_oracle_check.py`, the `update_p1_bbox` pixel→AABB
oracle booted under Unicorn) agrees on INIT + first frames. This proves the functions
**compose** into a faithful running game — correct call order, inter-function state flow,
loop timing — not just that each is faithful in isolation.

**Read-set the gate had to capture (each a genuine input `game_tick` consumes; `INT8_VERSION`
guards stale traces, bumped on every layout change):**

- **Move-script system (v2/v3).** `p1_step_scripted_move` derefs the `p1_move_script` far
  ptr each tick, and `enter_game_mode` re-arms it from `mode_script_tbl` (DGROUP 0x2252) →
  a `[steps,facing,off,seg]` header → the `[anim,dx,dy]` step array. These are
  loader-relocated static data; without them the first real replay read a NULL far ptr and
  crashed. Captured as the live `p1_move_script` (off/seg) scalars + a low-DGROUP static
  window.
- **Sprite-object descriptors (v2).** `p1/p2_update_grid_cell` read the sprite origin words
  at the `p1_sprite`/`p2_sprite` pointee `+0x14`/`+0x16` (which feed the COMPARED grid-cell
  scalars). Captured into the `entity_state` block (previously reserved zeros) and seeded
  into the host sprite backing buffers.
- **Cell-animation tables (v3).** `apply_cell_animation` + the anim-channel steppers read
  the anim far-ptr tables (`anim_a_tiledef_tbl` 0x2ede, `anim_a/b_frame_tbl` 0x3d6a/0x40a6,
  the grid/pos tables) and follow them to the tile-def/frame/stream blobs. The v2 window was
  widened (v3) to cover the whole low-DGROUP move+anim static region (0x0000..0x4600) so
  every far hop resolves 1:1; without them `apply_cell_animation` read a zeroed tile-def and
  spuriously stamped `tilemap[0x28]=0`, cascading into a `move_override` divergence.

**Capture-calibration bug found + fixed (v2→v3):** the `02` patch read `move_step_count`
from DGROUP `0x855e` (the `p1_step_col_count` slot) with an incorrect "aliases
p1_step_col_count" comment. `move_step_count` (`jump_step_counter`) is actually DGROUP
**`0x824c`** (confirmed live: `gamemode_default_idle` 1000:2905 `MOV byte ptr [0x824c],0x8`).
The host computed the real value (8); the mis-read capture stored 0. Fixed to 0x824c.

**One precise exclusion (NOT a tolerance) — the render-core animated-tile FX layer.** After
closing the read-set above, the only residual divergence is the per-frame `tilemap_hash`,
caused by a single FX cell (e.g. `0xc8` = active anim-slot cell `0x28` + `0xa0`) cycling its
**displayed tile-graphic index** (+6/tick). That write is produced INSIDE the carved-out BGI
render core: `draw_anim_channels_a` → `render_player_view` (1000:93b8) → `bgi_set_mode_10` →
the un-analyzed EGAVGA overlay handler (`1ab9:0db0`) — the documented render-leaf carve-out
(`src/anim.c` FIDELITY note; `docs/rendering-pipeline.md`). No reconstructed (or original)
`game_tick` state-callee writes that FX layer (confirmed by decompiling `draw_anim_channels_a`
/ `step_anim_channels_a` / `render_player_view`), and NO gameplay-collision callee READS it
(collision reads only `tilemap[cell]`/`+0x30`/`+0x60`, all of which match across all 150
frames). So `tilemap_hash` is excluded from `run_replay`'s compare with an in-code
justification; the collision-layer tilemap is validated per-cell by the items/anim/spawn
per-function gates. This is the render-core partition `validate_integration.sh` already
asserts — a single named exclusion, not a broad tolerance.

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

## Playable host platform (Plan A — the *Devilution-X*-flavored runnable side)

The playable build (`BUMPYP.EXE`, `wmake play`, all TUs compiled `-dBUMPY_PLAYABLE`)
links the `src/host/*.c` platform layer instead of the faithful default's
`game_stubs.c`.  The faithful default build (`BUMPY.EXE`) is **byte-unchanged** by
this work (verified: md5 identical; `validate_integration.sh` green); every edit to a
gameplay TU is a minimal `#ifndef BUMPY_PLAYABLE` guard around an existing stub body,
so the real body comes from the host layer only under the flag.

**Task 2 — host framebuffer + render-leaf binding (`src/host/host_render.c`) — the
documented CORE render divergence:**

- **Flat 4-plane RAM framebuffer vs VGA A000/A200 hardware.** The original engine
  blits straight to VGA: the planar blitter programs the Sequencer / Graphics-
  Controller registers (`out 0x3c4 / 0x3ce` map-mask + bit-mask) and writes into the
  A000/A200 MMIO window, double-buffering across two VGA pages and presenting via a
  CRTC start-address page flip.  `host_fb_init` instead allocates a single flat
  `4 × 0x10000 B` RAM image (`host_framebuffer`) — the SAME memory image the validated
  blitters (`sprite_blit_planar_vga`, `bg_render_grid`) and the composite gate
  (`tools/composite_ctest.c`) already produce byte-exact.  The two VGA pages are
  modelled as byte offsets `0x0000` (page0/a000) and `0x2000` (page1/a200) WITHIN each
  plane.  `host_video.c`'s present path packs page-0 and copies that RAM image to real
  VGA.  This is a behavior-faithful **memory model** of the hardware port writes +
  double-buffer, NOT a 1:1 transcription of the register sequence.

- **Page table as real host globals.** The engine's `sprite_table_base` (DGROUP
  `0x5415`, two far ptrs page1/page0) and `cur_sprite_data` (DGROUP `0x56e2/0x56e4`,
  the current draw page) are reconstructed as real C globals in `host_render.c`
  (`host_sprite_table_off/seg[2]`, `host_cur_sprite_data_off/seg`) pointing into
  `host_framebuffer`.  In the host memory model the "segment" is a synthetic VGA tag
  (`0xa000`/`0xa200`) and the page is expressed purely through the `(off,seg)` pair the
  blitter folds into the dest offset (`view->data_off/data_seg` → `desc[0x08/0x0a]` →
  `voff`), exactly as `composite_ctest`'s view does.

- **The render-leaf wrappers made real.** The engine's per-tick render leaves
  (`blit_sprite` 1000:942a, `render_player_view` 1000:93b8, `restore_bg_view`
  1000:80bc) are far-pointer calls that read the bank / DGROUP entity shadow / active
  view from globals.  Their gameplay-module stubs (`anim_*_leaf`, `p1_*_leaf`,
  `p2_*_leaf`) were faithful-signature NOPs because they carry no work-buffer context.
  Under `BUMPY_PLAYABLE` the real bodies live in `host_render.c`:
  - **Blit leaves** (`anim_blit_sprite_leaf` / `p1_blit_sprite_leaf` /
    `p2_blit_sprite_leaf`) resolve the current draw page within `host_framebuffer`,
    read the populated obj's X/Y/frame from the registered dg shadow at `obj_off`
    (`0x792e` p1_sprite / `0x795a` p2_sprite), and re-run the **validated**
    `entity_draw_p1`/`entity_draw_p2` pipeline into the framebuffer — the exact call
    shape `composite_ctest` uses.  `level.c` registers the bank / dg / framebuffer via
    `host_render_bind` (the engine's "leaf reads globals" convention).
  - **View leaves** (`*_render_view_leaf` / `*_restore_view_leaf`) drive the
    reconstructed `bgi_overlay.c` `render_player_view` / `restore_bg_view` with a
    code-embedded NOP view (`word00=0xc3fb` / `word0e=0x85b3 > 1`) — STRUCTURAL NOPs in
    the gameplay context, matching the engine (`present_model.md §5`).  The visible
    per-tick pixels come solely from the blit leaves.
  - **Out-of-scope leaves** (`anim_render_leaf_80ac` B-side render leaf, the BGI text
    leaves) stay faithful NOPs (no clean decomp).

- **`g_planes` aliases `host_framebuffer`.** Under the flag, `level.c`'s static level
  compose and the per-tick leaf draws share one RAM image (`level_alloc_buffers` calls
  `host_fb_init` and sets `g_planes = host_framebuffer`), and present blits that image.

- **Validation.** `tools/validate_host_compose.sh` + `tools/host_compose_ctest.c` drive
  a full level compose (bg → layerC → P1-via-`p1_blit_sprite_leaf` → P2 → layerA →
  layerB) through the real host render layer and assert `host_framebuffer` ==
  `composite_ctest`'s validated reference, **plane-for-plane, byte-exact (262144/262144)**.
  The pixels the playable build composes through the leaves are identical to the
  already-trusted composite gate.

**Task 7 — view/setup leaves + background save-under (`src/view_setup.c`):**

- **Flat-RAM save-under (setup_fullscreen_view).** The original engine's
  `setup_fullscreen_view` (1000:483c) copies VGA page-0 (`a000:0000`) into
  `fullscreen_buf` via `render_player_view` + the BGI overlay mode-10 subhandler-0
  full-plane copy.  In the host flat-RAM model we additionally copy
  `host_framebuffer` page-0 (4 planes × 0x1F40 B each = 32000 B total) into a
  static `hv_saveunder_buf` allocated from the far heap.  The view-descriptor
  build at `render_descriptor_ptr` and the `render_player_view` call are
  faithfully reconstructed 1:1; only the source (flat RAM vs VGA a000) and
  destination (`hv_saveunder_buf` vs `fullscreen_buf`) differ.

- **Sprite-obj far-ptr init (init_sprite_structs).** The engine stores sprite
  objects at fixed DGROUP offsets (0x792e p1_sprite, 0x795a p2_sprite, 0x7986
  hud-icon) and seeds each object's `+0x06/+0x08` from `screen_sprite_buf`
  (DGROUP 0xa0c6/0xa0c8) and sets the flag byte (`+0x09`: old & 0x87 | 0x80).
  In the host: `p1_sprite` and `p2_sprite` are pointed at
  `g_entity_dg[0x792e]` / `g_entity_dg[0x795a]` via `level_get_entity_dg()` —
  the same shadow buffer `hr_blit_obj` reads from — ensuring `draw_p1/p2_sprite`
  obj writes and the blit leaf read the same bytes.  The sprite-sheet buf seed
  (`+0x06/+0x08`) and flag byte (`+0x09`) are skipped: the frametable is seeded
  by `level_populate_dg` (level.c) from `g_bank_buf` (the correct frametable);
  `screen_sprite_buf` is a BGI-overlay ptr unused by the planar pipeline.
  `hud_icon_sprite_ptr` (obj at 0x7986) has no C declaration; skipped.

- **BGI mode-11 call (init_fullscreen_view_desc).** The engine ends
  `init_fullscreen_view_desc` with `bgi_set_mode_11_thunk` (→ `bgi_set_mode_11`
  1ab9:126e, dynamically-loaded BGI overlay code not decompilable).  In the host
  this call is replaced by `present_frame(1)` which copies the composed RAM image
  to real VGA.  The view-descriptor build is 1:1.

- **redraw_level_background_tiles carve-out.** `setup_fullscreen_view` calls
  `redraw_level_background_tiles` (1000:2a0a) 1:1 before the save-under copy.
  That function is not yet reconstructed; a NOP stub is defined locally in
  `view_setup.c` under `BUMPY_PLAYABLE` (not in `game_stubs.c` to preserve
  BUMPY.EXE byte-equality).

**Task 8 — boot init + per-round level (re)load (`src/host/host_boot.c`):**

- **`init_joystick_handlers` — calibration skipped.** Engine body (1000:7532)
  zeros `g_joystick_handler_table[16]` then calls `calibrate_joystick()` twice.
  Host: table zeroed via `memset`; `calibrate_joystick()` skipped (keyboard-only
  Tier 1, no joystick hardware).

- **`mouse_reset` — benign no-op.** INT 33h AX=0 not issued.  Keyboard-only host.

- **`init_sound_tables` — SILENT SOUND (Tier 1).** Engine body (1000:7563)
  installs a sound-driver far-fn pointer into the joystick handler table.
  Benign no-op in the host.  Sound output is Plan B / Tier 2.

- **`set_disk_swap_callback` — benign no-op.** INT 24h critical-error handler not
  installed (single-mount Tier-1 run; no disk-swap prompt needed).

- **`set_resource_table` — benign no-op.** `resource_table_ptr` not written.
  `level.c start_level` bypasses the resource table and builds filenames directly
  (see level.c RECONSTRUCTION FIDELITY note #3).

- **`load_current_level_data` — calls `start_level()` (over-does engine 32b0).**
  Engine body (1000:32b0): lightweight 0x96-byte header copy from the in-memory
  level archive (`cur_level_ptr` / `level_src_ptr`) into the tilemap buffer.
  In the playable host the in-memory archive is never populated; host calls
  `start_level(current_level, current_level)` which performs a full file-load
  + bank reload + render pass.  Functionally equivalent for Tier 1 round resets
  (same data, redundant render), but NOT a 0x96-byte lightweight copy.
  Runtime proof (round-reset loads correct level state) deferred to Tasks 9/11.

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

## Playable host build (Plan A, Task 9 — `-dBUMPY_PLAYABLE` / `BUMPYP.EXE`)

These deviations exist ONLY in the `BUMPYP.EXE` playable build (`wmake play`); the default
`BUMPY.EXE` is byte-identical (md5 `cac9ff236a832284fec6fafff2d8602b`, 233230 B).

- **graphics-adapter select screen** (`config_screens.c`, `gfx_driver_init`) — a
  faithful 1:1 reconstruction of `gfx_driver_init` (1ab9:02ce): BIOS text mode 0x02, header
  "BUMPY (C) LORICIEL 1992" at (row 1, col 1), then the adapter menu printed at col 33 from
  row 10 for each present entry of the shipped 6-byte adapter table `{0,1,1,0,0,0}` (EGA +
  VGA), then poll F2 → `palette_mode = 1` / F3 → `palette_mode = 2`.  All strings + the table
  are verbatim from the binary (DGROUP 0x529c / 0x52b4 / 0x548b).  TWO documented host
  divergences: (1) input is read via BIOS INT 16h, not the engine's `get_key_state` over
  `g_key_state_table` — that table's INT9 ISR is not installed until `init_game_session_state`,
  which runs *after* this screen; the on-screen result (mode/strings/layout/F2-EGA/F3-VGA) is
  identical.  (2) the host uses the shipped static adapter table rather than running the live
  hardware probe (`detect_video_adapter`, 202c:0000) that would overwrite it; EGA+VGA-present
  is correct for the validated VGA path.  `DAT_203b_530e = 0x40` (a BGI flag with no host
  effect) is not reproduced.  Verified headless: BUMPYP.EXE sets mode 0x02 and parks at the
  BIOS INT 16h wait (CS:IP f000:cf4x) for the F2/F3 keypress.
  NOTE: the original's `sound_select_device` (1000:6de3) draws **no screen** (it is a silent
  device-mask init); the playable build correctly has no audio-select screen.
- **view-descriptor storage binding** — `host_view_descriptors_init` (game.c) binds the 15
  per-tick view-descriptor far pointers to one **static DGROUP buffer** (`s_host_view_desc_blk`,
  0x40 B × 15).  In the original these structs are DGROUP-resident; the reconstruction split
  each descriptor's POINTER into its own C global without backing storage, so the playable
  build binds them.  A static DGROUP buffer (not `_fmalloc`) is both faithful (DGROUP-resident)
  and robust — the first WIP used `_fmalloc`, which returned NULL after `host_fb_init`'s
  `halloc(256 KB)` exhausted the far heap, leaving the pointers NULL → `LDS BX,render_descriptor_ptr`
  loaded 0000:0000 → writes corrupted the IVT → triple fault (diagnosed in the Task-9 bring-up).
- **keyboard handler script** — `init_joystick_handlers` (host_boot.c) seeds slot 0 of
  `g_joystick_handler_table` with a faithful `read_input_action` script (arrows + FIRE);
  the original's runtime populator of this table was a Task-1 open item.
- **16 KB stack** — `wmake play` links with `-k0x4000` (vs the Watcom 2 KB default); the
  default build's real call depth + host paths overflowed the 2 KB stack at boot.  DGROUP
  data is ~47 KB so the stack must fit under the 64 KB group limit; 16 KB is the headroom.

- **host title-path `restore_bg_view` shim** (screens.c, `BUMPY_PLAYABLE` only) — screens.c
  models the engine's `restore_bg_view` (1000:80bc) with its ENGINE-FAITHFUL 2-arg far-ptr
  signature `restore_bg_view(view, seg)` and treats it as a *stubbed* BGI-overlay render leaf
  (the title present is produced by the descriptor build + `present_frame(1)` that follow).
  But `bgi_overlay.c` reconstructs the SAME symbol with the EXPANDED host 3-arg form
  `restore_bg_view(planes, vga_src, view)` (used by entity.c/player.c/view_setup.c).  Under
  `__watcall` (-ml) that body takes its first two far-ptr args in registers and its THIRD on
  the STACK, cleaning it with `retf 0x0004`; screens.c's 2-arg call pushes nothing, so the
  shared `restore_bg_view_` `retf 4` pops 4 bytes the caller never pushed → 4-byte stack
  imbalance → the title fn's own `retf` then pops a garbage frame (the `0824:5E38` wild jump →
  crash before the menu).  Under `BUMPY_PLAYABLE`, screens.c now routes its title/menu
  `restore_bg_view(view,seg)` calls to a host NOP leaf (`screens_host_restore_bg_view`) with the
  MATCHING 2-arg convention, so the host build never invokes the 3-arg body with a mismatched
  ABI.  This preserves the documented "stubbed render leaf" semantics (NOP; present via
  `present_frame`) and matches the engine's NOP-guard behaviour for these title views.  The
  default `BUMPY.EXE` build is unaffected (the `#ifndef BUMPY_PLAYABLE` extern branch is
  byte-stable; that build is byte-compared, never executed, so its latent ABI mismatch is inert).

- **playable host: BGI overlay primitives** (`src/host/host_bgi.c`, `BUMPY_PLAYABLE` only) —
  the engine reaches its graphics primitives through main-segment thunks (`1000:7b4a`…) that
  dispatch into the Borland **BGI graphics-driver overlay at segment `1ab9`**, which selects a
  per-`palette_mode` handler through runtime vector tables (`pm*2 + 0x4dda/0x5435/0x5441/…`).
  The overlay is a third-party library, **absent from the Ghidra decompilation corpus**, so its
  internals cannot be reconstructed 1:1.  Per the agreed decision (see
  `docs/faithfulness-gap-audit.md` §1 and the priority-#1 plan), these primitives are
  **reimplemented host-side in `host_bgi.c` for functional equivalence** on the VGA
  (`palette_mode==2`) path — the only path the playable build exercises — while the game still
  *invokes* them through its existing thunks (the default `BUMPY.EXE` keeps the faithful-signature
  NOP stubs in `screens.c`, so it stays byte-identical).  This un-stubs, one primitive per task,
  the present/flip/viewport leaves the title/menu transitions need (the iris wipe, blank-present
  and page-flip were dead NOPs).
  - **page-flip** (`fun_7bca_flip` = `bgi_page_flip_thunk` `1000:7bca` → `bgi_page_flip_dispatch`
    `1ab9:02b1` → VGA vector handler): `host_bgi_page_flip(page)` routes to `present_frame`
    (host_video.c) — the standard EGA/VGA double-buffer (framebuffer copy + vblank sync + CRTC
    flip), the same observable tear-free page flip.  KNOWN INTERACTION (to unify in a later task):
    `show_title_background`/`show_title_and_init` and two highscore sites call `fun_7bca_flip(0)`
    immediately followed by `present_frame(1)`, so those sites now present twice (two vblank waits;
    same content, visually harmless).  The redundancy is a symptom of the host's `present_frame`
    already merging the BGI putimage+flip; it is resolved once the putimage/present primitives land
    and the present model is unified (priority-#1 plan, Tasks 2–5).
  - **clip/viewport** (`fun_7b4a_view_blit` = `bgi_set_viewport_thunk` `1000:7b4a` →
    `bgi_init_viewport` `1ab9:0179`): `host_bgi_set_viewport(view, seg)` (host_bgi.c) mirrors the
    original 1:1 for the VGA path: writes CONSTANT clip extents `view[+0x18]=0x14`,
    `view[+0x1a]=0x19`; sets `bgi_write_mode_flag_a=2` (DGROUP 0x541f), `bgi_write_mode_flag_b=1`
    (0x5420); then returns with NO pixel blit (VGA dispatch slot `0x4dda[2]=0x0000` is null).
    **VGA iris degeneration (critical, documented in findings §2 and `faithfulness-gap-audit.md §1`):**
    `bgi_init_viewport` ignores the iris loop's per-step rect (`+0x14/+0x16/+0x1e/+0x20`) — it
    always writes the CONSTANTS 0x14/0x19 to `+0x18/+0x1a`; the compose path reads clip from
    `+0x0a/+0x0c` (different fields), so no geometric clip shrink occurs on VGA.  The visible VGA
    iris = the vsync-timed hold (4x `wait_vretrace_thunk`/step, 10 steps, Task-2 pacing) + the
    final blank-palette upload (`fun_7b93` zeroed-tiles → `fun_7bca` DAC zeroed → screen black).
    This is a **TIMED-HOLD → BLANK-TO-BLACK**, not a shrinking rectangle (the geometric iris is an
    EGA/CGA effect for non-null blit handler modes 0/1; on VGA mode 2 it degenerates).  The host
    faithfully reconstructs this degeneration — no geometric wipe is invented.  The two
    `bgi_write_mode_flag` globals (DGROUP 0x541f/0x5420) are declared in `host_bgi.c` (playable
    only; not needed in the default build whose NOP stub never reaches them).

The Task-9 OPEN blocker (`retf` at `0824:5E38` popping a corrupted frame in the title/present
path) was root-caused to the **`restore_bg_view` signature schism** above and FIXED (the host
title-path shim).  Root-cause evidence: `wdis play/bgi_overlay.obj` shows `restore_bg_view_`
reads its `view` arg from `0xa[bp]` (a STACK param) and ends `retf 0x0004`; `wdis play/screens.obj`
shows `show_title_background`'s call site (`mov ax,render_descriptor_ptr; mov dx,…+2;
mov bx,0x203b; call restore_bg_view_`) pushes NOTHING → the `retf 4` unbalances the stack.

Boot progress after the fix (DOSBox headless, `tools/dosbox/scripts/bumpyp-boot.txt`,
DGROUP 0x3fdd): BEFORE — stuck at wild `0008:3ec1` within ~6 frames of reaching mode 0x0D.
AFTER — `BUMPYP.EXE` reaches mode 0x0D (frame 64, `current_level=1`) and runs **10 000+ frames
stably with CS pinned to the real code segment `0824` and DS toggling to the correct DGROUP
`0x3fdd`** — no crash.  It then settles into the title/intro/menu input-wait loop
(IPs cycling `0824:3f7x`–`408x`).

### RESOLVED (Task-9 debug3 — the `0x3f7x`–`0x408x` idle spin was an anim-channel infinite loop)

The "stable wait loop, `game_mode` stays 0" symptom (IPs cycling `0824:3f7x`–`408x`) was NOT a
starved `wait_keypress` and NOT a re-looping intro — it was an **infinite loop inside
`draw_anim_channels_a` (anim.c, 1000:165e)**, which `game_tick()` calls every tick.  The game DID
reach the per-tick `game_tick()` loop; it just never completed a single tick.

Root-cause trace (debug3):
- Mapped the spin: `ndisasm` of `BUMPYP.EXE` at the live CS:IP `0824:3f76` matched the
  `mov bl,[bp-4]; shl bx,1; shl bx,1; mov si,ss:[bx-54ae]` channel-loop body; that exact byte
  signature is found ONLY in `play/anim.obj` → the spin is in `anim.c`.  Sub-mapping by obj offset
  put it in `step_anim_channels_a` ↔ `draw_anim_channels_a`.
- Added temporary DGROUP observability (`input_state`@0x9e83, `key[0x1c]`): across the whole run
  `input_state` stayed 0 even while `key[0x1c]=1` during the FIRE windows — the spin never polls
  input, so it is not any keypress wait; it is a pure compute loop.
- `draw_anim_channels_a` / `_b` (and the erase variants) terminate ONLY when a slot's `active`
  byte reads `0xFF`: `do { slot = anim_channels_a_tbl[i++]; active = slot->active; … } while
  (active != 0xFF);`.  The slot tables `anim_channels_a_tbl[]` / `anim_channels_b_tbl[]` are
  far-ptr arrays the engine's level-load path populates with pointers to the per-channel records
  **plus a 0xFF-terminator record**.  In the reconstruction the records + terminators are anim.c
  statics, but the table-pointer **wiring** was left to the caller (the ctest harnesses
  `tools/anim_chan_ctest.c` / `tools/int8_ctest.c` do it).  The **playable build had no such
  caller** → the tables were all-NULL → the `while (active != 0xFF)` scan dereferenced NULL slots
  and walked off the array forever (no terminator ever seen).

Fix (`#ifdef BUMPY_PLAYABLE`, view_setup.c `init_sprite_structs`): wire the slot tables to the
anim.c records + 0xFF terminators (the same wiring the harnesses do), at `game_loop`'s one-time
`init_sprite_structs` setup, before `start_level`/`spawn_and_draw_level_entities` deref the slots.
The default `BUMPY.EXE` build is unaffected (view_setup.c is not compiled there; md5 stays
`cac9ff236a832284fec6fafff2d8602b`).  This is a host-side WIRING of engine-owned data that the
slice deferred to the harness — recorded as a Playable-host divergence.

### VERIFIED (Task-9 debug3 — `BUMPYP.EXE` now reaches the level-1 per-tick gameplay loop)

With the anim-wiring fix, `BUMPYP.EXE` boots end-to-end into gameplay (DOSBox headless, the
deterministic `bumpyp.conf`, boot script `tools/dosbox/scripts/bumpyp-boot.txt`):

```
frame=70-350   level=1 mode=0   IP 0824:1ddf-1e71 (DGROUP 0x3fe4)   ← run_main_menu (FIRE picks Start)
frame=385      level=1 mode=4   IP ba51:1218       ← game_mode flips 0→4: PER-TICK GAMEPLAY ENTERED
frame=385-3500 level=1 mode=4                       ← game_tick() loop runs stably (dispatching to the BGI overlay)
```

`game_mode` transitions 0→4 at frame 385 and holds `mode=4` (active gameplay) for the rest of the
run — the documented success criterion (reach the per-tick `game_tick()` region with `mode != 0`).
The IP at `ba51:1218` is the dynamically-loaded BGI render overlay the per-tick draw path far-calls
into (with periodic `f000:caXX` BIOS dips), i.e. the engine's per-tick render/timer loop executing.

NB on the runtime DGROUP segment: adding the wiring to `init_sprite_structs` grew the code image by
0x70 bytes (entry IP 0x9EB0→0x9F20), shifting the runtime DGROUP segment 0x3FDD→**0x3FE4**.  The
DGROUP-INTERNAL offsets are unchanged (no new globals added), so `BUMPYCAP_OFF_KEYTBL=0x9dbe`,
`OFF_CURLEVEL=0x05c2`, `OFF_GAMEMODE=0x9eca` stay valid; only `BUMPYCAP_DGROUP` must be `0x3fe4`
for this build (verified live: `input_state` still at DGROUP 0x9e83, byte-signature confirmed).

Methodology note (recorded so it does not recur): the BUMPYCAP dosbox hook lives in
`hardware/vga_draw.cpp`, which is archived into `hardware/libhardware.a` before the final link.  A
plain `make` after editing `vga_draw.cpp` recompiles the `.o` but does NOT always re-archive
`libhardware.a`, so the relinked `dosbox-x` can silently keep STALE hook code.  After any hook edit,
force `rm hardware/vga_draw.o hardware/libhardware.a src/dosbox-x` before `make`, and verify with
`strings src/dosbox-x | grep BUMPYCAP`.  (An earlier debug3 run was misdiagnosed as an "0xa358
allocator hang" purely because a stale `libhardware.a` ran old hook code; the clean rebuild showed
the game reaching `mode=4` normally.)

### RESOLVED (menu/title speckle) — `restore_bg_view` is a CLIPPED-RECT blit, not a full-page copy

**Supersedes the "host NOP leaf" wording above and the earlier "menu auto-exit / stack
corruption" hypothesis — both were wrong.** The host title-path shim
(`screens_host_restore_bg_view`) is no longer a NOP: it routes the engine's 2-arg
`restore_bg_view(view, seg)` to `host_compose_bg_view(view)` (host_render.c), which reads the
descriptor's source far ptr (`view+0x02/+0x04`) and calls the real 3-arg `restore_bg_view`
(bgi_overlay.c) to compose the title/menu/text background into `host_framebuffer`. Verified
(external BIOS-scratch probes, BUMPYCAP `DGROUP=0x0040`): boot is monotonic-forward into
`run_main_menu`, which **reaches and waits correctly** — there is no auto-exit and no stack
corruption (the prior diagnosis was an artifact of in-process DIAG writes perturbing an
uninitialized-memory-sensitive layout).

**Root cause of the speckle:** the 3-arg `restore_bg_view` modelled the erase as a blind
plane-by-plane PAGE_SIZE (8000 B) copy, ignoring the descriptor's blit rectangle. The original
(`1000:80bc`, decomp-confirmed in `run_main_menu` @ `1000:35a5`) is a **clipped-rect blit**: the
descriptor carries `width`(`+0x0a`)×`height`(`+0x0c`) tiles at dest origin (`+0x14`,`+0x16`).
`run_main_menu`'s loop redraws ONLY a 6×2-tile option strip at tile (0xb,0x12) each iteration; the
full-page copy instead smeared a full page sourced from that small (and, for the option strip,
uninitialised) descriptor over the entire screen every frame → full-screen speckle.

**Fix** (`bgi_overlay.c`, `#ifdef BUMPY_PLAYABLE`): `restore_bg_view` now copies only the
descriptor's `+0x0a`×`+0x0c` tile rectangle (src packed at its own width; dest VGA-page row
stride 40 at the tile origin). This is a strict generalization — the full-screen background
descriptor (20×25 @ 0,0) reduces byte-for-byte to the old 200×40-per-plane copy, and
`show_object_preview_wait_input`'s 20×1 strip now blits its row instead of over-reading. The
copy extent keys on `+0x0a/+0x0c` (the source dims, set by every `word0e≤1` call site), NOT the
`+0x1e/+0x20` "extent" fields (those belong to the mode-10 `render_player_view` path). Result:
`PLAY / HIGH-SCORE / LEVEL: / PASSWORD` render cleanly; full-screen nonzero bytes drop 28957→18371
(the original menu measures 18991).

**Default `BUMPY.EXE` unaffected:** the clip-aware body is `#ifdef BUMPY_PLAYABLE`; the default
build keeps the verbatim full-page `#else` copy, so `bgi_overlay.obj` is byte-stable (md5
`cac9ff236a832284fec6fafff2d8602b`). The default build's `restore_bg_view` is byte-compared but
never executed, so its behavioral content is immaterial there. The gameplay erase/anim-channel
path and the composite ctests reach `restore_bg_view` only with `word0e>1` (NOP guard) views, so
they are untouched (`validate_composite.sh` green).

**Option-2 difficulty strips (EASY / MEDIUM / HARD) — loaded from the original at runtime.**
The cycling "LEVEL :" value beside the menu is a 6×2-tile (96×16, 16-colour planar, 768 B) strip;
`run_main_menu` blits one of three via `restore_bg_view`, the source far ptrs coming from the
static DGROUP table at `+0x75e` (off) / `+0x760` (seg), indexed by `menu_option2_setting`
(setting 0→`0x8b88`=EASY, 1→`0x824e`=MEDIUM, 2→`0x8582`=HARD).  Those DGROUP offsets are **BSS**
(zero in the EXE image): in the engine the three strips are RUNTIME-DECODED sprite frames that the
sprite pipeline writes into DGROUP by the time `run_main_menu` runs.  The reconstruction's sprite
path does not reproduce that particular DGROUP decode (`process_sprites`/`prepare_sprite_frames`
decode the p1/p2/hud sprite OBJECTS into a different scratch region, not these strips), so in the
playable build `menu_opt2_img_off/seg[]` would point at meaningless offsets → garbage.

The strips are copyright game data, so they are NOT embedded in `src/`.  Instead the host LOADS
them from `MENUDIFF.BIN` — a sidecar (3×768 B, setting order EASY/MEDIUM/HARD) the user generates
from their OWN original via `tools/dosbox/extract_menu_strips.sh` (which boots the original under
the instrumented dosbox-x to `run_main_menu`, reads the live `+0x75e` pointer table, and dumps the
three populated strips via the BUMPYCAP memory-dump hook, `tools/dosbox/patches/04-bumpycap-memdump.patch`).
The sidecar lives under the git-ignored `local/` tree and is never committed.  At startup
`host_load_menu_strips` (src/host/host_resource.c, `BUMPY_PLAYABLE` only) reads it into a far buffer
and points `menu_opt2_img_off/seg[]` at the three blobs; if the sidecar is absent the buffer stays
zero → blank strip (the menu still renders).  The captured bytes are exactly the planar layout the
clip-aware `restore_bg_view` consumes (plane-major, 12 B/row × 16 rows × 4 planes).  The default
`BUMPY.EXE` build is unaffected (host_resource.c is `BUMPY_PLAYABLE`-only; md5
`cac9ff236a832284fec6fafff2d8602b`).  Verified: the playable menu renders "LEVEL : EASY".

### RESOLVED (title-logo "rainbow") — a screenshot-decoder artifact, NOT a game bug

The playable's title/menu logo (`TITRE.VEC`) appeared rainbow-banded in PNG screenshots, which
looked like a wrong palette.  It is **not** a rendering bug — the playable composes the screen and
uploads the DAC correctly; the offline screenshot decoder (`tools/dosbox/shot_to_png.py`) was
mis-decoding it.

**Root cause.** A mode-0x0D pixel value is not a direct DAC index: the VGA resolves it as
`DAC[ AC[pixel] ]`, where `AC` is the 16 Attribute Controller palette registers.  The engine's BGI
init (reconstructed as `host_set_bgi_attribute_palette`, `host_video.c`, called once from
`init_display_97f1` after the BIOS mode-set) programs `AC[v] = v` for `v<8` and `AC[v] = 0x10+(v-8)`
for `v≥8`, and `vga_dac_upload_from_buffer` loads the image's 16-colour palette into DAC slots
`0..7` / `0x10..0x17` (leaving `8..15` at the BIOS-default EGA ramp).  So pixels with value `8..15`
resolve to DAC `0x10..0x17` (the image colours).  `shot_to_png.py` decoded `DAC[pixel]` directly,
reading the leftover EGA-default ramp (which contains reds) for every pixel `8..15` → the gold
`BUMPY'S` logo banded into blue/red/green.  Confirmed: re-decoding the *same* shot with the AC stage
applied yields the gold logo, pixel-matching the pure-Python ground truth (`vec_to_png.py`, validated
99.95% vs a real DOSBox capture); `TITRE.VEC`'s own embedded palette IS that blue→gold gradient, and
the playable's dumped DAC holds it split across `0..7`/`0x10..0x17` exactly as expected.

**Fix (tooling only).** `tools/dosbox/patches/05-bumpycap-shot-attr-palette.patch` appends the real
`vga.attr.palette[0..15]` to the `BUMPYCAP_SHOT` dump, and `shot_to_png.py` now resolves
`DAC[ AC[pixel] ]` using the dumped AC (falling back to the engine's standard BGI map for older
dumps).  This is faithful for **both** the playable (VGA: AC dumped as `{0..7,0x10..0x17}`, logo
renders gold) **and** the original in EGA mode, which programs an *image-specific* AC
(e.g. `{0,0,0,0,0,0,0,2,0xa,4,6,6,0xc,0xe,0xf,0xf}`) that maps the gradient-sky indices to black —
so the original EGA menu correctly decodes as a black background + red/white logo (its intended EGA
look), where the old hard-coded BGI map would have rainbowed it.  No game code changed; `BUMPY.EXE`
and `BUMPYP.EXE` are untouched.

**Remaining (documented, out of scope).** Booted in EGA mode (`F2`), the *playable* still uploads
`TITRE.VEC`'s VGA palette (its `dispatch_by_palette_mode` shim always reads `+0x33`) and uses the
fixed BGI AC, so it shows the VGA gold look rather than the original's EGA black/red.  The playable's
default and validated path is VGA (`F3` → `palette_mode=2`), where it matches; the EGA-mode palette
divergence is a non-default corner case left as-is.

### Task 11 — scripted pixel frame-compare gate (`tools/validate_playable.sh`)

The Plan-A integration gate runs BOTH `BUMPYP.EXE` and the **real original** `BUMPY.EXE`
under one instrumented DOSBox-X build, captures the rendered VGA A000 4-plane framebuffer
per per-tick frame (patch `03-framebuffer-capture.patch`), and compares them plane-for-plane
(`tools/fb_compare.c`, with a bounded whole-frame phase shift to absorb page-flip phase).
Scope is the Tier-1 **idle in-level** compare: each build is driven to level 1 by its own
boot script (the original through F2/F5, the playable skipping them), then N per-tick frames
are dumped with no further input — the per-tick pipeline (render/draw/anim/present/flip) runs
fully, so it exercises the host present/flip glue against the original's direct-VGA output.

**Status: scaffolded, original-side validated, NOT yet green.** The original capture produces
32 frames that decode to a real level render. Two genuine harness bugs were found and fixed:
(1) DOSBox-X does not strip inline `#` comments, so the template conf's `machine = vgaonly  # …`
corrupted the machine type and stalled the playable boot at a BIOS wait (`f000:8db4`) — the gate
now emits a comment-free conf (`mk_conf`); (2) dosbox ignored `timeout`'s SIGTERM and orphaned,
holding the pipe — hardened to `timeout -k`.

**Remaining (documented divergence from a green gate):** the playable per-tick FB-capture trigger
CS:IP is not yet calibrated. The playable is a relinked image with a non-trivial code-segment
layout — its gameplay code runs at runtime segment `ba51` (the boot/menu code is at `0824`), and
`game_loop`'s per-tick loop-top is NOT at the linker-map code offset within that segment
(`0824:1639` and `ba51:1639` both capture 0 frames). Pinning it requires an instruction-level
CS:IP probe of the running playable `game_loop` (the analogue of the original's `rng=rand()` site
`0824:0cda`). Until then the playable capture yields 0 frames and the gate FAILs at "playable
capture produced no frames". This is a **validation-harness calibration** gap only: reconstruction
correctness is already gated tick-for-tick by the int8 end-to-end gate and byte-exact by the
composite/host-compose gates; the pixel frame-compare is additional present/flip-glue assurance.

### VERIFIED+FIXED (Task-11 interactive bring-up — the mode-4 NULL P2-handler crash)

Running `BUMPYP.EXE` interactively (real keyboard) revealed that the earlier "reaches the
per-tick `game_tick()` loop / `mode=4` holds" result was a **false positive**: `game_mode` flips
to 4, then the very first P2 per-tick dispatch jumps through a NULL pointer and the CPU storms
INT6 (invalid opcode) forever — no image, just the runaway error counter the user saw.

Root cause (traced via a dosbox CS:IP ring buffer at the wild jump): `p2_run_move_state_handler`
(player2.c, 1000:5003) does `(*p2_state_handler_tbl[p2_move_state])()` — an indirect FAR call
through the DGROUP 0x085c shadow table — once `p2_step_idx==5`. The disasm at the fault is
`les bx, p2_state_handler_tbl ; add bx, move_state*4 ; call far [bx]`, and the table base was
**NULL** → `call far [0000:state*4]` executes the IVT/wild memory (landed at the all-`0xFF`
`BA51:1218`). The engine populates that table at level/round init; the reconstruction keeps it a
host-seeded far shadow (player2.c decl) and — like the anim-channel slot tables — leaves the
WIRING to the caller. The default `BUMPY.EXE` + its `tools/p2_ctest.c` seed it; the playable
build had no caller, so it stayed NULL.

Fix (`#ifdef`-free, host-only `view_setup.c init_sprite_structs`, alongside the anim-slot wiring):
seed a 16-slot far-ptr table `[1]=p2_cell_move_up [2]=down [3]=left [4]=right` (verbatim the
indices `seed_state_handler_tbl` in p2_ctest uses) and point `p2_state_handler_tbl` at it. The
INT6 storm is eliminated (5.7M hits → 0). NOTE: full headless mode-4 gameplay re-verification is
pending — recompiling shifts the BUMPYCAP injection calibration (DGROUP/keytbl offsets) so the
scripted menu-drive needs re-derivation; interactive keyboard play (which needs no calibration)
is the confirmation. Default `BUMPY.EXE` unchanged (view_setup.c is play-only; md5 cac9ff23).

UPDATE (hardening + audit): the first fix seeded only p2_state_handler_tbl[1..4] (the indices
tools/p2_ctest.c's captured scenario exercises).  Interactive play still crashed — P2 is
AI-controlled (active even in 1-player), so its move_state at dispatch can be an index outside
1..4 (0, or the engine's other states), which was still a NULL slot → the same INT6 storm.
HARDENED: seed ALL 16 slots — [1..4] = the four p2_cell_move_* handlers, the rest = a safe host
no-op (hv_p2_state_noop) — so no slot can be a NULL far-call.  DIVERGENCE: faithful values for the
non-1..4 slots are unknown; the no-op prevents the crash (documented).  AUDIT: a disassembly scan of
ALL playable objects for indirect far-call dispatch (`call dword ptr [...]`) finds p2_state_handler_tbl
(player2.obj 049E) is the ONLY such table in the gameplay path — P1's game_mode_handlers[] is a
fully-initialised C array, the move-step dispatch goes through move_step_handler_for_offset, and the
0x870 P2 path (p2_dispatch_move_state_handler) is a clean empty stub.  So this fully closes the
wild-far-call (INT6) crash class.  NOTE: not yet re-verified in headless gameplay — the scripted-input
calibration (DGROUP/keytbl offsets) drifts every recompile, leaving the automated drive stuck at the
title wait; interactive keyboard play is the confirmation.  Default BUMPY.EXE unchanged (cac9ff23).

### FIXED (render: blank screen — the title/menu/text backgrounds never composed)

Running the playable build showed an entirely BLANK screen (VGA memory all-zero) even at the
title — not a display issue: nothing was being drawn.  Root cause: the Task-9 title-present
fix replaced screens.c's `restore_bg_view(view,seg)` with a host shim that was a pure NOP, on
the (incorrect) assumption that `present_frame(1)` alone would show the title.  But
`restore_bg_view` is the COMPOSE step — it copies the freshly `vec_decode`'d fullscreen image
(`fullscreen_buf`, at descriptor+0x02/+0x04, `word0e==1`→page A000) into the displayed page.
NOP'd out, `host_framebuffer` stayed zero, so `present_frame` faithfully copied a blank image
to VGA.  This blanked EVERY screen built that way (title, menu, level-intro, highscore).
FIX: `host_compose_bg_view` (host_render.c) drives the real 3-arg `restore_bg_view`
(planes=`host_framebuffer`, source from the descriptor) so the background actually composes;
screens.c's 2-arg shim now routes there instead of NOP.  Constants align exactly
(BGI_OVL_PLANE_SIZE==HOST_PLANE_SIZE==0x10000, BGI_OVL_PAGE_SIZE==VGA_PLANE_BYTES==0x1F40), so
the blit lands where present_frame reads.  VERIFIED: VGA non-zero bytes 0 → 57948 at the title.
Dynamic glyph text (draw_string_glyphs_9804) is still a separate stub (next).  Default
BUMPY.EXE byte-identical (cac9ff23; all host-only).

### Playable host: resource loader + .VEC raster decode (host_resource.c)

The default build stubs the whole resource pipeline (open_resource / read_chunked / c_close /
set_resource_table / vec_decode are faithful-signature NOPs), so the playable build composed an
UNINITIALISED fullscreen_buf → RGB noise on every screen.  host_resource.c (#ifdef BUMPY_PLAYABLE)
un-stubs it: open_resource maps the engine's vec_resource_table index (docs/data-files.md load
order) to a filename and opens it via the real DOS I/O (dosio.c); read_chunked reads it; vec_decode
drives the already-reconstructed vec_decode_planar and lays the result where the screen builders
expect (palette @+0x33, plane-major raster @+99).  host_screens_buf_init backs fullscreen_buf with a
halloc'd buffer (the engine allocates it at boot; the reconstruction only read it) — allocated
BEFORE host_fb_init's 256 KB halloc (else the heap is exhausted and the read lands at 0000:0000),
and SEGMENT-ALIGNED (offset 0) because DOS INT 21h/3Fh wraps/truncates a read whose buffer
offset+length crosses 64 KB.  RECONSTRUCTION FIDELITY divergence: the engine walks a DGROUP vec_res
descriptor table (name far-ptr + QUELDISK disk-id) with a floppy disk-swap retry; the host
shortcuts to a static filename table + direct dosio open (data is mounted, no copy-protection).

STATUS: file I/O + read + buffer all VERIFIED working (the game opens SCORE/MASKBUMP/BUMPRESE.VEC and
the read lands in the aligned buffer).  vec_decode renders FULLSCREEN-RASTER .VEC (TITRE.VEC, the
MONDE*.VEC level backgrounds — header 99 / palette @51 / planar @99).  The title-flow's first
composed resources (SCORE.VEC, BUMPRESE.VEC) are STRUCTURED .VEC command streams (not rasters), so
they still compose blank — they need the .VEC vector/op12 interpreter (the BGI-overlay command
machinery), the larger remaining render piece.  Default BUMPY.EXE byte-identical (cac9ff23).

### VERIFIED (boot graphics/sound select screens — config_screens.c)

These two screens are reconstructed ENGINE functions (not host platform glue), so they live
in `src/config_screens.c`, not `src/host/` — built only into the playable image.

`gfx_driver_init` is a faithful 1:1 reconstruction of `gfx_driver_init` (1ab9:02ce), the
BGI-overlay routine the original runs at boot to choose the display adapter / palette mode.
The original does not cleanly decompile (inline `swi(0x10)`/`swi(0x21)`); the reconstruction
mirrors its disassembly exactly: `int 10h AX=0002h` (text mode 0x02) → cursor (1,1) + DOS
print "BUMPY (C) LORICIEL 1992$" → loop the 6-entry adapter table at DGROUP 0x548b
(`{0,1,1,0,0,0}`), printing each present adapter's 15-byte menu line (base 0x52b4) at col 33
from row 10 → poll F2 (0x3c) → `palette_mode`=1 / F3 (0x3d) → `palette_mode`=2.  Because only
table entries 1 and 2 are set, the screen shows exactly two lines, "< F2 >: EGA" and
"< F3 >: VGA".  All strings + the table are verbatim from the binary.

An EARLIER version was a placeholder that invented the screen (text mode 0x03, a fixed 6-line
"< F1 >: CGA … < F6 >: VGA256" menu, and a fabricated "< F5 >: NO SOUND … < F8 >: MT32" sound
screen).  That was wrong on mode, layout, which adapters appear, and accepted keys, and the
sound screen does not exist in the original at all — `sound_select_device` (1000:6de3) draws
NO screen (silent device-mask init).  The placeholder is replaced by this reconstruction and
the invented audio screen is removed.

VERIFIED headless: BUMPYP.EXE flips to BIOS video mode 0x02 (frame 61), then parks at the
BIOS INT 16h keyboard wait (CS:IP f000:cf4x) for 1200+ stable frames — the DOS prints ran and
it is waiting for F2/F3.  Two documented host divergences (see the Playable-host section
above): input via BIOS INT 16h instead of the engine's `get_key_state` table (the INT9 ISR is
not installed until `init_game_session_state`, which runs after this screen), and the shipped
static adapter table instead of the live `detect_video_adapter` probe.  Default BUMPY.EXE
byte-identical (cac9ff23).

### FIXED (render: TITRE title/menu raster now decodes + composes + presents in colour)

Three coupled host bugs left the playable build blank/garbage past the config screens; all
three are now fixed and the TITRE.VEC title-menu raster renders (verified headless: clean
blue-sky background, gold-banded "BUMPY'S" logo, "PLAY / HIGH-SCORE / LEVEL: EASY / PASSWORD"
menu text).  Default BUMPY.EXE byte-identical (cac9ff23); all changes are host-only / behind
`#ifdef BUMPY_PLAYABLE`.

1. **Resource-table base switching (`host_resource.c`).**  The host `set_resource_table` was a
   NOP and `open_resource` always indexed the 0x932 vec table, so `run_main_menu`'s
   `open_resource(0x12)` (TITRE) fell off the end and returned −1.  The engine keeps ONE
   contiguous DGROUP resource array and `set_resource_table(off,seg)` selects the base entry:
   off 0x932 → MASKBUMP.VEC, off 0x928 → BUMSPJEU.BIN (one 10-byte entry earlier).  The host
   now models the array from its earliest base (`hr_full_files[19]`, BUMSPJEU prepended) and
   `set_resource_table` selects `hr_base_idx` (0x928→0, 0x932→1); `open_resource` indexes
   `hr_base_idx + res_idx`.  The unused 89 KB BUMSPJEU sprite-bank read (`process_sprites` is a
   host NOP) targets the literal seg 0xa0c8 (== VGA memory) — `read_chunked` DRAINS it to a
   4 KB discard buffer instead of smashing memory.

2. **Framebuffer allocation order + `fullscreen_buf` in framebuffer slack (`main.c`,
   `host_resource.c`, `host_video.c`).**  Conventional memory leaves only ~8 KB over the
   program + the 256 KB `host_framebuffer` halloc, so a SEPARATE 34 KB `fullscreen_buf` halloc
   (the old `host_screens_buf_init`, allocated first) fragmented the heap and `host_fb_init`'s
   256 KB request returned NULL → `present_frame` silently NOP'd → blank.  (Measured: largest
   contiguous halloc was 248 KB < 256 KB.)  FIX: allocate the framebuffer FIRST (unfragmented
   heap), then point `fullscreen_buf` at plane 0's permanently-unused slack
   `[0x4000..0x10000)` inside the framebuffer — NO second allocation.  `clear_viewport` is
   bounded to each plane's display extent `[0..0x4000)` (covers page 0 + page 1) so it never
   wipes that window; compose/present only touch `[0..0x3f40]`.  This SUPERSEDES the
   "allocated BEFORE host_fb_init" note in the resource-loader section above.

3. **BGI palette: Attribute Controller + DAC upload (`host_video.c`, `screens.c`).**
   The decoded image's palette was never sent to the DAC, so screens showed the BIOS
   mode-0x0D default ramp.  FIX: (a) `host_set_bgi_attribute_palette` (one-time, in
   `init_display_97f1`) programs the AC palette to the BGI 16-colour mapping — pixel i →
   DAC (i<8 ? i : 0x10+(i−8)) — matching the DAC targets (0..7, 0x10..0x17) of
   `host_bgi_upload_palette_to_dac` / `vga_dac_upload_from_buffer`; the BIOS default AC
   otherwise maps pixel 6→DAC 0x14 and 8..15→DAC 0x38..0x3f, which the image palette never
   loads.  (b) the menu/title DAC upload flows through `fun_7bca_flip` →
   `host_bgi_upload_palette_to_dac` (Task 1, staged by `fun_7b93_present_blank`).
   RECONSTRUCTION FIDELITY: these are host BGI-init reconstructions (the original BGI
   mode/palette handler is not decompilable); the DAC write SEQUENCE stays the
   corpus-validated `host_bgi_upload_palette_to_dac` (slots 0..7, 0x10..0x17).

4. **Level-palette pipeline + vsync-wait misnomer correction (`host_video.c`, `level.c`,
   `screens.c`) — Task 2.**  Two corrections:
   (a) **MISNOMER.** `upload_vga_dac_palette` (`1000:9864`) / `dispatch_by_palette_mode`
   (`2036:0000`) were NOT a DAC upload — they are a **vsync WAIT**.  The mode-2 overlay
   handler (`2036:0015`, table `[pm*2+0x6976][2]`) polls Input Status #1 (`0x3da`) bit 3:
   wait for vertical retrace to start, then end.  Renamed to `wait_vretrace_thunk` /
   `wait_vretrace_dispatch` (src + Ghidra).  The vsync poll body is `#ifdef BUMPY_PLAYABLE`
   in `wait_vretrace_dispatch`; the default build keeps its verbatim NOP (md5 unchanged).
   The overlay table is runtime-populated and `2036:0015` is not in the corpus, so the
   `9864→2036:0000→2036:0015` chain is collapsed into the poll (behavior-faithful).
   (b) **`load_palette` (`1000:08d1`) reconstructed** (host body in `host_video.c`,
   `BUMPY_PLAYABLE`).  Engine: decode 16 packed RGB words from `[src:0x578]` into a staging
   buffer (DGROUP `0x6c42`, palette @+0x33; each channel `<<3`; biases `DAT_75eb`/`DAT_75ec`
   are init'd to 0 and never re-written, so 0 in the gameplay path), then stage
   (`7b93`)→DAC upload (`7bca`)→vsync (`9864`).  `apply_level_palette` (`1000:0604`) calls
   `load_palette(0x578,0x203b)`; the host's `apply_level_palette` now does the same instead
   of the old (misnamed) `upload_vga_dac_palette` NOP.
   **DATA-SOURCING DEVIATION:** the host never stages the packed palette at DGROUP `0x578`
   (`level_populate_dg` fills only the entity frametable; `load_palette_byteswapped`
   (`1000:063b`), whose sole job is to fill `0x578` byte-swapped from `cur_level_ptr`, is
   therefore vestigial and OMITTED).  The host has the already-DECODED 48-byte DAC palette
   at `g_pav_buf+51` (the engine's per-idx decode produces exactly these 48 bytes), exposed
   via `level_pav_palette()`; the host copies those into the staging buffer +0x33 (skipping
   the packed-word decode) then runs the faithful stage→upload→vsync tail.
   **DAC-LAYOUT RECONCILIATION:** `load_palette`→`host_bgi_upload_palette_to_dac` writes the
   level palette to DAC slots {0..7, 0x10..0x17} — the slots the active BGI AC mapping reads
   — so gameplay colours 8..15 (AC→DAC 0x10..0x17) are correct.  `render_level`'s
   `video_set_palette6` (DAC 0..15 contiguous) is KEPT (the structurally-validated path); it
   covers only the AC's low 8 slots, so the two COEXIST: `video_set_palette6` writes 0..15
   (8..15 unread by the AC) and `load_palette` writes the AC-read {0..7, 0x10..0x17}.  The
   faithful path is what makes colours 8..15 resolve correctly under the BGI AC.

REMAINING (not yet fixed): `run_main_menu` returns 1 almost immediately on its first poll
(so the normal flow falls into a blank `show_highscore_screen` instead of holding the menu) —
under investigation; `read_input_action` returns 0 with all keys up, so the cause is a spurious
key/state or a sprite-blit side effect, not the interpreter.  The title PRESENTATION screen
(`init_title_graphics`: MASKBUMP + `process_sprites` STUB + placeholder sprite far-ptrs) renders
speckled because the sprite overlay is stubbed.  Gameplay/level rendering needs a SECOND 256 KB
plane buffer (`level.c` `g_planes`) that cannot coexist with `host_framebuffer` in 640 KB — a
separate memory problem.  The clean TITRE menu raster itself is confirmed rendering correctly.
