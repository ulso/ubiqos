#ifndef UBIQOS_STDIO_H
#define UBIQOS_STDIO_H

#include "ubiqos_posix.h"
#include "ubiqos_string.h"
#include "ubiqos_ctype.h"
#include "ubiqos_stdlib.h"

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
#define UBIQOS_FOPEN_MAX 8
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
    int16_t  unget;     // one pushed-back character, -1 for none; scanf needs it
} FILE;

// Every piece of per-process state the C library keeps: errno, the streams, and
// what strtok remembers between calls. The program owes one line, exactly as a
// C library would have owed it:
//
//     UBIQOS_LIBC_DEFINE
//
// It cannot live in the header: -fno-common makes a tentative definition in
// several translation units a duplicate, and it cannot be static without every
// file getting its own errno.
#define UBIQOS_LIBC_DEFINE \
    UBIQOS_STRING_DEFINE \
    __thread int errno; \
    __thread FILE __ubiqos_files[UBIQOS_FOPEN_MAX];

extern __thread FILE __ubiqos_files[UBIQOS_FOPEN_MAX];

// The three standard streams are the first three slots, bound to their
// descriptors the first time anyone asks. Lazily, because there is no earlier
// moment to do it in.
static inline FILE *__ubiqos_std(int32_t fd)
{
    FILE *f = &__ubiqos_files[fd];
    if (!f->used) { f->fd = fd; f->unget = -1; f->used = 1; }
    return f;
}

#define stdin  __ubiqos_std(UBIQOS_STDIN)
#define stdout __ubiqos_std(UBIQOS_STDOUT)
#define stderr __ubiqos_std(UBIQOS_STDERR)

