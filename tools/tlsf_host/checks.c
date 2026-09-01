#include "../../kernel/tlsf.h"
#include "checks.h"
#include <stdint.h>

// The allocator's own tests, written once and run twice: on the Mac, where
// lldb is, and on qemu-system-riscv32, where the word is the board's width.
// That second run is the whole point -- the host cannot show a fault in the
// size arithmetic, because its block header is twice the size.

// Aligned, as the kernel's own myrtos_heap is: the control block is cast
// straight onto the front of this, and a misaligned one traps on RISC-V. The
// first qemu run of this harness hung for exactly that reason, which looks
// identical to a hang because an unhandled trap with no vector loops for ever.
static uint8_t arena[40 * 1024] __attribute__((aligned(8)));

// The kernel's wrapper, copied from scheduler.c. The board goes through this,
// so a harness that called TLSF directly would be comparing a different thing.
typedef struct alloc_hdr {
    struct alloc_hdr *next;
    uint32_t owner, size, magic;
} alloc_hdr_t;
#define ALLOC_MAGIC 0x4d454d21u

static alloc_hdr_t *allocs;

static void *wrap_alloc(tlsf_pool_t pool, uint32_t size) {
    alloc_hdr_t *h = myrtos_tlsf_malloc(pool, size + sizeof(alloc_hdr_t));
    if (!h) return 0;
    h->size = size; h->magic = ALLOC_MAGIC; h->owner = 1;
    h->next = allocs; allocs = h;
    return (void*)(h + 1);
}

static int wrap_free(tlsf_pool_t pool, void *ptr) {
    if (!ptr) return 0;
    alloc_hdr_t *h = ((alloc_hdr_t*)ptr) - 1;
    if (h->magic != ALLOC_MAGIC || h->owner != 1) return -1;
    alloc_hdr_t **pp = &allocs;
    while (*pp) { if (*pp == h) { *pp = h->next; h->next = 0; goto found; } pp = &(*pp)->next; }
    return -1;
found:
    h->magic = 0;
    myrtos_tlsf_free(pool, h);
    return 0;
}

static void line(const char *what, unsigned long v) {
    out_str("  "); out_str(what); out_str(" "); out_u32(v); out_str("\n");
}

int tlsf_checks(void) {
    int bad = 0;
    out_str("  header is "); out_u32(sizeof(void*) * 8); out_str(" bit\n");

    out_str("  creating...\n");
    tlsf_pool_t pool = myrtos_tlsf_create(arena, sizeof arena);
    if (!pool) { out_str("  create failed\n"); return 1; }
    out_str("  created\n");

    unsigned long start = myrtos_tlsf_largest_free(pool);
    out_str("  first walk done\n");
    line("largest free at the start:", start);

    void *a = wrap_alloc(pool, 1024);
    unsigned long held = myrtos_tlsf_largest_free(pool);
    int rc = wrap_free(pool, a);
    unsigned long back = myrtos_tlsf_largest_free(pool);
    void *b = wrap_alloc(pool, 1024);

    line("with 1024 held:", held);
    line("after freeing it:", back);
    line("free returned:", (unsigned long)rc);
    if (back != start) { out_str("  POOL DID NOT COME BACK\n"); bad = 1; }
    if (a != b)        { out_str("  SECOND 1024 LANDED ELSEWHERE\n"); bad = 1; }
    wrap_free(pool, b);

    unsigned rnd = 12345;
    void *hp[32]; unsigned long hn[32];
    for (int i = 0; i < 32; i++) { hp[i] = 0; hn[i] = 0; }
    unsigned long taken = 0, freed = 0, refused = 0;

    for (int k = 0; k < 200000; k++) {
        rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
        int slot = rnd % 32;
        if (hp[slot]) {
            unsigned char *p = hp[slot];
            for (unsigned long i = 0; i < hn[slot]; i++)
                if (p[i] != (unsigned char)((uintptr_t)p + i)) {
                    out_str("  CORRUPTION after "); out_u32((unsigned long)k);
                    out_str(" operations\n");
                    return 1;
                }
            wrap_free(pool, hp[slot]); hp[slot] = 0; freed++;
            continue;
        }
        unsigned long n = 8 + (rnd >> 8) % 512;
        unsigned char *p = wrap_alloc(pool, (uint32_t)n);
        if (!p) { refused++; continue; }
        for (unsigned long i = 0; i < n; i++) p[i] = (unsigned char)((uintptr_t)p + i);
        hp[slot] = p; hn[slot] = n; taken++;
    }
    for (int i = 0; i < 32; i++) if (hp[i]) { wrap_free(pool, hp[i]); freed++; }

    line("taken:", taken); line("freed:", freed); line("refused:", refused);
    unsigned long end = myrtos_tlsf_largest_free(pool);
    line("largest free at the end:", end);
    if (end != start) { out_str("  POOL SHRANK\n"); bad = 1; }

    out_str(bad ? "  FAILED\n" : "  passed\n");
    return bad;
}
