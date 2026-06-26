# Faithfulness gap audit — every original function not yet a faithful 1:1

**Purpose.** The leading tenet (CLAUDE.md) is a structure-faithful decompilation: one C
function per original function, same control flow and data layout. This document is the
**complete inventory of every original function that is currently NOT a faithful 1:1** in
`src/` — stubbed (no-op), merged into a host shim, modeled behaviorally, or entirely
unreconstructed — together with each one's Ghidra address, whether the original *uses* it,
whether it is *decompilable*, and the action required.

It exists because a stub for a function the game actually runs is, by the tenet, not
faithful (and breaks behavior). Everything here is either work to do or a deviation that
must stay documented (genuinely redundant, or self-modifying code that does not decompile).

**Decisions (agreed).**
- **BGI graphics-driver overlay (seg `1ab9`) → reimplement in the host module
  (`src/host/`)**, performing the *same functionality* as the original driver. It is a
  third-party graphics library, not the game's own logic (same category as the CRT), so it
  belongs in the host platform layer rather than as a 1:1 `src/` mirror. The game's
  call sites (the main-segment thunks + the `palette_mode` dispatch) stay so graphics is
  *invoked* exactly as the original; the host body produces the same observable result.
- **Borland C runtime (seg `1000` tail) → migrate to the Open Watcom runtime.** Do not
  reconstruct 1:1; rely on the toolchain's equivalents and **document any behavioral
  divergence** between the Borland and Open Watcom runtimes where it could affect the game.
- **Self-modifying planar blit cores + `init_round_state`** → remain documented carve-outs
  (behavior-faithful / stub) — the only entries here that are not reconstruction work.

**Method.** Ghidra `list_functions` (399 functions across the segments below) diffed against
the addresses cited in `src/`. A function counts as *reconstructed* only when `src/` carries
its real body; *modeled* means a host shim reproduces its effect without mirroring its
structure; *stub* means a no-op; *missing* means no `src/` presence at all.

**Segment map (Ghidra):**

| Seg | Role | Reconstruction status |
|-----|------|-----------------------|
| `1000` CODE_0 | main game logic + Borland CRT | game logic mostly 1:1; CRT redundant (see §6) |
| `1ab9` CODE_1 | **BGI graphics-driver overlay** | **largely unreconstructed (§1)** |
| `1c28` CODE_2 | `.VEC` interpreter (`vec_run`) | reconstructed (vec.c / tools) |
| `1cd5/1cda/1ce5` | seg-alloc, `vec_xform`, PRNG | reconstructed |
| `1cec` CODE_6 | **sprite pipeline** | **partial (§2)** |
| `202c/2036` | sound-select / palette-dispatch screens | reconstructed (config_screens.c / screens.c) |
| `203b` | DGROUP (data) | n/a |

**Headline.** The *game logic* (physics, AI, collision, the per-tick spine, menu/title state
machines) is reconstructed and in large part validated. The **graphics + IO substrate is the
real gap**: the engine's BGI graphics-driver overlay (seg `1ab9`, ~27 fns) is almost entirely
unreconstructed and is instead *modeled* by a few host shims (`restore_bg_view`,
`render_player_view`, `present_frame`), and the sprite pipeline (seg `1cec`) is only half
ported. This is the root of the title/menu offset/jitter/pacing symptoms and the reason
several presentation primitives are no-ops.

---

## §1 — BGI graphics-driver overlay (seg `1ab9`) — PRIMARY GAP

