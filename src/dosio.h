#ifndef DOSIO_H
#define DOSIO_H

#include "bumpy.h"

/* Minimal DOS file I/O via INT 21h (Open Watcom open/read/write/close).
   Returns the number of bytes read into buf (capped at cap), or -1 on error. */
s16 dosio_load(const char *path, u8 *buf, u16 cap);

/* Write `len` bytes from buf to a newly-created file `path`.
   Returns 0 on success, -1 on error. */
int dosio_save(const char *path, const u8 *buf, u16 len);

/* Streaming I/O (for buffers larger than the single-shot 64 KB dosio_save/load).
   Open in chunks <= 0x4000 each; the caller advances a __huge pointer between
   chunks.  Handles are >=0; read and write return bytes or -1; create and open
   return the handle; close returns 0 or -1. */
int dosio_create(const char *path);
int dosio_write(int fd, const u8 __far *buf, u16 len);
int dosio_open_read(const char *path);
int dosio_read(int fd, u8 __far *buf, u16 len);
int dosio_close(int fd);

#endif /* DOSIO_H */
