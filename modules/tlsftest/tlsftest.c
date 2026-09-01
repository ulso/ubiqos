#include "../../common/myrtos_stdio.h"

// tlsftest -- how long TLSF takes, and whether it survives being used.
//
//     tlsftest          the SRAM pool, which is what real-time modules get
//     tlsftest bulk     PSRAM, where the big allocations live
//
// Two questions, and they need different tests. Timing wants the same thing
// done many times over; survival wants many different things done in an order
// nobody chose. Doing both at once measures neither.

MYRTOS_LIBC_DEFINE
MYRTOS_MEM_SIZE(8192);

// mcycle, not the millisecond tick: an allocation is supposed to be O(1), which
// at 125 MHz means the tick cannot see it at all. Everything here runs in
// machine mode, so the counter is readable directly.
static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    return c;
}

// The counters come up inhibited on this core, so mcycle reads the same value
// every time and every measurement is zero. That is what the first run of this
// test reported, and a zero that means "not counting" looks exactly like a zero
// that means "too fast to see".
static inline void start_counting(void)
{
    __asm__ volatile("csrw 0x320, zero");    // mcountinhibit
}

// Thread-local, not a plain static: a shareable module may have no writable
// data, and check_module.py refused this file until it said so.
static __thread bool use_bulk;

static void *take(uint32_t n) { return use_bulk ? myrtos_alloc_bulk(n) : myrtos_alloc(n); }
static uint32_t largest(void)
{
    return (uint32_t)myrtos_meminfo(use_bulk ? MYRTOS_MEM_BULK_FREE
                                             : MYRTOS_MEM_LARGEST_FREE);
}

// --- how long it takes -----------------------------------------------------
// Twenty, not two hundred. The SRAM pool has about twelve kilobytes free once
// this process has taken its own eight, so two hundred allocations of anything
// larger than a few bytes are refused -- and an average over refusals measures
// the refusal.
#define ROUNDS 20

static void time_one_size(uint32_t size)
{
    void *p[ROUNDS];
    uint32_t t0 = cycles();
    for (int i = 0; i < ROUNDS; i++) p[i] = take(size);
    uint32_t t_alloc = cycles() - t0;
    int got = 0;
    for (int i = 0; i < ROUNDS; i++) if (p[i]) got++;

    // Freed in the order taken. Freeing backwards is the easy case for a
    // coalescing allocator and would flatter it.
    t0 = cycles();
    for (int i = 0; i < ROUNDS; i++) if (p[i]) myrtos_free(p[i]);
    uint32_t t_free = cycles() - t0;

    if (!got) { printf("  %6u B   all %d refused\n", size, ROUNDS); return; }
    printf("  %6u B   alloc %4u   free %4u cycles", size,
           t_alloc / (uint32_t)got, t_free / (uint32_t)got);
    if (got != ROUNDS) printf("   (%d of %d refused)", ROUNDS - got, ROUNDS);
    printf("\n");
}

// --- whether it survives ---------------------------------------------------
#define HELD 24

// A pattern that depends on the address, so a block written through a stale
// pointer is caught even when the value looks plausible.
static uint8_t mark(void *p, uint32_t i) { return (uint8_t)(((uintptr_t)p >> 3) + i); }

static bool stress(uint32_t iterations, uint32_t max_size)
{
    void *p[HELD];
    uint32_t n[HELD];
    for (int i = 0; i < HELD; i++) { p[i] = 0; n[i] = 0; }

    uint32_t rnd = 12345;
    uint32_t taken = 0, freed = 0, refused = 0;

    for (uint32_t k = 0; k < iterations; k++) {
        rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;   // xorshift
        uint32_t slot = rnd % HELD;

        if (p[slot]) {
            // Verify before letting go. Anything that overwrote this block is
            // found here, at the moment the evidence is still present.
            for (uint32_t i = 0; i < n[slot]; i++)
                if (((uint8_t*)p[slot])[i] != mark(p[slot], i)) {
                    printf("  CORRUPTION at %p byte %u after %u operations\n",
                           p[slot], i, k);
                    return false;
                }
            myrtos_free(p[slot]);
            p[slot] = 0;
            freed++;
            continue;
        }

        uint32_t size = 1 + (rnd >> 8) % max_size;
        p[slot] = take(size);
        if (!p[slot]) { refused++; continue; }
        n[slot] = size;
        for (uint32_t i = 0; i < size; i++) ((uint8_t*)p[slot])[i] = mark(p[slot], i);
        taken++;
    }

    for (int i = 0; i < HELD; i++) if (p[i]) { myrtos_free(p[i]); freed++; }
    printf("  %u operations: %u taken, %u freed, %u refused\n",
           iterations, taken, freed, refused);
    return true;
}

void module_main(int argc, char **argv)
{
    start_counting();
    use_bulk = argc > 1 && argv[1][0] == 'b';
    printf("\ntlsftest: %s pool\n", use_bulk ? "PSRAM" : "SRAM");

    uint32_t before = largest();
    printf("  largest free at the start: %u bytes\n", before);

    // One block, taken and given back. Everything else here is worth measuring
    // only if this works.
    {
        void *q = take(1024);
        uint32_t held = largest();
        int32_t rc = myrtos_free(q);
        uint32_t back = largest();

        // If the same address comes again, the block is on a free list and the
        // fault is coalescing. A different one means it was never returned.
        void *again = take(1024);
        printf("  1024 at %p: %u held, %u after free (rc %d) %s\n",
               q, held, back, rc, back == before ? "-- returned" : "-- NOT RETURNED");
        printf("  taking 1024 again gives %p %s\n", again,
               again == q ? "-- the same block" : "-- a different one");
        myrtos_free(again);
    }

    // Largest is not total. A pool can be roomy and unable to give out a
    // kilobyte, and the two readings tell fragmentation from exhaustion --
    // which is the difference between an allocator that is failing and a pool
    // that is simply full.
    {
        void *held[64];
        uint32_t n = 0, total = 0;
        while (n < 64) {
            void *q = take(64);
            if (!q) break;
            held[n++] = q;
            total += 64;
        }
        for (uint32_t i = 0; i < n; i++) myrtos_free(held[i]);
        printf("  in 64-byte pieces it can give out %u bytes in %u of them\n", total, n);
    }

    printf("\n  timing, %d allocations of each size\n", ROUNDS);
    time_one_size(16);
    time_one_size(64);
    time_one_size(256);
    if (use_bulk) { time_one_size(1024); time_one_size(16384); }

    printf("\n  mixed sizes, held and released in no order\n");
    bool ok = stress(20000, use_bulk ? 4096 : 256);

    uint32_t after = largest();
    printf("\n  largest free at the end:   %u bytes  %s\n", after,
           after == before ? "-- nothing lost" : "-- FRAGMENTED OR LEAKED");
    printf("  %s\n", ok && after == before ? "passed" : "FAILED");
}
