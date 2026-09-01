#ifndef MYRTOS_STRING_H
#define MYRTOS_STRING_H

#include <stdint.h>

// string.h and memory.h, as far as a module needs them.
//
// All of it is inline and none of it holds state, which is what a shareable
// module requires -- so there is nothing here for the position-independence
// check to object to, and a module pays only for the functions it calls.
//
// strtok is the exception and is in myrtos_stdio.h, because its state has to be
// thread-local and that means the program has to define it.

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

#endif
