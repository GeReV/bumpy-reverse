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
- **graphics-driver overlay (seg `1ab9`) → reimplement in the host module
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
| `1ab9` CODE_1 | **graphics-driver overlay** | **largely unreconstructed (§1)** |
| `1c28` CODE_2 | `.VEC` interpreter (`vec_run`) | reconstructed (vec.c / tools) |
| `1cd5/1cda/1ce5` | seg-alloc, `vec_xform`, PRNG | reconstructed |
| `1cec` CODE_6 | **sprite pipeline** | **partial (§2)** |
| `202c/2036` | sound-select / palette-dispatch screens | reconstructed (config_screens.c / screens.c) |
| `203b` | DGROUP (data) | n/a |

**Headline.** The *game logic* (physics, AI, collision, the per-tick spine, menu/title state
machines) is reconstructed and in large part validated. The **graphics + IO substrate is the
real gap**: the engine's graphics-driver overlay (seg `1ab9`, ~27 fns) is almost entirely
unreconstructed and is instead *modeled* by a few host shims (`restore_bg_view`,
`render_player_view`, `present_frame`), and the sprite pipeline (seg `1cec`) is only half
ported. This is the root of the title/menu offset/jitter/pacing symptoms and the reason
several presentation primitives are no-ops.

---

## §1 — graphics-driver overlay (seg `1ab9`) — PRIMARY GAP

> ⚠️ **CORRECTION (2026-06-27): the role labels below for `7b93`/`7bca` are MISNOMERS.**
> Capture-driven RE of the actual VGA (`palette_mode==2`) handlers (disassembled from the
> binary; recorded as Ghidra disassembly comments at the handler addresses) proved:
> - `1ab9:01e1` (`gfx_stage_palette_dispatch`, thunk `7b93`, VGA handler `1ab9:0620`) is a
>   **16-colour palette stage** — `rep movsw` of the 48-byte palette from `buf+0x33` into the
>   current draw-object's per-**page** slot (`*0x5311 + page*99 + 0x33`). NOT a pixel putimage.
> - `1ab9:02b1` (`gfx_upload_palette_to_dac_dispatch`, thunk `7bca`, VGA handler `1ab9:0677`)
>   is a **DAC palette upload** (ports `0x3c8`/`0x3c9`, slots 0–7 & 0x10–0x17). NOT a page flip.
> - `1ab9:0351` (`gfx_present_dispatch`, `present_frame` `7bdd`, VGA handler `1ab9:0379` →
>   `1ab9:06c1`) is **the one true CRTC page flip** (`XOR` Start-Address bit 5 = 0x2000 swap).
> - `1ab9:0179` (`gfx_init_viewport`, thunk `7b4a`) sets the clip/viewport extent, then
>   dispatches `[pm*2+0x4dda]`. **CORRECTION (2026-07-05): the EGA/VGA slot value `0` is NOT a
>   null no-op** — `CALL word ptr [BX+0x4dda]`==0 calls near `1ab9:0000`, a secondary dispatcher
>   → `0x4dcc[view+0x1c]`; `0x4dcc[0]=1ab9:002b` = a **solid black rect fill** (4-plane SEQ
>   map-mask + `rep stosw`, geometry from `view[+0x14/+0x16/+0x1e/+0x20]`, colour `+0x22..+0x25`).
>   This is the real **geometric iris** + name-entry cursor erase + code-screen clear (Task 24),
>   reconstructed in `host_gfx_set_viewport`. The earlier "null slot / iris = timed-hold+blank-DAC"
>   claim below was a disassembly error, now superseded.
>
> ⚠️ **CORRECTION (2026-06-27, Task 2): `upload_vga_dac_palette`/`dispatch_by_palette_mode`
> were MISNOMERS — they are a vsync WAIT, not a DAC upload.** The thunk `1000:9864`
> (renamed Ghidra `wait_vretrace_thunk`) CALLFs `2036:0000` (renamed `wait_vretrace_dispatch`,
> was `dispatch_by_palette_mode_2036`), which indirect-calls overlay table `[pm*2 + 0x6976]`;
> for the VGA boot (`pm==2`) entry[2]=`0x0015` → `2036:0015` = a **VERTICAL-RETRACE (vsync)
> WAIT** (`mov dx,0x3da; in al,dx; test al,8`: wait for retrace start, then end). The iris
> wipe calls it 4×/step as the wipe PACING. The genuine DAC upload is `7bca`
> (`host_gfx_upload_palette_to_dac`) / `vga_dac_upload_from_buffer` — kept as-is. src mirror:
> `wait_vretrace_thunk`/`wait_vretrace_dispatch` (`src/screens.c`, `#ifdef BUMPY_PLAYABLE`).
> The level-palette loader `load_palette` (`1000:08d1`) reuses `7b93`→`7bca`→`9864`
> (stage→DAC upload→vsync); reconstructed host body in `src/host/host_video.c` (Task 2),
> driven by `apply_level_palette` (`1000:0604`).
>
> Reconstruction is tracked in `docs/superpowers/plans/2026-06-27-host-bgi-present-transitions.md`
> (rewritten). **Task 4 (2026-06-27) has now updated the per-row "Role"/"Action" text below.**

