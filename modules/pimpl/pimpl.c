#include "state.h"

// pimpl -- shows a module split across files keeping per-process state.
//
//   pimpl NAME COUNT
//
// Two of these running at once must not see each other's counter, which is the
// whole reason a module may not use a static variable for this.

void bump(uint32_t times);

void module_main(int argc, char **argv) {
    uint32_t size = 0;
    state_t *s = (state_t *)myrtos_data_area(&size);
    if (!s || size < sizeof(state_t)) {
        myrtos_write_str(MYRTOS_STDERR, "pimpl: no data area\n");
        return;
    }

    s->counter = 0;
    int n = 0;
    if (argc > 1) while (n < 15 && argv[1][n]) { s->label[n] = argv[1][n]; n++; }
    s->label[n] = 0;

    uint32_t rounds = 0;
    if (argc > 2) for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
        rounds = rounds * 10 + (uint32_t)(*p - '0');
    if (!rounds) rounds = 1000;

    // Counted in the other file, and interleaved with the other process.
    for (uint32_t i = 0; i < rounds / 100; i++) { bump(100); myrtos_sleep(1); }

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, s->label);
    myrtos_line_str(&l, ": counter ");
    myrtos_line_u32(&l, s->counter);
    myrtos_line_str(&l, ", area ");
    myrtos_line_u32(&l, size);
    myrtos_line_str(&l, " bytes\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}
