#ifndef MYRTOS_POSIX_H
#define MYRTOS_POSIX_H

#include "myrtos_abi.h"

// A thin POSIX face on the system calls, so that ordinary C which opens, reads
// and closes a file can be built here with its file-handling untouched.
//
// Inline functions rather than macros, and the difference matters: a macro
// rewrites every occurrence of the name, and this repository has a driver
// struct whose members are called open, read, write and close, and a C++ class
// with a write method. A macro shim breaks both. Functions cannot, and they may
// carry these names safely because modules are built -nostdlib and there is no
// C library here to collide with.
//
// What it does NOT do is pretend. A flag that cannot be honoured is refused
// rather than dropped: a program that asks for O_APPEND and is quietly given a
// plain write would put its output at the start of the file, and that is the
// kind of fault that costs an evening. See the table below.

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40      // implied: the first write brings the file into being
#define O_TRUNC  0x200     // honoured by removing the file before opening it
#define O_APPEND 0x400     // REFUSED: needs the file's length, and there is no stat

#define SEEK_SET MYRTOS_SEEK_SET
#define SEEK_CUR MYRTOS_SEEK_CUR
#define SEEK_END 2         // REFUSED by lseek, for the same reason

#define ENOENT  2
#define EBADF   9
#define EINVAL 22
#define ENOSYS 38

// errno is per-process writable state, which a shareable module may not have as
// a static -- so it is thread-local, which is how a module keeps anything of its
// own. Declared here and defined once by the program:
//
//     __thread int errno;
//
// exactly as a C library would define it for you.
extern __thread int errno;

// The mode argument is accepted and ignored: there are no permissions to set.
static inline int open(const char *path, int flags, ...)
{
    if (flags & O_APPEND) { errno = ENOSYS; return -1; }

    // Truncation is the one flag that needs doing rather than allowing. Writing
    // does not shorten a file, so a shorter text over a longer one would leave
    // the old tail in place -- removing it first is what the `write` utility
    // has always done by hand.
    if (flags & O_TRUNC) myrtos_fs_remove(path);

    int32_t fd = myrtos_open(path);
    if (fd < 0) { errno = ENOENT; return -1; }
    return (int)fd;
}

static inline int close(int fd)
{
    return myrtos_close((int32_t)fd) < 0 ? (errno = EBADF, -1) : 0;
}

// Nought is end of file and a negative is an error, as POSIX has it. Note that
// opening does not check a file exists -- there are no flags yet to say whether
// a write should create -- so a missing file is discovered here.
static inline int32_t read(int fd, void *buf, uint32_t len)
{
    int32_t n = myrtos_read((int32_t)fd, buf, len);
    if (n < 0) errno = ENOENT;
    return n;
}

static inline int32_t write(int fd, const void *buf, uint32_t len)
{
    int32_t n = myrtos_write((int32_t)fd, buf, len);
    if (n < 0) errno = EBADF;
    return n;
}

static inline int32_t lseek(int fd, int32_t offset, int whence)
{
    if (whence == SEEK_END) { errno = ENOSYS; return -1; }
    int32_t n = myrtos_seek((int32_t)fd, offset, (uint32_t)whence);
    if (n < 0) errno = EINVAL;
    return n;
}

#define STDIN_FILENO  MYRTOS_STDIN
#define STDOUT_FILENO MYRTOS_STDOUT
#define STDERR_FILENO MYRTOS_STDERR

#endif
