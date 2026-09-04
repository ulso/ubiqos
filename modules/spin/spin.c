#include "../../common/myrtos_abi.h"

// spin -- busy-loops at a given priority for a given time and reports how much
// work it got through. Two of these running at once is how you see whether the
// scheduler is doing what it claims: equal priorities should each get about
// half, and unequal ones should not share at all.

static bool parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (!*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (uint32_t)(*s - '0');
    }
    *out = v;
    return true;
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: spin [PRIORITY] MILLISECONDS\n\nBurns processor time, for testing the scheduler.\n")) return;

    // With one argument it leaves its priority alone, which is what makes it
    // useful for showing that a child inherits: if spin set its own, there
    // would be no way to tell inheriting from overriding.
    uint32_t prio, ms;
    if (argc == 2 && parse_u32(argv[1], &ms)) {
        prio = (uint32_t)myrtos_getprio();
    } else if (argc == 3 && parse_u32(argv[1], &prio) && parse_u32(argv[2], &ms)) {
        myrtos_setprio(prio);
    } else {
        myrtos_write_str(MYRTOS_STDERR, "usage: spin [PRIORITY] MILLISECONDS\n");
        return;
    }

    uint32_t start = myrtos_ticks_now();
    uint32_t rounds = 0;
    while (myrtos_ticks_now() - start < ms) rounds++;

    // Drop back and sleep before reporting. The idle process drives TinyUSB, so
    // while anything above it is spinning the USB console cannot be serviced --
    // printing from up here would go into a FIFO nobody is draining.
    myrtos_setprio(MYRTOS_PRIO_DEFAULT);
    myrtos_sleep(150);

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "prio ");
    myrtos_line_u32(&l, prio);
    myrtos_line_str(&l, ": ");
    myrtos_line_u32(&l, rounds);
    myrtos_line_str(&l, " rounds\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}