// --- opening ---------------------------------------------------------------
// "r", "w" and "a". Appending works because open can ask how long the file is
// and place the descriptor at the end; before stat existed it was refused,
// since a program that asks to append and is given the start of the file
// destroys it.
static inline FILE *fopen(const char *path, const char *mode)
{
    if (!mode || (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a')) {
        errno = EINVAL;
        return 0;
    }
    // "+" would need a stream that can turn round mid-way, and this one holds
    // one direction at a time -- which is what makes the buffer arithmetic
    // simple enough to be right. Refused rather than approximated.
    for (const char *m = mode; *m; m++)
        if (*m == '+') { errno = ENOSYS; return 0; }

    int writing = (mode[0] != 'r');
    int fd = open(path, mode[0] == 'w' ? (O_WRONLY | O_CREAT | O_TRUNC)
                      : mode[0] == 'a' ? (O_WRONLY | O_CREAT | O_APPEND)
                      : O_RDONLY);
    if (fd < 0) return 0;

    for (int i = 3; i < UBIQOS_FOPEN_MAX; i++) {
        FILE *f = &__ubiqos_files[i];
        if (f->used) continue;
        f->fd = (int32_t)fd;
        f->size = f->len = f->pos = 0;
        f->writing = (uint8_t)writing;
        f->eof = f->err = 0;
        f->buf = (uint8_t *)ubiqos_alloc_bulk(BUFSIZ);
        f->owned = f->buf ? 1 : 0;      // no buffer is slow, not broken
        f->size = f->buf ? BUFSIZ : 0;
        f->unget = -1;
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
    if (f->owned && f->buf) ubiqos_free(f->buf);
    f->buf = 0; f->owned = 0; f->used = 0; f->size = 0;
    return rc;
}

// The program's own buffer instead of ours, which is how a module avoids PSRAM
// or asks for a bigger one. Only before anything has been read or written.
static inline int setvbuf(FILE *f, char *buf, int mode, uint32_t size)
{
    (void)mode;
    if (!f || !f->used || f->len || f->pos) { errno = EINVAL; return -1; }
    if (f->owned && f->buf) ubiqos_free(f->buf);
    f->buf = (uint8_t *)buf;
    f->size = buf ? size : 0;
    f->owned = 0;
    return 0;
}

// --- reading ---------------------------------------------------------------
static inline int __ubiqos_fill(FILE *f)
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
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (f->pos >= f->len && __ubiqos_fill(f) < 0) {
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

// One character back, which is all the standard promises and all scanf needs:
// it reads one too many to know a number has ended, and must put it back.
static inline int ungetc(int c, FILE *f)
{
    if (!f || !f->used || c == EOF || f->unget >= 0) return EOF;
    f->unget = (int16_t)c;
    f->eof = 0;
    return c;
}


// --- printf ----------------------------------------------------------------
// One formatter, two destinations. A stream and a fixed buffer differ only in
// where a character goes, so the sink is the difference and nothing else is
// written twice. The count is kept whether or not the buffer can take it, which
// is what lets snprintf answer the standard's question: how long would it have
// been.
typedef struct {
    FILE     *f;
    char     *buf;
    uint32_t  cap;
    uint32_t  len;
} __ubiqos_sink;

static inline void __ubiqos_put(__ubiqos_sink *k, int c)
{
    k->len++;
    if (k->f) fputc(c, k->f);
    else if (k->buf && k->len < k->cap) k->buf[k->len - 1] = (char)c;
}

static inline void __ubiqos_pad(__ubiqos_sink *k, int n, char c)
{
    while (n-- > 0) __ubiqos_put(k, c);
}

static inline int vfprintf_sink(__ubiqos_sink *k, const char *fmt, __builtin_va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') { __ubiqos_put(k, *fmt); continue; }
        fmt++;

        int left = 0, zero = 0, plus = 0, space = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else break;
        }

        int width = 0;
        if (*fmt == '*') { width = __builtin_va_arg(ap, int); fmt++;
                           if (width < 0) { left = 1; width = -width; } }
        else while (isdigit((int)(uint8_t)*fmt)) width = width * 10 + (*fmt++ - '0');

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = __builtin_va_arg(ap, int); fmt++; }
            else while (isdigit((int)(uint8_t)*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }

        // long is the same width as int here, so l and h are read and ignored
        // rather than refused: code being ported is full of them.
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') fmt++;

        char tmp[12];
        int n = 0, base = 10, upper = 0, neg = 0;
        const char *str = 0;
        uint32_t v = 0;

        switch (*fmt) {
        case 0: return (int)k->len;
        case '%': __ubiqos_put(k, '%'); continue;
        case 'c': tmp[0] = (char)__builtin_va_arg(ap, int); str = tmp; n = 1; break;
        case 's': {
            str = __builtin_va_arg(ap, const char *);
            if (!str) str = "(null)";
            n = (int)(prec >= 0 ? strnlen(str, (uint32_t)prec) : strlen(str));
            break;
        }
        case 'p': base = 16; v = (uint32_t)(uintptr_t)__builtin_va_arg(ap, void *); break;
        case 'X': upper = 1; /* fall through */
        case 'x': base = 16; v = __builtin_va_arg(ap, uint32_t); break;
        case 'o': base = 8;  v = __builtin_va_arg(ap, uint32_t); break;
        case 'u': v = __builtin_va_arg(ap, uint32_t); break;
        case 'd': case 'i': {
            int sv = __builtin_va_arg(ap, int);
            neg = sv < 0;
            v = (uint32_t)(neg ? -(int64_t)sv : sv);
            break;
        }
        default: __ubiqos_put(k, '%'); __ubiqos_put(k, *fmt); continue;
        }

        if (!str) {                                   // a number, built backwards
            const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
            int i = (int)sizeof tmp;
            if (!v) tmp[--i] = '0';
            while (v) { tmp[--i] = set[v % (uint32_t)base]; v /= (uint32_t)base; }
            str = &tmp[i];
            n = (int)sizeof tmp - i;
        }

        char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
        int total = n + (sign ? 1 : 0);
        if (!left && !zero) __ubiqos_pad(k, width - total, ' ');
        if (sign) __ubiqos_put(k, sign);
        if (!left && zero) __ubiqos_pad(k, width - total, '0');
        for (int i = 0; i < n; i++) __ubiqos_put(k, str[i]);
        if (left) __ubiqos_pad(k, width - total, ' ');
    }
    return (int)k->len;
}

static inline int vfprintf(FILE *f, const char *fmt, __builtin_va_list ap)
{
    __ubiqos_sink k = { f, 0, 0, 0 };
    return vfprintf_sink(&k, fmt, ap);
}

static inline int vsnprintf(char *buf, uint32_t cap, const char *fmt, __builtin_va_list ap)
{
    __ubiqos_sink k = { 0, buf, cap, 0 };
    int n = vfprintf_sink(&k, fmt, ap);
    if (buf && cap) buf[k.len < cap ? k.len : cap - 1] = 0;
    return n;
}

static inline int fprintf(FILE *f, const char *fmt, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

static inline int printf(const char *fmt, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

static inline int snprintf(char *buf, uint32_t cap, const char *fmt, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

// sprintf with no ceiling is how buffers are overrun, so it is given the largest
// one that cannot be wrong about the caller's intent and no more.
static inline int sprintf(char *buf, const char *fmt, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, 0x7fffffffu, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

// --- scanf -----------------------------------------------------------------
// The mirror of the sink: a source that is either a stream or a string, and one
// character of pushback either way, because reading a number means reading one
// character too many.
typedef struct {
    FILE       *f;
    const char *s;
    uint32_t    i;
} __ubiqos_src;

static inline int __ubiqos_get(__ubiqos_src *r)
{
    if (r->f) return fgetc(r->f);
    return r->s[r->i] ? (int)(uint8_t)r->s[r->i++] : EOF;
}

static inline void __ubiqos_unget(__ubiqos_src *r, int c)
{
    if (c == EOF) return;
    if (r->f) ungetc(c, r->f);
    else if (r->i) r->i--;
}

static inline int vfscanf_src(__ubiqos_src *r, const char *fmt, __builtin_va_list ap)
{
    int filled = 0;

    for (; *fmt; fmt++) {
        if (isspace((int)(uint8_t)*fmt)) {          // any run of space matches any
            int c;
            while ((c = __ubiqos_get(r)) != EOF && isspace(c)) { }
            __ubiqos_unget(r, c);
            continue;
        }
        if (*fmt != '%') {
            int c = __ubiqos_get(r);
            if (c != *fmt) { __ubiqos_unget(r, c); return filled; }
            continue;
        }

        fmt++;
        int skip = 0, width = 0;
        if (*fmt == '*') { skip = 1; fmt++; }
        while (isdigit((int)(uint8_t)*fmt)) width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') fmt++;
        if (!width) width = 0x7fffffff;

        int c;
        if (*fmt == 'c') {
            c = __ubiqos_get(r);
            if (c == EOF) return filled ? filled : EOF;
            if (!skip) { *__builtin_va_arg(ap, char *) = (char)c; filled++; }
            continue;
        }

        while ((c = __ubiqos_get(r)) != EOF && isspace(c)) { }   // leading space
        if (c == EOF) return filled ? filled : EOF;

        if (*fmt == 's') {
            char *out = skip ? 0 : __builtin_va_arg(ap, char *);
            int n = 0;
            while (c != EOF && !isspace(c) && n < width) {
                if (out) out[n] = (char)c;
                n++;
                c = __ubiqos_get(r);
            }
            __ubiqos_unget(r, c);
            if (out) { out[n] = 0; filled++; }
            continue;
        }

        int base = *fmt == 'x' || *fmt == 'X' ? 16 : *fmt == 'o' ? 8 : 10;
        if (*fmt != 'd' && *fmt != 'i' && *fmt != 'u'
            && *fmt != 'x' && *fmt != 'X' && *fmt != 'o') {
            __ubiqos_unget(r, c);
            return filled;
        }

        int neg = 0, any = 0;
        long v = 0;
        if (c == '+' || c == '-') { neg = (c == '-'); c = __ubiqos_get(r); width--; }
        for (; c != EOF && width > 0; c = __ubiqos_get(r), width--) {
            int d;
            if (isdigit(c)) d = c - '0';
            else if (isxdigit(c)) d = tolower(c) - 'a' + 10;
            else break;
            if (d >= base) break;
            v = v * base + d;
            any = 1;
        }
        __ubiqos_unget(r, c);
        if (!any) return filled;                    // a conversion that matched nothing
        if (!skip) { *__builtin_va_arg(ap, int *) = (int)(neg ? -v : v); filled++; }
    }
    return filled;
}

static inline int sscanf(const char *str, const char *fmt, ...)
{
    __ubiqos_src r = { 0, str, 0 };
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = vfscanf_src(&r, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

static inline int fscanf(FILE *f, const char *fmt, ...)
{
    __ubiqos_src r = { f, 0, 0 };
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = vfscanf_src(&r, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

static inline int scanf(const char *fmt, ...)
{
    __ubiqos_src r = { stdin, 0, 0 };
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = vfscanf_src(&r, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

static inline int feof(FILE *f)   { return f && f->eof; }
static inline int ferror(FILE *f) { return f && f->err; }

#endif
