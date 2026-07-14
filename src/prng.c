/*
 * prng.c — 3-word PRNG (prng_seed / prng_step)
 *
 * Transcribed verbatim from Ghidra decompiled corpus:
 *   local/decomp/prng_seed@1ce5_0000.c
 *   local/decomp/prng_step@1ce5_001f.c
 *
 * Caller analysis: rand() calls prng_step() void; prng_seed_thunk (1000:93a4)
 * calls prng_seed() void.  Neither caller reads a return value, so both
 * functions are void and callers read the state globals directly.
 *
 * CARRY2(a,b): carry-out of 16-bit unsigned addition a+b.
 * The >> 1 | carry << 0xf idiom is a 17-bit rotate-right-through-carry;
 * transcribed literally per porting rules.
 */

#include "bumpy.h"

/* Carry-out of a 16-bit unsigned addition. */
#define CARRY2(a, b) (((u32)(a) + (u32)(b)) > 0xFFFFU)

void prng_seed(u16 seed)
{
    prng_state0 = seed;
    prng_state1 = 0;
    prng_state2 = 0;
}

void prng_step(void)
{
    u16 mixed;

    /* stage 1: state2 mix — combine state2 with a constant, XOR in state1/state0. */
    prng_state2 = prng_state2 + 0x2432 ^ prng_state1 ^ prng_state0;

    /* stage 2: rotate state2 (self-rotate, bit0 -> bit15), combine with state0/
       state1/a constant/state2 again, then self-rotate the result once more. */
    mixed = ((prng_state2 >> 1 | (u16)((prng_state2 & 1) != 0) << 0xf) - prng_state0 ^ prng_state1) +
            0x1c12 ^ prng_state2;
    mixed = mixed >> 1 | (u16)((mixed & 1) != 0) << 0xf;

    /* stage 3: state1 combine — mixed+state2, rotated right through the CARRY
       from that addition (true rotate-through-carry, unlike stage 2's self-
       rotates: the bit shifted into position 15 here is the add's carry-out,
       not either operand's own bit0). */
    prng_state1 = mixed + prng_state2 >> 1 | (u16)CARRY2(mixed, prng_state2) << 0xf;

    /* stage 4: state0 combine — state0+state2, XORed with a constant and the
       just-computed new state1. */
    prng_state0 = prng_state0 + prng_state2 ^ 0x3812 ^ prng_state1;
}
