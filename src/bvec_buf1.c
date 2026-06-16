#include "bumpy.h"
#include "vec.h"

/* Large static buffers for bvec.c, declared with __far storage class so
   the linker places them in a named far data segment outside DGROUP.  This
   avoids exceeding the 64 KB DGROUP limit of the Open Watcom large memory
   model (-ml).  In -ml data pointers are already 32-bit far, so __far here
   only controls segment placement, not the pointer type callers see.
   This file: .VEC input file buffer.
   Note: the decode scratch buffer (VEC_DECODE_MAX bytes) is now declared
   static inside vec.c (vec_decode_scratch) and is not exposed here. */

#define VEC_FILE_MAX 0x4000u   /* 16 KB */

u8 __far g_file[VEC_FILE_MAX];   /* raw .VEC bytes  (~16 KB max) */
