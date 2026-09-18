#include "../../common/ubiqos_abi.h"

// mount -- takes the SD card from the beginning, or says who has it.
//
//   mount        report which bus the card is on, and change nothing
//   mount sdio   take the card over four-bit SDIO, which reads about twice as fast
//   mount spi    take the card over SPI
//
// The bare form reports rather than mounts on purpose. It used to mean "mount
// over SPI", which made asking the question the same as answering it wrongly: a
// card that came up on four bits was pulled down to one by the act of looking,
// and the way back is a power cycle. A query has to be a query.
//
// Four bits cost a power cycle in general, and that is the card's rule and not
// ours: it latches into SPI the moment it is addressed that way and stays there
// until the power is cut. So SDIO has to be the first thing asked after
// power-up, or it cannot be had at all.
//
// The filesystem server mounts over SDIO at startup and falls back to SPI, so
// the explicit forms are mostly for a card put in afterwards, or to take the
// card again after it has been swapped.
//
// A swapped card needs the whole conversation repeated rather than the boot
// sector reread: a fresh card comes up idle and knows nothing of what was asked
// of the last one.
static bool same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: mount [spi|sdio]\n\n"
            "With no argument, reports which bus the card is on and changes nothing.\n"))
        return;

    uint32_t bus = UBIQOS_MOUNT_QUERY;

    if (argc > 1) {
        if (same(argv[1], "sdio")) {
            bus = UBIQOS_MOUNT_SDIO;
        } else if (same(argv[1], "spi")) {
            bus = UBIQOS_MOUNT_SPI;
        } else {
            ubiqos_write_str(UBIQOS_STDOUT, "usage: mount [spi|sdio]\n");
            return;
        }
    }

    int32_t rc = ubiqos_mount(bus);
    if (rc == -2) {
        ubiqos_write_str(UBIQOS_STDOUT,
            "mount: the host has the card. Eject it there, then 'usbdisk off'\n");
        return;
    }
    if (bus == UBIQOS_MOUNT_QUERY) {
        if (rc == (int32_t)UBIQOS_MOUNT_SDIO) {
            ubiqos_write_str(UBIQOS_STDOUT, "/sd: four-bit SDIO\n");
        } else if (rc == (int32_t)UBIQOS_MOUNT_SPI) {
            ubiqos_write_str(UBIQOS_STDOUT, "/sd: SPI\n");
        } else {
            ubiqos_write_str(UBIQOS_STDOUT,
                             "no card mounted. 'mount sdio' for four bits, 'mount spi' for one.\n");
        }
        return;
    }
    if (rc == 0) {
        ubiqos_write_str(UBIQOS_STDOUT, "card mounted\n");
    } else if (bus == UBIQOS_MOUNT_SDIO) {
        ubiqos_write_str(UBIQOS_STDOUT,
                         "mount: no SDIO. If the card was already mounted over SPI it\n"
                         "       cannot change bus until the power is cut.\n");
    } else {
        ubiqos_write_str(UBIQOS_STDOUT, "mount: no card, or not FAT32\n");
    }
}
