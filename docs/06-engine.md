# 06 — Engine: game loop, buffers, level pipeline (decomp in progress)

Reverse-engineered in the live `BumpyDecomp` Ghidra project. Names below are
applied there.

## Decompilation coverage (post multi-agent fan-out)

A multi-agent workflow (script `workflows/scripts/bumpy-decomp-fanout-*.js`) fanned
out ~25 agents over slices of the function list; combined with manual work, **~340
of ~399 functions are named, typed, and commented** in the `BumpyDecomp` Ghidra
project (the ~60 left are C-runtime startup garble, tiny thunks, and low-confidence
stubs — deliberately not guessed). Subsystem inventory:

| Subsystem | Segment | Representative functions |
|---|---|---|
| Game loop / session | `1000` | `game_loop`, `run_game_session`, `init_game_session_state`, `load_current_level_data`, `level_intro_screen` |
| Movement / physics SM | `1000` | `p1_movement_dispatch`, `enter_game_mode`, `dispatch_move_step`, `move_*`, `gamemode_*`, `land_on_tile_below`, `teleport_to_next_exit_tile`, contact tables `0x4256–0x42f6` |
| Two-player pipeline | `1000` | `p1/p2_*` grid/view/sprite/bbox, `render_player_view`, `restore_bg_view`, `check_pvp_collision` |
| Anim channels | `1000` | `step/draw/erase_anim_channels_a/b` |
| Menus / front-end | `1000` | `run_main_menu`, `play_intro_animation_loop`, `play_iris_wipe_transition`, `show_*_screen`, high-score |
| Timer / PIT / ISR | `1000` | `pit_set_counter0`, `pit_read_counter0`, `install/uninstall_interrupt_handler`, `arm/disable_timer_callback`, `set_timer_slot*` |
| Input | `1000` | `poll_input`, `handle_gameplay_input`, `flush_keyboard_buffer`, joystick globals |
| Sound | `1000` | `play_sound`, `snddrv_dispatch_a`, OPL/MIDI globals |
| Resources / CRT | `1000` | `open_resource`, `read_chunked`, `xfer_chunked`, `malloc`, heap/atexit/streams |
| Font / text + graphics overlay (Loriciel-custom, NOT Borland BGI — `gfx_*`, renamed from `bgi_*`) | `1ab9` | `draw_char_glyph`, `draw_string_glyphs`, `measure_string_width`, `font_glyph_ptr`, `gfx_set_mode_*` |
| Sprite codec | `1cec` | `sprite_rle_decode`, `sprite_rle_encode`, `decode_2bpp_planes`, `blit_sprite_vga` |
| Vector renderer | `1c28`/`1cda` | `vec_run`, `vec_read_record`, `vec_xform`, `low_nibble` |
| PRNG | `1ce5` | `prng_seed`, `prng_step` (3-word state, consts `0x2432/0x1c12/0x3812`) |

### Note: far-pointer / long globals are kept SPLIT

Adjacent global pairs that form one far pointer (`_off`/`_seg`) or one 32-bit value
(`_lo`/`_hi`) are deliberately left as two named 2-byte items rather than merged
into a single 4-byte typed symbol. We evaluated merging (a headless Java script on
a throwaway project copy): it runs cleanly but makes the decompiled C *worse* —
consumers like `start_level` re-split the value at every call
(`read_chunked((uint)(undefined*)level_pav, level_pav._2_2_, …)`) because the I/O
routines take the halves as separate params, and pointer derefs degrade to
`*(int*)(ptr+2)` byte-offset casts. This is 16-bit segmented code that genuinely
passes and manipulates far pointers as two words, so the split form matches the
code and reads better. Merge rejected; pairs stay split.

## Main game loop — `game_loop` (`FUN_1000_0c18`)

The top-level play loop. Structure:

```
game_loop:
  FUN_1000_33c5(); FUN_1000_2ef8()          // one-time init
  restart:
    ... menu/select (FUN_1000_35a5 -> 5681 / 0f7a) ...
    do:
      start_level()                          // load + decompress level resources
      per-frame loop:
        FUN_1000_3852()                      // input/timing
        if (game_state == -1) -> died, restart
        <~30 update/draw calls per frame>    // physics, enemies, blits, collision
      on level clear: current_level++; if (==10) wrap   // 9 levels
```

