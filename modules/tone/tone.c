#include "../../common/ubiqos_abi.h"

// tone -- a sine into /dev/audio, so there is something to listen to.
//
//   tone            440 Hz for a second
//   tone HZ         that pitch for a second
//   tone HZ MS      and for that long
//
// A table and a phase accumulator, with no arithmetic wider than 32 bits and
// no floating point anywhere. That is not thrift: a module is built -nostdlib,
// and on RISC-V a float becomes a call into libgcc that is not there -- the
// neopixel driver learned it the hard way and this is written knowing it.
//
// The amplitude is deliberately short of full scale. This comes out of a
// headphone amplifier into somebody's ears, and a first tone from a driver
// nobody has heard yet is not the moment to find out whether the volume
// registers were what you thought.

UBIQOS_MEM_SIZE(8192);

// Asked, not assumed. This was 48000 written in, and when the device moved to
// 46875 every tone came out 2.3% flat and every duration 2.3% long -- a wrong
// answer that looked entirely right. The device knows its own rate.
#define RATE_FALLBACK 48000u
#define TABLE    64u
#define FRAMES   256u          // per write: 1 kB, about 5 ms

static const int16_t sine[TABLE] = {
         0,    882,   1756,   2613,   3444,   4243,   5000,   5710,
      6364,   6957,   7483,   7937,   8315,   8612,   8827,   8957,
      9000,   8957,   8827,   8612,   8315,   7937,   7483,   6957,
      6364,   5710,   5000,   4243,   3444,   2613,   1756,    882,
         0,   -882,  -1756,  -2613,  -3444,  -4243,  -5000,  -5710,
     -6364,  -6957,  -7483,  -7937,  -8315,  -8612,  -8827,  -8957,
     -9000,  -8957,  -8827,  -8612,  -8315,  -7937,  -7483,  -6957,
     -6364,  -5710,  -5000,  -4243,  -3444,  -2613,  -1756,   -882,
};

static uint32_t to_u32(const char *s, bool *ok) {
    uint32_t v = 0, n = 0;
    for (; *s >= '0' && *s <= '9'; s++, n++) v = v * 10 + (uint32_t)(*s - '0');
    *ok = n && !*s;
    return v;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: tone [-v] [HZ [MS]]\n\nA sine out of /dev/audio. 440 Hz for a second by default.\n-v reports what the device took, which is how the ring is checked.\n"))
        return;

    uint32_t hz = 440, ms = 1000;
    bool ok = true, verbose = false;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'v' && !argv[1][2]) {
        verbose = true;
        argv++; argc--;
    }
    if (argc > 1) hz = to_u32(argv[1], &ok);
    if (ok && argc > 2) ok = (ms = to_u32(argv[2], &ok), ok);

    int32_t fd = ubiqos_open("/dev/audio");
    if (fd < 0) { ubiqos_write_str(UBIQOS_STDERR, "tone: no /dev/audio\n"); return; }

    // The device is opened before the arguments are checked, because half of
    // what makes an argument wrong is the rate, and only the device knows it.
    uint32_t rate = RATE_FALLBACK;
    if (ubiqos_getstat(fd, UBIQOS_SS_RATE, &rate, sizeof rate) < 0 || !rate)
        rate = RATE_FALLBACK;

    if (!ok || !hz || hz > rate / 2 || !ms || ms > 30000) {
        ubiqos_write_str(UBIQOS_STDERR, "usage: tone [-v] [HZ [MS]]  -- up to half the device rate, 30000 ms\n");
        ubiqos_close(fd);
        return;
    }

    // Phase in 8.8 of a table entry, so the step is exact enough that nobody
    // can hear the rounding: 440 Hz asks for 150.19 and gets 150, which is
    // 439.5 Hz.
    uint32_t step = (hz * TABLE * 256u) / rate;
    uint32_t phase = 0;
    uint32_t total = (rate * ms) / 1000u;

    int16_t buf[FRAMES * 2];
    uint32_t asked = total, wrote = 0, shorts = 0, zeros = 0;
    while (total) {
        uint32_t n = total < FRAMES ? total : FRAMES;
        for (uint32_t i = 0; i < n; i++) {
            int16_t s = sine[(phase >> 8) & (TABLE - 1)];
            buf[i * 2] = s;          // the same in both ears
            buf[i * 2 + 1] = s;
            phase += step;
        }
        int32_t w = ubiqos_write(fd, (const uint8_t *)buf, n * 4u);
        if (w < 0) break;
        if (w == 0) { if (++zeros > 100000) break; continue; }
        if ((uint32_t)w < n * 4u) shorts++;
        wrote += (uint32_t)w / 4u;
        // Only what actually went. Decrementing by what was OFFERED was the
        // bug that made a three-second tone finish instantly and silently: the
        // device answers with what it took, and a short answer means try again
        // rather than move on.
        total -= (uint32_t)w / 4u;
        phase -= step * (n - (uint32_t)w / 4u);   // rewind what was not taken
    }

    // Kept, but not in the way. These four numbers are how the ring was
    // debugged -- a silent tone that "finished" instantly showed up here as
    // asked 144000, wrote 2048 -- so they are worth a flag rather than a
    // deletion. They are not worth printing at every beep.
    if (!verbose) { ubiqos_close(fd); return; }
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "asked "); ubiqos_line_u32(&l, asked);
    ubiqos_line_str(&l, ", wrote "); ubiqos_line_u32(&l, wrote);
    ubiqos_line_str(&l, ", short writes "); ubiqos_line_u32(&l, shorts);
    ubiqos_line_str(&l, ", empty "); ubiqos_line_u32(&l, zeros);
    ubiqos_line_str(&l, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);

    ubiqos_close(fd);
}
