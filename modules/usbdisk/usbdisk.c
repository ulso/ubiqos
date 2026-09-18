#include "../../common/ubiqos_abi.h"

// usbdisk -- hands the SD card to the host over USB, so a program built on the
// host can be copied straight onto the card.
//
//   usbdisk        give the card to the host
//   usbdisk off    take it back, if the host did not eject it
//
// Only one side may have the card. A host that mounts a FAT volume caches its
// directory and its free-cluster map, and so does this filesystem; if both
// write, the volume is ruined within seconds and neither notices until it reads
// something back. So `usbdisk` unmounts /sd before the host is allowed to see
// anything, and /sd stays gone until it is mounted again.
//
// The way back is `usbdisk off`, and that is all: the card was lent, not lost,
// so it is still on whatever bus it was and only the filesystem needs re-reading
// after the host's writes. Running `mount` instead would re-initialise the card
// over SPI and cost four-bit SDIO for the rest of the power cycle. Ejecting on the host first is
// good manners -- it makes sure the host has flushed what it was writing -- but
// it is not enough on its own: macOS unmounts the volume and stops asking
// without sending START_STOP_UNIT, so the board never hears about it. UbiqOS
// does listen for the eject when one comes, but it will not wait for one.

// If the host is a Mac, put these on the card once and mounting goes from
// fifteen seconds to none:
//
//     touch /Volumes/UBIQOS/.metadata_never_index
//     mkdir -p /Volumes/UBIQOS/.fseventsd && touch /Volumes/UBIQOS/.fseventsd/no_log
//
// Measured. The first mount of this card read 15920 sectors -- eight megabytes,
// off a volume holding a hundred kilobytes of files -- and wrote 2796, because
// Spotlight indexes anything it is shown. With indexing turned off the same
// mount reads 91 sectors and the volume appears at once. The slowness was never
// the card or the wire; it was the host being thorough about a disk it had
// never seen before.

static bool same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: usbdisk [off | force]\n\nHands the card to the host as a disk.\n  off     take it back\n  force   take it back even if the host has not ejected it\n")) return;

    uint32_t what = 1;                  // hand it over

    if (argc > 1) {
        if      (same(argv[1], "off"))   what = 0;
        else if (same(argv[1], "force")) what = 2;
        else {
            ubiqos_write_str(UBIQOS_STDOUT, "usage: usbdisk [off | force]\n");
            return;
        }
    }

    int32_t rc = ubiqos_usbdisk(what);

    if (rc == -2) {
        ubiqos_write_str(UBIQOS_STDOUT,
            "usbdisk: the host has not ejected the card. Eject it there first --\n"
            "         taking it back now would leave the host hung on a device\n"
            "         that has stopped answering. 'usbdisk force' if the host\n"
            "         has gone away and will never eject it.\n");
        return;
    }
    if (rc != 0) {
        ubiqos_write_str(UBIQOS_STDOUT,
            what == 1 ? "usbdisk: no card is mounted to hand over\n"
                      : "usbdisk: could not take the card back\n");
        return;
    }

    ubiqos_write_str(UBIQOS_STDOUT,
        what == 1 ? "the card is the host's. Eject it there, then 'usbdisk off'\n"
                  : "the card is back, on the bus it was already using\n");
}
