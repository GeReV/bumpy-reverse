# Session handoff — 2026-07-02 (TEMPORARY working doc, do not commit)

> Handoff for continuing the playable-host bug-fixing effort.  Written explicitly so a
> less capable model can pick this up: every fix below names the evidence, the file, and
> the recognition pattern for finding MORE bugs of the same class.
>
> Governing rule (CLAUDE.md): **ground every change in the binary** (Ghidra decomp +
> asm).  Never fix by guessing.  If you cannot point to the engine code/data your change
> reproduces, stop and read the binary first.
>
> **VERIFY, don't trust this doc** — line numbers drift; re-grep before editing.

---

## 0. State at handoff

- Branch `reconstruct/11-playable-host-platform`, everything **uncommitted** (user
  commits when asked; never commit `local/` or game data).
- Both builds warning-free: default `BUMPY.EXE` (faithful decompilation target) and
  playable `BUMPYP.EXE` (`wmake play`, all playable code `#ifdef BUMPY_PLAYABLE`).
- All 19 validators PASS (`tools/validate_*.sh`; see §6 for how to run).
- Playable status: menu/overworld/levels 1-2 render + play correctly; icon row works.
  **OPEN BUGS**: (a) at least one late world-1 level broken — bg renders, NO platforms,
  input dead (investigation was in flight at handoff — check the task list / rerun it);
  (b) menu-arrow flicker fix (latch copy) + text rendering + pause strip landed but
  NOT yet user-playtested; (c) user suspects input delay (unconfirmed).

## 1. What was wrong and how it was fixed (this session)

### 1.1 Code-review round (10 verified findings, all fixed)

| Bug | Root cause | Fix (file) |
|---|---|---|
| Game modes 0x1d-0x20/0x30 misbehave | `FUN_1000_4437`/`FUN_1000_1e3d` carried each other's BODIES (dispatch table was right) | swapped bodies back (`game_stubs.c`) |
| Wrong layer-A frames | `anim_a_frame_tbl` rebuilt as LINEAR type→desc[type-1]; the binary's table at DGROUP 0x3d6a deviates in 88/197 entries (types 96-197 permuted, 198 NULL) | generator emits the real per-type index table `g_anim_a_frame_idx` (`gen_anim_data.py` → `anim_data.c`) |
| Anim under-erase dead | erase repainted an orphaned RAM buffer; clean-bg snapshot read a never-written buffer | both endpoints rewired to real VGA (`host_render.c`, `view_setup.c`) |
| Wrong palette on levels >1 | `level_packed_palette` returned block 0; engine reads via `cur_level_ptr` (stride 0x32c) | return `cur_level_ptr` (`level.c`) |
| World 9 crash | worldmap blob 4 bytes short (level-9 anim far ptr at DGROUP 0x1110 outside blob+reloc loop) | blob → [0x9e6,0x1114), loop bound +1 (`worldmap_data.c`) |
| P2 frame base garbage | `spawn_p2_frame_tbl` words 0+19 are loader-relocated far-ptr SEG halves (EXE relocs at DGROUP+0x2546/+0x256c) copied unrelocated | init reproduces the loader fixup (`anim_data.c`) |
| Respawn stall + progress wipe | per-round FULL re-decode (3 files + 87 KB bank) with a refuted "buffers clobbered" justification; also re-cleared move-descriptor flags | `load_current_level_data` = faithful 1000:32b0 only (`level.c`) |
| OOM cliff | dead 64 KB flat `host_framebuffer` (nothing displayed it post-VGA-migration) | reclaimed under `HOST_FB_16K` → un-gated the tiledef data (issues C+D landed) |
| default-build IVT store | `draw_anim_seq_frame` writes through NULL `p1_sprite` | NULL guard (`screens.c`) |

### 1.2 Round 2 (playtest bugs)

1. **THE BIG ONE — present is a REAL page flip.**  Engine `present_frame` (1000:7bdd →
   1ab9:0351, pm=2 → **1ab9:06c1**): `CRTC reg 0x0c ^= 0x20` (display 0x0000↔0x2000)
   **+ swap `sprite_table_base[0]↔[1]`** (DGROUP 0x5415: [0]=a200:0000, [1]=a000:0000).
   Descriptor `word00`/`word0e` are **table INDICES**, not fixed pages.  The old host
   modeled present as a NOP on one page, based on a MISREAD of `crtc_page.md` (its
   single CRTC value 0xDF00 = `0xFF ^ 0x20`; the Unicorn VGA model returns 0xFF on
   CRTC reads — the flip fired every present).  The NOP broke the invariant *"a
   present separates draw from save-under"* → the overworld start-node Bumpy ghost +
   trails.  FIX: real port writes in `present_frame` (`host_video.c`), table swap
   (`host_page_table_swap`), every draw/save/erase/compose leaf resolves pages from
   the LIVE table (`host_draw_page_off()` / `host_page_off_of(idx)`, `host.h`), and
   `init_fullscreen_view_desc` is the engine's mode-11 **full-page SYNC copy**
   (1ab9:126e: copy `page[table[word00]] → page[table[word0e]]`), not a present.
