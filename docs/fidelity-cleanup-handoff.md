# Fidelity-cleanup handoff (TEMPORARY working doc — not a permanent reference)

> Scratch/handoff for a **parallel effort**. Captures places where the `src/`
> reconstruction *invents* behavior instead of reproducing the binary, plus the
> conventional-memory cliff blocking the in-level fidelity work. Delete once landed.
>
> Governing rule (CLAUDE.md → "Adhere to the binary — never invent"): ground every
> change in the asm/decomp; reuse the original's buffers/offsets/tables/control flow;
> where 1:1 is genuinely impossible, surface + document — don't swap in an invention.
>
> **VERIFY, don't trust this doc.** Re-decompile / re-grep before editing; this was
> written from a long session and the line numbers drift. "Don't guess."

---

## Priority 1 — `level_populate_dg` invents posA/B/C instead of reading the tables  (LOW risk)

**Where:** `src/level.c` `level_populate_dg()` (~`level.c:391–461`). The comment is
explicit: *"Here we compute them analytically from the grid geometry, which matches."*

**What the binary does:** the engine **reads** per-cell coordinate tables from DGROUP:
- posA @ `0xf4`   (layer-A draw pos, `anim_posA_tbl`)
- posB @ `0x3f4`  (layer-B draw pos, `anim_posB_tbl`)
- posC @ `0x274`  (layer-C / P2 pos, `p2_cell_coord_tbl`; also read by `p1_set_pixel_from_cell` @ `g_entity_dg+0x274`)

**What the recon does:** computes them with formulas and stores into `g_entity_dg`:
- posA: `x = col*40`,    `y = row*32 + 24`
- posC: `x = col*40 + 8`, `y = row*32 + 8`
- posB: `x = col*40 + 32`, `y = row*32`