> ⚠️ **CORRECTION (2026-06-27): the role labels below for `7b93`/`7bca` are MISNOMERS.**
> Capture-driven RE of the actual VGA (`palette_mode==2`) handlers (disassembled from the
> binary; recorded as Ghidra disassembly comments at the handler addresses) proved:
> - `1ab9:01e1` (`bgi_stage_palette_dispatch`, thunk `7b93`, VGA handler `1ab9:0620`) is a
>   **16-colour palette stage** — `rep movsw` of the 48-byte palette from `buf+0x33` into the
>   current draw-object's per-**page** slot (`*0x5311 + page*99 + 0x33`). NOT a pixel putimage.
> - `1ab9:02b1` (`bgi_upload_palette_to_dac_dispatch`, thunk `7bca`, VGA handler `1ab9:0677`)
>   is a **DAC palette upload** (ports `0x3c8`/`0x3c9`, slots 0–7 & 0x10–0x17). NOT a page flip.
> - `1ab9:0351` (`bgi_present_dispatch`, `present_frame` `7bdd`, VGA handler `1ab9:0379` →
>   `1ab9:06c1`) is **the one true CRTC page flip** (`XOR` Start-Address bit 5 = 0x2000 swap).
> - `1ab9:0179` (`bgi_init_viewport`, thunk `7b4a`) has a **null** VGA blit slot
>   (`0x4dda[2]==0`): it sets the clip/viewport extent only — no pixel blit on VGA.
>
> Reconstruction is tracked in `docs/superpowers/plans/2026-06-27-host-bgi-present-transitions.md`
> (rewritten). This table's per-row "Role"/"Action" text is rewritten by that plan's Task 4.

The engine's graphics primitives are a Borland-BGI-style driver overlay. Main-segment
**thunks** (`1000:7b4a`…) call **dispatch** functions here, which indirect-call a
**runtime vector table** indexed by `palette_mode` (DGROUP `0x4dda`/`0x5435`/`0x5441`/
`0x5475`/`0x555e`), reaching the per-mode handler. All of `1ab9` is in the corpus and
decompiles, **except** the innermost self-modifying planar blit cores (the `bgi_set_mode_*`
→ `1ab9:0aa0`-family targets), which is the one legitimate behavior-faithful carve-out
(already done once as `restore_bg_view`).

| Addr | Ghidra name | Role | `src/` status | Decompiles? | Action |
|------|-------------|------|---------------|-------------|--------|
| `1ab9:0179` | bgi_init_viewport | set viewport 0x14×0x19, dispatch `[pm*2+0x4dda]` | missing (thunk `1000:7b4a` = NOP) | yes | reconstruct 1:1 |
| `1ab9:01e1` | bgi_putimage_dispatch | putimage via `[pm*2+0x5435]` | missing (thunk `7b93` = NOP) | yes | reconstruct 1:1 |
| `1ab9:01ff` | bgi_cleardevice_dispatch | cleardevice via vector | missing | yes | reconstruct 1:1 |
| `1ab9:0232` | bgi_device_reset_dispatch | device reset via vector | missing (thunk `7bbd`=NOP) | yes | reconstruct 1:1 |
| `1ab9:02b1` | bgi_palette_dispatch (page flip) | page-flip via `[pm*2+0x5441]` | missing (thunk `7bca`=NOP) | yes | reconstruct 1:1 |
| `1ab9:0351` | bgi_present_dispatch | present via `[pm*2+0x5475]` | missing | yes | reconstruct 1:1 |
| `1ab9:0384` | bgi_device_inc_dispatch | device-inc via vector | missing (thunk `7bea`=NOP) | yes | reconstruct 1:1 |
| `1ab9:01c0` | bgi_driver_nop | driver no-op slot | missing | yes | reconstruct 1:1 (trivial) |
| `1ab9:01c1` | bgi_device_clear_flag | clear device flag | missing | yes | reconstruct 1:1 |
| `1ab9:021b` | gfx_set_current_pos | set current draw pos | missing | yes | reconstruct 1:1 |
| `1ab9:0a73` | bgi_set_mode_00 | CGA-mode handler | missing | partial (self-mod core) | reconstruct dispatch; blit core behavior-faithful |
| `1ab9:0d77` | bgi_set_mode_01 | bg/erase handler → `0aa0` masked blit | **modeled** by `restore_bg_view` | partial | un-merge: real `bgi_set_mode_01` + behavior-faithful blit core |
| `1ab9:1028` | bgi_set_mode_10 | player-view handler | **modeled** by `render_player_view` | partial | un-merge similarly |
| `1ab9:126e` | bgi_set_mode_11 | mode-11 handler | partial citation | partial | reconstruct dispatch + behavior-faithful core |
| `1ab9:12b0` | bgi_char_width | glyph width | missing | yes | reconstruct 1:1 |
| `1ab9:1311` | bgi_text_render_dispatch | text render dispatch | missing | yes | reconstruct 1:1 |
| `1ab9:132b` | bgi_set_current_object | select current draw object | missing (thunk `97d5`) | yes | reconstruct 1:1 |
| `1ab9:137b` | bgi_draw_sequence | draw a sequence | missing | yes | reconstruct 1:1 |
| `1ab9:13bc` | draw_char_glyph | render one glyph | missing (thunk `97f7`) | yes | reconstruct 1:1 |
| `1ab9:13ec` | draw_string_glyphs | render glyph string | missing (`9804`/screens.c NOP) | yes | reconstruct 1:1 |
| `1ab9:1409` | bgi_set_text_mode | text mode | missing | yes | reconstruct 1:1 |
| `1ab9:1422` | bgi_set_clip_rect | clip rect | missing | yes | reconstruct 1:1 |
| `1ab9:1441` | bgi_set_text_position | text position | missing | yes | reconstruct 1:1 |
| `1ab9:1458` | bgi_set_text_attr | text attributes | missing (thunk `9847`) | yes | reconstruct 1:1 |
| `1ab9:146b` | measure_string_width | string width (thunk `9854`) | missing | yes | reconstruct 1:1 |
| `1ab9:14d3` | font_glyph_ptr | glyph-data pointer | missing | yes | reconstruct 1:1 |
| `1ab9:02ce` | gfx_driver_init | adapter/palette select screen | **reconstructed** (config_screens.c) | yes | — |

