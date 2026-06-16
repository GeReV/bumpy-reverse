#ifndef BUMPY_H
#define BUMPY_H

#include <dos.h>   /* MK_FP, int86, REGS */

typedef unsigned char  u8;
typedef unsigned int   u16;   /* 16-bit int in this model */
typedef unsigned long  u32;
typedef signed char    s8;
typedef int            s16;
typedef long           s32;

/* Far-pointer / 32-bit globals are kept SPLIT as two words (see docs/06-engine.md).
   Reconstruct a far pointer at the use site with MK_FP(seg, off). */

/* PRNG state (segment 203b in the original DGROUP). */
extern u16 prng_state0;
extern u16 prng_state1;
extern u16 prng_state2;

/* PRNG functions (src/prng.c). */
void prng_seed(u16 seed);
void prng_step(void);

#endif /* BUMPY_H */
