#include "bumpy.h"
#include "vec.h"

/* Large static buffers for bvec.c, declared with __far storage class so
   the linker places them in a named far data segment outside DGROUP.  This
   avoids exceeding the 64 KB DGROUP limit of the Open Watcom large memory
   model (-ml).  In -ml data pointers are already 32-bit far, so __far here
   only controls segment placement, not the pointer type callers see.
   This file: planar pixel buffer (plane-major, consumed directly by
   video_blit_planar after vec_decode_planar).
   Note: g_chunky removed; the faithful pipeline is fully planar end-to-end. */

u8 __far g_planar[32000u];      /* 32000-byte plane-major planar buffer      */