`game_state` = `DAT_203b_928d` (0 = playing, -1 = died). The ~30 per-frame calls
are the engine: object updates, enemy AI, sprite blits, collision.

### Per-frame engine functions (decomp in progress)

The loop runs the **same pipeline for two players** (Bumpy supports 2-player co-op;
`two_player_active` / `DAT_8571` gates all the P2 calls). Mapped so far:

| Function | Name | Role |
|---|---|---|
| `FUN_1000_1473` | `p1_update_grid_cell` | pixel→grid cell for P1: `(pixel - view_origin) >> {4,3}`, clamped to the 19×23 playfield |
| `FUN_1000_4b4e` | `p2_update_grid_cell` | same for P2 |
| `FUN_1000_138c` | `p1_advance_grid_history` | shift grid pos: new→current→previous (for erase/redraw) |
| `FUN_1000_13b2` | `p2_advance_grid_history` | same for P2 |
| `FUN_1000_14e4` | `step_anim_channels_a` | step 3 animation channels — per-channel byte-script (`0xFF`=end), byte indexes frame table `0x3d6a` → data ptr |
| `FUN_1000_15a1` | `step_anim_channels_b` | same, 4 channels, table `0x40a6` |
| `FUN_1000_1bd7` | `render_p1_view` | compute P1 camera/scroll (center, 4 cells from edge, clamp at bounds), fill `p1_view` struct, call `render_player_view` |
| `FUN_1000_1c41` | `render_p2_view` | same for P2 |
| `FUN_1000_93b8` | `render_player_view` | the per-player viewport draw (consumes the view struct) |
| `FUN_1000_13df` / `4c14` | `p1_step_scripted_move` / `p2_step_scripted_move` | apply a canned movement script (triples `[anim,dx,dy]`; `dx` negated if facing left) to the player's pixel pos; decrement steps-left |
| `FUN_1000_19e4` / `19a1` | `erase_p1_view` / `erase_p2_view` | erase pass: set view origin to the player's *previous* grid cell, `restore_bg_view` |
| `FUN_1000_80bc` | `restore_bg_view` | draw background tiles for a view region (erase primitive) |
| `FUN_1000_1a20` | `restore_bg_pending` | timed background restore at `pending_erase_x/y` while `pending_erase_count`>0 |
| `FUN_1000_165e` / `17c7` | `draw_anim_channels_a` / `draw_anim_channels_b` | draw the 3/4 animation channels (erase old cell, look up frame pos+sprite, blit) |
| `FUN_1000_1a67` / `1b2b` | `erase_anim_channels_a` / `erase_anim_channels_b` | erase pass for the anim channels |
| `FUN_1000_5085` / `50c0` | `update_p1_bbox` / `update_p2_bbox` | collision AABB = `[x-5, x+6, y-5, y+5]` |
| `FUN_1000_1cb2` / `1cea` | `draw_p1_sprite` / `draw_p2_sprite` | blit the player at pixel pos with current anim frame (`frame==100` = hidden) |
| `FUN_1000_50fb` | `check_pvp_collision` | 2-player AABB overlap → `players_colliding` + bounce |
| `FUN_1000_1d26` | `handle_gameplay_input` | debug/cheat F-keys + movement input dispatch |
| `FUN_1000_4c99` | `p2_tile_move_check` | read `tilemap` around `p2_cell` for blocked dirs, dispatch by `p2_move_state` |
| `FUN_1000_1e02` | `p1_movement_dispatch` | **the physics state machine** (below) |
| `FUN_1000_3852` | `level_intro_screen` | per-level intro: draw border+HUD+Bumpy, wait for fire to start |

View struct (`p*_view`/`p*_erase_view`): `[+6]`=grid_x, `[+8]`=grid_y, `[+0x14/0x16]`
=view_origin, `[+0x1e]`=scroll_x, `[+0x20]`=scroll_y, `[+0x1c]`=flags. Player state
globals are named `p1_*`/`p2_*` (`p{1,2}_pixel_x/y`, `grid_x/y[_prev/_new]`,
`scroll_x/y`, `move_script/steps_left/anim`, `facing_left`, `bbox_{l,r,t,b}`,
`start_x/y`, `cell`, `sprite`, `view`).

### Physics / movement state machine

