#include "../../common/myrtos_abi.h"

MYRTOS_MEM_SIZE(8192);

// scope -- the ADC on the screen, drawn as line segments.
//
// A trace is 639 short segments, one per column, and that is the case the
// scanline renderer is good at: each one crosses only the few rows between two
// samples, so what a scanline costs is the number of times the waveform crosses
// that height -- a handful, whatever the trace looks like. The 1024-segment
// limit is about memory; the 64-per-scanline limit is about time, and a trace
// comes nowhere near it.

#define W        640
#define H        480
#define TOP      40             // room for the reading, in text rows 0 and 1
#define TRACE_H  (H - TOP - 8)

static uint16_t sample[W];

static uint32_t to_u32(const char *s) {
    uint32_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (uint32_t)(*s - '0');
    return v;
}

// The graticule: eight divisions across and four down, drawn once and left in
// the list under the trace.
static void graticule(void) {
    for (int32_t i = 0; i <= 8; i++) {
        int32_t x = i * (W - 1) / 8;
        myrtos_vec_line(x, TOP, x, TOP + TRACE_H, 0x49);
    }
    for (int32_t i = 0; i <= 4; i++) {
        int32_t y = TOP + i * TRACE_H / 4;
        myrtos_vec_line(0, y, W - 1, y, 0x49);
    }
}

int main(int argc, char **argv);
void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
        "scope -- the ADC drawn as a trace\n"
        "  scope [A1-A5] [sweeps] [samples/s per channel]\n"
        "Needs the ADC converting: run 'adc 4' first.\n"
        "Leaves the picture behind; 'vec clear' takes it away.\n"))
        return;

    // The board's labels, A1 to A5, which are the channel numbers -- see the
    // note in modules/adc. A1 is the default because A0 is not a pin.
    uint32_t ch = argc > 1 ? to_u32(argv[1]) : 1;
    uint32_t sweeps = argc > 2 ? to_u32(argv[2]) : 20;
    uint32_t rate   = argc > 3 ? to_u32(argv[3]) : 0;    // per channel
    if (ch < 1 || ch > 5) ch = 1;

    int32_t fd = myrtos_open("/dev/adc");
    if (fd < 0) { myrtos_write_str(MYRTOS_STDERR, "scope: no /dev/adc\n"); return; }

    if (rate && myrtos_setstat(fd, MYRTOS_SS_RATE, &rate, sizeof rate) < 0)
        myrtos_write_str(MYRTOS_STDERR, "scope: that rate was refused; keeping the old one\n");

    for (uint32_t s = 0; s < sweeps; s++) {
        // One sweep, taken by the ADC's own interrupt rather than by this
        // loop. Arm it, wait, read it back: the samples are then evenly spaced
        // by the hardware, which is what makes the horizontal axis mean
        // anything at all.
        myrtos_adccap_t cap = { ch, W, 0, 0 };
        if (myrtos_setstat(fd, MYRTOS_SS_CAPTURE, &cap, sizeof cap) < 0) {
            myrtos_write_str(MYRTOS_STDERR,
                "scope: the device would not arm a sweep. Run 'adc 4' first.\n");
            myrtos_close(fd);
            return;
        }

        // Sleep for as long as the sweep is expected to take, then ask. Asking
        // every two milliseconds meant a hundred and sixty system calls per
        // sweep, and a trap runs with interrupts off -- there is no reason to
        // hold them off a hundred and sixty times to learn something that could
        // be worked out from the rate.
        uint32_t hz = 0;
        if (myrtos_getstat(fd, MYRTOS_SS_RATE, &hz, sizeof hz) < 0 || !hz)
            hz = 2000;
        myrtos_sleep(W * 1000u / hz);

        for (uint32_t wait = 0; wait < 100; wait++) {
            if (myrtos_getstat(fd, MYRTOS_SS_CAPTURE, &cap, sizeof cap) < 0) break;
            if (cap.taken >= W) break;
            myrtos_sleep(10);
        }

        int32_t got = myrtos_getstat(fd, MYRTOS_SS_CAPDATA, sample, sizeof sample);
        if (got <= 0) {
            myrtos_write_str(MYRTOS_STDERR, "scope: the sweep did not finish\n");
            myrtos_close(fd);
            return;
        }
        uint32_t n = (uint32_t)got / 2u;

        // In place: the read left raw counts here and the picture wants rows.
        for (uint32_t x = 0; x < n; x++) {
            uint32_t raw = sample[x];
            if (raw > 4095) raw = 4095;
            sample[x] = (uint16_t)(TOP + TRACE_H - raw * TRACE_H / 4095u);
        }

        myrtos_vec_clear();
        graticule();
        // A segment per column is right for a signal that moves and disastrous
        // for one that does not: a flat trace puts every one of them on the
        // SAME scanline, and the per-scanline cap then draws sixty-four of
        // them and stops. That is what a dead input looked like -- a short
        // green stub in the bottom left corner rather than a line.
        //
        // So a run of equal samples becomes one horizontal segment, and only
        // the steps between runs cost a segment each. A signal that really does
        // move gives the same output as before; a flat one gives two.
        uint32_t refused = 0, start = 0;
        for (uint32_t x = 1; x <= n; x++) {
            if (x < n && sample[x] == sample[start])
                continue;
            if (x - 1 > start)
                refused += myrtos_vec_line((int32_t)start, sample[start],
                                           (int32_t)x - 1, sample[start], 0x1c) < 0;
            if (x < n)
                refused += myrtos_vec_line((int32_t)x - 1, sample[start],
                                           (int32_t)x, sample[x], 0x1c) < 0;
            start = x;
        }

        // The reading, in the console rows the graticule leaves free.
        uint32_t mid = sample[W / 2];
        uint32_t mv = (TOP + TRACE_H - mid) * 3300u / (uint32_t)TRACE_H;
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "\x1b[H\x1b[Kscope: A");
        myrtos_line_u32(&l, ch);
        myrtos_line_str(&l, "  centre ");
        myrtos_line_u32(&l, mv);
        // The timebase is the span the handler measured across the sweep,
        // divided by the graticule. Not the rate that was asked for -- what
        // the samples actually took.
        myrtos_line_str(&l, " mV   ");
        myrtos_line_u32(&l, cap.span_us / 8000u ? cap.span_us / 8000u : 0u);
        myrtos_line_str(&l, ".");
        myrtos_line_u32(&l, (cap.span_us / 800u) % 10u);
        myrtos_line_str(&l, " ms/div   sweep ");
        myrtos_line_u32(&l, s + 1);
        myrtos_line_str(&l, "/");
        myrtos_line_u32(&l, sweeps);
        // Say so rather than drawing a picture that is quietly incomplete.
        if (refused) {
            myrtos_line_str(&l, "   ");
            myrtos_line_u32(&l, refused);
            myrtos_line_str(&l, " SEGMENTS REFUSED");
        }
        myrtos_line_str(&l, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    }

    myrtos_close(fd);
}
