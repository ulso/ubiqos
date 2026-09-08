#include "../../common/myrtos_abi.h"

// adc -- the analogue inputs, and how well the driver's interrupt is being let
// run.
//
//   adc          the four channels, in millivolts
//   adc -i       what the interrupt has been doing
//   adc -r       forget the worst case and start again
//
// The -i report is the point of the exercise. The driver's handler sits at
// priority 0x40 and the kernel's critical sections mask down to 0x80, so the
// handler is never held off by one -- and the worst gap between two runs is
// the number that says whether that is true. Under a kernel that masks with
// PRIMASK instead, every critical section shows up in it.

MYRTOS_MEM_SIZE(8192);

static void u32(myrtos_line_t *l, const char *label, uint32_t v, const char *tail)
{
    myrtos_line_str(l, label);
    myrtos_line_u32(l, v);
    myrtos_line_str(l, tail);
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: adc [-o | -f | -i | -r]\n\nWith no argument, the four analogue inputs in millivolts.\n"
            "A digit brings it up a step at a time: 1 pads, 2 the ADC block,\n3 the interrupt installed, 4 converting. 0 stops.\n"
            "-i reports the interrupt; -r clears its worst case.\n"))
        return;

    int32_t fd = myrtos_open("/dev/adc");
    if (fd < 0) { myrtos_write_str(MYRTOS_STDERR, "adc: no /dev/adc\n"); return; }

    bool info  = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'i';
    bool reset = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'r';
    // A digit is how far to bring the driver up: 1 pads, 2 the ADC block,
    // 3 the interrupt installed, 4 converting. Each includes the ones before.
    bool step  = argc > 1 && argv[1][0] >= '0' && argv[1][1] == 0;

    if (step) {
        uint32_t v = (uint32_t)(argv[1][0] - '0');
        if (myrtos_setstat(fd, MYRTOS_SS_RUN, &v, sizeof v) < 0)
            myrtos_write_str(MYRTOS_STDERR,
                "adc: refused -- one of GP40-43 belongs to something else. Try 'gpio'.\n");
        myrtos_close(fd);
        return;
    }

    myrtos_line_t l;
    myrtos_line_reset(&l);

    if (reset) {
        uint32_t zero = 0;
        if (myrtos_setstat(fd, MYRTOS_SS_IRQSTATS, &zero, sizeof zero) < 0)
            myrtos_write_str(MYRTOS_STDERR, "adc: the device would not clear it\n");
        myrtos_close(fd);
        return;
    }

    if (info) {
        myrtos_irqstats_t s;
        if (myrtos_getstat(fd, MYRTOS_SS_IRQSTATS, &s, sizeof s) < 0) {
            myrtos_write_str(MYRTOS_STDERR, "adc: no interrupt statistics\n");
            myrtos_close(fd);
            return;
        }
        u32(&l, "stage ", s.priority, "");
        u32(&l, ", taken ", s.taken, "");
        u32(&l, ", overruns ", s.overruns, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
        myrtos_line_reset(&l);
        u32(&l, "gap between runs: expected ", s.expected_us, " us");
        u32(&l, ", best ", s.best_gap_us, "");
        u32(&l, ", WORST ", s.worst_gap_us, " us\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
        myrtos_close(fd);
        return;
    }

    uint8_t buf[8];
    int32_t n = myrtos_read(fd, buf, sizeof buf);
    if (n < 2) { myrtos_write_str(MYRTOS_STDERR, "adc: nothing to read\n"); myrtos_close(fd); return; }
    for (int32_t i = 0; i + 1 < n; i += 2) {
        uint32_t raw = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8);
        // 12 bits over a 3.3 V reference. Integers throughout: 3300 * raw
        // reaches 13.5 million, which is nowhere near overflowing.
        myrtos_line_str(&l, "A");
        myrtos_line_u32(&l, (uint32_t)(i / 2));
        myrtos_line_str(&l, " ");
        myrtos_line_u32(&l, raw * 3300u / 4095u);
        myrtos_line_str(&l, " mV  (");
        myrtos_line_u32(&l, raw);
        myrtos_line_str(&l, ")\n");
    }
    myrtos_line_flush(MYRTOS_STDOUT, &l);
    myrtos_close(fd);
}