2. **Item pickup left ghost pixels + flicker**: the pending-erase staging (disasm
   1000:6ca1-6ce7) existed only as a COMMENT in `p1_collect_item_score`; reconstructed
   (`items.c`), and `restore_bg_pending` re-routed from the P1 save-under leaf (wrong
   source — contains the item ghost) to the clean-bg leaf (`player.c`,
   `host_render.c` identity gate now accepts `pending_erase_view`).
3. **Platform frame at Bumpy's position**: `p1_move_anim` is the engine's **WORD**
   @0x824a (1000:1417/1cc9); bounce modes 0x3d/0x3f use frames 0x1d1..0x1d7 — the u8
   model truncated them onto platform frames.  u8→u16 (`player.c/.h`, `screens.c`);
   `physics_ctest` compares `(u8)p1_move_anim` (its trace stores only the low byte).
4. **No HUD icons**: engine byte DGROUP **0x791a** has FOUR users (game_loop init=5,
   '#' pickup ++, physics settle --, draw_icon_row count) but was modeled as TWO C
   globals (`settle_countdown` + `sharp_item_counter`); the reader saw the always-0
   copy.  Unified on `game.c settle_countdown`.  NOTE: the original has NO persistent
   in-level HUD — icons+score live on the P-key pause overlay and the world map.

### 1.3 Round 3

1. **Menu arrow not erasing → then flickering**: the engine has NO cursor save-under —
   the erase IS the mode-11 page sync, which works only because UI screens bracket
   draws on table slot 0 (`set_sprite_table_ptr(0)…(1)`).  FIX: wired
   `fun_9410_set_sprite_table → host_set_draw_page` (`screens.c`) + **boot CRTC parity
   = page 1 / 0x2000** (`init_crtc_window`, invariant `displayed == page[table[0]]`).
   The subsequent FLICKER was the first mode-11 implementation being slow (8 port ops
   × 8000 bytes per menu frame) → replaced with the VGA **write-mode-1 latch copy**
   (`view_setup.c` — 1 read + 1 write per byte, all 4 planes).
2. **Right-tilt platform trails**: `anim_restore_bg_view_leaf` ignored the descriptor
   `+0x1c` half-tile flag word (engine parser 1ab9:03c5/0400/044f: bit 0x200 = src X
   +8 px, 0x400 = dest X +8 px, 0x100 = src Y += `view+0x26`; each self-clears).
   `draw_anim_channels_a` sets 0x600 for ODD cells (posA X = grid*16+8) → odd cells'
   rightmost 8-px column was never erased; only the rightward dust frames (0x80-0x83)
   put pixels there.  Fixed with separate src/dest offsets (`host_render.c`).
3. **Pause strip + text rendering** (tasks 15/16): pause save/restore = direct VGA
   strip copy (engine buf DGROUP 0x9694, 40 B × 8 rows × 4 planes, page `table[0]`).
   Text leaves were MISNAMED stubs: 1000:9837 = SET TEXT POSITION(x,y) (DGROUP
   0x6942/4), 1000:9804 = DRAW STRING(str far ptr) → overlay 1ab9:13ec/13bc/1607.
   The FONT is **DDFNT2.CAR** (game resource 4, bound at DGROUP 0x68a2 by
   load_graphics_resources) — loaded at RUNTIME (`host_load_font`, never committed).
   `show_text_screen` renders the real "GAME OVER" (DGROUP 0x1327 via the 0x11ae far
   ptr).  Fg color = 0x0F white with a FIDELITY note (engine fg expansion is set at
   runtime through a BGI color op; not statically traceable).

## 2. RECOGNITION PATTERNS — likely causes of similar bugs still lurking

These are the recurring root-cause CLASSES this session kept finding.  When a new
symptom appears, check these in order:

1. **Split-brain globals** — one engine DGROUP address modeled as two (or more) C
   globals.  Found twice (0x791a; earlier 0x855e/0x8566).  RECOGNIZE: a counter/flag
   "never changes" or "is always 0" for one reader.  FIND MORE: grep two symbols
   claiming the same `DGROUP 0x....` in comments: `grep -oE "DGROUP 0x[0-9a-f]{4}" src/*.c | sort | uniq -d` then check each duplicate address has ONE owning definition.
2. **Wrong-width globals** — engine WORD modeled as u8 (or signed/unsigned mixups).
   Found once (p1_move_anim).  RECOGNIZE: value looks "truncated"/wraps; symptom only
   for large values.  VERIFY: the engine's store/load instructions (word vs byte
   MOV) in the disasm, not the Ghidra decomp types.
3. **Comment-only / deferred engine behavior on live paths** — a fidelity note says
   "documented here rather than reproduced".  Found once live (pending-erase);
   remaining known instances are inventoried in `docs/faithfulness-gap-audit.md`
   (STALE — task 19 is to refresh it) and in §4 below.
