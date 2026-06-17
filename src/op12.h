#ifndef OP12_H_INCLUDED
#define OP12_H_INCLUDED

#include "bumpy.h"

/* op12 masked-blit compositor + inner vec_run record loop.

   Faithful C port of tools/extract/op12_port.py (validated byte-for-byte
   against the vec_cpu oracle).  The Python runs over a flat 1 MB array with
   absolute 20-bit linear addresses and DGROUP state at DG=0x114b0.  This port
   keeps that exact linear-address model — every pointer is a 32-bit linear
   value — but materializes only the two regions actually touched:

     - DECODE ARENA  at linear base 0x67bf0 (op12_port's STREAM): the inner
       record stream and where op12 builds the final decoded image.  Reads and
       (almost all) writes land here.  Maps to op12_arena[lin - ARENA_BASE].
     - WINDOW (0x400) at linear base DG+0x4e97 = 0x119347: the sliding window
       that the dst write cursor wraps into.  Maps to window[lin - WIN_BASE].

   The two regions are disjoint and far apart in the linear space, so routing
   by (lin >= ARENA_BASE) is unambiguous.  DGROUP state (offsets 0x4df6..0x4e35)
   is a small byte array (st[]); op12_port's gv(o)/sv(o,v) become reads/writes
   of st[o - ST_BASE], preserving the engine's split off/seg and lo/hi word
   pairs exactly.

   Entry point: op12_vec_run runs the inner record loop over the arena (which
   already holds the inner stream after the outer op4 decode), driving op12 /
   op4 records until the stream terminates, leaving the final decoded image in
   the arena. */

/* Arena big enough for the 32099-byte decoded image (op12_port DECLARED_LEN
   0x7d63) plus the few-byte slop that finalize's 4-byte slice copies touch. */
#define OP12_ARENA_SIZE   0x8000u

/* Run the inner vec_run record loop over the decode arena.
   arena      : OP12_ARENA_SIZE-byte buffer; on entry holds the inner record
                stream at offset 0; on return holds the final decoded image.
   declared_len : decoded-image length bound (op12_port DECLARED_LEN, 0x7d63),
                  used to set vec_end = stream + declared_len.
   payload_len  : the original .VEC file size (op12_port vsav seed = file len),
                  used as the op4 input-end bound for any inner op4 record. */
void op12_vec_run(u8 __far *arena, u16 declared_len, u16 payload_len);

#endif /* OP12_H_INCLUDED */