`p1_movement_dispatch` (`FUN_1000_1e02`) is the core: it dispatches through the
jump table **`game_mode_handlers`** (DGROUP `0x7ca`) indexed by `game_mode`
(`DAT_792c`). Each `game_mode` value selects a movement behaviour; modes
`5/0xb/0x1c` disable scripted-move stepping. `physics_frozen` (`DAT_a0ce`) gates
bbox/movement updates. Collision is grid-based: `tilemap` (`DAT_a0d8`) is probed
around each player's cell for blocked directions, plus AABB tests for player-vs-
player. The per-frame loop runs **erase → update → draw** for two players and the
3+4 animation channels, double-buffered via `present_frame`.

### `game_mode_handlers` map (per-mode physics)

The jump table (DGROUP `0x7ca`, indexed by `game_mode`) — most modes share a
default; the distinct handlers:

| mode(s) | handler | role |
|---|---|---|
| most (0,1,2,4,6–9,17–27,…) | `gamemode_default_idle` (`28f9`) | idle: process `p1_pending_action` (space→sound, 0x16→`FUN_4305`, 3→`FUN_463d`, else `handle_move_input`); else default-move by `p1_cell` |
| `0x21` | `gamemode_21_start` (`1e5e`) | start/launch; if `p1_contact_code==8` sound+`gamemode_26_contact`, else →`0x24` |
| `0x22` | `gamemode_22` (`1e90`) | mirror of 0x21; else →`0x23` |
| `0x23` | `gamemode_23_walk` (`1ec2`) | `step_walk_anim(0xb,5,…0x1ca4)`; down→`FUN_1f03` |
| `0x24` | `gamemode_24_walk` (`1f3e`) | `step_walk_anim(0xb,5,…0x1cba)`; down→`FUN_1f7f` |
| `0x25` | `FUN_2138` | (fall/other; from 0x22 land path) |
| `0x26` | `gamemode_26_contact` (`21e7`) | contact/landing: index `contact_transition_tbl` (`0x42f6`) by `p1_contact_code` → next action |
| `0x1c` | `FUN_4344` | (special) |
| `0x1d–0x20` | `FUN_4437` | (special, shared) |
| `0x10`,`0x2c` | `FUN_22b0` | |
| `3`,`0xf` | `FUN_23b6` | |
| `0xa–0xe` | `2470/248e/24d7/250a/25ad` | |

**Movement primitives:** `step_walk_anim(anim_base,period,frame_off,frame_seg)`
advances `anim_frame_ctr` and applies a frame every `period` ticks;
`handle_move_input` dispatches L/R to `p1_move_left`/`p1_move_right` (which read
`move_left_tbl`/`move_right_tbl` `0x36ee`/`0x371e` by `p1_pending_action`).
`play_sound(id)` (`6e11`) gates on `sound_mode` (DGROUP `0x689c`, flat
`0x26c4c`; `0x8000`=off, `==4` selects an alt SFX set). Collision uses
`p1_contact_code`/`p1_cell` against the `tilemap`.

### Movement transition mechanism

The state machine is driven by three central functions:

- **`enter_game_mode(mode)`** (`4263`) — the transition: sets `game_mode` and loads
  that mode's movement script from **`mode_script_tbl`** (DGROUP `0x2252`):
  `p1_move_steps_left`, `p1_facing_left`, and the `[anim,dx,dy]` `p1_move_script`.
  Gated by `move_locked` (`DAT_8242`). Modes `5/0xb/0x1c` carry no script.
- **`dispatch_move_step`** (`238e`) — called at the tail of each move handler;
  continues the sequence via the 2D table **`move_step_dispatch_tbl`**
  (`0x43c0`, indexed `[game_mode][p1_move_step_idx]`, stride `0x22`). The table holds
  raw little-endian **near offsets** the engine `CALL`s directly (in real mode the
  offset *is* the code address). In the `src/` reconstruction the byte table is kept
  byte-identical and a host `move_step_handler_for_offset` resolver maps each offset to
  its reconstructed C function (the one isolated host-execution shim; see
  `docs/reconstruction-fidelity.md` Phase-9 T2).
- **`exec_move_action(action)`** (`46bb`) — maps an action code to a directional
  primitive: `move_down` (`4747`, randomises via `rng_frame` when the tile above
  is clear), `move_left` (`2634`), `move_settle` (`27de`), etc.