4. **Unrelocated link-time far pointers** — any extracted DGROUP data with seg 0x103b
   needs the DOS-loader fixup reproduced at init, applied exactly ONCE per pointer
   (shared headers caused a double-reloc bug before; dedup by unique offset).  Also
   check EXE RELOCATION TABLE entries inside any extracted range — words that the
   loader patches (found in spawn_p2_frame_tbl).  Tools: python over
   `local/build/unpack/BUMPY_unpacked.exe`, DGROUP file base 0x11440.
5. **Body↔symbol swaps / mis-attribution** — a reconstructed body attached to the
   wrong `FUN_seg_off` symbol.  VERIFY suspicious handlers by disassembling their
   address and comparing store-for-store; check the dispatch table bytes in the
   binary (game_mode_handlers @DGROUP 0x7ca = 2-byte NEAR offsets).
6. **"Obviously regular" data rebuilt instead of extracted** — the layer-A frame
   table looked linear but wasn't (88/197 permuted).  RULE: never synthesize a table
   the engine READS; extract it verbatim and byte-compare (assert in the generator).
7. **Descriptor semantics dropped** — BGI view descriptors carry flag words (+0x1c
   half-tile bits, +0x26 Y offset) and PAGE INDICES (word00/word0e → sprite table).
   Any leaf that consumes a descriptor must parse ALL fields the engine parser
   (1ab9:03c5..045b) consumes.  If a new erase/copy leaf misdraws by 8 px or one
   page, check the flags first.
8. **Oracle/emulator misreads** — the "no CRTC flip" conclusion came from an emulator
   returning 0xFF on port reads.  RULE: before trusting a captured port VALUE,
   check what the emulator returns for READS of that port (0xDF00 = 0xFF^0x20).
9. **Page-discipline bugs** (post-flip model) — anything drawing/saving/restoring VGA
   must resolve its page from the LIVE table (`host_draw_page_off`/`host_page_off_of`),
   never a cached or hardcoded page.  Grep for new `MK_FP(VGA_SEG_PAGE0` or raw
   `0x2000u` page math in host code.  Invariants: displayed == `page[table[0]]`
   (boot CRTC 0x2000); gameplay draws slot 1; UI screens bracket
   `fun_9410_set_sprite_table(0)…(1)`; mode-11 = sync copy `[word00]→[word0e]`.