**Also required (the wiring):** the main-segment dispatch thunks
`1000:7b4a/7b76/7b86/7b93/7ba7/7bbd/7bca/7bea` (all NOP today), the per-`palette_mode`
**vector tables** (DGROUP `0x4dda/0x5435/0x5441/0x5475/0x555e`), and the **BGI-init code that
populates them** (currently absent — `init_misc_7bd7`/`init_misc_7bbd` are NOP). Without the
table population the dispatch indirection cannot resolve.

---

## §2 — Sprite pipeline (seg `1cec`) — PARTIAL

The blit chain is reconstructed (`sprite.c`/`sprite_blit.c`/`sprite_chain.c`); the decode/
dispatch front-end is not. `sprite_chain` **merges** `object_list`+`clip`+`setup` (a documented
deviation to un-merge).

| Addr | Ghidra name | `src/` status | Decompiles? | Action |
|------|-------------|---------------|-------------|--------|
| `1cec:00aa` | decode_2bpp_planes | missing | yes | reconstruct 1:1 |
| `1cec:0182` | sprite_rle_decode | missing | yes | reconstruct 1:1 (sprite-bank decode) |
| `1cec:01bd` | sprite_rle_encode | missing | yes | reconstruct 1:1 |
| `1cec:0e29` | sprite_blit_dispatch | missing | yes | reconstruct 1:1 |
| `1cec:0e34` | sprite_blit_store_obj | missing | yes | reconstruct 1:1 |
| `1cec:0e48` | sprite_blit_object_list | merged into sprite_chain | yes | un-merge to 1:1 |
| `1cec:0f50` | sprite_blit_clip | merged into sprite_chain | yes | un-merge to 1:1 |
| `1cec:103d` | sprite_blit_setup | merged into sprite_chain | yes | un-merge to 1:1 |
| `1cec:10e1` | sprite_blit_planar_vga | reconstructed (sprite_blit.c, behavior-faithful) | partial (self-mod) | keep behavior-faithful; document |
| `1cec:2ced` | sprite_proc_dispatch | missing (`process_sprites` `1000:93d8` = NOP) | yes | reconstruct 1:1 |
| `1cec:2d15` | dispatch_by_palette_mode_2d15 | missing | yes | reconstruct 1:1 |
| `1cec:2d43` | dispatch_by_palette_mode_2d43 | missing | yes | reconstruct 1:1 |
| `1cec:2d6d` | dispatch_palette_mode_with_src_ptr | cited (host_render.c model) | yes | reconstruct 1:1 |
| `1cec:2da8` | dispatch_by_palette_mode_2da8 | missing | yes | reconstruct 1:1 |
| `1cec:2dd2` | set_sprite_table_ptr | cited; `1000:9410` thunk = NOP | yes | reconstruct 1:1 |
| `1cec:2ded` | prepare_sprite_frames | cited (model) | yes | reconstruct 1:1 |
| `1cec:3137` | build_bit_reverse_lut | missing (thunk `9424`) | yes | reconstruct 1:1 |
| `1cec:31b7` | blit_sprite_vga | missing | partial (self-mod) | behavior-faithful core; document |

