#ifndef MYRTOS_STDIO_H
#define MYRTOS_STDIO_H

#include "myrtos_posix.h"

// stdio, as far as it can honestly go here.
//
// The FILE objects live in thread-local storage and the buffers do not, and
// that split is the whole design. A FILE is twenty-odd bytes, so an array of
// them costs a module almost nothing and -- more importantly -- makes stdin,
// stdout and stderr addressable without any initialisation at all. Modules have
// no constructor phase: nothing runs before module_main, so objects that had to
// be built first could not be printed to.
//
// A buffer is another matter. It is charged to the process whether it opens a
// file or not, and a single 512-byte buffer is an eighth of a module's default
// four kilobytes. So buffers come from PSRAM, of which there are about eight
// megabytes, and that is exactly the bulk data that pool exists for. A stream
// whose buffer cannot be had still works, one byte at a time.
//
// Buffering earns more here than on Unix rather than less: every write to the
// card is a message to the filesystem server and a walk through the FAT.

#define BUFSIZ 512
#define MYRTOS_FOPEN_MAX 8
#define EOF (-1)

typedef struct {
    int32_t  fd;
    uint8_t *buf;
    uint32_t size;      // what the buffer holds
    uint32_t len;       // bytes read into it, or bytes waiting to go out
    uint32_t pos;       // how far through them we are
    uint8_t  used;      // this slot is a stream
    uint8_t  writing;   // one direction at a time; see the note on modes
    uint8_t  owned;     // the buffer is ours to give back
    uint8_t  eof, err;
} FILE;

// The program owes one line, exactly as a C library would have owed it:
//
//     MYRTOS_STDIO_DEFINE
//
// It cannot live in the header: -fno-common makes a tentative definition in
// several translation units a duplicate, and it cannot be static without every
// file getting its own errno.
#define MYRTOS_STDIO_DEFINE \
    __thread int errno; \
    __thread FILE __myrtos_files[MYRTOS_FOPEN_MAX];

extern __thread FILE __myrtos_files[MYRTOS_FOPEN_MAX];

// The three standard streams are the first three slots, bound to their
// descriptors the first time anyone asks. Lazily, because there is no earlier
// moment to do it in.
static inline FILE *__myrtos_std(int32_t fd)
{
    FILE *f = &__myrtos_files[fd];
    if (!f->used) { f->fd = fd; f->used = 1; }
    return f;
}

#define stdin  __myrtos_std(MYRTOS_STDIN)
#define stdout __myrtos_std(MYRTOS_STDOUT)
#define stderr __myrtos_std(MYRTOS_STDERR)

// --- opening ---------------------------------------------------------------
// "r" and "w" only. "a" needs the file's length to start at the end, and "+"
// needs a stream that can turn round mid-way; both want a stat this filesystem
// does not have. They are refused rather than approximated, because a program
// that asks to append and is given the beginning of the file destroys it.
static inline FILE *fopen(const char *path, const char *mode)
{
    if (!mode || (mode[0] != 'r' && mode[0] != 'w')) { errno = EINVAL; return 0; }
    for (const char *m = mode; *m; m++)
        if (*m == '+' || *m == 'a') { errno = ENOSYS; return 0; }

    int writing = (mode[0] == 'w');
    int fd = open(path, writing ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY);
    if (fd < 0) return 0;

    for (int i = 3; i < MYRTOS_FOPEN_MAX; i++) {
        FILE *f = &__myrtos_files[i];
        if (f->used) continue;
        f->fd = (int32_t)fd;
        f->size = f->len = f->pos = 0;
        f->writing = (uint8_t)writing;
        f->eof = f->err = 0;
        f->buf = (uint8_t *)myrtos_alloc_bulk(BUFSIZ);
        f->owned = f->buf ? 1 : 0;      // no buffer is slow, not broken
        f->size = f->buf ? BUFSIZ : 0;
        f->used = 1;
        return f;
    }
    close(fd);
    errno = ENOSYS;                     // no free stream
    return 0;
}

static inline int fflush(FILE *f)
{
    if (!f || !f->used) { errno = EBADF; return EOF; }
    if (!f->writing || !f->len) return 0;
    int32_t n = write(f->fd, f->buf, f->len);
    f->len = 0;
    if (n < 0) { f->err = 1; return EOF; }
    return 0;
}

static inline int fclose(FILE *f)
{
    if (!f || !f->used) { errno = EBADF; return EOF; }
    int rc = fflush(f);
    close(f->fd);
    if (f->owned && f->buf) myrtos_free(f->buf);
    f->buf = 0; f->owned = 0; f->used = 0; f->size = 0;
    return rc;
}

