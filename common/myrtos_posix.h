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
#define O_APPEND 0x400     // the descriptor is placed at the end after opening

#define SEEK_SET MYRTOS_SEEK_SET
#define SEEK_CUR MYRTOS_SEEK_CUR
// SEEK_END is still refused by lseek, and the reason is no longer the missing
// stat: a raw descriptor does not carry the path, so there is nothing to ask
// about. fopen knows its path, so stdio's "a" mode works.
#define SEEK_END 2

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

// Existence, and the size if wanted. Enough of stat for what porting asks of
// it; there are no owners, times or permissions here to report.
struct stat {
    uint32_t st_size;
    uint32_t st_mode;
};
#define S_IFDIR   0x10
#define S_ISDIR(m) (((m) & S_IFDIR) != 0)

static inline int stat(const char *path, struct stat *st)
{
    uint32_t size = 0;
    int32_t attr = myrtos_fs_stat(path, &size);
    if (attr < 0) { errno = ENOENT; return -1; }
    if (st) { st->st_size = size; st->st_mode = (uint32_t)attr; }
    return 0;
}

// The mode argument is accepted and ignored: there are no permissions to set.
static inline int open(const char *path, int flags, ...)
{
    // Opening for reading something that is not there is an error, and since
    // stat arrived it can be said at the right moment instead of being
    // discovered by the first read.
    int writing = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0;
    uint32_t size = 0;
    int32_t attr = myrtos_fs_stat(path, &size);
    if (!writing && attr < 0) { errno = ENOENT; return -1; }

    // Truncation is the one flag that needs doing rather than allowing. Writing
    // does not shorten a file, so a shorter text over a longer one would leave
    // the old tail in place -- removing it first is what the `write` utility
    // has always done by hand.
    if (flags & O_TRUNC) { myrtos_fs_remove(path); size = 0; }

    int32_t fd = myrtos_open(path);
    if (fd < 0) { errno = ENOENT; return -1; }

    // Appending is a seek, now that there is something to seek to.
    if ((flags & O_APPEND) && size)
        myrtos_seek(fd, (int32_t)size, MYRTOS_SEEK_SET);
    return (int)fd;
}

static inline int pipe(int fds[2])
{
    int32_t f[2];
    if (myrtos_pipe(f) < 0) { errno = ENOSYS; return -1; }
    fds[0] = (int)f[0];
    fds[1] = (int)f[1];
    return 0;
}

static inline int dup(int fd)
{
    int32_t n = myrtos_dup((int32_t)fd, -1);
    if (n < 0) errno = EBADF;
    return (int)n;
}

static inline int dup2(int oldfd, int newfd)
{
    int32_t n = myrtos_dup((int32_t)oldfd, (int32_t)newfd);
    if (n < 0) errno = EBADF;
    return (int)n;
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