Entry/thunk no-ops in `1000`: `process_sprites` (`93d8`), `set_sprite_table_ptr` (`9410`),
`prepare_sprite_frames_thunk` (`941a`), `build_bit_reverse_lut_thunk` (`9424`),
`blit_sprite` (`942a`), `palette_dispatch_*_thunk` (`93e2/93f2/93fc/9406`).

---

## §3 — `game_stubs.c` carve-outs that are no-ops in BOTH builds (used)

These run in the original but are no-ops even in the playable. Decompilable unless noted.

**Sound L4/L6 driver backend** (reached by the ported sound pipeline):
`snddrv_dispatch_b_mode0/1/4` (`91cf/8e48/8af6`), `…c…` (`91d7/8e50/8b04`), `…d…`
(`91df/8e58/8b0d`), `snddrv_init_substep` (`8b2a`), `mpu401_reset_to_uart` (`8a75`),
`timer_teardown_restore` (`7fef`), `record_min_status_code` (`945b`). → reconstruct 1:1.

**Player handler-table targets outside the level-1 slice** (used on other game modes):
`move_walk_right_anim_step` (`2423`), `enter_mode_0b_jump_start` (`2470`),
`move_anim_step_to_mode0c` (`248e`), `move_step_check_walkable` (`24d7`),
`move_step_dispatch_input` (`250a`), `p1_input_dispatch_bit10` (`4344`),
`game_mode_handler_idx1d` (`4437`), `advance_physics_freeze` (`22d2`),
`game_mode_handler_idx30` (`1e3d`), `move_step_teleport_exit` (`4802`),
`play_walk_anim_default` (`4361`), `p1_set_pixel_from_cell` (`4906`),
`step_walk_anim` (`495c`), `sweep_active_entities` (`6183`),
`p2_dispatch_move_state_handler` (DGROUP `0x870`). → reconstruct 1:1.

**Never-decompiled (genuine carve-out):** `init_round_state`/`reset_round_counters`
(`31de`) — Ghidra decompilation fails (address-out-of-bounds). Stays a documented stub
unless the disassembly can be recovered by hand.

**Misc init (NOP both builds):** `init_misc_7bd7` (`7bd7`), `init_misc_7bbd` (`7bbd`) —
the BGI gfx-init thunks; needed for §1 vector-table population. → reconstruct 1:1.

---

## §4 — `screens.c` presentation no-ops (used on the title/menu/highscore path)

`process_sprites` (`93d8`), `fun_7bca_flip`/`fun_7b93_present_blank`/`fun_7b4a_view_blit`
(the §1 thunks), `fun_9410_set_sprite_table` (`9410`), `play_intro_animation_loop`
(`30dd` — real body in corpus), `wait_50_frames` (`3e74` — real body),
`draw_string_glyphs_9804` / `text_clip_leaf_9837`, `draw_icon_row` (`6130`),
`play_anim_sequence` (`3c4f`), `p1_move_step_up/down/left/right` (`3ab2/3b0f/3b6c/3bc9`),
`compute_move_descriptor_ptr` (`3a88`). All decompilable → reconstruct 1:1. (Several
resolve through §1/§2 once those land.)

