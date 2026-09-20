#include "../../common/ubiqos_abi.h"

// wifi -- the name people type, in front of ehrpc.
//
//   wifi connect <ssid>    join a network; the password is asked for
//   wifi auto              join whichever network the key store has a key for
//   wifi scan              which networks are in earshot
//   wifi rssi              how strong the access point's signal is
//   wifi mode | ps         what the radio is doing
//
// Until September 2026 this was the NINA client, and `wifi connect` is what
// the fingers remember. The ESP32-C6 runs ESP-Hosted now and ehrpc is what
// speaks to it; typing `wifi` got "no such module". So this is a front, not a
// second client: it starts ehrpc with the same words and waits for it, which
// hands it the console -- the password prompt is ehrpc's, typed there, never
// echoed and never an argument, and this program never sees it.
//
// /sd/config.txt is still the way to have the board join by itself at boot.

static bool is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return !*a && !*b;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: wifi connect <ssid> | auto | scan | rssi | mode | ps\n\n"
            "  connect <ssid>  join a network. A password kept in the key store\n"
            "                  under 'wifi.<ssid>' is used without asking; failing\n"
            "                  that the password is asked for, never echoed and\n"
            "                  never an argument\n"
            "  auto            of the networks in earshot, join the strongest\n"
            "                  one the unlocked store has a key for -- one board,\n"
            "                  several places\n"
            "  scan            which networks are in earshot, strongest first,\n"
            "                  and which of them there is a key for\n"
            "  rssi            the access point's signal strength\n"
            "  mode, ps        which mode and power saving the radio is in\n\n"
            "The same as ehrpc, which does the work. To join at every boot\n"
            "without typing, put ssid and password in /sd/config.txt.\n"))
        return;

    const bool connect = argc == 3 && is(argv[1], "connect");
    const bool simple  = argc == 2 && (is(argv[1], "rssi") || is(argv[1], "mode")
                                       || is(argv[1], "ps") || is(argv[1], "auto")
                                       || is(argv[1], "scan"));
    if (!connect && !simple) {
        ubiqos_write_str(UBIQOS_STDOUT,
                         "usage: wifi connect <ssid> | auto | scan | rssi | mode | ps\r\n");
        return;
    }

    // The words back into one line, as exec takes them. A network name holds
    // spaces only if the shell passed it as one word, and 32 bytes is the
    // longest there is.
    char args[48];
    uint32_t n = 0;
    for (int i = 1; i < argc; i++) {
        if (n && n < sizeof args - 1) args[n++] = ' ';
        for (const char *p = argv[i]; *p && n < sizeof args - 1; p++) args[n++] = *p;
    }
    args[n] = 0;

    const int32_t pid = ubiqos_exec("ehrpc", args);
    if (pid < 0) {
        ubiqos_write_str(UBIQOS_STDOUT, "wifi: no ehrpc on this machine\r\n");
        return;
    }
    ubiqos_wait(pid);
}
