# 05 — Resource load pipeline & the .VEC interpreter

Traced in the live `BumpyDecomp` Ghidra project (all names below are now applied
there). Builds on the descriptor tables in `04-data-files.md`.

## File I/O stack (Turbo C runtime + game wrapper)

```
open_resource(idx, mode)        FUN_1000_736f  — game resource opener
  walks descriptor record  DAT_203b_a1b4 + idx*10
  record = { uint16 name_off; uint16 name_seg(=0x103b); char disk_id; ...; uint32 size }
  record[+4] = disk_id char ('a'/'b' = which floppy, 'z' = absent/skip)
  on wrong disk: sets QUELDISK, calls (*DAT_203b_a1b8)(disk_id) = "INSERT THE
    OTHER DISK" prompt, retries up to ~10 times
  opens via c_open(name_off, name_seg, mode|0x8004, 0x180) -> file handle

c_open(off, seg, oflag, pmode)  FUN_1000_a21c  — C runtime open(); builds the fd
  table at DGROUP 0x6b1c + fd*2; DOS INT 21h/3D open is its callee FUN_1000_a360

read_chunked(h, buf_off, buf_seg, n_lo, n_hi)  FUN_1000_745e
  -> xfer_chunked(_read @ 0xa3ae, h, buf_off, buf_seg, n_lo, n_hi)  FUN_1000_7235
  generic far transfer loop: calls the per-chunk fn repeatedly, ≤64000 B/chunk,
  accumulates a 32-bit byte count, advances the far buffer with segment carry
  (handles >64 KB reads under 16-bit DOS). _read (1000:a3ae) = INT 21h/3F.

c_close(h)                      FUN_1000_7319 -> FUN_1000_988e   — close
```

A typical load (from `start_level`): `h = open_resource(i, mode);
read_chunked(h, buf, size); c_close(h); vec_decode(buf, size, …);`

## The .VEC format = a vector/polyline bytecode interpreter

`.VEC` files are **not** raw bitmaps; they are interpreted command streams.

```
vec_decode(buf, len, …)   FUN_1000_7b5a  -> overlay  vec_run  FUN_1c28_0000
vec_run:
  save stream ptr / length into globals (DAT_203b_4e0e = stream, 4e10 = seg, …)
  loop:
    vec_fetch()                      FUN_1c28_0a09   — read next token
    if carry (end of stream) -> return
    dispatch: ( *table[ (opcode & 0x7fff) - 1 ] )()   table @ DGROUP 0x4e37
    while opcode > 0
```

`vec_fetch` reads the stream as **big-endian 16-bit words** (the `CONCAT11`
byte-swaps). It pulls a coordinate pair (`4e35`,`4e33`); **if the lead word is
`< 0x10` it is an opcode** (and 3 more operand words are read), otherwise the
words are coordinate data. So the bulk of a `.VEC` is a polyline of big-endian
(x,y) pairs, punctuated by a few control opcodes.

### Dispatch table @ DGROUP `0x4e37` (near offsets into overlay seg `1c28`)

- Most slots point to `1c28:0193`, which is a bare `ret` — the **no-op** handler
  for unused/reserved opcodes.
- **opcode 4** → `1c28:0194`: sets `DS=0x103b`, reads a byte from the stream into
  `DAT_203b_4e22` — looks like **set color / pen / attribute**.
- **opcode 12** → `1c28:04b0`: **bounds/clip check** — transforms the current
  coord (via `lcall 0xcda:0`) and compares against limits `DAT_203b_4e0a` /
  `DAT_203b_4e0c` (`ja` skips when outside).
- `0xffff` terminator after the last real entry.
- `lcall 0x0cda:0` (`FUN_1cda_0000`) is a shared helper the handlers call with a
  selector in AX (e.g. `AX=0xc`) — likely the coordinate transform / address
  calc. Not yet named.

## Confidence / open

- I/O stack: **high** confidence (clean decompiles, matches DOS INT 21h sites).
- `.VEC` = interpreted vector stream with big-endian coords and a `0x4e37`
  dispatch table: **high** confidence on structure; **partial** on opcode
  semantics — only opcodes 4 and 12 read so far. Full opcode set + the operand
  encoding (what the non-opcode coordinate words drive: line? fill? blit?) is the
  next step. `FUN_1cda_0000` (the transform helper) anchors the geometry.
