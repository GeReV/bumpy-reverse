# 04 — Data files & resource system

First pass over the asset/resource architecture of `BUMPY_unpacked.exe`, via the
live Ghidra project (`BumpyDecomp`). Bumpy is a Loriciel title, so names are
French (`monde` = world, `grille` = grid, `fleche` = arrow).

## Resource-descriptor tables

Assets are managed through tables of fixed-size **descriptor records** in DGROUP
(segment `0x103b` at runtime / Ghidra segment `203b`). Each base record is 10
bytes:

```
struct vec_res {          // 10 bytes
  uint16 name_off;        // offset of filename string (in DGROUP)
  uint16 name_seg;        // = 0x103b (DGROUP)
  uint16 type;            // 0x61 = .VEC graphics, 0x62 = sound bank/music
  uint32 size;            // resource size / load capacity in bytes
};
```

- `vec_resource_table` @ `203b:0932` — array of `vec_res`, one per shared asset,
  in load order: `MASKBUMP.VEC`, `BUMPRESE.VEC`, `SCORE.VEC`, `BUMPY.BNK`,
  `BUMPY.MID`, (2 empty slots, type `0x7a`), then `MONDE1.VEC` … `MONDE9.VEC`,
  `DESSFIN.VEC`, `TITRE.VEC`.
- `grille_resource_table` @ `203b:00ae` — array of **20-byte** records (two
  10-byte sub-resources each) for the grid/object set: `GRILLE.VEC`,
  `BUMPYOBJ.VEC`, `GRILLOBJ.VEC`, `FLECHE.BIN`, …

(`get_xrefs_to` finds no references to the filename strings — 16-bit data refs
aren't auto-created by analysis. The filenames are reached *only* through these
descriptor records, found by scanning DGROUP for the string offsets as words.)

## Asset inventory

| File(s) | Type | Purpose |
|---|---|---|
| `GRILLE.VEC` | .VEC | the play-field grid |
| `BUMPYOBJ.VEC`, `GRILLOBJ.VEC` | .VEC | object/sprite sets |
| `MASKBUMP.VEC` | .VEC | **EASY/MEDIUM/HARD difficulty-select UI** (partial-screen overlay) → `results/images/maskbump.png` |
| `BUMPRESE.VEC` | .VEC | **"Bumpy's Arcade Fantasy" presentation/splash** (Bumpy + logo + LORICIEL © 1992) → `results/images/bumprese.png` |
| `SCORE.VEC` | .VEC | score/HUD glyphs |
| `MONDE1.VEC` … `MONDE9.VEC` | .VEC | the 9 worlds' graphics |
| `TITRE.VEC` | .VEC | title screen |
| `DESSFIN.VEC` | .VEC | ending screen |
| `FLECHE.BIN` | .BIN | 16×16 world-map level-select cursor arrow → `results/sprites/fleche_arrow.png` |
| `BUMPY.BNK` | bank | AdLib/OPL instrument bank |
| `BUMPY.MID` | music | song data |
| `<n>.PAV`, `<n>.DEC`, `<n>.BUM` | per-level | level data (n = level digit) |

The per-level files use a **filename template** with the level digit patched in
at runtime: `start_level` (`FUN_1000_2d14`) writes `current_level + '0'` into the
filename buffers before loading. So level N loads `N.PAV`, `N.DEC`, `N.BUM`.
(`.PAV`/`.DEC`/`.BUM` internal formats are still to be reversed — likely the
tilemap / object placement / level metadata.)

## Code structure notes

- The binary is segmented Turbo C++ (1990) with **overlays**: a thunk table
  around `1000:93xx` is full of `lcall <seg>:<off>` trampolines into the library
  overlay segments `0x0ab9` (Ghidra `1ab9`), `0x0cec` (`1cec`), `0x0ce5`
  (`1ce5`). Watch for these — a raw word matching an asset offset can be a
  coincidental far-call offset, not a pointer (e.g. `lcall 0ab9:126e`).
- `FUN_1000_745e` is the data loader/decoder invoked by `start_level` (and the
  copy-protect screen) to pull a resource into a buffer; the `.VEC` decode lives
  behind it. Next target.

## Next targets

1. The file-open/load primitive that walks a `vec_res` record (open `name`,
   read `size` bytes) — anchor for all asset loading.
2. `FUN_1000_745e` `.VEC` decode → understand the sprite/vector format.
3. `main` / the main game loop (entry → mode state machine: title, play, editor).
4. `.PAV`/`.DEC`/`.BUM` per-level format.