The engine's graphics primitives are a Loriciel-custom driver overlay. Main-segment
**thunks** (`1000:7b4a`…) call **dispatch** functions here, which indirect-call a
**runtime vector table** indexed by `palette_mode` (DGROUP `0x4dda`/`0x5435`/`0x5441`/
`0x5475`/`0x555e`), reaching the per-mode handler. All of `1ab9` is in the corpus and
decompiles, **except** the innermost self-modifying planar blit cores (the `gfx_set_mode_*`
→ `1ab9:0aa0`-family targets), which is the one legitimate behavior-faithful carve-out
(already done once as `restore_bg_view`).

| Addr | Ghidra name | Role | `src/` status | Decompiles? | Action |
|------|-------------|------|---------------|-------------|--------|
| `1ab9:0179` | gfx_init_viewport | set viewport 0x14×0x19, dispatch `[pm*2+0x4dda]` → (slot 0) `1ab9:0000` → `0x4dcc[+0x1c]` → `1ab9:002b` **rect fill** | **host-modeled** (thunk `1000:7b4a` → `host_gfx_set_viewport`, `#ifdef BUMPY_PLAYABLE`; default NOP kept) — slot is a **solid black rect fill** = geometric iris + name-entry cursor erase + code-screen clear (Task 24, corrected 2026-07-05; the old "null slot, timed-hold iris" was a disasm error) | yes | — |
| `1ab9:01e1` | gfx_stage_palette_dispatch | **palette stage** via `[pm*2+0x5435]` (VGA handler `1ab9:0620` = `rep movsw` of 48-byte palette into per-page slot; NOT a pixel putimage — misnomer corrected 2026-06-27) | **host-modeled** (thunk `1000:7b93` → `host_gfx_stage_image_palette`, `#ifdef BUMPY_PLAYABLE`; default NOP kept; Tasks 1–2) | yes | — |
| `1ab9:05b6` | gfx_page_slot_offset | shared helper: per-page draw-object slot = `page*99` | **reconstructed** (`gfx_page_slot_offset`, `src/gfx_palette.c`; host-unit-tested via `tools/gfx_palette_ctest.c`) | yes | — |
| `1ab9:0605` | gfx_stage_palette_cga | CGA palette-stage slot (`cmdvec_stage_palette_modes[0]`) | **reconstructed** (`gfx_stage_palette_cga`, `src/gfx_palette.c`) — bare `RET`, no-op | yes | — |
| `1ab9:0606` | gfx_stage_palette_ega | **EGA palette stage** (`cmdvec_stage_palette_modes[1]`) — `rep movsw` 16-byte AC-index palette from `img+0x23` into the draw-object's per-page slot | **reconstructed** (`gfx_stage_palette_ega`, `src/gfx_palette.c`, 2026-07-11; host-unit-tested byte-exact); playable host models it via `host_gfx_stage_image_palette`'s `palette_mode==1` branch (`host_gfx.c`) | yes | — |
| `1ab9:0620` | gfx_stage_palette_vga | VGA palette stage (`cmdvec_stage_palette_modes[2]`) — same handler `01e1` dispatches to for `palette_mode==2` | **reconstructed** (`gfx_stage_palette_vga`, `src/gfx_palette.c`) | yes | — |
| `1ab9:01ff` | gfx_cleardevice_dispatch | cleardevice via vector | missing | yes | reconstruct 1:1 |
| `1ab9:0232` | gfx_device_reset_dispatch | device reset via vector | missing (thunk `7bbd`=NOP) | yes | reconstruct 1:1 |
| `1ab9:02b1` | gfx_upload_palette_to_dac_dispatch | **DAC palette upload** via `[pm*2+0x5441]` (VGA handler `1ab9:0677` = DAC write to ports `0x3c8`/`0x3c9`, slots 0–7 & 0x10–0x17; NOT a page flip — misnomer corrected 2026-06-27) | **host-modeled** (thunk `1000:7bca` → `host_gfx_upload_palette_to_dac`, `#ifdef BUMPY_PLAYABLE`; default NOP kept; Tasks 1–2) | yes | — |
| `1ab9:0661` | gfx_upload_palette_cga | CGA palette-upload slot (`cmdvec_upload_palette_modes[0]`) | **reconstructed** (`gfx_upload_palette_cga`, `src/gfx_palette.c`) — bare `RET`, no-op | yes | — |
| `1ab9:0662` | gfx_upload_palette_ega | **EGA palette upload** (`cmdvec_upload_palette_modes[1]`) — `INT 10h AX=1002h`, programs the 16 Attribute Controller registers + overscan from the staged `+0x23` table | **reconstructed** (`gfx_upload_palette_ega`, `src/gfx_palette.c`, 2026-07-11); playable host models it via the equivalent direct `0x3c0` AC-port sequence (`host_gfx_upload_palette_to_dac`'s `palette_mode==1` branch, `host_gfx.c`) | yes | — |
| `1ab9:0677` | gfx_upload_palette_vga | VGA palette upload (`cmdvec_upload_palette_modes[2]`) — same handler `02b1` dispatches to for `palette_mode==2` | **reconstructed** (`gfx_upload_palette_vga`, `src/gfx_palette.c`) | yes | — |
| `1ab9:0351` | gfx_present_dispatch | **CRTC page flip** via `[pm*2+0x5475]` (VGA handler `1ab9:0379` → `1ab9:06c1` = XOR Start-Address bit 5, 0x2000 swap; the ONE true frame present) | **host-modeled** (thunk `1000:7bdd` → `present_frame`, `src/host/host_video.c`; Tasks 1–2) | yes | — |
| `1ab9:0384` | gfx_device_inc_dispatch | device-inc via vector | missing (thunk `7bea`=NOP) | yes | reconstruct 1:1 |
| `1ab9:01c0` | gfx_driver_nop | driver no-op slot | missing | yes | reconstruct 1:1 (trivial) |
| `1ab9:01c1` | gfx_device_clear_flag | clear device flag | missing | yes | reconstruct 1:1 |
| `1ab9:021b` | gfx_set_current_pos | set current draw pos | missing | yes | reconstruct 1:1 |
| `1ab9:0a73` | gfx_set_mode_00 | CGA-mode handler | missing | partial (self-mod core) | reconstruct dispatch; blit core behavior-faithful |
| `1ab9:0d77` | gfx_set_mode_01 | bg/erase handler → `0aa0` masked blit | **modeled** by `restore_bg_view` | partial | un-merge: real `gfx_set_mode_01` + behavior-faithful blit core |
| `1ab9:1028` | gfx_set_mode_10 | player-view handler | **modeled** by `render_player_view` | partial | un-merge similarly |
| `1ab9:126e` | gfx_set_mode_11 | mode-11 handler | partial citation | partial | reconstruct dispatch + behavior-faithful core |
| `1ab9:12b0` | gfx_char_width | glyph width | missing | yes | reconstruct 1:1 |
| `1ab9:1311` | gfx_text_render_dispatch | text render dispatch | missing | yes | reconstruct 1:1 |
| `1ab9:132b` | gfx_set_current_object | select current draw object | missing (thunk `97d5`) | yes | reconstruct 1:1 |
| `1ab9:137b` | gfx_draw_sequence | draw a sequence | missing | yes | reconstruct 1:1 |
| `1ab9:13bc` | draw_char_glyph | render one glyph | missing (thunk `97f7`) | yes | reconstruct 1:1 |
| `1ab9:13ec` | draw_string_glyphs | render glyph string | missing (`9804`/screens.c NOP) | yes | reconstruct 1:1 |
| `1ab9:1409` | gfx_set_text_mode | text mode | missing | yes | reconstruct 1:1 |
| `1ab9:1422` | gfx_set_clip_rect | clip rect | missing | yes | reconstruct 1:1 |
| `1ab9:1441` | gfx_set_text_position | text position | missing | yes | reconstruct 1:1 |
| `1ab9:1458` | gfx_set_text_attr | text attributes | missing (thunk `9847`) | yes | reconstruct 1:1 |
| `1ab9:146b` | measure_string_width | string width (thunk `9854`) | missing | yes | reconstruct 1:1 |
| `1ab9:14d3` | font_glyph_ptr | glyph-data pointer | missing | yes | reconstruct 1:1 |
| `1ab9:02ce` | gfx_driver_init | adapter/palette select screen | **reconstructed** (config_screens.c) | yes | — |

**Also required (the wiring):** the main-segment dispatch thunks
`1000:7b4a/7b76/7b86/7b93/7ba7/7bbd/7bca/7bdd/7bea`; **Tasks 1–3 host-modeled four of these on the
VGA (`palette_mode==2`) path**: `7b4a` → `host_gfx_set_viewport`, `7b93` →
`host_gfx_stage_image_palette`, `7bca` → `host_gfx_upload_palette_to_dac`, `7bdd` →
`present_frame`; their default-build NOP stubs remain. The remaining thunks (`7b76`/`7b86`/
`7ba7`/`7bbd`/`7bea`) are still NOP. The per-`palette_mode` **vector tables** (DGROUP
`0x4dda/0x5435/0x5441/0x5475/0x555e`) remain unresolved for the 1:1 path in the sense that
nothing routes the reconstructed dispatch through them yet (the host bypass avoids the
table indirection entirely). **Correction (2026-07-11):** the earlier framing here —
"graphics-overlay init code that populates them (currently absent)" — was wrong for
`0x4dda`/`0x5435`/`0x5441`/`0x5475` (and their sibling `0x545d`/`0x5469`/`0x5481`): a raw
disasm read of the unpacked image (`local/build/unpack/BUMPY_unpacked.exe`) shows these are
**static initialised data**, not runtime-populated — `init_misc_7bd7`/`init_misc_7bbd`
being NOP is consistent with that (there is nothing left for them to populate). The
`0x5435`/`0x5441` (palette stage/upload) pair is now typed `word[3]` in Ghidra with
CGA/EGA/VGA slot annotations and its three handlers per slot are reconstructed 1:1 in
`src/gfx_palette.c` (see the `0605/0606/0620` and `0661/0662/0677` rows above); `0x555e`
(the `gfx_set_mode_01`/`restore_bg_view` dispatch table — a different family, see
[rendering-pipeline.md](rendering-pipeline.md) §1) is untouched by this correction.

---

## §2 — Sprite pipeline (seg `1cec`) — PARTIAL

The blit chain is reconstructed (`sprite.c`/`sprite_blit.c`/`sprite_chain.c`); the decode/
dispatch front-end is not. `sprite_chain` **merges** `object_list`+`clip`+`setup` (a documented
deviation to un-merge).

**EGA note (2026-07-11):** the `1cec` per-`palette_mode` sprite-op vector tables adjacent to
the dispatch functions below (`0x2d37/2d61/2d9c/2dc6`) have an **EGA slot identical to their
VGA slot** in every case — read directly from the unpacked image, same method as the palette
cmdvec tables in §1. The sprite/blit path therefore has **no EGA-specific divergence**:
reconstructing the VGA dispatch (the `Action` column below) automatically covers EGA too: no
separate EGA reconstruction is needed here.

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
the gfx-init thunks; needed for §1 vector-table population. → reconstruct 1:1.

---

## §4 — `screens.c` presentation no-ops (used on the title/menu/highscore path)

`process_sprites` (`93d8`), `fun_7bca_flip`/`fun_7b93_present_blank` (§1 thunks — host-modeled, Tasks 1-2), `fun_7b4a_view_blit` (**host-modeled**, Task 3 — `gfx_set_viewport_thunk`/`host_gfx_set_viewport`), `fun_9410_set_sprite_table` (`9410`), `play_intro_animation_loop`
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
| `restore_bg_view` (gfx_overlay.c) | `gfx_set_mode_01` (`1ab9:0d77`) + its `0aa0` blit core | un-merge: real dispatch + behavior-faithful core |
| `render_player_view` model | `gfx_set_mode_10` (`1ab9:1028`) | un-merge |
| `present_frame` (host_video.c) | CRTC double-buffer (engine mechanism unresolved) | keep behavior-faithful; document |
| `load_palette` (host_video.c) | `1000:08d1` — host sources decoded palette `g_pav_buf+51` instead of decoding packed `0x578` (which the host never stages); omits `load_palette_byteswapped` (`1000:063b`, fills `0x578` — vestigial in host) | data-sourcing deviation; structure (stage→upload→vsync) faithful |
| `wait_vretrace_thunk`/`_dispatch` (screens.c) | `1000:9864`/`2036:0000`/`2036:0015` — overlay-table dispatch collapsed to the vsync poll (table runtime-populated, `2036:0015` not in corpus) | keep behavior-faithful; document (misnomer corrected) |
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

1. **graphics-overlay dispatch + vector tables + graphics-overlay init (§1 dispatch layer + §3 init_misc)** —
   the foundation; unblocks every visual primitive and the title/menu transitions.
2. **`gfx_set_mode_01/10/11` un-merge (§1 + §5)** — real handlers + behavior-faithful blit
   cores; makes `restore_bg_view`/`render_player_view` faithful.
3. **Sprite pipeline front-end (§2)** — RLE decode, `sprite_proc_dispatch`, palette
   dispatches, un-merge `sprite_chain`; restores sprite/cursor/entity decode.
4. **graphics-overlay text/font (§1 text group + §4 glyph no-ops)** — menu/highscore/HUD text.
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