Contact/landing handlers (`gamemode_25_contact`, `gamemode_26_contact`) resolve
collisions by indexing `contact_transition_tbl_a/b` (`0x42f6`/`0x42d6`) and
`contact_action_tbl_left` (`0x4256`) by `p1_contact_code`, then `enter_game_mode`.
`step_walk_anim` advances the walk frame every N ticks; `play_sound(id)` gates on
`sound_mode`. So the full movement spine is:
**`p1_movement_dispatch` → `game_mode_handlers[mode]` → (move primitive / contact
resolve) → `enter_game_mode(next)` + `dispatch_move_step`**, with the per-frame
`p1_step_scripted_move` applying the loaded `[anim,dx,dy]` script to pixel pos.

Still unnamed (next): the per-mode callees `FUN_2138`-family done; remaining
`FUN_22fc/4344/4437/2470/248e/24d7/250a/25ad`, the move sub-primitives
(`270c/26a1/2776/1fbe/207d/472d`), enemy AI, scoring, the sound driver
(`FUN_6e30`+), menu/high-score, and `FUN_1000_1349`.

## Memory / buffer map

`alloc_level_buffers` (`FUN_1000_0416`) `malloc`s (`FUN_1000_808e`) every level
buffer up front; `release_level_buffers` (`FUN_1000_0569`) frees them.
`start_level` (`FUN_1000_2d14`) reads each level file and **decompresses it in
place** (op4 RLE) into its buffer:

| Buffer global | malloc size | Holds (decompressed) |
|---|---:|---|
| `level_pav_buf` (`DAT_6fa6`) | `0x7806` (30726 ≈ 320×192 4-plane) | `D<n>.PAV` — playfield |
| `level_dec_buf` (`DAT_6be8`) | `0x2f96` (12182) | `D<n>.DEC` — décor |
| `level_bum_buf` (`DAT_75de`) | `0x0b60` (2912) | `D<n>.BUM` — objects/Bumpy |
| `fullscreen_buf` (`DAT_7926`) | `0x7d63` (32099) | full-screen image (GRILLE/screen) |
| others (`DAT_75da`,`6c2c`,`a0c6`,`6c30`) | `0x7c3`/`0x898`/`0x5c70`/`0x500` | work / object buffers |

The descriptor table at DGROUP `0x90` (`{name_off, name_seg, disk_id, size,
ptr}` × N) supplies these sizes and the per-resource filenames (the level digit
is patched into the filename at load time).

## Key finding: level/world data is **structured**, not a bitmap

The decompressed `.PAV/.DEC/.BUM/MONDE` buffers do **not** render as rasters at
any width/plane layout (autocorrelation ~0.1 vs TITRE's 0.7; tile-sheet and
delta tests also fail). The op4 RLE decode is correct (cross-checked with the
Unicorn oracle, `tools/render/vec_oracle.py`), so the buffers genuinely hold
structured data — tilemap / object-placement / draw-command records — that the
per-frame draw functions interpret. Only the full-screen images (`TITRE`, and
the `0x7d63` resources) are direct rasters.

## The renderer is runtime-generated code (architectural wall)

Tracing the blit (`FUN_1000_942a` → `FUN_1000_1cec_31b7`) shows the actual
pixel-pushing is **not in the static load module**:

- `1cec:31b7` does `MOV CS:[0x320e],AL` — **self-modifying code**, patching a
  transparency/colour byte into the blit, then `CALL 0x2000:fcad` / `0x2000:fc2d`.
- Those targets are **beyond the load module** (in BSS/heap), and `1cec:320e`
  is zeros statically. So the blit is a **runtime "compiled sprite"**: a resident
  generator emits unrolled x86 blit code (sprite pixels baked in as immediates)
  into a heap buffer at load time. Classic Turbo C technique.
- `BUMSPJEU.BIN` is **not** that code (disassembles as data — it's the sprite
  source bitmaps), and neither EXE copy has an overlay area appended.

**Consequence:** the world/level *pixel* rendering cannot be recovered by static
decompilation — the code that draws it is generated at runtime. The structured
`.PAV/.DEC/.BUM/MONDE` buffers + the compiled-sprite blitter together produce the
image, but the blitter only exists in a running instance.