**Why it's wrong (per the rule):** invents data the engine reads; and it now
**duplicates** that data — the real tables were extracted verbatim this session into
`src/anim_data.c` (`g_anim_posA` @0xf4, `g_anim_posB` @0x3f4, `g_posC` @0x274), and
`init_anim_data()` already points `anim_posA_tbl`/`anim_posB_tbl`/`p2_cell_coord_tbl`
at them. (These pos tables are NOT gated by `EMIT_TILEDEF`; they're always emitted.)

**Fix:** in `level_populate_dg`, copy the real extracted tables into `g_entity_dg`
(`g_entity_dg[0xf4 + cell*4] = anim_posA_tbl[cell*4]`, etc.) instead of computing.
Removes the invention and unifies the (currently two) copies of posC.

**VERIFY FIRST:** confirm the formulas actually equal the extracted tables for all 48
cells. They matched on spot-checks (posA/posC), but the engine reads the tables
*regardless* — if any cell differs, reading the table is the correct (and bug-fixing)
behavior. Do a byte-compare of formula-output vs `g_anim_posA/posB/g_posC` to document.

**Cost:** none (data already resident). No new memory.

---

## Priority 2 — `move_scripts.c` double-relocates shared move-script headers  ✅ FIXED 2026-07-01

**Status:** FIXED (`init_move_scripts`, `#ifdef BUMPY_PLAYABLE`). Root cause of the 2026-07-01
playtest **trails + wrong-frame (platform-instead-of-player) + erratic keys** on the in-level
walk: `game_mode 4` (the main in-level walk mode) shares header `0x14e4` with mode 45, so its
`entries` far-ptr was **double-relocated** — at the runtime blob base `0x4610` the pointer landed
`+0x3294` bytes out of range (`0x14b4`→correct `0x4748`, but buggy `0x79dc`). `p1_step_scripted_move`
then read `p1_move_anim = script[0]` (garbage → a platform frame at Bumpy's position), `dx = script[1]`
and `dy = script[2]` (garbage → erratic jumps, and the erratic motion smeared save-under trails).
`0x1756` (modes 14/48/49) was *triple*-relocated. The bug is invisible in the original binary because
its DOS-loader relocation is single **and** `base == MS_BASE (0x137c)` there (double==single); the host
build's runtime `base ≠ MS_BASE` exposes it.

**Fix applied:** relocate each header's `entries` far-ptr field **exactly once** — skip the in-place
fixup when an earlier mode already relocated the same `hoff` (scan `s_hdr_off[0..m)`), then still fill
`mode_script_tbl[m]` for every referencing mode. This mirrors the DOS EXE loader (each pointer field
relocated once). Verified numerically (single vs double reloc) and build-clean; default `BUMPY.EXE`
md5 unchanged (`c8c0a3f5…`). NB: confirms the save-under **geometry is faithful** — Bumpy's frames are
≤32 px wide / ≤21 px tall and the 64×32 px save rect covers them at every sub-cell phase; the trails
were a *symptom* of corrupt motion, not a save-under coverage gap.

**Where:** `src/move_scripts.c` `init_move_scripts()` (~`move_scripts.c:287–320`).

**Bug:** the loop relocates each mode's internal `entries` far-ptr in place:
`g_move_script_blob[hidx+2] = base + (eoff - MS_BASE)`. But `s_hdr_off` has **shared**
header offsets — `0x1756` at modes 14/48/49, `0x14e4` at modes 4/45. Each shared header
is relocated 2–3×; the 2nd/3rd pass reads the already-relocated value and double-adds
`base` → corrupt `entries` ptr for those modes' move scripts.

**Why masked:** those modes likely aren't exercised on level 1, so it hasn't surfaced.
Still a latent correctness bug (movement glitch for whatever game_mode maps to 4/14/45/48/49).

**Fix:** dedup the internal-ptr relocation — collect the unique non-zero `hoff` values,
relocate each header's `entries` ptr **once**, then fill `mode_script_tbl[m]` for all
modes (multiple modes may legitimately point at the same relocated header). This is the
same dedup used in the tiledef extraction (`gen_anim_data.py` `g_tiledef_a_uniq`).

**VERIFY FIRST:** re-read `s_hdr_off` for the current duplicate set (it changes if the
blob is re-dumped). Confirm the engine genuinely shares headers across modes (it does in
the static image — the duplicate offsets are byte-identical far-ptrs).

**Cost:** none.

---

## Priority 3 — `g_entity_dg` 41 KB shadow is the conventional-memory cliff  (HIGHER risk, strategic unlock)

**Where:** `src/level.c` — `#define LEVEL_DG_SIZE 0xa200` (`level.h:60`), allocated in
`start_level` via `LEVEL_FAR_ALLOC(LEVEL_DG_SIZE)` (`level.c:258`).

**Problem:** `g_entity_dg` is a **41 KB** image that mirrors the engine's entire DGROUP
address space `[0, 0xa200)`, but only holds a handful of fields placed at their engine
offsets:
- posA `0xf4` (192 B), posC `0x274` (192 B), posB `0x3f4` (192 B) → used `[0xf4, 0x4b4)`
- P1 sprite obj `DG_P1_OBJ = 0x792e`, P2 sprite obj `DG_P2_OBJ = 0x795a`, plus
  `DG_P2_CELL` / `DG_P2_FRAME_BASE` (near the objects) → used `[0x792e, ~0x7980)`
- The gap `[0x4b4, 0x792e)` ≈ **29 KB is dead** — allocated only to place the objects at
  the engine's fixed offset (e.g. `hr_blit_obj` reads `hr_dg+0x792e`; `init_sprite_structs`
  sets `p1_sprite = g_entity_dg + 0x792e`).

**Why it matters:** this is the documented cause of the conventional-memory cliff
(`docs/reconstruction-fidelity.md` → "conventional-memory OOM"). It's why a +2.2 KB
program growth (the tiledef fix) broke the level, and it's what blocks the faithful anim
save-under (Priority B below, also ~2.2 KB). Reclaiming ~29 KB here unblocks BOTH.

**It is itself a deviation:** the engine has no DG "shadow"; those fields are just *in*
its DGROUP. The host built the image to satisfy fixed-offset reads. Tightening it is both
a memory win and a fidelity-shape improvement.

**Fix options (pick after measuring all access sites):**
1. **Split allocation** — two small buffers: low `[0xf4, 0x4b4)` (pos tables) and a small
   one for the objects `[0x792e, 0x7980)`; keep the high one at a base that still presents
   `+0x792e`/`+0x795a` to the readers (e.g. allocate at `obj_base - 0x792e` so the same
   pointer arithmetic lands in the small block). Frees the 29 KB middle gap.
2. **Rebase the object offsets** — move the sprite objs to right after the pos tables and
   update every reader (`hr_blit_obj` `+0x792e`, `init_sprite_structs`, `level_get_entity_dg`
   consumers, `entity.c`). More invasive; touches more sites.

**MUST DO FIRST (do not guess):** `grep -rn` for every `g_entity_dg` / `hr_dg` / `0x792e`
/ `0x795a` / `level_get_entity_dg` access across `src/` + `src/host/` and enumerate the
COMPLETE set of used offsets before changing the layout. Option 1 is lower-risk.

**Verify after:** the level still loads + renders (this was exactly the failure mode of
the OOM regression — overworld renders but the level goes black / `vganz=0`). Headless:
`bash local/build/op12-handoff/shotcap_long.sh ... 7100 out.png` then `brender.py`; the
level histogram should be index-1 (dark-blue bg) ≈ 31 k, NOT the overworld (`vganz=23000`).

---

## Blocked-on-Priority-3 work (the actual goals)

These are correct/extracted already but can't land until P3 frees headroom (~2.2 KB each):

### B. Faithful anim-channel save-under (issue B) — the in-level sprite trails / under-erase
**What the binary does:** gameplay sprite-erase runs through `render_player_view`
(1000:93b8) / `restore_bg_view` (1000:80bc) → the **self-modifying BGI overlay
(1ab9:0db0) that does not decompile**. Behaviorally it's a per-sprite **save-under**:
save the clean bg under each anim sprite before blit, restore next frame. Per-channel
DGROUP buffers (from the asm / `anim.c` view setup):
- layer A draw view: `0x79be + channel*0x180` (3 ch) → `[0x79be, 0x7e3e)` = 1152 B
- layer B draw view: `0x7e3e + channel*0x100` (4 ch) → `[0x7e3e, 0x823e)` = 1024 B
- contiguous total ≈ **2176 B** of per-channel save buffers.

**What the host has:** the SAME mechanism is already reimplemented for P1/P2 —
`hr_save_under` / `hr_restore_under` + `s_p1_saveunder`/`s_p2_saveunder`
(`src/host/host_render.c`). The anim leaves `anim_render_view_leaf` /
`anim_restore_bg_view_leaf` (`host_render.c:392–393`) are explicit NOPs ("channels next").

**Faithful fix (once memory exists):** add the per-channel anim save buffers mirroring the
engine's `0x79be`/`0x7e3e` layout, extract the channel index from the view (`view+0x10` =
`channel*0x100+0x7e3e` for B, `channel*0x180+0x79be` for A), and route the two leaves
through `hr_save_under`/`hr_restore_under` (save cell @ `view+0x06/0x08`, restore cell @
`view+0x14/0x16`, extent @ `view+0x1e/0x20`). Do NOT invent a smaller/shared scheme — the
binary uses per-channel buffers; reproduce that.