// The program's own buffer instead of ours, which is how a module avoids PSRAM
// or asks for a bigger one. Only before anything has been read or written.
static inline int setvbuf(FILE *f, char *buf, int mode, uint32_t size)
{
    (void)mode;
    if (!f || !f->used || f->len || f->pos) { errno = EINVAL; return -1; }
    if (f->owned && f->buf) myrtos_free(f->buf);
    f->buf = (uint8_t *)buf;
    f->size = buf ? size : 0;
    f->owned = 0;
    return 0;
}

// --- reading ---------------------------------------------------------------
static inline int __myrtos_fill(FILE *f)
{
    if (f->writing || f->eof || f->err) return -1;
    f->pos = f->len = 0;
    if (!f->buf) return -1;                 // unbuffered: fgetc reads direct
    int32_t n = read(f->fd, f->buf, f->size);
    if (n < 0) { f->err = 1; return -1; }
    if (n == 0) { f->eof = 1; return -1; }
    f->len = (uint32_t)n;
    return 0;
}

static inline int fgetc(FILE *f)
{
    if (!f || !f->used || f->writing) { errno = EBADF; return EOF; }
    if (f->pos >= f->len && __myrtos_fill(f) < 0) {
        if (f->buf) return EOF;
        uint8_t c;                          // no buffer, so one byte at a time
        int32_t n = read(f->fd, &c, 1);
        if (n <= 0) { if (n == 0) f->eof = 1; else f->err = 1; return EOF; }
        return c;
    }
    return f->buf[f->pos++];
}

static inline uint32_t fread(void *ptr, uint32_t size, uint32_t nmemb, FILE *f)
{
    uint8_t *p = (uint8_t *)ptr;
    uint32_t want = size * nmemb, got = 0;
    while (got < want) {
        int c = fgetc(f);
        if (c == EOF) break;
        p[got++] = (uint8_t)c;
    }
    return size ? got / size : 0;
}

// Up to n-1 bytes, stopping after a newline, which is kept. Null on end of file
// with nothing read, as everywhere else.
static inline char *fgets(char *s, int n, FILE *f)
{
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return 0;
    s[i] = 0;
    return s;
}

// --- writing ---------------------------------------------------------------
static inline int fputc(int c, FILE *f)
{
    if (!f || !f->used) { errno = EBADF; return EOF; }
    f->writing = 1;
    uint8_t b = (uint8_t)c;
    if (!f->buf) return write(f->fd, &b, 1) == 1 ? c : EOF;
    f->buf[f->len++] = b;
    if (f->len >= f->size && fflush(f) == EOF) return EOF;
    return c;
}

static inline uint32_t fwrite(const void *ptr, uint32_t size, uint32_t nmemb, FILE *f)
{
    const uint8_t *p = (const uint8_t *)ptr;
    uint32_t want = size * nmemb, put = 0;
    while (put < want)
        if (fputc(p[put], f) == EOF) break; else put++;
    return size ? put / size : 0;
}

static inline int fputs(const char *s, FILE *f)
{
    while (*s) if (fputc(*s++, f) == EOF) return EOF;
    return 0;
}

static inline int puts(const char *s)
{
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

// --- position --------------------------------------------------------------
// Whatever is in the buffer is thrown away, and for a reader that means the
// descriptor is ahead of where the program thinks it is -- so the seek is
// absolute and the buffer is emptied, never adjusted.
static inline int fseek(FILE *f, int32_t offset, int whence)
{
    if (!f || !f->used) { errno = EBADF; return -1; }
    if (f->writing && fflush(f) == EOF) return -1;
    if (whence == SEEK_CUR) {           // undo what the buffer had read ahead
        int32_t here = lseek(f->fd, 0, SEEK_CUR);
        if (here < 0) return -1;
        offset += here - (int32_t)(f->len - f->pos);
        whence = SEEK_SET;
    }
    if (lseek(f->fd, offset, whence) < 0) return -1;
    f->len = f->pos = 0;
    f->eof = 0;
    return 0;
}

static inline int32_t ftell(FILE *f)
{
    if (!f || !f->used) { errno = EBADF; return -1; }
    int32_t here = lseek(f->fd, 0, SEEK_CUR);
    if (here < 0) return -1;
    if (f->writing) return here + (int32_t)f->len;
    return here - (int32_t)(f->len - f->pos);
}

static inline int feof(FILE *f)   { return f && f->eof; }
static inline int ferror(FILE *f) { return f && f->err; }

#endif
