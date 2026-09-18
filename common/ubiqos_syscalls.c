// The bottom end of newlib: what the C library calls when it needs the system.
//
// A module built this way gets the whole standard library -- qsort, strtod,
// time, the full stdio -- instead of the header-only subset in ubiqos_stdio.h
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
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include "ubiqos_abi.h"

#undef errno
extern int errno;

// How much heap the program gets, taken once from the bulk pool the first time
// malloc asks. A module that needs more defines its own; the symbol is weak so
// that doing so is one line and needs no build flag.
//
// The bulk pool rather than the process's own area, because a ported program
// sizes its appetite by what it is editing rather than by what the module
// header declared, and PSRAM has eight megabytes to be casual with.
__attribute__((weak)) uint32_t ubiqos_heap_bytes = 64u * 1024u;

static char *heap_base, *heap_end, *heap_brk;

void *_sbrk(int incr)
{
    if (!heap_base) {
        heap_base = (char *)ubiqos_alloc_bulk(ubiqos_heap_bytes);
        if (!heap_base) { errno = ENOMEM; return (void *)-1; }
        heap_brk = heap_base;
        heap_end = heap_base + ubiqos_heap_bytes;
    }
    char *prev = heap_brk;
    if (incr > 0 && heap_brk + incr > heap_end) { errno = ENOMEM; return (void *)-1; }
    heap_brk += incr;
    return prev;
}

// Why an open failed, worked out afterwards rather than reported by the kernel,
// which answers -1 to every cause alike.
//
// The distinction that matters is "not there" against "there and it still did
// not open": a program told ENOENT will create the file, and being told it for
// a descriptor table that is full makes it try again and fail the same way for
// ever. Config-file loading is the shape that hits this -- open, get ENOENT,
// write a default -- and it would quietly overwrite nothing at all.
//
// EMFILE for the last case is the likeliest of what remains rather than a fact:
// once the name exists and is a plain file, running out of descriptors is the
// only cause the file server has left that a caller can do anything about.
static int open_errno(const char *path)
{
    uint32_t size = 0;
    int32_t attr = ubiqos_fs_stat(path, &size);
    if (attr < 0) return ENOENT;
    if (attr & UBIQOS_ATTR_DIRECTORY) return EISDIR;
    return EMFILE;
}

int _write(int fd, const char *buf, int len)
{
    int32_t n = ubiqos_write(fd, buf, (uint32_t)len);
    if (n < 0) { errno = EBADF; return -1; }
    return n;
}

int _read(int fd, char *buf, int len)
{
    int32_t n = ubiqos_read(fd, buf, (uint32_t)len);
    if (n < 0) { errno = EBADF; return -1; }
    return n;
}

// The access mode is the same number in both worlds -- 0, 1 and 2, straight
// from Unix -- and NOTHING ABOVE IT IS. Newlib's O_CREAT is 0x200, which UbiqOS
// reads as O_TRUNC; newlib's O_TRUNC is 0x400, which UbiqOS reads as O_APPEND.
// Passed through unchanged, fopen(path, "w") asks to truncate a file it never
// creates. Caught by reading the two headers rather than by running it: the
// first test only opened for reading, where every one of these bits is zero.
int _open(const char *path, int flags, int mode)
{
    (void)mode;
    // A flag that cannot be honoured is refused rather than dropped. That is
    // the rule common/ubiqos_posix.h states for the other library here, and it
    // holds for the same reason: a program that asks for O_EXCL is asking to be
    // told whether it won a race, and answering yes to one it never entered is
    // how two instances come to believe they hold the same lock. Better a port
    // that fails at the call than one that fails at three in the morning.
    //
    // Everything else newlib can name -- O_CLOEXEC, O_NOCTTY, O_NONBLOCK on a
    // file -- either means nothing on a system without exec-across-fork,
    // terminals or a network, or means nothing for a file. Those are ignored on
    // purpose rather than by omission.
    if (flags & O_EXCL) { errno = ENOTSUP; return -1; }

    uint32_t f = (uint32_t)flags & 3u;
    if (flags & O_CREAT)  f |= UBIQOS_O_CREAT;
    if (flags & O_TRUNC)  f |= UBIQOS_O_TRUNC;
    if (flags & O_APPEND) f |= UBIQOS_O_APPEND;
    int32_t fd = ubiqos_open_flags(path, f);
    // A refusal says so itself, and does not need guessing at.
    if (fd == UBIQOS_FS_REFUSED) { errno = EACCES; return -1; }
    if (fd < 0) { errno = open_errno(path); return -1; }
    return fd;
}

