#include "../../common/myrtos_abi.h"

// memtest -- exercises the allocator.
//
//   memtest         allocate, write, grow, verify, release
//   memtest leak    allocate and exit without freeing, on purpose
//
// The second mode is the interesting one: run free before and after and the
// largest block should be unchanged, because dying returns what dying takes.

static void say(const char *a, uint32_t v, bool with_v) {
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, a);
    if (with_v) myrtos_line_u32(&l, v);
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}

static bool eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void module_main(int argc, char **argv) {
    if (argc == 2 && eq(argv[1], "leak")) {
        for (int i = 0; i < 4; i++) {
            if (!myrtos_alloc(2000)) { say("leak: allocation refused", 0, false); return; }
        }
        say("leaked 4 x 2000 bytes on purpose", 0, false);
        return;                      // no frees; the kernel must reclaim
    }

    uint8_t *p = (uint8_t*)myrtos_alloc(1000);
    if (!p) { say("alloc failed", 0, false); return; }
    for (uint32_t i = 0; i < 1000; i++) p[i] = (uint8_t)(i & 0xff);

    uint8_t *q = (uint8_t*)myrtos_realloc(p, 4000);
    if (!q) { say("realloc failed", 0, false); myrtos_free(p); return; }

    uint32_t bad = 0;
    for (uint32_t i = 0; i < 1000; i++) if (q[i] != (uint8_t)(i & 0xff)) bad++;
    say(bad ? "realloc lost bytes: " : "realloc kept all 1000 bytes, wrong: ", bad, true);

    // A pointer we were never given must be refused rather than freed.
    uint8_t fake[8];
    say("free of a stranger returns ", (uint32_t)myrtos_free(fake + 4), true);
    say("free of ours returns ", (uint32_t)myrtos_free(q), true);
    say("double free returns ", (uint32_t)myrtos_free(q), true);
}
