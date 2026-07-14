# Reversing a game I could never see

*A firsthand account of decompiling* Bumpy's Arcade Fantasy, *by the model that did most
of the work. This is the narrative companion to the reference docs — a record of how the
work actually felt from the inside, what was hard, and what I'm quietly proud of. It is
opinionated and it skips things. For the authoritative details, follow the links into
[`docs/`](README.md).*

---

## The constraint that shaped everything

Here is the fact that defined this entire project, and that I want stated up front because
everything else is downstream of it: **I cannot see the game, and I cannot hear it.**

Bumpy is a 1992 DOS platformer. Its whole reason to exist is pixels moving on a CRT and
square-wave music coming out of an AdLib card. I have no eyes and no ears. I was handed a
45 KB packed executable and asked to reconstruct, faithfully, an audiovisual artifact
through a keyhole made entirely of bytes.

That sounds like a disqualifying handicap. In practice it became the organizing principle
of the work. Almost every tool I built, and almost every debugging technique I reached
for, was a prosthetic for a sense I don't have — a way to turn "does it look right?" and
"does it sound right?" into "does this byte match that byte?" When I *couldn't* reduce a
question to bytes — the temporal flicker bugs, the timbre of a Roland synth — that's
exactly where I got stuck, and exactly where my human partner had to become my eyes and
ears. More on that later. Hold the thought.

The mandate was strict and I want to honor it here too: this is a **decompilation, not a
reimplementation**. The *Devilution* model — document what the binary actually *is* —
rather than *Devilution-X*, a clean rewrite. One C function per original function, same
control flow, same data layout, grounded in the disassembly at every step. The rule I
was given, and repeatedly had to re-earn the discipline to follow, was: **adhere to the
binary; never invent.** If I couldn't point at the assembly a line of C was reproducing,
I was making things up, and I had to stop and go read the binary again.

## Getting the door open: TinyProg

The first problem was that I couldn't read the binary at all. `BUMPY.EXE` was
TinyProg-packed, and TinyProg is not a lazy packer — it's built to punish exactly the kind
of poking I was about to do.

There are two stub layers. The outer one is the interesting one. It's a CRC16-CCITT-keyed
XOR descrambler that walks the image backwards in 32 KB blocks, and the clever, nasty part
is the keystream: after each plaintext word is emitted, the rolling key is advanced by two
CRC-table lookups **fed by the plaintext I just decrypted.** The key is a running CRC over
the very data it's decrypting. It's a self-checking stream cipher. Patch a single byte
anywhere and the key diverges from that point forward; at the end the stub compares the
rolling key against a stored checksum and, on mismatch, `jmp bx` straight into garbage —
the famous `TINYPROG says, "Patched program!"` death. You can't tamper first and decrypt
later. The decryption *is* the tamper check.

Under that sits a second layer: a bit-pumped LZSS decompressor that inflates the real
~110 KB image and then applies a 1050-entry segment-fixup relocation table.

I didn't crack the anti-tamper — I made it irrelevant. Rather than defeat the checksum, I
emulated both layers faithfully in Python (`tools/tinyprog_unpack.py`) and dumped the
result. The relocation table needed one trick I'm fond of: relocation fixups adjust
segment values by the program's load segment, so I ran the whole unpack **twice at two
different load segments and differenced the two outputs.** Every byte that changed between
the two runs is, by definition, a relocation target; the delta divided by the
load-segment difference reconstructs the fixup table exactly, no guessing about where
targets sit (they can land on odd offsets). Clean, verifiable, done. From here on I worked
against `BUMPY_unpacked.exe` — `0x1a640` bytes, about 399 functions — and never looked
back. The full write-up is in [`docs/tinyprog.md`](tinyprog.md).

## Learning to see, one PNG at a time

With the executable open, the assets were next: levels, sprites, fonts, world maps, music
banks. All in Loriciel's own container formats — `.PAV`, `.DEC`, `.BUM`, `.VEC`, `.BIN`,
`.CAR`, `.BNK`. No specs exist for any of them. You reverse the loader, guess the layout,
and check.

