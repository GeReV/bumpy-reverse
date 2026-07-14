# Engine internals

Reference documentation for *Bumpy's Arcade Fantasy*'s game engine — the game loop,
the two-player physics/AI pipeline, the animation-channel and entity-draw systems,
level loading, input, and audio. Names below are the ones assigned in the live
`BumpyDecomp` Ghidra project and carried into `src/`; addresses are `segment:offset`
in the unpacked image (`local/build/unpack/BUMPY_unpacked.exe`) unless given as a bare
`DGROUP` offset.

## Subsystem map

`BUMPY.EXE` is ~399 functions across several Ghidra segments. All of them are named,
typed, and commented in the Ghidra project (the last raw `FUN_*` symbols were resolved
2026-07-14); a handful of low-confidence identities carry a `maybe_` prefix rather than
an invented meaning instead of being left unnamed — see
[ghidra-symbol-map.md](ghidra-symbol-map.md#uncertain-identities).

| Subsystem | Segment | Representative functions |
|---|---|---|
| Game loop / session | `1000` | `game_loop`, `run_game_session`, `init_game_session_state`, `start_level`, `level_intro_screen` |
| P1 movement / physics state machine | `1000` | `p1_movement_dispatch`, `enter_game_mode`, `dispatch_move_step`, `game_mode_handlers[]`, `land_on_tile_below`, `teleport_to_next_exit_tile` |
| P2 AI pipeline | `1000` | `p2_dispatch_move_state_handler`, `p2_tile_move_check`, `p2_ai_select_move_a/b/random`, `check_pvp_collision` |
| Animation channels & entity draw | `1000` | `step/draw/erase_anim_channels_a/b`, `entity_draw_layer_a/b/c`, `draw_p1/p2_sprite` |
| Level loading & world data | `1000` | `start_level`, `spawn_and_draw_level_entities`, `worldmap_move_descriptor` |
| Menus / front-end | `1000` | `run_main_menu`, `play_intro_animation_loop`, `play_iris_wipe_transition`, `show_*_screen` |
| Input | `1000` | `poll_input`, `read_input_action`, `handle_gameplay_input`, joystick globals |
| Sound | `1000` | `play_sound`, `snddrv_dispatch_*`, `midi_process_event`, OPL/MIDI globals |
| Timer / PIT / ISR | `1000` | `pit_set_counter0`, `pit_read_counter0`, `install/uninstall_interrupt_handler` |
| Resources / CRT | `1000` | `open_resource`, `read_chunked`, `malloc`, heap/atexit/streams |
| Graphics-driver overlay (Loriciel-custom, **not** Borland BGI) | `1ab9` | `gfx_set_mode_*`, `gfx_init_viewport`, `gfx_stage_palette_*`, `gfx_upload_palette_*`, `draw_char_glyph`, `draw_string_glyphs` |
| Sprite codec | `1cec` | `sprite_rle_decode`, `sprite_rle_encode`, `decode_2bpp_planes`, `blit_sprite_vga` |
| Vector/`.VEC` interpreter | `1c28`/`1cda` | `vec_run`, `vec_read_record`, `vec_xform` |
| PRNG | `1ce5` | `prng_seed`, `prng_step` (3-word state) |

The graphics-overlay dispatch, the VGA double-buffer, and the sprite/background/entity
blit paths are documented separately in [rendering-pipeline.md](rendering-pipeline.md).
Per-module fidelity notes (what's a 1:1 transcription vs. a documented deviation) are in
[reconstruction-fidelity.md](reconstruction-fidelity.md).

### Far pointers and 32-bit globals are kept split

Adjacent global pairs that form one far pointer (`_off`/`_seg`) or one 32-bit value
(`_lo`/`_hi`) are deliberately kept as two named 2-byte items rather than merged into a
single 4-byte typed symbol. Merging was tried (a headless pass on a throwaway project
copy): it compiles, but makes the decompiled C *worse* for this segmented code —
consumers like `start_level` re-split the value at every call because the I/O routines
take the halves as separate parameters, and pointer derefs degrade to byte-offset casts.
This is 16-bit segmented code that genuinely passes far pointers as two words; the split
form matches the code and reads better.

## Game loop and session spine

`run_game_session` → `init_game_session_state` → `game_loop` (1000:0258 / 0282 / 0c18,
`src/game.c`) is the top-level structure: one-time init, then a menu/level-select loop
around `start_level` + the per-tick `game_tick()` loop. `game_loop` runs until the
player quits; `game_state` (0 = playing, -1 = died) drives the restart-vs-continue
branch after each level.

**Per-tick order** (`game_tick`, `src/game.c`, inlined body of 1000:0c18's loop) — the
same pipeline runs for both players, gated on `two_player_active` for the P2 calls:

```
rng_frame = rand()
p1_advance_grid_history()        p2_advance_grid_history()
p1_step_scripted_move()          p2_step_scripted_move()
p1_update_grid_cell()            p2_update_grid_cell()
step_anim_channels_a()            step_anim_channels_b()
erase_p2_view()                   erase_p1_view()
restore_bg_pending()
draw_anim_channels_a()             draw_anim_channels_b()
update_p1_bbox()                  update_p2_bbox()
erase_anim_channels_a()            erase_anim_channels_b()
render_p1_view()                  render_p2_view()
draw_p1_sprite()                  draw_p2_sprite()
present_frame(1)                   ← the sole page-flip/present call in the tick
rotate_timing_flags_and_wait()
game_post_present()
handle_gameplay_input()
p2_tile_move_check()
check_pvp_collision()
game_post_input()
(pause-key check → show_pause_screen() if pressed)
```

So relative to movement: anim-channel **step** happens right after the grid-cell
update and before any drawing; anim-channel **draw/erase** and the entity draws all
happen before `present_frame`; input handling and PvP collision are checked strictly
*after* the present.

`handle_gameplay_input` (1000:1d26) is the per-tick P1 entry point: it polls debug
F-keys, then — unless colliding with P2 (`pvp_collision_flag`) — reads the tile under
P1, polls input, and calls `p1_movement_dispatch()` for a fresh move
(`p1_move_steps_left == 0`) or `dispatch_move_step()` to continue one in progress. On
a P1↔P2 collision it calls `begin_physics_freeze()` instead.

View struct fields referenced throughout (`p1_view`/`p1_erase_view` etc., raw byte
offsets, no named C struct — see the anim-channel section for the equivalent A/B
descriptors): `+6`=grid_x, `+8`=grid_y, `+0x14/0x16`=view_origin, `+0x1e`=scroll_x,
`+0x20`=scroll_y, `+0x1c`=flags.

## Physics / movement state machine (P1)

`p1_movement_dispatch` (1000:1e02, `src/player.c`) is the state-machine entry point:
it clears the pending-action code, saves `game_mode` into `prev_game_mode`, and either
runs `move_settle()` (if `move_override` is set and physics isn't frozen) or dispatches
through **`game_mode_handlers[64]`** (DGROUP `0x7ca`) indexed by `game_mode`
(DGROUP `0x792c`).

### `game_mode_handlers` map

Most modes share the idle handler; the distinct ones:

| mode(s) | handler | role |
|---|---|---|
| most (0–2,4,6–9,0x11–0x1b,0x27–0x2b,0x31–0x3f) | `gamemode_default_idle` (1000:28f9) | idle: dispatch pending action, else default-move by cell |
| `0x03`, `0x0f` | `gamemode_03_move` (1000:23b6) | mid-move tick |
| `0x05` | `move_walk_right_anim_step` (1000:2423) | walk-right anim step + move dispatch |
| `0x0a` | `enter_mode_0b_jump_start` (1000:2470) | jump/jet start → mode `0x0b` |
| `0x0b` | `move_anim_step_to_mode0c` (1000:248e) | walk-anim tick → mode `0x0c` on down-input |
| `0x0c` | `move_step_check_walkable` (1000:24d7) | mid-step ground probe |
| `0x0d` | `move_step_dispatch_input` (1000:250a) | move-step input dispatch (left/right/down/settle) |
| `0x0e` | `teleport_to_next_exit_tile` (1000:25ad) | teleport-tile handling |
| `0x10`, `0x2c` | `game_mode_handler_idx10` (1000:22b0) | settle-wrap (calls `run_physics_settle`, discards result) |
| `0x1c` | `p1_input_dispatch_bit10` (1000:4344) | top of the P1 input-dispatch ladder |
| `0x1d`–`0x20` | `game_mode_handler_idx1d` (1000:4437) | input router: fire→settle idle, else dispatch direction |
| `0x21` | `gamemode_21_start` (1000:1e5e) | start/launch right |
| `0x22` | `gamemode_22` (1000:1e90) | start/launch left |
| `0x23` | `gamemode_23_walk` (1000:1ec2) | walk-right anim tick |
| `0x24` | `gamemode_24_walk` (1000:1f3e) | walk-left anim tick |
| `0x25` | `gamemode_25_contact` (1000:2138) | left-walk contact resolution |
| `0x26` | `gamemode_26_contact` (1000:21e7) | right-walk contact resolution |
| `0x2d` | `game_mode_handler_idx2d` (1000:22c1) | wraps `run_physics_settle()` |
| `0x2e` | `advance_physics_freeze` (1000:22d2) | end-of-level freeze countdown (3 ticks → settle) |
| `0x2f` | `land_on_tile_below` (1000:2810) | landing resolution |
| `0x30` | `game_mode_handler_idx30` (1000:1e3d) | activate current move-descriptor entry (round-continue latch) |

`game_mode_handler_idx10`/`idx1d`/`idx30` keep their `FUN_1000_xxxx`-style spelling in
`src/` on purpose, to avoid churning the `game_mode_handlers[]` call sites — their
canonical names above are cited in-code and in the Ghidra project.

### The state-machine drivers

- **`enter_game_mode(mode)`** (1000:4263) — the transition primitive: sets `game_mode`
  and (for modes outside `{0x05, 0x0b, 0x1c}`) loads that mode's movement script from
  **`mode_script_tbl`** (DGROUP `0x2252`) into `p1_move_steps_left`, `p1_facing_left`,
  and the `[anim,dx,dy]` cursor `p1_move_script`. Gated by `move_locked` (DGROUP
  `0x8242`).
- **`dispatch_move_step`** (1000:238e) — called at the tail of most move handlers;
  continues the sequence via the 2-D table **`move_step_dispatch_tbl`** (DGROUP
  `0x43c0`, indexed `[game_mode][p1_move_step_idx]`, stride `0x22`). The table holds
  raw little-endian **near offsets** the engine calls directly (in real mode the offset
  *is* the code address). `src/` keeps the byte table identical and resolves each
  offset to its reconstructed C function through a host-only lookup
  (`move_step_handler_for_offset`).
- **`p1_step_scripted_move`** (1000:13df) — the per-tick move executor: applies the
  current `[anim,dx,dy]` script entry to `p1_pixel_x/y` (dx negated if facing left),
  advances the script cursor, decrements `p1_move_steps_left`.
- **`exec_move_action(action)`** (1000:46bb) — maps an action code to a directional
  primitive: `move_down`, `move_left`, `move_settle`, etc.

So the full movement spine is: **`p1_movement_dispatch` → `game_mode_handlers[mode]` →
(move primitive / contact resolve) → `enter_game_mode(next)` + `dispatch_move_step`**,
with the per-tick `p1_step_scripted_move` applying the loaded script to pixel
position.

### Movement primitives and collision

`step_walk_anim`/`p1_advance_move_anim` advance the idle-walk animation on a period
counter. `move_left`/`move_right` (1000:2634/26a1) and their action-variant siblings
`move_left_step_resolve`/`move_right_step_resolve_alt` (1000:270c/2776) resolve a
one-cell move by probing the adjacent tile through a contiguous, densely-aliased bank
of contact/collision tables at DGROUP `0x4256`–`0x4476`
(`contact_action_tbl_left`, `collision_mode_table_left/right(_alt)`,
`contact_transition_tbl(_b)`, `left/right_walk_contact_tbl_34/35/38/39`) — indexed with
an *unmasked* tilemap byte, so codes ≥ `0x20` legitimately spill into the
next physically-adjacent table (documented design, not a bug).
`gamemode_25_contact`/`gamemode_26_contact` and `p1_resolve_walk_left/right_contact`
(1000:1fbe/207d) resolve multi-cell walk contacts the same way. `land_on_tile_below`
(1000:2810) and `check_tile_below_ladder_or_land` (1000:29a6) resolve landings/ladders
against the tile below the player. `run_physics_settle`/`run_physics_settle_wrap`
(1000:22fc/22c1) and `begin_physics_freeze`/`advance_physics_freeze` (1000:228d/22d2)
implement the freeze-then-settle sequence used at level completion and on P1↔P2
contact. `apply_contact_action` (1000:6a89, `src/player.c`) is also the channel-B
animation-slot allocator (see below) — it claims a slot, stamps the tilemap, and plays
the contact sound.

Two Ghidra-labelling traps worth knowing: DGROUP `0x855e` is `p1_step_col_count` (a
cursor/step column counter), not `move_step_count` — the real `move_step_count` is
`0x824c` (the jump/move-step tick counter); several raw decomps carry a stale comment.
And `p1_move_anim` (DGROUP `0x824a`) is a 16-bit **WORD**, not a byte — the idle-bounce
modes `0x3d`/`0x3f` use frame values `0x1d1`–`0x1d7` that don't fit in `u8`.

## Two-player pipeline (P2 AI)

P2 is autonomous, not player-2-key-driven. It runs the same trajectory layer as P1
(`p2_set_move_state`, `p2_step_scripted_move`, `p2_update_grid_cell`,
`p2_advance_grid_history`) driven by a canned `[anim,dx,dy]` micro-script per move
state, plus an AI decision layer that picks the next state once the current script
runs out (`p2_move_steps_left == 0`).

**Move-state scripts.** `p2_state_script_tbl` (DGROUP `0x2520`) holds ten 4-byte
entries (`{steps, facing, entries_off, entries_seg}`, state 0 unused, states 1–9 real)
— populated at boot by `init_p2_state_scripts()` in `src/move_scripts.c`, which
extracts and relocates a verbatim 502-byte blob from DGROUP `[0x2352, 0x2548)` (the
same loader-fixup pattern `init_move_scripts()` uses for P1's `mode_script_tbl`, since
several state headers are shared and must be relocated exactly once).

**AI decision.** `p2_tile_move_check` probes the four cardinal tile directions around
P2's cell into `p2_dir_blocked_0..3`. If all four are blocked,
`p2_ai_select_move_random` draws a fresh state from the engine's PRNG
(`rand()`/`prng_step`). Otherwise `p2_dispatch_move_state_handler` indexes the static
DGROUP `0x870` near-pointer table by `p2_move_state` into one of
`p2_pick_move_priority_a/b/c`/`p2_ai_dispatch_move`, each of which branches on
`rng_frame < p2_ai_threshold` and `rng_frame & 1` parity to call
`p2_ai_select_move_a/b` or `p2_choose_move_state1/2`. A second, distinct table,
`p2_state_handler_tbl` (DGROUP `0x85c`, indexed `move_state*2`), holds the four
per-cell movement handlers `p2_cell_move_up/down/left/right`, invoked by
`p2_run_move_state_handler` only once P2's script index reaches its terminal step.
Both `0x870` and `0x85c` are confirmed **static image data** (no runtime writer) — an
opcode scan proved it, correcting an earlier assumption that they were
runtime-populated.

**Collision.** `check_pvp_collision` (1000:50fb) is a separating-axis AABB overlap
test between `pvp_p1_bbox`/`pvp_p2_bbox` (each recomputed every tick by
`update_p1_bbox`/`update_p2_bbox` as a `[-5,+6]×[-5,+5]` box around the player's pixel
position), gated to active two-player play. On overlap it sets `pvp_collision_flag`
and plays a device-selected contact sound.

## Animation channels and entity draw pipeline

Two independent animation-channel layers advance and draw sprites that aren't P1/P2:
**layer A has 3 slots**, **layer B has 4 slots** (`ANIM_A_SLOTS`/`ANIM_B_SLOTS`,
`src/anim.h`). Each slot is a 12-byte record (`anim_chan_rec`): `active` (0 free / 1
active / `0xff` table terminator), the grid `cell` it animates, a far pointer to its
byte-script stream, the current `frame` byte, and a far pointer to the current frame's
blit data.

**Per-tick step** (`step_anim_channels_a/b`, 1000:14e4/15a1): for each active slot,
read one byte from the stream, advance the cursor, and store the byte as `frame`. A
byte of `0xff` ends the channel (`active = 0`); `0` is stored with no further lookup;
any other byte indexes the frame table (`anim_a_frame_tbl`/`anim_b_frame_tbl`) to load
the frame's blit-data far pointer.

**Per-tick draw/erase**, each following the same **erase → blit → save-under**
sequence used by every entity draw path:

1. **erase** — `restore_bg_view` restores background at the *previous* cell.
2. **blit** — the sprite/tile pixels are actually written (the only visible-drawing
   step).
3. **save-under** — `render_player_view` copies the region for next tick's erase.

`draw_anim_channels_a/b` (1000:165e/17c7) and `erase_anim_channels_a/b` (1000:1a67/
1b2b) run this sequence per active slot, each channel touching its own view
descriptor. There are **seven** distinct view-descriptor globals, not one shared
struct:

| Global | DGROUP | Role |
|---|---|---|
| `anim_a_erase_view` | `0x8d4` | erase pass, layer A |
| `anim_a_draw_view` | `0x8e0` | save-under pass, layer A |
| `anim_a_clear_view` | `0x8c0` | erase pass, layer A (`erase_anim_channels_a`) |
| `anim_b_view0` | `0x8c8` | draw pass 0 (pre-pass), layer B |
| `anim_b_view1` | `0x8cc` | shadow/mask draw pass, layer B |
| `anim_b_draw_view` | `0x8d0` | save-under pass, layer B |
| `anim_b_clear_view` | `0x8bc` | erase pass, layer B |

Each is accessed as a raw far byte pointer with hardcoded offsets, not a named C
struct: `+6`/`+8` = X/Y (view), `+0x14`/`+0x16` = X/Y (erase/clear view), `+0x10`/
`+0x12` = work-buffer far pointer, `+0x1c` = flag bits (e.g. layer-A erase sets
`0x400`/`0x600` for odd cells — a half-tile offset, not a bug).

**Channel-A allocation** is `apply_cell_animation` (1000:69aa, `src/anim.c`): scans
the slot table for one already matching the target cell, else the first free slot,
and stamps the base tilemap with the action's tile. **Channel-B allocation** is
`apply_contact_action` (1000:6a89, `src/player.c`) — the same claim pattern, driven by
tile-contact events rather than a fixed call site.

**Entity draw** (`src/entity.c`) runs the same three-step sequence per grid cell for
the level's static content: `entity_draw_layer_a`/`_b` walk the 48-cell (6×8) grid
reading the BUM header's per-cell tile bytes (`+0x00`/`+0x30`, skipping column 7 for
B) and the animation descriptor tables; `entity_draw_layer_c` is purely level-data
driven with no animation-channel involvement (frame = tile value + `0x179`). P1/P2
themselves are drawn by `draw_p1_sprite`/`draw_p2_sprite` (1000:1cb2/1cea, in
`player.c`/`player2.c`, not `entity.c`) — `draw_p1_sprite` hides P1 when
`move_anim == 100`; `draw_p2_sprite` hides P2 when `p2_cell == -1`.

**Position tables**: `anim_posA_tbl`/`anim_posB_tbl` (DGROUP `0xf4`/`0x3f4`) hold the
48-entry `{x,y}` pixel-position tables for layers A/B; `p2_cell_coord_tbl` (a.k.a.
posC, DGROUP `0x274`) is shared by layer-C placement and P2. `anim_a_frame_tbl`/
`anim_b_frame_tbl` (DGROUP `0x3d6a`/`0x40a6`) are runtime far-pointer tables built at
boot by `init_anim_data()` from generated data in `src/anim_data.c` (extracted
verbatim from the binary — a permuted, non-linear per-type index remap, not a formula;
an earlier draft that computed these positions analytically instead of reading the
real tables was a documented invention, since corrected).

## Level loading and world data

`start_level` (1000:2d14, `src/level.c`) is the level-load entry point:

1. Allocate the level's runtime buffers (`level_alloc_buffers`).
2. Look up the level's move-descriptor and anim-coordinate table
   (`worldmap_move_descriptor`/`worldmap_anim_coord`, `src/worldmap_data.c`) —
   `current_level` (the overworld node just entered) directly indexes these.
3. Build the `D<n>.PAV`/`.DEC`/`.BUM` filenames and stream-decode each into its
   buffer, plus the `BUMSPJEU.BIN` sprite bank.
4. `level_populate_dg()` fills the entity-DGROUP shadow (position tables, P1/P2
   sprite far pointers, `p2_cell = -1` until spawn sets it), then renders the level.

**Buffer sizes** (`src/level.h`, matching the original's own malloc sizes exactly):
`level_pav_buf` (playfield atlas) `0x7806` bytes, `level_dec_buf` (décor) `0x2f96`
bytes, `level_bum_buf` (objects/spawn) `0x0b60` bytes. (`fullscreen_buf` and its
`0x7d63`-byte decode size belong to the menu/screen system, `src/screens.c` — a
different subsystem from the level PAV/DEC/BUM pipeline.)

**Spawn** (`spawn_and_draw_level_entities`, 1000:2a78, `src/spawn.c`) runs once per
level load: resets the animation-channel slot tables, reads P1/P2 spawn fields
straight out of the BUM header (`p1_cell`, `level_exit_cell`, `items_remaining`,
`p2_cell`, `p2_ai_threshold`, `p2_move_state`), and populates the initial anim-channel
records and layer-C static sprites for the level's 48-cell grid.

**Items** (`src/items.c`): `move_step_read_item` reads the layer-C tile under P1;
`p1_collect_item` clears the tile, awards score by item type (with the original's
exact carry-correct 32-bit add), and decrements `items_remaining` — reaching 0 arms
the level-complete animation. `check_exit_tile_vert` and `teleport_to_next_exit_tile`
handle the level-exit and mid-level teleport tiles.

### Level and world art: the VEC/op12 interpreter

Level playfields, décor, and full-screen art (`TITRE`, `MONDE1..9`, etc.) are all
decoded by the same interpreter, `.VEC`'s `vec_run` (segment `1c28`) — full record
format, opcode table, and the op4 RLE algorithm are documented in
[formats/VEC.md](formats/VEC.md); the container header shared with `.PAV`/`.DEC`/
`.BUM` is in [formats/README.md](formats/README.md#the-shared-container-vecpavdecbum).
`src/op12.c`'s `op12_vec_run` is the faithful C port of the decode-arena interpreter
(op4 decompress + op12 masked-blit, byte-for-byte validated); `src/vec.c` provides the
simpler planar-decode path. `level.c`'s per-file load (above) and `screens.c`'s
full-screen loaders are the two call sites.

## Input

`poll_input` (1000:1dde) reads `read_input_action` (1000:75a2) and — only on a
nonzero result — latches it into `input_state` (a **latch**, not a per-tick reset;
clearing is the caller's job). `read_input_action` is a two-phase bytecode
interpreter over a per-handler script (`g_joystick_handler_table[16]`, DGROUP
`0x4cf2`): phase 1 accumulates joystick axis state (`poll_joystick_state` →
`read_joystick_axes`, game port `0x201`); phase 2 scans keyboard scancode groups,
ORing in a group's bit if any of its scancodes is down in `g_key_state_table`
(DGROUP `0x4d42`). `input_state` bits: `1`=up, `2`=down, `4`=left, `8`=right,
`0x10`=fire. The default key bindings are the arrow keys (`0x48`/`0x50`/`0x4b`/
`0x4d`) and Enter (`0x1c`) for fire — see
[playable-dos.md](playable-dos.md#audio-devices) for the full scancode table.
`install_keyboard_isr` (1000:798a) saves the original INT 9 vector and installs the
game's own handler; `flush_keyboard_buffer` (1000:7b01) resets the BIOS keyboard
buffer.

## Memory / buffer map

`alloc_level_buffers` allocates every level buffer up front; `start_level` reads each
level file and decompresses it in place (op4 RLE):

| Buffer global | malloc size | Holds (decompressed) |
|---|---:|---|
| `level_pav_buf` | `0x7806` (30726 ≈ 320×192 4-plane) | `D<n>.PAV` — playfield |
| `level_dec_buf` | `0x2f96` (12182) | `D<n>.DEC` — décor |
| `level_bum_buf` | `0x0b60` (2912) | `D<n>.BUM` — objects/Bumpy |
| `fullscreen_buf` | `0x7d63` (32099) | full-screen image (menu/world-map/title) |

The resource descriptor table at DGROUP `0x90` (`{name_off, name_seg, disk_id, size,
ptr}` per entry) supplies these sizes and the per-resource filenames (the level digit
is patched into the filename at load time) — see [data-files.md](data-files.md) for
the full resource-table format.

## Audio subsystem

Addresses below are `segment:offset` in the unpacked image (or `DGROUP:off`). This
documents the **original** as reconstructed in `src/sound.c` and `src/midi.c`;
deviations are recorded in [reconstruction-fidelity.md](reconstruction-fidelity.md).

### Two engines sharing one hardware/timer substrate

BUMPY.EXE has **two separate audio engines**, both living in Ghidra segment `1000`:

| Engine | Module | Drives | Data source |
|---|---|---|---|
| **Effect-tone engine** | `src/sound.c` | short SFX (jump, land, collect, contact, exit, menu blips) | procedural tone sweeps — no asset file |
| **MIDI music engine** | `src/midi.c` | the background music | `BUMPY.MID` (SMF) + `BUMPY.BNK` (OPL2 instruments) |

They are not two independent stacks: `run_game_session` calls `sound_select_device`
(`6de3`) **once** per session, which runs `snddrv_init` (`88e5`) +
`select_sound_device_from_mask` (`891e`) to probe/pick a device and set the shared
`snddrv_mode` (PC-speaker `0`/OPL2 `1`/MPU-401 `4`) + `sound_active_device_mask` — the
one piece of engine state that gates **both** engines' hardware output afterward.

Both engines then reach the **same** L4 hardware primitives (below) and install their
timer needs through the **same** L3 far-callback table (`set_timer_slot_raw`/
`arm_timer_callback`, DGROUP `0x5516`), serviced by the **same** PIT/int-8 ISR
substrate. `snddrv_dispatch_a/b/c/d` (`85b5/85db/8600/8626`) all live in `sound.c` and
fan out on the same `snddrv_mode`, but per the reconstructed call graph, **none of the
four is reached from the effect-tone engine's own SFX path** — all four are
exclusively called by the **MIDI engine**: `snddrv_dispatch_a` silences the previous
device before playback starts, and `snddrv_dispatch_b/c/d` are `midi_process_event`'s
own per-event dispatch (below).

### The effect-tone engine (`src/sound.c`) — SFX

A five-layer pipeline, transcribed 1:1 except the L5 ISR (behavior-faithful,
documentation-only — see Register-entry conventions below):

| Layer | Functions | Role |
|---|---|---|
| **L1 dispatch** | `play_sound` (`6e11`), `play_sound_effect` (`6e30`, a 21-case effect→tone-parameter switch), and 6 event wrappers — `play_action_sound`, `play_contact_sound`, `play_exit_sound`, `play_pickup_sound`, `play_event_sound_64c1`, `play_state_sound_647e` | Each event wrapper indexes one of 6 per-device 0x30-byte LUTs by the current game event/state to get a sound id, then calls `play_sound`. |
| **L2 device state** | `sound_select_device` (`6de3`), `snddrv_init` (`88e5`), `select_sound_device_from_mask` (`891e`), `snd_busy_delay` (`872e`) | The once-per-session device init/select state machine, and `snddrv_mode`. |
| **L3 tone-submit + timer-table mgmt** | `schedule_timer_callback_a/b/c`, `set_timer_slot(_raw)`, `arm_timer_callback`, `disable_timer_callback`, `timer_restore` | Fills the 10-word tone parameter frame `snd_param_frame[0..9]` and installs a far PIT-timer callback. |
| **L4 hardware drivers** | `pc_speaker_silence`, `speaker_gate_reset/strobe`, `opl_write_reg`, `opl_play_note`, MPU-401 byte/sample/settle, `opl2_all_notes_off`, `opl2_reset_all_regs` | Real port I/O: PC-speaker gate/PIT-ch2 (port `0x61`), MPU-401 UART (`0x330`/`0x331`), OPL2/AdLib register file (`0x388` status / `0x389` index+data). Validated by a port-write-sequence differential. |
| **L5 ISR tone-sequencer** | `pit_timer_isr_multiplexer` (`7c02`), `tone_seq_callback_*` | The PIT/int-8 IRQ0 handler: once per tick it walks the `0x5516` callback table and, on each slot's reload period, sweeps the installed tone-sequencer callback. |

### The MIDI music engine (`src/midi.c`) — music

A straightforward SMF sequencer that plays `BUMPY.MID` by driving the L2/L4 hardware
layers `sound.c` already implements — it has no separate hardware driver of its own.

**Load/parse:** `midi_load_sequence` (`87cd`) stages `BUMPY.MID` + `BUMPY.BNK` as far
pointers and calls `midi_parse_file` (`8809`), which validates the `MThd` header (see
[formats/MID.md](formats/MID.md)) and fills `midi_track_ptr_table[16][2]` with each
`MTrk` chunk's start; `midi_init_track_table` (`87a2`) seeds each track's first event
time. `midi_install_tempo_timer` (`86e9`) computes the PIT reload value from the
`FF 51` tempo meta-event and installs it.

**Per-track dispatch, `midi_process_event` (`873c`):** for each track whose next event
is due, decodes one status byte and dispatches. Meta events (`0xFF <type> <len> …`)
are handled locally (tempo, end-of-track, channel-prefix). Channel-voice/system bytes
are forwarded by status-byte range to the already-reconstructed `sound.c` L2 backends:
`<0xF0` → `snddrv_dispatch_d`, `==0xF0` (SysEx) → `snddrv_dispatch_c`, `==0xF7` →
`snddrv_dispatch_b` — each fanning out on `snddrv_mode` to one of 3 mode-specific
handlers (9 total, `snddrv_dispatch_{b,c,d}_mode{0,1,4}`).

**MIDI-to-OPL2 voice bridge (mode 1):** `snddrv_dispatch_d_mode1` (`8e58`) routes by
MIDI status: **Program Change (`0xC0`)** → `midi_emit_voice_msg_w3→w2→w1` →
`emit_midi_voice_message` (`8bc8`), which locates the instrument's 30-byte OPL2 patch
descriptor in the loaded `BUMPY.BNK` data (see [formats/BNK.md](formats/BNK.md)) and
writes it into the OPL2 register file; **Note On (`0x90`)** → `opl_event_note_on` →
`opl_play_note` (`905d`), which computes the OPL2 F-number/block and key-ons the
voice; **Note Off (`0x80`)** clears the key-on bit directly. On mode 0 (PC-speaker,
no OPL2 registers), Program Change instead calls the lighter `seq_set_channel_param`.

```
BUMPY.MID (SMF fmt1, 7 tracks, division 192)
   │  midi_load_sequence → midi_parse_file → midi_init_track_table
   ▼
midi_process_event  (per due event, per track)
   │  meta (tempo/EOT/…) ──────────────────► handled in midi_process_event
   │  channel-voice / SysEx ──► snddrv_dispatch_{b,c,d} ─► mode{0,1,4} handler (sound.c)
   ▼                                                         │ (mode 1 = OPL2)
mode-1 Program Change (0xC0)                                 │ mode-1 Note On (0x90)
   │  midi_emit_voice_msg_w3→w2→w1                           │  opl_event_note_on
   │  (index BUMPY.BNK instrument via the loaded song data)  │  → opl_play_note
   ▼                                                         ▼
emit_midi_voice_message                              OPL2 register file (0x388/0x389)
   │  writes the 30-byte rol0NN OPL2 patch (BUMPY.BNK)
   ▼
opl_write_reg  →  audible OPL2 FM voice
```

`BUMPY.BNK` is a standard AdLib instrument bank (129 named `rol0NN` patches);
`BUMPY.MID` is a plain 7-track SMF with no Loriciel container — see
[formats/BNK.md](formats/BNK.md) and [formats/MID.md](formats/MID.md).

### Register-entry conventions and carve-outs

Most of the MIDI engine's leaf functions — and several sound-effect L4 drivers — are
**register-entry** in the original: no stack arguments, only ambient CPU registers a
hand-written asm caller left staged. The reconstruction models each such register as
a file-scope global standing in for it, never inventing a stack-arg signature the
binary doesn't have. Two carve-outs: **the tempo-ISR playback loop** (reconstructed as
documentation but not runtime-gated — reached only via an installed far pointer with
no Ghidra function boundary, driven by a hardware timer interrupt a deterministic host
replay can't reproduce), and **the 9 MIDI mode-dispatch handlers** plus a few
sound-effect leaves with pure-ambient-register inputs (reconstructed and linked, but
validated by inspection against the disassembly rather than a runtime differential,
for lack of a captured register-state trace to replay).

### Target playback hardware

The device the engine drives is picked once at the boot sound-device menu — **None**
(F5), **PC-speaker** (F6), **AdLib/OPL2** (F7), or **MT-32/MPU-401** (F8) — which sets
`sound_device_state` (DGROUP `0x689c`) and the shared `snddrv_mode`. Two of the three
sounding devices target a **specific** period card, and faithful playback out of the
(byte-accurate) reconstruction depends on emulating *that* card, not a
superficially-similar one:

- **MT-32 path (device `4`) targets the Roland CM-32L / LAPC-I, not a bare MT-32.**
  Both the MIDI engine and the effect-tone engine's device-4 SFX branch address the
  rhythm part with note numbers that only the CM-32L-class extended
  rhythm/sound-effect bank defines (e.g. the overworld→level "enter" sound plays note
  `0x53`/83 — unassigned and silent on a first-generation MT-32 control ROM, audible
  on the CM-32L). Some lower keys the game uses are mapped on both, so a bare MT-32
  plays *some* effects and drops the CM-32L-only ones.
- **AdLib path (device `1`) is a single OPL2 (YM3812)** at ports `0x388`/`0x389`.
  Because the register writes are byte-faithful, the audible envelope is whatever the
  *emulated* OPL2 produces — an approximate FM core can add an onset transient a
  cycle-accurate core doesn't.

The emulator settings that satisfy both (built-in `munt` with CM-32L ROMs; the
`nuked` OPL2 core) are in
[playable-dos.md](playable-dos.md#running--playing-it-under-dosbox).
