#include "../../common/ubiqos_abi.h"

// memtest -- exercises the allocator.
//
//   memtest         allocate, write, grow, verify, release
//   memtest leak    allocate and exit without freeing, on purpose
//   memtest bulk N  take N kB from PSRAM, fill it, read it back
//   memtest reserve  write and verify the half megabyte the pool withholds
//
// The second mode is the interesting one: run free before and after and the
// largest block should be unchanged, because dying returns what dying takes.

static void say(const char *a, uint32_t v, bool with_v) {
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, a);
    if (with_v) ubiqos_line_u32(&l, v);
    ubiqos_line_str(&l, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}

static bool eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: memtest [leak | bulk KB | reserve]\n\n  (none)     allocate, write, grow, verify, release\n  leak       allocate and exit without freeing, on purpose\n  bulk KB    take KB from PSRAM, fill it, read it back\n  reserve    write and verify the region the pool withholds\n")) return;

    if (argc == 2 && eq(argv[1], "leak")) {
        for (int i = 0; i < 4; i++) {
            if (!ubiqos_alloc(2000)) { say("leak: allocation refused", 0, false); return; }
        }
        say("leaked 4 x 2000 bytes on purpose", 0, false);
        return;                      // no frees; the kernel must reclaim
    }

    // The region at UBIQOS_SINGLE_BASE, which the bulk pool is told to stop
    // short of. Nothing is linked there and nothing allocates from it, so it can
    // be written freely -- and the question it answers is whether the top of
    // PSRAM is really there, which handing it to the allocator would otherwise
    // ask at boot with no way to see the answer.
    //
    // The pattern is derived from the address, so a region that aliases a lower
    // one fails on the read-back rather than passing quietly.
    if (argc == 2 && eq(argv[1], "reserve")) {
        volatile uint32_t *p = (volatile uint32_t *)UBIQOS_SINGLE_BASE;
        uint32_t words = UBIQOS_SINGLE_RESERVE / 4;

        say("writing ", UBIQOS_SINGLE_RESERVE / 1024, true);
        for (uint32_t i = 0; i < words; i++)
            p[i] = ((uint32_t)(uintptr_t)&p[i]) ^ 0xa5a5a5a5u;

        uint32_t bad = 0, first = 0;
        for (uint32_t i = 0; i < words; i++) {
            uint32_t want = ((uint32_t)(uintptr_t)&p[i]) ^ 0xa5a5a5a5u;
            if (p[i] != want) {
                if (!bad) first = (uint32_t)(uintptr_t)&p[i];
                bad++;
            }
        }
        if (bad) {
            say("words wrong: ", bad, true);
            say("first at ", first, true);
        } else {
            say("all of it reads back what was written", 0, false);
        }

        // And the two ends, in case the middle is fine and the edges are not.
        say("first word ", p[0], true);
        say("last word  ", p[words - 1], true);
        return;
    }

    if (argc == 3 && eq(argv[1], "bulk")) {
        uint32_t kb = 0;
        for (const char *c = argv[2]; *c >= '0' && *c <= '9'; c++) kb = kb * 10 + (uint32_t)(*c - '0');
        if (!kb) kb = 512;
        uint32_t n = kb * 1024;

        uint8_t *b = (uint8_t*)ubiqos_alloc_bulk(n);
        if (!b) { say("bulk: allocation refused", 0, false); return; }
        say("got a block at ", (uint32_t)(uintptr_t)b, true);

        // A pattern that depends on position, so a wrong address shows up as a
        // wrong value rather than as silence.
        for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)((i * 31u + (i >> 8)) & 0xff);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < n; i++)
            if (b[i] != (uint8_t)((i * 31u + (i >> 8)) & 0xff)) bad++;

        say(bad ? "bulk: wrong bytes: " : "bulk verified, wrong bytes: ", bad, true);
        say("freed, returns ", (uint32_t)ubiqos_free(b), true);
        return;
    }

    uint8_t *p = (uint8_t*)ubiqos_alloc(1000);
    if (!p) { say("alloc failed", 0, false); return; }
    for (uint32_t i = 0; i < 1000; i++) p[i] = (uint8_t)(i & 0xff);

    uint8_t *q = (uint8_t*)ubiqos_realloc(p, 4000);
    if (!q) { say("realloc failed", 0, false); ubiqos_free(p); return; }

    uint32_t bad = 0;
    for (uint32_t i = 0; i < 1000; i++) if (q[i] != (uint8_t)(i & 0xff)) bad++;
    say(bad ? "realloc lost bytes: " : "realloc kept all 1000 bytes, wrong: ", bad, true);

    // A pointer we were never given must be refused rather than freed.
    uint8_t fake[8];
    say("free of a stranger returns ", (uint32_t)ubiqos_free(fake + 4), true);
    say("free of ours returns ", (uint32_t)ubiqos_free(q), true);
    say("double free returns ", (uint32_t)ubiqos_free(q), true);
}