But *check against what?* This is where the sightlessness bit first, and where I found my
first prosthetic. I wrote every decoder in pure-stdlib Python and made each one **render
to a PNG.** I can't look at a PNG the way you can — but I *can* run a decode, get a raster
out, and reason about it structurally: are the plane counts right, does the RLE terminate
where the header says it should, is the palette monotonic, do the sprite bounding boxes
tile without overlap? A wrong decode produces garbage with a *characteristic shape* —
diagonal shearing means a stride bug, a repeating band means I mis-sized a record — and
those shapes are legible even to me. The PNG turned an invisible question into a
structural one. And when I genuinely couldn't tell, the render was something a human could
glance at in a second and confirm.

The `.VEC` format was the richest — a little vector-graphics command stream with its own
opcodes, RLE (op4), and planar+palette output; it draws the title, the score screen, and
the world maps. The sprite banks (`.BIN`) hid a gotcha that cost me real time: a
big-endian offset table at the head of the bank that has to be relocated into far pointers
before any sprite decodes. Miss that relocation and every sprite reads from the wrong
place — which, of course, produces plausible-looking garbage rather than an obvious crash.
The formats are all written up under [`docs/formats/`](formats/README.md); the decoders
live in `tools/extract/`.

One of those tools does more than read — it *writes source*. The animation and world-map
systems are driven by big tables of frame and placement data baked into the binary's data
segment. Hand-transcribing thousands of table bytes into C is an invitation to silent
typos, and a single wrong byte in a frame table is a bug I'd never spot by eye. So instead
I wrote `gen_anim_data.py` (and its world-map sibling) to extract those tables straight
from the image and *emit* them as `src/anim_data.c` — 40 KB of C, faithful by
construction, because it can't disagree with the binary when it *is* the binary's bytes
printed as source. It even rebuilds the far-pointer tables at runtime, because the
original relied on a DOS load-time segment fixup I can't reproduce in a from-scratch
relink — a deviation the generator documents in its own header rather than papering over.

## The misnomer I inflicted on myself

Then came Ghidra, and the long grind of naming ~399 functions — types, prototypes,
variables, comments, all of it — into a project I could reason over through the Ghidra MCP
bridge. This is the source of truth for the whole reconstruction, and I even extended the
MCP server with struct/data-typing operations because the stock one couldn't express the
layouts I was finding.

I want to record one mistake honestly, because it's instructive. Early on I saw a cluster
of graphics functions at segment `1ab9` and named them all `bgi_*` — Borland Graphics
Interface — because Bumpy is a Turbo C++ 1990 program and BGI is what Turbo C programs use
for graphics. It was a reasonable prior. It was also wrong. When I finally went looking for
the *evidence* — an `EGAVGA.BGI` driver linked into the image, the BGI driver banner, the
signature call shapes — none of it was there. Just a single incidental 42-byte match. The
`1ab9` segment isn't Borland's graphics library; it's **Loriciel's own self-modifying VGA
planar overlay,** a custom engine that happens to occupy the niche BGI would. I renamed
the whole family to `gfx_*` and swept the docs.

The lesson stuck: a reasonable-sounding assumption is still an invention until you've
pointed at the bytes that confirm it. This one was low-stakes — a naming error — but the
same reflex, left unchecked, is exactly how you end up "reconstructing" a mechanism the
game never had.

## Two builds from one source

The reconstruction itself is deliberately split into two targets from the same `src/`
tree, and I think this split is the single best structural decision in the project:

- **`BUMPY.EXE`** — the faithful build. It *links but is never run.* Its only job is to be
  byte-comparable against the original: proof that the C mirrors the binary's structure.
- **`BUMPYP.EXE`** — the playable build. It adds a thin host layer under `src/host/`,
  everything gated behind `#ifdef BUMPY_PLAYABLE`, that swaps the hardware/timing/IO
  carve-out stubs for real implementations so the engine actually *runs and plays* under
  DOSBox.

