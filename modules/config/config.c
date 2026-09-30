#include "../../common/ubiqos_stdio.h"

UBIQOS_LIBC_DEFINE

// config -- the settings a board keeps in flash, for a board with no card.
//
//     ubiqos:/> config set hostname bridge
//     kept: hostname = bridge -- it takes effect at the next start
//     ubiqos:/> config
//     hostname     challenger
//     usb address  192.168.8.1
//     kept in flash:
//       hostname = bridge
//
// What /sd/config.txt says on a board that has a card, a board without one
// keeps in a flash sector of its own -- the part that is nobody's secret:
// hostname, usb_address and timezone. The network's password is not among
// them; that is the key store's, sealed (`key set wifi.NAME`).
//
// A setting takes effect at the next start, not now: mDNS has announced the
// name and cannot unsay it, and the address is the one the computer at the
// other end of the cable was given. A card's config.txt, where there is one,
// is read after the flash and wins.

static void say_error(int32_t rc, const char *key) {
    if (rc == UBIQOS_CFG_ENOKEY)
        printf("config: %s is not a setting kept in flash on this machine -- hostname, usb_address\n"
               "        (where there is a network on the USB cable) or timezone\n", key);
    else if (rc == UBIQOS_CFG_EVALUE)
        printf("config: not a value %s can take\n", key);
    else if (rc == UBIQOS_CFG_EFULL)
        printf("config: the settings would not fit their sector\n");
    else if (rc == UBIQOS_CFG_EFLASH)
        printf("config: written, but the flash reads back different\n");
    else
        printf("config: this machine keeps no settings in flash\n");
}

static void show(void) {
    char v[64];
    if (ubiqos_config_get(UBIQOS_CFG_HOSTNAME, v, sizeof v) > 0)
        printf("hostname     %s\n", v);
    if (ubiqos_config_get(UBIQOS_CFG_USB_ADDRESS, v, sizeof v) > 0)
        printf("usb address  %s\n", v);

    // Allocated, not static: a writable static would make every process that
    // runs this carry its own copy of the module.
    char *kept = (char *)ubiqos_alloc(1025);
    if (!kept) { printf("config: no memory\n"); return; }
    const int32_t n = ubiqos_config_get(UBIQOS_CFG_STORED, kept, 1025);
    if (n <= 0) {
        printf("nothing kept in flash\n");
        ubiqos_free(kept);
        return;
    }
    printf("kept in flash:\n");
    for (char *l = kept; *l; ) {
        char *e = l;
        while (*e && *e != '\n') e++;
        const char c = *e;
        *e = 0;
        printf("  %s\n", l);
        if (!c) break;
        l = e + 1;
    }
    ubiqos_free(kept);
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: config [set KEY VALUE | unset KEY]\n\n"
            "Settings kept in flash, for a board with no card to hold\n"
            "/sd/config.txt: hostname, usb_address and timezone.\n\n"
            "  config                   what is in effect, and what is kept\n"
            "  config set KEY VALUE     keep a setting\n"
            "  config unset KEY         forget one\n\n"
            "A setting takes effect at the next start. A card's config.txt,\n"
            "where there is one, wins. The WiFi password is not kept here:\n"
            "'key set wifi.NAME' seals it in the key store.\n"))
        return;

    if (argc == 1) { show(); return; }

    const bool set   = argc == 4 && strcmp(argv[1], "set") == 0;
    const bool unset = argc == 3 && strcmp(argv[1], "unset") == 0;
    if (!set && !unset) {
        printf("usage: config [set KEY VALUE | unset KEY]\n");
        return;
    }

    const int32_t rc = ubiqos_config_set(argv[2], set ? argv[3] : "");
    if (rc != 0) { say_error(rc, argv[2]); return; }
    if (set) printf("kept: %s = %s -- it takes effect at the next start\n", argv[2], argv[3]);
    else     printf("forgotten: %s -- the default returns at the next start\n", argv[2]);
}
