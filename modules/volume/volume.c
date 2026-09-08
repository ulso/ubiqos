#include "../../common/myrtos_abi.h"

// volume -- how loud the headphone output is.
//
//   volume        what it is now
//   volume N      0 to 100, where 100 is full scale
//
// This writes the codec's own digital volume over /dev/i2c, which means the
// TLV320's address and register numbers appear here as well as in the audio
// driver. That is a wart and it is worth naming: the right home for this is
// the audio device itself, through some way of handing a device a command that
// is not data. myrtos has no ioctl, so until it does, this is the honest
// version -- a separate command that says plainly which chip it is poking.
//
// Nought to a hundred maps onto half-decibel steps: 100 is 0 dB and 0 is -50,
// which is quiet rather than silent. Muting is what not writing is for.

#define DAC_ADDR  0x18
#define REG_LEFT  0x41
#define REG_RIGHT 0x42

static bool xfer(int32_t fd, uint8_t nw, uint8_t nr, const uint8_t *w) {
    uint8_t b[4 + 4];
    b[0] = DAC_ADDR; b[1] = nw; b[2] = nr; b[3] = 0;
    for (uint8_t i = 0; i < nw; i++) b[4 + i] = w[i];
    return myrtos_write(fd, b, (uint32_t)(4 + nw)) >= 0;
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: volume [0-100]\n\n100 is full scale, 0 is about -50 dB. With no argument, reports.\n"))
        return;

    int32_t fd = myrtos_open("/dev/i2c");
    if (fd < 0) { myrtos_write_str(MYRTOS_STDERR, "volume: no /dev/i2c\n"); return; }

    // Page 0, where the DAC volume lives. The audio driver leaves the codec
    // there, but saying so costs one write and removes an assumption.
    uint8_t page[2] = { 0x00, 0x00 };
    xfer(fd, 2, 0, page);

    if (argc == 1) {
        uint8_t reg = REG_LEFT, got = 0;
        if (!xfer(fd, 1, 1, &reg) || myrtos_read(fd, &got, 1) != 1) {
            myrtos_write_str(MYRTOS_STDERR, "volume: no answer from the codec\n");
            myrtos_close(fd);
            return;
        }
        // Signed half-decibels back to the scale above.
        int32_t steps = (int32_t)(int8_t)got;
        int32_t n = 100 + steps;
        if (n < 0) n = 0;
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "volume ");
        myrtos_line_u32(&l, (uint32_t)n);
        myrtos_line_str(&l, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
        myrtos_close(fd);
        return;
    }

    uint32_t n = 0, digits = 0;
    for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++, digits++) n = n * 10 + (uint32_t)(*p - '0');
    if (!digits || argv[1][digits] || n > 100) {
        myrtos_write_str(MYRTOS_STDERR, "usage: volume [0-100]\n");
        myrtos_close(fd);
        return;
    }

    uint8_t v = (uint8_t)(int8_t)((int32_t)n - 100);
    uint8_t w[2];
    w[0] = REG_LEFT;  w[1] = v; xfer(fd, 2, 0, w);
    w[0] = REG_RIGHT; w[1] = v; xfer(fd, 2, 0, w);
    myrtos_close(fd);
}
