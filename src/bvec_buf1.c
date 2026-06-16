#include "bumpy.h"
#include "vec.h"

/* Large static buffers for bvec.c, declared with __far storage class so
   the linker places them in a named far data segment outside DGROUP.  This
   avoids exceeding the 64 KB DGROUP limit of the Open Watcom large memory
   model (-ml).  In -ml data pointers are already 32-bit far, so __far here
   only controls segment placement, not the pointer type callers see.
   This file: .VEC input file buffer + decode scratch buffer. */

#define VEC_FILE_MAX 0x4000u   /* 16 KB */

u8 __far g_file[VEC_FILE_MAX];       /* raw .VEC bytes  (~16 KB max) */
u8 __far g_scratch[VEC_DECODE_MAX];  /* decode work buf (~32 KB)     */
