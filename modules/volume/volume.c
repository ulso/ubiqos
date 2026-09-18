#include "../../common/ubiqos_abi.h"

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
// kept there. See UBIQOS_SS_VOLUME in ubiqos_abi.h.

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: volume [0-100]\n\n100 is full scale, 0 is about -50 dB. With no argument, reports.\n"))
        return;

    int32_t fd = ubiqos_open("/dev/audio");
    if (fd < 0) { ubiqos_write_str(UBIQOS_STDERR, "volume: no /dev/audio\n"); return; }

    if (argc == 1) {
        uint32_t v = 0, rate = 0;
        if (ubiqos_getstat(fd, UBIQOS_SS_VOLUME, &v, sizeof v) < 0) {
            ubiqos_write_str(UBIQOS_STDERR, "volume: the device has no volume\n");
            ubiqos_close(fd);
            return;
        }
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "volume ");
        ubiqos_line_u32(&l, v);
        // Not an error to be without one: a device that answers the volume
        // need not answer the rate, and saying nothing is the right amount to
        // say about a question this device did not take.
        if (ubiqos_getstat(fd, UBIQOS_SS_RATE, &rate, sizeof rate) == 0) {
            ubiqos_line_str(&l, ", ");
            ubiqos_line_u32(&l, rate);
            ubiqos_line_str(&l, " Hz");
        }
        ubiqos_line_str(&l, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
        ubiqos_close(fd);
        return;
    }

    uint32_t n = 0, digits = 0;
    for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++, digits++) n = n * 10 + (uint32_t)(*p - '0');
    if (!digits || argv[1][digits] || n > 100) {
        ubiqos_write_str(UBIQOS_STDERR, "usage: volume [0-100]\n");
        ubiqos_close(fd);
        return;
    }

    if (ubiqos_setstat(fd, UBIQOS_SS_VOLUME, &n, sizeof n) < 0)
        ubiqos_write_str(UBIQOS_STDERR, "volume: the device would not take it\n");

    ubiqos_close(fd);
}
