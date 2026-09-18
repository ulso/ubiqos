#include "pimpl.h"

// pimpl -- a module written as a class, split across two files.
//
//   pimpl NAME COUNT
//
// Two of these at once share one copy of the code and keep separate counters,
// which is what a module may not use a static variable for.

// An initialised thread-local, to exercise the other half of the mechanism:
// this one has a value in the module image that the kernel copies per process,
// rather than being zeroed like the instance itself.
static __thread char tag[8] = "ubiqos";

void Pimpl::run(int argc, char **argv) {
    counter = 0;
    tag[0] = (char)('A' + (ubiqos_ticks_now() & 7));   // proves it is per process

    int n = 0;
    if (argc > 1) while (n < 15 && argv[1][n]) { label[n] = argv[1][n]; n++; }
    label[n] = 0;

    uint32_t rounds = 0;
    if (argc > 2) for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
        rounds = rounds * 10 + (uint32_t)(*p - '0');
    if (!rounds) rounds = 1000;

    // Counted in the other file, and interleaved with the other process.
    for (uint32_t i = 0; i < rounds / 100; i++) { bump(100); ubiqos_sleep(1); }

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, label);
    ubiqos_line_str(&l, ": counter ");
    ubiqos_line_u32(&l, counter);
    ubiqos_line_str(&l, ", tag ");
    ubiqos_line_str(&l, tag);
    ubiqos_line_str(&l, ", area ");
    ubiqos_line_u32(&l, room());
    ubiqos_line_str(&l, " bytes\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}

UBIQOS_MODULE(Pimpl)
