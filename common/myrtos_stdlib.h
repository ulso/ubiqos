#ifndef MYRTOS_STDLIB_H
#define MYRTOS_STDLIB_H

#include "myrtos_abi.h"
#include "myrtos_string.h"
#include "myrtos_ctype.h"

// stdlib.h: the allocator, the string-to-number conversions, and exit.
//
// malloc asks PSRAM first. A process's own pool is four kilobytes by default --
// enough for a stack and a few locals, not for what code being ported expects
// of malloc -- while the bulk pool has megabytes and is exactly what it is for.
// A machine without PSRAM falls back to the process pool, which is the right
// order: prefer the roomy one, but do not fail for want of it.
//
// Eight bytes go in front of every block to remember its size. The kernel knows
// it too, but not through any call a module can make, and realloc cannot work
// without it. Eight rather than four keeps what follows eight-byte aligned.

static inline void *malloc(uint32_t n)
{
    uint32_t *p = (uint32_t *)myrtos_alloc_bulk(n + 8);
    if (!p) p = (uint32_t *)myrtos_alloc(n + 8);
    if (!p) return 0;
    p[0] = n;
    return (void *)(p + 2);
}

static inline void free(void *ptr)
{
    if (ptr) myrtos_free((uint32_t *)ptr - 2);
}

static inline void *calloc(uint32_t nmemb, uint32_t size)
{
    uint32_t n = nmemb * size;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

static inline void *realloc(void *ptr, uint32_t n)
{
    if (!ptr) return malloc(n);
    uint32_t old = ((uint32_t *)ptr)[-2];
    if (n <= old) return ptr;                 // shrinking in place is allowed
    void *q = malloc(n);
    if (!q) return 0;                         // the old block is still the
    memcpy(q, ptr, old);                      // caller's, as the standard says
    free(ptr);
    return q;
}

// --- numbers ---------------------------------------------------------------
// strtol without the locale and without ERANGE: what overflows wraps, which is
// what the parsing in this system has always done. Base 0 recognises 0x and 0.
static inline long strtol(const char *s, char **end, int base)
{
    const char *p = s;
    while (isspace((int)(uint8_t)*p)) p++;

    int neg = 0;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (*p == '0') ? 8 : 10;
    }

    long v = 0;
    const char *digits = p;
    for (;; p++) {
        int c = (int)(uint8_t)*p, d;
        if (isdigit(c)) d = c - '0';
        else if (isalpha(c)) d = tolower(c) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    // Nothing consumed means nothing converted, and end must say so.
    if (end) *end = (char *)(p == digits ? s : p);
    return neg ? -v : v;
}

static inline int atoi(const char *s) { return (int)strtol(s, 0, 10); }
static inline long atol(const char *s) { return strtol(s, 0, 10); }
static inline int abs(int v) { return v < 0 ? -v : v; }
static inline long labs(long v) { return v < 0 ? -v : v; }

static inline void exit(int status) { (void)status; myrtos_exit(); }

#endif