---

## §5 — Host-shim merges & behavior-faithful models (playable build)

Already documented in `reconstruction-fidelity.md`; listed here as the un-merge work the
faithfulness goal implies.

| Host symbol | Merges / models | Action |
|-------------|-----------------|--------|
| `restore_bg_view` (bgi_overlay.c) | `bgi_set_mode_01` (`1ab9:0d77`) + its `0aa0` blit core | un-merge: real dispatch + behavior-faithful core |
| `render_player_view` model | `bgi_set_mode_10` (`1ab9:1028`) | un-merge |
| `present_frame` (host_video.c) | CRTC double-buffer (engine mechanism unresolved) | keep behavior-faithful; document |
| `sprite_chain` | `object_list`+`clip`+`setup` (`1cec:0e48/0f50/103d`) | un-merge to three 1:1 fns |
| `sprite_blit` / `bg_render` | self-modifying planar blit cores | keep behavior-faithful; document |
| hardware-init (host_boot/host_video) | `init_display_*`, `init_crtc_window`, `set_display_page` | reconstruct 1:1 where decompilable |

---

## §6 — Borland C runtime (`1000:986a`–`ab83`) — MIGRATE to Open Watcom (DECIDED)

~40 functions: `atexit/close/exit`, `stdio_*`, `fseek/ftell/setvbuf`, `dos_*`,
`farheap_*/heap_*`, `crt_*`, `malloc`, `fmemcpy`, `crt_uldiv_32`, `crt_lmul_32`, etc. **Decision:
migrate to the Open Watcom runtime** — do not reconstruct 1:1. The work here is to **document
any behavioral divergence** between the Borland and OW equivalents that could affect the game
(e.g. `malloc`/far-heap allocation layout, `dos_*` INT 21h wrappers' error semantics, buffered
`stdio` flush order). Record divergences in this section and in `reconstruction-fidelity.md`.
(The `.VEC`/PRNG/`vec_xform` runtime in `1c28/1cda/1ce5` is game-specific and already
reconstructed — not part of this.)

---

## Reconstruction priority order

1. **BGI overlay dispatch + vector tables + BGI-init (§1 dispatch layer + §3 init_misc)** —
   the foundation; unblocks every visual primitive and the title/menu transitions.
2. **`bgi_set_mode_01/10/11` un-merge (§1 + §5)** — real handlers + behavior-faithful blit
   cores; makes `restore_bg_view`/`render_player_view` faithful.
3. **Sprite pipeline front-end (§2)** — RLE decode, `sprite_proc_dispatch`, palette
   dispatches, un-merge `sprite_chain`; restores sprite/cursor/entity decode.
4. **BGI text/font (§1 text group + §4 glyph no-ops)** — menu/highscore/HUD text.
5. **Title/menu pacing (§4: `play_intro_animation_loop`, `wait_50_frames`)** — fixes the
   black-gap/“extra input”/jitter pacing.
6. **Sound L4/L6 backend (§3)** — audio fidelity.
7. **Player out-of-scope handlers (§3)** — full game-mode coverage beyond the level-1 slice.
8. **Hardware-init un-merge (§5)** — `init_display_*`/CRTC to 1:1 where decompilable.
9. **Borland CRT (§6)** — only if documenting the runtime is in scope (else keep documented
   as redundant).

**Permanent documented carve-outs (not "to do"):** the self-modifying planar blit cores
(`1ab9:0aa0` family, `1cec:10e1/31b7`) and `init_round_state` (`1cec`… `1000:31de`,
non-decompiling). These stay behavior-faithful / stubbed with a `RECONSTRUCTION FIDELITY`
note — the only entries here that are *not* reconstruction work.
