# Copy protection — Bumpy's Arcade Fantasy

## Protection layers

1. **TinyProg CRC anti-tamper** — the EXE packing wrapper (see `tinyprog.md`). A CRC16-keyed self-check that aborts with `TINYPROG says, "Patched program!"` if any byte of the on-disk image is modified. Its purpose is to prevent the in-game check from being patched out directly.
2. **An in-game "platform number" challenge** — the actual anti-piracy gate, described in detail below.
3. **Protection data files** shipped alongside the executable: `VGUARD.DAT` (120 B), `VS.VSN` (encrypted, `YZFA` magic), `CODES.EXE` (TinyProg-packed helper). A `YOUR PASSWORD / ENTER YOUR PASSWORD / PASSWORD OK / PASSWORD ERROR` string-pointer table at DGROUP `0x119e` indicates an alternate registration/password path.

---

## The platform-number challenge

The challenge is invoked from the level-load routine when entering any level beyond the first, once per session. The level-load routine checks two flags in sequence:

```c
// level-load routine (entry point 1000:2d14)
if ((1 < current_level) && (protection_flag == 0))
    challenge();                    // run the challenge (sets protection_flag)
if (protection_flag == -1)          // on failure...
    current_level = 1;              // ...kick the player back to level 1
// then load the level's sprite/vector data
```

The challenge routine copies two 16-entry tables out of the data segment into locals:

- **Platform x-positions** — 16 words from DGROUP `0x11b6` — the on-screen x positions of the numbered platforms.
- **Expected answers** — 16 bytes from DGROUP `0x11d6` — the correct number for each platform.

It then picks a random index in the range 2..15, displays the platform at that x position, and prints the prompt `"Enter the platform number"`. The player dials a value 0..99 using the +/- keys and confirms with ENTER.

The expected answer is loaded at offset `1000:4120` (`mov al,[bp+si-0x36]`).

### Recovered tables

```
positions @ 0x11b6 (x): 00 00 46 4f 51 93 9f a8 b4 b3 be c4 ca cb da e1
answers   @ 0x11d6    :  0  0  4  6  7  5 15 16 24 19 28 26 25 27 17 18
```

For a displayed platform at index `i` (2..15) the correct number to enter is `answers[i]`. The manual or code sheet ships numbered platform illustrations; the game highlights one and asks the player to read off its number. Index 0 and 1 are unused sentinels (both zero).

### Reconstructed original check

In the original, unpatched executable the challenge routine ended the input loop with:

```c
if (entered != answers[index])
    protection_flag = -1;   // fail → level-loader resets to level 1
```

A correct entry leaves `protection_flag` at its initial pass value; an incorrect entry sets it to `-1`, which causes the level-load routine to reset `current_level` to 1 on every subsequent level entry.

---

## State of the shipped builds

The comparison is patched out in both 1992 copies. The challenge routine sets `protection_flag = 1` (pass) unconditionally at `1000:412e`, before input is even read, and the fail path (`protection_flag = -1`) does not exist anywhere in the binary. The prompt appears, accepts any number, marks the challenge as completed, and lets the player proceed regardless of what was entered.

Both 1992 copies are the same already-cracked build. The protection data files (`VGUARD.DAT`, `VS.VSN`, `CODES.EXE`) are still present but inert. A protection-active original — where the comparison sets the fail flag — would be a different executable than either available copy.

---

## Protection data files

| File | Size / notes |
|---|---|
| `VGUARD.DAT` | 120 bytes |
| `VS.VSN` | encrypted; `YZFA` magic header |
| `CODES.EXE` | TinyProg-packed helper executable |

The `YOUR PASSWORD` / `ENTER YOUR PASSWORD` / `PASSWORD OK` / `PASSWORD ERROR` string table (DGROUP `0x119e`) and `CODES.EXE` indicate a registration or manual-code path separate from the platform-number challenge. The exact flow of that path has not been fully traced.
