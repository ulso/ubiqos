// The allocator, on the Mac.
//
// tlsf.c has no hardware in it beyond the critical sections, which the header
// beside this file stands in for. Running it here costs a second instead of a
// flash, a reboot and a serial session -- and lldb works.
//
// ONE CAVEAT, and it matters: the host is 64-bit, so block_header_t is twice
// the size it is on the board and every block is aligned differently. A fault
// in the list logic will show here. A fault in the size arithmetic might not,
// or might show as a different one.

#include "../../kernel/tlsf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The board's own pool size, so the internal layout matches.
static uint8_t arena[40 * 1024];

// The kernel's wrapper, copied from scheduler.c. The board goes through this
// and the first version of this harness did not, which is comparing two
// different things -- the fault could as easily be here as in the allocator.
typedef struct alloc_hdr {
    struct alloc_hdr *next;
    uint32_t owner;
    uint32_t size;
    uint32_t magic;
} alloc_hdr_t;
#define ALLOC_MAGIC 0x4d454d21u

static alloc_hdr_t *allocs;              // the process's list, of which there is one

static void *wrap_alloc(tlsf_pool_t pool, uint32_t size) {
    alloc_hdr_t *h = myrtos_tlsf_malloc(pool, size + sizeof(alloc_hdr_t));
    if (!h) return NULL;
    h->size = size;
    h->magic = ALLOC_MAGIC;
    h->owner = 1;
    h->next = allocs;
    allocs = h;
    return (void*)(h + 1);
}

static int wrap_free(tlsf_pool_t pool, void *ptr) {
    if (!ptr) return 0;
    alloc_hdr_t *h = ((alloc_hdr_t*)ptr) - 1;
    if (h->magic != ALLOC_MAGIC) return -1;
    if (h->owner != 1) return -1;
    alloc_hdr_t **pp = &allocs;
    while (*pp) {
        if (*pp == h) { *pp = h->next; h->next = NULL; goto found; }
        pp = &(*pp)->next;
    }
    return -1;
found:
    h->magic = 0;
    myrtos_tlsf_free(pool, h);
    return 0;
}

static void report(const char *what, size_t v) { printf("  %-34s %zu\n", what, v); }

int main(void)
{
    tlsf_pool_t pool = myrtos_tlsf_create(arena, sizeof arena);
    if (!pool) { printf("create failed\n"); return 1; }

    size_t start = myrtos_tlsf_largest_free(pool);
    report("largest free at the start", start);

    // The board asks through a wrapper that adds an eight-byte header, so the
    // request that actually reaches TLSF is 1032 and not 1024. The exact size
    // decides which size class is searched and how the block is split, so the
    // difference is not cosmetic.
    void *a = wrap_alloc(pool, 1024);
    size_t held = myrtos_tlsf_largest_free(pool);
    int rc = wrap_free(pool, a);
    size_t back = myrtos_tlsf_largest_free(pool);
    void *b = wrap_alloc(pool, 1024);

    report("with 1024 held", held);
    report("after freeing it", back);
    printf("  %-34s %s (free returned %d)\n", "did the pool come back?", back == start ? "yes" : "NO", rc);
    printf("  %-34s %s\n", "same address the second time?", a == b ? "yes" : "no");
    wrap_free(pool, b);

    // And a longer run, to see whether it merely leaks or also corrupts.
    unsigned rnd = 12345;
    void *held_p[32] = {0};
    size_t held_n[32] = {0};
    size_t taken = 0, freed = 0, refused = 0;
    for (int k = 0; k < 200000; k++) {
        rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
        int slot = rnd % 32;
        if (held_p[slot]) {
            unsigned char *p = held_p[slot];
            for (size_t i = 0; i < held_n[slot]; i++)
                if (p[i] != (unsigned char)((uintptr_t)p + i)) {
                    printf("  CORRUPTION at %p byte %zu after %d operations\n", p, i, k);
                    return 1;
                }
            wrap_free(pool, held_p[slot]);
            held_p[slot] = 0;
            freed++;
            continue;
        }
        size_t n = 8 + (rnd >> 8) % 512;   // as the wrapper would ask
        unsigned char *p = wrap_alloc(pool, (uint32_t)n);
        if (!p) { refused++; continue; }
        for (size_t i = 0; i < n; i++) p[i] = (unsigned char)((uintptr_t)p + i);
        held_p[slot] = p;
        held_n[slot] = n;
        taken++;
    }
    for (int i = 0; i < 32; i++) if (held_p[i]) { wrap_free(pool, held_p[i]); freed++; }

    printf("\n  %zu taken, %zu freed, %zu refused\n", taken, freed, refused);
    report("largest free at the end", myrtos_tlsf_largest_free(pool));
    printf("  %s\n", myrtos_tlsf_largest_free(pool) == start ? "passed" : "FAILED");
    return 0;
}
