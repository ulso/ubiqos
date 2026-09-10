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

// This header IS the C library for a module that does not link one, so it must
// not be mixed with a module that does. The names below are the same as
// <fcntl.h>'s and the numbers are not: myrtos took Linux's -- O_CREAT 0x40,
// O_TRUNC 0x200, O_APPEND 0x400 -- and newlib took BSD's, 0x200, 0x400 and
// 0x008. Two honest Unix lineages that disagree above the access mode.
//
// Mixing the two already fails to compile, on struct stat and on open. It fails
// in forty-six diagnostics that never mention the flags, so this says it once
// and first. A module built NEWLIB wants <fcntl.h> and newlib's own open;
// common/myrtos_syscalls.c translates the numbers on the way to the kernel.
#ifdef O_RDONLY
#error "myrtos_posix.h is for a module built without a C library, and this one has <fcntl.h>. Use it and newlib's open(); common/myrtos_syscalls.c translates the flags."
#endif

// Aliases, not translations: the kernel uses POSIX's own numbers and acts on
// them itself, so nothing here has to arrange afterwards what the flag asked
// for. That is the difference between a flag and a convention.
#define O_RDONLY MYRTOS_O_RDONLY
#define O_WRONLY MYRTOS_O_WRONLY
#define O_RDWR   MYRTOS_O_RDWR
#define O_CREAT  MYRTOS_O_CREAT
#define O_TRUNC  MYRTOS_O_TRUNC
#define O_APPEND MYRTOS_O_APPEND

#define SEEK_SET MYRTOS_SEEK_SET
#define SEEK_CUR MYRTOS_SEEK_CUR
// SEEK_END works on a raw descriptor now. It used to be refused because a
// descriptor did not carry the path and there was nothing to ask about the
// length of; the kernel keeps the path it was opened with, so SYS_SEEK sends
// this one case to the file server and gets an answer.
#define SEEK_END MYRTOS_SEEK_END

#define ENOENT  2
#define EBADF   9
#define EISDIR 21
#define EACCES 13
#define EINVAL 22
#define EMFILE 24
// 38 is Linux's, and newlib's is 88. The two libraries here cannot be included
// together -- the guard above this says so -- but the divergence is the same
// one the open flags have, and worth knowing before somebody compares them.
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
// Everything else is the kernel's: it refuses a missing file, empties one for
// O_TRUNC and positions the descriptor for O_APPEND, all before this returns.
// Why an open failed, worked out afterwards rather than reported by the kernel,
// which answers -1 to every cause alike. The same reasoning -- and the same
// three answers -- as common/myrtos_syscalls.c gives the other library here:
// "not there" and "there and it still did not open" are different problems, and
// a caller told ENOENT for the second will create a file it should not.
//
// EMFILE for the last case is the likeliest of what remains rather than a fact.
static inline int myrtos_open_errno(const char *path)
{
    uint32_t size = 0;
    int32_t attr = myrtos_fs_stat(path, &size);
    if (attr < 0) return ENOENT;
    if (attr & MYRTOS_ATTR_DIRECTORY) return EISDIR;
    return EMFILE;
}

static inline int open(const char *path, int flags, ...)
{
    int32_t fd = myrtos_open_flags(path, (uint32_t)flags);
    // A refusal says so itself, and does not need guessing at.
    if (fd == MYRTOS_FS_REFUSED) { errno = EACCES; return -1; }
    if (fd < 0) { errno = myrtos_open_errno(path); return -1; }
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