## Dynamic breakthrough — the from-scratch emulator runs the renderer

Rather than wait for DOSBox-X, we built a **dependency-free DOS emulator**
(`tools/render/dosemu.py`, ~370 lines on Unicorn UC_MODE_16) that boots the real
unpacked binary and serves INT 21h/10h/16h/33h, the PIT/keyboard/VGA-status ports,
and a full **VGA planar** model (4 plane buffers, sequencer/GC registers, write
modes 0–3, latches, DAC). It now drives the game end to end:

1. **Boots** the MZ image, runs C startup + game init.
2. **Renders the title and the menu in full colour** (VGA planar + DAC palette,
   `build/render/dosemu_vga_p*.png`) — the title screen and the blue PLAY /
   HIGH-SCORE / LEVEL / PASSWORD menu, pixel-faithful to DOSBox.
3. **Navigates the menus by keyboard injection** (writes the game's key matrix at
   DGROUP `[0x4d42]`): holds F3+F7 through the video/sound mode menus, pulses the
   **fire** key (space — decoded from the runtime key-binding table at `0x4cf2`:
   fire `0x10` ← ENTER/`0x74`/SPACE) to advance title→menu and select **PLAY**.
4. **Runs `start_level`** — which loads every game resource (BUMSPJEU sprites,
   BUMPY.BNK/.MID audio, MASKBUMP, TITRE) and the level files, reading **`D1.PAV`
   fully** into `level_pav_buf`.
5. **Executes the runtime renderer `vec_run`** (the "architectural wall" above) —
   it is a **vector/RLE record interpreter** at `1c28:0000`: each 12-byte record
   header gives `w4`=opcode, dispatched via the table at DGROUP `0x4e37` (only
   **op4** = RLE-decompress, `1c28:0194`, and **op12** = plot+clip, `1c28:04b0`
   are implemented; the rest are no-op `ret`). op12 plotted **63 868 points**
   (≈ a full 320×192 playfield) — i.e. the renderer **decoded real game art into
   RAM** (a heap scan, `tools/render/scan_heap.py`, shows recognisable sprites —
   a hot-air balloon, bumpers, fruit). The "wall" is no longer static-only: it
   *runs* here.

### `vec_run` calling convention and record format (fully reversed)

`vec_decode` (`1000:7b5a`) is a thin thunk that loads the C args into registers
and far-calls `vec_run` (`1c28:0000`). Both `start_level` calls supply the same
five values per resource — confirmed by the now-clean decompile:

```c
vec_decode(level_pav_buf, level_pav_seg, bytes_read, pav_len_lo, pav_len_hi);
```

Register mapping into `vec_run` (`DI:SI`, `AX:BX`, `CX:DX`) and the DGROUP slots it
seeds:

| C arg | reg | `vec_run` global | meaning |
|---|---|---|---|
| `buf_off` | SI | `vec_stream_off` (`0x4e0e`) | stream (record list) far ptr — offset |
| `buf_seg` | DI | `vec_stream_seg` (`0x4e10`) | … segment |
| `bytes_read` (lo/hi) | AX/BX | `vec_readcount_lo/hi` (`0x4e12/14`) | bytes `read_chunked` actually delivered |
| `declared_len_lo` | CX | → `vec_xform` → `vec_end_off` (`0x4e0a`) | **stream-END bound** (offset) |
| `declared_len_hi` | DX | → `vec_xform` → `vec_end_seg` (`0x4e0c`) | … segment |

So the third arg is the **declared file length** from the resource descriptor
(`pav_len_lo:hi` = DGROUP `0x96:0x98`), which `vec_run` normalises (`vec_xform`
`1000:cda0`) into a far end-of-stream pointer `vec_end_seg:vec_end_off`.

**Records are 12-byte headers** read by `vec_read_record` (`1c28:0a09`) as six
big-endian words `w0..w5`, validated and dispatched by `vec_run`:

- `w0` (`vec_src_seg`) **must be ≤ 0x0f** — else CF set → `vec_run` returns (this
  is the natural terminator). `w0:w1` together (`vec_src_seg:vec_src_off`,
  `0x4e26:0x4e24`) form the record's **embedded source far-pointer** to its data.