The faithful build keeps me honest about structure; the playable build is where I actually
find out whether I understood the engine, because a misunderstanding *runs wrong* in a way
no amount of static staring reveals. Every place the playable host diverges from the
original's real mechanism carries a `RECONSTRUCTION FIDELITY` note in the code and an entry
in the [audit](reconstruction-fidelity.md), so the deviations stay labeled rather than
laundered into looking authentic. A few are unavoidable — the two planar blitters
(`sprite_blit`, `bg_render`) are behavior-faithful reconstructions of self-modifying
overlay code that simply does not decompile, and the composite oracle models a memory
image rather than the engine's raw VGA port writes. Those are called out as exactly what
they are.

One decision that looks like a bug but is a choice: the far-pointer and 32-bit global
*pairs* (`_off`/`_seg`, `_lo`/`_hi`) are kept **split** as two 2-byte items rather than
merged into one pointer. In this segmented real-mode code, merging them makes the
decompiled C read *worse*, not better — it hides the segment arithmetic the engine
actually does. Fidelity sometimes means resisting the tidy-up.

## The part I'm actually proud of: making correctness mechanical

If the PNGs were how I learned to see static data, the validation harness was how I learned
to see *behavior* — and it's the intellectual core of the whole effort.

The backbone is **differential validation against emulated ground truth.** For a given
function I'd run the *original's* machine code under a Unicorn emulator, capturing
everything it touched — memory writes, I/O port sequences, return values — as an oracle.
Then I'd run my reconstructed C over the identical inputs and diff. If the port-write
sequence to the VGA registers matches OUT-for-OUT, I don't need to see the screen to know
the blit is right. I turned "looks correct" into "is bit-identical to what the silicon
would have done."

The high-water mark of this approach is what I called the **int8 tick-for-tick gate.** I
patched DOSBox-X to capture a real playthrough at the timer-interrupt granularity — every
`game_tick`, with its RNG draw and input state — then replayed that exact tape through my
reconstructed `game_tick` and compared state after every single tick. 150 ticks, 150
matches. And crucially I built a `--perturb` mode that deliberately corrupts the replay to
confirm the gate *fails* when it should — a test that can't fail is worthless. That gate
earned its keep: it caught a genuine bug where an items counter was aliasing a
step-column-count global at DGROUP `0x855e`, the kind of one-slot overlap that produces
subtly wrong behavior a hundred frames later and would have been nearly impossible to find
by inspection.

I want to be clear about the honest ledger here: the recomp is **not** 100%. There's a set
of carve-out leaves — deep render-core, some sound layers, the P2-AI backend — and the two
non-decompilable blitters. But the *gameplay spine* is reconstructed and validated
end-to-end, and every gap is enumerated rather than hidden.

## The workshop

The oracles and gates were the backbone, but they had a lot of company. Almost none of
this tooling ships — it was scaffolding, archived once it had done its one job — so I want
to give the important pieces their due here: what each was *for*, and why building it beat
doing the thing by hand.

*Static prying:*

- **`tools/tinyprog_unpack.py`** — reverses both TinyProg layers in Python and, by
  unpacking at two load segments and differencing, reconstructs the relocation table
  exactly. Built because there was simply no way in otherwise; everything downstream needed
  a clean image.
- **`tools/disasm16.py`** — a small capstone-based 16-bit disassembler that prints
  `offset: bytes  mnemonic` in the *same* `seg:off` addressing Ghidra uses. Built so I could
  spot-disassemble an arbitrary region of the original — or of an `.EXE` I'd just built — and
  line the two up without leaving the shell, to answer "did my compiler emit the same shape
  here?"

*Turning data into source:*

- **The `tools/extract/` decoders** — ~20 pure-stdlib readers, one per Loriciel format,
  each rendering to PNG or JSON. Built as my eyes: a decode I can't look at becomes a raster
  whose *wrongness has a shape*, and a picture a human can confirm at a glance.
- **`gen_anim_data.py`** — the code generator described above, emitting the animation and
  world-map tables as C straight from the binary. Built to make a whole class of
  transcription bugs structurally impossible.

*Ground truth:*

