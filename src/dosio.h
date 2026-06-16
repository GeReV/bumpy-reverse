#ifndef DOSIO_H
#define DOSIO_H

#include "bumpy.h"

/* Minimal DOS file I/O via INT 21h (Open Watcom open/read/write/close).
   Returns the number of bytes read into buf (capped at cap), or -1 on error. */
s16 dosio_load(const char *path, u8 *buf, u16 cap);

/* Write `len` bytes from buf to a newly-created file `path`.
   Returns 0 on success, -1 on error. */
int dosio_save(const char *path, const u8 *buf, u16 len);

#endif /* DOSIO_H */