### C. Layer-A tile-def animation table (active-platform frames)
Extraction is DONE and correct but gated off (`gen_anim_data.py` `EMIT_TILEDEF = False`)
because its ~2.2 KB tipped the OOM cliff. Re-enable once P3 frees headroom; regenerate
`anim_data.c`; rebuild; verify the level still loads. See
`docs/reconstruction-fidelity.md` → "layer-A tile-def animation table".

### D. `contact_tiledef_tbl` (layer-B contact path) — same latent bug as C
`player.c:2444` (`contact_tiledef_tbl` @0x3256) is statically initialised with the
engine's link-seg-`0x103b` far ptrs but is NOT relocated in the playable build, and its
tile-def blob (`0x306a+`) is not dumped → garbage stream on tile contacts. Same
blob+dedup-reloc pattern as C. Also memory-gated by P3.

---

## Justified deviations — DOCUMENT, do not "fix" to 1:1 (the rule's escape hatch)

Already labelled; listed so the parallel effort doesn't mistake them for inventions:
- Host save-under standing in for the un-decompilable BGI overlay (`host_render.c`).
- 64 KB framebuffer / single a000-image model + `HOST_FB_16K` (vs engine a000/a200 + 256 KB).
- The two planar blitters (`sprite_blit`, `bg_render`) as behavior-faithful semantic
  reconstructions of self-modifying overlay code.
- INT8/PIT pacing host platform layer (`host_timer.c`) — note the FRAME pace itself was
  corrected this session to the engine's vblank wait; only the ISR plumbing is host-side.

## Already corrected this session (reference for the pattern)
- `run_n_frames` (`game.c`): was spinning on 500 Hz `host_tick` (invented) → restored to
  the engine's `wait_vretrace_thunk()` vblank pace (1000:05e7, 1:1). Fixed "too fast".
- `init_round_state` / `reset_round_counters`: was a no-op stub → reconstructed 1:1 from
  the disasm (1000:31de) in `player.c` (fixed player spawn position + entry move state).