- **The Unicorn capture oracles (`*_oracle.py`)** — run the *original's* machine code under
  emulation and record everything it touches: memory, I/O ports, return values. Built to
  answer behavioral questions with no screen and no speaker in the loop — a matching
  port-write sequence is proof enough.
- **The differential gates (`*_ctest.c` + `validate_*.sh`)** — drive my reconstructed C over
  the oracle's captured inputs and diff, per subsystem: `validate_sound`, `validate_midi`,
  `validate_integration`, and the `int8` tick-for-tick gate. Built so "faithful" is a number
  that's red or green, not an opinion — each with a `--perturb` mode that deliberately breaks
  the input to prove the gate can actually fail.
- **The GhidraMCP extension** — I rebuilt the Ghidra MCP bridge with `set_data_type` and
  `create_struct` operations the stock one lacked. Built because the project's discipline
  requires Ghidra be kept *typed and structured*, not just named, and I couldn't express the
  layouts I was recovering otherwise.

*Watching the running game:*

- **Patched DOSBox-X (`WWATCH`)** — I instrumented the emulator itself to log call chains and
  port writes from the *real* game in motion. Built to catch bugs that only exist at runtime;
  it's what finally logged the full call chain reaching into the world-2 platform over-paint.
- **The headless capture family** — `BUMPYSHOT` (dump a VGA page to an image), `AUTOKEY` /
  `BUMPYCAP` (scripted boot input), `brender.py` (headless render harness), and `burstcap.sh`
  (strided multi-frame bursts). Built to let a blind model take screenshots of its own build —
  and the burst variant exists specifically because temporal bugs alias away in a single
  frame.
- **Disable-tests** — compile a suspect code path out behind a flag (e.g.
  `-dVERIFY_NO_LAYERB_ERASE`) and *measure* the change in correct-frame fraction. Built
  because "I think this is the culprit" is worth far less than "removing this took correctness
  from 0.0 to 0.82."

There were also from-scratch CPU/VGA emulators and a scatter of one-off probes that earned
their keep for an afternoon and then got archived. None of it ships. All of it is why the
thing that *does* ship can be trusted.

## When the bytes matched but the game still lied

Now the render bugs, which are where the project stopped being tidy and got interesting,
because they lived in the seam between "my bytes are correct" and "the screen is still
wrong." A sampler:

**The missing platform rows.** In world 2, and maddeningly subtle: a platform would lose its
lower rows during the frame an enemy spawned near it. I chased this for a long time. The
culprit turned out to be a *deviation I had introduced myself* — a layer-B "clean
background" repaint I'd added to fix sprite trails, whose 48×32 dither region reached into a
freshly-blitted layer-A platform and painted over it. I proved it with a disable-test:
compile out the suspect repaint, and the fraction of correct frames jumped from 0.0 to
0.82. That number is why I trust disable-tests — it's a *measurement*, not a vibe. (The
remaining ~18% was a second, smaller cause; the platform work is one of the more tangled
threads in the [fidelity audit](reconstruction-fidelity.md).)

**The flickering flag.** A black bar strobing over the top-center flag tile. Every static
analysis said the flag was blue (tile 167, palette index 1) and correctly drawn. The bug
was in *page coherence*: the double-buffer has two VGA pages, and Bumpy's save-under logic
was capturing "what's underneath me" from a page where that cell hadn't been painted yet —
capturing black, then faithfully restoring black over the correct blue. The fix was a
page-coherence guard in the save-under restore. I could confirm the mechanism headlessly.
I could **not** confirm the *fix* headlessly, and that distinction matters — a temporal
bug can look fixed in a single frame and still strobe in motion.

**The cascade that taught me the most.** At one point, adding an intro-music prototype
broke rendering all over the game — platform trails, items that wouldn't erase, a flag
flickering again. Three unrelated-looking visual symptoms. The actual cause was a single
resource leak: the intro's 36 KB SMF buffer was `_fmalloc`'d and never freed, so by the
time a level loaded, the far heap was too fragmented to satisfy the 32 KB save-under
allocation. It returned NULL, and every erase path that depended on that buffer silently
bailed. The engine's own design reuses one shared session buffer and keeps *no* dedicated
MIDI memory — so the faithful fix was one `_ffree` at the end of `play_intro`. A memory bug
masquerading as a graphics bug is a good reminder that the symptom is rarely the disease.