int _close(int fd) { ubiqos_close(fd); return 0; }

int _lseek(int fd, int offset, int whence)
{
    int32_t n = ubiqos_seek(fd, offset, (uint32_t)whence);
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

// The attribute byte UbiqOS answers with is FAT's, and the one bit of it that
// a ported program actually reads is whether this is a directory: S_ISDIR is
// how anything that walks a tree decides to descend. Reporting every entry as a
// regular file makes such a program treat a directory as a file it cannot read,
// which looks like a broken filesystem rather than a missing translation.
int _stat(const char *path, struct stat *st)
{
    uint32_t size = 0;
    int32_t attr = ubiqos_fs_stat(path, &size);
    if (attr < 0) { errno = ENOENT; return -1; }
    st->st_mode = (attr & UBIQOS_ATTR_DIRECTORY) ? S_IFDIR : S_IFREG;
    st->st_size = (off_t)size;
    return 0;
}

int _unlink(const char *path)
{
    if (ubiqos_fs_remove(path) < 0) { errno = ENOENT; return -1; }
    return 0;
}

int _isatty(int fd) { (void)fd; return 1; }

// There is no getpid syscall and nothing here needs one; newlib only wants
// the symbol so that _kill has something to be refused for.
int _getpid(void) { return 1; }

int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }

void _exit(int code) { (void)code; ubiqos_exit(); for (;;) { } }

// Time, and it is worth saying plainly what kind: this machine has no clock to
// ask. There is no battery-backed anything on the board, and nothing has told it
// what year it is, so what the tick counter knows is how long it has been
// running and nothing else.
//
// So the epoch is the moment of boot. time() answers seconds since then, which
// is wrong for a date and right for everything ports actually do with it --
// seeding a generator, measuring how long something took, giving a temporary
// file a name nobody else has. A program that formats the answer will print a
// day in January 1970, which is at least obviously not today rather than
// plausibly the wrong day.
//
// The tick count is 32 bits of milliseconds, so it wraps after 49.7 days.
// Nothing here has run that long yet, and when something does, the fix is a
// 64-bit tick in the kernel rather than arithmetic here.
int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (!tv) { errno = EINVAL; return -1; }
    uint32_t ms = ubiqos_ticks_now();
    tv->tv_sec  = (time_t)(ms / 1000u);
    tv->tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
    return 0;
}

// clock() is this, and it is the one of the two that means what it says: a
// count of processor time from an arbitrary origin, which is exactly what a
// tick since boot is. The division is in 64 bits because CLOCKS_PER_SEC is the
// machine's to choose and multiplying milliseconds by it overflows otherwise.
clock_t _times(struct tms *buf)
{
    uint32_t ms = ubiqos_ticks_now();
    clock_t t = (clock_t)((uint64_t)ms * CLOCKS_PER_SEC / 1000u);
    if (buf) {
        buf->tms_utime = t;
        buf->tms_stime = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return t;
}

// Ported programs have a main; UbiqOS starts a module at module_main. Weak, so
// a module written for UbiqOS from the start can define its own and never have
// a main at all.
__attribute__((weak)) void module_main(int argc, char **argv)
{
    extern int main(int argc, char **argv);
    main(argc, argv);
}
