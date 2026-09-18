#ifndef UBIQOS_STRING_H
#define UBIQOS_STRING_H

#include <stdint.h>

// string.h and memory.h, as far as a module needs them.
//
// All of it is inline and almost none of it holds state, which is what a
// shareable module requires -- so there is nothing here for the
// position-independence check to object to, and a module pays only for the
// functions it calls.
//
// strtok is the exception, and it is the one that catches people out. Written
// the usual way it keeps a `static char *` between calls, and check_module.py
// refuses the module outright: "writable section .sbss is present". A program
// that brings its own strtok will be refused for exactly that reason.
//
// So the state is thread-local, which means the program has to define it --
// UBIQOS_LIBC_DEFINE does, along with errno and the streams. strtok_r needs
// none of it, keeping the state in the caller's own variable, and is the better
// function anyway.

static inline void *memcpy(void *d, const void *s, uint32_t n)
{
    uint8_t *p = (uint8_t *)d; const uint8_t *q = (const uint8_t *)s;
    while (n--) *p++ = *q++;
    return d;
}

// Overlapping ranges copied the way round that survives them.
static inline void *memmove(void *d, const void *s, uint32_t n)
{
    uint8_t *p = (uint8_t *)d; const uint8_t *q = (const uint8_t *)s;
    if (p == q || !n) return d;
    if (p < q) { while (n--) *p++ = *q++; return d; }
    p += n; q += n;
    while (n--) *--p = *--q;
    return d;
}

static inline void *memset(void *d, int c, uint32_t n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

static inline int memcmp(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}

static inline void *memchr(const void *s, int c, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    while (n--) { if (*p == (uint8_t)c) return (void *)p; p++; }
    return 0;
}

static inline uint32_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (uint32_t)(p - s);
}

static inline uint32_t strnlen(const char *s, uint32_t n)
{
    uint32_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}

static inline char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) { }
    return r;
}

// The standard's own strncpy: it pads with NULs and does NOT terminate when the
// source fills the buffer. Kept faithful, surprises and all, because code being
// ported was written against that and quietly changing it would be worse.
static inline char *strncpy(char *d, const char *s, uint32_t n)
{
    char *r = d;
    while (n && *s) { *d++ = *s++; n--; }
    while (n--) *d++ = 0;
    return r;
}

static inline int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static inline int strncmp(const char *a, const char *b, uint32_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static inline char *strcat(char *d, const char *s)
{
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) { }
    return r;
}

static inline char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

static inline char *strrchr(const char *s, int c)
{
    const char *last = 0;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

static inline char *strstr(const char *h, const char *n)
{
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return 0;
}

// The state strtok keeps between calls. Thread-local because a shareable module
// may not have writable data, and per-process is what it should have been all
// along: two processes tokenising at once would otherwise tread on each other.
extern __thread char *__ubiqos_strtok;

// Defined by UBIQOS_LIBC_DEFINE. Here as well, for a program that wants the
// string functions without stdio.
#define UBIQOS_STRING_DEFINE __thread char *__ubiqos_strtok;

// The reentrant one, which needs no hidden state at all: the caller keeps it.
static inline char *strtok_r(char *s, const char *sep, char **save)
{
    if (!s) s = *save;
    if (!s) return 0;
    while (*s && strchr(sep, *s)) s++;          // leading separators
    if (!*s) { *save = 0; return 0; }
    char *start = s;
    while (*s && !strchr(sep, *s)) s++;
    if (*s) { *s = 0; *save = s + 1; } else *save = 0;
    return start;
}

static inline char *strtok(char *s, const char *sep)
{
    return strtok_r(s, sep, &__ubiqos_strtok);
}

#endif
