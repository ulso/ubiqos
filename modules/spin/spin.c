#include "../../common/ubiqos_abi.h"

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
    if (ubiqos_help(argc, argv,
            "usage: spin [PRIORITY] MILLISECONDS\n\nBurns processor time, for testing the scheduler.\n")) return;

    // With one argument it leaves its priority alone, which is what makes it
    // useful for showing that a child inherits: if spin set its own, there
    // would be no way to tell inheriting from overriding.
    uint32_t prio, ms;
    if (argc == 2 && parse_u32(argv[1], &ms)) {
        prio = (uint32_t)ubiqos_getprio();
    } else if (argc == 3 && parse_u32(argv[1], &prio) && parse_u32(argv[2], &ms)) {
        ubiqos_setprio(prio);
    } else {
        ubiqos_write_str(UBIQOS_STDERR, "usage: spin [PRIORITY] MILLISECONDS\n");
        return;
    }

    uint32_t start = ubiqos_ticks_now();
    uint32_t rounds = 0;
    while (ubiqos_ticks_now() - start < ms) rounds++;

    // Drop back and sleep before reporting. The idle process drives TinyUSB, so
    // while anything above it is spinning the USB console cannot be serviced --
    // printing from up here would go into a FIFO nobody is draining.
    ubiqos_setprio(UBIQOS_PRIO_DEFAULT);
    ubiqos_sleep(150);

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "prio ");
    ubiqos_line_u32(&l, prio);
    ubiqos_line_str(&l, ": ");
    ubiqos_line_u32(&l, rounds);
    ubiqos_line_str(&l, " rounds\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}
