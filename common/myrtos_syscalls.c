// The bottom end of newlib: what the C library calls when it needs the system.
//
// A module built this way gets the whole standard library -- qsort, strtod,
// time, the full stdio -- instead of the header-only subset in myrtos_stdio.h
// and its neighbours. That subset is smaller and needs nothing linked; this is
// for ported code, where the library is what the program was written against
// and rewriting it is not the job.
//
// Newlib does not want the _xxx_r forms from us. Those live in the library and
// call these, so the twelve below are the whole contract.
//
// --- REENTRANCY ------------------------------------------------------------
// The per-process C library state -- errno, the open streams, strtok's memory,
// the multiprecision buffers printf uses -- lives in a struct _reent that
// _impure_ptr points at. On a normal RTOS that pointer has to be swapped at
// every context switch, or each thread given its own through __getreent.
//
// Here neither is needed, and the reason is the module format. _impure_ptr and
// _impure_data are writable data, so the loader marks any module using newlib
// PRIVATE and copies it per process -- and the copy carries its own pointer and
// its own struct. Every process gets a private C library without anyone
// arranging it.
//
// The price is that such a module is not shared: two processes running it cost
// two copies of the code as well. Making it shareable would need
// __DYNAMIC_REENT__ and a newlib rebuilt to match, since the shipped library
// resolves _REENT to _impure_ptr internally. Not worth it.
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/times.h>
#include "myrtos_abi.h"

#undef errno
extern int errno;

// How much heap the program gets, taken once from the bulk pool the first time
// malloc asks. A module that needs more defines its own; the symbol is weak so
// that doing so is one line and needs no build flag.
//
// The bulk pool rather than the process's own area, because a ported program
// sizes its appetite by what it is editing rather than by what the module
// header declared, and PSRAM has eight megabytes to be casual with.
__attribute__((weak)) uint32_t myrtos_heap_bytes = 64u * 1024u;

static char *heap_base, *heap_end, *heap_brk;

void *_sbrk(int incr)
{
    if (!heap_base) {
        heap_base = (char *)myrtos_alloc_bulk(myrtos_heap_bytes);
        if (!heap_base) { errno = ENOMEM; return (void *)-1; }
        heap_brk = heap_base;
        heap_end = heap_base + myrtos_heap_bytes;
    }
    char *prev = heap_brk;
    if (incr > 0 && heap_brk + incr > heap_end) { errno = ENOMEM; return (void *)-1; }
    heap_brk += incr;
    return prev;
}

int _write(int fd, const char *buf, int len)
{
    int32_t n = myrtos_write(fd, buf, (uint32_t)len);
    if (n < 0) { errno = EBADF; return -1; }
    return n;
}

int _read(int fd, char *buf, int len)
{
    int32_t n = myrtos_read(fd, buf, (uint32_t)len);
    if (n < 0) { errno = EBADF; return -1; }
    return n;
}

// Newlib's O_RDONLY/O_WRONLY/O_CREAT happen to be the values myrtos_open_flags
// takes, which is not luck -- both took them from Unix.
int _open(const char *path, int flags, int mode)
{
    (void)mode;
    int32_t fd = myrtos_open_flags(path, (uint32_t)flags);
    if (fd < 0) { errno = ENOENT; return -1; }
    return fd;
}

int _close(int fd) { myrtos_close(fd); return 0; }

int _lseek(int fd, int offset, int whence)
{
    int32_t n = myrtos_seek(fd, offset, (uint32_t)whence);
    if (n < 0) { errno = ESPIPE; return -1; }
    return n;
}

// Enough for stdio to decide how to buffer. A character device is the safe
// answer for a descriptor whose nature we do not track: it makes newlib read
// and write without assuming it can seek.
int _fstat(int fd, struct stat *st)
{
    (void)fd;
    st->st_mode = S_IFCHR;
    st->st_size = 0;
    return 0;
}

int _stat(const char *path, struct stat *st)
{
    uint32_t size = 0;
    if (myrtos_fs_stat(path, &size) < 0) { errno = ENOENT; return -1; }
    st->st_mode = S_IFREG;
    st->st_size = (off_t)size;
    return 0;
}

int _unlink(const char *path)
{
    if (myrtos_fs_remove(path) < 0) { errno = ENOENT; return -1; }
    return 0;
}

int _isatty(int fd) { (void)fd; return 1; }

// There is no getpid syscall and nothing here needs one; newlib only wants
// the symbol so that _kill has something to be refused for.
int _getpid(void) { return 1; }

int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }

void _exit(int code) { (void)code; myrtos_exit(); for (;;) { } }

clock_t _times(struct tms *buf) { (void)buf; return (clock_t)-1; }

int _gettimeofday(void *tv, void *tz) { (void)tv; (void)tz; errno = ENOSYS; return -1; }

// Ported programs have a main; myrtos starts a module at module_main. Weak, so
// a module written for myrtos from the start can define its own and never have
// a main at all.
__attribute__((weak)) void module_main(int argc, char **argv)
{
    extern int main(int argc, char **argv);
    main(argc, argv);
}
