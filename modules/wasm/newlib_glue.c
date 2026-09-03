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
// Doubles are eight bytes and wasm3 stores them, so eight is the alignment to
// keep rather than four.
#define SBRK_ALIGN 8u

static char  *heap_base;
static uint32_t heap_size;
static uint32_t heap_used;

int wasm_heap_init(uint32_t bytes)
{
    // From PSRAM: myrtos_alloc takes the SRAM pool, which is 32 kB in total and
    // has under four free. The bulk pool is eight megabytes and is where a
    // module that is not real-time belongs anyway.
    heap_base = (char *)myrtos_alloc_bulk(bytes);
    if (!heap_base)
        return -1;

    // And the base itself, for the same reason: an aligned step from an odd
    // start is still odd.
    uint32_t skew = (uint32_t)((uintptr_t)heap_base & (SBRK_ALIGN - 1));
    if (skew) { heap_base += SBRK_ALIGN - skew; bytes -= SBRK_ALIGN - skew; }

    heap_size = bytes;
    heap_used = 0;
    return 0;
}

// Eight-byte aligned, and that is not a detail.
//
// The first version advanced the break by exactly what was asked for, and a
// request that is not a multiple of eight leaves every later block misaligned.
// newlib's malloc passes that straight on, wasm3 puts structures there, and a
// RISC-V without misaligned-access support traps on the first load -- which is
// what mcause 4 was. Where it happened moved when heap sizes changed, because
// which allocation lands askew depends on what came before it.
//
// Doubles are eight bytes and wasm3 stores them, so eight is the alignment to
// keep rather than four.
// What the heap covers, so the host can tell a pointer worth dereferencing from
// one that is not. wasm3 puts export names and error messages here.
char *wasm_heap_extent(unsigned long *size)
{
    if (size) *size = heap_size;
    return heap_base;
}

void *_sbrk(int incr)
{
    if (!heap_base) { errno = ENOMEM; return (void *)-1; }

    // Giving memory back is allowed: newlib's malloc trims the top of the heap
    // and refusing it made a shrink look like an error.
    if (incr < 0) {
        uint32_t give = (uint32_t)(-incr);
        heap_used = give > heap_used ? 0 : heap_used - give;
        return heap_base + heap_used;
    }

    uint32_t want = ((uint32_t)incr + (SBRK_ALIGN - 1)) & ~(SBRK_ALIGN - 1);
    if (heap_used + want > heap_size) { errno = ENOMEM; return (void *)-1; }

    char *p = heap_base + heap_used;
    heap_used += want;
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
