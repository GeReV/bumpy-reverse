# Data Files & Resource Pipeline

Bumpy's Arcade Fantasy (Loriciel, 1992) manages all game assets through
fixed-size descriptor records stored in DGROUP. French naming conventions
apply throughout: `monde` = world, `grille` = grid, `fleche` = arrow.

---

## Resource descriptor tables

Assets are described by tables of fixed-size records in DGROUP (segment
`0x103b` at runtime / Ghidra segment `203b`). The base record layout is:

```
struct vec_res {          // 10 bytes
  uint16 name_off;        // offset of filename string within DGROUP
  uint16 name_seg;        // always 0x103b (DGROUP)
  uint16 type;            // 0x61 = .VEC graphics stream, 0x62 = sound/music bank
  uint32 size;            // resource size / load-buffer capacity in bytes
};
```

Two tables are defined:

**`vec_resource_table` @ `203b:0932`** — array of `vec_res` (10-byte records),
one entry per shared asset, in load order:

| Slot | File |
|------|------|
| 0 | `MASKBUMP.VEC` |
| 1 | `BUMPRESE.VEC` |
| 2 | `SCORE.VEC` |
| 3 | `BUMPY.BNK` |
| 4 | `BUMPY.MID` |
| 5–6 | *(empty, type `0x7a`)* |
| 7–15 | `MONDE1.VEC` … `MONDE9.VEC` |
| 16 | `DESSFIN.VEC` |
| 17 | `TITRE.VEC` |

**`grille_resource_table` @ `203b:00ae`** — array of **20-byte** records (two
10-byte `vec_res` sub-resources per entry) covering the grid/object assets, in
order: `GRILLE.VEC`, `BUMPYOBJ.VEC`, `GRILLOBJ.VEC`, `FLECHE.BIN`, …

The filenames are not reached by pointer from code; they are addressed
exclusively through the `name_off`/`name_seg` fields of these descriptor
records.

---

## Asset inventory

| File(s) | Type | Purpose |
|---------|------|---------|
| `GRILLE.VEC` | `.VEC` | play-field grid graphics |
| `BUMPYOBJ.VEC` | `.VEC` | Bumpy sprite/object set |
| `GRILLOBJ.VEC` | `.VEC` | grid object set |
| `MASKBUMP.VEC` | `.VEC` | EASY / MEDIUM / HARD difficulty-select UI (partial-screen overlay) |
| `BUMPRESE.VEC` | `.VEC` | "Bumpy's Arcade Fantasy" presentation/splash (Bumpy + logo + LORICIEL © 1992) |
| `SCORE.VEC` | `.VEC` | score / HUD glyphs |
| `MONDE1.VEC` … `MONDE9.VEC` | `.VEC` | world graphics for each of the 9 worlds |
| `TITRE.VEC` | `.VEC` | title screen |
| `DESSFIN.VEC` | `.VEC` | ending screen |
| `FLECHE.BIN` | `.BIN` | 16×16 world-map level-select cursor arrow |
| `BUMPY.BNK` | bank | AdLib/OPL2 instrument bank |
| `BUMPY.MID` | music | MIDI song data |
| `<n>.PAV`, `<n>.DEC`, `<n>.BUM` | per-level | level tile brush atlas, layout stream, and metadata (n = level digit) |

All `.VEC` files are interpreted command streams, not raw bitmaps — see
[`formats/VEC.md`](formats/VEC.md). `.BIN` format details are in
[`formats/BIN.md`](formats/BIN.md). Per-level `.PAV`/`.DEC`/`.BUM` formats
are documented in [`formats/README.md`](formats/README.md) and (for `.PAV`)
[`formats/CAR.md`](formats/CAR.md).

---

## Per-level filename templating

Level data is loaded by patching the level digit into the filename buffers at
runtime. The level-start routine writes `current_level + '0'` into the
filename buffers before opening any file, so level N loads `N.PAV`, `N.DEC`,
and `N.BUM`. The three per-level files cover the tile/brush atlas (`.PAV`),
the layout command stream (`.DEC`), and the Bumpy-placement/metadata stream
(`.BUM`).

---

## Resource load pipeline

The load path is a thin stack over the Turbo C runtime and DOS INT 21h. The
outline below covers the critical stages; `.VEC` command-stream interpretation
is described in [`formats/VEC.md`](formats/VEC.md).

### 1. Resource opener

The resource opener walks a `vec_res` descriptor record to retrieve the
filename (via `name_off`/`name_seg` into DGROUP) and the `disk_id` byte at
record offset +4. The `disk_id` is `'a'` or `'b'` for floppy disk selection,
or `'z'` to mark an absent/skip slot. When the wrong disk is present the
opener sets `QUELDISK`, calls the "INSERT THE OTHER DISK" prompt function, and
retries up to approximately 10 times before giving up. It then opens the file
via the C-runtime `open()`, which calls through to DOS INT 21h/3Dh and
populates the file-descriptor table at DGROUP `0x6b1c + fd*2`.

### 2. Chunked far-transfer read

A chunked read primitive (`read_chunked`) transfers data into a far buffer in
chunks of at most 64 000 bytes. After each chunk it advances the far buffer
pointer with segment carry, so reads larger than 64 KB are handled correctly
under the 16-bit DOS segmented memory model. The underlying read issues DOS
INT 21h/3Fh.

### 3. Close

After the data is in the buffer, `c_close` closes the file handle via the
C-runtime close path.

### Typical level-load sequence

```
h = open_resource(i, mode);
read_chunked(h, buf_off, buf_seg, n_lo, n_hi);
c_close(h);
vec_decode(buf, size, …);   // for .VEC assets
```

`.VEC` files (and the per-level `.PAV`/`.DEC`/`.BUM` streams) are passed to
the engine's vector interpreter after loading. See [`formats/VEC.md`](formats/VEC.md)
for the command encoding: big-endian 16-bit word stream, words `< 0x10` are
opcodes (followed by 3 operand words), larger values are (x, y) coordinate
pairs, with a dispatch table at DGROUP `0x4e37` into overlay segment `1c28`.
