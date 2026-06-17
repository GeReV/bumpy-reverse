#include "bumpy.h"

/* BUMSPJEU.BIN is 89116 bytes (> 64 KB), so the bank is a __huge static array
   (a single __far object may not exceed a 64 KB segment).  sprite.c takes its
   base as a far pointer and reaches every frame via normalized far arithmetic. */
u8 __huge g_spr_bank[0x16000];   /* 90112 >= 89116 */
