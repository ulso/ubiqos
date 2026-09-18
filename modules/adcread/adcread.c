#include "../../common/ubiqos_abi.h"

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

UBIQOS_MEM_SIZE(8192);

static void u32(ubiqos_line_t *l, const char *label, uint32_t v, const char *tail)
{
    ubiqos_line_str(l, label);
    ubiqos_line_u32(l, v);
    ubiqos_line_str(l, tail);
}

static uint32_t to_u32(const char *p, bool *ok)
{
    uint32_t v = 0, n = 0;
    for (; *p >= '0' && *p <= '9'; p++, n++) v = v * 10u + (uint32_t)(*p - '0');
    if (!n || *p) *ok = false;
    return v;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: adc [-o | -f | -i | -r]\n\nWith no argument, the four analogue inputs in millivolts.\n"
            "A digit brings it up a step at a time: 1 pads, 2 the ADC block,\n3 the interrupt installed, 4 converting. 0 stops.\n"
            "-i reports the interrupt; -r clears its worst case.\n-p N gives the handler a pin to toggle, for a scope.\n"))
        return;

    int32_t fd = ubiqos_open("/dev/adc");
    if (fd < 0) { ubiqos_write_str(UBIQOS_STDERR, "adc: no /dev/adc\n"); return; }

    bool info  = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'i';
    bool reset = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'r';
    // A digit is how far to bring the driver up: 1 pads, 2 the ADC block,
    // 3 the interrupt installed, 4 converting. Each includes the ones before.
    bool step  = argc > 1 && argv[1][0] >= '0' && argv[1][1] == 0;

    // adc -p N gives the handler a pin to toggle; adc -p alone takes it back.
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'p') {
        uint32_t pin = 0xffffffffu;
        if (argc > 2) { bool k = true; pin = to_u32(argv[2], &k); if (!k) pin = 0xffffffffu; }
        if (ubiqos_setstat(fd, UBIQOS_SS_IRQPIN, &pin, sizeof pin) < 0)
            ubiqos_write_str(UBIQOS_STDERR, "adc: that pin is taken, or is not below 32\n");
        ubiqos_close(fd);
        return;
    }

    if (step) {
        uint32_t v = (uint32_t)(argv[1][0] - '0');
        if (ubiqos_setstat(fd, UBIQOS_SS_RUN, &v, sizeof v) < 0)
            ubiqos_write_str(UBIQOS_STDERR,
                "adc: refused -- one of GP40-43 belongs to something else. Try 'gpio'.\n");
        ubiqos_close(fd);
        return;
    }

    ubiqos_line_t l;
    ubiqos_line_reset(&l);

    if (reset) {
        uint32_t zero = 0;
        if (ubiqos_setstat(fd, UBIQOS_SS_IRQSTATS, &zero, sizeof zero) < 0)
            ubiqos_write_str(UBIQOS_STDERR, "adc: the device would not clear it\n");
        ubiqos_close(fd);
        return;
    }

    if (info) {
        ubiqos_irqstats_t s;
        if (ubiqos_getstat(fd, UBIQOS_SS_IRQSTATS, &s, sizeof s) < 0) {
            ubiqos_write_str(UBIQOS_STDERR, "adc: no interrupt statistics\n");
            ubiqos_close(fd);
            return;
        }
        u32(&l, "stage ", s.priority, "");
        u32(&l, ", taken ", s.taken, "");
        u32(&l, ", overruns ", s.overruns, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
        ubiqos_line_reset(&l);
        u32(&l, "gap between runs: expected ", s.expected_us, " us");
        u32(&l, ", best ", s.best_gap_us, "");
        u32(&l, ", WORST ", s.worst_gap_us, " us\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
        ubiqos_close(fd);
        return;
    }

    // Five inputs on the header, not four, and the first of them is A1.
    uint8_t buf[16];
    int32_t n = ubiqos_read(fd, buf, sizeof buf);
    if (n < 2) { ubiqos_write_str(UBIQOS_STDERR, "adc: nothing to read\n"); ubiqos_close(fd); return; }
    // Which channels came back, ascending, skipping any the driver could not
    // claim. Asked for rather than assumed.
    uint32_t mask = 0;
    if (ubiqos_getstat(fd, UBIQOS_SS_ADCCHANS, &mask, sizeof mask) < 0) mask = 0x3e;
    uint32_t label[8], nl = 0;
    for (uint32_t c = 0; c < 8; c++) if (mask & (1u << c)) label[nl++] = c;

    for (int32_t i = 0; i + 1 < n; i += 2) {
        uint32_t raw = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8);
        // 12 bits over a 3.3 V reference. Integers throughout: 3300 * raw
        // reaches 13.5 million, which is nowhere near overflowing.
        // The board's own label. A4 is missing when the kernel has GP44 for
        // its UART, so the numbering comes from the channel and not from the
        // position in the buffer -- see the note in modules/adc.
        ubiqos_line_str(&l, "A");
        ubiqos_line_u32(&l, label[i / 2]);
        ubiqos_line_str(&l, " ");
        ubiqos_line_u32(&l, raw * 3300u / 4095u);
        ubiqos_line_str(&l, " mV  (");
        ubiqos_line_u32(&l, raw);
        ubiqos_line_str(&l, ")\n");
    }
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
    ubiqos_close(fd);
}
