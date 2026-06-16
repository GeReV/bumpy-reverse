#include "dosio.h"
#include <fcntl.h>
#include <io.h>

/* Open Watcom DOS file I/O. open()/read()/write()/close() map onto INT 21h
   AH=3D/3F/3C/40/3E, which tools/emu/pydos.py services. We read/write in a
   single call each; all buffers in this slice are < 64 KB so one call suffices. */

s16 dosio_load(const char *path, u8 *buf, u16 cap)
{
    int fd;
    int n;

    fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) {
        return -1;
    }
    n = read(fd, buf, cap);
    close(fd);
    if (n < 0) {
        return -1;
    }
    return (s16)n;
}

int dosio_save(const char *path, const u8 *buf, u16 len)
{
    int fd;
    int n;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, S_IREAD | S_IWRITE);
    if (fd < 0) {
        return -1;
    }
    n = write(fd, buf, len);
    close(fd);
    if (n != (int)len) {
        return -1;
    }
    return 0;
}
