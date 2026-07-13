# Ghidra symbol map — BumpyDecomp

Reference for the names and identities of the functions and data in the live Ghidra
project (`BumpyDecomp`). Every `FUN_*` has been named and typed; the data labels are named
where groundable. Identities come from the Phases 1-7 reconstruction (the `src/` tree is the
authoritative source — names there were validated by the per-function gates), Borland Turbo
C++ 1990 RTL knowledge, and the Borland/BGI source under `local/borlandc/` (see
[reconstruction-fidelity.md](reconstruction-fidelity.md) and `engine.md`).

**Conventions.** 16-bit real-mode, near calls (`__cdecl16near`); far pointers are kept split
as `_off`/`_seg` pairs. Names ground in `src/` or a decompile/disassembly; genuinely
uncertain identities carry a `maybe_` prefix rather than an invented meaning (see
[Uncertain identities](#uncertain-identities)).

## Segment layout

| Segment | Range | Contents |
|---------|-------|----------|
| CODE_0 | `1000:0000`–`ab8f` | Main game + engine + the linked Borland C runtime |
| CODE_1 | `1ab9:0000`–`16ef` | The graphics-overlay kernel (linked from `GRAPHICS.LIB`) |
| CODE_2..9 | `1c28`/`1cd5`/`1cda`/`1ce5`/`1cec`/`202c`/`2036`/`203a` | Small overlays (heap, palette dispatch, relocation stubs) |
| CODE_10 | `203b:0000`–`a28f` | DGROUP-adjacent code + the graphics-overlay command-vector tables |
| DATA | `203b:a290`+ | DGROUP data: game state, sound/MIDI tables, framebuffers, heap |

Note: the game's DGROUP globals are addressed as `1000:<off>` in Ghidra (the code and data
segments overlap in this model). The `src/` reconstruction refers to them by the same `<off>`.

## CODE_0 functions

### Sound — driver layer (L4, Phase-6 validated 1:1)
`7df9` set_timer_slot_raw · `8a07` snd_emit_raw_sample · `8ad0` mpu401_settle_delay ·
`89e2` mpu401_write_data_polled · `8e2f` opl2_all_notes_off · `8b2a` snddrv_init_substep ·
`8af6`/`8b04`/`8b0d` snddrv_dispatch_{b,c,d}_mode4 · `8e48`/`8e50`/`8e58` …_mode1 ·
`91cf`/`91d7`/`91df` …_mode0 · `9056` opl_read_status.

### Sound — MIDI sequencer + OPL2
`86e9` midi_install_tempo_timer · `8977` midi_play_sequence · `8999` midi_get_track_count ·
`89a8` midi_sound_init · `8a23` seq_normalize_far_ptr · `8b6b`/`8b81`/`8e93`
midi_emit_voice_msg_w{2,1,3} · `922c` seq_set_channel_param · `8ea3` opl_event_note_on ·
`8eeb` opl2_reset_all_regs · `8fb6` `maybe_`opl2_detect_chip.

### Display / render / engine glue
`9804` draw_string_glyphs · `9814` set_active_display_page · `9821` set_crtc_window ·
`97c5` set_palette_display_mode · `97a4`/`97f1` init_display_controller_{a,b} ·
`80ac` blit_view_masked · `7b4a` blit_view_step · `9410` set_sprite_table_ptr ·
`7563` init_sound_tables · `75a2` read_input_action · `629c` game_post_present ·
`233a` game_post_input · `7fef` timer_teardown_restore.

### Engine logic
Move-step family: `6627` move_step_read_item · `6717` move_step_landed · `673a` move_step_noop ·
`4802` move_step_teleport_exit · `6699`/`66d8` move_step_{first,last}_variant ·
`6748`/`6789` …_variant_b · `67ca`/`67e2`/`67fb`/`6813` move_step_{first_gate,body,last_gate,last_body}_c.
Game-mode handlers: `1e3d` game_mode_handler_idx30 · `22b0` game_mode_handler_idx10 ·
`22c1` game_mode_handler_idx2d · `4437` game_mode_handler_idx1d · `43ef` p1_input_router_bit08.
State/entities: `31de` init_round_state (post-spawn state init — corrects the src's tentative
"reset_round_counters") · `6183` sweep_active_entities.
Input/key: `757b` set_joystick_handler_slot · `7abb` get_key_state_al · `7bfa`
get_key_state_far_thunk · `7ad5` `maybe_`wait_key_change · `97aa` get_key_state_far.
Timer: `7e18` set_timer_slot_stack · `7e1f` set_timer_slot_reg · `7efe` wait_tick_counter_a ·
`7f17` busy_wait_countdown · `7db2` pit_set_counter0_wrap.
DOS/BIOS wrappers: `7120` bios_int10_thunk · `7221` dos_getcurdir_wrap · `74b6`
dos_select_disk_wrap · `74c3` dos_chdir_wrap · `74d4` noop_empty_74d4.

### Overlay thunks (near→far trampolines into the graphics-overlay segments)
`7b76`/`7b86`/`7b93`/`7ba7`/`7bad`/`7bbd`/`7bca`/`7bd7`/`7bea` gfx_overlay_thunk_* (→ `1ab9`) ·
`809e` overlay_thunk_cd82 (→ `1cd5`) · `93a4` prng_seed_thunk · `93c8` gfx_set_mode_11_thunk ·
`93e2`/`93f2`/`93fc`/`9406` palette_dispatch_*_thunk · `941a` prepare_sprite_frames_thunk ·
`9424` build_bit_reverse_lut_thunk · `97d5` gfx_set_current_object_thunk ·
`97f7` draw_char_glyph_thunk · `9837` gfx_text_clip_thunk · `9854` measure_string_width_thunk ·
`9438` ret_thunk_9438 · `9376`/`9390` `maybe_`overlay_thunk (→ unanalyzed `1cda` targets) ·
`9847` `maybe_`gfx_text_thunk_1458.

### Borland C runtime (linked from the Turbo C++ libraries)
Exit/startup: `00f6` crt_exit_terminate · `0115` crt_terminate · `012f` crt_install_trap_vectors ·
`0172` crt_restore_trap_vectors · `01e2` crt_write_stderr · `01ea` crt_int0_divide_error.
DOS/errno: `a178` crt_dos_set_errno (feeds ~18 `dos_*` INT-21h wrappers) · `a20a` dos_truncate_handle.
Far heap: `a4fc` farheap_coalesce_prev · `a55e` farheap_coalesce_next ·
`a5cf` farheap_freelist_unlink · `a5f8` farheap_freelist_link · `a79b` farheap_realloc_move ·
`a813` farheap_realloc_split · `a995` crt_farptr_add_long.
Stdio: `98dd` stdio_fill_read_buffer · `995b` stdio_read_buffered · `999a` stdio_read_buffered_long.
Misc: `ab83` borland_stack_overflow (the stack-check-fail handler called in every prologue) ·
`0698` `maybe_`crt_return_one_stub (dead — zero xrefs).

## Graphics-overlay kernel (`1ab9`, from GRAPHICS.LIB)

Device-command dispatchers (each indexes a per-display-mode fn-pointer table by `[0x541d]`,
the mode index — confirmed against `local/borlandc/INCLUDE/GRAPHICS.H` + the in-binary VGA
behavior of each vector target):

`0179` gfx_init_viewport · `01c0` gfx_driver_nop · `01c1` gfx_device_clear_flag ·
`01e1` gfx_putimage_dispatch (overlay cmd 21, `rep movsw` image copy) ·
`01ff` gfx_cleardevice_dispatch (mode-0Dh reset) · `0232` gfx_device_reset_dispatch ·
`02b1` gfx_palette_dispatch (VGA-DAC `OUT 0x3c8/0x3c9` upload) ·
`0351` gfx_present_dispatch (called by `present_frame`) · `0384` gfx_device_inc_dispatch.

Text output: `12b0` gfx_char_width · `1311` gfx_text_render_dispatch · `1409` gfx_set_text_mode ·
`1422` gfx_set_clip_rect · `1441` gfx_set_text_position · `1458` gfx_set_text_attr. The text
state lives in the DGROUP block `0x6936`–`0x6946` (`gfx_clip_x0/y0/x1/y1`, `gfx_text_mode`, …).

### Graphics-overlay command-vector tables (DGROUP, slot = `[0x541d]*2 + base`)
`0x4dda` cmdvec_init · `0x5435` cmdvec_putimage · `0x5441` cmdvec_palette ·
`0x545d` cmdvec_cleardevice · `0x5469` cmdvec_device_reset · `0x5475` cmdvec_present ·
`0x5481` cmdvec_setvisualpage. (Label binding is best-effort — the bases are computed offsets,
not standalone defined-data items.)

## Other overlays

`1cd5:0000` dos_alloc_paragraphs (INT-21h AH=48) · `1cd5:0032` dos_free_segment (AH=49).
`203b:f87d`/`f8fd` are **not code** — data in the DATA segment that auto-analysis mis-typed
as functions (zero xrefs); named `data_not_code_*` and should be cleared to data in the GUI.

## Data labels

The Phases 1-7 reconstruction had already named the large majority of DGROUP globals while
building `src/`. The remaining unnamed labels were named where groundable — notably the sound
block (`0x83cc` snd_voice_table, `0x9788`–`0x9798` snd_param_frame, `0x979a` snd_isr_state),
the MIDI track tables, the far-heap internals, the saved keyboard ISR vector, and the graphics-overlay
clip/text state block. The genuinely un-nameable residue is left as `DAT_*`: framebuffer
interiors (`0x7350`–`0x75bc`), CRT/stdio scratch, and the `switchdataD_*` jump tables.

The named globals are also **typed** (≈165 labels), grounded in the `src/` C declarations.
The GhidraMCP plugin was extended with `set_data_type` + `create_struct` endpoints for this
(see [Tooling](#tooling)). Applied types: structs `anim_channel_rec` (12 B, the 8 channel
records), `timer_cb_entry`/`timer_slot` (8 B, the `0x5516`/`0x549c` tables), `sprite_obj`
(6 B, the `0x792e`/`0x795a` blit descriptors); arrays for the bounded tables/LUTs
(`pixel_bitrev_lut` `byte[256]`, `g_key_state_table` `byte[128]`, `snd_voice_table`
`byte[15]`, `snd_param_frame` `word[9]`, the palette-patch + menu/highscore tables); far
pointers as split `_off`/`_seg` `word` pairs (or `dword` for single far-ptr slots); and
scalar `byte`/`word`/`dword` for the rest. Skipped (left at primitive types) where a src
array size is a *logical* over-allocation that physically overlaps neighbours (the anim
far-ptr tables `0x2ede`/`0x3d6a`/`0x40a6`, the OPL freq LUTs `0x5593`…, some level tables)
or where the layout isn't groundable (the MIDI track-state tables).

Note: the DGROUP globals are addressed at Ghidra segment `203b` (the DATA segment); the
`1000:<off>` form is a different linear view of the overlapping code/data model. The anim
channel-record storage is the exception — it lives in seg `1000` (`1000:4c40`…).

## Tooling

The GhidraMCP plugin (`com.lauriewired.GhidraMCPPlugin`) was extended in this project with two
HTTP endpoints + matching MCP bridge tools, since stock GhidraMCP can rename but not retype
global data:
- `set_data_type(address, data_type)` — apply a type at a global address (built-ins, arrays
  `t[N]`, pointers, named structs/enums; resolved via Ghidra's `DataTypeParser`, errors on
  unknown).
- `create_struct(name, fields)` — define a named struct from a `name:type,…` field list.

Source build: `local/toolchain/build-ghidramcp/` (upstream LaurieWired/GhidraMCP source +
the two added handlers, compiled with the bundled JDK 21 against the Ghidra 12.1.2 API jars).
The rebuilt extension jar is deployed under `…/Extensions/GhidraMCP/lib/GhidraMCP.jar` (with
`.bak` rollbacks); the bridge tools are in `…/ghidramcp/GhidraMCP-release-1-4/bridge_mcp_ghidra.py`.
Loading a new build requires restarting the Ghidra GUI + reconnecting the MCP.

## Uncertain identities

Behavior is known; the precise purpose is best-effort and flagged `maybe_`:
`7ad5` wait_key_change (loop exit condition unclear) · `8fb6` opl2_detect_chip (matches the
AdLib presence-detect pattern, no caller confirmation) · `9376`/`9390` overlay_thunk (targets
`1cda:0045`/`0089` are unanalyzed) · `9847` gfx_text_thunk_1458 · `0698` crt_return_one_stub
(dead code).
