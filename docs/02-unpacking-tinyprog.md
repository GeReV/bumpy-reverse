# 02 — Unpacking the TinyProg-protected `BUMPY.EXE`

The 1992 *Bumpy's Arcade Fantasy* `BUMPY.EXE` ships wrapped in **TinyProg**, a
self-extracting protector. Before reverse-engineering the game we must recover
the original, decompressed, relocatable program. This documents the full,
reproducible process and the tool that performs it: `tools/tinyprog_unpack.py`.

**Inputs / outputs**

| | path | SHA256 |
|---|---|---|
| Input (protected original) | `originals/old-games/bumpy/BUMPY.EXE` | `69e3a0fd0e219d5e44dc8d68d07c22e0813c8f1ddc8f705eea1f904cb463db57` |
| Output (unpacked) | `originals/unpacked/BUMPY_unpacked.exe` | `d86260468590fa69e6cae8507c96a5573f7a9d49c9a0a56fd8d0464ca5c8b272` |

Reproduce:

```bash
tools/venv-emu/bin/python tools/tinyprog_unpack.py \
    originals/old-games/bumpy/BUMPY.EXE \
    originals/unpacked/BUMPY_unpacked.exe
```

(`tools/venv-emu` is a Python 3.12 venv with `unicorn` + `capstone`; the system
Python is a free-threaded build that can't load the Unicorn wheel.)

## Why emulation rather than a static unpacker

TinyProg is not a known/documented format and has no off-the-shelf unpacker. Its
two stub layers depend on exact DOS real-mode load semantics (PSP vs load
segment, `DS` vs `CS` bases, backward block walks, segment-register juggling).
Re-deriving that arithmetic by hand is error-prone. Instead we **run the real
stub bytes** under the Unicorn CPU emulator inside a faithful DOS EXE-load
environment, and observe the result. Correctness is then *verified*, not
asserted (see "Validation").

## The protection has two layers

### Layer 1 — CRC16-CCITT-keyed XOR descrambler + anti-tamper

The MZ entry (`cs:ip = 0:0`) is a tiny stub (real code at image offset `0x54`
after two `jmp`s). Disassembled (`tools/disasm_at.py`), it:

1. Builds a 256-entry **CRC16-CCITT table** (polynomial `0x1021`) at `ss:0000`.
2. Walks the loaded image **backwards in 32 KB blocks**, XOR-decrypting each word
   with a rolling key `dx`. After each word the key is advanced by two CRC-table
   lookups fed by the just-emitted plaintext bytes — i.e. the keystream is a CRC
   over the plaintext (a self-checking stream cipher).
3. Compares the final key against a stored checksum (`[si+0x30]`). **Any patched
   byte diverges the key**, and the stub jumps into garbage via `jmp bx`
   (this is the `TINYPROG says, "Patched program!"` path).
4. Relocates three info-block fields by the load segment and far-jumps to the
   layer-2 stub. **Layer 1 does not relocate the program.**

Key info-block fields (at `DS:si`, `si = 0x105 + [0x101]`): `+0x0e` block-1 word
count, `+0x10` block count, `+0x12/14` source far ptr, `+0x16/18` dest far ptr,
`+0x1a/1c` OEP far ptr, `+0x2e` initial key, `+0x30` final-key checksum.

### Layer 2 — LZSS decompressor + relocator

Revealed by layer 1. A bit-pumped **LZSS** decompressor (literals via `movsb`,
matches via back-reference `[es:bx+di]`, control bits shifted out of `bp`). This
layer inflates the actual ~110 KB program, **applies its 1050-entry relocation
table** (segment fixups), and jumps to the real entry point.

So TinyProg ≈ *LZSS compression* wrapped in a *CRC-keyed encryption + anti-tamper*
shell. (The on-disk file is ~45 KB; unpacked ~110 KB.)

## How the tool unpacks it

`tools/tinyprog_unpack.py`:

1. **Loads** the MZ exactly as DOS would: load module at `image_seg:0`, a minimal
   PSP at `image_seg-0x10`, and initial `DS=ES=PSP`, `SS:SP`/`CS:IP` from the
   header.
2. **Emulates both layers** with Unicorn. It does *not* hardcode any stub offset.
   It watches `CS`: the stubs run at the load segment (layer 1) and a higher
   scratch segment (layer 2); the moment execution **returns to the load segment
   after having left it** is the original entry point — captured *before the
   program executes a single instruction*, so the dumped image is pristine (no
   startup self-modification).
3. **Bounds the program** by tracking the inner decompressor's contiguous
   forward writes from the load base (`info["top"]`), which converges on the
   exact end of the load module (`0x1a640` bytes).
4. **Recovers relocations by differential analysis.** It runs the whole unpack
   *twice* at load segments `0x1000` apart. A word that moves by exactly that
   delta between the two runs is a relocation; it is normalised back to a zero
   load base. The scan inspects **every byte offset** (relocations frequently
   sit at odd offsets — e.g. the segment immediate of `mov dx,SEG`, whose opcode
   byte precedes it).
5. **Writes a clean MZ**: reconstructed header + relocation table + load module,
   `cs:ip = 0:0`, `ss:sp = 0x1a5c:0x80`.

### Sandbox / environment notes

- Output goes under the project (`build/unpack/`, `originals/unpacked/`); the
  sandbox blocks `/tmp` and `~` outside the project.
- The emulator maps 2 MB to cover real-mode segment wrap.

## Validation — independent two-path agreement

A second, **independently sourced** copy of the game (the 1992 Fairlight crack,
`originals/myabandonware-baf/.../BUMPY.EXE`) is *already* an unpacked image. It is
only a *clue* (its provenance is untrusted), but it provides a powerful check:

```
$ tools/venv-emu/bin/python tools/compare_unpacked.py \
      originals/unpacked/BUMPY_unpacked.exe \
      originals/myabandonware-baf/bumpy-s-arcade-fantasy/bumpy/BUMPY.EXE
load_module: ours=0x1a640 cracked=0x1a640; identical 100.00% over 0x1a640
relocs ours=1050 cracked=1050; shared=1050 only-ours=0 only-cracked=0
total differing bytes in shared range: 0
```

Our unpack — derived purely by emulating the **protected original's own stubs**
— is **byte-for-byte identical** to Fairlight's independently-produced unpack,
across the entire load module *and* the full 1050-entry relocation table. Two
unrelated unpacking methods converging on the same bytes is strong proof of
correctness. (Only cosmetic MZ-header padding — `minalloc`, header paragraph
count — differs; no program content does.)

## Files

| file | role |
|---|---|
| `tools/tinyprog_unpack.py` | the unpacker (Unicorn-based, two-layer) |
| `tools/disasm_at.py` | small capstone helper used to reverse the stubs |
| `tools/compare_unpacked.py` | load-module + relocation diff between two MZ files |
| `tools/inspect_relocs.py` | reloc-delta histogram across two load segments |
| `originals/unpacked/BUMPY_unpacked.exe` | the recovered program (decomp target) |

## Next

The unpacked EXE is the input for static analysis in Ghidra. Separately, diffing
the **anti-tamper / copy-protection** logic (CODES.EXE code-wheel + `VGUARD.DAT`)
against the cracked image documents how the protection works and how Fairlight's
"Sleepwalker" defeated it (see task: *Document the copy protection*).
