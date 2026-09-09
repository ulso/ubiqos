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
        "  scope [channel] [sweeps]\n"
        "Needs the ADC converting: run 'adc 4' first.\n"
        "Leaves the picture behind; 'vec clear' takes it away.\n"))
        return;

    uint32_t ch = argc > 1 ? to_u32(argv[1]) : 0;
    uint32_t sweeps = argc > 2 ? to_u32(argv[2]) : 20;
    if (ch > 3) ch = 3;

    int32_t fd = myrtos_open("/dev/adc");
    if (fd < 0) { myrtos_write_str(MYRTOS_STDERR, "scope: no /dev/adc\n"); return; }

    for (uint32_t s = 0; s < sweeps; s++) {
        // One sweep: fill the buffer as fast as the device will answer. The
        // sample rate is whatever this loop achieves, which is the honest
        // thing to say about it -- there is no timebase here yet.
        for (uint32_t x = 0; x < W; x++) {
            uint8_t buf[8];
            int32_t n = myrtos_read(fd, buf, sizeof buf);
            uint32_t raw = 0;
            if (n >= (int32_t)(ch * 2 + 2))
                raw = (uint32_t)buf[ch * 2] | ((uint32_t)buf[ch * 2 + 1] << 8);
            if (raw > 4095) raw = 4095;
            sample[x] = (uint16_t)(TOP + TRACE_H - raw * TRACE_H / 4095u);
        }

        myrtos_vec_clear();
        graticule();
        for (uint32_t x = 0; x + 1 < W; x++)
            myrtos_vec_line((int32_t)x, sample[x], (int32_t)x + 1, sample[x + 1], 0x1c);

        // The reading, in the console rows the graticule leaves free.
        uint32_t mid = sample[W / 2];
        uint32_t mv = (TOP + TRACE_H - mid) * 3300u / (uint32_t)TRACE_H;
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "\x1b[H\x1b[Kscope: A");
        myrtos_line_u32(&l, ch);
        myrtos_line_str(&l, "  centre ");
        myrtos_line_u32(&l, mv);
        myrtos_line_str(&l, " mV   sweep ");
        myrtos_line_u32(&l, s + 1);
        myrtos_line_str(&l, "/");
        myrtos_line_u32(&l, sweeps);
        myrtos_line_str(&l, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    }

    myrtos_close(fd);
}
