#include "../../common/myrtos_abi.h"

// leds -- the five RGB lamps.
//
//   leds off              all dark
//   leds R G B            all five that colour
//   leds N R G B          only lamp N, the others left as they are
//   leds                  what they are showing now
//
// The device takes three bytes per lamp and nothing else; this exists because
// a shell writes text. Reading it back is what lets "leds N ..." leave the
// others alone without this command having to remember anything -- the state
// lives in the driver, where it belongs, and is asked for.

static bool is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return !*a && !*b;
}

static uint32_t to_u32(const char *s, bool *ok) {
    uint32_t v = 0, digits = 0;
    for (; *s >= '0' && *s <= '9'; s++) { v = v * 10 + (uint32_t)(*s - '0'); digits++; }
    *ok = digits && !*s && v <= 255;
    return v;
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: leds [off | R G B | N R G B]\n\n"
            "  off        all five dark\n"
            "  R G B      all five, each 0-255\n"
            "  N R G B    lamp N only, 0-4\n"
            "  (none)     what they are showing\n"))
        return;

    int32_t fd = myrtos_open("/dev/leds");
    if (fd < 0) { myrtos_write_str(MYRTOS_STDERR, "leds: no /dev/leds\n"); return; }

    uint8_t px[15];
    int32_t got = myrtos_read(fd, px, sizeof px);
    if (got != (int32_t)sizeof px) for (int i = 0; i < 15; i++) px[i] = 0;

    if (argc == 1) {
        myrtos_line_t l;
        for (int i = 0; i < 5; i++) {
            myrtos_line_reset(&l);
            myrtos_line_u32(&l, (uint32_t)i);
            myrtos_line_str(&l, ": ");
            for (int c = 0; c < 3; c++) {
                myrtos_line_u32(&l, px[i * 3 + c]);
                myrtos_line_str(&l, c < 2 ? " " : "\n");
            }
            myrtos_line_flush(MYRTOS_STDOUT, &l);
        }
        myrtos_close(fd);
        return;
    }

    bool ok = true;
    if (argc == 2 && is(argv[1], "off")) {
        for (int i = 0; i < 15; i++) px[i] = 0;
    } else if (argc == 4) {
        uint8_t c[3];
        for (int i = 0; i < 3; i++) { bool g; c[i] = (uint8_t)to_u32(argv[i + 1], &g); ok = ok && g; }
        if (ok) for (int i = 0; i < 5; i++) { px[i*3] = c[0]; px[i*3+1] = c[1]; px[i*3+2] = c[2]; }
    } else if (argc == 5) {
        bool g;
        uint32_t n = to_u32(argv[1], &g);
        ok = g && n < 5;
        for (int i = 0; i < 3; i++) { bool h; uint32_t v = to_u32(argv[i + 2], &h); ok = ok && h;
                                      if (ok) px[n * 3 + i] = (uint8_t)v; }
    } else {
        ok = false;
    }

    if (!ok) {
        myrtos_write_str(MYRTOS_STDERR, "usage: leds [off | R G B | N R G B]\n");
        myrtos_close(fd);
        return;
    }

    myrtos_write(fd, px, sizeof px);
    myrtos_close(fd);
}
