#include "../../common/myrtos_abi.h"

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

MYRTOS_MEM_SIZE(8192);

#define RATE     48000u
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
    if (myrtos_help(argc, argv,
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
    if (!ok || !hz || hz > RATE / 2 || !ms || ms > 30000) {
        myrtos_write_str(MYRTOS_STDERR, "usage: tone [-v] [HZ [MS]]  -- up to 24000 Hz, 30000 ms\n");
        return;
    }

    int32_t fd = myrtos_open("/dev/audio");
    if (fd < 0) { myrtos_write_str(MYRTOS_STDERR, "tone: no /dev/audio\n"); return; }

    // Phase in 8.8 of a table entry, so the step is exact enough that nobody
    // can hear the rounding: 440 Hz asks for 150.19 and gets 150, which is
    // 439.5 Hz.
    uint32_t step = (hz * TABLE * 256u) / RATE;
    uint32_t phase = 0;
    uint32_t total = (RATE * ms) / 1000u;

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
        int32_t w = myrtos_write(fd, (const uint8_t *)buf, n * 4u);
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
    if (!verbose) { myrtos_close(fd); return; }
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "asked "); myrtos_line_u32(&l, asked);
    myrtos_line_str(&l, ", wrote "); myrtos_line_u32(&l, wrote);
    myrtos_line_str(&l, ", short writes "); myrtos_line_u32(&l, shorts);
    myrtos_line_str(&l, ", empty "); myrtos_line_u32(&l, zeros);
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);

    myrtos_close(fd);
}
