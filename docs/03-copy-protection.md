# 03 — Copy protection (and why this copy is already defeated)

Analysis of `originals/unpacked/BUMPY_unpacked.exe` (the unpacked original) in
Ghidra. All function/global names are Ghidra auto-names at this stage.

## The layers of protection

1. **TinyProg CRC anti-tamper** (the EXE wrapper, see `02-unpacking-tinyprog.md`).
   A CRC16-keyed self-check that crashes the program (`TINYPROG says, "Patched
   program!"`) if any byte of the on-disk image is modified. Its purpose is to
   stop anyone patching out the in-game check below.
2. **An in-game "platform number" challenge** — the actual anti-piracy gate.
3. **Protection data files** shipped by the protected release and stripped by the
   crack: `VGUARD.DAT` (120 B), `VS.VSN` (encrypted, `YZFA` magic), `CODES.EXE`
   (TinyProg-packed helper). Plus a `YOUR PASSWORD / ENTER YOUR PASSWORD /
   PASSWORD OK / PASSWORD ERROR` string-pointer table (DGROUP `0x119e`) — an
   alternate/registration password path.

## The "platform number" challenge

Invoked from the **level-load routine** `FUN_1000_2d14` when entering any level
beyond the first, once per session:

```c
// FUN_1000_2d14 (level load)
if ((1 < current_level /*DAT_..79b2*/) && (protection_flag /*DAT_..119a*/ == 0))
    FUN_1000_4015();                       // run the challenge
if (protection_flag == -1)                 // on failure...
    current_level = 1;                     // ...kick the player back to level 1
... // then load the level's sprite/vector data (FUN_1000_745e etc.)
```

The challenge `FUN_1000_4015` (`FUN_1000_a9f5` is a `memcpy(dest, DS:src, n)`):
- copies two tables out of DGROUP into locals:
  - `local_28` (16 words) ← `DS:0x11b6` — on-screen **x positions** of the platforms;
  - `local_38` (16 bytes) ← `DS:0x11d6` — the **expected answer** for each platform;
- picks a random index 2..15 (`call FUN_1000_93b1; and 0xf; cmp 2; jl`), displays
  the platform at `local_28[index]`, and loads the expected answer
  `local_6 = local_38[index]` (`mov al,[bp+si-0x36]` @ `1000:4120`);
- prints `"Enter the platform number"` and reads a number 0..99 dialed with the
  +/- keys and confirmed with ENTER (`local_4`).

### The recovered tables (DGROUP)

```
positions @ 0x11b6 (x): 00 00 46 4f 51 93 9f a8 b4 b3 be c4 ca cb da e1
answers   @ 0x11d6    :  0  0  4  6  7  5 15 16 24 19 28 26 25 27 17 18
```

So for a displayed platform at index `i` (2..15) the correct number to enter is
`answers[i]` — i.e. the manual/codes show numbered platforms and the game asks
you to read off the number of the one it highlights. The intended check is:
*entered number == `answers[index]`?* — on mismatch set the fail flag (`-1`),
which makes the level-loader bounce the player back to level 1.

### Proof the check is patched out (disassembly of `FUN_1000_4015`)

```
4120: mov al,[bp+si-0x36]   ; al = local_38[index]  = EXPECTED answer
4123: mov [bp-4],al         ; local_6 = expected
4126: mov al,0
4128: mov [bp-3],al         ; local_5 = 0  (loop flag)
412b: mov [bp-2],al         ; local_4 = 0  (entered number)
412e: mov byte [0x119a],1   ; protection_flag = 1  (PASS, set UNCONDITIONALLY)
...   <print prompt, input loop dialing local_4 with +/- and ENTER> ...
4210: ...                   ; loop until ENTER
      mov al,[bp-2]         ; local_6 = local_4  (store entered number)
      ...                   ; local_7 = 0xff -> return
```

The expected answer is fetched at `4120` and then **never compared** to the
entered number; `protection_flag` is set to `1` (pass) at `412e` before input is
even read, and is **never** set to `-1` anywhere in the program. The comparison
and fail-set that the original protection performed here have been removed.

## Why this binary's protection is already defeated

Tracing the flag `DAT_203b_119a` across the whole program (`FlagRefs.java`):

```
symbol DAT_203b_119a @ 203b:119a
  ref 1000:412e WRITE  func=FUN_1000_4015   ->  DAT_203b_119a = 1;   (the ONLY write)
  ref 1000:2d2f READ   func=FUN_1000_2d14   ->  == 0   (not-yet-asked)
  ref 1000:2d3b READ   func=FUN_1000_2d14   ->  == -1  (failure)
```

**The flag is only ever written with `1`. Nothing in the binary ever sets it to
`-1`.** The answer comparison is absent and the failure branch (`== -1` → level
1) is unreachable dead code. The challenge therefore displays its prompt, accepts
any number, marks itself "asked", and lets the player proceed unconditionally.

In other words, in the copy we have, the anti-piracy check has been **patched
out** — only the cosmetic prompt remains.

## Consistency with the unpacking result

This dovetails with `02`: our unpack of the **protected** `BUMPY.EXE` is
byte-for-byte identical to the independent **Fairlight crack**. Both 1992 copies
are the *same already-cracked build* — the old-games "protected" release is a
TinyProg-wrapped copy of an already-patched executable that still ships the
now-inert protection data files (`VGUARD.DAT` / `VS.VSN` / `CODES.EXE`).

A truly pristine, protection-active original (where the comparison sets the fail
flag) would be a *different* executable than either copy we have.

## Reconstructed original check

```c
// what FUN_1000_4015 originally did at the end of the input loop:
if (entered /*local_4*/ != answers[index] /*local_38[index]*/)
    protection_flag /*DAT_..119a*/ = -1;   // fail -> level-loader resets to level 1
// the cracked build replaces this with an unconditional `protection_flag = 1`
// (at 1000:412e) and a no-op `local_6 = local_4`.
```

## Open / next

- The `PASSWORD` string-table path (DGROUP `0x119e`) and `CODES.EXE` deserve a
  look — likely the registration/manual-code variant of the same protection.

## Scripts

`tools/ghidra_scripts/FindProtection.java` (string→routine), `FindCallers.java`
(who invokes it), `FlagRefs.java` (trace a global), `tools/find_string_refs.py`
(DGROUP-relative string-offset xref scan). Re-run against the `BumpyDecomp`
Ghidra project with `tools/bin/ghidra-headless ... -process BUMPY_unpacked.exe
-noanalysis -postScript <script>`.
