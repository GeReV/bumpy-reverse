# TinyProg — Executable Packer Format Reference

TinyProg is a self-extracting packer used by *Bumpy's Arcade Fantasy* (1992) to
wrap `BUMPY.EXE`. It consists of two sequential stub layers: an outer
CRC16-CCITT-keyed XOR descrambler with anti-tamper enforcement, and an inner
LZSS decompressor with segment-fixup relocation. The on-disk file is ~45 KB;
the unpacked load module is `0x1a640` bytes with a 1050-entry relocation table.

---

## Layer 1 — CRC16-CCITT-keyed XOR descrambler + anti-tamper

The MZ entry stub (real code at image offset `0x54`, reached via two `jmp`s
from `cs:ip = 0:0`) performs three operations:

1. **Table construction.** Builds a 256-entry CRC16-CCITT table at `ss:0000`
   using polynomial `0x1021`.

2. **Backwards decryption.** Walks the loaded image backwards in 32 KB blocks.
   Each word is XOR-decrypted with a rolling key `dx`. After each word is
   emitted the key is advanced by two CRC-table lookups fed by the just-emitted
   plaintext bytes — the keystream is a CRC over the plaintext, making it a
   self-checking stream cipher. Any modified byte causes the key to diverge from
   that point onward.

3. **Anti-tamper check.** After the final block the stub compares the rolling
   key against a stored checksum at `[si+0x30]`. A diverged key (caused by any
   patched byte) redirects execution via `jmp bx` into garbage — the
   `TINYPROG says, "Patched program!"` path. A matching key relocates three
   info-block pointer fields by the load segment and far-jumps to the layer-2
   stub. Layer 1 does **not** relocate the program.

---

## Layer 2 — LZSS decompressor + relocator

Revealed by layer 1. A bit-pumped LZSS scheme: control bits are shifted out of
a shift register (`bp`); a set bit copies a literal byte (`movsb`); a clear bit
reads a back-reference offset+length pair and copies from `[es:bx+di]`. This
inflates the ~110 KB program image from the compressed payload, then applies the
1050-entry segment-fixup relocation table (scanning every byte offset, since
relocation targets can sit at odd offsets), and transfers to the original entry
point.

---

## Info block

The info block is located at `DS:si`, where `si = 0x105 + [0x101]`. Layer 1
reads and patches it; layer 2 consumes it.

| Offset | Size  | Field |
|--------|-------|-------|
| `+0x0e` | word | Block-1 word count |
| `+0x10` | word | Block count |
| `+0x12` / `+0x14` | far ptr | Source (compressed data) |
| `+0x16` / `+0x18` | far ptr | Destination |
| `+0x1a` / `+0x1c` | far ptr | Original entry point (OEP) |
| `+0x2e` | word | Initial key (`dx` seed) |
| `+0x30` | word | Final-key checksum (anti-tamper reference value) |

---

## Unpacker

`tools/tinyprog_unpack.py` recovers the unpacked MZ by emulating both layers
at two different load segments and differencing the outputs to reconstruct the
relocation table. `tools/compare_unpacked.py` diffs two MZ load modules for
cross-validation.
