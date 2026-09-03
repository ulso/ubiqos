#include "../../common/myrtos_abi.h"
#include <sys/stat.h>
#include <errno.h>

// What newlib asks of the system underneath it.
//
// wasm3 uses vsnprintf, strtod and a little stdio, and newlib's stdio is built
// on these. They are the standard port layer, and myrtos has a call for each of
// the ones that matter; the rest exist so the linker stops asking.
//
// This is only reachable from a SINGLE module. A shareable one may not have
// writable data, and newlib is full of it -- _impure_ptr, errno, the FILE
// table. That is the whole reason the wasm host is SINGLE.

// The heap newlib's malloc grows into.
//
// Not a static array: a SINGLE module is built with -fno-zero-initialized-in-bss
// so that its zeroed data is in the image rather than dropped by objcopy, and a
// static arena would therefore be megabytes of zeroes in the .mod file. So it
// is asked for at startup instead, from the pool -- which for a module that is
// not real-time is PSRAM, where there is room to be generous.
static char  *heap_base;
static uint32_t heap_size;
static uint32_t heap_used;

int wasm_heap_init(uint32_t bytes)
{
    heap_base = (char *)myrtos_alloc(bytes);
    if (!heap_base)
        return -1;
    heap_size = bytes;
    heap_used = 0;
    return 0;
}

void *_sbrk(int incr)
{
    if (!heap_base || incr < 0 || heap_used + (uint32_t)incr > heap_size) {
        errno = ENOMEM;
        return (void *)-1;
    }
    char *p = heap_base + heap_used;
    heap_used += (uint32_t)incr;
    return p;
}

// The descriptors are myrtos's own, so these are one line each.
int _write(int fd, const char *buf, int len)
{
    int32_t n = myrtos_write(fd, buf, (uint32_t)len);
    if (n < 0) { errno = EBADF; return -1; }
    return (int)n;
}

int _read(int fd, char *buf, int len)
{
    int32_t n = myrtos_read(fd, buf, (uint32_t)len);
    if (n < 0) { errno = EBADF; return -1; }
    return (int)n;
}

int _close(int fd)               { myrtos_close(fd); return 0; }
int _lseek(int fd, int off, int w) { (void)fd; (void)off; (void)w; return 0; }

// Enough for newlib to decide stdout is a terminal and stop trying to buffer
// it into a file it cannot stat.
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd)              { (void)fd; return 1; }

int _getpid(void)                { return 1; }
int _kill(int pid, int sig)      { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int code)             { (void)code; myrtos_exit(); for (;;) { } }
int _times(void *buf)            { (void)buf; return -1; }
