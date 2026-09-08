#include "../../common/myrtos_abi.h"

// crit -- hold a kernel critical section on purpose, for as long as asked.
//
//   crit US [TIMES]
//
// It exists for one measurement. A handler installed above
// MYRTOS_CRITICAL_BASEPRI is supposed to run even while the kernel is inside a
// critical section, and the only way to see whether it does is to be inside
// one for long enough to notice. Real critical sections in this kernel measure
// under three microseconds -- so under load, PRIMASK and BASEPRI produce
// exactly the same worst-case latency and the experiment says nothing.
//
// Run this against `adc -i` and the difference is the whole point: with the
// kernel masking by PRIMASK the worst gap grows to about the hold time, and
// with it masking by BASEPRI it does not move.

static uint32_t to_u32(const char *p, bool *ok)
{
    uint32_t v = 0, n = 0;
    for (; *p >= '0' && *p <= '9'; p++, n++) v = v * 10u + (uint32_t)(*p - '0');
    if (!n || *p) *ok = false;
    return v;
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: crit US [TIMES]\n\nHolds a kernel critical section for US microseconds, TIMES over.\n"
            "A test instrument: see what it does to 'adc -i'.\n"))
        return;

    uint32_t us = 500, times = 20;
    bool ok = true;
    if (argc > 1) us = to_u32(argv[1], &ok);
    if (ok && argc > 2) times = to_u32(argv[2], &ok);
    if (!ok || !us) {
        myrtos_write_str(MYRTOS_STDERR, "usage: crit US [TIMES]\n");
        return;
    }

    for (uint32_t i = 0; i < times; i++) {
        myrtos_crit_hold(us);
        // A gap between them, so the scheduler and everything else get their
        // turn and what is measured is one hold rather than a solid block.
        myrtos_sleep(5);
    }

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "held a critical section ");
    myrtos_line_u32(&l, times);
    myrtos_line_str(&l, " times, ");
    myrtos_line_u32(&l, us);
    myrtos_line_str(&l, " us each\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}