- `w2,w3` → `0x4e1e/0x4e20` (auxiliary pointer/operands).
- `w4` (`vec_opcode`, `0x4e31`) — opcode; index `(w4 & 0x3f)` is range-checked
  against `vec_opcode_valid_table` (`0x4e5b`) and `(w4 & 0x7f00)` must be 0.
- `w5` — XOR checksum: must equal `w0^w1^w2^w3^w4`, else CF → return.

Dispatch: `vec_run` indexes `vec_dispatch_table` (`0x4e37`) by `(w4 & 0x7fff)-1`
and calls the handler. Only **op4** (RLE decompress, `1c28:0194`) and **op12**
(plot+clip, `1c28:04b0`) are real; the rest point at a bare `ret` (`1c28:0193`).

**The "stream-pointer advance" is not hidden self-modifying code.** Each handler
walks the record's own `w0:w1` source pointer forward (normalising every step via
`vec_xform` `1cda:0089`) and stops when that pointer crosses the
`vec_end_seg:vec_end_off` bound — op12 enforces it explicitly at its head
(`1c28:04ca`: `cmp dx,[vec_end_seg]; ja ret` / `1c28:04d2`: `cmp ax,[vec_end_off];
ja ret`). op4 (`1c28:0194`) decompresses from `w0:w1` into the output through a
1 KB sliding-window buffer at DGROUP `0x4e97`, terminating when the output reaches
its computed target end (`1c28:046e-0x4aa`).

### Instrumented run — what actually happens (June 2026)

A traced emulator run (`vec_run`-entry + op12-entry + gated heap-write hooks)
nailed down the runtime behaviour:

- **`vec_run` terminates cleanly and is finite** for every resource: `op4`=4,
  `vec_read_record`=10, `op12`=6 (no infinite loop). The earlier "spurious op12
  loop" is gone.
- **`declared_len` is the *decompressed buffer size*, not the file length.** For
  `D1.PAV`: `stream=472d:0000`, `declared_len=30726` (`0x7806`, the PAV buffer
  size), `bytes_read=15071` (the compressed file). `vec_xform(CX:DX)` yields
  `vec_end=4ead:0006 ≈ stream + 0x7806` — exactly the buffer end.
- **The end-of-stream marker is opcode `0x800c`.** Its high bit makes `w4`
  *negative*, so `vec_run`'s loop check `cmp [vec_opcode],0; jg` fails and the
  interpreter returns. Confirmed at the buffer end for both the full-screen
  resources (`src=0000:7d63`, buffer `0x7d63`) and `D1.PAV` (`src=0000:7806`).
- **op12 is a recursive vector/transform interpreter, not a tile blitter.** It
  re-enters `vec_run`'s record loop (`1c28:063f: call 0x7e`) and runs nested
  `vec_xform` passes (modes 0 / 0x45 / 0x89) — drawing *transformed primitives*.
  It plotted ≈63 868 points into a heap buffer at **`0x48000–0x4e000`** (the
  hottest post-PAV write pages). Because the output is vector-drawn, no flat
  raster layout (seq/rowint/byteint × widths 160/256/320) reconstructs it — the
  pixels only become a coherent frame once the game's own per-frame blit runs.

### BREAKTHROUGH — the emulator renders live gameplay

After fixing the two walls below, the from-scratch emulator now drives the real
binary **all the way into a rendered level**: title → menu → PLAY → `start_level`
loads `D1.PAV`+`D1.DEC`+`D1.BUM` → the level-intro (teddy-bear) screen → a fire
pulse dismisses it → the **live playfield renders** (platform grid, collectibles,
bumpers, level decorations), captured at `build/render/bumpy_level1_gameplay.png`.
No faults; the game runs its normal per-frame loop. The colour pipeline is now
correct (Attribute Controller → DAC); level 1 uses an 8-colour dark-blue palette.

### Why `start_level` stalls after PAV — two walls found and BOTH fixed

Instrumenting the run (open_resource logger + null-catcher + fault-location
capture) showed `start_level` never reaches the DEC/BUM reads because execution
**crashes during the PAV decode**, not because it stops early. Two distinct walls:

**Wall 1 (FIXED) — saved INT 8 vector was null.** The init routine
`1000:7cde` does `INT 21/AH=35/AL=8` to **save the original timer (INT 8) vector**
to `[DGROUP:0x54d0]`, then installs its own timer ISR at `0x7c02`
(`INT 21/AH=25/AL=8`). That ISR chains back to the original via
`1000:7e7a: pushf; lcall [0x54d0]`. The emulator never installed a default INT 8
handler, so `AH=35` returned `0000:0000`, the saved vector was null, and the
injected timer tick chained into null. Fix: install a bare **IRET** stub
(`0050:0000`) into every still-null IVT vector at boot — exactly what real
DOS/BIOS would provide.

**Wall 2 (FIXED) — was a self-inflicted stub bug.** The first attempted fix
pointed `[0x54d0]` at a `retf`. But `pushf; lcall [vec]` invokes an **IRET**-style
handler (the chain expects FLAGS+CS+IP = 3 words popped); a `retf` pops only 2,
**leaking one word of stack per timer tick**. After ~28M instructions the drift
caused a far-return onto garbage (`CS=0x7cb9`, an offset value used as a segment) —
which looked like the documented "runtime compiled-sprite blitter" wall but was
not. Replacing the stub with the proper boot-time **IRET** handler (Wall 1 fix)
balances the chain and the symptom vanished. (Lesson: a far pointer invoked via
`pushf; lcall` must end in `iret`, not `retf`.)

### Result

With both walls fixed the game runs cleanly into gameplay: `start_level` loads all
three level files, the intro screen shows, a fire pulse advances to the live
playfield, and the per-frame loop renders the level
(`build/render/bumpy_level1_gameplay.png`). The colour path is correct
(Attribute Controller palette → DAC); level 1 is an 8-colour dark-blue theme.

**Colour completeness — missing white blocks (FIXED).** The first gameplay frames
had black rectangular holes where platforms/highlights belong. A pixel-value
histogram showed ~8800 pixels using palette index **15** (and hundreds at 8/13/14)
while DAC entries 8–15 were black. Cause: the game re-loads only DAC entries
**0–7** (its custom level palette) and relies on **8–15 being the BIOS default
EGA palette** (15 = white, …) that `INT 10h` mode-set normally installs. The
emulator's mode-set didn't seed that default, so those entries stayed black. Fix:
pre-seed the standard 16-colour EGA DAC palette at boot. The platforms (white,
index 15) and highlights then render correctly — see
`build/render/bumpy_level1_gameplay_3x.png`.

Remaining polish (not blockers): a few black vertical bars at the far-left/right
screen edges (likely edge-column/pel-pan or level-border art), capturing a
specific gameplay moment deterministically, and DOSBox-X ground-truth comparison.

**Full-screen palette source (SOLVED, pure data).** Every full-screen `.VEC`
(`MONDE*` world maps, `TITRE`, etc.) carries its own 16-colour palette **embedded in
the decoded image** — 16 × 6-bit-RGB triples at offset 51 of the 99-byte header,
planar pixels at offset 99. `level_intro_screen` (`1000:3852`) `vec_decode`s the
screen (resource `current_level+7`) and pushes that embedded palette to the VGA DAC
via `upload_vga_dac_palette` (`1000:9864`, the DAC port writer; 15 callers = every
screen). So full-screen images decode 100% from the file — no DAC capture, no
attribute-map guessing, no screenshot. The pure-Python decoder
(`tools/extract/vec_to_png.py`) is validated **99.95% vs a real DOSBox capture**
(`results/oracle/world1_dosbox.png`). Format details: `docs/formats/VEC.md`.

What is solid: the full calling convention + record format, finite clean renderer
runs, the `0x800c` terminator, op12's plot-buffer location, the decoded sprite
sheet (recognisable Bumpy art at `0x46000`), and the title/menu rendered in
colour.

### Two ways to finish

1. **Finish the emulator path** — nail `DAT_4e0e` advancement (and the op12 plot
   destination buffer + planar layout) so `vec_run` completes; then composite the
   playfield buffer. Closest to a clean world render.
2. **Reverse the renderer statically** — now that op4/op12 and the dispatch table
   are mapped, port the vector interpreter to the greenfield engine directly.

What IS solid and self-contained: asset RLE decompression (all files), full-screen
`.VEC` rendering (`TITRE`), title + menu rendered by the emulator, the renderer's
opcode/dispatch map, the extraction toolkit, and the engine architecture above.