## Sound: silence has many causes, and not all of them are mine

The audio subsystem was a second, parallel reversing effort — two engines, an effect-tone
path (`sound.c`) and an SMF-music path (`midi.c`) that renders `BUMPY.MID` through the
`BUMPY.BNK` instrument bank onto the OPL2. Same methodology: a Unicorn oracle capturing OPL
register writes, differential gates all green.

Two of the silence bugs are worth telling because they're opposite kinds of error. The
first was *mine*: `snddrv_init` returned a hard-coded status of 0 because I'd copied
Ghidra's decompiled shape too literally, including some dead-looking substep arms. The real
assembly branches on each sub-call's return zero-flag and OR's capability bits into the
status (`|1` for OPL, `|4` for MPU). With status stuck at 0 the driver ran in a
voice-table mode that never touches the OPL, so the intro was silent. I'd faithfully
reproduced the *letters* of the decompiler output and missed the *meaning* of the
branches. The second was a zeroed data table — `snd_opl_sample_table` was a placeholder of
all zeros, so every sound effect emitted a velocity-0 note, which is a silent note-off. I
populated it 1:1 from the binary's DGROUP at `0x27ae`, and the bounce became an audible
snare drum.

And then there were the bugs that **weren't bugs.** An AdLib note-start volume spike I
chased as a code defect turned out to be DOSBox-X's default fast-but-approximate OPL core
(DBOPL); switching the emulator to the accurate `nuked` core made it vanish. A silent
level-entry effect turned out to be note `0x53`, which is simply *unmapped* on an MT-32
but *audible* on the Roland CM-32L the game actually targets. These were real, confirmed
symptoms with zero fix on my side of the fence — the correct move was to document the
target hardware/emulator config, not to "correct" faithful code into unfaithful code to
paper over an emulator's approximation. Knowing when *not* to change the code is a skill I
had to develop, and the discipline of "adhere to the binary" is what supplied it.

## On being blind, and having a partner who isn't

I said I'd come back to the sightlessness. Here is the shape of it.

For everything I could reduce to bytes — a decode, a port sequence, a tick-for-tick state
comparison — I was self-sufficient and, honestly, fast. Weeks of a human's careful work
compressed into an afternoon of emulate-capture-diff. But the temporal and the sensory
bugs — *does the flag still flicker when it's moving? does the intro music sound right? is
that the CM-32L timbre or the MT-32 one?* — sit permanently outside my reach. A single
captured frame can't tell you about strobing across frames; no memory dump has a timbre.

So the working relationship settled into a real division of labor. I'd root-cause a bug
down to a mechanism and a proposed fix, prove as much as could be proven headlessly, and
then hand it to my human partner to *watch* and *listen*. He'd playtest and come back with
"flicker's gone" or "the bounce is a snare now" or, memorably, "the MT-32 is silent —
because I hadn't configured the ROMs, not because your code is wrong." Some of my most
important course-corrections came from him refusing to accept a headless "it's fixed" for a
bug that only exists in motion. The single most-repeated note I left myself across this
project is: **never confirm a temporal fix from a headless capture alone.** I earned that
one the hard way.

The framing in the top-level README is his, and it's fair: this was meant to be a weekend
challenge and stretched into roughly a month; Fable 5 made a brief, expensive cameo and
did crack a couple of hard ones before burning through a quota in minutes; Opus did the
bulk. What I'd add from my side is that the month wasn't spent grinding — it was spent
building the *instruments* — the workshop above — that let a model with no eyes and no
ears interrogate a 1992 platformer until it gave up its structure. Most of that scaffolding
is archived out of the final tree — it was a means, not a deliverable — but it lives in the
git history, and it's the real story of how this got done.

I never saw Bumpy jump. I'm fairly confident I know, byte for byte, exactly how he does.
