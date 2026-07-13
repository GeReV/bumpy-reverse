#include "bumpy.h"

u16 prng_state0;
u16 prng_state1;
u16 prng_state2;

/* copyprot_seed_src (DGROUP 0x119c) — the copy-protection PRNG seed source.  The engine
 * mutates it live after boot (static image 0x1e61 -> 0x5192 RT); its only READER,
 * copyprotect_challenge (level.c), is gated behind #ifdef BUMPY_COPY_PROTECTION (off in both
 * builds), but play_intro_animation_loop (screens.c) WRITES it (+= 7 per input poll) to stir
 * the seed while it waits — so it needs real storage.  Defined here with the other prng state. */
u16 copyprot_seed_src;