10. **Harness drift** — reconstructing a function into src/ breaks ctest harnesses
    that still stub it (dup symbol) or reference globals it now needs.  After moving
    any function, run the full validator sweep and fix harness stubs (pattern: remove
    the dup stub; assert via a semantic side effect; add missing cross-module globals).
    Also: trace formats may store NARROWER fields than the engine state (compare
    through the trace's width, like `(u8)p1_move_anim`).
11. **Performance-shaped visual bugs** — per-byte port I/O in per-frame paths causes
    flicker/lag that LOOKS like a logic bug.  Full-page/large copies must use the VGA
    write-mode-1 latch copy (see `init_fullscreen_view_desc` in `view_setup.c`).

## 3. OPEN BUG (RESOLVED root cause; fix in two parts): late world-1 level = the P2 level

ROOT CAUSE (investigated + capture/memdump-verified 2026-07-03): the broken level is
**world-1 block 2 = overworld node 3 — the only world-1 level with a P2 opponent**
(the node path ends …→4→3, hence "one of the last levels").  The P2 move-state
script subsystem (engine DGROUP **[0x2352, 0x2548)**: 10-entry state table @0x2520 +
9 headers `{steps,facing,entries far ptr}` + entry arrays) was NEVER extracted —
`p2_state_script_tbl` stayed NULL and the whole P2 machine read scripts through the
IVT (memdump: `p2_move_script = 00cf:0915`), producing wild blits/erases that wiped
platforms and killed input.  Levels without a P2 never consume it (p2_cell==-1 gate).
All other suspects affirmatively ruled out (data scan clean; layer-B level block 3
works in capture).

FIX PART 1 — DONE: `src/move_scripts.c` `init_p2_state_scripts()` — blob extracted
verbatim (byte-verified 502/502), headers' entries far ptrs relocated once, runtime
table rebuilt, `p2_state_script_tbl` set.  Verified live: table/script pointers sane
in a node-3 memdump.  NOTE the engine overlap: the state-9 table entry's seg word at
DGROUP 0x2546 IS `spawn_p2_frame_tbl[0]` (comment in the code).

FIX PART 2 — DONE (2026-07-03): `p2_dispatch_move_state_handler` implemented
(`game_stubs.c`, playable branch).  KEY GROUNDING: both P2 handler tables (0x85c
AND 0x870) are **STATIC image data** — an opcode scan proved no writer exists; the
old "runtime-populated" claims were wrong.  0x870 entries: [1]=4dbf, [2]=4e44,
[3]=4ec9, [4]=4f4e (all already reconstructed in player2.c phase 4; each ends in
`p2_set_move_state(1..4)` = the re-arm), [5..9]=4dbf, [0]/[10]=1000:7111 (the
compiled EMPTY Borland fn — the filler for unused slots; the same 7111 fills the
0x85c non-move slots, retroactively validating view_setup's no-op seeding).
Live-verified on node 3: `p2_pixel=(263,143)`, state 3, coherent scene, P2 visible.
p2 74/74, physics 17405/17405, int8 150/150 all PASS.  → THE LATE-LEVEL BUG IS
FIXED end-to-end; user playtest pending.

HEADLESS VERIFY RECIPE (works; calibrate after every relink):
- Input script `local/build/op12-handoff/node3.txt` walks to node 3 (PASS
  REPO-RELATIVE PATHS to shotcap — it prefixes `$ROOT/`; an absolute path silently
  breaks key injection and the game sits at the text-mode F2/F5 screen forever).
- Memdump: run dosbox-x directly with `BUMPYCAP_MEMDUMP_OUT/OFF/LEN` (see §6 of the
  fidelity notes or the one-off command in the session transcript); the dump file is
  **DGROUP-RELATIVE** (file offset == DGROUP offset).  Symbol offsets from a wlink
  map that `cmp`s identical to BUMPYP.EXE (src/BP_cal.lnk pattern); runtime DGROUP =
  read the `DS=` field from a BUMPYCAP log line (load delta was 0x824 this build —
  it CHANGES with EXE header size; don't hardcode).
- Success = p2_pixel sane, p2_move_state 1..9, coherent level shot at ~frame 27000.

## 4. Remaining queued work (task list at handoff)

- **#18** Reconstruct `process_sprites` (pm=2 == the already-reconstructed
  `sprite_bank_load_transform`, 1cec:0c34 — mostly wiring + un-drain resource 0 in
  host_resource.c), `play_intro_animation_loop` (title-music loop; MUST keep the
  `copyprot_seed_src += 7` churn; sound leaves 8977/8999/89a8 are carve-outs),
  `wait_50_frames` (already live).
- **#19** Refresh `docs/faithfulness-gap-audit.md` — many §3/§4 entries are stale
  (done since written); fold in everything from this doc.  Do LAST.
- **#20** The late-level bug (§3).
- Known intentional gaps: layer-B restore drop in `anim_restore_bg_view_leaf`
  (documented follow-up); glyph fg color 0x0F assumption; boot CRTC value inferred
  (0x2000), never directly captured from the original.

## 5. Key architecture facts (hard-won; do not re-litigate without new evidence)

- Two builds from one tree: default = faithful decompilation (byte-compared, never
  run); playable = `#ifdef BUMPY_PLAYABLE` + `-dHOST_FB_16K`.  NEVER change default-
  build behavior silently.
- Video model: real VGA, mode 0x0D, two pages (a000:0000 / a200:0000 == a000:2000).
  Present = CRTC flip + table swap (§1.2.1).  Blitters write VGA via
  `host_vga_rmw4/put4/read4` (absolute a000-window offsets; caller folds the page).
- The self-modifying BGI overlay (seg 1ab9) does not decompile — recover semantics by
  disassembling the RUNTIME-RELOCATED bytes (e.g. from `local/build/render/
  op12_cpu_mem.bin` or the unpacked EXE at the overlay file offsets).
- Engine anim streams end in a DOUBLED final byte (`…32 32 ff`) — the device for
  drawing the rest frame once per page.  Don't "fix" it.
- `p1_advance_grid_history` keeps a 2-tick history BECAUSE of the 2-page flip (erase
  targets the same physical page as the save 2 ticks earlier).

## 6. How to build / validate / capture (exact recipes)

```sh
# Watcom env + build both targets
export WATCOM=$PWD/local/toolchain/open-watcom
export PATH="$WATCOM/binl64:$WATCOM/binl:$PATH"; export INCLUDE="$WATCOM/h"
( cd src && command wmake -h ../local/build/src/BUMPY.EXE && command wmake -h play )

# Validators (all must pass; use command/timeout, zsh gotchas in CLAUDE.md)
export UV_CACHE_DIR=$PWD/local/.uvcache   # ~/.cache is sandbox-blocked
for v in spawn bg anim items p2 player input physics int8 blit composite sprites \
         p1_spine screens screen_fns sound copyprot host_compose integration; do
  timeout 600 bash tools/validate_$v.sh >/tmp/claude/v_$v.log 2>&1 \
    && echo "$v PASS" || echo "$v FAIL"; done

# Headless capture (script = "frame scancode updown" lines)
bash local/build/op12-handoff/shotcap_2026-07-02.sh \
     local/build/op12-handoff/enter-level-sweep.txt 7000 /tmp/claude/out.png
# CAPTURE CALIBRATION SHIFTS ON EVERY RELINK: re-derive from the map —
#   wcl ... -fm=../local/build/src/BUMPYP.MAP ...  (or add -fm to the play rule)
#   runtime DGROUP = map "DGROUP" para + 0x82a;  offsets: _game_mode, _current_level
#   then edit BUMPYCAP_DGROUP / OFF_GAMEMODE / OFF_CURLEVEL in the shotcap script.
# NOTE: shots read PHYSICAL PAGE 0 — under the flip model that is the UI sync page
# (cursor-free BY DESIGN) or the gameplay back page; a complete frame either way.

# Ghidra ground truth: MCP tools (mcp__ghidra__*, needs the GUI+plugin running;
# a curl to 127.0.0.1:8080 fails under the sandbox even when it's up — just call the
# MCP tool).  Offline: local/decomp/*.c (per-function exports, named fn@1000_xxxx.c);
# binary: local/build/unpack/BUMPY_unpacked.exe (DGROUP offset N = file 0x11440+N).
```

## 7. Where the details live

- `docs/reconstruction-fidelity.md` — every deviation + the 2026-07-02 correction
  sections (review round, present flip, round 3, round 4/text).
- Memory notes (assistant memory): `bumpy-2026-07-02-review-findings`,
  `bumpy-2026-07-02-fixes-applied`, `bumpy-2026-07-02-round2-present-flip` (includes
  round 3) — same content as §1, with extra addresses.
- `docs/faithfulness-gap-audit.md` — the not-yet-reconstructed inventory (STALE; task #19).
- `docs/fidelity-cleanup-handoff.md` — the PREVIOUS session's handoff (all its items
  are now done; delete both handoff docs once the audit refresh lands).

---

## 8. NEW PLAYTEST BUGS (2026-07-03, reported after the P2 fix)

Written for continuation by a smaller model.  Investigate in this order; each has a
hypothesis + recipe.  Re-read §2 (recognition patterns) and §5 (settled facts) first.

> **CONTINUATION STATUS (2026-07-03, second pass — capture-verified):**
> - **§8.1 menu arrow — FIXED + CAPTURE-CONFIRMED.**  Two DOWN presses move the
>   arrow cleanly PLAY→LEVEL:EASY (item 0→2), fully visible, no hide/desync.
>   Root fix (from the prior pass): `init_crtc_window` is a NOP (it was inventing a
>   CRTC-start write that clobbered boot page parity — the real 1000:9821→1ab9:1422
>   is a clip-window STORE); boot parity moved to `host_crtc_set_start(0x2000)`; the
>   host cursor save-under (`hr_cursor_box_copy`) was deleted (engine has none — the
>   mode-11 page sync IS the erase).  Capture: menu-arrow-test.txt → menu_2600.png.
> - **§8.2 pause score — FIXED + CAPTURE-CONFIRMED.**  Pause overlay shows the
>   7-digit score in legible LIGHT-GRAY (fg=14) + the HUD icon row, NOT black
>   squares.  Root fix: the misnamed `set_palette_mode` (wrote palette_mode=14!)
>   was corrected to `set_text_color` (host_video.c:157) so the engine's own
>   `set_text_color(0x0e,0x01)` from init_game_session_state (game.c:181) reaches
>   the glyph blitter → fg=14/bg=1 instead of the default white (15 = near-black in
>   the level palette).  The DDFNT2.CAR glyph parse was ALSO verified correct
>   (dumped '0'/'1'/'5'/'9' render as clean digits; raw file, not compressed:
>   header `20 ff 07 08 01 00` = first/last/px_h/rowc/spacing).  Capture:
>   pause-cap.txt (dismiss intro-wait with a key AFTER the ~14s decode, THEN P at
>   ~11000) → pause_11500.png.
> - **§8.3 level 6/7 — FIXED (buffer sizes) + level-1 render confirmed clean.**
> - **§8.5 high scores — DIAGNOSED, NOT the text/page roots.**  See updated §8.5:
>   it is COUPLED to §8.4 (iris + highscore present path), a real multi-leaf host
>   integration bug, not low-hanging.
>
> Builds clean (both targets), 19/19 validators PASS after all the above.
> Capture harness calibration UNCHANGED this pass (rebuild produced an identical
> map): DGROUP para 0x4732 → runtime 0x4f5c, _game_mode 0x4e03, _current_level
> 0x0b66 — shotcap_2026-07-03b.sh is valid as-is.  New input scripts live in
> local/build/op12-handoff/{pause-cap,menu-arrow-test,highscore-cap}.txt.

### 8.1 Menu arrow: hides on input, appears on NEXT item; can desync and appear on the PREVIOUS item

STRONG HINT: "appears on previous item" = the DISPLAYED page carries the arrow drawn
one menu ITERATION ago → the cursor is being drawn on the page that becomes visible
one present LATER (draw/display parity inverted in the menu loop), or an extra/missing
present somewhere in the menu path flipped the parity (candidates: fun_7bca_flip /
play_iris_wipe_transition / show_title paths adding a flip our model doesn't expect;
or the engine's cursor draw happens BEFORE present in the iteration, not after).
INVESTIGATE: decompile run_main_menu (1000:35a5 region) and list the EXACT per-
iteration order of {strip compose, present, mode-11 sync, cursor blit, vretrace};
compare with src/screens.c run_main_menu.  Then hand-simulate the two physical pages
across 3 iterations under our boot parity (displayed==page[table[0]], see §1.3.1) and
find where the recon's arrow lands on the hidden page.  Also suspect the leftover
host cursor save/under (hr_cursor_box_copy, host_render.c ~:700) — the engine has NO
cursor save-under (the mode-11 sync is the erase); our box restore may now repaint a
STALE box over the freshly-synced page each frame.  Try simply DISABLING the box
save/restore in host_blit_cursor (keep the blit) and re-checking — that may be the
whole bug.  Verify: capture BOTH pages (page0 shot + the a200 window: BUMPYCAP raw
VGA dump env BUMPYCAP_RAWVGA_OUT exists) around a scripted menu-down keypress
(local/build/op12-handoff/menu-down.txt).

### 8.2 Pause overlay: BLACK SQUARES instead of score digits (original = medium gray)

Two independent defects to check in the host glyph blitter (host_render.c
host_text_draw_string + the DDFNT2.CAR parse, added round 4):
(a) SHAPE: full black rectangles = every row byte rendering as 0xFF or the glyph
    bitmap offset wrong (parsing the BE u16 glyph-offset table at font+6, glyph =
    {w_px, h_rows, y_skip, rows}).  Dump DDFNT2.CAR ('0'..'9' glyphs) with a tiny
    python script and eyeball the row bits vs what the blitter reads.
(b) COLOR: the round-4 note assumed fg = white 0x0F (planes = mask each).  The real
    game shows MEDIUM GRAY → the engine's fg expansion (DGROUP 0x68a6 words, set via
    the BGI color op 1ab9:14ef) selects a palette index ≠ 15.  Find the setcolor
    call before draw_number in show_pause_screen/the world map (decompile
    1000:49d7 + level_intro_screen callers; look for a small int pushed to a BGI
    color thunk) and use that index's plane expansion: plane p value = mask if
    (color>>p)&1 else 0.  "BLACK squares" = all four planes written 0 with full
    mask — i.e. the current code probably passes values 0 (check the rmw4 call:
    likely arguments (off, m,m,m,m, m) intended but wired as (off, 0,0,0,0, m)).
VERIFY: pause capture (P key = scancode 0x19 in an input script) → digits legible.

### 8.3 World-1 level 6 ≠ original; level 7 visually broken (screenshot local/build/play-captures/bumpyp_011.png)

HYPOTHESIS (high confidence): the node→level-block mapping is wrong.  The recon does
`current_level_index = current_entity_index - 1` (src/game.c ~:437) — i.e. node N →
block N-1.  But the ORIGINAL likely maps nodes to blocks through the worldmap MOVE
DESCRIPTOR (9-byte entries — one field may be the level id!) or another table.  The
overworld node PATH is nonlinear (1→2→9→8→{5,12→13→14→15→11→10→6→7→4→3}, see §3),
so "the 6th level you play" ≠ block 5.  INVESTIGATE: decompile the engine's
assignment of _current_level_index (grep local/decomp for 0x3b8b-equivalent writes;
in game_loop 1000:0c18 after level_intro_screen) and the 9-byte move-descriptor
layout (worldmap_data.c blob; the entry the cursor lands on).  Compare our
render_levels.py block N renders (results/levels_png/world1_lvlNN.png) with the
original game's levels — the user already confirmed block-index vs original mismatch
at "level 6"/"level 7".  Also check whether bumpyp_011.png's breakage is a BLOCK
with data our tables don't cover (re-run the §3 data-scan script for that block) —
"visually broken" may be a second, independent bug (e.g. a bg tile id ≥ atlas size).

STATUS (2026-07-03, root-cause investigation DONE — hypothesis REFUTED, real cause
found; NOT yet fixed):

- **The node→block mapping is FAITHFUL.**  The engine itself does
  `current_level_index = current_entity_index - 1` — verbatim in the decomp
  (`local/decomp/game_loop@1000_0c18.c:43`, right after level_intro_screen returns).
  The 9-byte move-descriptor holds NO level id: layout is `[0]=done flag,
  [1]/[2]=up target-node/step-distance, [3]/[4]=down, [5]/[6]=left, [7]/[8]=right`
  (see p1_move_step_{up,down,left,right}@1000_3ab2/3b0f/3b6c/3bc9 — each reads the
  target byte, sets `current_entity_index`, walks `desc[dir+1]>>2` 4-px steps).
  `src/worldmap_data.c` g_worldmap_blob byte-compares IDENTICAL to the EXE DGROUP
  [0x9e6,0x1114).  `results/levels_png/world1_lvlNN.png` = block NN-1 = node NN —
  the extraction renders ARE node-numbered exactly like the engine.  No doc renaming
  needed.
- **REAL ROOT CAUSE: truncated decode-copy buffers in `src/level.c`.**  The engine
  vec_decodes D<n>.DEC to **0x2f96** bytes and D<n>.BUM to **0xb60** bytes
  (start_level@1000_2d14 `dec_len/bum_len`; DGROUP-initialized words @0x00a0=0x2f96,
  @0x00e6=0x0b60 in BUMPY_unpacked.exe; 2 + 15*0x32c and 2 + 15*0xc2).  The recon
  decodes correctly into g_op12_arena but then copies out only
  `LEVEL_DEC_BUF_SIZE=0x2000` and `LEVEL_BUM_BUF_SIZE=0x400` (src/level.h:39-40,
  used at the `level_copy_arena` calls in start_level, src/level.c ~:970-980), and
  `level_src_ptr = g_bum_buf+2+idx*0xc2` / `cur_level_ptr = g_dec_buf+2+idx*0x32c`
  read past the allocations for later blocks.  Per-block damage:
  BUM blocks 0-4 (nodes 1-5) FULL; **block 5 (node 6) PARTIAL — cut at +0x34, so
  layer A + 4 cells of layer B valid, rest of layer B/layer C/spawn/exit/items/P2 =
  heap garbage → "similar but not the same"**; **blocks 6-14 (nodes 7-15) read 100%
  garbage → wild entity/item/spawn/P2 data → "visually broken"**.  DEC blocks 0-9
  (nodes 1-10) FULL; block 10 (node 11) partial; blocks 11-14 (nodes 12-15) garbage
  (background grid + per-level palette) — a second, not-yet-reported breakage tier.
- **bumpyp_011.png identified**: it IS node 7 = block 6 — its DEC background is
  intact (block 6 DEC is inside 0x2000; bg-only structural correlation ranks block 6
  #1 of 15 vs the capture) with garbage sprite blits sprayed over it from the
  all-garbage BUM layers.  Not a tile-id/atlas or anim-table range bug.
- **FAITHFUL FIX (small)**: `LEVEL_DEC_BUF_SIZE 0x2000 → 0x2f96` and
  `LEVEL_BUM_BUF_SIZE 0x400 → 0x0b60` (the engine's own dec_len/bum_len), i.e. +5.9 KB
  far heap.  op12 window chaining across PAV→DEC→BUM is already modeled (static
  `g_win` persists across level_decode_file calls, matching the engine's DG:0x4e97
  window; D1.{PAV,DEC,BUM} arena decode previously verified byte-exact), so fixing
  the two copy sizes is sufficient.  After fixing, playtest nodes 6, 7 AND 12-15
  (blocks 11-14 exercise the DEC tail + the BUM wrap region).
- CONFIDENCE: high (decomp-grounded mapping proof + byte-verified data + truncation
  boundaries matching the two reported symptoms exactly + capture bg correlation).

**FIX APPLIED (2026-07-03)**: `src/level.h` — `LEVEL_DEC_BUF_SIZE 0x2000 → 0x2f96`,
`LEVEL_BUM_BUF_SIZE 0x400 → 0x0b60` (the ENGINE's dec_len/bum_len).  Both builds
clean; bg/items/spawn/int8 validators pass.  User playtest of nodes 6, 7, 11-15
pending.  §8.3 is otherwise CLOSED (mapping + worldmap data affirmatively faithful).

### 8.4 Screen-transition iris ("shrinking square fade") never worked

play_iris_wipe_transition (engine 1000:3467) → the host_gfx viewport/iris path
(src/host/host_gfx.c, commit 9bf5946).  Never functional in the playable build.
Related to task #18's presentation leaves.  INVESTIGATE: decompile 1000:3467 —
likely a loop shrinking a clip viewport + fill; check what host_gfx_set_viewport
does and whether the fill leaf is a NOP.  LOW RISK to gameplay; cosmetic.

### 8.5 High scores "broken"

**RESOLVED (2026-07-04): NOT a game bug — a CAPTURE-TOOL artifact.**  The highscore
screen renders CORRECTLY (the "HALL OF FAME" background + table on physical page 1).
The apparent breakage (a frozen, ghosted TITRE menu) was the BUMPYSHOT capture tool
mis-reading the wrong VGA page.

Root cause of the artifact: the dosbox BUMPYSHOT hook computed the displayed-page byte
offset as `poff = (st << 1) & 0x3FFF` — a WORD-addressed formula that yields poff=0 for
BOTH start=0x0000 and start=0x2000, so it ALWAYS dumped physical page 0.  But mode 0x0D
CRTC start is BYTE-addressed (start value == plane byte offset; page1 = start 0x2000) —
the exact correction already made in the ENGINE code (src/host/host_video.c
CRTC_PAGE1_ADDR, fixed 2026-06-27) but NOT in the capture tool.  The bug was invisible
until now because (a) every prior shot happened at start=0x0000 (poff correct either
way) and (b) gameplay double-buffers similar frames on both pages, so reading page 0
still looked right.  The highscore is the first screen that ends displayed on page 1
(start=0x2000) with DRASTICALLY different page-0 content (stale TITRE) vs page-1
content (HALL OF FAME) — so the tool bug finally showed.

FIX (capture harness, not the game): `poff = st & 0x3FFF` in
tools/dosbox/patches/01-bumpycap-hook.patch (and the same fix in
03-framebuffer-capture.patch for consistency; that patch is currently inactive — it
fails to apply on an unrelated debug.cpp hunk).  dosbox-x rebuilt.  After the fix, a
shot of the highscore at start=0x2000 reads page 1 = HALL OF FAME (verified:
`shot == rawvga page1`, and the PNG shows the correct screen).

PROOF CHAIN (all verified this session): open_resource(3)→SCORE.VEC opened (fd=5),
read_chunked read 32099 B, vec_decode returned 0 (success) → HALL OF FAME in
fullscreen_buf; hr_compose_screen_vga blits it via the LIVE page table
(host_page_off_of); page invariant `displayed==page[table[0]]` holds at the freeze
(memdump: table[0]=a200=page1, CRTC=0x2000); render_highscore_table drew the table to
the displayed page then wait_keypress (the "freeze" is the normal keypress wait).
The whole present chain is faithful and works.

**CAUTION for future capture work:** any prior conclusion drawn from a shot taken at
CRTC start=0x2000 (page 1 displayed) was reading page 0 and may be wrong.  Re-check
with the fixed tool.  Menu-arrow (§8.1) and pause-score (§8.2) were shot at
start=0x0000, so those verifications stand.

Minor follow-up (not the reported bug): the HALL OF FAME table shows the dot-grid
placeholder rows without visible default name/score entries — likely correct for a
fresh no-saved-scores game (score 0 does not qualify), but worth a glance vs the
original if the user wants default entries populated.

---
(historical diagnosis below — superseded by the RESOLVED finding above)

STATUS (2026-07-03, DIAGNOSED via capture; NOT the text/page roots):
selecting HIGH-SCORE (menu item 1 → `show_highscore_screen`, game.c:428) leaves a
FROZEN, ghosted menu under a color-wash — the highscore background + table never
appear.  Capture proof: framebuffer is BYTE-IDENTICAL at frames 3200 and 5200
(18219/32000 nonzero both) — nothing is redrawing; the score table is absent and
the previous menu bleeds through a faded palette.  (highscore-cap.txt →
hs_5200.png.)

This is NOT the §8.2 text-color / §8.1 page-parity bug (both now fixed).
`show_highscore_screen` (screens.c:1710) is fully reconstructed and does:
`open_resource(3,4)` + read_chunked + vec_decode (highscore BG image) →
`play_iris_wipe_transition` → `restore_bg_view` (the real BG blit, gfx_overlay.c:125)
→ `fun_7b93_present_blank` (stage image palette) → `fun_7bca_flip` (DAC upload) →
`present_frame(1)` → `render_highscore_table`.  The failure is in THIS chain, and it
is COUPLED TO §8.4: the iris (`play_iris_wipe_transition`, screens.c:697) uploads a
BLANK/faded palette via fun_7b93/fun_7bca (its per-step geometric wipe is a
documented host deviation — `fun_7b4a_view_blit` only sets a viewport, no pixel
blit), and the screen never gets its REAL palette back → the persistent color-wash.
SUSPECTS to check IN ORDER (all host-integration, like the menu/level present work):
  1. **Palette** — after the iris fade, is the highscore BG image's embedded palette
     (img+0x33, staged by fun_7b93 then DAC-uploaded by fun_7bca) actually the
     highscore palette, or still the blank/faded one?  A wrong/blank palette alone
     explains the ghost.  Verify host_gfx_stage_image_palette reads +0x33 of the
     DECODED resource-3 image, and that fun_7bca uploads THAT page's palette.
  2. **Resource load** — does `open_resource(3,4)` + vec_decode actually populate
     fullscreen_img_buf on the host?  If resource 3 fails/decodes empty,
     restore_bg_view blits nothing → menu stays.  (Add a one-shot host log of the
     decoded length + first bytes.)
  3. **Page discipline** — restore_bg_view / present_frame / render_highscore_table
     must all resolve the LIVE page (§2 pattern 9).  The frozen byte-identical
     capture suggests present flipped to a page the shot doesn't read, OR the table
     is drawn to the hidden page.  Hand-simulate the pages as in §8.1.
Do §8.4 (iris) and §8.5 TOGETHER — same present/palette leaves.  These are real
multi-leaf integration work, NOT quick host tweaks.  validate_screens/screen_fns
pass (they gate the PORTED logic, not the host present/palette), so the defect is
host-only, consistent with the above.

### Sequencing note

8.1 and 8.2 are quick, isolated host fixes.  8.3 needs engine grounding
(the node→block mapping) and may invalidate "level N" naming across docs — do it
carefully and byte-verify against the original with render_levels.py.  8.4/8.5 last.
