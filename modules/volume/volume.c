#include "../../common/myrtos_abi.h"

// volume -- how loud the headphone output is.
//
//   volume        what it is now, and what rate the device runs at
//   volume N      0 to 100, where 100 is full scale
//
// This asks the audio device, and knows nothing about what is behind it. The
// first version wrote a TLV320's registers over /dev/i2c, so this file had to
// know the codec's address, which two registers held the volume, and that they
// were signed half-decibels; a different codec meant a different command, and
// nothing stopped a second program setting the volume without the driver
// knowing what it now was. All of that was the driver's business and is now
// kept there. See MYRTOS_SS_VOLUME in myrtos_abi.h.

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: volume [0-100]\n\n100 is full scale, 0 is about -50 dB. With no argument, reports.\n"))
        return;

    int32_t fd = myrtos_open("/dev/audio");
    if (fd < 0) { myrtos_write_str(MYRTOS_STDERR, "volume: no /dev/audio\n"); return; }

    if (argc == 1) {
        uint32_t v = 0, rate = 0;
        if (myrtos_getstat(fd, MYRTOS_SS_VOLUME, &v, sizeof v) < 0) {
            myrtos_write_str(MYRTOS_STDERR, "volume: the device has no volume\n");
            myrtos_close(fd);
            return;
        }
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "volume ");
        myrtos_line_u32(&l, v);
        // Not an error to be without one: a device that answers the volume
        // need not answer the rate, and saying nothing is the right amount to
        // say about a question this device did not take.
        if (myrtos_getstat(fd, MYRTOS_SS_RATE, &rate, sizeof rate) == 0) {
            myrtos_line_str(&l, ", ");
            myrtos_line_u32(&l, rate);
            myrtos_line_str(&l, " Hz");
        }
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

    if (myrtos_setstat(fd, MYRTOS_SS_VOLUME, &n, sizeof n) < 0)
        myrtos_write_str(MYRTOS_STDERR, "volume: the device would not take it\n");

    myrtos_close(fd);
}
